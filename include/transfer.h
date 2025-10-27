/*
 * transfer.h - File transfer management for otelnet
 *
 * Handles file transfer protocols (ZMODEM, XMODEM, YMODEM, Kermit)
 * with 8-bit transparent transmission, auto-detection, and error handling
 */

#ifndef OTELNET_TRANSFER_H
#define OTELNET_TRANSFER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/types.h>
#include <termios.h>

/* Need telnet_t for function prototypes */
#include "telnet.h"

/* Buffer sizes */
#define BUFFER_SIZE         4096
#define SMALL_BUFFER_SIZE   256

/* Transfer constants */
#define ZMODEM_DETECT_BUFFER_SIZE 32
#define XMODEM_YMODEM_DETECT_BUFFER_SIZE 64  /* Buffer for text pattern detection */
#define XMODEM_YMODEM_DETECT_WINDOW 3   /* Detection time window in seconds */
#define XMODEM_YMODEM_MIN_REPEATS   3   /* Minimum repeats to trigger detection */
#define TRANSFER_DEFAULT_TIMEOUT  300   /* 5 minutes */
#define TRANSFER_DATA_TIMEOUT     30    /* 30 seconds */

/* ZMODEM protocol constants */
#define ZPAD '*'    /* Pad character (0x2A) */
#define ZDLE 0x18   /* Ctrl-X (24 decimal, 0x18 hex) */

/* XMODEM/YMODEM protocol constants */
#define XMODEM_NAK  0x15  /* NAK - Negative Acknowledge (checksum mode) */
#define XMODEM_CRC  0x43  /* 'C' - CRC mode request */
#define XMODEM_SOH  0x01  /* Start of 128-byte block */
#define XMODEM_STX  0x02  /* Start of 1024-byte block (XMODEM-1K/YMODEM) */
#define XMODEM_EOT  0x04  /* End of Transmission */
#define XMODEM_ACK  0x06  /* Acknowledge */
#define XMODEM_CAN  0x18  /* Cancel (Ctrl-X) */

/* Success/Error codes */
#define SUCCESS             0
#define ERROR_GENERAL       -1
#define ERROR_INVALID_ARG   -2
#define ERROR_IO            -3
#define ERROR_TIMEOUT       -4
#define ERROR_CONNECTION    -5

/* File transfer protocol types */
typedef enum {
    TRANSFER_NONE,
    TRANSFER_ZMODEM_SEND,
    TRANSFER_ZMODEM_RECV,
    TRANSFER_XMODEM_SEND,
    TRANSFER_XMODEM_RECV,
    TRANSFER_YMODEM_SEND,
    TRANSFER_YMODEM_RECV,
    TRANSFER_KERMIT_SEND,
    TRANSFER_KERMIT_RECV
} transfer_protocol_t;

/* Transfer error types */
typedef enum {
    TRANSFER_ERROR_NONE = 0,
    TRANSFER_ERROR_TIMEOUT,
    TRANSFER_ERROR_NETWORK,
    TRANSFER_ERROR_PROTOCOL,
    TRANSFER_ERROR_CHILD_CRASH,
    TRANSFER_ERROR_USER_CANCEL,
    TRANSFER_ERROR_DISK_FULL,
    TRANSFER_ERROR_PERMISSION,
    TRANSFER_ERROR_UNKNOWN
} transfer_error_t;

/* File transfer state */
typedef struct {
    bool active;                    /* Transfer currently active */
    transfer_protocol_t protocol;   /* Protocol being used */
    time_t start_time;              /* Transfer start timestamp */
    time_t last_data_time;          /* Last data received timestamp */
    pid_t child_pid;                /* Child process PID (rz/sz/kermit) */
    bool auto_triggered;            /* Auto-triggered by remote signal */
    char filename[BUFFER_SIZE];     /* Current filename being transferred */
    uint64_t bytes_transferred;     /* Bytes transferred so far */
    uint64_t total_bytes;           /* Total bytes (if known) */

    /* Saved telnet state for restoration after transfer */
    bool saved_binary_local;        /* Original local BINARY mode state */
    bool saved_binary_remote;       /* Original remote BINARY mode state */
    bool saved_echo_local;          /* Original local ECHO mode state */
    bool saved_echo_remote;         /* Original remote ECHO mode state */
    bool saved_sga_local;           /* Original local SGA mode state */
    bool saved_sga_remote;          /* Original remote SGA mode state */
    bool saved_linemode_active;     /* Original LINEMODE active state */
} transfer_state_t;

