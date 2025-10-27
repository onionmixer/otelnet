/*
 * otelnet.h - Standalone telnet client
 *
 * A standalone telnet client based on ModemBridge's telnet implementation
 * Features:
 * - Ctrl+] for console mode
 * - External program integration (kermit, sz, rz)
 * - Full multibyte character support
 * - RFC 854 compliant telnet protocol
 */

#ifndef OTELNET_H
#define OTELNET_H

/* Standard includes - avoid common.h to prevent symbol conflicts */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>

/* Telnet protocol (standalone header) */
#include "telnet.h"

/* File transfer management */
#include "transfer.h"

/* Constants from common.h */
#define BUFFER_SIZE         4096
#define SMALL_BUFFER_SIZE   256
#define LINE_BUFFER_SIZE    1024
#define SUCCESS             0
#define ERROR_GENERAL       -1
#define ERROR_INVALID_ARG   -2
#define ERROR_IO            -3
#define ERROR_TIMEOUT       -4
#define ERROR_CONNECTION    -5
#define ERROR_CONFIG        -6


/* Utility macros */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define SAFE_STRNCPY(dst, src, size) do { \
    strncpy(dst, src, size - 1); \
    dst[size - 1] = '\0'; \
} while(0)

/* Application name and version */
#define OTELNET_VERSION "1.0.0"
#define OTELNET_APP_NAME "otelnet"

/* Default configuration file */
#define OTELNET_DEFAULT_CONFIG "otelnet.conf"

/* Console mode constants */
#define CONSOLE_TRIGGER_KEY 0x1D    /* Ctrl+] (telnet escape character) */

/* otelnet operation modes */
typedef enum {
    OTELNET_MODE_CLIENT,        /* Normal telnet client mode */
    OTELNET_MODE_CONSOLE,       /* Console command mode (Ctrl+] pressed) */
    OTELNET_MODE_TRANSFER       /* File transfer mode */
} otelnet_mode_t;

/* Configuration structure */
typedef struct {
    /* Session logging */
    bool log_enabled;
    char log_file[BUFFER_SIZE];

    /* Transfer configuration (delegated to transfer module) */
    transfer_config_t transfer;
} otelnet_config_t;

/* Main otelnet context */
typedef struct otelnet_ctx {
    /* Telnet connection */
    telnet_t telnet;

    /* Terminal settings */
    struct termios orig_termios;
    bool termios_saved;

    /* Mode */
    otelnet_mode_t mode;

    /* Console input buffer */
    char console_buffer[LINE_BUFFER_SIZE];
    size_t console_buffer_len;

    /* Line mode input buffer (for redisplay after server output) */
    char line_buffer[LINE_BUFFER_SIZE];
    size_t line_buffer_len;

    /* Configuration */
    otelnet_config_t config;

    /* Running flag */
    bool running;

    /* Logging */
    FILE *log_fp;

    /* Statistics */
    uint64_t bytes_sent;
    uint64_t bytes_received;
    time_t connection_start_time;

    /* Transfer state */
    transfer_state_t transfer;

    /* Protocol auto-detection */
    zmodem_detector_t zmodem_detector;
    xmodem_detector_t xmodem_detector;
    ymodem_detector_t ymodem_detector;

    /* Pending data buffer (for drain process)
     * During BINARY mode negotiation, if non-telnet data arrives early,
     * it's stored here to prevent loss */
    unsigned char pending_data[BUFFER_SIZE];
    size_t pending_data_len;
} otelnet_ctx_t;

/* Function prototypes */

/**
 * Initialize otelnet context
 * @param ctx Context to initialize
 */
void otelnet_init(otelnet_ctx_t *ctx);

/**
 * Load configuration from file
 * @param ctx Context
 * @param config_file Path to configuration file
 * @return SUCCESS on success, error code on failure
 */
int otelnet_load_config(otelnet_ctx_t *ctx, const char *config_file);

/**
 * Setup terminal for raw mode
 * @param ctx Context
 * @return SUCCESS on success, error code on failure
 */
int otelnet_setup_terminal(otelnet_ctx_t *ctx);

/**
 * Restore terminal to original settings
 * @param ctx Context
 */
void otelnet_restore_terminal(otelnet_ctx_t *ctx);

/**
 * Connect to telnet server
 * @param ctx Context
 * @param host Hostname or IP address
 * @param port Port number
 * @return SUCCESS on success, error code on failure
 */
int otelnet_connect(otelnet_ctx_t *ctx, const char *host, int port);

/**
 * Disconnect from telnet server
 * @param ctx Context
 */
void otelnet_disconnect(otelnet_ctx_t *ctx);

/**
 * Main event loop
 * @param ctx Context
 * @return SUCCESS on success, error code on failure
 */
int otelnet_run(otelnet_ctx_t *ctx);

/**
 * Process data from stdin
 * @param ctx Context
 * @return SUCCESS on success, error code on failure
 */
int otelnet_process_stdin(otelnet_ctx_t *ctx);

/**
 * Process data from telnet server
 * @param ctx Context
 * @return SUCCESS on success, error code on failure
 */
int otelnet_process_telnet(otelnet_ctx_t *ctx);

/**
 * Enter console mode
 * @param ctx Context
 */
void otelnet_enter_console_mode(otelnet_ctx_t *ctx);

/**
 * Exit console mode
 * @param ctx Context
 */
void otelnet_exit_console_mode(otelnet_ctx_t *ctx);

/**
 * Process console command
 * @param ctx Context
 * @param command Command string
 * @return SUCCESS on success, error code on failure
 */
int otelnet_process_console_command(otelnet_ctx_t *ctx, const char *command);

/**
 * Execute external program
 * @param ctx Context
 * @param program_path Path to program
 * @return SUCCESS on success, error code on failure
 */
int otelnet_execute_external_program(otelnet_ctx_t *ctx, const char *program_path);

/**
 * Print statistics
 * @param ctx Context
 */
void otelnet_print_stats(otelnet_ctx_t *ctx);

/**
 * Print usage information
 * @param program_name Program name
 */
void otelnet_print_usage(const char *program_name);

/**
 * Write data to log file (hex + ASCII format)
 * @param ctx Context
 * @param direction Direction label ("SEND", "RECEIVE", "KERMIT-SEND", "KERMIT-RECEIVE", etc.)
 * @param data Data buffer
 * @param len Data length
 */
void otelnet_log_data(otelnet_ctx_t *ctx, const char *direction,
                      const unsigned char *data, size_t len);

#endif /* OTELNET_H */
