# otelnet File Transfer Enhancement TODO

**Last Updated:** 2025-10-28
**Current Branch:** getkermit
**Status:** Embedded Kermit implementation completed

---

## ✅ Completed Work

### Embedded Kermit Client Integration (Phase 1.1 - COMPLETED)
**Branch:** getkermit
**Commits:**
- `6fc2201` - Add embedded Kermit client for file reception (rkermit command)
- `1a58661` - Fix critical bugs in skermit (client send) readf callback

**Implemented Features:**
- ✅ Embedded ekermit submodule integration (NO_LP, NO_SSW mode)
- ✅ `rkermit` command - Client receives files from server (via `/getkermit` server command)
- ✅ `skermit <file>` command - Client sends files to server (via `/putkermit` server command)
- ✅ Binary mode negotiation for file transfers
- ✅ Telnet state save/restore (BINARY, ECHO, SGA, LINEMODE)
- ✅ 8-bit transparent data transmission
- ✅ CRC-16 checksum (Type 3 block check, 99.998% error detection)
- ✅ Stop-and-wait protocol (94-byte packets)
- ✅ Progress tracking and error handling
- ✅ File integrity verification (MD5 hash match)

**Critical Bugs Fixed:**
1. **zinptr reset bug** - Segfault after 81,920 bytes (buffer overflow)
2. **EOF return value** - Infinite loop with 6x data duplication
3. **readf byte return** - 100% data corruption (MD5 mismatch)

**Test Results:**
- ✅ 2,097,152 bytes transferred successfully
- ✅ MD5: `9a005b974d4bd00c8fda1fcb6a7773c6` (100% match)
- ✅ No crashes, no infinite loops, no data corruption
- ✅ Identical behavior to Python reference implementation

**Files Modified:**
- `src/kermit_client.c` - Complete Kermit client implementation
- `include/kermit_client.h` - Client interface definitions
- `src/transfer.c` - Transfer execution wrappers
- `src/telnet.c` - Binary mode state management
- `include/telnet.h` - State save/restore structures
- `Makefile` - ekermit submodule integration
- `.gitmodules` - Added ekermit submodule

**Reference Implementation:**
- ekermit/kermit.c - Protocol engine
- ekermit/unixio.c - File I/O reference (readfile, writefile, closefile)
- ekermit/kermit.h - P_PKTLEN, P_WSLOTS constants

---

## 🚧 In Progress / Planned Work

### Phase 2: Performance Optimization (FUTURE)

#### 2.1 Long Packet Support (F_LP)
**Status:** Disabled (compile-time flag)
**Priority:** High
**Potential Improvement:** 43.6x throughput increase

**Current State:**
- NO_LP flag active (disabled in Makefile)
- Using 94-byte standard packets
- 39,322 packets needed for 3MB file

**Target State:**
- Enable F_LP in ekermit compilation
- Use 4096-byte long packets
- Only 768 packets needed for 3MB file
- 51.2x reduction in packet count

**Implementation:**
- Remove NO_LP from Makefile CFLAGS
- Verify server-side ekermit supports long packets
- Test negotiation: k_send.s_maxlen = 4096
- Benchmark actual throughput improvement

#### 2.2 Sliding Windows Support (F_SSW)
**Status:** Disabled (compile-time flag)
**Priority:** Medium
**Potential Improvement:** 31x throughput increase (combined with F_LP: 1,350x)

**Current State:**
- NO_SSW flag active (disabled in Makefile)
- Stop-and-wait: 1 packet in flight
- 39,322 round trips for 3MB file

**Target State:**
- Enable F_SSW in ekermit compilation
- Use 31-slot sliding window
- Only ~26 round trips needed for 3MB file
- 1,512x reduction in round trips

**Implementation:**
- Remove NO_SSW from Makefile CFLAGS
- Set k_send.window = 31
- Implement packet buffering and ACK tracking
- Handle out-of-order packets and retransmission

**Combined Performance (F_LP + F_SSW):**
- Current: 94 bytes × 1 packet/RTT = ~3 KB/s @ 100ms RTT
- With F_LP + F_SSW: 4096 bytes × 31 packets/RTT = ~1.27 MB/s @ 100ms RTT
- **1,350x improvement in high-latency networks**

