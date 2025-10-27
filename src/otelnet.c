/*
 * otelnet.c - Standalone telnet client
 *
 * Based on ModemBridge's telnet implementation
 * Supports console mode (Ctrl+M) and external program integration
 */

#include "otelnet.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <ctype.h>

/* Global signal handler flags */
static volatile sig_atomic_t g_running_local = 1;
static volatile sig_atomic_t g_winsize_changed = 0;

/* Utility functions (from common.c) */

/**
 * Trim leading and trailing whitespace from string (local version)
 */
static char *otelnet_trim_whitespace(char *str)
{
    char *end;

    if (str == NULL) {
        return NULL;
    }

    /* Trim leading space */
    while (isspace((unsigned char)*str)) {
        str++;
    }

    if (*str == 0) {
        return str;
    }

    /* Trim trailing space */
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }

    /* Write new null terminator */
    *(end + 1) = '\0';

    return str;
}

/**
 * Get current timestamp string in format [YYYY-MM-DD HH:MM:SS]
 * Returns pointer to internal static buffer
 */

/* UTF-8 helper functions (from bridge.c) - may be used for future enhancements */

/**
 * Check if byte is start of multibyte UTF-8 sequence
 */
__attribute__((unused))
static bool is_utf8_start(unsigned char byte)
{
    /* UTF-8 start bytes: 11xxxxxx */
    return (byte & 0xC0) == 0xC0 && (byte & 0xFE) != 0xFE;
}

/**
 * Check if byte is UTF-8 continuation byte
 */
__attribute__((unused))
static bool is_utf8_continuation(unsigned char byte)
{
    /* UTF-8 continuation bytes: 10xxxxxx */
    return (byte & 0xC0) == 0x80;
}

/**
 * Get expected length of UTF-8 sequence from first byte
 */
__attribute__((unused))
static int utf8_sequence_length(unsigned char byte)
{
    if ((byte & 0x80) == 0x00) {
        /* 0xxxxxxx - 1 byte (ASCII) */
        return 1;
    } else if ((byte & 0xE0) == 0xC0) {
        /* 110xxxxx - 2 bytes */
        return 2;
    } else if ((byte & 0xF0) == 0xE0) {
        /* 1110xxxx - 3 bytes */
        return 3;
    } else if ((byte & 0xF8) == 0xF0) {
        /* 11110xxx - 4 bytes */
        return 4;
    }

    /* Invalid */
    return 0;
}

/**
 * Signal handler
 */
static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        /* Request transfer cancellation if active */
        transfer_request_cancel();
        g_running_local = 0;
    } else if (signum == SIGWINCH) {
        g_winsize_changed = 1;
    }
}

/**
 * Initialize otelnet context
 */
void otelnet_init(otelnet_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    memset(ctx, 0, sizeof(otelnet_ctx_t));

    telnet_init(&ctx->telnet);

    ctx->mode = OTELNET_MODE_CLIENT;
    ctx->running = true;
    ctx->termios_saved = false;
    ctx->console_buffer_len = 0;
    ctx->line_buffer_len = 0;
    ctx->bytes_sent = 0;
    ctx->bytes_received = 0;
    ctx->connection_start_time = 0;
    ctx->log_fp = NULL;

    /* Initialize transfer state */
    transfer_init(&ctx->transfer);

    /* Initialize protocol detectors */
    zmodem_detector_init(&ctx->zmodem_detector);
    xmodem_detector_init(&ctx->xmodem_detector);
    ymodem_detector_init(&ctx->ymodem_detector);
}

/**
 * Load configuration from file
 */
int otelnet_load_config(otelnet_ctx_t *ctx, const char *config_file)
{
    FILE *fp;
    char line[LINE_BUFFER_SIZE];
    char key[SMALL_BUFFER_SIZE];
    char value[BUFFER_SIZE];

    if (ctx == NULL || config_file == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Set defaults */
    ctx->config.log_enabled = false;
    SAFE_STRNCPY(ctx->config.log_file, "otelnet.log", sizeof(ctx->config.log_file));

    /* Initialize transfer configuration with defaults */
    transfer_config_init(&ctx->config.transfer);

    fp = fopen(config_file, "r");
    if (fp == NULL) {
        fprintf(stderr, "[%s][WARNING] Could not open config file %s, using defaults\r\n", otelnet_get_timestamp(), config_file);
        return SUCCESS;  /* Not fatal, use defaults */
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }

        /* Parse KEY=VALUE */
        if (sscanf(line, "%255[^=]=%4095s", key, value) == 2) {
            /* Trim whitespace */
            char *k = otelnet_trim_whitespace(key);
            char *v = otelnet_trim_whitespace(value);

            /* Remove quotes from value */
            if (v[0] == '"' || v[0] == '\'') {
                v++;
                size_t len = strlen(v);
                if (len > 0 && (v[len-1] == '"' || v[len-1] == '\'')) {
                    v[len-1] = '\0';
                }
            }

            /* Parse configuration options */
            if (strcmp(k, "KERMIT") == 0) {
                SAFE_STRNCPY(ctx->config.transfer.kermit_path, v,
                           sizeof(ctx->config.transfer.kermit_path));
            } else if (strcmp(k, "SEND_ZMODEM") == 0) {
                SAFE_STRNCPY(ctx->config.transfer.send_zmodem_path, v,
                           sizeof(ctx->config.transfer.send_zmodem_path));
            } else if (strcmp(k, "RECEIVE_ZMODEM") == 0) {
                SAFE_STRNCPY(ctx->config.transfer.receive_zmodem_path, v,
                           sizeof(ctx->config.transfer.receive_zmodem_path));
            } else if (strcmp(k, "LOG") == 0) {
                ctx->config.log_enabled = (strcmp(v, "1") == 0 ||
                                          strcasecmp(v, "true") == 0 ||
                                          strcasecmp(v, "yes") == 0);
            } else if (strcmp(k, "LOG_FILE") == 0) {
                SAFE_STRNCPY(ctx->config.log_file, v, sizeof(ctx->config.log_file));
            } else if (strcmp(k, "AUTO_ZMODEM") == 0) {
                ctx->config.transfer.auto_zmodem_enabled = (strcmp(v, "1") == 0 ||
                                                           strcasecmp(v, "true") == 0 ||
                                                           strcasecmp(v, "yes") == 0);
            } else if (strcmp(k, "AUTO_ZMODEM_PROMPT") == 0) {
                ctx->config.transfer.auto_zmodem_prompt = (strcmp(v, "1") == 0 ||
                                                          strcasecmp(v, "true") == 0 ||
                                                          strcasecmp(v, "yes") == 0);
            } else if (strcmp(k, "AUTO_ZMODEM_DOWNLOAD_DIR") == 0) {
                SAFE_STRNCPY(ctx->config.transfer.auto_zmodem_download_dir, v,
                           sizeof(ctx->config.transfer.auto_zmodem_download_dir));
            } else if (strcmp(k, "AUTO_XMODEM") == 0) {
                ctx->config.transfer.auto_xmodem_enabled = (strcmp(v, "1") == 0 ||
                                                           strcasecmp(v, "true") == 0 ||
                                                           strcasecmp(v, "yes") == 0);
            } else if (strcmp(k, "AUTO_XMODEM_PROMPT") == 0) {
                ctx->config.transfer.auto_xmodem_prompt = (strcmp(v, "1") == 0 ||
                                                          strcasecmp(v, "true") == 0 ||
                                                          strcasecmp(v, "yes") == 0);
            } else if (strcmp(k, "AUTO_YMODEM") == 0) {
                ctx->config.transfer.auto_ymodem_enabled = (strcmp(v, "1") == 0 ||
                                                           strcasecmp(v, "true") == 0 ||
                                                           strcasecmp(v, "yes") == 0);
            } else if (strcmp(k, "AUTO_YMODEM_PROMPT") == 0) {
                ctx->config.transfer.auto_ymodem_prompt = (strcmp(v, "1") == 0 ||
                                                          strcasecmp(v, "true") == 0 ||
                                                          strcasecmp(v, "yes") == 0);
            } else if (strcmp(k, "TRANSFER_TIMEOUT") == 0) {
                ctx->config.transfer.transfer_timeout_seconds = atoi(v);
            } else if (strcmp(k, "TRANSFER_DATA_TIMEOUT") == 0) {
                ctx->config.transfer.transfer_data_timeout_seconds = atoi(v);
            } else if (strcmp(k, "TRANSFER_LOG") == 0) {
                ctx->config.transfer.transfer_log_enabled = (strcmp(v, "1") == 0 ||
                                                            strcasecmp(v, "true") == 0 ||
                                                            strcasecmp(v, "yes") == 0);
            } else if (strcmp(k, "TRANSFER_LOG_FILE") == 0) {
                SAFE_STRNCPY(ctx->config.transfer.transfer_log_file, v,
                           sizeof(ctx->config.transfer.transfer_log_file));
            } else if (strcmp(k, "TRANSFER_KEEP_PARTIAL") == 0) {
                ctx->config.transfer.transfer_keep_partial = (strcmp(v, "1") == 0 ||
                                                             strcasecmp(v, "true") == 0 ||
                                                             strcasecmp(v, "yes") == 0);
            }
        }
    }

    fclose(fp);

    printf("[%s][INFO] Configuration loaded from %s\r\n", otelnet_get_timestamp(), config_file); fflush(stdout);
    printf("[%s][INFO]   KERMIT: %s\r\n", otelnet_get_timestamp(), ctx->config.transfer.kermit_path); fflush(stdout);
    printf("[%s][INFO]   SEND_ZMODEM: %s\r\n", otelnet_get_timestamp(), ctx->config.transfer.send_zmodem_path); fflush(stdout);
    printf("[%s][INFO]   RECEIVE_ZMODEM: %s\r\n", otelnet_get_timestamp(), ctx->config.transfer.receive_zmodem_path); fflush(stdout);
    printf("[%s][INFO]   LOG: %s\r\n", otelnet_get_timestamp(), ctx->config.log_enabled ? "enabled" : "disabled"); fflush(stdout);
    if (ctx->config.log_enabled) {
        printf("[%s][INFO]   LOG_FILE: %s\r\n", otelnet_get_timestamp(), ctx->config.log_file); fflush(stdout);
    }
    printf("[%s][INFO]   AUTO_ZMODEM: %s\r\n", otelnet_get_timestamp(), ctx->config.transfer.auto_zmodem_enabled ? "enabled" : "disabled"); fflush(stdout);
    printf("[%s][INFO]   TRANSFER_TIMEOUT: %d seconds\r\n", otelnet_get_timestamp(), ctx->config.transfer.transfer_timeout_seconds); fflush(stdout);

    return SUCCESS;
}

/**
 * Setup terminal for raw mode
 */
int otelnet_setup_terminal(otelnet_ctx_t *ctx)
{
    struct termios raw;

    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Save original terminal settings */
    if (tcgetattr(STDIN_FILENO, &ctx->orig_termios) < 0) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Failed to get terminal attributes: %s\r\n", otelnet_get_timestamp(), __FILE__, __LINE__, strerror(errno));
        return ERROR_IO;
    }
    ctx->termios_saved = true;

    /* Set up raw mode */
    raw = ctx->orig_termios;

    /* Input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    /* Output modes: disable post processing */
    raw.c_oflag &= ~(OPOST);

    /* Control modes: set 8 bit chars */
    raw.c_cflag |= (CS8);

    /* Local modes: echoing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    /* Control chars: set return condition: min number of bytes and timer */
    raw.c_cc[VMIN] = 0;   /* Return each byte, or zero for timeout */
    raw.c_cc[VTIME] = 1;  /* 100ms timeout */

    /* Apply terminal settings */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Failed to set terminal attributes: %s\r\n", otelnet_get_timestamp(), __FILE__, __LINE__, strerror(errno));
        return ERROR_IO;
    }

    /* Set stdin to non-blocking */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    #ifdef DEBUG
    printf("[DEBUG] %s:%d: Terminal setup complete (raw mode)\r\n", __FILE__, __LINE__); fflush(stdout);
