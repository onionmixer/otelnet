/*
 * telnet.c - Telnet client protocol implementation
 */

#include "telnet.h"

/**
 * Initialize telnet structure
 */
void telnet_init(telnet_t *tn)
{
    if (tn == NULL) {
        return;
    }

    memset(tn, 0, sizeof(telnet_t));
    tn->fd = -1;
    tn->is_connected = false;
    tn->state = TELNET_STATE_DATA;

    /* Initialize option tracking */
    memset(tn->local_options, 0, sizeof(tn->local_options));
    memset(tn->remote_options, 0, sizeof(tn->remote_options));

    /* Set default options we support */
    tn->local_options[TELOPT_BINARY] = true;
    tn->local_options[TELOPT_SGA] = true;

    /* Default to line mode until server requests character mode */
    tn->linemode = true;

    /* Set default terminal type and initialize cycle index */
    SAFE_STRNCPY(tn->terminal_type, "XTERM", sizeof(tn->terminal_type));
    tn->ttype_index = 0;

    /* Set default terminal size (RFC 1073) */
    tn->term_width = 80;
    tn->term_height = 24;

    /* Set default terminal speed (RFC 1079) */
    SAFE_STRNCPY(tn->terminal_speed, "38400,38400", sizeof(tn->terminal_speed));

    MB_LOG_DEBUG("Telnet initialized");
}

/**
 * Connect to telnet server
 */