---

### Phase 3: ZMODEM/XMODEM/YMODEM Integration

#### 3.1 Automatic Protocol Detection
**Status:** Partially implemented (manual commands work)
**Priority:** Medium

**Current State:**
- Manual commands: `sz`, `rz`, `sx`, `sy`, `rx`, `ry` work
- No automatic detection of remote ZMODEM initiation
- User must enter console mode manually

**Target State:**
- Detect remote ZMODEM patterns: `**\030B00` (ZRQINIT), `**\030B0` (ZRINIT)
- Auto-trigger local rz/sz without user intervention
- Configurable enable/disable: `AUTO_ZMODEM=1` in config

**Implementation Tasks:**
- [ ] Add ZMODEM detector to `otelnet_process_telnet()`
- [ ] Pattern matching: `{0x2A, 0x2A, 0x18, 0x42, 0x30}`
- [ ] Auto-trigger transfer on match
- [ ] User notification: "ZMODEM download detected..."

---

### Phase 4: Error Handling Improvements

#### 4.1 Transfer Timeout Management
**Status:** Basic timeout exists (5 minutes default)
**Priority:** High

**Current Limitations:**
- Global transfer timeout only
- No packet-level timeout
- No progressive retry logic

**Enhancements:**
- [ ] Add packet timeout (15 seconds default)
- [ ] Implement NAK retry limit (10 attempts)
- [ ] Add progress timeout (30 seconds no data)
- [ ] Graceful degradation on timeout

#### 4.2 Partial Transfer Recovery
**Status:** Not implemented
**Priority:** Low

**Target:**
- Display partial transfer stats on error
- Option to keep/delete incomplete files
- Resume capability (if protocol supports - ZMODEM only)

---

### Phase 5: Documentation and Testing

#### 5.1 User Documentation
**Priority:** Medium

**Tasks:**
- [ ] Update README.md with Kermit usage examples
- [ ] Document skermit/rkermit commands
- [ ] Add troubleshooting section
- [ ] Create performance comparison table

#### 5.2 Integration Testing
**Priority:** High

**Test Cases:**
- [x] Binary file integrity (2MB random data)
- [x] MD5 hash verification
- [x] Telnet state restoration
- [ ] Various file sizes (1KB - 100MB)
- [ ] Network latency simulation
- [ ] Error injection testing
- [ ] Concurrent transfers

#### 5.3 Performance Benchmarking
**Priority:** Low

**Metrics to Measure:**
- Current throughput (NO_LP, NO_SSW)
- With F_LP enabled
- With F_LP + F_SSW enabled
- Comparison with native sz/rz over serial

---

## 📊 Current Implementation Status

### Working Features
- ✅ Embedded Kermit send (skermit)
- ✅ Embedded Kermit receive (rkermit)
- ✅ Binary mode negotiation
- ✅ 8-bit transparent transmission
- ✅ CRC-16 error detection
- ✅ File integrity verification
- ✅ Progress tracking (10% increments)
- ✅ Error messages and logging
- ✅ Terminal state save/restore
- ✅ Telnet protocol state management

### Known Limitations
- ⚠️ No long packet support (94 bytes only)
- ⚠️ No sliding windows (stop-and-wait only)
- ⚠️ No ZMODEM auto-detection
- ⚠️ No transfer resume capability
- ⚠️ No bandwidth throttling

### Performance Characteristics
- **Packet Size:** 94 bytes (standard Kermit)
- **Window Size:** 1 packet (stop-and-wait)
- **Checksum:** CRC-16 (Type 3, 99.998% detection)
- **Throughput:** ~3-5 KB/s @ 100ms RTT
- **Latency Sensitivity:** High (1 RTT per packet)

**With F_LP + F_SSW (future):**
- **Packet Size:** 4096 bytes
- **Window Size:** 31 packets
- **Throughput:** ~1.27 MB/s @ 100ms RTT
- **Improvement:** 1,350x in high-latency networks

---

## 🔧 Configuration

### Current Settings (otelnet.conf)
```ini
KERMIT=/usr/bin/kermit
SEND_ZMODEM=/path/to/lsz
RECEIVE_ZMODEM=/path/to/lrz
LOG=1
LOG_FILE=otelnet.log
AUTO_ZMODEM=1  # Not yet implemented
TRANSFER_TIMEOUT=300
```