#endif

    return SUCCESS;
}

/**
 * Restore terminal to original settings
 */
void otelnet_restore_terminal(otelnet_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->termios_saved) {
        return;
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &ctx->orig_termios);

    /* Restore blocking mode */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
    }

    #ifdef DEBUG
    printf("[DEBUG] %s:%d: Terminal restored\r\n", __FILE__, __LINE__); fflush(stdout);
#endif
}

/**
 * Connect to telnet server
 */
int otelnet_connect(otelnet_ctx_t *ctx, const char *host, int port)
{
    int ret;

    if (ctx == NULL || host == NULL) {
        return ERROR_INVALID_ARG;
    }

    printf("[%s][INFO] Connecting to %s:%d...\r\n", otelnet_get_timestamp(), host, port); fflush(stdout);
    printf("Connecting to %s:%d...\r\n", host, port);

    ret = telnet_connect(&ctx->telnet, host, port);
    if (ret != SUCCESS) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Failed to connect to %s:%d\r\n", otelnet_get_timestamp(), __FILE__, __LINE__, host, port);
        printf("Connection failed: %s\r\n", strerror(errno));
        return ret;
    }

    ctx->connection_start_time = time(NULL);
    printf("Connected to %s:%d\r\n", host, port);
    printf("Press Ctrl+] for console mode\r\n");

    /* Get initial window size and store it */
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        ctx->telnet.term_width = ws.ws_col;
        ctx->telnet.term_height = ws.ws_row;
        #ifdef DEBUG
    printf("[DEBUG] %s:%d: Initial window size: %dx%d\r\n", __FILE__, __LINE__, ctx->telnet.term_width, ctx->telnet.term_height); fflush(stdout);
#endif
    }

    return SUCCESS;
}

/**
 * Disconnect from telnet server
 */
void otelnet_disconnect(otelnet_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (telnet_is_connected(&ctx->telnet)) {
        printf("[%s][INFO] Disconnecting from telnet server\r\n", otelnet_get_timestamp()); fflush(stdout);
        telnet_disconnect(&ctx->telnet);
        printf("\r\nConnection closed\r\n");
    }
}

/**
 * Update terminal window size and send NAWS subnegotiation
 */
static int otelnet_update_window_size(otelnet_ctx_t *ctx)
{
    struct winsize ws;

    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Get current window size using ioctl */
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        int new_width = ws.ws_col;
        int new_height = ws.ws_row;

        /* Check if size actually changed */
        if (new_width != ctx->telnet.term_width || new_height != ctx->telnet.term_height) {
            printf("[%s][INFO] Window size changed: %dx%d -> %dx%d\r\n", otelnet_get_timestamp(), ctx->telnet.term_width, ctx->telnet.term_height,
                       new_width, new_height); fflush(stdout);

            /* Update stored size */
            ctx->telnet.term_width = new_width;
            ctx->telnet.term_height = new_height;

            /* Send NAWS if negotiated */
            if (ctx->telnet.local_options[TELOPT_NAWS] && telnet_is_connected(&ctx->telnet)) {
                telnet_send_naws(&ctx->telnet, new_width, new_height);
            }
        }

        /* Reset flag */
        g_winsize_changed = 0;

        return SUCCESS;
    } else {
        fprintf(stderr, "[%s][WARNING] Failed to get window size: %s\r\n", otelnet_get_timestamp(), strerror(errno));
        g_winsize_changed = 0;
        return ERROR_IO;
    }
}

/**
 * Open log file for writing
 */
static int otelnet_open_log(otelnet_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->config.log_enabled) {
        return SUCCESS;
    }

    ctx->log_fp = fopen(ctx->config.log_file, "a");
    if (ctx->log_fp == NULL) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Failed to open log file %s: %s\r\n", otelnet_get_timestamp(), __FILE__, __LINE__, ctx->config.log_file, strerror(errno));
        printf("Warning: Failed to open log file %s\r\n", ctx->config.log_file);
        ctx->config.log_enabled = false;
        return ERROR_IO;
    }

    /* Write log start marker */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(ctx->log_fp, "\n[%s] === Session started ===\n", timestamp);
    fflush(ctx->log_fp);

    printf("[%s][INFO] Logging enabled to %s\r\n", otelnet_get_timestamp(), ctx->config.log_file); fflush(stdout);
    printf("Logging to: %s\r\n", ctx->config.log_file);

    return SUCCESS;
}

/**
 * Close log file
 */
static void otelnet_close_log(otelnet_ctx_t *ctx)
{
    if (ctx == NULL || ctx->log_fp == NULL) {
        return;
    }

    /* Write log end marker */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(ctx->log_fp, "[%s] === Session ended ===\n\n", timestamp);

    fclose(ctx->log_fp);
    ctx->log_fp = NULL;
}

/**
 * Write data to log file
 */
void otelnet_log_data(otelnet_ctx_t *ctx, const char *direction,
                      const unsigned char *data, size_t len)
{
    if (ctx == NULL || ctx->log_fp == NULL || !ctx->config.log_enabled) {
        return;
    }

    if (data == NULL || len == 0) {
        return;
    }

    /* Get timestamp */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    /* Write log entry */
    fprintf(ctx->log_fp, "[%s][%s] ", timestamp, direction);

    /* Write data as hex and ASCII */
    for (size_t i = 0; i < len; i++) {
        fprintf(ctx->log_fp, "%02x ", data[i]);
        if ((i + 1) % 16 == 0 && i < len - 1) {
            fprintf(ctx->log_fp, " | ");
            for (size_t j = i - 15; j <= i; j++) {
                if (isprint(data[j])) {
                    fprintf(ctx->log_fp, "%c", data[j]);
                } else {
                    fprintf(ctx->log_fp, ".");
                }
            }
            fprintf(ctx->log_fp, "\n[%s][%s] ", timestamp, direction);
        }
    }

    /* Write remaining ASCII */
    size_t remaining = len % 16;
    if (remaining > 0 || len < 16) {
        size_t spaces = (16 - remaining) * 3;
        for (size_t i = 0; i < spaces; i++) {
            fprintf(ctx->log_fp, " ");
        }
        fprintf(ctx->log_fp, " | ");
        size_t start = len - remaining;
        if (len < 16) {
            start = 0;
            remaining = len;
        }
        for (size_t i = start; i < start + remaining; i++) {
            if (isprint(data[i])) {
                fprintf(ctx->log_fp, "%c", data[i]);
            } else {
                fprintf(ctx->log_fp, ".");
            }
        }
    }

    fprintf(ctx->log_fp, "\n");
    fflush(ctx->log_fp);
}

/**
 * Enter console mode
 */
void otelnet_enter_console_mode(otelnet_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    ctx->mode = OTELNET_MODE_CONSOLE;
    ctx->console_buffer_len = 0;
    memset(ctx->console_buffer, 0, sizeof(ctx->console_buffer));

    printf("\r\n[Console Mode - Enter empty line to return, 'quit' to exit]\r\n");
    printf("otelnet> ");
    fflush(stdout);
}

/**
 * Exit console mode
 */
void otelnet_exit_console_mode(otelnet_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    ctx->mode = OTELNET_MODE_CLIENT;
    ctx->console_buffer_len = 0;

    printf("\r\n[Back to client mode]\r\n");
    fflush(stdout);
}

/**
 * Check if program exists in PATH
 */
static bool otelnet_check_program_exists(const char *program)
{
    char path_env[BUFFER_SIZE];
    char *path_copy, *dir;
    char full_path[BUFFER_SIZE];

    if (program == NULL || strlen(program) == 0) {
        return false;
    }

    /* If program contains '/', check directly */
    if (strchr(program, '/') != NULL) {
        return access(program, X_OK) == 0;
    }

    /* Get PATH environment variable */
    const char *path = getenv("PATH");
    if (path == NULL) {
        return false;
    }

    SAFE_STRNCPY(path_env, path, sizeof(path_env));
    path_copy = path_env;

    /* Search in PATH directories */
    while ((dir = strsep(&path_copy, ":")) != NULL) {
        if (strlen(dir) == 0) {
            continue;
        }

        snprintf(full_path, sizeof(full_path), "%s/%s", dir, program);
        if (access(full_path, X_OK) == 0) {
            return true;
        }
    }

    return false;
}

/**
 * Parse command into program and arguments
 *
 * IMPORTANT: parse_buffer will be modified by strsep(), and args[] will point
 * into this buffer. The caller must ensure parse_buffer remains valid for the
 * lifetime of the args[] pointers.
 */
static int otelnet_parse_command_args(const char *input, char *parse_buffer, size_t buffer_size,
                                      char *program, size_t prog_size,
                                      char **args, int max_args, int *arg_count)
{
    char *input_copy, *token;
    int count = 0;

    if (input == NULL || parse_buffer == NULL || program == NULL ||
        args == NULL || arg_count == NULL) {
        return ERROR_INVALID_ARG;
    }

    SAFE_STRNCPY(parse_buffer, input, buffer_size);
    input_copy = parse_buffer;

    /* Get program name */
    token = strsep(&input_copy, " \t");
    if (token == NULL) {
        return ERROR_INVALID_ARG;
    }
    SAFE_STRNCPY(program, token, prog_size);

    /* Get arguments */
    while ((token = strsep(&input_copy, " \t")) != NULL && count < max_args) {
        if (strlen(token) > 0) {
            args[count++] = token;
        }
    }

    *arg_count = count;
    return SUCCESS;
}

/**
 * Execute external program with arguments
 */
int otelnet_execute_external_program_with_args(otelnet_ctx_t *ctx, const char *program_path,
                                               char * const argv[])
{
    pid_t pid;
    int status;

    if (ctx == NULL || program_path == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (strlen(program_path) == 0) {
        printf("Error: Program path not configured\r\n");
        return ERROR_CONFIG;
    }

    /* Check if program exists */
    if (!otelnet_check_program_exists(program_path)) {
        printf("\r\nError: Program '%s' not found in PATH\r\n", program_path);
        printf("Tip: Check your configuration file or install the program\r\n");
        return ERROR_CONFIG;
    }

    /* Show execution info */
    printf("\r\n[Executing: %s", program_path);
    if (argv != NULL) {
        for (int i = 1; argv[i] != NULL; i++) {
            printf(" %s", argv[i]);
        }
    }
    printf("]\r\n");
    printf("[Telnet session will be redirected to the program]\r\n");
    printf("[The program will exit when transfer completes]\r\n\r\n");

    /* Restore terminal before running external program */
    otelnet_restore_terminal(ctx);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Failed to fork: %s\r\n", otelnet_get_timestamp(), __FILE__, __LINE__, strerror(errno));
        printf("Error: Failed to fork process\r\n");
        otelnet_setup_terminal(ctx);
        return ERROR_GENERAL;
    } else if (pid == 0) {
        /* Child process */
        /* Get telnet FD */
        int telnet_fd = telnet_get_fd(&ctx->telnet);

        /* Redirect stdin/stdout/stderr to telnet socket */
        if (telnet_fd >= 0) {
            dup2(telnet_fd, STDIN_FILENO);
            dup2(telnet_fd, STDOUT_FILENO);
            dup2(telnet_fd, STDERR_FILENO);
        }

        /* Execute program with arguments */
        if (argv != NULL) {
            execvp(program_path, (char * const *)argv);
        } else {
            execlp(program_path, program_path, NULL);
        }

        /* If we get here, exec failed */
        fprintf(stderr, "Error: Failed to execute %s: %s\r\n", program_path, strerror(errno));
        exit(EXIT_FAILURE);
    } else {
        /* Parent process */
        printf("[%s][INFO] Waiting for child process %d (%s) to complete\r\n", otelnet_get_timestamp(), pid, program_path); fflush(stdout);

        /* Wait for child to complete */
        waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            printf("[%s][INFO] Program exited with code %d\r\n", otelnet_get_timestamp(), exit_code); fflush(stdout);
            printf("\r\n[Program exited with code %d]\r\n", exit_code);
            if (exit_code == 0) {
                printf("[Transfer completed successfully]\r\n");
            } else {
                printf("[Transfer may have failed - check exit code]\r\n");
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            fprintf(stderr, "[%s][WARNING] Program terminated by signal %d\r\n", otelnet_get_timestamp(), sig);
            printf("\r\n[Program terminated by signal %d]\r\n", sig);
        }

        /* Restore raw mode */
        otelnet_setup_terminal(ctx);
    }

    return SUCCESS;
}

