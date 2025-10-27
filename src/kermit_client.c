/*
 * kermit_client.c - Embedded Kermit integration for otelnet client
 *
 * Implements in-process Kermit file transfer using ekermit library.
 * Replaces external kermit program execution with direct API calls.
 */

#include <stddef.h>   /* For NULL */
#include "otelnet.h"  /* For otelnet_ctx_t and detector functions - must come first */
#include "kermit_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <syslog.h>

/* Include ekermit debug header for debug constants */
#ifdef DEBUG
#include "debug.h"
#endif

/**
 * Get current timestamp string in format [YYYY-MM-DD HH:MM:SS]
 * Returns pointer to internal static buffer
 */
static const char *kermit_get_timestamp(void)
{
    static char timestamp[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    return timestamp;
}


/**
 * Kermit character encoding/decoding (from ekermit)
 * tochar: number to printable character (add 32)
 * xunchar: printable character to number (subtract 32)
 * Note: SP is already defined in ekermit/kermit.h as 040 (octal) = 32 (decimal)
 */
#define kermit_tochar(ch)   ((unsigned char)(((ch) + SP) & 0xFF))
#define kermit_xunchar(ch)  ((unsigned char)(((ch) - SP) & 0xFF))

#ifdef DEBUG
/**
 * Calculate Kermit Type 1 checksum (6-bit checksum)
 * Same algorithm as ekermit's chk1() function
 *
 * NOTE: This function is only used for DEBUG packet analysis.
 * When bct=3, ekermit internally uses CRC-16 (chk3() function) automatically.
 * We keep this Type 1 implementation for backward compatibility with debug logs.
 */
static unsigned char kermit_calc_checksum(const unsigned char *pkt, size_t len)
{
    unsigned int sum = 0;

    /* Sum all bytes (Type 2 checksum - 12 bit) */
    for (size_t i = 0; i < len; i++) {
        sum += pkt[i];
    }

    /* Fold upper 2 bits into lower 6 bits (Type 1 checksum) */
    sum = (((sum & 0300) >> 6) + sum) & 077;

    return (unsigned char)sum;
}

/**
 * Dump packet contents in hex and ASCII for debugging
 */
static void kermit_dump_packet(const char *label, const unsigned char *pkt, int len)
{
    char hex_buf[256];
    char ascii_buf[64];
    int pos = 0;
    int apos = 0;

    printf("[%s][KERMIT-DEBUG] %s (%d bytes):\r\n", kermit_get_timestamp(), label, len);
    printf("[%s][KERMIT-DEBUG]   Hex: ", kermit_get_timestamp());

    for (int i = 0; i < len && i < 80; i++) {
        pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02X ", pkt[i]);

        /* ASCII representation */
        if (pkt[i] >= 32 && pkt[i] < 127) {
            apos += snprintf(ascii_buf + apos, sizeof(ascii_buf) - apos, "%c", pkt[i]);
        } else {
            apos += snprintf(ascii_buf + apos, sizeof(ascii_buf) - apos, ".");
        }
    }

    printf("%s\r\n", hex_buf);
    printf("[%s][KERMIT-DEBUG]   ASCII: [%s]\r\n", kermit_get_timestamp(), ascii_buf);
    fflush(stdout);
}

/**
 * Analyze and log Kermit packet structure in detail
 */
static void kermit_analyze_packet(const unsigned char *pkt, int len)
{
    if (len < 5) {
        printf("[%s][KERMIT-DEBUG] Packet too short (%d bytes)\r\n", kermit_get_timestamp(), len);
        return;
    }

    /* Dump raw packet */
    kermit_dump_packet("Received packet", pkt, len);

    /* Parse packet structure */
    unsigned char mark = pkt[0];
    unsigned char len_field = pkt[1];
    unsigned char seq_field = pkt[2];
    unsigned char type = pkt[3];

    int packet_len = kermit_xunchar(len_field);
    int seq_num = kermit_xunchar(seq_field);

    printf("[%s][KERMIT-DEBUG] Packet structure:\r\n", kermit_get_timestamp());
    printf("[%s][KERMIT-DEBUG]   MARK: 0x%02X (%s)\r\n",
            kermit_get_timestamp(), mark, mark == 0x01 ? "SOH - OK" : "INVALID");
    printf("[%s][KERMIT-DEBUG]   LEN:  0x%02X ('%c') = %d bytes\r\n",
            kermit_get_timestamp(), len_field, len_field, packet_len);
    printf("[%s][KERMIT-DEBUG]   SEQ:  0x%02X ('%c') = sequence %d\r\n",
            kermit_get_timestamp(), seq_field, seq_field, seq_num);
    printf("[%s][KERMIT-DEBUG]   TYPE: 0x%02X ('%c')\r\n", kermit_get_timestamp(), type, type);

    /* Determine check field length based on packet type
     * Per KERMIT_PACKET_REPORT_1.md and PROCESS_REPORT_1_02_RECEIVER.md:
     * - S/Y packets: Type 1 checksum (1 byte) for backward compatibility
     * - F/A/D/Z/B/N/E packets: Type 3 CRC-16 (3 bytes) for strong error detection */
    int check_len;
    if (type == 'S' || type == 'Y') {
        check_len = 1;  /* Type 1: Single-byte 6-bit checksum */
    } else {
        check_len = 3;  /* Type 3: Three-byte CRC-CCITT (CRC-16) */
    }

    /* Data field length = packet_len - 2 (SEQ + TYPE) - check_len */
    int data_len = packet_len - 2 - check_len;

    if (data_len > 0 && len >= 4 + data_len) {
        printf("[%s][KERMIT-DEBUG]   DATA: %d bytes: ", kermit_get_timestamp(), data_len);
        for (int i = 0; i < data_len && i < 20; i++) {
            printf("%02X ", pkt[4 + i]);
        }
        if (data_len > 20) fprintf(stderr, "...");
        printf("\r\n");

        /* Show ASCII representation of data */
        printf("[%s][KERMIT-DEBUG]   DATA (ASCII): ", kermit_get_timestamp());
        for (int i = 0; i < data_len && i < 40; i++) {
            unsigned char c = pkt[4 + i];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("\r\n");
    }

    /* Check field validation based on type */
    int check_pos = 4 + data_len;
    if (len > check_pos + check_len - 1) {
        if (check_len == 1) {
            /* Type 1 Checksum (S/Y packets) */
            unsigned char check_received = pkt[check_pos];
            unsigned char check_value = kermit_xunchar(check_received);

            printf("[%s][KERMIT-DEBUG]   CHECK TYPE: 1 (6-bit checksum, 1 byte)\r\n", kermit_get_timestamp());
            printf("[%s][KERMIT-DEBUG]   CHECK: 0x%02X ('%c') = value %d (0x%02X)\r\n",
                    kermit_get_timestamp(), check_received, check_received, check_value, check_value);

            /* Calculate expected Type 1 checksum
             * Checksum is calculated over: LEN + SEQ + TYPE + DATA
             * LEN field value = SEQ + TYPE + DATA + CHECK (does not include LEN itself)
             * Checksum range = LEN byte itself + (LEN field value - CHECK)
             *                = 1 + (packet_len - 1) = packet_len bytes total */
            unsigned char calc_chk = kermit_calc_checksum(pkt + 1, packet_len);
            unsigned char calc_chk_encoded = kermit_tochar(calc_chk);

            printf("[%s][KERMIT-DEBUG]   CHECKSUM VERIFICATION:\r\n", kermit_get_timestamp());
            printf("[%s][KERMIT-DEBUG]     Calculated: 0x%02X (encoded: 0x%02X '%c')\r\n",
                    kermit_get_timestamp(), calc_chk, calc_chk_encoded, calc_chk_encoded);
            printf("[%s][KERMIT-DEBUG]     Received:   0x%02X (encoded: 0x%02X '%c')\r\n",
                    kermit_get_timestamp(), check_value, check_received, check_received);
            printf("[%s][KERMIT-DEBUG]     Match: %s\r\n",
                    kermit_get_timestamp(), (check_received == calc_chk_encoded) ? "YES" : "NO - MISMATCH!");

            if (check_received != calc_chk_encoded) {
                printf("[%s][KERMIT-DEBUG]     WARNING: Checksum mismatch detected!\r\n", kermit_get_timestamp());
            }
        } else {
            /* Type 3 CRC-16 (F/A/D/Z/B/N/E packets) */
            printf("[%s][KERMIT-DEBUG]   CHECK TYPE: 3 (CRC-16, 3 bytes)\r\n", kermit_get_timestamp());
            printf("[%s][KERMIT-DEBUG]   CRC-16: ", kermit_get_timestamp());
            for (int i = 0; i < 3; i++) {
                printf("%02X ", pkt[check_pos + i]);
            }
            printf("(");
            for (int i = 0; i < 3; i++) {
                unsigned char c = pkt[check_pos + i];
                printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
            printf(")\r\n");
            printf("[%s][KERMIT-DEBUG]   Note: CRC-16 validation handled by ekermit internally (chk3)\r\n", kermit_get_timestamp());
            printf("[%s][KERMIT-DEBUG]         Polynomial: 0x1021 (CRC-CCITT), 99.998%% error detection\r\n", kermit_get_timestamp());
        }
    }

    /* CR terminator (after check field) */
    int terminator_pos = check_pos + check_len;
    if (len > terminator_pos) {
        unsigned char terminator = pkt[terminator_pos];
        printf("[%s][KERMIT-DEBUG]   TERMINATOR: 0x%02X (%s) at position %d\r\n",
                kermit_get_timestamp(), terminator, terminator == 0x0D ? "CR - OK" : "UNEXPECTED", terminator_pos);

        /* Check for extra bytes after CR */
        if (len > terminator_pos + 1) {
            printf("[%s][KERMIT-DEBUG]   WARNING: %d extra bytes after CR: ",
                    kermit_get_timestamp(), len - terminator_pos - 1);
            for (int i = terminator_pos + 1; i < len && i < terminator_pos + 9; i++) {
                printf("%02X ", pkt[i]);
            }
            printf("\r\n");
            printf("[%s][KERMIT-DEBUG]   (This could indicate LINEMODE CR->CRLF conversion!)\r\n", kermit_get_timestamp());
        }
    }

    printf("[%s][KERMIT-DEBUG] ----------------------------------------\r\n", kermit_get_timestamp());
    fflush(stdout);
}
#endif  /* DEBUG */

/**
 * Re-enable auto-detection after Kermit transfer completes
 * This must be called before returning from kermit_client_send/receive
 */
static inline void kermit_restore_auto_detection(struct otelnet_ctx *otelnet_ctx,
                                                  bool was_enabled)
{
    if (otelnet_ctx != NULL && was_enabled) {
        zmodem_detector_set_enabled(&otelnet_ctx->zmodem_detector, true);
        xmodem_detector_set_enabled(&otelnet_ctx->xmodem_detector, true);
        ymodem_detector_set_enabled(&otelnet_ctx->ymodem_detector, true);
        printf("[%s][INFO] Auto-detection re-enabled after Kermit transfer\r\n", kermit_get_timestamp()); fflush(stdout);
    }
}

/*
 * Static k_data structures for send and receive operations
 * These are allocated in BSS segment (not stack) and automatically zero-initialized.
 * k_data is very large (~135KB) and should not be stack-allocated.
 */
static struct k_data k_send;
static struct k_data k_recv;

/*
 * Static context pointers for send and receive operations
 * IMPORTANT: We store ctx pointers here instead of in k->istring because
 * ekermit uses k->istring for storing filenames, which would overwrite our pointer!
 * This was causing segfaults when txd() tried to access ctx after filename was set.
 */
static kermit_client_ctx_t *g_send_ctx = NULL;
static kermit_client_ctx_t *g_recv_ctx = NULL;

/*
 * Get client context from k_data
 * Returns the appropriate global ctx pointer based on which k_data struct is being used
 */
static inline kermit_client_ctx_t *get_ctx(struct k_data *k) {
    if (k == &k_send) {
        return g_send_ctx;
    } else if (k == &k_recv) {
        return g_recv_ctx;
    }
    /* This should never happen */
    fprintf(stderr, "[%s][ERROR] %s:%d: get_ctx() called with unknown k_data pointer!\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
    return NULL;
}

/*
 * Binary mode IAC escaping for file transfers
 * In BINARY mode, ONLY escape 0xFF bytes (0xFF → 0xFF 0xFF)
 * Do NOT process other telnet protocol commands
 */
static size_t binary_mode_escape(const unsigned char *input, size_t input_len,
                                 unsigned char *output, size_t output_size) {
    size_t out_pos = 0;

    for (size_t i = 0; i < input_len; i++) {
        if (input[i] == TELNET_IAC) {
            /* Escape IAC byte by doubling it */
            if (out_pos + 2 > output_size) {
                fprintf(stderr, "[%s][ERROR] %s:%d: Binary escape buffer overflow\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
                return 0;
            }
            output[out_pos++] = TELNET_IAC;
            output[out_pos++] = TELNET_IAC;
        } else {
            /* Pass through all other bytes unchanged */
            if (out_pos + 1 > output_size) {
                fprintf(stderr, "[%s][ERROR] %s:%d: Binary escape buffer overflow\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
                return 0;
            }
            output[out_pos++] = input[i];
        }
    }

    return out_pos;
}

/*
 * Binary mode IAC unescaping for file transfers
 * In BINARY mode, ONLY unescape doubled IAC (0xFF 0xFF → 0xFF)
 * Detect telnet protocol commands (IAC followed by non-IAC) as mode change signal
 * Returns: number of decoded bytes, or (size_t)-1 if telnet command detected
 */
static size_t binary_mode_unescape(const unsigned char *input, size_t input_len,
                                   unsigned char *output, size_t output_size) {
    size_t out_pos = 0;
    size_t i = 0;

    while (i < input_len) {
        if (input[i] == TELNET_IAC) {
            if (i + 1 < input_len) {
                if (input[i + 1] == TELNET_IAC) {
                    /* Doubled IAC (0xFF 0xFF) - output single IAC */
                    if (out_pos + 1 > output_size) {
                        fprintf(stderr, "[%s][ERROR] %s:%d: Binary unescape buffer overflow\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
                        return 0;
                    }
                    output[out_pos++] = TELNET_IAC;
                    i += 2;  /* Skip both IAC bytes */
                } else if ((input[i + 1] == TELNET_WILL || input[i + 1] == TELNET_DO ||
                           input[i + 1] == TELNET_WONT || input[i + 1] == TELNET_DONT) &&
                           i + 2 < input_len && input[i + 2] == TELOPT_BINARY) {
                    /* BINARY mode negotiation packets (IAC WILL/DO/WONT/DONT BINARY)
                     * These can arrive at the start of transfer - silently skip them */
                    #ifdef DEBUG
    printf("[DEBUG] %s:%d: Skipping BINARY mode negotiation: IAC 0x%02X 0x%02X\r\n", __FILE__, __LINE__, input[i + 1], input[i + 2]); fflush(stdout);
#endif
                    i += 3;  /* Skip IAC + command + option */
                } else {
                    /* Other IAC commands = telnet protocol command
                     * This indicates server has exited BINARY mode
                     * Signal to caller that transfer should be aborted */
                    fprintf(stderr, "[%s][WARNING] Detected telnet protocol command during transfer (IAC 0x%02X), aborting\r\n", kermit_get_timestamp(), input[i + 1]);
                    return (size_t)-1;  /* Special return value */
                }
            } else {
                /* IAC at end of buffer - incomplete, pass through for now */
                if (out_pos + 1 > output_size) {
                    fprintf(stderr, "[%s][ERROR] %s:%d: Binary unescape buffer overflow\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
                    return 0;
                }
                output[out_pos++] = input[i];
                i++;
            }
        } else {
            /* Pass through all other bytes unchanged */
            if (out_pos + 1 > output_size) {
                fprintf(stderr, "[%s][ERROR] %s:%d: Binary unescape buffer overflow\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
                return 0;
            }
            output[out_pos++] = input[i];
            i++;
        }
    }

    return out_pos;
}

/*
 * xerror() - Required by ekermit for error injection simulation
 * We provide a stub implementation since we don't use this feature
 * Note: ekermit always references this, not just in DEBUG mode
 */
int xerror(void) {
    return 0;  /* No simulated errors */
}

/*
 * Communication I/O Callbacks
 */

int otelnet_kermit_rxd(struct k_data *k, UCHAR *buf, int len) {
    kermit_client_ctx_t *ctx = get_ctx(k);
    /* Use P_PKTLEN * 2 (Kermit packet size with IAC escaping)
     * P_PKTLEN = 94 bytes with -DNO_LP, doubled for IAC escaping = 188 bytes
     * This matches server implementation and is memory-efficient */
    unsigned char raw_buf[P_PKTLEN * 2];
    int nread = 0;

    /* PRIORITY: Check pending buffer first (data from drain process)
     * This data was received during BINARY mode negotiation and saved
     * to prevent loss. It must be returned before reading new data. */
    if (ctx->otelnet_ctx && ctx->otelnet_ctx->pending_data_len > 0) {
        size_t copy_len = MIN(ctx->otelnet_ctx->pending_data_len, (size_t)len);

        memcpy(buf, ctx->otelnet_ctx->pending_data, copy_len);
        printf("[%s][INFO] Returning %zu bytes from pending buffer (remaining: %zu)\r\n", kermit_get_timestamp(), copy_len, ctx->otelnet_ctx->pending_data_len - copy_len); fflush(stdout);

        /* Pending data is already processed (SOH removed by telnet_process_input)
         * But we still need to remove SOH if present (defensive programming) */
        if (copy_len > 0 && buf[0] == SOH) {
            memmove(buf, buf + 1, copy_len - 1);
            copy_len--;
            #ifdef DEBUG
    printf("[DEBUG] %s:%d: Removed SOH from pending data\r\n", __FILE__, __LINE__); fflush(stdout);
#endif
        }

        /* Sanity check: verify this looks like a valid Kermit packet
         * This is a defensive check - should never fail if drain logic is correct */
        if (copy_len >= 4) {
            unsigned char len_field = buf[0];
            unsigned char seq_field = buf[1];
            unsigned char type_field = buf[2];

            /* Valid LEN field: (SP + 3) to (SP + 94) = 35 to 126 */
            if (len_field < 35 || len_field > 126) {
                fprintf(stderr, "[%s][ERROR] %s:%d: Invalid Kermit packet in pending buffer: LEN=0x%02X (expected 35-126)\r\n", kermit_get_timestamp(), __FILE__, __LINE__, len_field);

                /* Log hex dump for debugging */
                char hex_dump[128];
                int offset = 0;
                size_t dump_len = (copy_len < 32 ? copy_len : 32);
                for (size_t i = 0; i < dump_len; i++) {
                    offset += snprintf(hex_dump + offset, sizeof(hex_dump) - offset, "%02X ", buf[i]);
                }
                fprintf(stderr, "[%s][ERROR] %s:%d: Discarding invalid data from pending buffer: %s\r\n", kermit_get_timestamp(), __FILE__, __LINE__, hex_dump);

                /* Clear entire pending buffer to avoid cascading errors */
                ctx->otelnet_ctx->pending_data_len = 0;
                fprintf(stderr, "[%s][WARNING] Cleared pending buffer, will retry with normal socket read\r\n", kermit_get_timestamp());

                /* Return 0 to trigger timeout in Kermit, which will cause NAK/retry */
                ctx->last_activity = time(NULL);
                return 0;
            }

            /* Additional checks for SEQ and TYPE fields (should be printable) */
            if (seq_field < 32 || seq_field > 126) {
                fprintf(stderr, "[%s][WARNING] Suspicious SEQ field in pending buffer: 0x%02X (expected 32-126)\r\n", kermit_get_timestamp(), seq_field);
            }
            if (type_field < 32 || type_field > 126) {
                fprintf(stderr, "[%s][WARNING] Suspicious TYPE field in pending buffer: 0x%02X (expected 32-126)\r\n", kermit_get_timestamp(), type_field);
            }
        }

        /* Shift remaining data if any */
        if (copy_len < ctx->otelnet_ctx->pending_data_len) {
            memmove(ctx->otelnet_ctx->pending_data,
                    ctx->otelnet_ctx->pending_data + copy_len,
                    ctx->otelnet_ctx->pending_data_len - copy_len);
            ctx->otelnet_ctx->pending_data_len -= copy_len;
        } else {
            /* All pending data consumed */
            ctx->otelnet_ctx->pending_data_len = 0;
        }

        ctx->last_activity = time(NULL);
        return (int)copy_len;
    }

    /* No pending data - read from socket with timeout */
    fd_set readfds;
    struct timeval tv;
    tv.tv_sec = k->r_timo;  /* Use Kermit's timeout setting */
    tv.tv_usec = 0;

    FD_ZERO(&readfds);
    FD_SET(ctx->socket_fd, &readfds);

    int ret = select(ctx->socket_fd + 1, &readfds, NULL, NULL, &tv);

    /* Log select result */
    #ifdef DEBUG
    printf("[DEBUG] %s:%d: select() returned: %d (timeout=%d sec)\r\n", __FILE__, __LINE__, ret, k->r_timo); fflush(stdout);
#endif

    if (ret < 0) {
        if (errno == EINTR) {
            #ifdef DEBUG
    printf("[DEBUG] %s:%d: select() interrupted by signal\r\n", __FILE__, __LINE__); fflush(stdout);
#endif
            return 0;  /* Retry */
        }
        fprintf(stderr, "[%s][ERROR] %s:%d: select() failed: %s\r\n", kermit_get_timestamp(), __FILE__, __LINE__, strerror(errno));
        ctx->last_errno = errno;
        return -1;  /* Fatal error */
    }
    if (ret == 0) {
        /* Timeout - not fatal, let Kermit retry */
        time_t elapsed = time(NULL) - ctx->last_activity;
        printf("[%s][KERMIT-TIMEOUT] No data for %d seconds (last activity: %ld sec ago)\r\n",
               kermit_get_timestamp(), k->r_timo, elapsed);
        fflush(stdout);
        #ifdef DEBUG
    printf("[DEBUG] %s:%d: Timeout waiting for data (%d seconds)\r\n", __FILE__, __LINE__, k->r_timo); fflush(stdout);
#endif
        return 0;
    }

    /* Data available - read raw data from socket */
    #ifdef DEBUG
    printf("[DEBUG] %s:%d: About to read from socket (fd=%d)\r\n", __FILE__, __LINE__, ctx->socket_fd); fflush(stdout);
#endif
    nread = read(ctx->socket_fd, raw_buf, sizeof(raw_buf));
    #ifdef DEBUG
    printf("[DEBUG] %s:%d: read() returned: %d\r\n", __FILE__, __LINE__, nread); fflush(stdout);
#endif

    if (nread < 0) {
        printf("[%s][KERMIT-ERROR] read() failed: errno=%d (%s)\r\n",
               kermit_get_timestamp(), errno, strerror(errno));
        fflush(stdout);
        if (errno == EINTR || errno == EAGAIN) {
            #ifdef DEBUG
    printf("[DEBUG] %s:%d: read() interrupted or would block\r\n", __FILE__, __LINE__); fflush(stdout);
#endif
            return 0;  /* Retry */
        }
        fprintf(stderr, "[%s][ERROR] %s:%d: read() from socket failed: %s\r\n", kermit_get_timestamp(), __FILE__, __LINE__, strerror(errno));
        ctx->last_errno = errno;
        return -1;
    }
    if (nread == 0) {
        printf("[%s][KERMIT-ERROR] Socket EOF detected - server closed connection\r\n",
               kermit_get_timestamp());
        fflush(stdout);
        fprintf(stderr, "[%s][WARNING] Connection closed by remote peer\r\n", kermit_get_timestamp());
        return -1;  /* Connection closed */
    }

    /* Log read data as hex dump */
    if (nread > 0) {
        char hex_str[256];
        int hex_len = nread > 32 ? 32 : nread;
        for (int i = 0; i < hex_len; i++) {
            sprintf(hex_str + i*3, "%02X ", raw_buf[i]);
        }
        #ifdef DEBUG
    printf("[DEBUG] %s:%d: READ %d bytes: %s%s\r\n", __FILE__, __LINE__, nread, hex_str, nread > 32 ? "..." : ""); fflush(stdout);
#endif
    }

    /* CRITICAL FIX: Use temporary buffer for unescape operation
     * Problem: ekermit's buffer (len) may be 94 bytes (standard packet size),
     * but raw packet with SOH+EOM can be 96 bytes. This causes overflow.
     * Solution: Use temporary buffer, then copy after removing SOH/EOM.
     * P_PKTLEN * 2 = 188 bytes is sufficient for any Kermit packet with IAC escaping.
     */
    unsigned char temp_buf[P_PKTLEN * 2];  /* Large enough for any packet */

    /* BINARY mode: only unescape doubled IAC bytes (0xFF 0xFF → 0xFF)
     * Also detect telnet protocol commands as mode change signal */
    size_t decoded_len = binary_mode_unescape(raw_buf, nread, temp_buf, sizeof(temp_buf));
    if (decoded_len == (size_t)-1) {
        /* Telnet protocol command detected - server exited BINARY mode */
        fprintf(stderr, "[%s][ERROR] %s:%d: Server exited BINARY mode during transfer, aborting\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
        return -1;  /* Signal fatal error to ekermit */
    }
    if (decoded_len == 0 && nread > 0) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Binary mode unescape failed\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
        return -1;
    }

    /* Log unescaped data if different from raw data */
    if (decoded_len > 0 && decoded_len != (size_t)nread) {
        char dec_hex[256];
        int dec_len = decoded_len > 32 ? 32 : decoded_len;
        for (int i = 0; i < dec_len; i++) {
            sprintf(dec_hex + i*3, "%02X ", temp_buf[i]);
        }
        #ifdef DEBUG
    printf("[DEBUG] %s:%d: UNESCAPED to %zu bytes: %s%s\r\n", __FILE__, __LINE__, decoded_len, dec_hex, decoded_len > 32 ? "..." : ""); fflush(stdout);
#endif
    }

    ctx->last_activity = time(NULL);

    /* CRITICAL: ekermit expects data WITHOUT SOH and EOM bytes!
     * ekermit's standard readpkt() function (unixio.c:247-298):
     *   - Discards SOH at line 290 (continue after detecting SOH)
     *   - Does NOT store EOM, returns when EOM detected (line 298)
     * We must do the same: remove SOH from beginning and EOM from end.
     *
     * Kermit packet on wire:  SOH + LEN + SEQ + TYPE + DATA + CHECK + EOM
     * Expected by ekermit:           LEN + SEQ + TYPE + DATA + CHECK
     */
    if (decoded_len > 0 && temp_buf[0] == SOH) {
#ifdef DEBUG
        /* Analyze packet BEFORE removing SOH (complete packet structure) */
        printf("\n[%s][KERMIT-RXD] ========================================\r\n", kermit_get_timestamp());
        printf("[%s][KERMIT-RXD] Raw packet received (before SOH removal)\r\n", kermit_get_timestamp());
        printf("[%s][KERMIT-RXD] ========================================\r\n", kermit_get_timestamp());
        kermit_analyze_packet(temp_buf, decoded_len);
        printf("[%s][KERMIT-RXD] ========================================\n\r\n", kermit_get_timestamp());
        fflush(stdout);
#endif

        /* Remove SOH byte by shifting data left */
        memmove(temp_buf, temp_buf + 1, decoded_len - 1);
        decoded_len--;
        #ifdef DEBUG
    printf("[DEBUG] %s:%d: Removed SOH byte, packet now %zu bytes\r\n", __FILE__, __LINE__, decoded_len); fflush(stdout);
#endif
    }

    /* Remove EOM byte (CR or LF) - ekermit's readpkt() discards it
     * ekermit/unixio.c:readpkt() returns packet WITHOUT SOH and EOM:
     *   - SOH: discarded at line 290 (continue after detecting SOH)
     *   - EOM: NOT stored, function returns when EOM detected (line 298)
     * Therefore we must also remove the trailing EOM byte. */
    if (decoded_len > 0 && (temp_buf[decoded_len - 1] == 0x0D || temp_buf[decoded_len - 1] == 0x0A)) {
        #ifdef DEBUG
    printf("[DEBUG] %s:%d: Removing EOM byte 0x%02X at position %zu\r\n", __FILE__, __LINE__, temp_buf[decoded_len - 1], decoded_len - 1); fflush(stdout);
#endif
        decoded_len--;
        #ifdef DEBUG
    printf("[DEBUG] %s:%d: Removed EOM byte, packet now %zu bytes\r\n", __FILE__, __LINE__, decoded_len); fflush(stdout);
#endif
    }

    /* Now check if the final packet (after SOH/EOM removal) fits in ekermit's buffer
     * IMPORTANT: ekermit's actual buffer size is P_PKTLEN+8 (see kermit.h:353)
     * getrslot() returns ipktbuf[P_PKTLEN+8], not ipktbuf[P_PKTLEN]
     * The 'len' parameter is P_PKTLEN (94), but actual buffer is P_PKTLEN+8 (102)
     * This allows for CRC-16 encoded checksum (3 bytes) in longer packets.
     */
    if (decoded_len > (size_t)(len + 8)) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Packet too large for ekermit buffer: %zu bytes > %d bytes (actual buffer: %d)\r\n",
                kermit_get_timestamp(), __FILE__, __LINE__, decoded_len, len, len + 8);
        return -1;
    }

    /* Copy to ekermit's buffer */
    memcpy(buf, temp_buf, decoded_len);

    /* Log to file (if logging enabled) */
    if (decoded_len > 0 && ctx->otelnet_ctx) {
        otelnet_log_data(ctx->otelnet_ctx, "KERMIT-RECEIVE", buf, decoded_len);
    }

#ifdef DEBUG
    /* Log raw and decoded data for debugging */
    char raw_hex[256], decoded_hex[256];
    int raw_len = nread > 32 ? 32 : nread;
    int dec_len = decoded_len > 32 ? 32 : decoded_len;

    for (int i = 0; i < raw_len; i++) {
        sprintf(raw_hex + i*3, "%02X ", raw_buf[i]);
    }
    for (int i = 0; i < dec_len; i++) {
        sprintf(decoded_hex + i*3, "%02X ", buf[i]);
    }

    #ifdef DEBUG
    printf("[DEBUG] %s:%d: RX: %d raw bytes: %s\r\n", __FILE__, __LINE__, nread, raw_hex); fflush(stdout);
#endif
    #ifdef DEBUG
    printf("[DEBUG] %s:%d: RX: %zu decoded bytes (SOH and EOM removed): %s\r\n", __FILE__, __LINE__, decoded_len, decoded_hex); fflush(stdout);
#endif
#else
    #ifdef DEBUG
    printf("[DEBUG] %s:%d: Received %d bytes from socket, unescaped to %zu bytes\r\n", __FILE__, __LINE__, nread, decoded_len); fflush(stdout);
#endif
#endif

    return (int)decoded_len;
}

int otelnet_kermit_txd(struct k_data *k, UCHAR *buf, int len) {
    kermit_client_ctx_t *ctx = get_ctx(k);
    /* Use P_PKTLEN (Kermit packet size) instead of BUFFER_SIZE
     * P_PKTLEN = 94 bytes with -DNO_LP, doubled for IAC escaping = 188 bytes
     * This matches server implementation and is memory-efficient */
    unsigned char encoded_buf[P_PKTLEN * 2];  /* IAC escaping may double size */
    size_t encoded_len = 0;
    int total_sent = 0;

    /* CRITICAL DEBUG: Log every txd() call to track packet transmission */
    printf("[%s][INFO] ===== TXD CALLED: transmitting %d bytes to server =====\r\n", kermit_get_timestamp(), len); fflush(stdout);

#ifdef DEBUG
    /* Analyze packet type to identify ACK/NAK/DATA packets */
    if (len >= 4) {
        unsigned char mark = buf[0];        /* SOH (0x01) */
        unsigned char pkt_len = buf[1];     /* LEN field */
        unsigned char pkt_seq = buf[2];     /* SEQ field */
        unsigned char pkt_type = buf[3];    /* TYPE field */

        printf("[%s][KERMIT-TXD] Packet details:\r\n", kermit_get_timestamp());
        printf("[%s][KERMIT-TXD]   MARK: 0x%02X (SOH)\r\n", kermit_get_timestamp(), mark);
        printf("[%s][KERMIT-TXD]   LEN: 0x%02X ('%c') = %d bytes\r\n",
                kermit_get_timestamp(), pkt_len, pkt_len, pkt_len - 32);
        printf("[%s][KERMIT-TXD]   SEQ: 0x%02X ('%c') = sequence %d\r\n",
                kermit_get_timestamp(), pkt_seq, pkt_seq, pkt_seq - 32);
        printf("[%s][KERMIT-TXD]   TYPE: 0x%02X ('%c')\r\n", kermit_get_timestamp(), pkt_type, pkt_type);

        /* Identify packet type */
        const char *type_name = "UNKNOWN";
        switch (pkt_type) {
            case 'S': type_name = "Send-Init"; break;
            case 'F': type_name = "File-Header"; break;
            case 'D': type_name = "Data"; break;
            case 'Z': type_name = "EOF"; break;
            case 'B': type_name = "Break"; break;
            case 'Y': type_name = "ACK"; break;
            case 'N': type_name = "NAK"; break;
            case 'E': type_name = "Error"; break;
        }
        printf("[%s][KERMIT-TXD]   Packet type: %s\r\n", kermit_get_timestamp(), type_name);

        /* Dump raw packet */
        printf("[%s][KERMIT-TXD]   Raw data: ", kermit_get_timestamp());
        for (int i = 0; i < len && i < 40; i++) {
            printf("%02X ", buf[i]);
        }
        if (len > 40) printf("...");
        printf("\r\n");
        fflush(stdout);
    }
#endif

    /* BINARY mode: only escape 0xFF bytes (0xFF → 0xFF 0xFF)
     * Do NOT add telnet protocol commands during file transfer */
    encoded_len = binary_mode_escape(buf, len, encoded_buf, sizeof(encoded_buf));
    if (encoded_len == 0 && len > 0) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Binary mode escape failed\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
        return -1;
    }

    #ifdef DEBUG
    printf("[DEBUG] %s:%d: About to write %zu bytes to socket_fd=%d (original: %d bytes)\r\n", __FILE__, __LINE__, encoded_len, ctx->socket_fd, len); fflush(stdout);
#endif

    /* Send escaped data to socket */
    while (total_sent < (int)encoded_len) {
        int sent = write(ctx->socket_fd, encoded_buf + total_sent,
                        encoded_len - total_sent);

        /* Log each write() result */
        printf("[%s][KERMIT-TXD] write() returned: %d (expected: %zu, total: %d/%zu)\r\n",
               kermit_get_timestamp(), sent, encoded_len - total_sent, total_sent + (sent > 0 ? sent : 0), encoded_len);
        fflush(stdout);
        #ifdef DEBUG
    printf("[DEBUG] %s:%d: write() returned: %d (total_sent: %d/%zu)\r\n", __FILE__, __LINE__, sent, total_sent + (sent > 0 ? sent : 0), encoded_len); fflush(stdout);
#endif

        if (sent < 0) {
            printf("[%s][KERMIT-ERROR] write() failed: errno=%d (%s)\r\n",
                   kermit_get_timestamp(), errno, strerror(errno));
            fflush(stdout);
            if (errno == EINTR) {
                #ifdef DEBUG
    printf("[DEBUG] %s:%d: write() interrupted by signal, retrying\r\n", __FILE__, __LINE__); fflush(stdout);
#endif
                continue;  /* Interrupted, retry */
            }
            if (errno == EAGAIN) {
                /* Socket buffer full, wait a bit */
                #ifdef DEBUG
    printf("[DEBUG] %s:%d: Socket buffer full (EAGAIN), waiting 10ms\r\n", __FILE__, __LINE__); fflush(stdout);
#endif
                usleep(10000);  /* 10ms */
                continue;
            }
            fprintf(stderr, "[%s][ERROR] %s:%d: write() to socket failed: %s (errno=%d)\r\n", kermit_get_timestamp(), __FILE__, __LINE__, strerror(errno), errno);
            printf("[%s][KERMIT-TXD] FATAL: write() failed with errno=%d (%s)\r\n",
                    kermit_get_timestamp(), errno, strerror(errno));
            fflush(stdout);
            ctx->last_errno = errno;
            return -1;
        }
        total_sent += sent;
    }

    printf("[%s][INFO] TXD SUCCESS: sent %d bytes total\r\n", kermit_get_timestamp(), total_sent); fflush(stdout);

    /* Log to file (if logging enabled) - log original packet data before escaping */
    if (len > 0 && ctx->otelnet_ctx) {
        otelnet_log_data(ctx->otelnet_ctx, "KERMIT-SEND", buf, len);
    }

    ctx->last_activity = time(NULL);

#ifdef DEBUG
    /* Log raw and encoded data for debugging */
    char raw_hex[256], encoded_hex[256];
    int raw_len = len > 32 ? 32 : len;
    int enc_len = encoded_len > 32 ? 32 : encoded_len;

    for (int i = 0; i < raw_len; i++) {
        sprintf(raw_hex + i*3, "%02X ", buf[i]);
    }
    for (int i = 0; i < enc_len; i++) {
        sprintf(encoded_hex + i*3, "%02X ", encoded_buf[i]);
    }

    #ifdef DEBUG
    printf("[DEBUG] %s:%d: TX: %d raw bytes: %s\r\n", __FILE__, __LINE__, len, raw_hex); fflush(stdout);
#endif
    #ifdef DEBUG
    printf("[DEBUG] %s:%d: TX: %zu escaped bytes: %s\r\n", __FILE__, __LINE__, encoded_len, encoded_hex); fflush(stdout);
#endif
#else
    #ifdef DEBUG
    printf("[DEBUG] %s:%d: Sent %d bytes (escaped to %zu bytes) to socket\r\n", __FILE__, __LINE__, len, encoded_len); fflush(stdout);
#endif
#endif

    return X_OK;  /* ekermit expects X_OK (0) on success, not byte count */
}

int otelnet_kermit_ixd(struct k_data *k) {
    /* Check if input available (for sliding windows) */
    kermit_client_ctx_t *ctx = get_ctx(k);

    fd_set readfds;
    struct timeval tv = {0, 0};  /* Non-blocking */

    FD_ZERO(&readfds);
    FD_SET(ctx->socket_fd, &readfds);

    int ret = select(ctx->socket_fd + 1, &readfds, NULL, NULL, &tv);
    if (ret > 0) {
        #ifdef DEBUG
    printf("[DEBUG] %s:%d: Input available on socket\r\n", __FILE__, __LINE__); fflush(stdout);
#endif
        return 1;  /* Data available */
    }

    return 0;  /* No data available */
}

/*
 * File I/O Callbacks
 */

int otelnet_kermit_openf(struct k_data *k, UCHAR *name, int mode) {
    kermit_client_ctx_t *ctx = get_ctx(k);

    /* mode: 1 = read (send), 2 = write (receive) */
    const char *fmode = (mode == 1) ? "rb" : "wb";
    const char *filepath_to_open;

    /* Close any existing file */
    if (ctx->file) {
        fclose(ctx->file);
        ctx->file = NULL;
    }

    /* For send mode (mode=1): use absolute path from context
     * For receive mode (mode=2): use basename from protocol */
    if (mode == 1 && ctx->send_file_absolute_path[0] != '\0') {
        filepath_to_open = ctx->send_file_absolute_path;
        printf("[%s][INFO] Send mode: using absolute path: %s\r\n", kermit_get_timestamp(), filepath_to_open); fflush(stdout);
    } else {
        filepath_to_open = (char *)name;
        printf("[%s][INFO] Receive mode: using basename: %s\r\n", kermit_get_timestamp(), filepath_to_open); fflush(stdout);
    }

    /* Open file */
    ctx->file = fopen(filepath_to_open, fmode);
    if (!ctx->file) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Failed to open file '%s' (mode=%s): %s\r\n", kermit_get_timestamp(), __FILE__, __LINE__, filepath_to_open, fmode, strerror(errno));
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "Cannot open file: %s", strerror(errno));
        ctx->last_errno = errno;
        return -1;
    }

    strncpy(ctx->current_filename, (char *)name, sizeof(ctx->current_filename) - 1);
    ctx->current_filename[sizeof(ctx->current_filename) - 1] = '\0';

    printf("[%s][INFO] Opened file: %s (mode=%s)\r\n", kermit_get_timestamp(), filepath_to_open, fmode); fflush(stdout);

    return 0;  /* Success */
}

int otelnet_kermit_readf(struct k_data *k) {
    kermit_client_ctx_t *ctx = get_ctx(k);

    if (!ctx->file) {
        fprintf(stderr, "[%s][ERROR] %s:%d: No file open for reading\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "No file open");
        return -1;
    }

    /* CRITICAL: Reset zinptr to start of buffer before reading
     * ekermit's getpkt() advances zinptr and decrements zincnt as it consumes data.
     * When zincnt reaches 0, readf is called again to refill the buffer.
     * We MUST reset zinptr to zinbuf before fread(), otherwise zinptr will be
     * pointing past the end of the buffer after ~10 reads, causing segfault.
     *
     * Example without reset:
     *   1st read: zinptr = zinbuf + 0    → OK
     *   2nd read: zinptr = zinbuf + 8192 → OK (but wrong!)
     *   11th read: zinptr = zinbuf + 81920 → SEGFAULT (out of bounds!)
     */
    k->zinptr = k->zinbuf;

    /* Read into Kermit's input buffer */
    size_t nread = fread(k->zinptr, 1, k->zinlen, ctx->file);

    if (ferror(ctx->file)) {
        fprintf(stderr, "[%s][ERROR] %s:%d: fread() failed: %s\r\n", kermit_get_timestamp(), __FILE__, __LINE__, strerror(errno));
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "File read error: %s", strerror(errno));
        ctx->last_errno = errno;
        return -1;
    }

    k->zincnt = nread;

    /* CRITICAL: Check for EOF and return -1 (not 0!)
     * ekermit's readfile() (unixio.c:523-524) returns -1 for EOF.
     * Returning 0 causes infinite loop because ekermit keeps calling readf
     * when zincnt==0, expecting it to refill the buffer.
     *
     * Per ekermit/unixio.c:523-524:
     *   if (k->zincnt == 0)    // Check for EOF
     *     return(-1);          // Return -1 for EOF
     */
    if (nread == 0) {
        #ifdef DEBUG
    printf("[DEBUG] %s:%d: End of file reached (total: %lu bytes) - returning -1 for EOF\r\n", __FILE__, __LINE__, (unsigned long)ctx->bytes_done); fflush(stdout);
#endif
        return -1;  /* EOF: return -1, NOT 0! */
    }

    ctx->bytes_done += nread;

    /* Update transfer state for progress tracking */
    if (ctx->transfer_state) {
        ctx->transfer_state->bytes_transferred = ctx->bytes_done;
        ctx->transfer_state->last_data_time = time(NULL);
    }

    #ifdef DEBUG
    printf("[DEBUG] %s:%d: Read %zu bytes from file (total: %lu / %lu)\r\n", __FILE__, __LINE__, nread, (unsigned long)ctx->bytes_done,
                     (unsigned long)ctx->bytes_total); fflush(stdout);
#endif

    /* CRITICAL: readf() must return first byte and advance zinptr!
     * Per ekermit/kermit.c:56, the gnc() macro expects readf to:
     * 1. Fill buffer (k->zinbuf, k->zincnt)
     * 2. Reset k->zinptr to k->zinbuf
     * 3. Return FIRST BYTE (not byte count!)
     * 4. Advance zinptr and decrement zincnt
     *
     * Per ekermit/unixio.c:525-531:
     *   k->zinptr = k->zinbuf;     // Reset pointer
     *   (k->zincnt)--;             // Decrement count
     *   return(*(k->zinptr)++ & 0xff); // Return first byte
     */
    k->zinptr = k->zinbuf;  /* Reset pointer to start */
    (k->zincnt)--;          /* Decrement count */
    return (*(k->zinptr)++ & 0xff);  /* Return first byte */
}

int otelnet_kermit_writef(struct k_data *k, UCHAR *buf, int len) {
    kermit_client_ctx_t *ctx = get_ctx(k);

    if (!ctx->file) {
        fprintf(stderr, "[%s][ERROR] %s:%d: No file open for writing\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "No file open");
        return -1;
    }

    size_t written = fwrite(buf, 1, len, ctx->file);
    if (written < (size_t)len) {
        fprintf(stderr, "[%s][ERROR] %s:%d: fwrite() failed: %s\r\n", kermit_get_timestamp(), __FILE__, __LINE__, strerror(errno));
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "File write error: %s", strerror(errno));
        ctx->last_errno = errno;
        return -1;
    }

    /* Flush to disk periodically to ensure data is written */
    if ((ctx->bytes_done % 8192) == 0) {
        fflush(ctx->file);
    }

    ctx->bytes_done += written;

    /* Update transfer state for progress tracking */
    if (ctx->transfer_state) {
        ctx->transfer_state->bytes_transferred = ctx->bytes_done;
        ctx->transfer_state->last_data_time = time(NULL);
    }

    #ifdef DEBUG
    printf("[DEBUG] %s:%d: Wrote %zu bytes to file (total: %lu)\r\n", __FILE__, __LINE__, written, (unsigned long)ctx->bytes_done); fflush(stdout);
#endif

    /* IMPORTANT: ekermit expects X_OK (0) on success, NOT byte count!
     * Returning byte count causes ekermit to treat it as an error.
     * See ekermit/kermit.c:1229 - "if (rc != X_OK) break;" */
    return X_OK;
}

int otelnet_kermit_closef(struct k_data *k, UCHAR status, int discard) {
    kermit_client_ctx_t *ctx = get_ctx(k);

    if (!ctx->file) {
        return 0;  /* Nothing to close */
    }

    fclose(ctx->file);
    ctx->file = NULL;

    /* Delete incomplete file only if transfer was incomplete
     * Following ekermit standard (unixio.c:602-607):
     * - discard=2: receive mode (closing output file)
     * - status='D': incomplete Data packet (transfer interrupted)
     * - k->ikeep=0: don't keep incomplete files
     *
     * DO NOT delete if:
     * - status='B': EOT (Break) packet - transfer completed successfully
     * - status='Z': EOF packet - file transfer completed
     */
    if (discard == 2 && !ctx->is_sending && status == 'D' && k->ikeep == 0 && ctx->current_filename[0]) {
        fprintf(stderr, "[%s][WARNING] Deleting incomplete file: %s (status='%c')\r\n",
                kermit_get_timestamp(), ctx->current_filename, status);
        if (unlink(ctx->current_filename) < 0) {
            fprintf(stderr, "[%s][WARNING] Failed to delete file: %s\r\n", kermit_get_timestamp(), strerror(errno));
        }
    }

    printf("[%s][INFO] Closed file: %s (status=%d, discard=%d, bytes=%lu)\r\n", kermit_get_timestamp(), ctx->current_filename, status, discard,
                (unsigned long)ctx->bytes_done); fflush(stdout);

    ctx->current_filename[0] = '\0';
    return 0;
}

ULONG otelnet_kermit_finfo(struct k_data *k, UCHAR *name, UCHAR *buf,
                           int buflen, short *type, short xmode) {
    kermit_client_ctx_t *ctx = get_ctx(k);
    struct stat st;

    (void)xmode;  /* Unused */

    if (stat((char *)name, &st) < 0) {
        fprintf(stderr, "[%s][ERROR] %s:%d: stat() failed for '%s': %s\r\n", kermit_get_timestamp(), __FILE__, __LINE__, name, strerror(errno));
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "Cannot stat file: %s", strerror(errno));
        ctx->last_errno = errno;
        return 0;
    }

    /* Set file type: always binary (1) for otelnet */
    if (type) {
        *type = 1;  /* Binary mode */
    }

    /* Format file date if buffer provided */
    if (buf && buflen > 0) {
        struct tm *tm = localtime(&st.st_mtime);
        if (tm) {
            strftime((char *)buf, buflen, "%Y%m%d %H:%M:%S", tm);
        } else {
            buf[0] = '\0';
        }
    }

    ctx->bytes_total = st.st_size;

    #ifdef DEBUG
    printf("[DEBUG] %s:%d: File info for '%s': size=%ld, mtime=%ld\r\n", __FILE__, __LINE__, name, (long)st.st_size, (long)st.st_mtime); fflush(stdout);
#endif

    return (ULONG)st.st_size;
}

#ifdef DEBUG
int otelnet_kermit_debug(int fc, UCHAR *label, UCHAR *sval, long nval) {
    /* Debug callback for ekermit */
    switch (fc) {
        case DB_MSG:
            if (label) {
                #ifdef DEBUG
    printf("[DEBUG] %s:%d: EKERMIT: %s\r\n", __FILE__, __LINE__, label); fflush(stdout);
#endif
            }
            break;
        case DB_LOG:
            if (sval) {
                #ifdef DEBUG
    printf("[DEBUG] %s:%d: EKERMIT: %s=[%s]\r\n", __FILE__, __LINE__, label ? (char *)label : "", sval); fflush(stdout);
#endif
            } else {
                #ifdef DEBUG
    printf("[DEBUG] %s:%d: EKERMIT: %s=%ld\r\n", __FILE__, __LINE__, label ? (char *)label : "", nval); fflush(stdout);
#endif
            }
            break;
        case DB_CHR:
            #ifdef DEBUG
    printf("[DEBUG] %s:%d: EKERMIT: %s=[%c]\r\n", __FILE__, __LINE__, label ? (char *)label : "", (char)nval); fflush(stdout);
#endif
            break;
        default:
            break;
    }
    return 0;
}
#endif

/*
 * High-Level API Implementation
 */

int kermit_client_send(int socket_fd, telnet_t *telnet_ctx,
                       transfer_state_t *transfer_state,
                       const char *filename,
                       struct otelnet_ctx *otelnet_ctx) {
    struct k_response r;
    kermit_client_ctx_t ctx;
    int status;
    short r_slot;
    UCHAR *inbuf;
    int rx_len;
    bool detectors_were_enabled = false;

    printf("[%s][INFO] === Kermit Send Started: %s ===\r\n", kermit_get_timestamp(), filename); fflush(stdout);

    /* CRITICAL: Disable all auto-detection during Kermit transfer
     * to prevent ZMODEM/XMODEM/YMODEM detectors from interfering with Kermit packets */
    if (otelnet_ctx != NULL) {
        /* Save current state and disable detectors */
        detectors_were_enabled = otelnet_ctx->zmodem_detector.enabled;

        zmodem_detector_set_enabled(&otelnet_ctx->zmodem_detector, false);
        xmodem_detector_set_enabled(&otelnet_ctx->xmodem_detector, false);
        ymodem_detector_set_enabled(&otelnet_ctx->ymodem_detector, false);

        printf("[%s][INFO] Auto-detection disabled for Kermit transfer\r\n", kermit_get_timestamp()); fflush(stdout);
    } else {
        fprintf(stderr, "[%s][WARNING] otelnet_ctx is NULL, cannot disable auto-detection!\r\n", kermit_get_timestamp());
    }

    /* Initialize context */
    memset(&ctx, 0, sizeof(ctx));
    ctx.socket_fd = socket_fd;
    ctx.telnet_ctx = telnet_ctx;
    ctx.transfer_state = transfer_state;
    ctx.otelnet_ctx = otelnet_ctx;  /* For pending buffer access */
    ctx.is_sending = true;
    ctx.start_time = time(NULL);
    ctx.last_activity = time(NULL);

    /* Store absolute path for file opening (openf will use this for mode=1) */
    strncpy(ctx.send_file_absolute_path, filename, sizeof(ctx.send_file_absolute_path) - 1);
    ctx.send_file_absolute_path[sizeof(ctx.send_file_absolute_path) - 1] = '\0';

    /* Extract basename for Kermit protocol transmission */
    const char *basename = strrchr(filename, '/');
    basename = basename ? basename + 1 : filename;

    /* Retry control - CLIENT_KERMIT_GUIDELINES.md */
    ctx.consecutive_naks = 0;
    ctx.max_consecutive_naks = 10;  /* Max 10 consecutive NAKs */
    ctx.transfer_timeout = 60;      /* 60 second global timeout */
    ctx.consecutive_timeouts = 0;   /* Consecutive timeout count */
    ctx.max_consecutive_timeouts = 5; /* Max 5 consecutive timeouts */
    ctx.last_progress_percent = -1; /* No progress reported yet */

    /* Initialize Kermit data structure */
    memset(&k_send, 0, sizeof(k_send));
    memset(&r, 0, sizeof(r));

    /* Store context pointer in global variable (NOT in k_send.istring!)
     * ekermit uses istring for filenames, so we use a separate global pointer */
    g_send_ctx = &ctx;

    /* Set up file input buffer */
    k_send.zinbuf = ctx.input_buffer;
    k_send.zinlen = sizeof(ctx.input_buffer);
    k_send.zinptr = ctx.input_buffer;
    k_send.zincnt = 0;

    /* Set up file output buffer */
    k_send.obuf = ctx.output_buffer;
    k_send.obuflen = sizeof(ctx.output_buffer);
    k_send.obufpos = 0;

    /* Set file list (basename only for protocol transmission) */
    UCHAR *filelist[2];
    filelist[0] = (UCHAR *)basename;
    filelist[1] = NULL;
    k_send.filelist = filelist;

    /* Protocol settings (MUST be set before K_INIT) */
    k_send.binary = 1;      /* Binary mode (always) */
    k_send.parity = 0;      /* No parity */
    k_send.bct = 3;         /* Block check type 3 (CRC-16) - 99.998% error detection */
    k_send.bctf = 0;        /* Sender: Normal negotiation (recommend CRC-16, S-Init uses Type 1, data uses negotiated Type 3) */
    k_send.remote = 1;      /* Remote mode (no local terminal) */

    /* Enable Long Packets (F_LP) for 43.6x performance improvement
     * - Standard packets: 94 bytes (39,322 packets for 3MB)
     * - Long packets: 4096 bytes (768 packets for 3MB)
     * - Reduces packet count by 51.2x */
#ifdef F_LP
    k_send.s_maxlen = 4096;  /* Request 4096-byte packets from receiver */
    k_send.r_maxlen = 4096;  /* Accept up to 4096-byte packets */
    printf("[%s][KERMIT-INFO] Long packets enabled: 4096 bytes (F_LP)\r\n", kermit_get_timestamp());
    fflush(stdout);
    printf("[%s][INFO] Long packets enabled: 4096 bytes\r\n", kermit_get_timestamp()); fflush(stdout);
#else
    printf("[%s][KERMIT-WARNING] Long packets NOT available - using 94 bytes (compile with F_LP)\r\n", kermit_get_timestamp());
    fflush(stdout);
    fprintf(stderr, "[%s][WARNING] Long packets NOT compiled - using standard 94-byte packets\r\n", kermit_get_timestamp());
#endif

    /* Enable Sliding Windows (F_SSW) for 31x performance improvement
     * - Stop-and-wait: 1 packet in flight (39,322 round trips for 3MB)
     * - Sliding windows: 31 packets in flight (~26 round trips for 3MB)
     * - Reduces round trips by 1,512x
     * - Combined with long packets: 1,350x total improvement */
#ifdef F_SSW
    k_send.window = 31;      /* Request 31-slot sliding window */
    printf("[%s][KERMIT-INFO] Sliding windows enabled: 31 slots (F_SSW)\r\n", kermit_get_timestamp());
    fflush(stdout);
    printf("[%s][INFO] Sliding windows enabled: 31 slots\r\n", kermit_get_timestamp()); fflush(stdout);
#else
    printf("[%s][KERMIT-WARNING] Sliding windows NOT available - using stop-and-wait (compile with F_SSW)\r\n", kermit_get_timestamp());
    fflush(stdout);
    fprintf(stderr, "[%s][WARNING] Sliding windows NOT compiled - using stop-and-wait protocol\r\n", kermit_get_timestamp());
#endif

    /* Register I/O callbacks (MUST be set BEFORE K_INIT for debug output) */
    k_send.rxd = otelnet_kermit_rxd;
    k_send.txd = otelnet_kermit_txd;
    k_send.ixd = otelnet_kermit_ixd;
    k_send.openf = otelnet_kermit_openf;
    k_send.finfo = otelnet_kermit_finfo;
    k_send.readf = otelnet_kermit_readf;
    k_send.writef = otelnet_kermit_writef;
    k_send.closef = otelnet_kermit_closef;
#ifdef DEBUG
    k_send.dbf = otelnet_kermit_debug;
#else
    k_send.dbf = 0;
#endif

    /* Debug: Verify settings before K_INIT */
    printf("[%s][KERMIT-DEBUG] Settings BEFORE K_INIT: bct=%d, bctf=%d\r\n",
           kermit_get_timestamp(), k_send.bct, k_send.bctf);
    fflush(stdout);

    /* Initialize Kermit protocol */
    status = kermit(K_INIT, &k_send, 0, 0, "", &r);
    if (status == X_ERROR) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Kermit initialization failed\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
        printf("\r\n[Error: Kermit initialization failed]\r\r\n");
        kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
        return ERROR_GENERAL;
    }

    /* Debug: Verify settings after K_INIT */
    printf("[%s][KERMIT-DEBUG] Settings AFTER K_INIT: bct=%d, bctf=%d\r\n",
           kermit_get_timestamp(), k_send.bct, k_send.bctf);
    fflush(stdout);

    /* IMPORTANT: Reset buffer pointers after K_INIT to prevent static variable pollution
     * Server reference: kermit_server.c:907-910 */
    k_send.zinptr = ctx.input_buffer;
    k_send.zincnt = 0;
    #ifdef DEBUG
    printf("[DEBUG] %s:%d: Buffer state reset after K_INIT\r\n", __FILE__, __LINE__); fflush(stdout);
#endif

    /* IMPORTANT: Set timeouts AFTER K_INIT to prevent platform defaults from overwriting
     * Server reference: kermit_server.c:161-165
     * Set adaptive timeout based on network characteristics
     * Per KERMIT_PACKET_REPORT_1.md performance analysis:
     * - Localhost (RTT ~0.1ms): 10s timeout sufficient
     * - LAN (RTT ~1ms): 15s timeout recommended
     * - Internet (RTT ~50ms): 15-20s timeout recommended
     * - Satellite (RTT ~600ms): 30s timeout recommended
     *
     * Default 15 seconds is appropriate for most scenarios.
     * For very slow links, user can increase via config. */
    k_send.r_timo = 15;  /* Receive timeout */
    k_send.s_timo = 15;  /* Send timeout */
    printf("[KERMIT-INFO] Packet timeout: %d seconds (appropriate for LAN/Internet)\r\n", k_send.r_timo);
    printf("[KERMIT-INFO] Note: Increase if using satellite or very high-latency links\r\n");
    fflush(stdout);
    printf("[%s][INFO] Timeouts set after K_INIT: r_timo=%d, s_timo=%d\r\n", kermit_get_timestamp(), k_send.r_timo, k_send.s_timo); fflush(stdout);

    printf("[%s][INFO] Kermit initialized, starting send sequence\r\n", kermit_get_timestamp()); fflush(stdout);

    /* CRITICAL: Drain socket buffer before starting Kermit transfer
     * Server reference: kermit_server.c:801-832
     * This ensures no residual telnet protocol packets interfere with Kermit data */
    printf("[%s][INFO] Draining socket buffer before Kermit send...\r\n", kermit_get_timestamp()); fflush(stdout);
    {
        unsigned char drain_buf[256];
        struct timeval tv;
        fd_set readfds;
        int drain_count = 0;

        tv.tv_sec = 0;
        tv.tv_usec = 50000;  /* 50ms timeout */

        FD_ZERO(&readfds);
        FD_SET(socket_fd, &readfds);

        while (select(socket_fd + 1, &readfds, NULL, NULL, &tv) > 0) {
            ssize_t n = recv(socket_fd, drain_buf, sizeof(drain_buf), MSG_DONTWAIT);
            if (n <= 0) break;

            drain_count += n;
            #ifdef DEBUG
    printf("[DEBUG] %s:%d: Drained %zd bytes (total: %d)\r\n", __FILE__, __LINE__, n, drain_count); fflush(stdout);
#endif

            /* Re-check for more data with shorter timeout */
            tv.tv_sec = 0;
            tv.tv_usec = 20000;  /* 20ms */
            FD_ZERO(&readfds);
            FD_SET(socket_fd, &readfds);
        }

        if (drain_count > 0) {
            fprintf(stderr, "[%s][WARNING] Drained %d bytes before Kermit send\r\n", kermit_get_timestamp(), drain_count);
        } else {
            printf("[%s][INFO] Socket buffer clean\r\n", kermit_get_timestamp()); fflush(stdout);
        }
    }

    /* Start send sequence - K_SEND should be called only ONCE (like ekermit/main.c) */
    status = kermit(K_SEND, &k_send, 0, 0, "", &r);
    if (status == X_ERROR) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Kermit send start failed\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
        printf("\r\n[Error: Failed to start Kermit send]\r\r\n");
        kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
        return ERROR_GENERAL;
    }

    /* Verify we're in send mode - Server reference: kermit_server.c:919-925 */
    if (k_send.what != W_SEND) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Not in W_SEND mode after K_SEND (k.what=%d)\r\n", kermit_get_timestamp(), __FILE__, __LINE__, k_send.what);
        printf("\r\n[Error: Kermit not in send mode]\r\r\n");
        kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
        return ERROR_GENERAL;
    }
    printf("[%s][INFO] Kermit send mode verified: state=%d\r\n", kermit_get_timestamp(), k_send.state); fflush(stdout);

    /* Protocol loop */
    printf("\r\n[Kermit Send Mode]\r\r\n");
    printf("[Sending: %s]\r\r\n", filename);

    while (status != X_DONE) {
        /* Check global transfer timeout - CLIENT_KERMIT_GUIDELINES.md Priority 2 */
        time_t now = time(NULL);
        if (now - ctx.start_time > ctx.transfer_timeout) {
            fprintf(stderr, "[%s][ERROR] %s:%d: Transfer timeout (%ld seconds) - aborting\r\n", kermit_get_timestamp(), __FILE__, __LINE__, (long)ctx.transfer_timeout);
            printf("\r\n[Error: Transfer timeout (%ld seconds)]\r\r\n", (long)ctx.transfer_timeout);
            kermit(K_ERROR, &k_send, 0, 0, "Transfer timeout", &r);
            kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
            return ERROR_TIMEOUT;
        }

        /* Allocate receive slot for incoming packet */
        inbuf = getrslot(&k_send, &r_slot);
        if (!inbuf) {
            fprintf(stderr, "[%s][ERROR] %s:%d: Failed to allocate receive slot\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
            kermit(K_ERROR, &k_send, 0, 0, "Internal error", &r);
            kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
            return ERROR_GENERAL;
        }

        /* Read packet from remote */
        rx_len = k_send.rxd(&k_send, inbuf, P_PKTLEN);

        if (rx_len < 1) {
            freerslot(&k_send, r_slot);
            if (rx_len < 0) {
                /* Fatal communication error */
                fprintf(stderr, "[%s][ERROR] %s:%d: Fatal communication error\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
                printf("\r\n[%s] [Error: Connection lost]\r\r\n", kermit_get_timestamp());
                kermit(K_ERROR, &k_send, 0, 0, "Communication error", &r);
                kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
                return ERROR_CONNECTION;
            }
            /* Timeout - check consecutive timeout limit
             * Server reference: kermit_server.c:946-958 */
            ctx.consecutive_timeouts++;
            fprintf(stderr, "[%s][WARNING] Timeout (consecutive: %d/%d)\r\n", kermit_get_timestamp(), ctx.consecutive_timeouts, ctx.max_consecutive_timeouts);

            if (ctx.consecutive_timeouts >= ctx.max_consecutive_timeouts) {
                fprintf(stderr, "[%s][ERROR] %s:%d: Too many consecutive timeouts, aborting\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
                printf("\r\n[Error: Too many consecutive timeouts (%d)]\r\r\n",
                       ctx.max_consecutive_timeouts);
                kermit(K_ERROR, &k_send, 0, 0, "Too many timeouts", &r);
                kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
                return ERROR_TIMEOUT;
            }
            continue;
        }

        /* Process packet */
        status = kermit(K_RUN, &k_send, r_slot, rx_len, "", &r);

        /* Check for NAK - CLIENT_KERMIT_GUIDELINES.md Priority 1 */
        if (k_send.opktlen >= 5 && k_send.opktbuf[3] == 'N') {
            ctx.consecutive_naks++;
            #ifdef DEBUG
    printf("[DEBUG] %s:%d: NAK sent (consecutive: %d/%d)\r\n", __FILE__, __LINE__, ctx.consecutive_naks, ctx.max_consecutive_naks); fflush(stdout);
#endif

            /* ENHANCED DEBUGGING: Analyze the packet that caused NAK */
#ifdef DEBUG
            printf( "\r\n");
            printf( "[KERMIT-DEBUG] ========================================\r\n");
            printf( "[KERMIT-DEBUG] NAK #%d - DETAILED PACKET ANALYSIS (SEND MODE)\r\n", ctx.consecutive_naks);
            printf( "[KERMIT-DEBUG] ========================================\r\n");
            kermit_analyze_packet(inbuf, rx_len);

            /* Show outgoing NAK packet too */
            printf( "[KERMIT-DEBUG] Sending NAK packet:\r\n");
            kermit_dump_packet("NAK packet", k_send.opktbuf, k_send.opktlen);
            printf( "[KERMIT-DEBUG] ========================================\r\n");
            printf( "\r\n");
            fflush(stdout);
#endif

            if (ctx.consecutive_naks >= ctx.max_consecutive_naks) {
                fprintf(stderr, "[%s][ERROR] %s:%d: Max consecutive NAKs (%d) exceeded - aborting\r\n", kermit_get_timestamp(), __FILE__, __LINE__, ctx.max_consecutive_naks);
                printf("\r\n[Error: Max retries (%d NAKs) exceeded]\r\r\n",
                       ctx.max_consecutive_naks);
                kermit(K_ERROR, &k_send, 0, 0, "Too many retries", &r);
                kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
                return ERROR_GENERAL;
            }
        } else if (status == X_OK && r.sofar > 0) {
            /* Reset on successful data transfer */
            ctx.consecutive_naks = 0;
            ctx.consecutive_timeouts = 0;  /* Reset on success */
        }

        switch (status) {
            case X_OK:
                /* Transfer in progress - show progress with 10% throttling
                 * Server reference: kermit_server.c:975-990 */
                if (r.filesize > 0 && r.sofar > 0) {
                    int current_progress = (int)((r.sofar * 100) / r.filesize);

                    /* Only log every 10% or at 100% to reduce log clutter */
                    if (current_progress >= ctx.last_progress_percent + 10 ||
                        current_progress == 100 || r.sofar == r.filesize) {
                        time_t elapsed = time(NULL) - ctx.start_time;
                        double rate = elapsed > 0 ? (double)r.sofar / elapsed : 0;

                        printf("\r[Progress: %ld / %ld bytes (%d%%) - %.1f KB/s - %lds]",
                               r.sofar, r.filesize, current_progress, rate / 1024.0, (long)elapsed);
                        fflush(stdout);

                        printf("[%s][INFO] Progress: %lld / %lld bytes (%.1f%%)\r\n", kermit_get_timestamp(), (long long)r.sofar, (long long)r.filesize,
                                    (r.sofar * 100.0) / r.filesize); fflush(stdout);
                        ctx.last_progress_percent = current_progress;
                    }
                }

                /* Update transfer state */
                if (transfer_state) {
                    transfer_state->bytes_transferred = r.sofar;
                    transfer_state->total_bytes = r.filesize;
                }
                continue;

            case X_DONE:
                /* Transfer completed successfully */
                printf("\r\n[Transfer complete: %ld bytes]\r\r\n", r.sofar);
                printf("[%s][INFO] Transfer completed successfully: %ld bytes\r\n", kermit_get_timestamp(), r.sofar); fflush(stdout);
                break;

            case X_ERROR:
                /* Protocol error */
                printf("\r\n[Error: Protocol error during transfer]\r\r\n");
                fprintf(stderr, "[%s][ERROR] %s:%d: Protocol error during transfer\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
                kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
                return ERROR_GENERAL;

            default:
                fprintf(stderr, "[%s][WARNING] Unexpected status from kermit(): %d\r\n", kermit_get_timestamp(), status);
                break;
        }
    }

    printf("[%s][INFO] === Kermit Send Completed Successfully ===\r\n", kermit_get_timestamp()); fflush(stdout);
    kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
    return SUCCESS;
}

int kermit_client_receive(int socket_fd, telnet_t *telnet_ctx,
                          transfer_state_t *transfer_state,
                          struct otelnet_ctx *otelnet_ctx) {
    struct k_response r;
    kermit_client_ctx_t ctx;
    int status;
    short r_slot;
    UCHAR *inbuf;
    int rx_len;
    bool detectors_were_enabled = false;

    printf("[%s][INFO] === Kermit Receive Started ===\r\n", kermit_get_timestamp()); fflush(stdout);

    /* CRITICAL: Disable all auto-detection during Kermit transfer
     * to prevent ZMODEM/XMODEM/YMODEM detectors from interfering with Kermit packets */
    if (otelnet_ctx != NULL) {
        /* Save current state and disable detectors */
        detectors_were_enabled = otelnet_ctx->zmodem_detector.enabled;

        zmodem_detector_set_enabled(&otelnet_ctx->zmodem_detector, false);
        xmodem_detector_set_enabled(&otelnet_ctx->xmodem_detector, false);
        ymodem_detector_set_enabled(&otelnet_ctx->ymodem_detector, false);

        printf("[%s][INFO] Auto-detection disabled for Kermit transfer\r\n", kermit_get_timestamp()); fflush(stdout);
    } else {
        fprintf(stderr, "[%s][WARNING] otelnet_ctx is NULL, cannot disable auto-detection!\r\n", kermit_get_timestamp());
    }

    /* Initialize context */
    memset(&ctx, 0, sizeof(ctx));
    ctx.socket_fd = socket_fd;
    ctx.telnet_ctx = telnet_ctx;
    ctx.transfer_state = transfer_state;
    ctx.otelnet_ctx = otelnet_ctx;  /* For pending buffer access */
    ctx.is_sending = false;  /* Receiving mode */
    ctx.start_time = time(NULL);
    ctx.last_activity = time(NULL);

    /* Retry control - CLIENT_KERMIT_GUIDELINES.md */
    ctx.consecutive_naks = 0;
    ctx.max_consecutive_naks = 10;  /* Max 10 consecutive NAKs */
    ctx.transfer_timeout = 60;      /* 60 second global timeout */
    ctx.consecutive_timeouts = 0;   /* Consecutive timeout count */
    ctx.max_consecutive_timeouts = 5; /* Max 5 consecutive timeouts */
    ctx.last_progress_percent = -1; /* No progress reported yet */

    /* Initialize Kermit data structure */
    memset(&k_recv, 0, sizeof(k_recv));
    memset(&r, 0, sizeof(r));

    /* Store context pointer in global variable (NOT in k_recv.istring!)
     * ekermit uses istring for filenames, so we use a separate global pointer */
    g_recv_ctx = &ctx;

    /* Set up file input buffer */
    k_recv.zinbuf = ctx.input_buffer;
    k_recv.zinlen = sizeof(ctx.input_buffer);
    k_recv.zinptr = ctx.input_buffer;
    k_recv.zincnt = 0;

    /* Set up file output buffer */
    k_recv.obuf = ctx.output_buffer;
    k_recv.obuflen = sizeof(ctx.output_buffer);
    k_recv.obufpos = 0;

    /* Protocol settings (MUST be set before K_INIT) */
    k_recv.binary = 1;      /* Binary mode (always) */
    k_recv.parity = 0;      /* No parity */
    k_recv.bct = 3;         /* Block check type 3 (CRC-16) - 99.998% error detection */
    k_recv.bctf = 0;        /* Receiver: Normal negotiation (accept CRC-16 request, respond with Type 3 capability) */
    k_recv.remote = 1;      /* Remote mode (no local terminal) */
    k_recv.ikeep = 0;       /* Don't keep incomplete files by default */

    /* Enable Long Packets (F_LP) for 43.6x performance improvement
     * - Standard packets: 94 bytes (39,322 packets for 3MB)
     * - Long packets: 4096 bytes (768 packets for 3MB)
     * - Reduces packet count by 51.2x */
#ifdef F_LP
    k_recv.s_maxlen = 4096;  /* Announce 4096-byte packet capability */
    k_recv.r_maxlen = 4096;  /* Accept up to 4096-byte packets */
    printf("[KERMIT-INFO] Long packets enabled: 4096 bytes (F_LP)\r\n");
    fflush(stdout);
    printf("[%s][INFO] Long packets enabled: 4096 bytes\r\n", kermit_get_timestamp()); fflush(stdout);
#else
    printf("[KERMIT-WARNING] Long packets NOT available - using 94 bytes (compile with F_LP)\r\n");
    fflush(stdout);
    fprintf(stderr, "[%s][WARNING] Long packets NOT compiled - using standard 94-byte packets\r\n", kermit_get_timestamp());
#endif

    /* Enable Sliding Windows (F_SSW) for 31x performance improvement
     * - Stop-and-wait: 1 packet in flight (39,322 round trips for 3MB)
     * - Sliding windows: 31 packets in flight (~26 round trips for 3MB)
     * - Reduces round trips by 1,512x
     * - Combined with long packets: 1,350x total improvement */
#ifdef F_SSW
    k_recv.window = 31;      /* Announce 31-slot sliding window capability */
    printf("[KERMIT-INFO] Sliding windows enabled: 31 slots (F_SSW)\r\n");
    fflush(stdout);
    printf("[%s][INFO] Sliding windows enabled: 31 slots\r\n", kermit_get_timestamp()); fflush(stdout);
#else
    printf("[KERMIT-WARNING] Sliding windows NOT available - using stop-and-wait (compile with F_SSW)\r\n");
    fflush(stdout);
    fprintf(stderr, "[%s][WARNING] Sliding windows NOT compiled - using stop-and-wait protocol\r\n", kermit_get_timestamp());
#endif

    /* Register I/O callbacks (MUST be set BEFORE K_INIT for debug output) */
    k_recv.rxd = otelnet_kermit_rxd;
    k_recv.txd = otelnet_kermit_txd;
    k_recv.ixd = otelnet_kermit_ixd;
    k_recv.openf = otelnet_kermit_openf;
    k_recv.finfo = otelnet_kermit_finfo;
    k_recv.readf = otelnet_kermit_readf;
    k_recv.writef = otelnet_kermit_writef;
    k_recv.closef = otelnet_kermit_closef;
#ifdef DEBUG
    k_recv.dbf = otelnet_kermit_debug;
#else
    k_recv.dbf = 0;
#endif

    /* Debug: Verify settings before K_INIT */
    printf("[KERMIT-DEBUG] Settings BEFORE K_INIT: bct=%d, bctf=%d\r\n", k_recv.bct, k_recv.bctf);
    fflush(stdout);

    /* Initialize Kermit protocol */
    status = kermit(K_INIT, &k_recv, 0, 0, "", &r);
    if (status == X_ERROR) {
        fprintf(stderr, "[%s][ERROR] %s:%d: Kermit initialization failed\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
        printf("\r\n[Error: Kermit initialization failed]\r\r\n");
        kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
        return ERROR_GENERAL;
    }

    /* Debug: Verify settings after K_INIT */
    printf("[KERMIT-DEBUG] Settings AFTER K_INIT: bct=%d, bctf=%d\r\n", k_recv.bct, k_recv.bctf);
    fflush(stdout);

    /* IMPORTANT: Reset buffer pointers after K_INIT to prevent static variable pollution
     * Server reference: kermit_server.c:907-910 */
    k_recv.zinptr = ctx.input_buffer;
    k_recv.zincnt = 0;
    #ifdef DEBUG
    printf("[DEBUG] %s:%d: Buffer state reset after K_INIT\r\n", __FILE__, __LINE__); fflush(stdout);
#endif

    /* IMPORTANT: Set timeouts AFTER K_INIT to prevent platform defaults from overwriting
     * Server reference: kermit_server.c:161-165
     * Set adaptive timeout based on network characteristics
     * Per KERMIT_PACKET_REPORT_1.md performance analysis:
     * - Localhost (RTT ~0.1ms): 10s timeout sufficient
     * - LAN (RTT ~1ms): 15s timeout recommended
     * - Internet (RTT ~50ms): 15-20s timeout recommended
     * - Satellite (RTT ~600ms): 30s timeout recommended
     *
     * Default 15 seconds is appropriate for most scenarios.
     * For very slow links, user can increase via config. */
    k_recv.r_timo = 15;  /* Receive timeout */
    k_recv.s_timo = 15;  /* Send timeout */
    printf("[KERMIT-INFO] Packet timeout: %d seconds (appropriate for LAN/Internet)\r\n", k_recv.r_timo);
    printf("[KERMIT-INFO] Note: Increase if using satellite or very high-latency links\r\n");
    fflush(stdout);
    printf("[%s][INFO] Timeouts set after K_INIT: r_timo=%d, s_timo=%d\r\n", kermit_get_timestamp(), k_recv.r_timo, k_recv.s_timo); fflush(stdout);

    printf("[%s][INFO] Kermit initialized, ready to receive\r\n", kermit_get_timestamp()); fflush(stdout);

    /* Note: For receive mode, we DON'T call K_SEND - just start receiving */

    /* Protocol loop */
    printf("\r\n[%s] [Kermit Receive Mode]\r\r\n", kermit_get_timestamp());
    printf("[%s] [Waiting for sender...]\r\r\n", kermit_get_timestamp());

    while (status != X_DONE) {
        /* Check global transfer timeout - CLIENT_KERMIT_GUIDELINES.md Priority 2 */
        time_t now = time(NULL);
        if (now - ctx.start_time > ctx.transfer_timeout) {
            fprintf(stderr, "[%s][ERROR] %s:%d: Transfer timeout (%ld seconds) - aborting\r\n", kermit_get_timestamp(), __FILE__, __LINE__, (long)ctx.transfer_timeout);
            printf("\r\n[Error: Transfer timeout (%ld seconds)]\r\r\n", (long)ctx.transfer_timeout);
            kermit(K_ERROR, &k_recv, 0, 0, "Transfer timeout", &r);
            kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
            return ERROR_TIMEOUT;
        }

        /* Allocate receive slot */
        inbuf = getrslot(&k_recv, &r_slot);
        if (!inbuf) {
            fprintf(stderr, "[%s][ERROR] %s:%d: Failed to allocate receive slot\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
            kermit(K_ERROR, &k_recv, 0, 0, "Internal error", &r);
            kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
            return ERROR_GENERAL;
        }

        /* Read packet from remote */
        rx_len = k_recv.rxd(&k_recv, inbuf, P_PKTLEN);

        /* Log rxd result */
        #ifdef DEBUG
    printf("[DEBUG] %s:%d: rxd() returned: %d bytes\r\n", __FILE__, __LINE__, rx_len); fflush(stdout);
#endif

        if (rx_len < 1) {
            freerslot(&k_recv, r_slot);
            if (rx_len < 0) {
                /* Fatal communication error */
                fprintf(stderr, "[%s][ERROR] %s:%d: Fatal communication error\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
                printf("\r\n[%s] [Error: Connection lost]\r\r\n", kermit_get_timestamp());
                kermit(K_ERROR, &k_recv, 0, 0, "Communication error", &r);
                kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
                return ERROR_CONNECTION;
            }
            /* Timeout - check consecutive timeout limit
             * Server reference: kermit_server.c:946-958 */
            ctx.consecutive_timeouts++;
            fprintf(stderr, "[%s][WARNING] Timeout (consecutive: %d/%d)\r\n", kermit_get_timestamp(), ctx.consecutive_timeouts, ctx.max_consecutive_timeouts);

            if (ctx.consecutive_timeouts >= ctx.max_consecutive_timeouts) {
                fprintf(stderr, "[%s][ERROR] %s:%d: Too many consecutive timeouts, aborting\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
                printf("\r\n[Error: Too many consecutive timeouts (%d)]\r\r\n",
                       ctx.max_consecutive_timeouts);
                kermit(K_ERROR, &k_recv, 0, 0, "Too many timeouts", &r);
                kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
                return ERROR_TIMEOUT;
            }
            #ifdef DEBUG
    printf("[DEBUG] %s:%d: rxd timeout, retrying...\r\n", __FILE__, __LINE__); fflush(stdout);
#endif
            continue;
        }

        /* Log received packet analysis */
        if (rx_len >= 4) {
            #ifdef DEBUG
    printf("[DEBUG] %s:%d: Packet: LEN=%02X SEQ=%02X TYPE=%c\r\n", __FILE__, __LINE__, inbuf[0], inbuf[1], inbuf[2]); fflush(stdout);
#endif
        }

        /* Note: Detailed packet analysis moved to otelnet_kermit_rxd() (before SOH removal)
         * and is only shown for NAK packets below to reduce output overhead */

        /* Process packet */
        printf("[KERMIT-DEBUG] About to call kermit(K_RUN): r_slot=%d, rx_len=%d\r\n", r_slot, rx_len);
        printf("[KERMIT-DEBUG] Buffer inbuf=%p, first 4 bytes: %02X %02X %02X %02X\r\n",
               (void*)inbuf, inbuf[0], inbuf[1], inbuf[2], inbuf[3]);
        printf("[KERMIT-DEBUG] k_recv.dbf=%p, k_recv.state=%d, k_recv.what=%d\r\n",
               (void*)k_recv.dbf, k_recv.state, k_recv.what);
        fflush(stdout);

        status = kermit(K_RUN, &k_recv, r_slot, rx_len, "", &r);

        printf("[KERMIT-DEBUG] kermit(K_RUN) returned!\r\n");
        fflush(stdout);

        /* Log kermit() execution result */
        #ifdef DEBUG
    printf("[DEBUG] %s:%d: kermit(K_RUN) returned status: %d\r\n", __FILE__, __LINE__, status); fflush(stdout);
#endif

        /* Debug: Check bct after processing S-Init packet */
        if (rx_len >= 4 && inbuf[2] == 'S') {
            printf("[KERMIT-DEBUG] After S-Init processing: bct=%d, bctf=%d\r\n", k_recv.bct, k_recv.bctf);
            fflush(stdout);
        }

        /* Check for NAK - CLIENT_KERMIT_GUIDELINES.md Priority 1 */
        if (k_recv.opktlen >= 5 && k_recv.opktbuf[3] == 'N') {
            ctx.consecutive_naks++;

            /* Log NAK reason with details */
            fprintf(stderr, "[%s][WARNING] Sending NAK #%d for seq=%d (received packet type='%c')\r\n", kermit_get_timestamp(), ctx.consecutive_naks,
                           k_recv.opktbuf[2] - 32,  /* Sequence number */
                           inbuf[3]);  /* Received packet type */

            #ifdef DEBUG
    printf("[DEBUG] %s:%d: NAK sent (consecutive: %d/%d)\r\n", __FILE__, __LINE__, ctx.consecutive_naks, ctx.max_consecutive_naks); fflush(stdout);
#endif

            /* Show outgoing NAK packet (detailed analysis already shown in rxd) */
#ifdef DEBUG
            printf( "[KERMIT-NAK] NAK #%d sent for seq=%d, received packet type='%c'\r\n",
                    ctx.consecutive_naks,
                    k_recv.opktbuf[2] - 32,
                    inbuf[2]);  /* TYPE is at index 2 (after SOH removal) */
            kermit_dump_packet("Outgoing NAK packet", k_recv.opktbuf, k_recv.opktlen);
            printf( "\r\n");
            fflush(stdout);
#endif

            if (ctx.consecutive_naks >= ctx.max_consecutive_naks) {
                fprintf(stderr, "[%s][ERROR] %s:%d: Max consecutive NAKs (%d) exceeded - aborting\r\n", kermit_get_timestamp(), __FILE__, __LINE__, ctx.max_consecutive_naks);
                printf("\r\n[Error: Max retries (%d NAKs) exceeded]\r\r\n",
                       ctx.max_consecutive_naks);
                kermit(K_ERROR, &k_recv, 0, 0, "Too many retries", &r);
                kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
                return ERROR_GENERAL;
            }
        } else if (status == X_OK && r.sofar > 0) {
            /* Reset on successful data transfer */
            ctx.consecutive_naks = 0;
            ctx.consecutive_timeouts = 0;  /* Reset on success */
        }

        switch (status) {
            case X_OK:
                /* Transfer in progress - show progress with 10% throttling
                 * Server reference: kermit_server.c:975-990 */
                if (r.filename[0] && r.filesize > 0 && r.sofar > 0) {
                    int current_progress = (int)((r.sofar * 100) / r.filesize);

                    /* Only log every 10% or at 100% to reduce log clutter */
                    if (current_progress >= ctx.last_progress_percent + 10 ||
                        current_progress == 100 || r.sofar == r.filesize) {
                        time_t elapsed = time(NULL) - ctx.start_time;
                        double rate = elapsed > 0 ? (double)r.sofar / elapsed : 0;

                        printf("\r[Receiving: %s - %ld / %ld bytes (%d%%) - %.1f KB/s - %lds]",
                               r.filename, r.sofar, r.filesize, current_progress, rate / 1024.0, (long)elapsed);
                        fflush(stdout);

                        printf("[%s][INFO] Progress: %lld / %lld bytes (%.1f%%)\r\n", kermit_get_timestamp(), (long long)r.sofar, (long long)r.filesize,
                                    (r.sofar * 100.0) / r.filesize); fflush(stdout);
                        ctx.last_progress_percent = current_progress;
                    }
                }

                /* Update transfer state */
                if (transfer_state) {
                    transfer_state->bytes_transferred = r.sofar;
                    transfer_state->total_bytes = r.filesize;
                    if (r.filename[0]) {
                        strncpy(transfer_state->filename, (char *)r.filename,
                                sizeof(transfer_state->filename) - 1);
                    }
                }
                continue;

            case X_DONE:
                /* Transfer completed */
                printf("\r\n[Transfer complete: %s - %ld bytes]\r\r\n",
                       r.filename, r.sofar);
                printf("[%s][INFO] Transfer completed: %s - %ld bytes\r\n", kermit_get_timestamp(), r.filename, r.sofar); fflush(stdout);
                break;

            case X_ERROR:
                /* Protocol error */
                printf("\r\n[Error: Protocol error during transfer]\r\r\n");
                fprintf(stderr, "[%s][ERROR] %s:%d: Protocol error during transfer\r\n", kermit_get_timestamp(), __FILE__, __LINE__);
                kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
                return ERROR_GENERAL;

            default:
                fprintf(stderr, "[%s][WARNING] Unexpected status from kermit(): %d\r\n", kermit_get_timestamp(), status);
                break;
        }
    }

    printf("[%s][INFO] === Kermit Receive Completed Successfully ===\r\n", kermit_get_timestamp()); fflush(stdout);
    kermit_restore_auto_detection(otelnet_ctx, detectors_were_enabled);
    return SUCCESS;
}
