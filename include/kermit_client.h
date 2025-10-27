/*
 * kermit_client.h - Embedded Kermit integration for otelnet client
 *
 * Provides in-process Kermit file transfer without requiring external
 * kermit binary or TTY. Uses ekermit library with custom I/O callbacks.
 */

#ifndef OTELNET_KERMIT_CLIENT_H
#define OTELNET_KERMIT_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "transfer.h"
#include "telnet.h"

/* Undefine X_OK from unistd.h to avoid conflict with ekermit */
#ifdef X_OK
#undef X_OK
#endif

/* Undefine F_CRC and F_AT if defined by command line to avoid redefinition errors
 * kermit.h will define them based on NO_CRC/NO_AT flags */
#ifdef F_CRC
#undef F_CRC
#endif
#ifdef F_AT
#undef F_AT
#endif

/* Include ekermit headers */
#include "cdefs.h"
#include "kermit.h"

/* ekermit's kermit.h undefines NULL intentionally - redefine it for our use */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* Forward declarations */
struct otelnet_ctx;

/* Buffer size for file I/O
 * Per PROCESS_REPORT_1_02_RECEIVER.md lines 1513-1582:
 * - Kermit uses control character prefixing (e.g., 0x00 → "#@")
 * - Binary data with many control characters can expand by up to 2x
 * - 20% control characters → 120% final size
 * - Safe buffer size: 2x to prevent truncation during encoding
 *
 * With Long Packets (F_LP):
 * - Max packet data: 4096 bytes raw
 * - Max encoded: 8192 bytes (worst case: all control characters)
 * - Buffer size: 8192 bytes ensures no overflow
 */
#define KERMIT_FILE_BUFFER_SIZE 8192

/*
 * Kermit client context - wraps otelnet state for ekermit I/O callbacks
 */
typedef struct {
    /* Communication */
    int socket_fd;                    /* Telnet socket for packet I/O */
    telnet_t *telnet_ctx;             /* Telnet context for BINARY mode */
    transfer_state_t *transfer_state; /* Transfer state for progress tracking */
    struct otelnet_ctx *otelnet_ctx;  /* Otelnet context (for pending buffer access) */

    /* File I/O state */
    FILE *file;                       /* Current file handle */
    char current_filename[256];       /* Current filename */
    char send_file_absolute_path[512]; /* Absolute path for sending (used in openf for mode=1) */
    bool is_sending;                  /* Send mode vs receive mode */

    /* Statistics */
    uint64_t bytes_total;             /* Total bytes to transfer */
    uint64_t bytes_done;              /* Bytes transferred so far */
    time_t start_time;                /* Transfer start time */
    time_t last_activity;             /* Last I/O activity */

    /* Error handling */
    int last_errno;                   /* Last error number */
    char error_msg[256];              /* Error message buffer */

    /* Retry control */
    int consecutive_naks;             /* Consecutive NAK count */
    int max_consecutive_naks;         /* Max consecutive NAKs (10) */
    time_t transfer_timeout;          /* Global transfer timeout (60 seconds) */
    int consecutive_timeouts;         /* Consecutive timeout count */
    int max_consecutive_timeouts;     /* Max consecutive timeouts (5) */

    /* Progress reporting */
    int last_progress_percent;        /* Last reported progress percentage (-1 = none) */

    /* Buffers */
    UCHAR input_buffer[KERMIT_FILE_BUFFER_SIZE];   /* File input buffer */
    UCHAR output_buffer[KERMIT_FILE_BUFFER_SIZE];  /* File output buffer */
} kermit_client_ctx_t;

/*
 * High-level API - Call these from transfer.c
 */

/**
 * Send a file via Kermit protocol
 *
 * @param socket_fd       Telnet socket file descriptor
 * @param telnet_ctx      Telnet context (for BINARY mode status)
 * @param transfer_state  Transfer state (for progress tracking, may be NULL)
 * @param filename        Absolute path to file to send
 * @param otelnet_ctx     Otelnet context (for disabling auto-detection during transfer)
 * @return SUCCESS on success, ERROR_* on failure
 */