/**
 * Execute external program (legacy wrapper)
 */
int otelnet_execute_external_program(otelnet_ctx_t *ctx, const char *program_path)
{
    return otelnet_execute_external_program_with_args(ctx, program_path, NULL);
}

/**
 * Execute file transfer using transfer module
 * Handles mode management, logging, and protocol execution
 */
static int otelnet_execute_transfer(otelnet_ctx_t *ctx,
                                    transfer_protocol_t protocol,
                                    const char *filename)
{
    int telnet_fd;
    int result;
    transfer_error_t error;

    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Get telnet socket file descriptor */
    telnet_fd = telnet_get_fd(&ctx->telnet);
    if (telnet_fd < 0) {
        printf("\r\nError: Not connected to telnet server\r\n");
        return ERROR_CONNECTION;
    }

    /* Enter transfer mode */
    if (transfer_enter_mode(&ctx->transfer, protocol) != SUCCESS) {
        printf("\r\nError: Failed to enter transfer mode\r\n");
        return ERROR_GENERAL;
    }

    /* Set filename if provided */
    if (filename != NULL) {
        strncpy(ctx->transfer.filename, filename, sizeof(ctx->transfer.filename) - 1);
        ctx->transfer.filename[sizeof(ctx->transfer.filename) - 1] = '\0';
    }

    /* Save current telnet protocol state */
    printf("[%s][INFO] Saving current telnet protocol state\r\n", otelnet_get_timestamp()); fflush(stdout);
    telnet_save_state(&ctx->telnet,
                     &ctx->transfer.saved_binary_local,
                     &ctx->transfer.saved_binary_remote,
                     &ctx->transfer.saved_echo_local,
                     &ctx->transfer.saved_echo_remote,
                     &ctx->transfer.saved_sga_local,
                     &ctx->transfer.saved_sga_remote,
                     &ctx->transfer.saved_linemode_active);

    /* Print telnet mode before transfer (DEBUG only) */
    telnet_debug_print_mode(&ctx->telnet, "Before file transfer");

    /* Check if already in BINARY mode to avoid unnecessary negotiation */
    bool already_binary = ctx->telnet.binary_local && ctx->telnet.binary_remote;
    int drain_count;

    if (!already_binary) {
        /* Request BINARY mode for 8-bit clean transmission */
        printf("[%s][INFO] Requesting BINARY mode (before: binary_local=%d, binary_remote=%d)\r\n", otelnet_get_timestamp(), ctx->telnet.binary_local, ctx->telnet.binary_remote); fflush(stdout);
        telnet_request_binary_mode(&ctx->telnet);
        printf("[%s][INFO] BINARY mode requested (after: binary_local=%d, binary_remote=%d)\r\n", otelnet_get_timestamp(), ctx->telnet.binary_local, ctx->telnet.binary_remote); fflush(stdout);

        /* Wait briefly and drain any pending BINARY negotiation packets
         * This ensures server's WILL/DO BINARY responses are consumed before
         * Kermit transfer starts. We don't use the while loop check because
         * telnet_request_binary_mode() sets flags immediately (optimistic).
         * Instead, we just wait a short time for network round-trip. */
        printf("\r\n[%s] [Waiting for BINARY mode negotiation...]\r\n", otelnet_get_timestamp());
        printf("[%s][INFO] Waiting for BINARY mode negotiation responses...\r\n", otelnet_get_timestamp()); fflush(stdout);

        drain_count = 3;  /* 300ms total (100ms * 3) - enough for network RTT */
    } else {
        /* Already in BINARY mode - just do a quick drain to clear any text */
        printf("[%s][INFO] Already in BINARY mode (local=%d, remote=%d), skipping negotiation\r\n", otelnet_get_timestamp(), ctx->telnet.binary_local, ctx->telnet.binary_remote); fflush(stdout);
        printf("\r\n[%s] [BINARY mode already active]\r\n", otelnet_get_timestamp());
        printf("[%s][INFO] Performing quick drain to clear any pending text messages...\r\n", otelnet_get_timestamp()); fflush(stdout);

        drain_count = 2;  /* 200ms - shorter drain since no negotiation needed */
    }
    while (drain_count > 0) {
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(ctx->telnet.fd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  /* 100ms timeout */

        int ret = select(ctx->telnet.fd + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(ctx->telnet.fd, &readfds)) {
            /* Drain BINARY negotiation responses only
             * Mode is still CLIENT/CONSOLE, so data won't be lost */
            unsigned char drain_buf[256];
            ssize_t n = recv(ctx->telnet.fd, drain_buf, sizeof(drain_buf), MSG_DONTWAIT);
            if (n > 0) {
                #ifdef DEBUG
    printf("[DEBUG] %s:%d: Drained %zd bytes during BINARY negotiation\r\n", __FILE__, __LINE__, n); fflush(stdout);
#endif
                /* Process only telnet protocol (IAC sequences)
                 * Any non-telnet data is saved to pending buffer for Kermit */
                unsigned char proc_buf[256];
                size_t proc_len;
                telnet_process_input(&ctx->telnet, drain_buf, n, proc_buf, sizeof(proc_buf), &proc_len);
                if (proc_len > 0) {
                    /* Check if this is a valid Kermit packet or just text */
                    bool is_kermit_packet = false;

                    /* Method 1: Check for SOH (0x01) at start */
                    if (proc_buf[0] == 0x01) {  /* SOH */
                        is_kermit_packet = true;
                        #ifdef DEBUG
    printf("[DEBUG] %s:%d: Detected SOH in early data - likely Kermit packet\r\n", __FILE__, __LINE__); fflush(stdout);
#endif
                    }
                    /* Method 2: Check if looks like Kermit packet (LEN field in valid range) */
                    else if (proc_len >= 4) {
                        unsigned char len_field = proc_buf[0];
                        /* Valid Kermit LEN field: (SP + 3) to (SP + 94) = 35 to 126 (printable ASCII) */
                        if (len_field >= 35 && len_field <= 126) {
                            /* Additional check: SEQ and TYPE should also be printable */
                            unsigned char seq_field = proc_buf[1];
                            unsigned char type_field = proc_buf[2];
                            if (seq_field >= 32 && seq_field <= 126 &&
                                type_field >= 32 && type_field <= 126) {
                                is_kermit_packet = true;
                                #ifdef DEBUG
    printf("[DEBUG] %s:%d: Detected valid Kermit packet format (LEN=%d, SEQ=%d, TYPE=%c)\r\n", __FILE__, __LINE__, len_field - 32, seq_field - 32, type_field); fflush(stdout);
#endif
                            }
                        }
                    }

                    if (is_kermit_packet) {
                        /* Valid Kermit packet - save it to prevent data loss */
                        fprintf(stderr, "[%s][WARNING] Early Kermit packet arrival during BINARY negotiation: %zu bytes\r\n", otelnet_get_timestamp(), proc_len);

                        /* Check if we have space in pending buffer */
                        if (ctx->pending_data_len + proc_len <= sizeof(ctx->pending_data)) {
                            memcpy(ctx->pending_data + ctx->pending_data_len, proc_buf, proc_len);
                            ctx->pending_data_len += proc_len;
                            printf("[%s][INFO] Saved Kermit packet to pending buffer (total: %zu bytes)\r\n", otelnet_get_timestamp(), ctx->pending_data_len); fflush(stdout);
                        } else {
                            fprintf(stderr, "[%s][ERROR] %s:%d: Pending buffer overflow! Lost %zu bytes of data\r\n", otelnet_get_timestamp(), __FILE__, __LINE__, proc_len);
                        }

                        /* Early packet detected - no need to continue draining
                         * The transfer will start soon anyway */
                        break;
                    } else {
                        /* Text message (like countdown) - discard it to prevent NAK */
                        printf("[%s][INFO] Discarding non-Kermit text during drain: %zu bytes\r\n", otelnet_get_timestamp(), proc_len); fflush(stdout);

                        /* Log first 60 chars for debugging */
                        if (proc_len > 0) {
                            char text_preview[61];
                            size_t preview_len = proc_len < 60 ? proc_len : 60;
                            memcpy(text_preview, proc_buf, preview_len);
                            text_preview[preview_len] = '\0';
                            /* Replace non-printable chars with '.' for readability */
                            for (size_t i = 0; i < preview_len; i++) {
                                if (text_preview[i] < 32 || text_preview[i] > 126) {
                                    text_preview[i] = '.';
                                }
                            }
                            #ifdef DEBUG
    printf("[DEBUG] %s:%d: Discarded text: [%s]\r\n", __FILE__, __LINE__, text_preview); fflush(stdout);
#endif
                        }

                        /* Do NOT save to pending buffer - continue draining */
                    }
                }
            }
        }
        drain_count--;
    }

    printf("\r\n[%s] [BINARY mode negotiation complete]\r\n", otelnet_get_timestamp());
    printf("[%s][INFO] BINARY mode negotiation complete\r\n", otelnet_get_timestamp()); fflush(stdout);

    /* NOW set application mode - after negotiation drain
     * This prevents otelnet_process_telnet() from discarding Kermit data */
    ctx->mode = OTELNET_MODE_TRANSFER;
    printf("[%s][INFO] Application mode set to TRANSFER\r\n", otelnet_get_timestamp()); fflush(stdout);

    /* Start transfer logging */
    transfer_log_start(&ctx->config.transfer, &ctx->transfer);

    /* Execute transfer based on protocol */
    switch (protocol) {
        case TRANSFER_KERMIT_SEND:
            result = transfer_execute_kermit_send(&ctx->config.transfer,
                                                 &ctx->transfer,
                                                 telnet_fd,
                                                 filename,
                                                 &ctx->telnet,
                                                 (struct otelnet_ctx *)ctx);
            break;

        case TRANSFER_KERMIT_RECV:
            result = transfer_execute_kermit_receive(&ctx->config.transfer,
                                                    &ctx->transfer,
                                                    telnet_fd,
                                                    &ctx->telnet,
                                                    (struct otelnet_ctx *)ctx);
            break;

        case TRANSFER_ZMODEM_SEND:
        case TRANSFER_XMODEM_SEND:
        case TRANSFER_YMODEM_SEND:
        case TRANSFER_ZMODEM_RECV:
        case TRANSFER_XMODEM_RECV:
        case TRANSFER_YMODEM_RECV:
            result = transfer_execute_modem(&ctx->config.transfer,
                                           &ctx->transfer,
                                           telnet_fd,
                                           protocol,
                                           filename,
                                           &ctx->telnet);
            break;

        default:
            printf("\r\nError: Unsupported protocol type: %d\r\n", protocol);
            result = ERROR_INVALID_ARG;
            break;
    }

    /* Determine error type based on result */
    if (result == SUCCESS) {
        error = TRANSFER_ERROR_NONE;
    } else if (transfer_is_cancel_requested()) {
        /* User cancelled the transfer */
        error = TRANSFER_ERROR_USER_CANCEL;
        printf("\r\n\nTransfer cancelled by user\r\n");
    } else if (result == ERROR_TIMEOUT) {
        error = TRANSFER_ERROR_TIMEOUT;
    } else if (result == ERROR_CONNECTION) {
        error = TRANSFER_ERROR_NETWORK;
    } else if (result == ERROR_IO) {
        error = TRANSFER_ERROR_PERMISSION;
    } else {
        error = TRANSFER_ERROR_UNKNOWN;
    }

    /* End transfer logging */
    transfer_log_end(&ctx->config.transfer, &ctx->transfer, error);

    /* Exit transfer mode */
    transfer_exit_mode(&ctx->transfer);

    /* Clear protocol detector buffers to prevent false triggers from leftover data */
    zmodem_detector_init(&ctx->zmodem_detector);
    xmodem_detector_init(&ctx->xmodem_detector);
    ymodem_detector_init(&ctx->ymodem_detector);

    /* Restore telnet protocol state to original values */
    telnet_restore_state(&ctx->telnet,
                        ctx->transfer.saved_binary_local,
                        ctx->transfer.saved_binary_remote,
                        ctx->transfer.saved_echo_local,
                        ctx->transfer.saved_echo_remote,
                        ctx->transfer.saved_sga_local,
                        ctx->transfer.saved_sga_remote,
                        ctx->transfer.saved_linemode_active);

    /* Flush socket buffer to prevent stale data from triggering auto-detection
     * After BINARY mode exit, socket may contain:
     * - IAC negotiation responses (WONT/DONT BINARY)
     * - Server text data (timestamps, prompts)
     * These must be discarded to prevent false ZMODEM/XMODEM/YMODEM triggers */
    telnet_fd = telnet_get_fd(&ctx->telnet);
    if (telnet_fd >= 0) {
        unsigned char flush_buf[BUFFER_SIZE];
        ssize_t flushed_total = 0;
        int flush_attempts = 0;
        const int MAX_FLUSH_ATTEMPTS = 10;

        /* Give server time to send any pending data */
        usleep(100000);  /* 100ms */

        while (flush_attempts < MAX_FLUSH_ATTEMPTS) {
            ssize_t n = recv(telnet_fd, flush_buf, sizeof(flush_buf), MSG_DONTWAIT);
            if (n > 0) {
                flushed_total += n;
                #ifdef DEBUG
    printf("[DEBUG] %s:%d: Flushed %zd bytes from socket after transfer (attempt %d)\r\n", __FILE__, __LINE__, n, flush_attempts + 1); fflush(stdout);
#endif
            } else if (n == 0) {
                /* Connection closed */
                break;
            } else {
                /* No more data or error */
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* No more data available */
                    break;
                }
                /* Other errors, stop flushing */
                break;
            }
            flush_attempts++;
            /* Small delay between attempts to allow data to arrive */
            if (flush_attempts < MAX_FLUSH_ATTEMPTS) {
                usleep(10000);  /* 10ms */
            }
        }

        if (flushed_total > 0) {
            printf("[%s][INFO] Flushed %zd bytes from socket buffer after transfer\r\n", otelnet_get_timestamp(), flushed_total); fflush(stdout);
        }
    }

    /* Print telnet mode after transfer (DEBUG only) */
    telnet_debug_print_mode(&ctx->telnet, "After file transfer");

    /* Clear cancellation flag */
    transfer_clear_cancel();

    /* Restore application mode */
    ctx->mode = OTELNET_MODE_CLIENT;

    return result;
}