### Recommended Settings for Production
```ini
# Enable all transfer methods
KERMIT=/usr/bin/kermit
SEND_ZMODEM=/usr/bin/lsz
RECEIVE_ZMODEM=/usr/bin/lrz

# Logging
LOG=1
LOG_FILE=otelnet.log

# Future: Auto-detection
AUTO_ZMODEM=1
TRANSFER_TIMEOUT=300
```

---

## 📝 Technical Notes

### Kermit Protocol Specifications
- **Standard:** Kermit File Transfer Protocol
- **Packet Format:** `SOH LEN SEQ TYPE DATA CHECK EOM`
- **Checksum Types:** Type 1 (6-bit), Type 3 (CRC-16)
- **Block Check:** 99.998% error detection with CRC-16
- **Flow Control:** Stop-and-wait (NO_SSW) or sliding window (F_SSW)

### Critical Implementation Details
1. **readf() contract:**
   - Fill k->zinbuf with file data
   - Set k->zincnt to bytes read
   - Reset k->zinptr to k->zinbuf
   - Return first byte (not byte count!)
   - Return -1 for EOF (not 0!)

2. **writef() contract:**
   - Write len bytes from buf to file
   - Return X_OK (0) on success
   - Return X_ERROR on failure

3. **closef() contract:**
   - Close file and clean up
   - status='B' or 'Z' = success (keep file)
   - status='D' = incomplete (delete if !ikeep)

### Binary Mode Negotiation Sequence
```
Client → Server: IAC WILL BINARY
Server → Client: IAC DO BINARY
Server → Client: IAC WILL BINARY
Client → Server: IAC DO BINARY
[Transfer in BINARY mode]
Client → Server: IAC WONT BINARY
Server → Client: IAC DONT BINARY
Server → Client: IAC WONT BINARY
Client → Server: IAC DONT BINARY
[Back to text mode]
```

---

## 🎯 Success Metrics

### Must Have (COMPLETED ✅)
- [x] Embedded Kermit client working
- [x] skermit/rkermit commands implemented
- [x] 8-bit transparent mode verified
- [x] Binary file transfers work (2MB tested)
- [x] MD5 hash verification passes
- [x] No data corruption
- [x] No crashes or hangs
- [x] Terminal state properly restored

### Should Have (FUTURE)
- [ ] Long packet support (F_LP)
- [ ] Sliding windows (F_SSW)
- [ ] ZMODEM auto-detection
- [ ] Transfer resume capability
- [ ] Comprehensive error handling
- [ ] Performance benchmarks

### Nice to Have (FUTURE)
- [ ] Transfer logging
- [ ] Bandwidth throttling
- [ ] Multiple file queue
- [ ] GUI file picker
- [ ] SSH support

---

## 🚀 Next Steps

### Immediate Priority
1. **Test with production servers**
   - Verify compatibility with various telnet servers
   - Test with different network conditions
   - Measure real-world performance

2. **Enable F_LP for performance**
   - Remove NO_LP flag
   - Test long packet negotiation
   - Benchmark throughput improvement

3. **Documentation**
   - Update README.md with complete examples
   - Add troubleshooting guide
   - Document configuration options

### Long-term Goals
1. Enable F_SSW (sliding windows)
2. Implement ZMODEM auto-detection
3. Add transfer resume capability
4. Performance optimization and testing

---

## 📚 References

### Implemented Based On
- ekermit source code (embedded Kermit-95 subset)
- RFC 854: Telnet Protocol Specification
- RFC 856: Telnet Binary Transmission
- RFC 858: Telnet Suppress Go Ahead Option

### Test Scripts
- `test_putkermit_python.py` - Python reference implementation (working)
- `test_getkermit_python.py` - Python reference for receive (working)
- `test_otelnet_skermit.exp` - Expect-based automated test
- `test_otelnet_kermit.exp` - Interactive test script

### Development Tools
- MD5 checksum verification
- Binary file comparison (cmp)
- dmesg for crash analysis
- gdb for debugging (debug build)

---

**Version:** 2.0
**Author:** Claude Code
**License:** Same as otelnet main project
