# otelnet - Standalone Telnet Client

A feature-rich standalone telnet client with integrated file transfer support.

## Features

- **RFC 854 Compliant Telnet Protocol**
  - Full IAC sequence handling
  - Automatic option negotiation (BINARY, ECHO, SGA)
  - Multibyte character support (UTF-8)

- **Console Mode**
  - Press `Ctrl+]` to enter console mode
  - Execute commands without disconnecting
  - Empty line + Enter to return to client mode

- **File Transfer Support**
  - XMODEM protocol (single file)
  - YMODEM protocol (batch transfer)
  - ZMODEM protocol (automatic, resumable)
  - Kermit protocol support

- **Session Logging**
  - Optional data logging with timestamps
  - Hex + ASCII dump format
  - Send/receive data tracking

- **File Management**
  - Built-in `ls`, `pwd`, `cd` commands
  - Browse files before transfer
  - Current directory display

## Build

```bash
# Standard build
make

# Clean build
make clean && make

# Debug build (with symbols, no optimization)
make debug

# Install system-wide (requires root)
sudo make install
```

## Usage

```bash
# Connect to telnet server
./otelnet <host> <port>

# Example
./otelnet localhost 23
./otelnet 192.168.1.100 8881

# With custom config file
./otelnet <host> <port> -c myconfig.conf

# Show help
./otelnet --help

# Show version
./otelnet --version
```

## Configuration

Copy `otelnet.conf` to your working directory or `/etc/otelnet.conf`:

```bash
# Copy to current directory
cp otelnet.conf .

# Or copy to system config (requires root)
sudo cp otelnet.conf /etc/otelnet.conf
```

Edit configuration file:

```conf
# External program paths
KERMIT=kermit
SEND_ZMODEM=sz
RECEIVE_ZMODEM=rz

# Logging
LOG=1                    # 1=enabled, 0=disabled
LOG_FILE=otelnet.log     # Log file path
```

## Console Mode

Press `Ctrl+]` during a telnet session to enter console mode.

### Available Commands

**File Transfer:**
- `sz [options] <files...>` - Send files via ZMODEM (default)
- `sy <files...>` - Send files via YMODEM
- `sx <file>` - Send single file via XMODEM
- `rz [options]` - Receive files via ZMODEM (default)
- `ry` - Receive files via YMODEM
- `rx` - Receive single file via XMODEM
- `kermit [args]` - Run kermit file transfer

**Protocol Options:**
- `--xmodem`, `-x` - Use XMODEM protocol
- `--ymodem`, `-y` - Use YMODEM protocol
- `--zmodem`, `-z` - Use ZMODEM protocol

**File Management:**
- `ls [dir]` - List files in directory
- `pwd` - Print working directory
- `cd <dir>` - Change directory

**Session Control:**
- `stats` - Show connection statistics
- `help`, `?` - Show help message
- `quit`, `exit` - Disconnect and exit
- `[empty line]` - Return to client mode

## Examples

### Basic Connection
```bash
# Connect to BBS
./otelnet bbs.example.com 23

# Connect to local telnet server
./otelnet localhost 8881
```

### File Transfer Examples
```bash
# In console mode (press Ctrl+] first):

# Send single file via ZMODEM
sz document.pdf

# Send multiple files via YMODEM
sy *.txt

# Send with explicit protocol option
sz --ymodem file1.txt file2.txt

# Send single file via XMODEM
sx firmware.bin

# Receive files via ZMODEM
rz

# Receive via YMODEM
ry

# Receive via XMODEM
rx
```

### File Management
```bash
# In console mode:

# List current directory
ls

# List specific directory
ls /tmp

# Check current directory
pwd

# Change directory
cd ~/Downloads

# Send files from new location
sz *.pdf
```

## Session Logging

When `LOG=1` in configuration file, all send/receive data is logged:

```
[2025-10-15 12:34:56] === Session started ===
[2025-10-15 12:34:56][send] 68 65 6c 6c 6f 0d 0a                | hello..
[2025-10-15 12:34:57][receive] 57 65 6c 63 6f 6d 65 21 0d 0a    | Welcome!..
[2025-10-15 12:35:00] === Session ended ===
```

## Requirements

- Linux system (Ubuntu 22.04 LTS or later recommended)
- GCC compiler (C11/GNU11 support)
- External programs (optional):
  - `sz`/`rz` for XMODEM/YMODEM/ZMODEM transfers
  - `kermit` for Kermit protocol

Install external programs:
```bash
# Ubuntu/Debian
sudo apt install lrzsz ckermit

# Fedora/RHEL
sudo dnf install lrzsz ckermit

# Arch Linux
sudo pacman -S lrzsz ckermit
```

## Keyboard Shortcuts

- `Ctrl+]` - Enter console mode
- `Ctrl+D` - Exit program (in console mode)
- `Ctrl+C` - Interrupt/disconnect

## Troubleshooting

**Input not visible:**
- otelnet automatically enables local echo when server doesn't echo
- This is normal for character-mode telnet servers

**File transfer programs not found:**
- Install `lrzsz` package for sz/rz commands
- Install `ckermit` package for kermit command
- Or configure full paths in `otelnet.conf`

**Permission denied on log file:**
- Check write permissions on log file directory
- Change `LOG_FILE` path in configuration

## Technical Details

- **Protocol**: RFC 854 Telnet Protocol
- **Character Encoding**: UTF-8 with multibyte support
- **Terminal Mode**: Raw mode with local echo management
- **I/O Multiplexing**: select() for responsive handling
- **Logging**: Hex+ASCII dump format with timestamps

## Acknowledgments

The telnet protocol implementation in this project references code from [PuTTY](https://www.chiark.greenend.org.uk/~sgtatham/putty/), the popular open-source SSH and telnet client. PuTTY is licensed under the MIT License.

We are grateful to the PuTTY development team for their well-documented and robust telnet protocol implementation, which served as a valuable reference for this project.

## Project History

otelnet originated as a component of the ModemBridge project but has since evolved into an independent standalone telnet client project. It is now maintained separately with its own development roadmap and features.

## License

This project is open source. Please refer to the LICENSE file for details.

## See Also

- RFC 854: Telnet Protocol Specification
- XMODEM/YMODEM/ZMODEM: File transfer protocols
- PuTTY: https://www.chiark.greenend.org.uk/~sgtatham/putty/