/**
 * Execute file transfer with multiple files using transfer module
 * Handles mode management, logging, and protocol execution
 */
static int otelnet_execute_transfer_multi(otelnet_ctx_t *ctx,
                                          transfer_protocol_t protocol,
                                          char * const filenames[],
                                          int file_count)
{
    int telnet_fd;
    int result;
    transfer_error_t error;

    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Get telnet socket file descriptor */
    telnet_fd = telnet_get_fd(&ctx->telnet);
    if (telnet_fd < 0) {
        printf("\r\nError: Not connected to telnet server\r\n");
        return ERROR_CONNECTION;
    }

    /* Enter transfer mode */
    if (transfer_enter_mode(&ctx->transfer, protocol) != SUCCESS) {
        printf("\r\nError: Failed to enter transfer mode\r\n");
        return ERROR_GENERAL;
    }

    /* Save current telnet protocol state */
    telnet_save_state(&ctx->telnet,
                     &ctx->transfer.saved_binary_local,
                     &ctx->transfer.saved_binary_remote,
                     &ctx->transfer.saved_echo_local,
                     &ctx->transfer.saved_echo_remote,
                     &ctx->transfer.saved_sga_local,
                     &ctx->transfer.saved_sga_remote,
                     &ctx->transfer.saved_linemode_active);

    /* Print telnet mode before transfer (DEBUG only) */
    telnet_debug_print_mode(&ctx->telnet, "Before file transfer");

    /* Request BINARY mode for 8-bit clean transmission */
    telnet_request_binary_mode(&ctx->telnet);

    /* Wait briefly and drain any pending BINARY negotiation packets */
    printf("\r\n[%s] [Waiting for BINARY mode negotiation...]\r\n", otelnet_get_timestamp());
    printf("[%s][INFO] Waiting for BINARY mode negotiation responses...\r\n", otelnet_get_timestamp()); fflush(stdout);

    int drain_count = 3;  /* 300ms total (100ms * 3) - enough for network RTT */
    while (drain_count > 0) {
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(ctx->telnet.fd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  /* 100ms timeout */

        int ret = select(ctx->telnet.fd + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(ctx->telnet.fd, &readfds)) {
            /* Drain BINARY negotiation responses only */
            unsigned char drain_buf[256];
            ssize_t n = recv(ctx->telnet.fd, drain_buf, sizeof(drain_buf), MSG_DONTWAIT);
            if (n > 0) {
                #ifdef DEBUG
    printf("[DEBUG] %s:%d: Drained %zd bytes during BINARY negotiation\r\n", __FILE__, __LINE__, n); fflush(stdout);
#endif
                unsigned char proc_buf[256];
                size_t proc_len;
                telnet_process_input(&ctx->telnet, drain_buf, n, proc_buf, sizeof(proc_buf), &proc_len);
                if (proc_len > 0) {
                    fprintf(stderr, "[%s][WARNING] Unexpected %zu bytes of data during BINARY negotiation\r\n", otelnet_get_timestamp(), proc_len);
                }
            }
        }
        drain_count--;
    }

    printf("\r\n[%s] [BINARY mode negotiation complete]\r\n", otelnet_get_timestamp());
    printf("[%s][INFO] BINARY mode negotiation complete\r\n", otelnet_get_timestamp()); fflush(stdout);

    /* NOW set application mode - after negotiation drain */
    ctx->mode = OTELNET_MODE_TRANSFER;

    /* Start transfer logging */
    transfer_log_start(&ctx->config.transfer, &ctx->transfer);

    /* Execute transfer with multiple files */
    result = transfer_execute_modem_files(&ctx->config.transfer,
                                         &ctx->transfer,
                                         telnet_fd,
                                         protocol,
                                         filenames,
                                         file_count,
                                         &ctx->telnet);

    /* Determine error type based on result */
    if (result == SUCCESS) {
        error = TRANSFER_ERROR_NONE;
    } else if (transfer_is_cancel_requested()) {
        /* User cancelled the transfer */
        error = TRANSFER_ERROR_USER_CANCEL;
        printf("\r\n\nTransfer cancelled by user\r\n");
    } else if (result == ERROR_TIMEOUT) {
        error = TRANSFER_ERROR_TIMEOUT;
    } else if (result == ERROR_CONNECTION) {
        error = TRANSFER_ERROR_NETWORK;
    } else if (result == ERROR_IO) {
        error = TRANSFER_ERROR_PERMISSION;
    } else {
        error = TRANSFER_ERROR_UNKNOWN;
    }

    /* End transfer logging */
    transfer_log_end(&ctx->config.transfer, &ctx->transfer, error);

    /* Exit transfer mode */
    transfer_exit_mode(&ctx->transfer);

    /* Clear protocol detector buffers to prevent false triggers from leftover data */
    zmodem_detector_init(&ctx->zmodem_detector);
    xmodem_detector_init(&ctx->xmodem_detector);
    ymodem_detector_init(&ctx->ymodem_detector);

    /* Restore telnet protocol state to original values */
    telnet_restore_state(&ctx->telnet,
                        ctx->transfer.saved_binary_local,
                        ctx->transfer.saved_binary_remote,
                        ctx->transfer.saved_echo_local,
                        ctx->transfer.saved_echo_remote,
                        ctx->transfer.saved_sga_local,
                        ctx->transfer.saved_sga_remote,
                        ctx->transfer.saved_linemode_active);

    /* Flush socket buffer to prevent stale data from triggering auto-detection
     * After BINARY mode exit, socket may contain:
     * - IAC negotiation responses (WONT/DONT BINARY)
     * - Server text data (timestamps, prompts)
     * These must be discarded to prevent false ZMODEM/XMODEM/YMODEM triggers */
    telnet_fd = telnet_get_fd(&ctx->telnet);
    if (telnet_fd >= 0) {
        unsigned char flush_buf[BUFFER_SIZE];
        ssize_t flushed_total = 0;
        int flush_attempts = 0;
        const int MAX_FLUSH_ATTEMPTS = 10;

        /* Give server time to send any pending data */
        usleep(100000);  /* 100ms */

        while (flush_attempts < MAX_FLUSH_ATTEMPTS) {
            ssize_t n = recv(telnet_fd, flush_buf, sizeof(flush_buf), MSG_DONTWAIT);
            if (n > 0) {
                flushed_total += n;
                #ifdef DEBUG
    printf("[DEBUG] %s:%d: Flushed %zd bytes from socket after transfer (attempt %d)\r\n", __FILE__, __LINE__, n, flush_attempts + 1); fflush(stdout);
#endif
            } else if (n == 0) {
                /* Connection closed */
                break;
            } else {
                /* No more data or error */
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* No more data available */
                    break;
                }
                /* Other errors, stop flushing */
                break;
            }
            flush_attempts++;
            /* Small delay between attempts to allow data to arrive */
            if (flush_attempts < MAX_FLUSH_ATTEMPTS) {
                usleep(10000);  /* 10ms */
            }
        }

        if (flushed_total > 0) {
            printf("[%s][INFO] Flushed %zd bytes from socket buffer after transfer\r\n", otelnet_get_timestamp(), flushed_total); fflush(stdout);
        }
    }

    /* Print telnet mode after transfer (DEBUG only) */
    telnet_debug_print_mode(&ctx->telnet, "After file transfer");

    /* Clear cancellation flag */
    transfer_clear_cancel();

    /* Restore application mode */
    ctx->mode = OTELNET_MODE_CLIENT;

    return result;
}

/**
 * Auto-start ZMODEM receive (triggered by remote sending)
 */
static int otelnet_auto_start_zmodem_receive(otelnet_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    printf("\r\n\r\n");
    printf("*** ZMODEM Download Detected ***\r\n");
    printf("*** Starting automatic receive... ***\r\n");
    printf("\r\n");

    /* Automatically start ZMODEM receive */
    return otelnet_execute_transfer(ctx, TRANSFER_ZMODEM_RECV, NULL);
}

/**
 * Auto-start ZMODEM send (triggered by remote 'rz' command)
 */
