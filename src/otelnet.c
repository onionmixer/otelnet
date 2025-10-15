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
    SAFE_STRNCPY(ctx->config.kermit_path, "kermit", sizeof(ctx->config.kermit_path));
    SAFE_STRNCPY(ctx->config.send_zmodem_path, "sz", sizeof(ctx->config.send_zmodem_path));
    SAFE_STRNCPY(ctx->config.receive_zmodem_path, "rz", sizeof(ctx->config.receive_zmodem_path));
    ctx->config.log_enabled = false;
    SAFE_STRNCPY(ctx->config.log_file, "otelnet.log", sizeof(ctx->config.log_file));

    fp = fopen(config_file, "r");
    if (fp == NULL) {
        MB_LOG_WARNING("Could not open config file %s, using defaults", config_file);
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
                SAFE_STRNCPY(ctx->config.kermit_path, v, sizeof(ctx->config.kermit_path));
            } else if (strcmp(k, "SEND_ZMODEM") == 0) {
                SAFE_STRNCPY(ctx->config.send_zmodem_path, v, sizeof(ctx->config.send_zmodem_path));
            } else if (strcmp(k, "RECEIVE_ZMODEM") == 0) {
                SAFE_STRNCPY(ctx->config.receive_zmodem_path, v, sizeof(ctx->config.receive_zmodem_path));
            } else if (strcmp(k, "LOG") == 0) {
                ctx->config.log_enabled = (strcmp(v, "1") == 0 ||
                                          strcasecmp(v, "true") == 0 ||
                                          strcasecmp(v, "yes") == 0);
            } else if (strcmp(k, "LOG_FILE") == 0) {
                SAFE_STRNCPY(ctx->config.log_file, v, sizeof(ctx->config.log_file));
            }
        }
    }

    fclose(fp);

    MB_LOG_INFO("Configuration loaded from %s", config_file);
    MB_LOG_INFO("  KERMIT: %s", ctx->config.kermit_path);
    MB_LOG_INFO("  SEND_ZMODEM: %s", ctx->config.send_zmodem_path);
    MB_LOG_INFO("  RECEIVE_ZMODEM: %s", ctx->config.receive_zmodem_path);
    MB_LOG_INFO("  LOG: %s", ctx->config.log_enabled ? "enabled" : "disabled");
    if (ctx->config.log_enabled) {
        MB_LOG_INFO("  LOG_FILE: %s", ctx->config.log_file);
    }

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
        MB_LOG_ERROR("Failed to get terminal attributes: %s", strerror(errno));
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
        MB_LOG_ERROR("Failed to set terminal attributes: %s", strerror(errno));
        return ERROR_IO;
    }

    /* Set stdin to non-blocking */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    MB_LOG_DEBUG("Terminal setup complete (raw mode)");

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

    MB_LOG_DEBUG("Terminal restored");
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

    MB_LOG_INFO("Connecting to %s:%d...", host, port);
    printf("Connecting to %s:%d...\r\n", host, port);

    ret = telnet_connect(&ctx->telnet, host, port);
    if (ret != SUCCESS) {
        MB_LOG_ERROR("Failed to connect to %s:%d", host, port);
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
        MB_LOG_DEBUG("Initial window size: %dx%d", ctx->telnet.term_width, ctx->telnet.term_height);
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
        MB_LOG_INFO("Disconnecting from telnet server");
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
            MB_LOG_INFO("Window size changed: %dx%d -> %dx%d",
                       ctx->telnet.term_width, ctx->telnet.term_height,
                       new_width, new_height);

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
        MB_LOG_WARNING("Failed to get window size: %s", strerror(errno));
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
        MB_LOG_ERROR("Failed to open log file %s: %s", ctx->config.log_file, strerror(errno));
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

    MB_LOG_INFO("Logging enabled to %s", ctx->config.log_file);
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
static void otelnet_log_data(otelnet_ctx_t *ctx, const char *direction,
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
 */
static int otelnet_parse_command_args(const char *input, char *program, size_t prog_size,
                                      char **args, int max_args, int *arg_count)
{
    char *input_copy, *token;
    char buffer[LINE_BUFFER_SIZE];
    int count = 0;

    if (input == NULL || program == NULL || args == NULL || arg_count == NULL) {
        return ERROR_INVALID_ARG;
    }

    SAFE_STRNCPY(buffer, input, sizeof(buffer));
    input_copy = buffer;

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
        MB_LOG_ERROR("Failed to fork: %s", strerror(errno));
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
        MB_LOG_INFO("Waiting for child process %d (%s) to complete", pid, program_path);

        /* Wait for child to complete */
        waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            MB_LOG_INFO("Program exited with code %d", exit_code);
            printf("\r\n[Program exited with code %d]\r\n", exit_code);
            if (exit_code == 0) {
                printf("[Transfer completed successfully]\r\n");
            } else {
                printf("[Transfer may have failed - check exit code]\r\n");
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            MB_LOG_WARNING("Program terminated by signal %d", sig);
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
 * Process console command
 */
int otelnet_process_console_command(otelnet_ctx_t *ctx, const char *command)
{
    char cmd_buffer[LINE_BUFFER_SIZE];
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

    /* Parse command and arguments */
    otelnet_parse_command_args(cmd, program, sizeof(program), args, 31, &arg_count);

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
        printf("  kermit [args]           - Run kermit file transfer\r\n\r\n");
        printf("Receive Files:\r\n");
        printf("  rz [options]  - Receive via ZMODEM (default)\r\n");
        printf("  ry            - Receive via YMODEM\r\n");
        printf("  rx            - Receive via XMODEM (single file)\r\n\r\n");
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
        printf("  rz                       - Receive via ZMODEM\r\n");
        printf("  ry                       - Receive via YMODEM\r\n");
        printf("  rx                       - Receive via XMODEM\r\n");
        printf("  kermit -s file.dat       - Send via Kermit\r\n");
        printf("  ls /tmp                  - List /tmp directory\r\n");
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
        argv[0] = (char *)ctx->config.kermit_path;
        for (int i = 0; i < arg_count && i < 31; i++) {
            argv[i + 1] = args[i];
        }
        argv[arg_count + 1] = NULL;

        return otelnet_execute_external_program_with_args(ctx, ctx->config.kermit_path, argv);
    }

    /* sz/sx/sy - send with protocol options */
    if (strcmp(program, "sz") == 0 || strcmp(program, "sx") == 0 || strcmp(program, "sy") == 0) {
        const char *protocol = NULL;
        const char *protocol_name = NULL;
        int file_start_idx = 0;

        /* Determine protocol from command or first argument */
        if (strcmp(program, "sx") == 0) {
            protocol = "--xmodem";
            protocol_name = "XMODEM";
        } else if (strcmp(program, "sy") == 0) {
            protocol = "--ymodem";
            protocol_name = "YMODEM";
        } else if (arg_count > 0 &&
                   (strcmp(args[0], "--xmodem") == 0 || strcmp(args[0], "-x") == 0)) {
            protocol = "--xmodem";
            protocol_name = "XMODEM";
            file_start_idx = 1;
        } else if (arg_count > 0 &&
                   (strcmp(args[0], "--ymodem") == 0 || strcmp(args[0], "-y") == 0)) {
            protocol = "--ymodem";
            protocol_name = "YMODEM";
            file_start_idx = 1;
        } else if (arg_count > 0 &&
                   (strcmp(args[0], "--zmodem") == 0 || strcmp(args[0], "-z") == 0)) {
            protocol = "--zmodem";
            protocol_name = "ZMODEM";
            file_start_idx = 1;
        } else {
            /* Default to ZMODEM */
            protocol = "--zmodem";
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

        /* Build argv array: sz <protocol> <files...> */
        argv[0] = (char *)ctx->config.send_zmodem_path;
        argv[1] = (char *)protocol;
        int argv_idx = 2;
        for (int i = file_start_idx; i < arg_count && argv_idx < 32; i++) {
            argv[argv_idx++] = args[i];
        }
        argv[argv_idx] = NULL;

        printf("\r\n[Protocol: %s]\r\n", protocol_name);
        printf("[Sending %d file(s)]\r\n", file_count);

        return otelnet_execute_external_program_with_args(ctx, ctx->config.send_zmodem_path, argv);
    }

    /* rz/rx/ry - receive with protocol options */
    if (strcmp(program, "rz") == 0 || strcmp(program, "rx") == 0 || strcmp(program, "ry") == 0) {
        const char *protocol = NULL;
        const char *protocol_name = NULL;

        /* Determine protocol from command or first argument */
        if (strcmp(program, "rx") == 0) {
            protocol = "--xmodem";
            protocol_name = "XMODEM";
        } else if (strcmp(program, "ry") == 0) {
            protocol = "--ymodem";
            protocol_name = "YMODEM";
        } else if (arg_count > 0 &&
                   (strcmp(args[0], "--xmodem") == 0 || strcmp(args[0], "-x") == 0)) {
            protocol = "--xmodem";
            protocol_name = "XMODEM";
        } else if (arg_count > 0 &&
                   (strcmp(args[0], "--ymodem") == 0 || strcmp(args[0], "-y") == 0)) {
            protocol = "--ymodem";
            protocol_name = "YMODEM";
        } else if (arg_count > 0 &&
                   (strcmp(args[0], "--zmodem") == 0 || strcmp(args[0], "-z") == 0)) {
            protocol = "--zmodem";
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
            protocol = "--zmodem";
            protocol_name = "ZMODEM";
        }

        /* Build argv array: rz <protocol> */
        argv[0] = (char *)ctx->config.receive_zmodem_path;
        if (protocol != NULL) {
            argv[1] = (char *)protocol;
            argv[2] = NULL;
        } else {
            argv[1] = NULL;
        }

        printf("\r\n[Protocol: %s]\r\n", protocol_name);
        printf("[Ready to receive file(s)]\r\n");
        char cwd[BUFFER_SIZE];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("[Save to: %s]\r\n", cwd);
        }

        return otelnet_execute_external_program_with_args(ctx, ctx->config.receive_zmodem_path, argv);
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

    n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return SUCCESS;
        }
        MB_LOG_ERROR("Failed to read from stdin: %s", strerror(errno));
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

            /* Prepare data (escape IAC) */
            telnet_prepare_output(&ctx->telnet, buf, n, telnet_buf, sizeof(telnet_buf), &telnet_len);

            if (telnet_len > 0) {
                ssize_t sent = telnet_send(&ctx->telnet, telnet_buf, telnet_len);
                if (sent > 0) {
                    ctx->bytes_sent += sent;
                    /* Log sent data */
                    otelnet_log_data(ctx, "send", buf, n);
                }
            }
        }
    } else {
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
        MB_LOG_ERROR("Telnet connection error");
        return ERROR_CONNECTION;
    }

    if (n == 0) {
        /* Connection closed or no data */
        if (!telnet_is_connected(&ctx->telnet)) {
            MB_LOG_INFO("Telnet connection closed by server");
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
        otelnet_log_data(ctx, "receive", output_buf, output_len);

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
                MB_LOG_ERROR("Failed to write to stdout: %s", strerror(errno));
                return ERROR_IO;
            }
        } else {
            /* Character mode: output as-is (server handles CRLF) */
            ssize_t written = write(STDOUT_FILENO, output_buf, output_len);
            if (written < 0) {
                MB_LOG_ERROR("Failed to write to stdout: %s", strerror(errno));
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
            MB_LOG_ERROR("select() error: %s", strerror(errno));
            return ERROR_IO;
        }

        if (ret == 0) {
            /* Timeout */
            continue;
        }

        /* Check stdin */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (otelnet_process_stdin(ctx) != SUCCESS) {
                MB_LOG_ERROR("Error processing stdin");
            }
        }

        /* Check telnet socket */
        if (telnet_is_connected(&ctx->telnet)) {
            int telnet_fd = telnet_get_fd(&ctx->telnet);
            if (telnet_fd >= 0 && FD_ISSET(telnet_fd, &readfds)) {
                if (otelnet_process_telnet(ctx) != SUCCESS) {
                    MB_LOG_ERROR("Error processing telnet data");
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
    printf("  Commands: quit, kermit, sz, rz, help, stats\n");
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