/* Transfer configuration */
typedef struct {
    char kermit_path[BUFFER_SIZE];
    char send_zmodem_path[BUFFER_SIZE];
    char receive_zmodem_path[BUFFER_SIZE];

    /* Auto-detection settings */
    bool auto_zmodem_enabled;           /* Auto-detect ZMODEM transfers */
    bool auto_zmodem_prompt;            /* Prompt for filename on upload */
    char auto_zmodem_download_dir[BUFFER_SIZE]; /* Download directory */
    bool auto_xmodem_enabled;           /* Auto-detect XMODEM transfers */
    bool auto_xmodem_prompt;            /* Prompt for filename on XMODEM upload */
    bool auto_ymodem_enabled;           /* Auto-detect YMODEM transfers */
    bool auto_ymodem_prompt;            /* Prompt for filename on YMODEM upload */

    /* Timeout settings */
    int transfer_timeout_seconds;       /* Overall transfer timeout */
    int transfer_data_timeout_seconds;  /* Data receive timeout */

    /* Logging settings */
    bool transfer_log_enabled;          /* Transfer-specific logging */
    char transfer_log_file[BUFFER_SIZE]; /* Transfer log file */
    bool transfer_keep_partial;         /* Keep partial files on error */
} transfer_config_t;

/* ZMODEM detection state */
typedef struct {
    unsigned char buffer[ZMODEM_DETECT_BUFFER_SIZE];
    size_t len;
    bool enabled;  /* Only detect when not already in transfer */
} zmodem_detector_t;

/* XMODEM detection state */
typedef struct {
    unsigned char buffer[XMODEM_YMODEM_DETECT_BUFFER_SIZE];  /* Sliding window for text pattern detection */
    size_t buf_len;             /* Current buffer length */
    char detected_filename[SMALL_BUFFER_SIZE];  /* Filename extracted from server message */
    unsigned char last_char;    /* Last character seen (NAK or 'C') */
    int repeat_count;           /* Number of repeats */
    time_t first_seen;          /* First occurrence timestamp */
    time_t last_seen;           /* Last occurrence timestamp */
    bool enabled;               /* Only detect when not already in transfer */
} xmodem_detector_t;

/* YMODEM detection state */
typedef struct {
    unsigned char buffer[XMODEM_YMODEM_DETECT_BUFFER_SIZE];  /* Sliding window for text pattern detection */
    size_t buf_len;             /* Current buffer length */
    char detected_filename[SMALL_BUFFER_SIZE];  /* Filename extracted from server message */
    int c_repeat_count;         /* Number of 'C' character repeats */
    time_t first_seen;          /* First 'C' occurrence timestamp */
    time_t last_seen;           /* Last 'C' occurrence timestamp */
    bool enabled;               /* Only detect when not already in transfer */
} ymodem_detector_t;

/* Transfer context (forward declaration to avoid circular dependency) */
struct otelnet_ctx;

/* Function prototypes */

/**
 * Initialize transfer state
 * @param state Transfer state to initialize
 */
void transfer_init(transfer_state_t *state);

/**
 * Initialize transfer configuration with defaults
 * @param config Transfer configuration to initialize
 */
void transfer_config_init(transfer_config_t *config);

/**
 * Enter transfer mode
 * @param state Transfer state
 * @param protocol Transfer protocol to use
 * @return SUCCESS on success, error code on failure
 */
int transfer_enter_mode(transfer_state_t *state, transfer_protocol_t protocol);

/**
 * Exit transfer mode
 * @param state Transfer state
 */
void transfer_exit_mode(transfer_state_t *state);

/**
 * Check if transfer is active
 * @param state Transfer state
 * @return true if transfer is active
 */
bool transfer_is_active(const transfer_state_t *state);

/**
 * Check if transfer has timed out
 * @param state Transfer state
 * @param config Transfer configuration
 * @return true if transfer has timed out
 */
bool transfer_check_timeout(const transfer_state_t *state, const transfer_config_t *config);

/**
 * Check if data timeout has occurred
 * @param state Transfer state
 * @param config Transfer configuration
 * @return true if data timeout has occurred
 */
bool transfer_check_data_timeout(const transfer_state_t *state, const transfer_config_t *config);

/**
 * Update last data received timestamp
 * @param state Transfer state
 */
void transfer_update_data_timestamp(transfer_state_t *state);

/**
 * Setup terminal for 8-bit transparent file transfer
 * Disables all character transformations and enables binary mode
 * @param saved_termios Pointer to store original termios settings
 * @return SUCCESS on success, error code on failure
 */
int transfer_setup_terminal(struct termios *saved_termios);

/**
 * Restore terminal from transfer mode
 * @param saved_termios Original termios settings to restore
 * @return SUCCESS on success, error code on failure
 */
int transfer_restore_terminal(const struct termios *saved_termios);

/**
 * Handle transfer error and cleanup
 * @param state Transfer state
 * @param error_type Type of error that occurred
 */
void transfer_handle_error(transfer_state_t *state, transfer_error_t error_type);

/**
 * Get error message for error type
 * @param error_type Error type
 * @return Human-readable error message
 */
