# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**otelnet** is a standalone telnet client with integrated file transfer support. It implements RFC 854 telnet protocol with full multibyte character (UTF-8) support and provides a console mode for executing file transfer commands without disconnecting from the telnet session.

**Key Characteristics:**
- Pure C implementation (C11/GNU11)
- Zero external dependencies beyond glibc and POSIX APIs
- Integration with external file transfer programs (sz/rz, kermit)
- Optional session logging with hex+ASCII dump format
- Minimum requirement: Ubuntu 22.04 LTS

## Build Commands

```bash
# Standard build
make

# Clean build
make clean && make

# Debug build (with symbols, no optimization)
make debug
# or
make DEBUG=1

# Static build (statically linked binary)
make static

# Install system-wide (requires root)
sudo make install

# Uninstall
sudo make uninstall
```

## Running the Application

```bash
# Basic usage
./build/otelnet <host> <port>

# Examples
./build/otelnet localhost 23
./build/otelnet 192.168.1.100 8881

# With custom config file
./build/otelnet <host> <port> -c myconfig.conf

# View help
./build/otelnet --help
```

## Architecture Overview

### Component Structure

```
┌─────────────────────────────────────┐
│         otelnet.c (Main)            │  Event loop, UI, commands
├─────────────────────────────────────┤
│       telnet.c (Protocol)           │  RFC 854 implementation
├─────────────────────────────────────┤
│   POSIX (termios, socket, select)   │  System APIs
└─────────────────────────────────────┘
```

### Critical Data Flow

**User Input → Telnet Server:**
1. `otelnet_process_stdin()` reads from stdin
2. Detects Ctrl+] for console mode or passes to telnet
3. `telnet_prepare_output()` escapes IAC bytes (0xFF → 0xFF 0xFF)
4. `telnet_send()` transmits to server
5. Optional: `otelnet_log_data()` logs sent data

**Telnet Server → User Display:**
1. `otelnet_process_telnet()` receives data
2. `telnet_recv()` reads from socket
3. `telnet_process_input()` parses IAC sequences, handles option negotiation
4. Clean data written to stdout
5. Optional: `otelnet_log_data()` logs received data

### State Management

**Application Modes** (otelnet.h):
- `OTELNET_MODE_CLIENT`: Normal telnet client mode (transparent data passing)
- `OTELNET_MODE_CONSOLE`: Console command mode (Ctrl+] pressed)

**Telnet Protocol State** (telnet.h):
- State machine in `telnet_process_input()` for IAC sequence parsing
- Tracks `TELNET_STATE_DATA`, `TELNET_STATE_IAC`, `TELNET_STATE_WILL/WONT/DO/DONT`, etc.

**Echo Mode**:
- `ctx->telnet.echo_remote`: Server echo status (WILL ECHO = true)
- Local echo activated when `!echo_remote` to show user input

### Key Design Patterns

**I/O Multiplexing:**
- `otelnet_run()` uses `select()` to monitor both stdin and telnet socket
- Non-blocking I/O throughout for responsive handling
- 1-second timeout allows periodic health checks and signal processing

**Signal Handling:**
- `SIGINT` (Ctrl+C) and `SIGTERM` trigger graceful shutdown via `g_running_local` flag
- `SIGPIPE` ignored to prevent crashes on broken pipe writes
- Terminal always restored on exit, even when interrupted

**Console Mode:**
- Ctrl+] triggers transition to console mode
- Commands executed without disconnecting telnet session
- Empty line returns to client mode
- Separate input buffer for command accumulation

**External Program Integration:**
- `fork()`/`exec()` pattern for running sz/rz/kermit
- File descriptors redirected: telnet socket → stdin/stdout/stderr of child
- Terminal restored before exec, re-set to raw mode after child exits
- Wait for child completion and report exit status

**Protocol Translation:**
- Telnet IAC escaping is bidirectional (0xFF must be escaped as 0xFF 0xFF)
- UTF-8 sequences preserved (BINARY mode negotiation)
- No ANSI filtering (unlike modembridge which filters cursor codes)

## Important Implementation Details

### Logging Macros

All logging uses prefixed macros to avoid conflicts with syslog.h constants:
- `MB_LOG_DEBUG()` - only active in DEBUG builds
- `MB_LOG_INFO()` - standard operational messages
- `MB_LOG_WARNING()` - non-critical issues
- `MB_LOG_ERROR()` - errors with context (file:line)

Never use `LOG_*` macros directly as they conflict with syslog priority constants.

### Terminal Configuration

Terminal setup in `otelnet_setup_terminal()` uses POSIX termios:
- Must save original settings (`orig_termios`) for restoration on close
- Raw mode: disable ICANON, ECHO, ISIG, and all input/output processing
- Non-blocking: `VMIN=0, VTIME=1` (100ms timeout) with `O_NONBLOCK` flag
- Local echo implemented in application when server doesn't echo

### Telnet Protocol Compliance

The telnet implementation (`telnet.c`) must:
- Always escape 0xFF bytes as 0xFF 0xFF when sending data
- Negotiate BINARY mode for UTF-8/multibyte character support
- Respond to DO/DONT/WILL/WONT within the state machine (automatic handling)
- Support both linemode and character mode transparently

