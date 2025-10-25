/*
 * telnet.h - Telnet client protocol implementation
 *
 * Implements Telnet protocol (RFC 854) for connecting to telnet servers
 * Handles IAC commands, option negotiation, line mode, and character mode
 */

#ifndef OTELNET_TELNET_H
#define OTELNET_TELNET_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* Constants */
#define BUFFER_SIZE         4096
#define SMALL_BUFFER_SIZE   256
#define SUCCESS             0
#define ERROR_GENERAL       -1
#define ERROR_INVALID_ARG   -2
#define ERROR_IO            -3
#define ERROR_TIMEOUT       -4
#define ERROR_CONNECTION    -5

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

/* Utility macros */
#define SAFE_STRNCPY(dst, src, size) do { \
    strncpy(dst, src, size - 1); \
    dst[size - 1] = '\0'; \
} while(0)

/* Telnet protocol constants (RFC 854) */
#define TELNET_IAC          255     /* Interpret As Command */
#define TELNET_DONT         254     /* Don't use option */
#define TELNET_DO           253     /* Do use option */
#define TELNET_WONT         252     /* Won't use option */
#define TELNET_WILL         251     /* Will use option */
#define TELNET_SB           250     /* Subnegotiation begin */
#define TELNET_GA           249     /* Go ahead */
#define TELNET_EL           248     /* Erase line */
#define TELNET_EC           247     /* Erase character */
#define TELNET_AYT          246     /* Are you there */
#define TELNET_AO           245     /* Abort output */
#define TELNET_IP           244     /* Interrupt process */
#define TELNET_BREAK        243     /* Break */
#define TELNET_DM           242     /* Data mark */
#define TELNET_NOP          241     /* No operation */
#define TELNET_SE           240     /* Subnegotiation end */
#define TELNET_EOR          239     /* End of record */

/* Telnet options */
#define TELOPT_BINARY       0       /* Binary transmission */
#define TELOPT_ECHO         1       /* Echo */
#define TELOPT_SGA          3       /* Suppress go ahead */
#define TELOPT_STATUS       5       /* Status */
#define TELOPT_TIMING_MARK  6       /* Timing mark */
#define TELOPT_TTYPE        24      /* Terminal type */
#define TELOPT_NAWS         31      /* Negotiate about window size */
#define TELOPT_TSPEED       32      /* Terminal speed */
#define TELOPT_LFLOW        33      /* Remote flow control */
#define TELOPT_LINEMODE     34      /* Linemode */
#define TELOPT_ENVIRON      36      /* Environment variables */

/* TERMINAL-TYPE subnegotiation codes (RFC 1091) */
#define TTYPE_IS            0       /* Terminal type IS */
#define TTYPE_SEND          1       /* Send terminal type */

/* ENVIRON subnegotiation codes (RFC 1572) */
#define ENV_IS              0       /* Environment IS */
#define ENV_SEND            1       /* Send environment */
#define ENV_VAR             0       /* Environment variable */
#define ENV_VALUE           1       /* Variable value */
#define ENV_ESC             2       /* Escape character */
#define ENV_USERVAR         3       /* User variable */

/* LINEMODE subnegotiation codes (RFC 1184) */
#define LM_MODE             1       /* Linemode MODE */
#define LM_FORWARDMASK      2       /* Forward mask */
#define LM_SLC              3       /* Set local characters */

/* LINEMODE MODE bits */
#define MODE_EDIT           0x01    /* Local editing */
#define MODE_TRAPSIG        0x02    /* Trap signals */
#define MODE_ACK            0x04    /* Acknowledge mode change */
#define MODE_SOFT_TAB       0x08    /* Soft tab */
#define MODE_LIT_ECHO       0x10    /* Literal echo */

/* Telnet state machine states */
typedef enum {
    TELNET_STATE_DATA,          /* Normal data */
    TELNET_STATE_IAC,           /* Received IAC */
    TELNET_STATE_WILL,          /* Received WILL */
    TELNET_STATE_WONT,          /* Received WONT */
    TELNET_STATE_DO,            /* Received DO */
    TELNET_STATE_DONT,          /* Received DONT */
    TELNET_STATE_SB,            /* In subnegotiation */
    TELNET_STATE_SB_IAC,        /* Received IAC in subnegotiation */
    TELNET_STATE_SEENCR         /* Received CR (for CR/LF processing) */
} telnet_state_t;