const char *transfer_get_error_message(transfer_error_t error_type);

/**
 * Initialize ZMODEM detector
 * @param detector ZMODEM detector state
 */
void zmodem_detector_init(zmodem_detector_t *detector);

/**
 * Enable/disable ZMODEM detection
 * @param detector ZMODEM detector state
 * @param enabled Enable or disable detection
 */
void zmodem_detector_set_enabled(zmodem_detector_t *detector, bool enabled);

/**
 * Detect ZMODEM trigger sequences in incoming data
 * @param detector ZMODEM detector state
 * @param data Incoming data buffer
 * @param len Length of data
 * @param is_receive_init Output: true if remote wants to send (we receive)
 * @param is_send_init Output: true if remote wants to receive (we send)
 * @return true if ZMODEM sequence detected
 */
bool zmodem_detect_trigger(zmodem_detector_t *detector, const unsigned char *data, size_t len,
                           bool *is_receive_init, bool *is_send_init);

/**
 * Initialize XMODEM detector
 * @param detector XMODEM detector state
 */
void xmodem_detector_init(xmodem_detector_t *detector);

/**
 * Enable/disable XMODEM detection
 * @param detector XMODEM detector state
 * @param enabled Enable or disable detection
 */
void xmodem_detector_set_enabled(xmodem_detector_t *detector, bool enabled);

/**
 * Detect XMODEM trigger sequences in incoming data
 * Detects repeated NAK or 'C' characters indicating receiver ready, or text message indicating sender ready
 * @param detector XMODEM detector state
 * @param data Incoming data buffer
 * @param len Length of data
 * @param is_receive_init Output: true if remote is sending (we should receive)
 * @param is_send_init Output: true if remote is receiving (we should send)
 * @return true if XMODEM sequence detected
 */
bool xmodem_detect_trigger(xmodem_detector_t *detector, const unsigned char *data, size_t len,
                           bool *is_receive_init, bool *is_send_init);

/**
 * Initialize YMODEM detector
 * @param detector YMODEM detector state
 */
void ymodem_detector_init(ymodem_detector_t *detector);

/**
 * Enable/disable YMODEM detection
 * @param detector YMODEM detector state
 * @param enabled Enable or disable detection
 */
void ymodem_detector_set_enabled(ymodem_detector_t *detector, bool enabled);

/**
 * Detect YMODEM trigger sequences in incoming data
 * Detects repeated 'C' characters indicating receiver ready, or text message indicating sender ready
 * @param detector YMODEM detector state
 * @param data Incoming data buffer
 * @param len Length of data
 * @param is_receive_init Output: true if remote is sending (we should receive)
 * @param is_send_init Output: true if remote is receiving (we should send)
 * @return true if YMODEM sequence detected
 */
bool ymodem_detect_trigger(ymodem_detector_t *detector, const unsigned char *data, size_t len,
                           bool *is_receive_init, bool *is_send_init);

/**
 * Get protocol name as string
 * @param protocol Transfer protocol
 * @return Protocol name string
 */
const char *transfer_protocol_name(transfer_protocol_t protocol);

/**
 * Log transfer start
 * @param config Transfer configuration
 * @param state Transfer state
 */
void transfer_log_start(const transfer_config_t *config, const transfer_state_t *state);

/**
 * Log transfer end
 * @param config Transfer configuration
 * @param state Transfer state
 * @param error_type Error type (TRANSFER_ERROR_NONE for success)
 */
void transfer_log_end(const transfer_config_t *config, const transfer_state_t *state,
                      transfer_error_t error_type);

/**
 * Execute Kermit send
 * @param config Transfer configuration
 * @param state Transfer state
 * @param socket_fd Telnet socket file descriptor
 * @param filename File to send
 * @param telnet_ctx Telnet context
 * @param otelnet_ctx Otelnet context (for disabling auto-detection)
 * @return SUCCESS on success, error code on failure
 */
int transfer_execute_kermit_send(const transfer_config_t *config, transfer_state_t *state,
                                 int socket_fd, const char *filename,
                                 telnet_t *telnet_ctx, struct otelnet_ctx *otelnet_ctx);

/**
 * Execute Kermit receive
 * @param config Transfer configuration
 * @param state Transfer state
 * @param socket_fd Telnet socket file descriptor
 * @param telnet_ctx Telnet context
 * @param otelnet_ctx Otelnet context (for disabling auto-detection)
 * @return SUCCESS on success, error code on failure
 */
int transfer_execute_kermit_receive(const transfer_config_t *config, transfer_state_t *state,
                                    int socket_fd,
                                    telnet_t *telnet_ctx, struct otelnet_ctx *otelnet_ctx);

