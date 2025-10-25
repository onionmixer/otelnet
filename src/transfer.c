/*
 * transfer.c - File transfer management implementation
 *
 * Implements file transfer protocols with 8-bit transparent transmission
 */

#include "transfer.h"
#include "telnet.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <syslog.h>
#include <ctype.h>

/* Logging macros */
#ifdef DEBUG
#define MB_LOG_DEBUG(fmt, ...) \
    syslog(LOG_DEBUG, "[DEBUG] %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define MB_LOG_DEBUG(fmt, ...) do {} while(0)
#endif

#define MB_LOG_INFO(fmt, ...) \
    syslog(LOG_INFO, "[INFO] " fmt, ##__VA_ARGS__)

#define MB_LOG_WARNING(fmt, ...) \
    syslog(LOG_WARNING, "[WARNING] " fmt, ##__VA_ARGS__)

#define MB_LOG_ERROR(fmt, ...) \
    syslog(LOG_ERR, "[ERROR] %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

/* Global cancellation flag (for signal handler) */
static volatile sig_atomic_t g_transfer_cancel_requested = 0;

/**
 * Request transfer cancellation (called from signal handler)
 */
void transfer_request_cancel(void)
{
    g_transfer_cancel_requested = 1;
}

/**
 * Check if transfer cancellation has been requested
 */
bool transfer_is_cancel_requested(void)
{
    return g_transfer_cancel_requested != 0;
}

/**
 * Clear transfer cancellation flag
 */
void transfer_clear_cancel(void)
{
    g_transfer_cancel_requested = 0;
}

/**
 * Initialize transfer state
 */
void transfer_init(transfer_state_t *state)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(transfer_state_t));
    state->active = false;
    state->protocol = TRANSFER_NONE;
    state->child_pid = 0;
    state->auto_triggered = false;
    state->bytes_transferred = 0;
    state->total_bytes = 0;
}

/**
 * Initialize transfer configuration with defaults
 */
void transfer_config_init(transfer_config_t *config)
{
    if (config == NULL) {
        return;
    }

    /* Set default paths */
    strncpy(config->kermit_path, "kermit", sizeof(config->kermit_path) - 1);
    strncpy(config->send_zmodem_path, "sz", sizeof(config->send_zmodem_path) - 1);
    strncpy(config->receive_zmodem_path, "rz", sizeof(config->receive_zmodem_path) - 1);

    /* Auto-detection defaults */
    config->auto_zmodem_enabled = true;
    config->auto_zmodem_prompt = true;
    strncpy(config->auto_zmodem_download_dir, ".", sizeof(config->auto_zmodem_download_dir) - 1);
    config->auto_xmodem_enabled = true;
    config->auto_xmodem_prompt = true;
    config->auto_ymodem_enabled = true;
    config->auto_ymodem_prompt = true;

    /* Timeout defaults */
    config->transfer_timeout_seconds = TRANSFER_DEFAULT_TIMEOUT;
    config->transfer_data_timeout_seconds = TRANSFER_DATA_TIMEOUT;

    /* Logging defaults */
    config->transfer_log_enabled = false;
    strncpy(config->transfer_log_file, "otelnet-transfers.log", sizeof(config->transfer_log_file) - 1);
    config->transfer_keep_partial = true;
}

/**
 * Enter transfer mode
 */
int transfer_enter_mode(transfer_state_t *state, transfer_protocol_t protocol)
{
    if (state == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (state->active) {
        MB_LOG_WARNING("Transfer already active");
        return ERROR_GENERAL;
    }

    state->active = true;
    state->protocol = protocol;
    state->start_time = time(NULL);
    state->last_data_time = time(NULL);
    state->bytes_transferred = 0;
    state->total_bytes = 0;
    state->child_pid = 0;

    MB_LOG_INFO("Entering transfer mode: %s", transfer_protocol_name(protocol));
    return SUCCESS;
}

/**
 * Exit transfer mode
 */
void transfer_exit_mode(transfer_state_t *state)
{
    if (state == NULL) {
        return;
    }

    if (!state->active) {
        return;
    }

    MB_LOG_INFO("Exiting transfer mode: %s", transfer_protocol_name(state->protocol));

    state->active = false;
    state->protocol = TRANSFER_NONE;
    state->child_pid = 0;
    state->auto_triggered = false;
    state->filename[0] = '\0';
}

/**
 * Check if transfer is active
 */
bool transfer_is_active(const transfer_state_t *state)
{
    return state != NULL && state->active;
}

/**
 * Check if transfer has timed out
 */
bool transfer_check_timeout(const transfer_state_t *state, const transfer_config_t *config)
{
    if (state == NULL || config == NULL || !state->active) {
        return false;
    }

    /* Check if timeout is enabled (0 = no timeout) */
    if (config->transfer_timeout_seconds == 0) {
        return false;
    }

    time_t now = time(NULL);
    time_t elapsed = now - state->start_time;

    return elapsed > config->transfer_timeout_seconds;
}

/**
 * Check if data timeout has occurred
 */
bool transfer_check_data_timeout(const transfer_state_t *state, const transfer_config_t *config)
{
    if (state == NULL || config == NULL || !state->active) {
        return false;
    }

    /* Check if timeout is enabled */
    if (config->transfer_data_timeout_seconds == 0) {
        return false;
    }

    time_t now = time(NULL);
    time_t elapsed = now - state->last_data_time;

    return elapsed > config->transfer_data_timeout_seconds;
}

/**
 * Update last data received timestamp
 */
void transfer_update_data_timestamp(transfer_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->last_data_time = time(NULL);
}

/**
 * Setup terminal for 8-bit transparent file transfer
 */
int transfer_setup_terminal(struct termios *saved_termios)
{
    struct termios tty;

    if (saved_termios == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Get current terminal settings and save them */
    if (tcgetattr(STDIN_FILENO, saved_termios) < 0) {
        MB_LOG_ERROR("Failed to get terminal attributes: %s", strerror(errno));
        return ERROR_IO;
    }

    /* Copy current settings for modification */
    memcpy(&tty, saved_termios, sizeof(struct termios));

    /* 8-bit clean input - disable all transformations */
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                     INLCR | IGNCR | ICRNL | IXON | IXOFF);
    tty.c_iflag |= IGNPAR;  /* Ignore parity errors */

    /* 8-bit clean output - no processing */
    tty.c_oflag &= ~OPOST;  /* No output processing */

    /* 8-bit clean line discipline */
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    /* 8-bit clean character size */
    tty.c_cflag &= ~(CSIZE | PARENB);
    tty.c_cflag |= CS8;  /* 8 bits per byte */

    /* Non-blocking, immediate read */
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    /* Apply new settings */
    if (tcsetattr(STDIN_FILENO, TCSANOW, &tty) < 0) {
        MB_LOG_ERROR("Failed to set terminal attributes: %s", strerror(errno));
        return ERROR_IO;
    }

    MB_LOG_DEBUG("Terminal configured for 8-bit transparent transfer");
    return SUCCESS;
}

/**
 * Restore terminal from transfer mode
 */
int transfer_restore_terminal(const struct termios *saved_termios)
{
    if (saved_termios == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (tcsetattr(STDIN_FILENO, TCSANOW, saved_termios) < 0) {
        MB_LOG_ERROR("Failed to restore terminal attributes: %s", strerror(errno));
        return ERROR_IO;
    }

    MB_LOG_DEBUG("Terminal restored from transfer mode");
    return SUCCESS;
}

/**
 * Handle transfer error and cleanup
 */
void transfer_handle_error(transfer_state_t *state, transfer_error_t error_type)
{
    if (state == NULL) {
        return;
    }

    const char *error_msg = transfer_get_error_message(error_type);
    MB_LOG_ERROR("Transfer error: %s", error_msg);

    /* Terminate child process if running */
    if (state->child_pid > 0) {
        MB_LOG_INFO("Terminating child process (PID: %d)", state->child_pid);

        /* Send SIGTERM first */
        kill(state->child_pid, SIGTERM);

        /* Wait up to 2 seconds */
        int status;
        for (int i = 0; i < 20; i++) {
            if (waitpid(state->child_pid, &status, WNOHANG) > 0) {
                break;
            }
            usleep(100000);  /* 100ms */
        }

        /* Force kill if still running */
        if (waitpid(state->child_pid, &status, WNOHANG) == 0) {
            MB_LOG_WARNING("Child process did not terminate, sending SIGKILL");
            kill(state->child_pid, SIGKILL);
            waitpid(state->child_pid, &status, 0);
        }

        state->child_pid = 0;
    }

    /* Exit transfer mode */
    transfer_exit_mode(state);
}

/**
 * Get error message for error type
 */
const char *transfer_get_error_message(transfer_error_t error_type)
{
    switch (error_type) {
        case TRANSFER_ERROR_NONE:
            return "No error";
        case TRANSFER_ERROR_TIMEOUT:
            return "Transfer timeout - no progress for too long";
        case TRANSFER_ERROR_NETWORK:
            return "Network connection lost during transfer";
        case TRANSFER_ERROR_PROTOCOL:
            return "Protocol error - invalid data received";
        case TRANSFER_ERROR_CHILD_CRASH:
            return "Transfer program crashed unexpectedly";
        case TRANSFER_ERROR_USER_CANCEL:
            return "Transfer cancelled by user";
        case TRANSFER_ERROR_DISK_FULL:
            return "Disk full - cannot write file";
        case TRANSFER_ERROR_PERMISSION:
            return "Permission denied - cannot access file";
        case TRANSFER_ERROR_UNKNOWN:
        default:
            return "Unknown transfer error";
    }
}

/**
 * Initialize ZMODEM detector
 */
void zmodem_detector_init(zmodem_detector_t *detector)
{
    if (detector == NULL) {
        return;
    }

    memset(detector, 0, sizeof(zmodem_detector_t));
    detector->len = 0;
    detector->enabled = true;
}

/**
 * Enable/disable ZMODEM detection
 */
void zmodem_detector_set_enabled(zmodem_detector_t *detector, bool enabled)
{
    if (detector == NULL) {
        return;
    }

    detector->enabled = enabled;
    if (!enabled) {
        detector->len = 0;  /* Clear buffer when disabled */
    }
}

/**
 * Detect ZMODEM trigger sequences in incoming data
 */
bool zmodem_detect_trigger(zmodem_detector_t *detector, const unsigned char *data, size_t len,
                           bool *is_receive_init, bool *is_send_init)
{
    if (detector == NULL || data == NULL || !detector->enabled) {
        return false;
    }

    if (is_receive_init) *is_receive_init = false;
    if (is_send_init) *is_send_init = false;

    /* Add new data to detection buffer (sliding window) */
    for (size_t i = 0; i < len; i++) {
        /* Shift buffer if full */
        if (detector->len >= ZMODEM_DETECT_BUFFER_SIZE) {
            memmove(detector->buffer, detector->buffer + 1, ZMODEM_DETECT_BUFFER_SIZE - 1);
            detector->len = ZMODEM_DETECT_BUFFER_SIZE - 1;
        }

        /* Add new byte */
        detector->buffer[detector->len++] = data[i];

        /* Check for ZMODEM patterns (need at least 6 bytes: **^XB0x) */
        if (detector->len >= 6) {
            /* Pattern: ** ZDLE B 0 0 - ZRQINIT (remote wants to send, we receive) */
            /* Hex: 2A 2A 18 42 30 30 */
            if (detector->buffer[detector->len - 6] == 0x2A &&  /* * */
                detector->buffer[detector->len - 5] == 0x2A &&  /* * */
                detector->buffer[detector->len - 4] == 0x18 &&  /* ZDLE (Ctrl-X) */
                detector->buffer[detector->len - 3] == 0x42 &&  /* B */
                detector->buffer[detector->len - 2] == 0x30 &&  /* 0 */
                detector->buffer[detector->len - 1] == 0x30) {  /* 0 */

                MB_LOG_INFO("ZMODEM ZRQINIT detected - remote wants to send");
                if (is_receive_init) *is_receive_init = true;
                detector->len = 0;  /* Clear buffer */
                return true;
            }

            /* Pattern: ** ZDLE B 0 1 - ZRINIT (remote starting to send, we receive) */
            /* Hex: 2A 2A 18 42 30 31 */
            if (detector->buffer[detector->len - 6] == 0x2A &&  /* * */
                detector->buffer[detector->len - 5] == 0x2A &&  /* * */
                detector->buffer[detector->len - 4] == 0x18 &&  /* ZDLE */
                detector->buffer[detector->len - 3] == 0x42 &&  /* B */
                detector->buffer[detector->len - 2] == 0x30 &&  /* 0 */
                detector->buffer[detector->len - 1] == 0x31) {  /* 1 */

                MB_LOG_INFO("ZMODEM ZRINIT detected - remote starting to send");
                if (is_receive_init) *is_receive_init = true;
                detector->len = 0;
                return true;
            }

            /* Pattern: ** ZDLE B 0 8 - ZFILE (remote sending file header, we receive) */
            /* Hex: 2A 2A 18 42 30 38 */
            if (detector->buffer[detector->len - 6] == 0x2A &&  /* * */
                detector->buffer[detector->len - 5] == 0x2A &&  /* * */
                detector->buffer[detector->len - 4] == 0x18 &&  /* ZDLE */
                detector->buffer[detector->len - 3] == 0x42 &&  /* B */
                detector->buffer[detector->len - 2] == 0x30 &&  /* 0 */
                detector->buffer[detector->len - 1] == 0x38) {  /* 8 */

                MB_LOG_INFO("ZMODEM ZFILE detected - remote sending file");
                if (is_receive_init) *is_receive_init = true;
                detector->len = 0;
                return true;
            }
        }

        /* Also check for text "rz" at start of sz output (remote sending, we receive) */
        /* sz (send) starts with "rz\r" - this signals that remote is SENDING */
        /* Look for "rz" followed by newline or carriage return */
        if (detector->len >= 3) {
            if ((detector->buffer[detector->len - 3] == 'r' &&
                 detector->buffer[detector->len - 2] == 'z' &&
                 (detector->buffer[detector->len - 1] == '\r' ||
                  detector->buffer[detector->len - 1] == '\n')) ||
                (detector->len >= 4 &&
                 detector->buffer[detector->len - 4] == 'r' &&
                 detector->buffer[detector->len - 3] == 'z' &&
                 detector->buffer[detector->len - 2] == '\r' &&
                 detector->buffer[detector->len - 1] == '\n')) {

                MB_LOG_INFO("ZMODEM 'rz' prefix detected - remote is sending (sz output)");
                if (is_receive_init) *is_receive_init = true;
                detector->len = 0;
                return true;
            }
        }
    }

    return false;
}

/**
 * Get protocol name as string
 */
const char *transfer_protocol_name(transfer_protocol_t protocol)
{
    switch (protocol) {
        case TRANSFER_NONE:
            return "None";
        case TRANSFER_ZMODEM_SEND:
            return "ZMODEM Send";
        case TRANSFER_ZMODEM_RECV:
            return "ZMODEM Receive";
        case TRANSFER_XMODEM_SEND:
            return "XMODEM Send";
        case TRANSFER_XMODEM_RECV:
            return "XMODEM Receive";
        case TRANSFER_YMODEM_SEND:
            return "YMODEM Send";
        case TRANSFER_YMODEM_RECV:
            return "YMODEM Receive";
        case TRANSFER_KERMIT_SEND:
            return "Kermit Send";
        case TRANSFER_KERMIT_RECV:
            return "Kermit Receive";
        default:
            return "Unknown";
    }
}

/**
 * Log transfer start
 */
void transfer_log_start(const transfer_config_t *config, const transfer_state_t *state)
{
    if (config == NULL || state == NULL || !config->transfer_log_enabled) {
        return;
    }

    FILE *fp = fopen(config->transfer_log_file, "a");
    if (fp == NULL) {
        MB_LOG_WARNING("Failed to open transfer log file: %s", config->transfer_log_file);
        return;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(fp, "%s | START  | %s | %s\n",
            timestamp,
            transfer_protocol_name(state->protocol),
            state->filename[0] ? state->filename : "");

    fclose(fp);
}

/**
 * Log transfer end
 */
void transfer_log_end(const transfer_config_t *config, const transfer_state_t *state,
                      transfer_error_t error_type)
{
    if (config == NULL || state == NULL || !config->transfer_log_enabled) {
        return;
    }

    FILE *fp = fopen(config->transfer_log_file, "a");
    if (fp == NULL) {
        MB_LOG_WARNING("Failed to open transfer log file: %s", config->transfer_log_file);
        return;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    time_t duration = now - state->start_time;

    if (error_type == TRANSFER_ERROR_NONE) {
        fprintf(fp, "%s | END    | %s | %s | %lu bytes | %lds | SUCCESS\n",
                timestamp,
                transfer_protocol_name(state->protocol),
                state->filename[0] ? state->filename : "",
                (unsigned long)state->bytes_transferred,
                (long)duration);
    } else {
        fprintf(fp, "%s | ERROR  | %s | %s | %s\n",
                timestamp,
                transfer_protocol_name(state->protocol),
                state->filename[0] ? state->filename : "",
                transfer_get_error_message(error_type));
    }

    fclose(fp);
}

/* ============================================================================
 * Telnet IAC Escaping Implementation
 * ============================================================================
 */

/**
 * Escape IAC bytes (0xFF → 0xFF 0xFF) for telnet transmission
 */
ssize_t telnet_escape_iac(const unsigned char *input, size_t input_len,
                          unsigned char *output, size_t output_max)
{
    if (input == NULL || output == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t out_idx = 0;

    for (size_t i = 0; i < input_len; i++) {
        unsigned char byte = input[i];

        if (byte == TELNET_IAC) {
            /* Need 2 bytes for escaped IAC */
            if (out_idx + 2 > output_max) {
                errno = ENOBUFS;
                MB_LOG_ERROR("IAC escape buffer overflow: need %zu bytes, have %zu",
                            out_idx + 2, output_max);
                return -1;
            }
            output[out_idx++] = TELNET_IAC;
            output[out_idx++] = TELNET_IAC;  /* Escape: 0xFF → 0xFF 0xFF */
        } else {
            /* Regular byte */
            if (out_idx + 1 > output_max) {
                errno = ENOBUFS;
                MB_LOG_ERROR("IAC escape buffer overflow: need %zu bytes, have %zu",
                            out_idx + 1, output_max);
                return -1;
            }
            output[out_idx++] = byte;
        }
    }

    return (ssize_t)out_idx;
}

/**
 * Unescape IAC bytes (0xFF 0xFF → 0xFF) from telnet stream
 */
ssize_t telnet_unescape_iac(const unsigned char *input, size_t input_len,
                            unsigned char *output, size_t output_max,
                            int *iac_state)
{
    if (input == NULL || output == NULL || iac_state == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t out_idx = 0;

    for (size_t i = 0; i < input_len; i++) {
        unsigned char byte = input[i];

        if (*iac_state == 1) {
            /* Previous byte was 0xFF */
            if (byte == TELNET_IAC) {
                /* 0xFF 0xFF → single 0xFF (escaped data byte) */
                if (out_idx + 1 > output_max) {
                    errno = ENOBUFS;
                    MB_LOG_ERROR("IAC unescape buffer overflow");
                    return -1;
                }
                output[out_idx++] = TELNET_IAC;
                *iac_state = 0;
            } else {
                /* 0xFF <other> → telnet command, skip both bytes */
                /* In BINARY mode during file transfer, this should not happen */
                MB_LOG_WARNING("Unexpected telnet command during binary transfer: "
                              "IAC 0x%02X (discarded)", byte);
                *iac_state = 0;
                /* Do not output anything - command is discarded */
            }
        } else {
            /* Normal state */
            if (byte == TELNET_IAC) {
                /* Start of potential escape sequence or command */
                *iac_state = 1;  /* Wait for next byte */
            } else {
                /* Regular data byte */
                if (out_idx + 1 > output_max) {
                    errno = ENOBUFS;
                    MB_LOG_ERROR("IAC unescape buffer overflow");
                    return -1;
                }
                output[out_idx++] = byte;
            }
        }
    }

    /* Note: iac_state may be 1 if buffer ended with 0xFF */
    /* This is normal - next call will process the following byte */

    return (ssize_t)out_idx;
}

/**
 * Relay data bidirectionally between socket and pipes (stdin/stdout)
 * Handles telnet protocol (IAC escape/unescape)
 * Returns 0 on success, -1 on error, 1 on child exit
 */
static int relay_data_pipes(int socket_fd, int stdin_pipe_fd, int stdout_pipe_fd,
                            pid_t child_pid, const transfer_config_t *config,
                            time_t start_time, telnet_t *telnet_ctx)
{
    fd_set readfds;
    struct timeval tv;
    unsigned char raw_buffer[BUFFER_SIZE];
    unsigned char proc_buffer[BUFFER_SIZE * 2];  /* Larger for IAC escaping */
    unsigned char pipe_buffer[BUFFER_SIZE];
    int status;
    bool binary_mode_ended = false;
    time_t drain_start_time = 0;
    const int DRAIN_TIMEOUT_SECONDS = 3;  /* Max time to drain buffers after BINARY mode exit */

    while (1) {
        /* Check if child has exited */
        int result = waitpid(child_pid, &status, WNOHANG);
        if (result > 0) {
            /* Child exited */
            return 1;
        } else if (result < 0 && errno != EINTR) {
            MB_LOG_ERROR("waitpid failed: %s", strerror(errno));
            return -1;
        }

        /* Check for timeout */
        if (config->transfer_timeout_seconds > 0) {
            time_t elapsed = time(NULL) - start_time;
            if (elapsed > config->transfer_timeout_seconds) {
                MB_LOG_WARNING("Transfer timeout after %ld seconds", (long)elapsed);
                return -1;
            }
        }

        /* Check for user cancellation */
        if (transfer_is_cancel_requested()) {
            MB_LOG_INFO("Transfer cancellation requested");
            return -1;
        }

        /* Check socket connection status */
        if (socket_fd >= 0) {
            char buf[1];
            ssize_t peek_result = recv(socket_fd, buf, 1, MSG_PEEK | MSG_DONTWAIT);
            if (peek_result == 0) {
                MB_LOG_WARNING("Socket connection closed during transfer");
                return -1;
            } else if (peek_result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                MB_LOG_WARNING("Socket error during transfer: %s", strerror(errno));
                return -1;
            }
        }

        /* Check drain timeout when in drain mode */
        if (binary_mode_ended && drain_start_time > 0) {
            time_t drain_elapsed = time(NULL) - drain_start_time;
            if (drain_elapsed > DRAIN_TIMEOUT_SECONDS) {
                MB_LOG_INFO("Buffer drain timeout after %ld seconds, completing transfer",
                           (long)drain_elapsed);
                return 1;  /* Signal completion */
            }
        }

        /* Setup select() */
        FD_ZERO(&readfds);
        /* Only monitor socket if BINARY mode is still active
         * After BINARY mode ends, we only drain stdout_pipe */
        if (!binary_mode_ended) {
            FD_SET(socket_fd, &readfds);
        }
        FD_SET(stdout_pipe_fd, &readfds);
        int maxfd = (socket_fd > stdout_pipe_fd) ? socket_fd : stdout_pipe_fd;

        tv.tv_sec = 0;
        tv.tv_usec = 100000;  /* 100ms timeout */

        result = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (result < 0) {
            if (errno == EINTR) continue;
            MB_LOG_ERROR("select failed: %s", strerror(errno));
            return -1;
        }

        if (result == 0) {
            /* Timeout, continue */
            continue;
        }

        /* Data from socket -> stdin pipe (server sending to sz/rz) */
        if (!binary_mode_ended && FD_ISSET(socket_fd, &readfds)) {
            ssize_t n = read(socket_fd, raw_buffer, sizeof(raw_buffer));
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    MB_LOG_ERROR("read from socket failed: %s", strerror(errno));
                    return -1;
                }
            } else if (n == 0) {
                MB_LOG_INFO("Socket closed by peer");
                return -1;
            } else {
                MB_LOG_INFO("Relay: Read %zd bytes from socket", n);

                /* Save BINARY mode state BEFORE processing
                 * This detects mode changes that happen during IAC processing */
                bool was_binary_mode = telnet_ctx->binary_remote && telnet_ctx->binary_local;

                /* Process telnet protocol (remove IAC sequences) */
                size_t processed_len;
                telnet_process_input(telnet_ctx, raw_buffer, n, proc_buffer, sizeof(proc_buffer), &processed_len);

                MB_LOG_INFO("Relay: After IAC processing: %zu bytes clean data (was_binary=%d)",
                           processed_len, was_binary_mode);

                /* Check if BINARY mode changed during this packet processing
                 * CRITICAL: If mode changed, processed_len may contain mixed data:
                 * - Data before IAC (safe BINARY mode data)
                 * - Data after IAC (unsafe TEXT mode data, e.g., timestamps)
                 * To be safe, we discard all data when mode changes in same packet. */
                bool is_binary_mode = telnet_ctx->binary_remote && telnet_ctx->binary_local;
                bool mode_changed_to_text = was_binary_mode && !is_binary_mode;

                MB_LOG_INFO("Relay: Mode check: was_binary=%d, is_binary=%d, mode_changed=%d",
                           was_binary_mode, is_binary_mode, mode_changed_to_text);

                if (mode_changed_to_text) {
                    MB_LOG_INFO("BINARY mode ended by remote, entering buffer drain mode");

                    /* CRITICAL FIX: Write data to stdin pipe BEFORE entering drain mode
                     * The processed_len may contain the final ACK from rz/sz that client needs.
                     * In YMODEM, the final ACK is sent by rz just before it exits, and the
                     * server sends BINARY mode exit immediately after. These can arrive in the
                     * same packet or consecutive reads. We must deliver this ACK to sz/rz
                     * before stopping socket reads, otherwise sz will hang waiting for ACK. */
                    if (processed_len > 0) {
                        MB_LOG_INFO("Relay: Writing %zu bytes to stdin pipe before drain mode (may contain final ACK)", processed_len);

                        /* Log data for debugging */
                        char hex_buf[96];
                        size_t log_len = processed_len < 32 ? processed_len : 32;
                        for (size_t i = 0; i < log_len; i++) {
                            snprintf(hex_buf + i*3, 4, "%02X ", proc_buffer[i]);
                        }
                        MB_LOG_INFO("Data to write (first %zu bytes): %s", log_len, hex_buf);

                        ssize_t written = write(stdin_pipe_fd, proc_buffer, processed_len);
                        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                            MB_LOG_ERROR("write to stdin pipe failed: %s", strerror(errno));
                            return -1;
                        }
                        MB_LOG_INFO("Relay: Successfully wrote %zd bytes to stdin pipe before drain", written);
                    } else {
                        MB_LOG_INFO("BINARY mode exit with no data in packet (IAC only)");
                    }

                    /* Now enter drain mode - stop reading from socket, only drain stdout pipe */
                    binary_mode_ended = true;
                    drain_start_time = time(NULL);
                } else if (processed_len > 0) {
                    /* Normal case: still in BINARY mode, write data to stdin pipe */
                    MB_LOG_INFO("Relay: Writing %zu bytes to stdin pipe", processed_len);
                    ssize_t written = write(stdin_pipe_fd, proc_buffer, processed_len);
                    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        MB_LOG_ERROR("write to stdin pipe failed: %s", strerror(errno));
                        return -1;
                    }
                    MB_LOG_INFO("Relay: Successfully wrote %zd bytes to stdin pipe", written);
                }
            }
        }

        /* Data from stdout pipe -> socket (sz/rz sending to server) */
        if (FD_ISSET(stdout_pipe_fd, &readfds)) {
            ssize_t n = read(stdout_pipe_fd, pipe_buffer, sizeof(pipe_buffer));
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    MB_LOG_ERROR("read from stdout pipe failed: %s", strerror(errno));
                    return -1;
                }
            } else if (n == 0) {
                /* Pipe closed, child probably exited or finished sending */
                if (binary_mode_ended) {
                    MB_LOG_INFO("Stdout pipe closed during drain mode, transfer complete");
                } else {
                    MB_LOG_INFO("Stdout pipe closed");
                }
                return 1;
            } else if (n > 0) {
                /* Continue sending data from sz/rz even in drain mode
                 * to ensure final ACK packets are transmitted */
                if (binary_mode_ended) {
                    MB_LOG_INFO("Draining %zd bytes from stdout pipe", n);
                } else {
                    MB_LOG_INFO("Relay: Read %zd bytes from stdout pipe", n);
                }

                /* Escape IAC bytes for telnet protocol */
                size_t escaped_len;
                telnet_prepare_output(telnet_ctx, pipe_buffer, n, proc_buffer, sizeof(proc_buffer), &escaped_len);

                if (escaped_len > 0) {
                    MB_LOG_INFO("Relay: Writing %zu bytes to socket", escaped_len);
                    /* Write escaped data to socket */
                    ssize_t written = write(socket_fd, proc_buffer, escaped_len);
                    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        MB_LOG_ERROR("write to socket failed: %s", strerror(errno));
                        return -1;
                    }
                    MB_LOG_INFO("Relay: Successfully wrote %zd bytes to socket", written);
                }
            }
        }
    }

    return 0;
}

/**
 * Execute external program using 'script' command for PTY emulation
 * Common helper for rz/sz/kermit execution
 */
static int execute_external_program(const char *program_path, char *const argv[],
                                    int socket_fd, transfer_state_t *state,
                                    const transfer_config_t *config,
                                    telnet_t *telnet_ctx)
{
    pid_t pid;
    int status;
    int stdin_pipe[2], stdout_pipe[2];
    time_t start_time;
    char command_line[BUFFER_SIZE];

    if (config == NULL) {
        MB_LOG_ERROR("Config is NULL");
        return ERROR_INVALID_ARG;
    }

    /* Check if program exists and is executable */
    if (access(program_path, X_OK) != 0) {
        MB_LOG_ERROR("Program not found or not executable: %s", program_path);
        fprintf(stderr, "\r\nError: Program '%s' not found or not executable\r\n", program_path);
        return ERROR_GENERAL;
    }

    /* lrzsz-lite doesn't need PTY, so no script command required */
    MB_LOG_INFO("Using direct execution mode (PTY-free)");

    /* Build command line for logging/display */
    size_t offset = 0;
    offset += snprintf(command_line + offset, sizeof(command_line) - offset, "%s", program_path);
    if (argv != NULL) {
        for (int i = 1; argv[i] != NULL && offset < sizeof(command_line) - 1; i++) {
            offset += snprintf(command_line + offset, sizeof(command_line) - offset, " %s", argv[i]);
        }
    }

    fprintf(stderr, "\r\n[Starting transfer: %s]\r\n", command_line);
    fflush(stderr);

    MB_LOG_INFO("Direct execution (no PTY): %s", command_line);

    /* Create pipes for stdin and stdout */
    if (pipe(stdin_pipe) < 0) {
        MB_LOG_ERROR("Failed to create stdin pipe: %s", strerror(errno));
        fprintf(stderr, "\r\nError: Failed to create stdin pipe\r\n");
        return ERROR_GENERAL;
    }

    if (pipe(stdout_pipe) < 0) {
        MB_LOG_ERROR("Failed to create stdout pipe: %s", strerror(errno));
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        fprintf(stderr, "\r\nError: Failed to create stdout pipe\r\n");
        return ERROR_GENERAL;
    }

    /* Set pipes to non-blocking mode */
    fcntl(stdin_pipe[1], F_SETFL, fcntl(stdin_pipe[1], F_GETFL, 0) | O_NONBLOCK);
    fcntl(stdout_pipe[0], F_SETFL, fcntl(stdout_pipe[0], F_GETFL, 0) | O_NONBLOCK);

    pid = fork();
    if (pid < 0) {
        MB_LOG_ERROR("Failed to fork: %s", strerror(errno));
        fprintf(stderr, "\r\nError: Failed to fork process: %s\r\n", strerror(errno));
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return ERROR_GENERAL;
    }

    if (pid == 0) {
        /* Child process */

        /* Close unused pipe ends */
        close(stdin_pipe[1]);   /* Close write end of stdin pipe */
        close(stdout_pipe[0]);  /* Close read end of stdout pipe */

        /* Redirect stdin from pipe */
        if (dup2(stdin_pipe[0], STDIN_FILENO) < 0) {
            perror("dup2 stdin");
            _exit(1);
        }

        /* Redirect stdout to pipe */
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0) {
            perror("dup2 stdout");
            _exit(1);
        }

        /* Redirect stderr to stdout (same pipe) */
        if (dup2(stdout_pipe[1], STDERR_FILENO) < 0) {
            perror("dup2 stderr");
            _exit(1);
        }

        /* Close original pipe fds */
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        /* Execute program directly (lrzsz-lite doesn't need PTY) */
        if (argv != NULL) {
            execv(program_path, argv);
        } else {
            execl(program_path, program_path, (char *)NULL);
        }

        /* If we get here, exec failed */
        perror("exec");
        _exit(EXIT_FAILURE);
    }

    /* Parent process */

    /* Close unused pipe ends */
    close(stdin_pipe[0]);   /* Close read end of stdin pipe */
    close(stdout_pipe[1]);  /* Close write end of stdout pipe */

    state->child_pid = pid;
    start_time = time(NULL);

    MB_LOG_INFO("Starting pipe relay for child process %d (timeout: %ds)",
                pid, config->transfer_timeout_seconds);

    /* Start bidirectional relay between socket and pipes */
    int relay_result = relay_data_pipes(socket_fd, stdin_pipe[1], stdout_pipe[0],
                                        pid, config, start_time, telnet_ctx);

    /* Close pipes */
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);

    /* Handle relay result */
    if (relay_result < 0) {
        /* Relay error or timeout/cancel - kill child */
        MB_LOG_WARNING("Relay failed, terminating child process %d", pid);

        /* First try SIGTERM */
        if (kill(pid, SIGTERM) == 0) {
            /* Wait up to 2 seconds for graceful termination */
            for (int i = 0; i < 20; i++) {
                int result = waitpid(pid, &status, WNOHANG);
                if (result > 0) {
                    MB_LOG_INFO("Child terminated gracefully after SIGTERM");
                    goto check_status;
                }
                usleep(100000);
            }

            /* Still running, force kill */
            MB_LOG_WARNING("Child did not respond to SIGTERM, sending SIGKILL");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }

        state->child_pid = 0;

        /* Determine error type */
        if (transfer_is_cancel_requested()) {
            transfer_handle_error(state, TRANSFER_ERROR_USER_CANCEL);
            return ERROR_GENERAL;
        } else {
            fprintf(stderr, "\r\n[Transfer aborted]\r\n");
            transfer_handle_error(state, TRANSFER_ERROR_NETWORK);
            return ERROR_CONNECTION;
        }
    }

    /* relay_result == 1: child exited, collect status */
    waitpid(pid, &status, 0);