int kermit_client_send(
    int socket_fd,
    telnet_t *telnet_ctx,
    transfer_state_t *transfer_state,
    const char *filename,
    struct otelnet_ctx *otelnet_ctx
);

/**
 * Receive a file via Kermit protocol
 *
 * @param socket_fd       Telnet socket file descriptor
 * @param telnet_ctx      Telnet context (for BINARY mode status)
 * @param transfer_state  Transfer state (for progress tracking, may be NULL)
 * @param otelnet_ctx     Otelnet context (for disabling auto-detection during transfer)
 * @return SUCCESS on success, ERROR_* on failure
 */
int kermit_client_receive(
    int socket_fd,
    telnet_t *telnet_ctx,
    transfer_state_t *transfer_state,
    struct otelnet_ctx *otelnet_ctx
);

/*
 * I/O Callbacks for ekermit (internal use only)
 * These are registered with ekermit's k_data structure
 */

/**
 * Read packet from telnet socket
 *
 * @param k    Kermit data structure
 * @param buf  Buffer to read into
 * @param len  Maximum bytes to read
 * @return Number of bytes read, 0 on timeout, -1 on fatal error
 */
int otelnet_kermit_rxd(struct k_data *k, UCHAR *buf, int len);

/**
 * Write packet to telnet socket
 *
 * @param k    Kermit data structure
 * @param buf  Buffer to write from
 * @param len  Number of bytes to write
 * @return Number of bytes written, -1 on error
 */
int otelnet_kermit_txd(struct k_data *k, UCHAR *buf, int len);

/**
 * Check if input is available on socket (for sliding windows)
 *
 * @param k    Kermit data structure
 * @return 1 if data available, 0 if not, -1 on error
 */
int otelnet_kermit_ixd(struct k_data *k);

/**
 * Open file for reading (send) or writing (receive)
 *
 * @param k     Kermit data structure
 * @param name  Filename to open
 * @param mode  1 = read (send), 2 = write (receive)
 * @return 0 on success, -1 on error
 */
int otelnet_kermit_openf(struct k_data *k, UCHAR *name, int mode);

/**
 * Read data from file (for sending)
 *
 * @param k    Kermit data structure
 * @return Number of bytes read, 0 on EOF, -1 on error
 */
int otelnet_kermit_readf(struct k_data *k);

/**
 * Write data to file (for receiving)
 *
 * @param k    Kermit data structure
 * @param buf  Buffer containing data to write
 * @param len  Number of bytes to write
 * @return Number of bytes written, -1 on error
 */
int otelnet_kermit_writef(struct k_data *k, UCHAR *buf, int len);

/**
 * Close currently open file
 *
 * @param k        Kermit data structure
 * @param status   Transfer status (0 = success, non-zero = error)
 * @param discard  1 = delete file (failed receive), 0 = keep
 * @return 0 on success, -1 on error
 */
int otelnet_kermit_closef(struct k_data *k, UCHAR status, int discard);

/**
 * Get file information (size, date, type)
 *
 * @param k        Kermit data structure
 * @param name     Filename to query
 * @param buf      Buffer for date string (output, may be NULL)
 * @param buflen   Size of date buffer
 * @param type     File type output: 1=binary, 0=text (may be NULL)
 * @param xmode    Transfer mode (unused)
 * @return File size in bytes, 0 on error
 */
ULONG otelnet_kermit_finfo(struct k_data *k, UCHAR *name, UCHAR *buf,
                           int buflen, short *type, short xmode);

/**
 * Debug callback (optional, only if DEBUG defined)
 *
 * @param fc     Function code (DB_OPN, DB_MSG, DB_LOG, etc.)
 * @param label  Label string
 * @param sval   String value (or NULL)
 * @param nval   Numeric value
 * @return 0
 */
#ifdef DEBUG
int otelnet_kermit_debug(int fc, UCHAR *label, UCHAR *sval, long nval);
#endif

#endif /* OTELNET_KERMIT_CLIENT_H */