int telnet_connect(telnet_t *tn, const char *host, int port)
{
    struct sockaddr_in server_addr;
    struct hostent *he;

    if (tn == NULL || host == NULL) {
        MB_LOG_ERROR("Invalid arguments to telnet_connect");
        return ERROR_INVALID_ARG;
    }

    if (tn->is_connected) {
        MB_LOG_WARNING("Already connected, disconnecting first");
        telnet_disconnect(tn);
    }

    MB_LOG_INFO("Connecting to telnet server: %s:%d", host, port);

    /* Create socket */
    tn->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tn->fd < 0) {
        MB_LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return ERROR_CONNECTION;
    }

    /* Set non-blocking mode */
    int flags = fcntl(tn->fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(tn->fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* Resolve hostname */
    he = gethostbyname(host);
    if (he == NULL) {
        MB_LOG_ERROR("Failed to resolve host: %s", host);
        close(tn->fd);
        tn->fd = -1;
        return ERROR_CONNECTION;
    }

    /* Setup server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);

    /* Connect to server */
    if (connect(tn->fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        if (errno != EINPROGRESS) {
            MB_LOG_ERROR("Failed to connect: %s", strerror(errno));
            close(tn->fd);
            tn->fd = -1;
            return ERROR_CONNECTION;
        }
        /* Connection in progress for non-blocking socket */
    }

    /* Save connection info */
    SAFE_STRNCPY(tn->host, host, sizeof(tn->host));
    tn->port = port;
    tn->is_connected = true;

    MB_LOG_INFO("Connected to telnet server");

    /* Send initial option negotiations */
    telnet_send_negotiate(tn, TELNET_WILL, TELOPT_BINARY);
    telnet_send_negotiate(tn, TELNET_WILL, TELOPT_SGA);
    telnet_send_negotiate(tn, TELNET_DO, TELOPT_SGA);
    telnet_send_negotiate(tn, TELNET_DO, TELOPT_ECHO);

    /* Offer TERMINAL-TYPE support (RFC 1091) */
    telnet_send_negotiate(tn, TELNET_WILL, TELOPT_TTYPE);

    /* Offer NAWS support (RFC 1073) */
    telnet_send_negotiate(tn, TELNET_WILL, TELOPT_NAWS);

    /* Offer TSPEED support (RFC 1079) */
    telnet_send_negotiate(tn, TELNET_WILL, TELOPT_TSPEED);

    /* Offer ENVIRON support (RFC 1572) */
    telnet_send_negotiate(tn, TELNET_WILL, TELOPT_ENVIRON);

    /* Offer LINEMODE support (RFC 1184) - character mode by default */
    telnet_send_negotiate(tn, TELNET_WILL, TELOPT_LINEMODE);

    return SUCCESS;
}

/**
 * Disconnect from telnet server
 */
int telnet_disconnect(telnet_t *tn)
{
    if (tn == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!tn->is_connected || tn->fd < 0) {
        return SUCCESS;
    }

    MB_LOG_INFO("Disconnecting from telnet server: %s:%d", tn->host, tn->port);

    close(tn->fd);
    tn->fd = -1;
    tn->is_connected = false;

    /* Reset state */
    tn->state = TELNET_STATE_DATA;
    tn->sb_len = 0;

    MB_LOG_INFO("Telnet disconnected");

    return SUCCESS;
}

/**
 * Send IAC command
 */
int telnet_send_command(telnet_t *tn, unsigned char command)
{
    unsigned char buf[2];

    if (tn == NULL || tn->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    buf[0] = TELNET_IAC;
    buf[1] = command;

    MB_LOG_DEBUG("Sending IAC command: %d", command);

    if (send(tn->fd, buf, 2, 0) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            MB_LOG_ERROR("Failed to send IAC command: %s", strerror(errno));
            return ERROR_IO;
        }
    }

    return SUCCESS;
}

/**
 * Send option negotiation
 */
int telnet_send_negotiate(telnet_t *tn, unsigned char command, unsigned char option)
{
    unsigned char buf[3];

    if (tn == NULL || tn->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    buf[0] = TELNET_IAC;
    buf[1] = command;
    buf[2] = option;

    MB_LOG_DEBUG("Sending IAC negotiation: %d %d", command, option);

    if (send(tn->fd, buf, 3, 0) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            MB_LOG_ERROR("Failed to send negotiation: %s", strerror(errno));
            return ERROR_IO;
        }
    }

    return SUCCESS;
}

/**
 * Update line mode vs character mode based on current options
 */
static void telnet_update_mode(telnet_t *tn)
{
    bool old_linemode;

    if (tn == NULL) {
        return;
    }

    old_linemode = tn->linemode;

    /* Update deprecated combined flags for compatibility */
    tn->binary_mode = tn->binary_local || tn->binary_remote;
    tn->sga_mode = tn->sga_local || tn->sga_remote;
    tn->echo_mode = tn->echo_remote;

    /* Character mode: Server echoes (WILL ECHO) and SGA enabled
     * Line mode: Client echoes (WONT ECHO) or no echo negotiation
     * LINEMODE overrides ECHO/SGA detection if active */
    if (tn->linemode_active) {
        tn->linemode = tn->linemode_edit;  /* LINEMODE MODE controls */
    } else if (tn->echo_remote && tn->sga_remote) {
        /* Character mode - server handles echo */
        tn->linemode = false;
        if (old_linemode != tn->linemode) {
            MB_LOG_INFO("Telnet mode: CHARACTER MODE (server echo, SGA enabled)");
        }
    } else {
        /* Line mode - client handles echo */
        tn->linemode = true;
        if (old_linemode != tn->linemode) {
            MB_LOG_INFO("Telnet mode: LINE MODE (client echo)");
        }
    }
}

/**
 * Handle received option negotiation (RFC 855 compliant with loop prevention)
 */
int telnet_handle_negotiate(telnet_t *tn, unsigned char command, unsigned char option)
{
    if (tn == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_DEBUG("Received IAC negotiation: cmd=%d opt=%d", command, option);

    switch (command) {
        case TELNET_WILL:
            /* Server will use option - only respond if state changes (RFC 855) */
            if (option == TELOPT_BINARY || option == TELOPT_SGA || option == TELOPT_ECHO) {
                if (!tn->remote_options[option]) {  /* State change check */
                    tn->remote_options[option] = true;
                    telnet_send_negotiate(tn, TELNET_DO, option);

                    if (option == TELOPT_BINARY) {
                        tn->binary_remote = true;
                        MB_LOG_INFO("Remote BINARY mode enabled");
                    } else if (option == TELOPT_SGA) {
                        tn->sga_remote = true;
                        MB_LOG_INFO("Remote SGA enabled");
                    } else if (option == TELOPT_ECHO) {
                        tn->echo_remote = true;
                        MB_LOG_INFO("Remote ECHO enabled");
                    }
                }
            } else {
                /* Reject unsupported options - send DONT (RFC 855) */
                MB_LOG_DEBUG("Rejecting unsupported option WILL %d", option);
                telnet_send_negotiate(tn, TELNET_DONT, option);
                /* Note: remote_options[option] remains false (not supported) */
            }
            telnet_update_mode(tn);
            break;

        case TELNET_WONT:
            /* Server won't use option - only respond if state changes */
            if (tn->remote_options[option]) {
                tn->remote_options[option] = false;
                telnet_send_negotiate(tn, TELNET_DONT, option);

                if (option == TELOPT_BINARY) {
                    tn->binary_remote = false;
                    MB_LOG_WARNING("Server rejected BINARY mode - multibyte characters (UTF-8, EUC-KR) may be corrupted!");
                } else if (option == TELOPT_SGA) {
                    tn->sga_remote = false;
                } else if (option == TELOPT_ECHO) {
                    tn->echo_remote = false;
                } else if (option == TELOPT_LINEMODE) {
                    tn->linemode_active = false;
                }
            }
            telnet_update_mode(tn);
            break;

        case TELNET_DO:
            /* Server wants us to use option - only respond if state changes */
            if (option == TELOPT_BINARY || option == TELOPT_SGA ||
                option == TELOPT_TTYPE || option == TELOPT_NAWS ||
                option == TELOPT_TSPEED || option == TELOPT_ENVIRON ||
                option == TELOPT_LINEMODE) {
                if (!tn->local_options[option]) {  /* State change check */
                    tn->local_options[option] = true;
                    telnet_send_negotiate(tn, TELNET_WILL, option);

                    if (option == TELOPT_BINARY) {
                        tn->binary_local = true;
                        MB_LOG_INFO("Local BINARY mode enabled");
                    } else if (option == TELOPT_SGA) {
                        tn->sga_local = true;
                        MB_LOG_INFO("Local SGA enabled");
                    } else if (option == TELOPT_TTYPE) {
                        MB_LOG_INFO("TERMINAL-TYPE negotiation accepted");
                        /* Server will send SB TTYPE SEND to request type */
                    } else if (option == TELOPT_NAWS) {
                        MB_LOG_INFO("NAWS negotiation accepted");
                        /* Send initial window size */
                        telnet_send_naws(tn, tn->term_width, tn->term_height);
                    } else if (option == TELOPT_TSPEED) {
                        MB_LOG_INFO("TSPEED negotiation accepted");
                        /* Server will send SB TSPEED SEND to request speed */
                    } else if (option == TELOPT_ENVIRON) {
                        MB_LOG_INFO("ENVIRON negotiation accepted");
                        /* Server will send SB ENVIRON SEND to request variables */
                    } else if (option == TELOPT_LINEMODE) {
                        tn->linemode_active = true;
                        MB_LOG_INFO("LINEMODE negotiation accepted");
                        /* Server may send MODE subnegotiation */
                    }
                }
            } else {
                /* Reject unsupported options - send WONT (RFC 855) */
                MB_LOG_DEBUG("Rejecting unsupported option DO %d", option);
                telnet_send_negotiate(tn, TELNET_WONT, option);
                /* Note: local_options[option] remains false (not supported) */
            }
            telnet_update_mode(tn);
            break;

        case TELNET_DONT:
            /* Server doesn't want us to use option - only respond if state changes */
            if (tn->local_options[option]) {
                tn->local_options[option] = false;
                telnet_send_negotiate(tn, TELNET_WONT, option);

                if (option == TELOPT_BINARY) {
                    tn->binary_local = false;
                    MB_LOG_WARNING("Server rejected local BINARY mode - multibyte characters may be corrupted on send!");
                } else if (option == TELOPT_SGA) {
                    tn->sga_local = false;
                } else if (option == TELOPT_LINEMODE) {
                    tn->linemode_active = false;
                }
            }
            telnet_update_mode(tn);
            break;

        default:
            MB_LOG_WARNING("Unknown negotiation command: %d", command);
            break;
    }

    return SUCCESS;
}

/**
 * Send subnegotiation (helper function)
 */
static int telnet_send_subnegotiation(telnet_t *tn, const unsigned char *data, size_t len)
{
    unsigned char buf[BUFFER_SIZE];
    size_t pos = 0;

    if (tn == NULL || data == NULL || len == 0 || tn->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    /* Build: IAC SB <data...> IAC SE */
    buf[pos++] = TELNET_IAC;
    buf[pos++] = TELNET_SB;

    for (size_t i = 0; i < len && pos < sizeof(buf) - 2; i++) {
        /* Escape IAC in subnegotiation data (RFC 854) */
        if (data[i] == TELNET_IAC) {
            buf[pos++] = TELNET_IAC;
            buf[pos++] = TELNET_IAC;
        } else {
            buf[pos++] = data[i];
        }
    }

    buf[pos++] = TELNET_IAC;
    buf[pos++] = TELNET_SE;

    MB_LOG_DEBUG("Sending subnegotiation: %zu bytes", pos);

    if (send(tn->fd, buf, pos, 0) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            MB_LOG_ERROR("Failed to send subnegotiation: %s", strerror(errno));
            return ERROR_IO;
        }
    }

    return SUCCESS;
}

/**
 * Send NAWS (Negotiate About Window Size) subnegotiation (RFC 1073)
 * Format: IAC SB NAWS WIDTH[1] WIDTH[0] HEIGHT[1] HEIGHT[0] IAC SE
 * Width and height are 16-bit values in network byte order (big-endian)
 * IAC bytes (255) in the data must be escaped as IAC IAC
 */
int telnet_send_naws(telnet_t *tn, int width, int height)
{
    unsigned char data[5];  /* NAWS option + 2 bytes width + 2 bytes height */

    if (tn == NULL || width < 0 || height < 0 || width > 65535 || height > 65535) {
        return ERROR_INVALID_ARG;
    }

    /* Build subnegotiation data: NAWS WIDTH[1] WIDTH[0] HEIGHT[1] HEIGHT[0] */
    data[0] = TELOPT_NAWS;
    data[1] = (unsigned char)((width >> 8) & 0xFF);   /* Width high byte */
    data[2] = (unsigned char)(width & 0xFF);           /* Width low byte */
    data[3] = (unsigned char)((height >> 8) & 0xFF);  /* Height high byte */
    data[4] = (unsigned char)(height & 0xFF);          /* Height low byte */

    MB_LOG_INFO("Sending NAWS: %dx%d", width, height);

    return telnet_send_subnegotiation(tn, data, 5);
}

/**
 * Handle subnegotiation (RFC 1091 TERMINAL-TYPE, RFC 1184 LINEMODE)
 */
int telnet_handle_subnegotiation(telnet_t *tn)
{
    if (tn == NULL || tn->sb_len < 1) {
        return ERROR_INVALID_ARG;
    }

    unsigned char option = tn->sb_buffer[0];

    MB_LOG_DEBUG("Received subnegotiation for option %d, length %zu", (int)option, tn->sb_len);

    switch (option) {
        case TELOPT_TTYPE:
            /* TERMINAL-TYPE subnegotiation (RFC 1091) with multi-type support */
            if (tn->sb_len >= 2 && tn->sb_buffer[1] == TTYPE_SEND) {
                /* Server requests terminal type - cycle through supported types */
                const char *terminal_types[] = {"XTERM", "VT100", "ANSI"};
                const int num_types = 3;

                /* Get current terminal type from cycle */
                const char *current_type = terminal_types[tn->ttype_index % num_types];

                /* Update stored terminal type */
                SAFE_STRNCPY(tn->terminal_type, current_type, sizeof(tn->terminal_type));

                /* Prepare response */
                unsigned char response[68];  /* 1 (option) + 1 (IS) + 64 (terminal type) + 2 safety */
                size_t term_len = strlen(tn->terminal_type);

                response[0] = TELOPT_TTYPE;
                response[1] = TTYPE_IS;
                memcpy(&response[2], tn->terminal_type, term_len);

                MB_LOG_INFO("Sending TERMINAL-TYPE IS %s (cycle %d)", tn->terminal_type, tn->ttype_index);
                telnet_send_subnegotiation(tn, response, 2 + term_len);

                /* Advance to next type for next request */
                tn->ttype_index++;

                /* RFC 1091: After cycling through all types, repeat the cycle */
                /* This allows the server to detect when we've looped */
            }
            break;

        case TELOPT_TSPEED:
            /* TERMINAL-SPEED subnegotiation (RFC 1079) */
            if (tn->sb_len >= 2 && tn->sb_buffer[1] == TTYPE_SEND) {  /* SEND = 1 */
                /* Server requests terminal speed - send IS response */
                unsigned char response[36];  /* 1 (option) + 1 (IS) + 32 (speed) + 2 safety */
                size_t speed_len = strlen(tn->terminal_speed);

                response[0] = TELOPT_TSPEED;
                response[1] = TTYPE_IS;  /* IS = 0 */
                memcpy(&response[2], tn->terminal_speed, speed_len);

                MB_LOG_INFO("Sending TSPEED IS %s", tn->terminal_speed);
                telnet_send_subnegotiation(tn, response, 2 + speed_len);
            }
            break;

        case TELOPT_ENVIRON:
            /* ENVIRON subnegotiation (RFC 1572) */
            if (tn->sb_len >= 2 && tn->sb_buffer[1] == ENV_SEND) {
                /* Server requests environment variables - send IS response */
                unsigned char response[BUFFER_SIZE];
                size_t pos = 0;

                response[pos++] = TELOPT_ENVIRON;
                response[pos++] = ENV_IS;

                /* Send USER variable if available */
                const char *user = getenv("USER");
                if (user != NULL && strlen(user) > 0 && strlen(user) < 64) {
                    response[pos++] = ENV_VAR;
                    const char *var_name = "USER";
                    size_t name_len = strlen(var_name);
                    memcpy(&response[pos], var_name, name_len);
                    pos += name_len;

                    response[pos++] = ENV_VALUE;
                    size_t user_len = strlen(user);
                    memcpy(&response[pos], user, user_len);
                    pos += user_len;

                    MB_LOG_DEBUG("Sending ENVIRON: USER=%s", user);
                }

                /* Send DISPLAY variable if available (for X11) */
                const char *display = getenv("DISPLAY");
                if (display != NULL && strlen(display) > 0 && strlen(display) < 64) {
                    response[pos++] = ENV_VAR;
                    const char *var_name = "DISPLAY";
                    size_t name_len = strlen(var_name);
                    memcpy(&response[pos], var_name, name_len);
                    pos += name_len;

                    response[pos++] = ENV_VALUE;
                    size_t display_len = strlen(display);
                    memcpy(&response[pos], display, display_len);
                    pos += display_len;

                    MB_LOG_DEBUG("Sending ENVIRON: DISPLAY=%s", display);
                }

                if (pos > 2) {  /* If we added any variables */
                    MB_LOG_INFO("Sending ENVIRON IS with %zu bytes", pos);
                    telnet_send_subnegotiation(tn, response, pos);
                } else {
                    MB_LOG_INFO("No environment variables to send");
                }
            }
            break;

        case TELOPT_LINEMODE:
            /* LINEMODE subnegotiation (RFC 1184) */
            if (tn->sb_len >= 2 && tn->sb_buffer[1] == LM_MODE) {
                /* MODE subnegotiation */
                if (tn->sb_len >= 3) {
                    unsigned char mode = tn->sb_buffer[2];
                    bool old_edit = tn->linemode_edit;

                    tn->linemode_edit = (mode & MODE_EDIT) != 0;

                    MB_LOG_INFO("LINEMODE MODE: EDIT=%s TRAPSIG=%s",
                               (mode & MODE_EDIT) ? "yes" : "no",
                               (mode & MODE_TRAPSIG) ? "yes" : "no");

                    /* Send ACK if MODE_ACK bit is set (RFC 1184 mode synchronization) */
                    if (mode & MODE_ACK) {
                        unsigned char response[3];
                        response[0] = TELOPT_LINEMODE;
                        response[1] = LM_MODE;
                        response[2] = mode;  /* Echo back the same mode */

                        MB_LOG_DEBUG("Sending LINEMODE MODE ACK");
                        telnet_send_subnegotiation(tn, response, 3);
                    }

                    /* Update mode if edit flag changed */
                    if (old_edit != tn->linemode_edit) {
                        telnet_update_mode(tn);
                    }
                }
            } else if (tn->sb_len >= 2 && tn->sb_buffer[1] == LM_FORWARDMASK) {
                /* FORWARDMASK - acknowledge but don't implement for now */
                MB_LOG_DEBUG("Received LINEMODE FORWARDMASK (not implemented)");
            } else if (tn->sb_len >= 2 && tn->sb_buffer[1] == LM_SLC) {
                /* SLC (Set Local Characters) - acknowledge but don't implement for now */
                MB_LOG_DEBUG("Received LINEMODE SLC (not implemented)");
            }
            break;

        default:
            /* Unknown option - just log and ignore */
            MB_LOG_DEBUG("Ignoring subnegotiation for unsupported option %d", option);
            break;
    }

    return SUCCESS;
}

/**
 * Process incoming data from telnet server
 */
int telnet_process_input(telnet_t *tn, const unsigned char *input, size_t input_len,
                         unsigned char *output, size_t output_size, size_t *output_len)
{
    size_t out_pos = 0;

    if (tn == NULL || input == NULL || output == NULL || output_len == NULL) {
        return ERROR_INVALID_ARG;
    }

    *output_len = 0;

    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = input[i];

        switch (tn->state) {
            case TELNET_STATE_DATA:
                if (c == TELNET_IAC) {
                    tn->state = TELNET_STATE_IAC;
                } else if (c == '\r' && !tn->binary_remote) {
                    /* CR in non-binary mode - need to check next byte (RFC 854) */
                    tn->state = TELNET_STATE_SEENCR;
                } else {
                    /* Regular data */
                    if (out_pos < output_size) {
                        output[out_pos++] = c;
                    } else {
                        /* Buffer full - log warning once */
                        static bool overflow_warned = false;
                        if (!overflow_warned) {
                            MB_LOG_WARNING("Telnet input buffer full - data may be truncated (multibyte chars may break)");
                            overflow_warned = true;
                        }
                    }
                }
                break;

            case TELNET_STATE_IAC:
                if (c == TELNET_IAC) {
                    /* Escaped IAC - output single IAC */
                    if (out_pos < output_size) {
                        output[out_pos++] = TELNET_IAC;
                    }
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_WILL) {
                    tn->state = TELNET_STATE_WILL;
                } else if (c == TELNET_WONT) {
                    tn->state = TELNET_STATE_WONT;
                } else if (c == TELNET_DO) {
                    tn->state = TELNET_STATE_DO;
                } else if (c == TELNET_DONT) {
                    tn->state = TELNET_STATE_DONT;
                } else if (c == TELNET_SB) {
                    tn->state = TELNET_STATE_SB;
                    tn->sb_len = 0;
                } else if (c == TELNET_GA) {
                    /* Go Ahead - silently ignore in character mode (RFC 858) */
                    MB_LOG_DEBUG("Received IAC GA (ignored)");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_NOP) {
                    /* No Operation - silently ignore (RFC 854) */
                    MB_LOG_DEBUG("Received IAC NOP");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_AYT) {
                    /* Are You There - respond with confirmation (RFC 854) */
                    MB_LOG_DEBUG("Received IAC AYT");
                    const char *response = "\r\n[ModemBridge: Yes, I'm here]\r\n";
                    telnet_send(tn, response, strlen(response));
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_IP) {
                    /* Interrupt Process - log but don't act (RFC 854) */
                    MB_LOG_INFO("Received IAC IP (Interrupt Process)");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_AO) {
                    /* Abort Output - log but don't act (RFC 854) */
                    MB_LOG_INFO("Received IAC AO (Abort Output)");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_BREAK) {
                    /* Break - log but don't act (RFC 854) */
                    MB_LOG_INFO("Received IAC BREAK");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_EL) {
                    /* Erase Line - log but don't act (RFC 854) */
                    MB_LOG_DEBUG("Received IAC EL (Erase Line)");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_EC) {
                    /* Erase Character - log but don't act (RFC 854) */
                    MB_LOG_DEBUG("Received IAC EC (Erase Character)");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_DM) {
                    /* Data Mark - marks end of urgent data (RFC 854) */
                    MB_LOG_DEBUG("Received IAC DM (Data Mark)");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_EOR) {
                    /* End of Record - log but don't act (RFC 885) */
                    MB_LOG_DEBUG("Received IAC EOR (End of Record)");
                    tn->state = TELNET_STATE_DATA;
                } else {
                    /* Unknown IAC command - log and ignore */
                    MB_LOG_WARNING("Received unknown IAC command: %d", c);
                    tn->state = TELNET_STATE_DATA;
                }
                break;

            case TELNET_STATE_WILL:
                telnet_handle_negotiate(tn, TELNET_WILL, c);
                tn->state = TELNET_STATE_DATA;
                break;

            case TELNET_STATE_WONT:
                telnet_handle_negotiate(tn, TELNET_WONT, c);
                tn->state = TELNET_STATE_DATA;
                break;

            case TELNET_STATE_DO:
                telnet_handle_negotiate(tn, TELNET_DO, c);
                tn->state = TELNET_STATE_DATA;
                break;

            case TELNET_STATE_DONT:
                telnet_handle_negotiate(tn, TELNET_DONT, c);
                tn->state = TELNET_STATE_DATA;
                break;

            case TELNET_STATE_SB:
                if (c == TELNET_IAC) {
                    tn->state = TELNET_STATE_SB_IAC;
                } else {
                    /* Accumulate subnegotiation data */
                    if (tn->sb_len < sizeof(tn->sb_buffer)) {
                        tn->sb_buffer[tn->sb_len++] = c;
                    }
                }
                break;

            case TELNET_STATE_SB_IAC:
                if (c == TELNET_SE) {
                    /* End of subnegotiation */
                    telnet_handle_subnegotiation(tn);
                    tn->sb_len = 0;
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_IAC) {
                    /* Escaped IAC in subnegotiation */
                    if (tn->sb_len < sizeof(tn->sb_buffer)) {
                        tn->sb_buffer[tn->sb_len++] = TELNET_IAC;
                    }
                    tn->state = TELNET_STATE_SB;
                } else {
                    /* Invalid sequence - return to SB state */
                    if (tn->sb_len < sizeof(tn->sb_buffer)) {
                        tn->sb_buffer[tn->sb_len++] = c;
                    }
                    tn->state = TELNET_STATE_SB;
                }
                break;

            case TELNET_STATE_SEENCR:
                /* RFC 854: CR must be followed by NUL or LF in non-binary mode
                 * CR NUL means carriage return only
                 * CR LF means newline
                 * CR <other> is illegal, treat as CR followed by the character */
                if (c == '\0') {
                    /* CR NUL - output just CR */
                    if (out_pos < output_size) {
                        output[out_pos++] = '\r';
                    }
                    MB_LOG_DEBUG("Received CR NUL (carriage return only)");
                } else if (c == '\n') {
                    /* CR LF - output CR LF (newline) */
                    if (out_pos + 1 < output_size) {
                        output[out_pos++] = '\r';
                        output[out_pos++] = '\n';
                    } else if (out_pos < output_size) {
                        /* Only room for CR */
                        output[out_pos++] = '\r';
                    }
                    MB_LOG_DEBUG("Received CR LF (newline)");
                } else if (c == TELNET_IAC) {
                    /* CR IAC - output CR and process IAC */
                    if (out_pos < output_size) {
                        output[out_pos++] = '\r';
                    }
                    tn->state = TELNET_STATE_IAC;
                    break;
                } else {
                    /* CR followed by other character - output CR and process character normally */
                    if (out_pos < output_size) {
                        output[out_pos++] = '\r';
                    }
                    if (out_pos < output_size) {
                        output[out_pos++] = c;
                    }
                    MB_LOG_DEBUG("Received CR followed by 0x%02x (non-standard)", c);
                }
                tn->state = TELNET_STATE_DATA;
                break;

            default:
                MB_LOG_WARNING("Invalid telnet state: %d", tn->state);
                tn->state = TELNET_STATE_DATA;
                break;
        }
    }

    *output_len = out_pos;

    if (out_pos > 0) {
        MB_LOG_DEBUG("Telnet processed %zu bytes -> %zu bytes", input_len, out_pos);
    }

    return SUCCESS;
}

/**
 * Prepare data for sending to telnet server (escape IAC bytes)
 */
int telnet_prepare_output(telnet_t *tn, const unsigned char *input, size_t input_len,
                          unsigned char *output, size_t output_size, size_t *output_len)
{
    size_t out_pos = 0;

    if (tn == NULL || input == NULL || output == NULL || output_len == NULL) {
        return ERROR_INVALID_ARG;
    }

    *output_len = 0;

    size_t i;
    for (i = 0; i < input_len; i++) {
        unsigned char c = input[i];

        if (c == TELNET_IAC) {
            /* Escape IAC by doubling it */
            if (out_pos + 1 < output_size) {
                output[out_pos++] = TELNET_IAC;
                output[out_pos++] = TELNET_IAC;
            } else {
                /* Output buffer full */
                break;
            }
        } else {
            /* Regular character */
            if (out_pos < output_size) {
                output[out_pos++] = c;
            } else {
                /* Output buffer full */
                break;
            }
        }
    }

    *output_len = out_pos;

    /* Warn if not all input was processed */
    if (i < input_len) {
        MB_LOG_WARNING("Telnet output buffer full - %zu of %zu bytes not processed (multibyte chars may break)",
                      input_len - i, input_len);
    }

    if (out_pos > 0) {
        MB_LOG_DEBUG("Telnet prepared %zu bytes -> %zu bytes", input_len, out_pos);
    }

    return SUCCESS;
}

/**
 * Send data to telnet server
 */
ssize_t telnet_send(telnet_t *tn, const void *data, size_t len)
{
    ssize_t sent;

    if (tn == NULL || data == NULL || tn->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!tn->is_connected) {
        return ERROR_CONNECTION;
    }

    MB_LOG_DEBUG("Telnet sending %zu bytes", len);

    sent = send(tn->fd, data, len, 0);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Would block */
            return 0;
        }
        MB_LOG_ERROR("Telnet send error: %s", strerror(errno));
        return ERROR_IO;
    }

    return sent;
}