check_status:
    state->child_pid = 0;

    /* Direct execution - exit codes are reliable and meaningful */
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);

        /* For lrzsz-lite:
         * - 0: Success
         * - 1: General error (file not found, network error, etc.)
         * - 2: Protocol error
         * - Other: Unexpected error
         */
        if (exit_code == 0) {
            MB_LOG_INFO("Transfer completed successfully (exit code 0)");
            fprintf(stderr, "\r\n[Transfer completed successfully]\r\n");
            return SUCCESS;
        } else if (relay_result == 1) {
            /* Relay completed normally but program returned error
             * This could be normal for some cases (e.g., user cancellation on remote) */
            MB_LOG_WARNING("Transfer completed with exit code %d", exit_code);
            fprintf(stderr, "\r\n[Transfer completed with warnings (exit code %d)]\r\n", exit_code);
            return SUCCESS;  /* Consider as success if relay completed */
        } else {
            /* Forced termination */
            MB_LOG_ERROR("Transfer program exited with code %d after forced termination", exit_code);
            fprintf(stderr, "\r\n[Transfer failed: program exited with code %d]\r\n", exit_code);
            return ERROR_GENERAL;
        }

    } else if (WIFSIGNALED(status)) {
        int signal = WTERMSIG(status);
        MB_LOG_ERROR("Transfer program terminated by signal %d", signal);
        fprintf(stderr, "\r\n[Transfer terminated by signal %d]\r\n", signal);
        return ERROR_GENERAL;
    }

    fprintf(stderr, "\r\n[Transfer failed: unknown error]\r\n");
    return ERROR_GENERAL;
}