static int otelnet_auto_start_zmodem_send(otelnet_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    printf("\r\n\r\n");
    printf("*** ZMODEM Upload Request Detected ***\r\n");

    /* Check if prompting is enabled */
    if (ctx->config.transfer.auto_zmodem_prompt) {
        printf("*** Enter filename to send (or press Enter to cancel): ");
        fflush(stdout);

        /* Temporarily restore terminal for input */
        struct termios saved_term;
        tcgetattr(STDIN_FILENO, &saved_term);

        struct termios cooked_term = saved_term;
        cooked_term.c_lflag |= (ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &cooked_term);

        /* Read filename */
        char filename[BUFFER_SIZE];
        if (fgets(filename, sizeof(filename), stdin) != NULL) {
            /* Remove trailing newline */
            size_t len = strlen(filename);
            if (len > 0 && filename[len - 1] == '\n') {
                filename[len - 1] = '\0';
            }

            /* Restore terminal */
            tcsetattr(STDIN_FILENO, TCSANOW, &saved_term);

            /* Check if user cancelled */
            if (strlen(filename) == 0) {
                printf("\r\n*** Upload cancelled ***\r\n\r\n");
                return SUCCESS;
            }

            /* Check if file exists */
            if (access(filename, F_OK) != 0) {
                printf("\r\n*** Error: File not found: %s ***\r\n\r\n", filename);
                return ERROR_IO;
            }

            if (access(filename, R_OK) != 0) {
                printf("\r\n*** Error: Cannot read file: %s ***\r\n\r\n", filename);
                return ERROR_IO;
            }

            printf("*** Sending: %s ***\r\n\r\n", filename);

            /* Start ZMODEM send */
            return otelnet_execute_transfer(ctx, TRANSFER_ZMODEM_SEND, filename);
        } else {
            /* Restore terminal */
            tcsetattr(STDIN_FILENO, TCSANOW, &saved_term);
            printf("\r\n*** Upload cancelled ***\r\n\r\n");
            return SUCCESS;
        }
    } else {
        printf("*** Auto-send disabled (no filename prompt) ***\r\n");
        printf("*** Use 'sz <filename>' command manually ***\r\n\r\n");
        return SUCCESS;
    }
}

/**
 * Auto-start XMODEM send (triggered by remote NAK or 'C' characters)
 */
static int otelnet_auto_start_xmodem_send(otelnet_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    printf("\r\n\r\n");
    printf("*** XMODEM Upload Request Detected ***\r\n");

    /* Check if prompting is enabled */
    if (ctx->config.transfer.auto_xmodem_prompt) {
        printf("*** Enter filename to send (or press Enter to cancel): ");
        fflush(stdout);

        /* Temporarily restore terminal for input */
        struct termios saved_term;
        tcgetattr(STDIN_FILENO, &saved_term);

        struct termios cooked_term = saved_term;
        cooked_term.c_lflag |= (ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &cooked_term);

        /* Read filename */
        char filename[BUFFER_SIZE];
        if (fgets(filename, sizeof(filename), stdin) != NULL) {
            /* Remove trailing newline */
            size_t len = strlen(filename);
            if (len > 0 && filename[len - 1] == '\n') {
                filename[len - 1] = '\0';
            }

            /* Restore terminal */
            tcsetattr(STDIN_FILENO, TCSANOW, &saved_term);

            /* Check if user cancelled */
            if (strlen(filename) == 0) {
                printf("\r\n*** Upload cancelled ***\r\n\r\n");
                return SUCCESS;
            }

            /* Check if file exists */
            if (access(filename, F_OK) != 0) {
                printf("\r\n*** Error: File not found: %s ***\r\n\r\n", filename);
                return ERROR_IO;
            }

            if (access(filename, R_OK) != 0) {
                printf("\r\n*** Error: Cannot read file: %s ***\r\n\r\n", filename);
                return ERROR_IO;
            }

            printf("*** Sending: %s ***\r\n\r\n", filename);

            /* Start XMODEM send */
            return otelnet_execute_transfer(ctx, TRANSFER_XMODEM_SEND, filename);
        } else {
            /* Restore terminal */
            tcsetattr(STDIN_FILENO, TCSANOW, &saved_term);
            printf("\r\n*** Upload cancelled ***\r\n\r\n");
            return SUCCESS;
        }
    } else {
        printf("*** Auto-send disabled (no filename prompt) ***\r\n");
        printf("*** Use 'sx <filename>' command manually ***\r\n\r\n");
        return SUCCESS;
    }
}

/**
 * Auto-start YMODEM send (triggered by remote 'C' characters)
 */
static int otelnet_auto_start_ymodem_send(otelnet_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    printf("\r\n\r\n");
    printf("*** YMODEM Upload Request Detected ***\r\n");

    /* Check if prompting is enabled */
    if (ctx->config.transfer.auto_ymodem_prompt) {
        printf("*** Enter filename(s) to send (space-separated, or press Enter to cancel): ");
        fflush(stdout);

        /* Temporarily restore terminal for input */
        struct termios saved_term;
        tcgetattr(STDIN_FILENO, &saved_term);

        struct termios cooked_term = saved_term;
        cooked_term.c_lflag |= (ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &cooked_term);

        /* Read filenames */
        char input[BUFFER_SIZE];
        if (fgets(input, sizeof(input), stdin) != NULL) {
            /* Remove trailing newline */
            size_t len = strlen(input);
            if (len > 0 && input[len - 1] == '\n') {
                input[len - 1] = '\0';
            }

            /* Restore terminal */
            tcsetattr(STDIN_FILENO, TCSANOW, &saved_term);

            /* Check if user cancelled */
            if (strlen(input) == 0) {
                printf("\r\n*** Upload cancelled ***\r\n\r\n");
                return SUCCESS;
            }

            /* Parse filenames */
            char *filenames[32];
            int file_count = 0;
            char *token = strtok(input, " \t");
            while (token != NULL && file_count < 32) {
                /* Check if file exists */
                if (access(token, F_OK) != 0) {
                    printf("\r\n*** Error: File not found: %s ***\r\n\r\n", token);
                    return ERROR_IO;
                }

                if (access(token, R_OK) != 0) {
                    printf("\r\n*** Error: Cannot read file: %s ***\r\n\r\n", token);
                    return ERROR_IO;
                }

                filenames[file_count++] = token;
                token = strtok(NULL, " \t");
            }

            if (file_count == 0) {
                printf("\r\n*** No files specified ***\r\n\r\n");
                return SUCCESS;
            }

            printf("*** Sending %d file(s) via YMODEM ***\r\n\r\n", file_count);

            /* Start YMODEM send */
            return otelnet_execute_transfer_multi(ctx, TRANSFER_YMODEM_SEND, filenames, file_count);
        } else {
            /* Restore terminal */
            tcsetattr(STDIN_FILENO, TCSANOW, &saved_term);
            printf("\r\n*** Upload cancelled ***\r\n\r\n");
            return SUCCESS;
        }
    } else {
        printf("*** Auto-send disabled (no filename prompt) ***\r\n");
        printf("*** Use 'sy <filename>' command manually ***\r\n\r\n");
        return SUCCESS;
    }
}

/**
 * Auto-start XMODEM receive (triggered by remote text message)
 */
static int otelnet_auto_start_xmodem_receive(otelnet_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    printf("\r\n\r\n");
    printf("*** XMODEM Download Detected ***\r\n");
    printf("*** Starting automatic receive... ***\r\n");
    printf("*** File will be saved as: xmodem.dat ***\r\n");
    printf("\r\n");

    /* Automatically start XMODEM receive (filename will default to xmodem.dat) */
    return otelnet_execute_transfer(ctx, TRANSFER_XMODEM_RECV, NULL);
}

/**
 * Auto-start YMODEM receive (triggered by remote text message)
 */
static int otelnet_auto_start_ymodem_receive(otelnet_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    printf("\r\n\r\n");
    printf("*** YMODEM Download Detected ***\r\n");
    printf("*** Starting automatic receive... ***\r\n");
    printf("\r\n");

    /* Automatically start YMODEM receive */
    return otelnet_execute_transfer(ctx, TRANSFER_YMODEM_RECV, NULL);
}

/**
 * Process console command
 */