/**
 * Receive data from telnet server
 */
ssize_t telnet_recv(telnet_t *tn, void *buffer, size_t size)
{
    ssize_t n;

    if (tn == NULL || buffer == NULL || tn->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!tn->is_connected) {
        return ERROR_CONNECTION;
    }

    n = recv(tn->fd, buffer, size, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* No data available */
            return 0;
        }
        MB_LOG_ERROR("Telnet recv error: %s", strerror(errno));
        return ERROR_IO;
    }

    if (n == 0) {
        /* Connection closed */
        MB_LOG_INFO("Telnet connection closed by server");
        tn->is_connected = false;
        return 0;
    }

    MB_LOG_DEBUG("Telnet received %zd bytes", n);

    return n;
}

/**
 * Get file descriptor for select/poll
 */
int telnet_get_fd(telnet_t *tn)
{
    if (tn == NULL) {
        return -1;
    }

    return tn->fd;
}

/**
 * Check if connected to telnet server
 */
bool telnet_is_connected(telnet_t *tn)
{
    if (tn == NULL) {
        return false;
    }

    return tn->is_connected && tn->fd >= 0;
}

/**
 * Check if in line mode
 */
bool telnet_is_linemode(telnet_t *tn)
{
    if (tn == NULL) {
        return true;
    }

    return tn->linemode;
}