/**
 * Execute Kermit send
 */
int transfer_execute_kermit_send(const transfer_config_t *config, transfer_state_t *state,
                                 int socket_fd, const char *filename,
                                 telnet_t *telnet_ctx)
{
    if (config == NULL || state == NULL || filename == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Save filename in state */
    strncpy(state->filename, filename, sizeof(state->filename) - 1);
    state->filename[sizeof(state->filename) - 1] = '\0';

    /* Build argument array */
    char *argv[] = {
        (char *)config->kermit_path,
        "-s",      /* Send */
        "-i",      /* Binary mode */
        (char *)filename,
        NULL
    };

    MB_LOG_INFO("Executing Kermit send: %s", filename);

    return execute_external_program(config->kermit_path, argv, socket_fd, state, config, telnet_ctx);
}

/**
 * Execute Kermit receive
 */
int transfer_execute_kermit_receive(const transfer_config_t *config, transfer_state_t *state,
                                    int socket_fd,
                                    telnet_t *telnet_ctx)
{
    if (config == NULL || state == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Build argument array */
    char *argv[] = {
        (char *)config->kermit_path,
        "-r",      /* Receive */
        "-i",      /* Binary mode */
        NULL
    };

    MB_LOG_INFO("Executing Kermit receive");

    return execute_external_program(config->kermit_path, argv, socket_fd, state, config, telnet_ctx);
}

/**
 * Execute ZMODEM/XMODEM/YMODEM transfer
 */
int transfer_execute_modem(const transfer_config_t *config, transfer_state_t *state,
                           int socket_fd, transfer_protocol_t protocol, const char *filename,
                           telnet_t *telnet_ctx)
{
    if (config == NULL || state == NULL) {
        return ERROR_INVALID_ARG;
    }

    const char *program_path;
    char *argv[16];
    int argc = 0;

    /* Determine program and build arguments */
    switch (protocol) {
        case TRANSFER_ZMODEM_SEND:
        case TRANSFER_XMODEM_SEND:
        case TRANSFER_YMODEM_SEND:
            if (filename == NULL) {
                MB_LOG_ERROR("Filename required for send operation");
                return ERROR_INVALID_ARG;
            }
            program_path = config->send_zmodem_path;
            argv[argc++] = (char *)program_path;

            /* Add protocol option */
            if (protocol == TRANSFER_XMODEM_SEND) {
                argv[argc++] = "--xmodem";
            } else if (protocol == TRANSFER_YMODEM_SEND) {
                argv[argc++] = "--ymodem";
            }
            /* ZMODEM is default, no flag needed */

            argv[argc++] = (char *)filename;
            argv[argc] = NULL;

            /* Save filename in state */
            strncpy(state->filename, filename, sizeof(state->filename) - 1);
            break;

        case TRANSFER_ZMODEM_RECV:
        case TRANSFER_XMODEM_RECV:
        case TRANSFER_YMODEM_RECV:
            program_path = config->receive_zmodem_path;
            argv[argc++] = (char *)program_path;

            /* Add protocol option and flags */
            if (protocol == TRANSFER_XMODEM_RECV) {
                argv[argc++] = "--xmodem";
                /* XMODEM doesn't include filename in protocol, must specify on command line */
                if (filename != NULL) {
                    argv[argc++] = (char *)filename;
                } else {
                    /* Use default filename if not specified */
                    argv[argc++] = "xmodem.dat";
                }
            } else if (protocol == TRANSFER_YMODEM_RECV) {
                argv[argc++] = "--ymodem";
            } else if (protocol == TRANSFER_ZMODEM_RECV) {
                /* ZMODEM receive options */
                argv[argc++] = "-b";  /* binary mode */
                argv[argc++] = "-y";  /* overwrite without prompt */
                /* NOTE: -v (verbose) option removed to prevent screen clutter from PTY control sequences */
            }

            argv[argc] = NULL;
            break;

        default:
            MB_LOG_ERROR("Invalid modem protocol: %d", protocol);
            return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Executing %s", transfer_protocol_name(protocol));

    return execute_external_program(program_path, argv, socket_fd, state, config, telnet_ctx);
}

/**
 * Execute ZMODEM/XMODEM/YMODEM transfer with multiple files
 */
int transfer_execute_modem_files(const transfer_config_t *config, transfer_state_t *state,
                                 int socket_fd, transfer_protocol_t protocol,
                                 char * const filenames[], int file_count,
                                 telnet_t *telnet_ctx)
{
    if (config == NULL || state == NULL) {
        return ERROR_INVALID_ARG;
    }

    const char *program_path;
    char *argv[64];  /* Large enough for program + options + many files */
    int argc = 0;

    /* Determine program and build arguments */
    switch (protocol) {
        case TRANSFER_ZMODEM_SEND:
        case TRANSFER_XMODEM_SEND:
        case TRANSFER_YMODEM_SEND:
            if (filenames == NULL || file_count == 0) {
                MB_LOG_ERROR("Filenames required for send operation");
                return ERROR_INVALID_ARG;
            }

            program_path = config->send_zmodem_path;
            argv[argc++] = (char *)program_path;

            /* Add protocol option */
            if (protocol == TRANSFER_XMODEM_SEND) {
                argv[argc++] = "--xmodem";
            } else if (protocol == TRANSFER_YMODEM_SEND) {
                argv[argc++] = "--ymodem";
            }
            /* ZMODEM is default, no flag needed */

            /* Add all filenames */
            for (int i = 0; i < file_count && argc < 63; i++) {
                argv[argc++] = filenames[i];
            }
            argv[argc] = NULL;

            /* Save first filename in state */
            if (file_count > 0) {
                strncpy(state->filename, filenames[0], sizeof(state->filename) - 1);
                state->filename[sizeof(state->filename) - 1] = '\0';

                /* If multiple files, append count */
                if (file_count > 1) {
                    size_t len = strlen(state->filename);
                    snprintf(state->filename + len, sizeof(state->filename) - len,
                            " (+%d more)", file_count - 1);
                }
            }
            break;

        case TRANSFER_ZMODEM_RECV:
        case TRANSFER_XMODEM_RECV:
        case TRANSFER_YMODEM_RECV:
            program_path = config->receive_zmodem_path;
            argv[argc++] = (char *)program_path;

            /* Add protocol option and flags */
            if (protocol == TRANSFER_XMODEM_RECV) {
                argv[argc++] = "--xmodem";
                /* XMODEM doesn't include filename in protocol, must specify on command line */
                if (filenames != NULL && file_count > 0) {
                    argv[argc++] = filenames[0];
                } else {
                    /* Use default filename if not specified */
                    argv[argc++] = "xmodem.dat";
                }
            } else if (protocol == TRANSFER_YMODEM_RECV) {
                argv[argc++] = "--ymodem";
            } else if (protocol == TRANSFER_ZMODEM_RECV) {
                /* ZMODEM receive options */
                argv[argc++] = "-b";  /* binary mode */
                argv[argc++] = "-y";  /* overwrite without prompt */
                /* NOTE: -v (verbose) option removed to prevent screen clutter from PTY control sequences */
            }

            argv[argc] = NULL;
            break;

        default:
            MB_LOG_ERROR("Invalid modem protocol: %d", protocol);
            return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Executing %s with %d file(s)", transfer_protocol_name(protocol), file_count);

    return execute_external_program(program_path, argv, socket_fd, state, config, telnet_ctx);
}

/**
 * Initialize XMODEM detector
 */
void xmodem_detector_init(xmodem_detector_t *detector)
{
    if (detector == NULL) {
        return;
    }

    memset(detector, 0, sizeof(xmodem_detector_t));
    detector->buf_len = 0;
    detector->last_char = 0;
    detector->repeat_count = 0;
    detector->first_seen = 0;
    detector->last_seen = 0;
    detector->enabled = true;
}

/**
 * Enable/disable XMODEM detection
 */
void xmodem_detector_set_enabled(xmodem_detector_t *detector, bool enabled)
{
    if (detector == NULL) {
        return;
    }

    detector->enabled = enabled;
    if (!enabled) {
        /* Clear state when disabled */
        detector->last_char = 0;
        detector->repeat_count = 0;
        detector->first_seen = 0;
        detector->last_seen = 0;
    }
}

/**
 * Detect XMODEM trigger sequences in incoming data
 * 1. XMODEM receiver sends repeated NAK (0x15) or 'C' (0x43) characters (remote wants to receive)
 * 2. XMODEM sender outputs text message "Give your local XMODEM receive command now." (remote wants to send)
 */
bool xmodem_detect_trigger(xmodem_detector_t *detector, const unsigned char *data, size_t len,
                           bool *is_receive_init, bool *is_send_init)
{
    if (detector == NULL || data == NULL || !detector->enabled) {
        return false;
    }

    if (is_receive_init) *is_receive_init = false;
    if (is_send_init) *is_send_init = false;

    /* Add new data to sliding window buffer */
    for (size_t i = 0; i < len; i++) {
        /* Shift buffer if full */
        if (detector->buf_len >= XMODEM_YMODEM_DETECT_BUFFER_SIZE) {
            memmove(detector->buffer, detector->buffer + 1, XMODEM_YMODEM_DETECT_BUFFER_SIZE - 1);
            detector->buf_len = XMODEM_YMODEM_DETECT_BUFFER_SIZE - 1;
        }

        /* Add new byte */
        detector->buffer[detector->buf_len++] = data[i];

        /* Check for text pattern "XMODEM receive" (sz output: "Give your local XMODEM receive command now.") */
        if (detector->buf_len >= 14) {
            /* Look for "XMODEM receive" at end of buffer (case insensitive) */
            bool found = true;
            size_t start = detector->buf_len - 14;
            const char *pattern = "XMODEM receive";
            for (size_t j = 0; j < 14; j++) {
                char c1 = (char)detector->buffer[start + j];
                char c2 = pattern[j];
                /* Case insensitive comparison */
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) {
                    found = false;
                    break;
                }
            }
            if (found) {
                MB_LOG_INFO("XMODEM text trigger detected: remote is sending");
                if (is_receive_init) *is_receive_init = true;
                detector->buf_len = 0;  /* Clear buffer */
                return true;
            }
        }

        /* Check for text pattern "XMODEM send" (server message: "Starting XMODEM send of ...") */
        if (detector->buf_len >= 11) {
            /* Look for "XMODEM send" at end of buffer (case insensitive) */
            bool found = true;
            size_t start = detector->buf_len - 11;
            const char *pattern = "XMODEM send";
            for (size_t j = 0; j < 11; j++) {
                char c1 = (char)detector->buffer[start + j];
                char c2 = pattern[j];
                /* Case insensitive comparison */
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) {
                    found = false;
                    break;
                }
            }
            if (found) {
                MB_LOG_INFO("XMODEM send trigger detected: remote is sending");
                if (is_receive_init) *is_receive_init = true;
                detector->buf_len = 0;  /* Clear buffer */
                return true;
            }
        }
    }

    time_t now = time(NULL);

    /* Scan incoming data for NAK or 'C' characters */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = data[i];

        /* Check for XMODEM trigger characters */
        if (c == XMODEM_NAK || c == XMODEM_CRC) {
            /* Check if this is the same character as before */
            if (c == detector->last_char) {
                /* Same character - increment count */
                detector->repeat_count++;
                detector->last_seen = now;

                /* Check if we have enough repeats within the time window */
                if (detector->repeat_count >= XMODEM_YMODEM_MIN_REPEATS) {
                    time_t elapsed = now - detector->first_seen;
                    if (elapsed <= XMODEM_YMODEM_DETECT_WINDOW) {
                        /* Pattern detected! Remote is ready to receive */
                        MB_LOG_INFO("XMODEM NAK/C trigger detected: remote is receiving");
                        if (is_send_init) *is_send_init = true;

                        /* Reset detector */
                        detector->last_char = 0;
                        detector->repeat_count = 0;
                        detector->first_seen = 0;
                        detector->last_seen = 0;

                        return true;
                    }
                }
            } else {
                /* Different character - start new sequence */
                detector->last_char = c;
                detector->repeat_count = 1;
                detector->first_seen = now;
                detector->last_seen = now;
            }
        } else if (c >= 0x20 && c < 0x7F && c != XMODEM_CRC) {
            /* Printable character (not 'C') - reset detection to avoid false positives */
            /* This prevents detecting normal text containing 'C' */
            detector->last_char = 0;
            detector->repeat_count = 0;
            detector->first_seen = 0;
            detector->last_seen = 0;
        }
    }

    /* Check for timeout - reset if too much time passed */
    if (detector->first_seen > 0 && (now - detector->last_seen) > XMODEM_YMODEM_DETECT_WINDOW) {
        detector->last_char = 0;
        detector->repeat_count = 0;
        detector->first_seen = 0;
        detector->last_seen = 0;
    }

    return false;
}

/**
 * Initialize YMODEM detector
 */
void ymodem_detector_init(ymodem_detector_t *detector)
{
    if (detector == NULL) {
        return;
    }

    memset(detector, 0, sizeof(ymodem_detector_t));
    detector->buf_len = 0;
    detector->c_repeat_count = 0;
    detector->first_seen = 0;
    detector->last_seen = 0;
    detector->enabled = true;
}

/**
 * Enable/disable YMODEM detection
 */
void ymodem_detector_set_enabled(ymodem_detector_t *detector, bool enabled)
{
    if (detector == NULL) {
        return;
    }

    detector->enabled = enabled;
    if (!enabled) {
        /* Clear state when disabled */
        detector->c_repeat_count = 0;
        detector->first_seen = 0;
        detector->last_seen = 0;
    }
}

/**
 * Detect YMODEM trigger sequences in incoming data
 * 1. YMODEM receiver sends repeated 'C' (0x43) characters for batch transfer (remote wants to receive)
 * 2. YMODEM sender outputs text message "Give your local YMODEM receive command now." (remote wants to send)
 */
bool ymodem_detect_trigger(ymodem_detector_t *detector, const unsigned char *data, size_t len,
                           bool *is_receive_init, bool *is_send_init)
{
    if (detector == NULL || data == NULL || !detector->enabled) {
        return false;
    }

    if (is_receive_init) *is_receive_init = false;
    if (is_send_init) *is_send_init = false;

    /* Add new data to sliding window buffer */
    for (size_t i = 0; i < len; i++) {
        /* Shift buffer if full */
        if (detector->buf_len >= XMODEM_YMODEM_DETECT_BUFFER_SIZE) {
            memmove(detector->buffer, detector->buffer + 1, XMODEM_YMODEM_DETECT_BUFFER_SIZE - 1);
            detector->buf_len = XMODEM_YMODEM_DETECT_BUFFER_SIZE - 1;
        }

        /* Add new byte */
        detector->buffer[detector->buf_len++] = data[i];

        /* Check for text pattern "YMODEM receive" (sz output: "Give your local YMODEM receive command now.") */
        if (detector->buf_len >= 14) {
            /* Look for "YMODEM receive" at end of buffer (case insensitive) */
            bool found = true;
            size_t start = detector->buf_len - 14;
            const char *pattern = "YMODEM receive";
            for (size_t j = 0; j < 14; j++) {
                char c1 = (char)detector->buffer[start + j];
                char c2 = pattern[j];
                /* Case insensitive comparison */
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) {
                    found = false;
                    break;
                }
            }
            if (found) {
                MB_LOG_INFO("YMODEM text trigger detected: remote is sending");
                if (is_receive_init) *is_receive_init = true;
                detector->buf_len = 0;  /* Clear buffer */
                return true;
            }
        }

        /* Check for text pattern "YMODEM send" (server message: "Starting YMODEM send of ...") */
        if (detector->buf_len >= 11) {
            /* Look for "YMODEM send" at end of buffer (case insensitive) */
            bool found = true;
            size_t start = detector->buf_len - 11;
            const char *pattern = "YMODEM send";
            for (size_t j = 0; j < 11; j++) {
                char c1 = (char)detector->buffer[start + j];
                char c2 = pattern[j];
                /* Case insensitive comparison */
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) {
                    found = false;
                    break;
                }
            }
            if (found) {
                MB_LOG_INFO("YMODEM send trigger detected: remote is sending");
                if (is_receive_init) *is_receive_init = true;
                detector->buf_len = 0;  /* Clear buffer */
                return true;
            }
        }
    }

    time_t now = time(NULL);

    /* Scan incoming data for 'C' characters */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = data[i];

        if (c == XMODEM_CRC) {  /* 'C' character */
            /* Increment count */
            detector->c_repeat_count++;
            detector->last_seen = now;

            /* Initialize first_seen if this is the first occurrence */
            if (detector->first_seen == 0) {
                detector->first_seen = now;
            }

            /* Check if we have enough repeats within the time window */
            if (detector->c_repeat_count >= XMODEM_YMODEM_MIN_REPEATS) {
                time_t elapsed = now - detector->first_seen;
                if (elapsed <= XMODEM_YMODEM_DETECT_WINDOW) {
                    /* Pattern detected! Remote is ready to receive */
                    MB_LOG_INFO("YMODEM 'C' trigger detected: remote is receiving");
                    if (is_send_init) *is_send_init = true;

                    /* Reset detector */
                    detector->c_repeat_count = 0;
                    detector->first_seen = 0;
                    detector->last_seen = 0;

                    return true;
                }
            }
        } else if (c >= 0x20 && c < 0x7F) {
            /* Other printable character - reset to avoid false positives */
            detector->c_repeat_count = 0;
            detector->first_seen = 0;
            detector->last_seen = 0;
        }
    }

    /* Check for timeout - reset if too much time passed */
    if (detector->first_seen > 0 && (now - detector->last_seen) > XMODEM_YMODEM_DETECT_WINDOW) {
        detector->c_repeat_count = 0;
        detector->first_seen = 0;
        detector->last_seen = 0;
    }

    return false;
}