Current implementation accepts BINARY and SGA options, rejects others.

### File Transfer Integration

External programs are executed via `otelnet_execute_external_program_with_args()`:
- Program existence checked before execution (`access()` with X_OK)
- Terminal restored to cooked mode before fork
- Telnet socket FD duplicated to stdin/stdout/stderr in child
- Parent waits for child and reports exit status
- Terminal returned to raw mode after child exits

Protocol options:
- `sz`/`rz` support `--xmodem`, `--ymodem`, `--zmodem` options
- Alias commands: `sx` (XMODEM send), `sy` (YMODEM send), `rx`, `ry`
- Automatic protocol detection from command name or first argument

### Session Logging

When `LOG=1` in configuration:
- `otelnet_open_log()` opens log file and writes session start marker
- `otelnet_log_data()` writes hex+ASCII dump with timestamp and direction
- Format: `[timestamp][send|receive] hex_data | ascii_representation`
- Logged after IAC processing (clean data, not raw protocol bytes)
- `otelnet_close_log()` writes session end marker on exit

### Configuration File Format

`otelnet.conf` uses simple `KEY=VALUE` format:
- Lines starting with `#` are comments
- Values can be quoted: `KERMIT="kermit"`
- Parser in `otelnet_load_config()` trims whitespace and validates
- Invalid values fall back to safe defaults with warnings

Configuration file search order:
1. Path specified via `-c` command-line option
2. `otelnet.conf` in current working directory (default)
3. If not found, defaults are used (not an error)

Supported settings:
- `KERMIT`: Path to kermit program (default: "kermit")
- `SEND_ZMODEM`: Path to sz program (default: "sz")
- `RECEIVE_ZMODEM`: Path to rz program (default: "rz")
- `LOG`: Enable logging (1/0, true/false, yes/no)
- `LOG_FILE`: Log file path (default: "otelnet.log")

## Code Modification Guidelines

**When adding new console commands:**
- Add handling in `otelnet_process_console_command()` switch/if-else chain
- Update help text in the "help" command handler
- Follow existing pattern: parse args, validate, execute, provide feedback

**When modifying telnet protocol:**
- Ensure IAC escaping remains correct in both directions
- Test with both character-mode and line-mode telnet servers
- Verify UTF-8 multibyte sequences are not corrupted
- Unsupported options must be rejected with DONT/WONT immediately (RFC 855 compliance)
- Option negotiation should prevent loops by checking state changes

**When changing terminal handling:**
- Always save original termios settings
- Restore terminal on all exit paths (normal and error)
- Be careful with signal handlers that may terminate program

**UTF-8 Character Safety:**
UTF-8 helper functions available in `otelnet.c`:
- `is_utf8_start()` - detects UTF-8 sequence start bytes
- `is_utf8_continuation()` - identifies continuation bytes
- `utf8_sequence_length()` - calculates expected sequence length
- These are marked `__attribute__((unused))` as they're reserved for future use

## Common Issues

**Compilation errors about LOG_* macros:**
- Use `MB_LOG_*` macros instead of `LOG_*`
- Include `<syslog.h>` before defining logging macros in otelnet.h

**Terminal not restored on crash:**
- Run `reset` or `stty sane` to restore terminal
- Consider adding signal handlers for graceful cleanup

**File transfer program not found:**
- Check PATH environment variable
- Install lrzsz package: `sudo apt install lrzsz`
- Or specify full path in otelnet.conf: `SEND_ZMODEM=/usr/bin/sz`

**Input not visible:**
- This is expected when server sends WONT ECHO
- otelnet automatically enables local echo in this case
- Check `ctx->telnet.echo_remote` flag status

**Log file permission denied:**
- Check write permissions in log file directory
- Try absolute path in LOG_FILE setting
- Ensure directory exists before running otelnet

## Testing Notes

**Telnet Server Testing:**
- Any RFC 854 compliant telnet server works
- For local testing: `busybox telnetd -p 8881 -f /dev/null`
- Or use netcat as simple echo server: `nc -l 8881`
- Or use xinetd with telnet service configuration
- Test with character-mode and line-mode servers to verify both work correctly

**File Transfer Testing:**
- Requires compatible receiver/sender on remote end
- For ZMODEM: remote must support rz/sz commands
- For Kermit: remote must have kermit installed
- Test both send and receive directions

**Console Mode Testing:**
- Press Ctrl+] to enter console mode
- Test: help, stats, ls, pwd, cd, quit
- Test empty line to return to client mode
- Test file transfers with various protocols

**UTF-8 Testing:**
- Connect to server that sends UTF-8 characters
- Verify multibyte characters display correctly
- Check log file for proper byte sequences

## Dependencies

**Build Dependencies:**
- gcc (C11/GNU11 support)
- make
- Standard C library (glibc)

**Runtime Dependencies (Optional):**
- lrzsz package (for sz/rz commands)
- ckermit package (for kermit command)

**System Requirements:**
- Linux (POSIX APIs: termios, select, fork, exec)
- Ubuntu 22.04 LTS or compatible