/**
 * Execute ZMODEM/XMODEM/YMODEM transfer
 * @param config Transfer configuration
 * @param state Transfer state
 * @param socket_fd Telnet socket file descriptor
 * @param protocol Transfer protocol
 * @param filename Filename for send operations (NULL for receive)
 * @return SUCCESS on success, error code on failure
 */
int transfer_execute_modem(const transfer_config_t *config, transfer_state_t *state,
                           int socket_fd, transfer_protocol_t protocol, const char *filename,
                           telnet_t *telnet_ctx);

/**
 * Execute ZMODEM/XMODEM/YMODEM transfer with multiple files
 * @param config Transfer configuration
 * @param state Transfer state
 * @param socket_fd Telnet socket file descriptor
 * @param protocol Transfer protocol
 * @param filenames Array of filenames for send operations (NULL for receive)
 * @param file_count Number of files
 * @return SUCCESS on success, error code on failure
 */
int transfer_execute_modem_files(const transfer_config_t *config, transfer_state_t *state,
                                 int socket_fd, transfer_protocol_t protocol,
                                 char * const filenames[], int file_count,
                                 telnet_t *telnet_ctx);

/**
 * Request transfer cancellation (called from signal handler)
 * Thread-safe: uses sig_atomic_t flag
 */
void transfer_request_cancel(void);

/**
 * Check if transfer cancellation has been requested
 * @return true if cancellation requested
 */
bool transfer_is_cancel_requested(void);

/**
 * Clear transfer cancellation flag
 * Should be called after handling cancellation or completing transfer
 */
void transfer_clear_cancel(void);

/* ============================================================================
 * Telnet IAC Escaping Functions
 * ============================================================================
 * These functions handle IAC (0xFF) byte escaping/unescaping for binary
 * data transmission over telnet protocol according to RFC 854.
 *
 * In telnet protocol, even in BINARY mode, 0xFF is a special byte (IAC -
 * Interpret As Command). Any 0xFF in the data stream must be escaped as
 * 0xFF 0xFF to distinguish it from telnet commands.
 *
 * Note: TELNET_IAC is defined in telnet.h (value 255 / 0xFF)
 */

/**
 * Escape IAC bytes (0xFF → 0xFF 0xFF) for telnet transmission
 *
 * Converts binary data for safe transmission over telnet protocol by
 * escaping all 0xFF bytes. This must be done BEFORE sending data to
 * the telnet socket.
 *
 * @param input      Input buffer (original binary data from lsz/sz)
 * @param input_len  Length of input data
 * @param output     Output buffer (escaped data for network transmission)
 * @param output_max Maximum size of output buffer (should be >= input_len * 2)
 * @return           Length of escaped data (>= input_len), or -1 on error
 *
 * Example: {0x12, 0xFF, 0x34} → {0x12, 0xFF, 0xFF, 0x34} (3 bytes → 4 bytes)
 *
 * Error conditions:
 *   - Returns -1 if output buffer too small (errno = ENOBUFS)
 *   - Returns -1 if input or output is NULL (errno = EINVAL)
 */
ssize_t telnet_escape_iac(const unsigned char *input, size_t input_len,
                          unsigned char *output, size_t output_max);

/**
 * Unescape IAC bytes (0xFF 0xFF → 0xFF) from telnet stream
 *
 * Converts telnet protocol data back to binary by unescaping IAC sequences.
 * This must be done AFTER receiving data from the telnet socket and BEFORE
 * passing it to lrz/rz or other binary protocol handlers.
 *
 * @param input      Input buffer (escaped telnet data from network)
 * @param input_len  Length of input data
 * @param output     Output buffer (unescaped binary data for lrz/rz)
 * @param output_max Maximum size of output buffer
 * @param iac_state  Pointer to IAC state variable (must persist between calls)
 *                   - Initialize to 0 before first call in a session
 *                   - Keep same variable across all buffer reads
 *                   - Handles partial IAC sequences at buffer boundaries
 * @return           Length of unescaped data (<= input_len), or -1 on error
 *
 * State values:
 *   0 = Normal data mode
 *   1 = Saw 0xFF, waiting for next byte
 *
 * Example: {0x12, 0xFF, 0xFF, 0x34} → {0x12, 0xFF, 0x34} (4 bytes → 3 bytes)
 *
 * Important: This function handles partial IAC sequences across buffer
 * boundaries. If a buffer ends with 0xFF, iac_state will be set to 1, and
 * the next call will process the following byte correctly.
 *
 * Error conditions:
 *   - Returns -1 if output buffer too small (errno = ENOBUFS)
 *   - Returns -1 if input, output, or iac_state is NULL (errno = EINVAL)
 */
ssize_t telnet_unescape_iac(const unsigned char *input, size_t input_len,
                            unsigned char *output, size_t output_max,
                            int *iac_state);

#endif /* OTELNET_TRANSFER_H */