/**
 * Check if in binary mode
 */
bool telnet_is_binary_mode(telnet_t *tn)
{
    if (tn == NULL) {
        return false;
    }

    return tn->binary_mode;
}

/**
 * Print current telnet mode and binary mode state (DEBUG only)
 */
void telnet_debug_print_mode(telnet_t *tn, const char *prefix)
{
    if (tn == NULL || prefix == NULL) {
        return;
    }

#ifdef DEBUG
    const char *mode_str;
    const char *binary_str;

    /* Determine telnet mode */
    if (tn->linemode_active) {
        mode_str = tn->linemode_edit ? "LINE MODE (LINEMODE option active)"
                                     : "CHARACTER MODE (LINEMODE option active)";
    } else if (tn->linemode) {
        mode_str = "LINE MODE (client echo)";
    } else {
        mode_str = "CHARACTER MODE (server echo, SGA enabled)";
    }

    /* Determine binary mode status */
    if (tn->binary_local && tn->binary_remote) {
        binary_str = "BINARY MODE (bidirectional)";
    } else if (tn->binary_local) {
        binary_str = "BINARY MODE (local only)";
    } else if (tn->binary_remote) {
        binary_str = "BINARY MODE (remote only)";
    } else {
        binary_str = "NORMAL MODE (7-bit ASCII)";
    }

    MB_LOG_DEBUG("%s: Telnet mode: %s, Binary mode: %s", prefix, mode_str, binary_str);
    MB_LOG_DEBUG("%s: State details - BINARY(L=%d,R=%d) ECHO(L=%d,R=%d) SGA(L=%d,R=%d) LINEMODE=%d",
                 prefix,
                 tn->binary_local, tn->binary_remote,
                 tn->echo_local, tn->echo_remote,
                 tn->sga_local, tn->sga_remote,
                 tn->linemode_active);
#else
    /* Suppress unused parameter warning in non-DEBUG builds */
    (void)tn;
    (void)prefix;
#endif
}

