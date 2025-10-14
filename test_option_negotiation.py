#!/usr/bin/env python3
"""
Test script to verify option negotiation bug fix

This script simulates a telnet server that sends unsupported option requests
and verifies that otelnet properly rejects them with DONT/WONT responses.
"""

import socket
import time
import sys
import threading

# Telnet protocol constants
IAC = 255
DONT = 254
DO = 253
WONT = 252
WILL = 251
SB = 250
SE = 240

# Telnet options
TELOPT_BINARY = 0
TELOPT_ECHO = 1
TELOPT_SGA = 3
TELOPT_TTYPE = 24
TELOPT_LINEMODE = 34
TELOPT_CHARSET = 42  # Unsupported option for testing

def bytes_to_hex(data):
    """Convert bytes to hex string for logging"""
    return ' '.join(f'{b:02x}' for b in data)

def telnet_command_name(cmd):
    """Convert telnet command byte to name"""
    names = {
        IAC: "IAC",
        DONT: "DONT",
        DO: "DO",
        WONT: "WONT",
        WILL: "WILL",
        SB: "SB",
        SE: "SE"
    }
    return names.get(cmd, f"UNKNOWN({cmd})")

def option_name(opt):
    """Convert option byte to name"""
    names = {
        TELOPT_BINARY: "BINARY",
        TELOPT_ECHO: "ECHO",
        TELOPT_SGA: "SGA",
        TELOPT_TTYPE: "TERMINAL-TYPE",
        TELOPT_LINEMODE: "LINEMODE",
        TELOPT_CHARSET: "CHARSET"
    }
    return names.get(opt, f"UNKNOWN({opt})")

def run_test_server(port=8881):
    """Run a simple telnet server for testing"""
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        server_socket.bind(('127.0.0.1', port))
        server_socket.listen(1)
        server_socket.settimeout(10)  # 10 second timeout

        print(f"[TEST SERVER] Listening on 127.0.0.1:{port}")
        print("[TEST SERVER] Waiting for connection...")

        conn, addr = server_socket.accept()
        conn.settimeout(2)  # 2 second timeout for recv

        print(f"[TEST SERVER] Connection from {addr}")

        # Receive initial negotiations from client
        print("\n[TEST SERVER] Receiving initial negotiations from client...")
        received_data = b''
        try:
            while True:
                chunk = conn.recv(1024)
                if not chunk:
                    break
                received_data += chunk
                if len(received_data) > 100:  # Enough initial data
                    break
        except socket.timeout:
            pass

        print(f"[TEST SERVER] Received {len(received_data)} bytes: {bytes_to_hex(received_data)}")

        # Parse initial negotiations
        i = 0
        while i < len(received_data):
            if received_data[i] == IAC and i + 2 < len(received_data):
                cmd = received_data[i + 1]
                opt = received_data[i + 2]
                print(f"[TEST SERVER]   {telnet_command_name(cmd)} {option_name(opt)}")
                i += 3
            else:
                i += 1

        # Test 1: Send unsupported option requests
        print("\n[TEST SERVER] Test 1: Sending unsupported option requests")
        print("[TEST SERVER]   Sending: IAC WILL CHARSET (unsupported)")
        conn.send(bytes([IAC, WILL, TELOPT_CHARSET]))

        time.sleep(0.1)

        print("[TEST SERVER]   Sending: IAC DO CHARSET (unsupported)")
        conn.send(bytes([IAC, DO, TELOPT_CHARSET]))

        # Receive responses
        time.sleep(0.2)
        try:
            response = conn.recv(1024)
            print(f"\n[TEST SERVER] Received response: {bytes_to_hex(response)}")

            # Parse response
            test_passed = True
            expected_responses = []

            i = 0
            while i < len(response):
                if response[i] == IAC and i + 2 < len(response):
                    cmd = response[i + 1]
                    opt = response[i + 2]
                    response_str = f"{telnet_command_name(cmd)} {option_name(opt)}"
                    print(f"[TEST SERVER]   {response_str}")

                    # Check expected responses
                    if cmd == DONT and opt == TELOPT_CHARSET:
                        print("[TEST SERVER]   ✓ Correctly rejected WILL CHARSET with DONT")
                        expected_responses.append("DONT CHARSET")
                    elif cmd == WONT and opt == TELOPT_CHARSET:
                        print("[TEST SERVER]   ✓ Correctly rejected DO CHARSET with WONT")
                        expected_responses.append("WONT CHARSET")

                    i += 3
                else:
                    i += 1

            # Verify results
            print("\n[TEST SERVER] ======================")
            print("[TEST SERVER] TEST RESULTS:")
            print("[TEST SERVER] ======================")

            if "DONT CHARSET" in expected_responses:
                print("[TEST SERVER] ✓ PASS: Client rejected WILL CHARSET with DONT")
            else:
                print("[TEST SERVER] ✗ FAIL: Client did not send DONT for WILL CHARSET")
                test_passed = False

            if "WONT CHARSET" in expected_responses:
                print("[TEST SERVER] ✓ PASS: Client rejected DO CHARSET with WONT")
            else:
                print("[TEST SERVER] ✗ FAIL: Client did not send WONT for DO CHARSET")
                test_passed = False

            if test_passed:
                print("\n[TEST SERVER] ✓✓✓ ALL TESTS PASSED ✓✓✓")
                print("[TEST SERVER] Bug fix verified: otelnet correctly rejects unsupported options")
            else:
                print("\n[TEST SERVER] ✗✗✗ TESTS FAILED ✗✗✗")
                print("[TEST SERVER] Bug may still be present")

            print("[TEST SERVER] ======================\n")

        except socket.timeout:
            print("[TEST SERVER] ✗ No response received (timeout)")
            test_passed = False

        # Keep connection open briefly
        time.sleep(0.5)

        conn.close()
        print("[TEST SERVER] Connection closed")

        return test_passed

    except socket.timeout:
        print("[TEST SERVER] Timeout waiting for connection")
        return False
    except Exception as e:
        print(f"[TEST SERVER] Error: {e}")
        import traceback
        traceback.print_exc()
        return False
    finally:
        server_socket.close()
        print("[TEST SERVER] Server socket closed")

if __name__ == "__main__":
    port = 8881
    if len(sys.argv) > 1:
        port = int(sys.argv[1])

    print("=" * 70)
    print("OTELNET OPTION NEGOTIATION BUG FIX TEST")
    print("=" * 70)
    print()
    print("This test verifies that otelnet correctly rejects unsupported options")
    print("with DONT/WONT responses (RFC 855 compliance).")
    print()
    print(f"Test port: {port}")
    print()
    print("To run the test:")
    print(f"  1. Run this script: python3 {sys.argv[0]} [{port}]")
    print(f"  2. In another terminal: ./build/otelnet localhost {port}")
    print()
    print("=" * 70)
    print()

    success = run_test_server(port)

    sys.exit(0 if success else 1)