int otelnet_process_console_command(otelnet_ctx_t *ctx, const char *command)
{
    char cmd_buffer[LINE_BUFFER_SIZE];
    char args_buffer[LINE_BUFFER_SIZE];  /* Buffer for argument parsing - must remain valid */
    char program[SMALL_BUFFER_SIZE];
    char *args[32];
    int arg_count = 0;
    char *argv[33];  /* program + args + NULL */

    if (ctx == NULL || command == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Trim whitespace */
    SAFE_STRNCPY(cmd_buffer, command, sizeof(cmd_buffer));
    char *cmd = otelnet_trim_whitespace(cmd_buffer);

    /* Empty command - return to client mode */
    if (strlen(cmd) == 0) {
        otelnet_exit_console_mode(ctx);
        return SUCCESS;
    }

    /* Parse command and arguments - args[] will point into args_buffer */
    otelnet_parse_command_args(cmd, args_buffer, sizeof(args_buffer),
                               program, sizeof(program), args, 31, &arg_count);

    /* quit/exit - exit program */
    if (strcmp(program, "quit") == 0 || strcmp(program, "exit") == 0) {
        ctx->running = false;
        return SUCCESS;
    }

    /* help */
    if (strcmp(program, "help") == 0 || strcmp(program, "?") == 0) {
        printf("\r\n");
        printf("=== Console Commands ===\r\n");
        printf("  [empty]       - Return to client mode\r\n");
        printf("  quit, exit    - Disconnect and exit program\r\n");
        printf("  help, ?       - Show this help message\r\n");
        printf("  stats         - Show connection statistics\r\n\r\n");
        printf("=== File Transfer Commands ===\r\n");
        printf("Send Files:\r\n");
        printf("  sz [options] <files...> - Send via ZMODEM (default)\r\n");
        printf("  sy <files...>           - Send via YMODEM\r\n");
        printf("  sx <file>               - Send via XMODEM (single file)\r\n");
        printf("  skermit <file>          - Send via Kermit protocol\r\n");
        printf("  kermit [args]           - Run kermit with custom arguments\r\n\r\n");
        printf("Receive Files:\r\n");
        printf("  rz [options]  - Receive via ZMODEM (default)\r\n");
        printf("  ry            - Receive via YMODEM\r\n");
        printf("  rx            - Receive via XMODEM (single file)\r\n");
        printf("  rkermit       - Receive via Kermit protocol\r\n\r\n");
        printf("Protocol Options (for sz/rz):\r\n");
        printf("  --xmodem, -x  - Use XMODEM protocol\r\n");
        printf("  --ymodem, -y  - Use YMODEM protocol\r\n");
        printf("  --zmodem, -z  - Use ZMODEM protocol\r\n\r\n");
        printf("=== File Management ===\r\n");
        printf("  ls [dir]      - List files in directory\r\n");
        printf("  pwd           - Print working directory\r\n");
        printf("  cd <dir>      - Change directory\r\n\r\n");
        printf("=== Examples ===\r\n");
        printf("  sz file.txt              - Send via ZMODEM\r\n");
        printf("  sz --ymodem f1.txt f2.txt - Send multiple via YMODEM\r\n");
        printf("  sy *.pdf                 - Send all PDFs via YMODEM\r\n");
        printf("  sx firmware.bin          - Send single file via XMODEM\r\n");
        printf("  skermit document.pdf     - Send via Kermit\r\n");
        printf("  rz                       - Receive via ZMODEM\r\n");
        printf("  ry                       - Receive via YMODEM\r\n");
        printf("  rx                       - Receive via XMODEM\r\n");
        printf("  ls /tmp                  - List /tmp directory\r\n\r\n");
        printf("=== Kermit Download Workflow (Manual) ===\r\n");
        printf("When server sends: \"Starting Kermit send of 'filename'...\"\r\n");
        printf("1. Press Ctrl+] to enter console mode\r\n");
        printf("2. Type: skermit filename\r\n");
        printf("3. Wait for transfer to complete\r\n");
        printf("Note: Kermit does NOT auto-start. You must manually run skermit.\r\n");
        printf("========================\r\n");
        return SUCCESS;
    }

    /* stats */
    if (strcmp(program, "stats") == 0) {
        otelnet_print_stats(ctx);
        return SUCCESS;
    }

    /* ls - list files */
    if (strcmp(program, "ls") == 0) {
        char ls_cmd[LINE_BUFFER_SIZE];
        int ret_code;
        if (arg_count > 0) {
            snprintf(ls_cmd, sizeof(ls_cmd), "ls -lh %s", args[0]);
        } else {
            snprintf(ls_cmd, sizeof(ls_cmd), "ls -lh");
        }
        printf("\r\n");
        ret_code = system(ls_cmd);
        if (ret_code != 0) {
            printf("Warning: ls command returned %d\r\n", ret_code);
        }
        return SUCCESS;
    }

    /* pwd - print working directory */
    if (strcmp(program, "pwd") == 0) {
        char cwd[BUFFER_SIZE];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("\r\nCurrent directory: %s\r\n", cwd);
        } else {
            printf("\r\nError: Failed to get current directory\r\n");
        }
        return SUCCESS;
    }

    /* cd - change directory */
    if (strcmp(program, "cd") == 0) {
        if (arg_count == 0) {
            printf("\r\nUsage: cd <directory>\r\n");
            return SUCCESS;
        }
        if (chdir(args[0]) == 0) {
            char cwd[BUFFER_SIZE];
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                printf("\r\nChanged to: %s\r\n", cwd);
            }
        } else {
            printf("\r\nError: Failed to change directory: %s\r\n", strerror(errno));
        }
        return SUCCESS;
    }

    /* kermit - run kermit with optional arguments */
    if (strcmp(program, "kermit") == 0) {
        if (arg_count == 0) {
            /* No arguments - show help */
            printf("\r\nKermit Usage:\r\n");
            printf("  kermit            - Interactive kermit\r\n");
            printf("  kermit -s <file>  - Send file\r\n");
            printf("  kermit -r         - Receive file\r\n");
            printf("\r\nNote: Telnet session will be redirected to kermit\r\n");
            printf("Run 'kermit' without args to start, or specify send/receive\r\n");
            return SUCCESS;
        }

        /* Build argv array */
        argv[0] = (char *)ctx->config.transfer.kermit_path;
        for (int i = 0; i < arg_count && i < 31; i++) {
            argv[i + 1] = args[i];
        }
        argv[arg_count + 1] = NULL;

        return otelnet_execute_external_program_with_args(ctx, ctx->config.transfer.kermit_path, argv);
    }

    /* skermit - send via Kermit protocol */
    if (strcmp(program, "skermit") == 0) {
        if (arg_count == 0) {
            /* No filename provided - show help */
            printf("\r\nKermit Send Usage:\r\n");
            printf("  skermit <filename>  - Send file via Kermit protocol\r\n");
            printf("\r\nExample:\r\n");
            printf("  skermit document.pdf\r\n");
            printf("\r\nNote: Binary mode (-i) will be used automatically\r\n");
            return SUCCESS;
        }

        if (arg_count > 1) {
            printf("\r\nError: skermit accepts only one file at a time\r\n");
            printf("Usage: skermit <filename>\r\n");
            return ERROR_INVALID_ARG;
        }

        /* Convert relative path to absolute path */
        char *abs_path = realpath(args[0], NULL);
        if (abs_path == NULL) {
            /* realpath failed - file doesn't exist or no permission */
            printf("\r\nError: Cannot access file '%s': %s\r\n", args[0], strerror(errno));
            return ERROR_IO;
        }

        printf("\r\n[Kermit Send Mode]\r\n");
        printf("[Sending: %s]\r\n", abs_path);

        /* Execute transfer using transfer module */
        int result = otelnet_execute_transfer(ctx, TRANSFER_KERMIT_SEND, abs_path);

        /* Free allocated path */
        free(abs_path);

        return result;
    }

    /* rkermit - receive via Kermit protocol */
    if (strcmp(program, "rkermit") == 0) {
        printf("[%s][INFO] >>> rkermit command processing started\r\n", otelnet_get_timestamp()); fflush(stdout);

        if (arg_count > 0) {
            printf("\r\nNote: rkermit does not accept arguments\r\n");
            printf("Files will be saved to current directory\r\n");
        }

        printf("\r\n[%s] [Kermit Receive Mode]\r\n", otelnet_get_timestamp());
        printf("[%s] [Ready to receive file(s)]\r\n", otelnet_get_timestamp());
        char cwd[BUFFER_SIZE];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("[Save to: %s]\r\n", cwd);
            printf("[%s][INFO] Save directory: %s\r\n", otelnet_get_timestamp(), cwd); fflush(stdout);
        }

        printf("[%s][INFO] Calling otelnet_execute_transfer(TRANSFER_KERMIT_RECV)\r\n", otelnet_get_timestamp()); fflush(stdout);
        /* Execute transfer using transfer module */
        int result = otelnet_execute_transfer(ctx, TRANSFER_KERMIT_RECV, NULL);
        printf("[%s][INFO] <<< rkermit command completed with result: %d\r\n", otelnet_get_timestamp(), result); fflush(stdout);
        return result;
    }

    /* sz/sx/sy - send with protocol options */
    if (strcmp(program, "sz") == 0 || strcmp(program, "sx") == 0 || strcmp(program, "sy") == 0) {
        transfer_protocol_t protocol_type;
        const char *protocol_name = NULL;
        int file_start_idx = 0;

        /* Determine protocol from command or first argument */
        if (strcmp(program, "sx") == 0) {
            protocol_type = TRANSFER_XMODEM_SEND;
            protocol_name = "XMODEM";
        } else if (strcmp(program, "sy") == 0) {
            protocol_type = TRANSFER_YMODEM_SEND;
            protocol_name = "YMODEM";
        } else if (arg_count > 0 &&
                   (strcmp(args[0], "--xmodem") == 0 || strcmp(args[0], "-x") == 0)) {
            protocol_type = TRANSFER_XMODEM_SEND;
            protocol_name = "XMODEM";
            file_start_idx = 1;
        } else if (arg_count > 0 &&
                   (strcmp(args[0], "--ymodem") == 0 || strcmp(args[0], "-y") == 0)) {
            protocol_type = TRANSFER_YMODEM_SEND;
            protocol_name = "YMODEM";
            file_start_idx = 1;
        } else if (arg_count > 0 &&
                   (strcmp(args[0], "--zmodem") == 0 || strcmp(args[0], "-z") == 0)) {
            protocol_type = TRANSFER_ZMODEM_SEND;
            protocol_name = "ZMODEM";
            file_start_idx = 1;
        } else {
            /* Default to ZMODEM */
            protocol_type = TRANSFER_ZMODEM_SEND;
            protocol_name = "ZMODEM";
        }

        /* Check if we have files to send */
        int file_count = arg_count - file_start_idx;
        if (file_count == 0) {
            printf("\r\n");
            printf("=== Send File Usage ===\r\n");
            printf("Commands:\r\n");
            printf("  sz [options] <file1> [file2] ...  - Send files\r\n");
            printf("  sx <file1> [file2] ...            - Send via XMODEM\r\n");
            printf("  sy <file1> [file2] ...            - Send via YMODEM\r\n\r\n");
            printf("Protocol Options:\r\n");
            printf("  --xmodem, -x  - Use XMODEM protocol (single file only)\r\n");
            printf("  --ymodem, -y  - Use YMODEM protocol (batch transfer)\r\n");
            printf("  --zmodem, -z  - Use ZMODEM protocol (default, automatic)\r\n\r\n");
            printf("Examples:\r\n");
            printf("  sz document.pdf              - Send via ZMODEM (default)\r\n");
            printf("  sz --ymodem file1.txt file2.txt - Send multiple files via YMODEM\r\n");
            printf("  sx firmware.bin              - Send single file via XMODEM\r\n");
            printf("  sy *.txt                     - Send all .txt files via YMODEM\r\n\r\n");
            printf("Note: XMODEM supports only single file transfers\r\n");
            printf("Tip: Use 'ls' to see available files\r\n");
            printf("======================\r\n");
            return SUCCESS;
        }

        printf("\r\n[Protocol: %s]\r\n", protocol_name);
        printf("[Sending %d file(s)]\r\n", file_count);

        /* Convert relative paths to absolute paths */
        char *abs_paths[32];  /* Max 32 files */
        int abs_count = 0;
        for (int i = 0; i < file_count && i < 32; i++) {
            const char *rel_path = args[file_start_idx + i];
            char *abs_path = realpath(rel_path, NULL);

            if (abs_path == NULL) {
                /* realpath failed - file doesn't exist or no permission */
                printf("\r\nError: Cannot access file '%s': %s\r\n", rel_path, strerror(errno));

                /* Free previously allocated paths */
                for (int j = 0; j < abs_count; j++) {
                    free(abs_paths[j]);
                }
                return ERROR_IO;
            }

            abs_paths[abs_count++] = abs_path;
            #ifdef DEBUG
    printf("[DEBUG] %s:%d: Converted '%s' to absolute path '%s'\r\n", __FILE__, __LINE__, rel_path, abs_path); fflush(stdout);
#endif
        }

        /* Send notification to server for XMODEM/YMODEM uploads to trigger auto-receive */
        if (protocol_type == TRANSFER_XMODEM_SEND || protocol_type == TRANSFER_YMODEM_SEND) {
            char notify_msg[512];

            /* Send same format message as server does, so server can detect with same pattern */
            /* For multiple files, use basename of first file (not full path) */
            const char *first_file = strrchr(abs_paths[0], '/');
            first_file = first_file ? first_file + 1 : abs_paths[0];

            snprintf(notify_msg, sizeof(notify_msg),
                    "\r\nStarting %s send of '%s'...\r\n", protocol_name, first_file);

            telnet_send(&ctx->telnet, notify_msg, strlen(notify_msg));
            printf("[%s][INFO] Sent %s upload notification to server\r\n", otelnet_get_timestamp(), protocol_name); fflush(stdout);

            /* Request BINARY mode for transfer */
            printf("\r\n*** Notifying server to start %s receive... ***\r\n", protocol_name);
            printf("*** Negotiating BINARY mode... ***\r\n");

            telnet_request_binary_mode(&ctx->telnet);
            printf("[%s][INFO] Requested BINARY mode for %s transfer\r\n", otelnet_get_timestamp(), protocol_name); fflush(stdout);

            /* Wait for BINARY mode negotiation to complete */
            int timeout_count = 50;  /* 5 seconds total (100ms * 50) */
            bool binary_ready = false;

            while (timeout_count > 0) {
                /* Check if both directions are in BINARY mode */
                if (ctx->telnet.binary_local && ctx->telnet.binary_remote) {
                    binary_ready = true;
                    printf("[%s][INFO] BINARY mode negotiation complete (bidirectional)\r\n", otelnet_get_timestamp()); fflush(stdout);
                    break;
                }

                /* Process incoming telnet data (negotiation responses) */
                fd_set readfds;
                struct timeval tv;
                FD_ZERO(&readfds);
                FD_SET(ctx->telnet.fd, &readfds);
                tv.tv_sec = 0;
                tv.tv_usec = 100000;  /* 100ms timeout */

                int ret = select(ctx->telnet.fd + 1, &readfds, NULL, NULL, &tv);
                if (ret > 0 && FD_ISSET(ctx->telnet.fd, &readfds)) {
                    /* Process telnet negotiation */
                    otelnet_process_telnet(ctx);
                }

                timeout_count--;
            }

            if (!binary_ready) {
                fprintf(stderr, "[%s][WARNING] BINARY mode negotiation timeout - continuing anyway\r\n", otelnet_get_timestamp());
                printf("*** Warning: BINARY mode negotiation incomplete ***\r\n");
            } else {
                printf("*** BINARY mode ready ***\r\n");
            }

            printf("\r\n");
        }

        /* Execute transfer using transfer module with absolute paths */
        int result = otelnet_execute_transfer_multi(ctx, protocol_type, abs_paths, abs_count);

        /* Free allocated absolute paths */
        for (int i = 0; i < abs_count; i++) {
            free(abs_paths[i]);
        }

        return result;
    }

    /* rz/rx/ry - receive with protocol options */
    if (strcmp(program, "rz") == 0 || strcmp(program, "rx") == 0 || strcmp(program, "ry") == 0) {
        transfer_protocol_t protocol_type;
        const char *protocol_name = NULL;

        /* Determine protocol from command or first argument */
        if (strcmp(program, "rx") == 0) {
            protocol_type = TRANSFER_XMODEM_RECV;
            protocol_name = "XMODEM";
        } else if (strcmp(program, "ry") == 0) {
            protocol_type = TRANSFER_YMODEM_RECV;
            protocol_name = "YMODEM";
        } else if (arg_count > 0 &&
                   (strcmp(args[0], "--xmodem") == 0 || strcmp(args[0], "-x") == 0)) {
            protocol_type = TRANSFER_XMODEM_RECV;
            protocol_name = "XMODEM";
        } else if (arg_count > 0 &&
                   (strcmp(args[0], "--ymodem") == 0 || strcmp(args[0], "-y") == 0)) {
            protocol_type = TRANSFER_YMODEM_RECV;
            protocol_name = "YMODEM";
        } else if (arg_count > 0 &&
                   (strcmp(args[0], "--zmodem") == 0 || strcmp(args[0], "-z") == 0)) {
            protocol_type = TRANSFER_ZMODEM_RECV;
            protocol_name = "ZMODEM";
        } else if (arg_count > 0 &&
                   strcmp(args[0], "--help") != 0 && strcmp(args[0], "-h") != 0) {
            /* Unknown argument */
            printf("\r\n");
            printf("=== Receive File Usage ===\r\n");
            printf("Commands:\r\n");
            printf("  rz [options]  - Receive files\r\n");
            printf("  rx            - Receive via XMODEM\r\n");
            printf("  ry            - Receive via YMODEM\r\n\r\n");
            printf("Protocol Options:\r\n");
            printf("  --xmodem, -x  - Use XMODEM protocol (single file)\r\n");
            printf("  --ymodem, -y  - Use YMODEM protocol (batch transfer)\r\n");
            printf("  --zmodem, -z  - Use ZMODEM protocol (default, automatic)\r\n\r\n");
            printf("Examples:\r\n");
            printf("  rz            - Receive via ZMODEM (default)\r\n");
            printf("  rz --ymodem   - Receive via YMODEM\r\n");
            printf("  rx            - Receive single file via XMODEM\r\n");
            printf("  ry            - Receive files via YMODEM\r\n\r\n");
            printf("Files will be saved to: ");
            char cwd[BUFFER_SIZE];
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                printf("%s\r\n", cwd);
            } else {
                printf("current directory\r\n");
            }
            printf("Use 'pwd' to check or 'cd' to change directory\r\n");
            printf("==========================\r\n");
            return SUCCESS;
        } else {
            /* Default to ZMODEM */
            protocol_type = TRANSFER_ZMODEM_RECV;
            protocol_name = "ZMODEM";
        }

        printf("\r\n[Protocol: %s]\r\n", protocol_name);
        printf("[Ready to receive file(s)]\r\n");
        char cwd[BUFFER_SIZE];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("[Save to: %s]\r\n", cwd);
        }

        /* Execute transfer using transfer module */
        return otelnet_execute_transfer(ctx, protocol_type, NULL);
    }

    /* Unknown command */
    printf("\r\nUnknown command: %s\r\n", program);
    printf("Type 'help' for available commands\r\n");

    return SUCCESS;
}