/**
 * Save current telnet protocol state (for file transfers)
 */
int telnet_save_state(telnet_t *tn,
                      bool *saved_binary_local, bool *saved_binary_remote,
                      bool *saved_echo_local, bool *saved_echo_remote,
                      bool *saved_sga_local, bool *saved_sga_remote,
                      bool *saved_linemode_active)
{
    if (tn == NULL || saved_binary_local == NULL || saved_binary_remote == NULL ||
        saved_echo_local == NULL || saved_echo_remote == NULL ||
        saved_sga_local == NULL || saved_sga_remote == NULL ||
        saved_linemode_active == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Save all protocol states */
    *saved_binary_local = tn->binary_local;
    *saved_binary_remote = tn->binary_remote;
    *saved_echo_local = tn->echo_local;
    *saved_echo_remote = tn->echo_remote;
    *saved_sga_local = tn->sga_local;
    *saved_sga_remote = tn->sga_remote;
    *saved_linemode_active = tn->linemode_active;

    MB_LOG_INFO("Saved telnet state: BINARY(L=%d,R=%d) ECHO(L=%d,R=%d) SGA(L=%d,R=%d) LINEMODE=%d",
                *saved_binary_local, *saved_binary_remote,
                *saved_echo_local, *saved_echo_remote,
                *saved_sga_local, *saved_sga_remote,
                *saved_linemode_active);

    return SUCCESS;
}

/**
 * Request BINARY mode for file transfers
 */
int telnet_request_binary_mode(telnet_t *tn)
{
    if (tn == NULL || tn->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Requesting BINARY mode for file transfer");

    /* Request to send binary (WILL BINARY) only if not already enabled */
    if (!tn->local_options[TELOPT_BINARY]) {
        telnet_send_negotiate(tn, TELNET_WILL, TELOPT_BINARY);
        tn->local_options[TELOPT_BINARY] = true;
        tn->binary_local = true;
    }

    /* Request remote to send binary (DO BINARY) only if not already enabled */
    if (!tn->remote_options[TELOPT_BINARY]) {
        telnet_send_negotiate(tn, TELNET_DO, TELOPT_BINARY);
        tn->remote_options[TELOPT_BINARY] = true;
        tn->binary_remote = true;
    }

    telnet_update_mode(tn);

    return SUCCESS;
}

/**
 * Restore telnet protocol state after file transfers
 * Restores BINARY, ECHO, SGA, and LINEMODE states to original values
 */
int telnet_restore_state(telnet_t *tn,
                        bool saved_binary_local, bool saved_binary_remote,
                        bool saved_echo_local, bool saved_echo_remote,
                        bool saved_sga_local, bool saved_sga_remote,
                        bool saved_linemode_active)
{
    if (tn == NULL || tn->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Restoring telnet state: BINARY(L=%d,R=%d) ECHO(L=%d,R=%d) SGA(L=%d,R=%d) LINEMODE=%d",
                saved_binary_local, saved_binary_remote,
                saved_echo_local, saved_echo_remote,
                saved_sga_local, saved_sga_remote,
                saved_linemode_active);

    /* Restore local BINARY mode state */
    if (saved_binary_local && !tn->binary_local) {
        MB_LOG_INFO("Re-enabling local BINARY mode");
        telnet_send_negotiate(tn, TELNET_WILL, TELOPT_BINARY);
        tn->local_options[TELOPT_BINARY] = true;
        tn->binary_local = true;
    } else if (!saved_binary_local && tn->binary_local) {
        MB_LOG_INFO("Disabling local BINARY mode");
        telnet_send_negotiate(tn, TELNET_WONT, TELOPT_BINARY);
        tn->local_options[TELOPT_BINARY] = false;
        tn->binary_local = false;
    }

    /* Restore remote BINARY mode state */
    if (saved_binary_remote && !tn->binary_remote) {
        MB_LOG_INFO("Re-requesting remote BINARY mode");
        telnet_send_negotiate(tn, TELNET_DO, TELOPT_BINARY);
        tn->remote_options[TELOPT_BINARY] = true;
        tn->binary_remote = true;
    } else if (!saved_binary_remote && tn->binary_remote) {
        MB_LOG_INFO("Stopping remote BINARY mode");
        telnet_send_negotiate(tn, TELNET_DONT, TELOPT_BINARY);
        tn->remote_options[TELOPT_BINARY] = false;
        tn->binary_remote = false;
    }

    /* Restore local ECHO mode state */
    if (saved_echo_local && !tn->echo_local) {
        MB_LOG_INFO("Re-enabling local ECHO mode");
        telnet_send_negotiate(tn, TELNET_WILL, TELOPT_ECHO);
        tn->local_options[TELOPT_ECHO] = true;
        tn->echo_local = true;
    } else if (!saved_echo_local && tn->echo_local) {
        MB_LOG_INFO("Disabling local ECHO mode");
        telnet_send_negotiate(tn, TELNET_WONT, TELOPT_ECHO);
        tn->local_options[TELOPT_ECHO] = false;
        tn->echo_local = false;
    }

    /* Restore remote ECHO mode state */
    if (saved_echo_remote && !tn->echo_remote) {
        MB_LOG_INFO("Re-requesting remote ECHO mode");
        telnet_send_negotiate(tn, TELNET_DO, TELOPT_ECHO);
        tn->remote_options[TELOPT_ECHO] = true;
        tn->echo_remote = true;
    } else if (!saved_echo_remote && tn->echo_remote) {
        MB_LOG_INFO("Stopping remote ECHO mode");
        telnet_send_negotiate(tn, TELNET_DONT, TELOPT_ECHO);
        tn->remote_options[TELOPT_ECHO] = false;
        tn->echo_remote = false;
    }

    /* Restore local SGA mode state */
    if (saved_sga_local && !tn->sga_local) {
        MB_LOG_INFO("Re-enabling local SGA mode");
        telnet_send_negotiate(tn, TELNET_WILL, TELOPT_SGA);
        tn->local_options[TELOPT_SGA] = true;
        tn->sga_local = true;
    } else if (!saved_sga_local && tn->sga_local) {
        MB_LOG_INFO("Disabling local SGA mode");
        telnet_send_negotiate(tn, TELNET_WONT, TELOPT_SGA);
        tn->local_options[TELOPT_SGA] = false;
        tn->sga_local = false;
    }

    /* Restore remote SGA mode state */
    if (saved_sga_remote && !tn->sga_remote) {
        MB_LOG_INFO("Re-requesting remote SGA mode");
        telnet_send_negotiate(tn, TELNET_DO, TELOPT_SGA);
        tn->remote_options[TELOPT_SGA] = true;
        tn->sga_remote = true;
    } else if (!saved_sga_remote && tn->sga_remote) {
        MB_LOG_INFO("Stopping remote SGA mode");
        telnet_send_negotiate(tn, TELNET_DONT, TELOPT_SGA);
        tn->remote_options[TELOPT_SGA] = false;
        tn->sga_remote = false;
    }

    /* Restore LINEMODE active state */
    if (saved_linemode_active != tn->linemode_active) {
        tn->linemode_active = saved_linemode_active;
        MB_LOG_INFO("Restored LINEMODE active state: %d", saved_linemode_active);
    }

    /* Update calculated mode (char/line mode) based on restored states */
    telnet_update_mode(tn);

    return SUCCESS;
}