/* Telnet connection structure */
typedef struct {
    int fd;                         /* Socket file descriptor */
    char host[SMALL_BUFFER_SIZE];   /* Remote host */
    int port;                       /* Remote port */
    bool is_connected;              /* Connection status */

    /* Protocol state */
    telnet_state_t state;           /* Current protocol state */
    unsigned char option;           /* Current option being negotiated */

    /* Subnegotiation buffer */
    unsigned char sb_buffer[BUFFER_SIZE];
    size_t sb_len;

    /* Option tracking */
    bool local_options[256];        /* Options we support locally */
    bool remote_options[256];       /* Options remote supports */

    /* Mode flags (bidirectional - RFC 855 compliant) */
    bool binary_local;              /* We send binary */
    bool binary_remote;             /* They send binary */
    bool echo_local;                /* We echo */
    bool echo_remote;               /* They echo */
    bool sga_local;                 /* We suppress GA */
    bool sga_remote;                /* They suppress GA */
    bool linemode_active;           /* Linemode option active */
    bool linemode_edit;             /* Local editing enabled */

    /* Deprecated: kept for compatibility, use bidirectional flags */
    bool binary_mode;               /* Binary transmission mode (OR of local/remote) */
    bool echo_mode;                 /* Echo mode (remote echo) */
    bool sga_mode;                  /* Suppress go ahead (OR of local/remote) */
    bool linemode;                  /* Line mode vs character mode */

    /* Terminal type (RFC 1091 multi-type support) */
    char terminal_type[64];         /* Current terminal type (e.g., "ANSI", "VT100") */
    int ttype_index;                /* Terminal type cycle index */

    /* Terminal size (NAWS - RFC 1073) */
    int term_width;                 /* Terminal width in columns */
    int term_height;                /* Terminal height in rows */

    /* Terminal speed (TSPEED - RFC 1079) */
    char terminal_speed[32];        /* Terminal speed (e.g., "38400,38400") */
} telnet_t;

/* Function prototypes */

/**
 * Initialize telnet structure
 * @param tn Telnet structure to initialize
 */
void telnet_init(telnet_t *tn);

/**
 * Connect to telnet server
 * @param tn Telnet structure
 * @param host Remote host (IP or hostname)
 * @param port Remote port
 * @return SUCCESS on success, error code on failure
 */
int telnet_connect(telnet_t *tn, const char *host, int port);

/**
 * Disconnect from telnet server
 * @param tn Telnet structure
 * @return SUCCESS on success, error code on failure
 */
int telnet_disconnect(telnet_t *tn);

/**
 * Process incoming data from telnet server
 * Handles IAC sequences and returns clean data
 * @param tn Telnet structure
 * @param input Input data buffer
 * @param input_len Input data length
 * @param output Output buffer for clean data
 * @param output_size Size of output buffer
 * @param output_len Pointer to store actual output length
 * @return SUCCESS on success, error code on failure
 */
int telnet_process_input(telnet_t *tn, const unsigned char *input, size_t input_len,
                         unsigned char *output, size_t output_size, size_t *output_len);

/**
 * Prepare data for sending to telnet server
 * Escapes IAC bytes (0xFF -> 0xFF 0xFF)
 * @param tn Telnet structure
 * @param input Input data buffer
 * @param input_len Input data length
 * @param output Output buffer for escaped data
 * @param output_size Size of output buffer
 * @param output_len Pointer to store actual output length
 * @return SUCCESS on success, error code on failure
 */
int telnet_prepare_output(telnet_t *tn, const unsigned char *input, size_t input_len,
                          unsigned char *output, size_t output_size, size_t *output_len);

/**
 * Send data to telnet server
 * @param tn Telnet structure
 * @param data Data to send
 * @param len Data length
 * @return Number of bytes sent, or error code on failure
 */
ssize_t telnet_send(telnet_t *tn, const void *data, size_t len);

/**
 * Receive data from telnet server
 * @param tn Telnet structure
 * @param buffer Buffer to store received data
 * @param size Maximum bytes to receive
 * @return Number of bytes received, or error code on failure
 */
ssize_t telnet_recv(telnet_t *tn, void *buffer, size_t size);

/**
 * Send IAC command
 * @param tn Telnet structure
 * @param command Command byte
 * @return SUCCESS on success, error code on failure
 */