/**
 * Process data from stdin
 */
int otelnet_process_stdin(otelnet_ctx_t *ctx)
{
    unsigned char buf[BUFFER_SIZE];
    unsigned char telnet_buf[BUFFER_SIZE * 2];
    size_t telnet_len;
    ssize_t n;

    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Do not process stdin during file transfer */
    if (ctx->mode == OTELNET_MODE_TRANSFER) {
        return SUCCESS;
    }

    n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return SUCCESS;
        }
        fprintf(stderr, "[%s][ERROR] %s:%d: Failed to read from stdin: %s\r\n", otelnet_get_timestamp(), __FILE__, __LINE__, strerror(errno));
        return ERROR_IO;
    }

    if (n == 0) {
        /* EOF on stdin (Ctrl+D in non-console mode) */
        if (ctx->mode == OTELNET_MODE_CLIENT) {
            ctx->running = false;
        }
        return SUCCESS;
    }

    /* Check for Ctrl+] in client mode */
    if (ctx->mode == OTELNET_MODE_CLIENT) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == CONSOLE_TRIGGER_KEY) {
                /* Found Ctrl+] - enter console mode */
                otelnet_enter_console_mode(ctx);
                return SUCCESS;
            }
        }

        /* Normal data - send to telnet server */
        if (telnet_is_connected(&ctx->telnet)) {
            /* Local echo if server doesn't echo */
            bool need_local_echo = !ctx->telnet.echo_remote;
            bool is_linemode = telnet_is_linemode(&ctx->telnet);

            /* Update line buffer in line mode for redisplay support */
            if (is_linemode) {
                for (ssize_t i = 0; i < n; i++) {
                    unsigned char c = buf[i];
                    if (c == '\r' || c == '\n') {
                        /* End of line - clear buffer */
                        ctx->line_buffer_len = 0;
                    } else if (c == 0x7F || c == 0x08) {
                        /* Backspace/Delete - remove last character */
                        if (ctx->line_buffer_len > 0) {
                            ctx->line_buffer_len--;
                        }
                    } else if (c >= 0x20 && ctx->line_buffer_len < sizeof(ctx->line_buffer) - 1) {
                        /* Add character to buffer */
                        ctx->line_buffer[ctx->line_buffer_len++] = c;
                    }
                }
            }

            if (need_local_echo) {
                /* Echo input locally - support multibyte characters */
                for (ssize_t i = 0; i < n; i++) {
                    unsigned char c = buf[i];
                    ssize_t written;
                    if (c == '\r') {
                        /* CR - echo as CR+LF */
                        written = write(STDOUT_FILENO, "\r\n", 2);
                        (void)written; /* Ignore write errors for echo */
                    } else if (c == 0x7F || c == 0x08) {
                        /* Backspace/Delete - echo backspace sequence */
                        written = write(STDOUT_FILENO, "\b \b", 3);
                        (void)written; /* Ignore write errors for echo */
                    } else if (c >= 0x20) {
                        /* Printable ASCII character or multibyte sequence byte (0x80-0xFF) */
                        written = write(STDOUT_FILENO, &c, 1);
                        (void)written; /* Ignore write errors for echo */
                    }
                    /* Only control characters (< 0x20) not echoed */
                }
            }

            /* Convert CR to CR+LF for telnet protocol (RFC 854) */
            unsigned char processed_buf[BUFFER_SIZE * 2];
            size_t processed_len = 0;

            for (ssize_t i = 0; i < n && processed_len < sizeof(processed_buf) - 1; i++) {
                if (buf[i] == '\r') {
                    /* CR -> CR+LF */
                    processed_buf[processed_len++] = '\r';
                    if (processed_len < sizeof(processed_buf)) {
                        processed_buf[processed_len++] = '\n';
                    }
                } else {
                    processed_buf[processed_len++] = buf[i];
                }
            }

            /* Prepare data (escape IAC) */
            telnet_prepare_output(&ctx->telnet, processed_buf, processed_len, telnet_buf, sizeof(telnet_buf), &telnet_len);

            if (telnet_len > 0) {
                ssize_t sent = telnet_send(&ctx->telnet, telnet_buf, telnet_len);
                if (sent > 0) {
                    ctx->bytes_sent += sent;
                    /* Log sent data */
                    otelnet_log_data(ctx, "SEND", buf, n);
                }
            }
        }
    } else if (ctx->mode == OTELNET_MODE_CONSOLE) {
        /* Console mode - accumulate input */
        for (ssize_t i = 0; i < n; i++) {
            unsigned char c = buf[i];

            if (c == '\n' || c == '\r') {
                /* End of line - process command */
                ctx->console_buffer[ctx->console_buffer_len] = '\0';
                printf("\r\n");

                /* Check for Ctrl+D (EOF) */
                if (ctx->console_buffer_len == 0 && n == 1 && buf[0] == 0x04) {
                    ctx->running = false;
                    return SUCCESS;
                }

                otelnet_process_console_command(ctx, ctx->console_buffer);

                /* Reset buffer */
                ctx->console_buffer_len = 0;

                /* Show prompt if still in console mode */
                if (ctx->mode == OTELNET_MODE_CONSOLE) {
                    printf("otelnet> ");
                    fflush(stdout);
                }
            } else if (c == 0x7F || c == 0x08) {
                /* Backspace */
                if (ctx->console_buffer_len > 0) {
                    ctx->console_buffer_len--;
                    printf("\b \b");
                    fflush(stdout);
                }
            } else if (c == 0x04) {
                /* Ctrl+D - quit */
                ctx->running = false;
                return SUCCESS;
            } else if (c >= 0x20 && c < 0x7F) {
                /* Printable character */
                if (ctx->console_buffer_len < sizeof(ctx->console_buffer) - 1) {
                    ctx->console_buffer[ctx->console_buffer_len++] = c;
                    printf("%c", c);
                    fflush(stdout);
                }
            }
        }
    }

    return SUCCESS;
}

/**
 * Process data from telnet server
 */
int otelnet_process_telnet(otelnet_ctx_t *ctx)
{
    unsigned char recv_buf[BUFFER_SIZE];
    unsigned char output_buf[BUFFER_SIZE];
    size_t output_len;
    ssize_t n;

    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!telnet_is_connected(&ctx->telnet)) {
        return ERROR_CONNECTION;
    }

    n = telnet_recv(&ctx->telnet, recv_buf, sizeof(recv_buf));
    if (n < 0) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Telnet connection error\r\n", otelnet_get_timestamp(), __FILE__, __LINE__);
        return ERROR_CONNECTION;
    }

    if (n == 0) {
        /* Connection closed or no data */
        if (!telnet_is_connected(&ctx->telnet)) {
            printf("[%s][INFO] Telnet connection closed by server\r\n", otelnet_get_timestamp()); fflush(stdout);
            ctx->running = false;
            return ERROR_CONNECTION;
        }
        return SUCCESS;
    }

    /* Process telnet protocol (remove IAC sequences) */
    telnet_process_input(&ctx->telnet, recv_buf, n, output_buf, sizeof(output_buf), &output_len);

    if (output_len > 0) {
        ctx->bytes_received += output_len;

        /* Log received data */
        otelnet_log_data(ctx, "RECEIVE", output_buf, output_len);

        /* During file transfer, do not output to stdout - transfer process handles it */
        if (ctx->mode == OTELNET_MODE_TRANSFER) {
            return SUCCESS;
        }

        /* ZMODEM auto-detection (if enabled and not already in transfer/console mode) */
        if (ctx->config.transfer.auto_zmodem_enabled &&
            ctx->mode == OTELNET_MODE_CLIENT &&
            !ctx->transfer.active) {

            bool is_receive_init = false;
            bool is_send_init = false;

            if (zmodem_detect_trigger(&ctx->zmodem_detector, output_buf, output_len,
                                     &is_receive_init, &is_send_init)) {
                if (is_receive_init) {
                    /* Remote is sending, we should receive */
                    printf("[%s][INFO] ZMODEM receive trigger detected\r\n", otelnet_get_timestamp()); fflush(stdout);
                    otelnet_auto_start_zmodem_receive(ctx);
                    return SUCCESS;  /* Return to allow transfer to proceed */
                } else if (is_send_init) {
                    /* Remote wants to receive, we should send */
                    printf("[%s][INFO] ZMODEM send trigger detected\r\n", otelnet_get_timestamp()); fflush(stdout);
                    otelnet_auto_start_zmodem_send(ctx);
                    return SUCCESS;  /* Return to allow transfer to proceed */
                }
            }
        }

        /* XMODEM auto-detection (if enabled and not already in transfer/console mode) */
        if (ctx->config.transfer.auto_xmodem_enabled &&
            ctx->mode == OTELNET_MODE_CLIENT &&
            !ctx->transfer.active) {

            bool is_receive_init = false;
            bool is_send_init = false;

            if (xmodem_detect_trigger(&ctx->xmodem_detector, output_buf, output_len,
                                     &is_receive_init, &is_send_init)) {
                if (is_receive_init) {
                    /* Remote is sending, we should receive */
                    printf("[%s][INFO] XMODEM receive trigger detected\r\n", otelnet_get_timestamp()); fflush(stdout);
                    otelnet_auto_start_xmodem_receive(ctx);
                    return SUCCESS;  /* Return to allow transfer to proceed */
                } else if (is_send_init) {
                    /* Remote wants to receive, we should send */
                    printf("[%s][INFO] XMODEM send trigger detected\r\n", otelnet_get_timestamp()); fflush(stdout);
                    otelnet_auto_start_xmodem_send(ctx);
                    return SUCCESS;  /* Return to allow transfer to proceed */
                }
            }
        }

        /* YMODEM auto-detection (if enabled and not already in transfer/console mode) */
        if (ctx->config.transfer.auto_ymodem_enabled &&
            ctx->mode == OTELNET_MODE_CLIENT &&
            !ctx->transfer.active) {

            bool is_receive_init = false;
            bool is_send_init = false;

            if (ymodem_detect_trigger(&ctx->ymodem_detector, output_buf, output_len,
                                     &is_receive_init, &is_send_init)) {
                if (is_receive_init) {
                    /* Remote is sending, we should receive */
                    printf("[%s][INFO] YMODEM receive trigger detected\r\n", otelnet_get_timestamp()); fflush(stdout);
                    otelnet_auto_start_ymodem_receive(ctx);
                    return SUCCESS;  /* Return to allow transfer to proceed */
                } else if (is_send_init) {
                    /* Remote wants to receive, we should send */
                    printf("[%s][INFO] YMODEM send trigger detected\r\n", otelnet_get_timestamp()); fflush(stdout);
                    otelnet_auto_start_ymodem_send(ctx);
                    return SUCCESS;  /* Return to allow transfer to proceed */
                }
            }
        }

        /* In line mode, check if we need to preserve current input line */
        bool is_linemode = telnet_is_linemode(&ctx->telnet);

        /* Check if server output ends with a prompt (ends with "> " or ">> ")
         * If it does, don't redisplay input as server will handle it */
        bool ends_with_prompt = false;
        if (is_linemode && output_len >= 2) {
            if (output_buf[output_len - 1] == ' ' && output_buf[output_len - 2] == '>') {
                ends_with_prompt = true;
            }
        }

        bool need_redisplay = is_linemode && ctx->line_buffer_len > 0 && !ends_with_prompt;

        if (need_redisplay) {
            /* Clear current input line by backspacing */
            for (size_t i = 0; i < ctx->line_buffer_len; i++) {
                ssize_t ret = write(STDOUT_FILENO, "\b \b", 3);
                (void)ret; /* Ignore write errors for backspace */
            }
        }

        /* Write server output to stdout with LF -> CRLF translation for line mode */
        if (is_linemode) {
            /* Line mode: translate LF to CRLF for proper display */
            unsigned char translated_buf[BUFFER_SIZE * 2];
            size_t translated_len = 0;

            for (size_t i = 0; i < output_len && translated_len < sizeof(translated_buf) - 1; i++) {
                if (output_buf[i] == '\n') {
                    /* LF -> CRLF */
                    translated_buf[translated_len++] = '\r';
                    translated_buf[translated_len++] = '\n';
                } else if (output_buf[i] == '\r') {
                    /* Standalone CR: check if next byte is not LF */
                    if (i + 1 < output_len && output_buf[i + 1] == '\n') {
                        /* CR LF sequence - keep as is */
                        translated_buf[translated_len++] = '\r';
                    } else {
                        /* Standalone CR - convert to CRLF for proper line break */
                        translated_buf[translated_len++] = '\r';
                        translated_buf[translated_len++] = '\n';
                    }
                } else {
                    translated_buf[translated_len++] = output_buf[i];
                }
            }

            ssize_t written = write(STDOUT_FILENO, translated_buf, translated_len);
            if (written < 0) {
                fprintf(stderr, "[%s][ERROR] %s:%d: Failed to write to stdout: %s\r\n", otelnet_get_timestamp(), __FILE__, __LINE__, strerror(errno));
                return ERROR_IO;
            }
        } else {
            /* Character mode: output as-is (server handles CRLF) */
            ssize_t written = write(STDOUT_FILENO, output_buf, output_len);
            if (written < 0) {
                fprintf(stderr, "[%s][ERROR] %s:%d: Failed to write to stdout: %s\r\n", otelnet_get_timestamp(), __FILE__, __LINE__, strerror(errno));
                return ERROR_IO;
            }
        }

        /* Redisplay user's input line if it was cleared and not ending with prompt */
        if (need_redisplay) {
            ssize_t written = write(STDOUT_FILENO, ctx->line_buffer, ctx->line_buffer_len);
            (void)written; /* Ignore errors for redisplay */
        }

        /* If server sent a prompt, clear our line buffer as user will start new input */
        if (is_linemode && ends_with_prompt) {
            ctx->line_buffer_len = 0;
        }
    }

    return SUCCESS;
}

/**
 * Main event loop
 */
int otelnet_run(otelnet_ctx_t *ctx)
{
    fd_set readfds;
    struct timeval timeout;
    int maxfd;
    int ret;

    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    while (ctx->running && g_running_local) {
        /* Check for window size changes */
        if (g_winsize_changed) {
            otelnet_update_window_size(ctx);
        }

        FD_ZERO(&readfds);
        maxfd = 0;

        /* Add stdin */
        FD_SET(STDIN_FILENO, &readfds);
        maxfd = MAX(maxfd, STDIN_FILENO);

        /* Add telnet socket if connected */
        if (telnet_is_connected(&ctx->telnet)) {
            int telnet_fd = telnet_get_fd(&ctx->telnet);
            if (telnet_fd >= 0) {
                FD_SET(telnet_fd, &readfds);
                maxfd = MAX(maxfd, telnet_fd);
            }
        }

        /* Set timeout */
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        /* Wait for activity */
        ret = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "[%s][ERROR] %s:%d: select() error: %s\r\n", otelnet_get_timestamp(), __FILE__, __LINE__, strerror(errno));
            return ERROR_IO;
        }

        if (ret == 0) {
            /* Timeout */
            continue;
        }

        /* Check stdin */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (otelnet_process_stdin(ctx) != SUCCESS) {
                fprintf(stderr, "[%s][ERROR] %s:%d: Error processing stdin\r\n", otelnet_get_timestamp(), __FILE__, __LINE__);
            }
        }

        /* Check telnet socket */
        if (telnet_is_connected(&ctx->telnet)) {
            int telnet_fd = telnet_get_fd(&ctx->telnet);
            if (telnet_fd >= 0 && FD_ISSET(telnet_fd, &readfds)) {
                if (otelnet_process_telnet(ctx) != SUCCESS) {
                    fprintf(stderr, "[%s][ERROR] %s:%d: Error processing telnet data\r\n", otelnet_get_timestamp(), __FILE__, __LINE__);
                    ctx->running = false;
                }
            }
        }
    }

    return SUCCESS;
}

/**
 * Print statistics
 */
void otelnet_print_stats(otelnet_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    printf("\r\n=== Connection Statistics ===\r\n");
    printf("Bytes sent:     %llu\r\n", (unsigned long long)ctx->bytes_sent);
    printf("Bytes received: %llu\r\n", (unsigned long long)ctx->bytes_received);

    if (ctx->connection_start_time > 0) {
        time_t duration = time(NULL) - ctx->connection_start_time;
        printf("Duration:       %ld seconds\r\n", (long)duration);
    }

    printf("============================\r\n");
}

/**
 * Print usage information
 */
void otelnet_print_usage(const char *program_name)
{
    printf("Usage: %s <host> <port> [options]\n", program_name);
    printf("\n");
    printf("Arguments:\n");
    printf("  host              Remote host (IP address or hostname)\n");
    printf("  port              Remote port number\n");
    printf("\n");
    printf("Options:\n");
    printf("  -c <config>       Configuration file (default: %s)\n", OTELNET_DEFAULT_CONFIG);
    printf("  -h, --help        Show this help message\n");
    printf("  -v, --version     Show version information\n");
    printf("\n");
    printf("Console Mode:\n");
    printf("  Press Ctrl+] to enter console mode\n");
    printf("  Commands: quit, skermit, rkermit, sz, rz, help, stats\n");
    printf("  Press Enter with empty line to return to client mode\n");
    printf("\n");
}

/**
 * Main function
 */
int main(int argc, char *argv[])
{
    otelnet_ctx_t ctx;
    char *host = NULL;
    int port = 0;
    char *config_file = OTELNET_DEFAULT_CONFIG;
    int ret;

    /* Open syslog */
    openlog(OTELNET_APP_NAME, LOG_PID | LOG_CONS, LOG_USER);

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            otelnet_print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("%s version %s\n", OTELNET_APP_NAME, OTELNET_VERSION);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 < argc) {
                config_file = argv[++i];
            } else {
                fprintf(stderr, "Error: -c requires an argument\n");
                otelnet_print_usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (host == NULL) {
            host = argv[i];
        } else if (port == 0) {
            port = atoi(argv[i]);
        } else {
            fprintf(stderr, "Error: Unknown argument: %s\n", argv[i]);
            otelnet_print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* Validate arguments */
    if (host == NULL || port == 0) {
        fprintf(stderr, "Error: Missing required arguments\n\n");
        otelnet_print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (port < 1 || port > 65535) {
        fprintf(stderr, "Error: Invalid port number: %d\n", port);
        return EXIT_FAILURE;
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGWINCH, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize context */
    otelnet_init(&ctx);

    /* Load configuration */
    ret = otelnet_load_config(&ctx, config_file);
    if (ret != SUCCESS) {
        fprintf(stderr, "Warning: Failed to load configuration file\n");
    }

    /* Open log file if enabled */
    otelnet_open_log(&ctx);

    /* Setup terminal */
    ret = otelnet_setup_terminal(&ctx);
    if (ret != SUCCESS) {
        fprintf(stderr, "Error: Failed to setup terminal\n");
        return EXIT_FAILURE;
    }

    /* Connect to telnet server */
    ret = otelnet_connect(&ctx, host, port);
    if (ret != SUCCESS) {
        otelnet_restore_terminal(&ctx);
        return EXIT_FAILURE;
    }

    /* Run main loop */
    ret = otelnet_run(&ctx);

    /* Cleanup */
    otelnet_disconnect(&ctx);
    otelnet_restore_terminal(&ctx);
    otelnet_print_stats(&ctx);

    /* Close log file */
    otelnet_close_log(&ctx);

    /* Close syslog */
    closelog();

    return (ret == SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;
}