int telnet_send_command(telnet_t *tn, unsigned char command);

/**
 * Send option negotiation
 * @param tn Telnet structure
 * @param command WILL/WONT/DO/DONT
 * @param option Option code
 * @return SUCCESS on success, error code on failure
 */
int telnet_send_negotiate(telnet_t *tn, unsigned char command, unsigned char option);

/**
 * Handle received option negotiation
 * @param tn Telnet structure
 * @param command WILL/WONT/DO/DONT
 * @param option Option code
 * @return SUCCESS on success, error code on failure
 */
int telnet_handle_negotiate(telnet_t *tn, unsigned char command, unsigned char option);

/**
 * Handle subnegotiation
 * @param tn Telnet structure
 * @return SUCCESS on success, error code on failure
 */
int telnet_handle_subnegotiation(telnet_t *tn);

/**
 * Send NAWS (Negotiate About Window Size) subnegotiation
 * @param tn Telnet structure
 * @param width Terminal width in columns
 * @param height Terminal height in rows
 * @return SUCCESS on success, error code on failure
 */
int telnet_send_naws(telnet_t *tn, int width, int height);

/**
 * Get file descriptor for select/poll
 * @param tn Telnet structure
 * @return File descriptor, or -1 if not connected
 */
int telnet_get_fd(telnet_t *tn);

/**
 * Check if connected to telnet server
 * @param tn Telnet structure
 * @return true if connected, false otherwise
 */
bool telnet_is_connected(telnet_t *tn);

/**
 * Check if in line mode
 * @param tn Telnet structure
 * @return true if line mode, false if character mode
 */
bool telnet_is_linemode(telnet_t *tn);

/**
 * Check if in binary mode
 * @param tn Telnet structure
 * @return true if binary mode, false otherwise
 */
bool telnet_is_binary_mode(telnet_t *tn);

/**
 * Print current telnet mode and binary mode state (DEBUG only)
 * Logs current char/line mode and binary transmission status
 * @param tn Telnet structure
 * @param prefix Message prefix (e.g., "Before transfer", "After transfer")
 */
void telnet_debug_print_mode(telnet_t *tn, const char *prefix);

/**
 * Save current telnet protocol state (for file transfers)
 * Saves BINARY, ECHO, SGA, and LINEMODE states for later restoration
 * @param tn Telnet structure
 * @param saved_binary_local Pointer to save local BINARY state
 * @param saved_binary_remote Pointer to save remote BINARY state
 * @param saved_echo_local Pointer to save local ECHO state
 * @param saved_echo_remote Pointer to save remote ECHO state
 * @param saved_sga_local Pointer to save local SGA state
 * @param saved_sga_remote Pointer to save remote SGA state
 * @param saved_linemode_active Pointer to save LINEMODE active state
 * @return SUCCESS on success, error code on failure
 */
int telnet_save_state(telnet_t *tn,
                      bool *saved_binary_local, bool *saved_binary_remote,
                      bool *saved_echo_local, bool *saved_echo_remote,
                      bool *saved_sga_local, bool *saved_sga_remote,
                      bool *saved_linemode_active);

/**
 * Request BINARY mode (for file transfers)
 * Enables 8-bit clean transmission by negotiating BINARY mode
 * @param tn Telnet structure
 * @return SUCCESS on success, error code on failure
 */
int telnet_request_binary_mode(telnet_t *tn);

/**
 * Restore telnet protocol state (after file transfers)
 * Restores BINARY, ECHO, SGA, and LINEMODE states to saved values
 * @param tn Telnet structure
 * @param saved_binary_local Original local BINARY state
 * @param saved_binary_remote Original remote BINARY state
 * @param saved_echo_local Original local ECHO state
 * @param saved_echo_remote Original remote ECHO state
 * @param saved_sga_local Original local SGA state
 * @param saved_sga_remote Original remote SGA state
 * @param saved_linemode_active Original LINEMODE active state
 * @return SUCCESS on success, error code on failure
 */
int telnet_restore_state(telnet_t *tn,
                        bool saved_binary_local, bool saved_binary_remote,
                        bool saved_echo_local, bool saved_echo_remote,
                        bool saved_sga_local, bool saved_sga_remote,
                        bool saved_linemode_active);

#endif /* OTELNET_TELNET_H */
