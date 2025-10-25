# otelnet File Transfer IAC Byte Handling Fix Plan

**목표**: Telnet IAC (0xFF) byte escaping/unescaping 문제 해결

**작성일**: 2025-10-26
**관련 이슈**: Exit code 128, 파일 전송 실패, 데이터 손상

---

## 1. 문제 진단 (Log Analysis)

### 1.1 현상

**Client (otelnet)**:
```
[Starting transfer: /mnt/USERS/onion/DATA_ORIGN/Workspace/lrzsz-lite/bin/lsz /mnt/USERS/onion/DATA_ORIGN/Workspace/otelnet/sample.jpg]
[Transfer completed with warnings (exit code 128)]
```

**Server (telnet_server)**:
```
lrz: sample.jpg removed.
[INFO] Transfer process exited with code 128
```

**Exit code 128**: lrz가 파일을 수신했지만 **데이터 손상 (CRC/checksum error)** 으로 인해 파일을 삭제함

### 1.2 로그에서 발견된 근본 원인

**Server log 분석** (server_debug.log):

```
Line 55:  Received 1024 bytes (raw) → After IAC processing: 1016 bytes (8 bytes lost)
Line 70:  Received 1024 bytes (raw) → After IAC processing: 1020 bytes (4 bytes lost)
Line 71:  Received 1024 bytes (raw) → After IAC processing: 1020 bytes (4 bytes lost)
Line 127: Received 1024 bytes (raw) → After IAC processing: 1015 bytes (9 bytes lost)
Line 283: Received 1024 bytes (raw) → After IAC processing: 1007 bytes (17 bytes lost!)
```

**문제**:
- ZMODEM 바이너리 데이터에 포함된 **0xFF 바이트가 텔넷 IAC (Interpret As Command) 바이트로 잘못 처리**됨
- 서버가 IAC processing에서 0xFF를 명령으로 해석하고 제거
- 손상된 데이터가 lrz로 전달됨
- lrz가 CRC 검증 실패 → 파일 삭제 → exit code 128

### 1.3 BINARY Mode는 왜 충분하지 않은가?

**Server log** (server_debug.log:37-38):
```
[DEBUG] BINARY negotiation: Client agreed to receive binary (DO BINARY -> WILL BINARY).
[DEBUG] Full BINARY mode activated (bidirectional).
```

BINARY mode가 활성화되었지만 여전히 데이터가 손실됩니다. 이유는:

**RFC 854 Telnet Protocol 규칙**:
> BINARY mode에서도 IAC (0xFF) 바이트는 특수 문자입니다.
> 바이너리 데이터에 0xFF가 포함된 경우 **반드시 0xFF 0xFF로 escape** 되어야 합니다.

**즉**:
- BINARY mode = 8-bit clean transmission (모든 바이트 0x00-0xFF 사용 가능)
- BUT: 0xFF는 여전히 "IAC" 명령 시작 바이트
- **해결책**: 0xFF → 0xFF 0xFF (escaping), 0xFF 0xFF → 0xFF (unescaping)

### 1.4 현재 구조의 문제점

#### Client Side (otelnet):

```
┌──────────────┐
│   otelnet    │  ← stdin에서 사용자 입력 받음
│   (parent)   │  ← Telnet protocol handler (IAC escaping 가능)
└──────┬───────┘
       │
       │ fork()
       │
       ▼
┌──────────────────────────────────────┐
│      lsz (child)                     │
│  - socket FD를 stdin/stdout으로 dup2 │
│  - 텔넷 프로토콜 인식 못함           │  ❌ 문제!
│  - 0xFF를 escape하지 않고 직접 전송  │
└──────────────────────────────────────┘
       │
       │ (raw socket write)
       ▼
   [Network]
```

**문제**: lsz가 socket에 직접 쓰면서 0xFF를 escape하지 않음

#### Server Side (telnet_server):

```
   [Network]
       │
       ▼
┌──────────────────────────────────────┐
│  telnet_server                       │
│  - IAC processing 수행               │
│  - 0xFF를 명령으로 해석              │  ❌ 문제!
│  - 손상된 데이터를 lrz로 relay       │
└──────┬───────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────┐
│      lrz (child)                     │
│  - 손상된 데이터 수신                │
│  - CRC 오류 감지 → 파일 삭제         │
└──────────────────────────────────────┘
```

**문제**: 서버가 IAC processing에서 0xFF를 제거하고, lrz는 손상된 데이터를 받음

---

## 2. 텔넷 프로토콜과 바이너리 데이터 충돌 분석

### 2.1 RFC 854 Telnet Protocol - IAC Escaping Rules

**IAC (Interpret As Command)**: 0xFF (255)

**규칙**:
1. 모든 텔넷 명령은 IAC로 시작: `IAC <command> [<option>]`
2. 데이터에 0xFF가 포함되면 **IAC IAC** (0xFF 0xFF)로 escape
3. 수신자는 0xFF 0xFF를 받으면 → 0xFF로 해석 (unescape)

**예시**:

| 원본 데이터          | Telnet Wire Format    | 설명                    |
|----------------------|-----------------------|-------------------------|
| `0x48 0x65 0x6C 0x6C` | `0x48 0x65 0x6C 0x6C` | 일반 데이터 (그대로)    |
| `0xFF 0x12 0x34`     | `0xFF 0xFF 0x12 0x34` | 0xFF를 0xFF 0xFF로 escape |
| `0xFF 0xFF`          | `0xFF 0xFF 0xFF 0xFF` | 두 개의 0xFF 모두 escape |

**BINARY mode에서도 동일 적용**:
- BINARY mode는 0x00-0xFF 모든 바이트 전송 허용
- BUT: 0xFF는 여전히 escape 필요 (protocol control byte)

### 2.2 ZMODEM Binary Data의 0xFF 발생 빈도

**JPEG 파일 분석** (sample.jpg):
- JPEG header: `0xFF 0xD8 0xFF 0xE0 ...` (0xFF 빈번)
- JPEG markers: 모두 0xFF로 시작 (`0xFF 0xDA`, `0xFF 0xC0`, etc.)
- JPEG 데이터: 압축 데이터에 0xFF 포함 가능성 높음

**예상 0xFF 발생률**:
- 일반 바이너리 데이터: ~0.4% (1/256)
- JPEG/PNG 등 이미지: ~2-5% (marker 포함)
- 압축 파일: ~0.4-1%

**로그에서 확인**:
```
1024 bytes → 1016 bytes (8 bytes lost)  ← 약 4개의 0xFF (0.4%)
1024 bytes → 1015 bytes (9 bytes lost)  ← 약 4-5개의 0xFF
1024 bytes → 1007 bytes (17 bytes lost) ← 약 8-9개의 0xFF (0.8%)
```

### 2.3 현재 구현의 IAC Handling 상태

#### otelnet (Client)

**파일**: `src/telnet.c`

**송신 경로** (`telnet_prepare_output()`):
```c
// 추정: 이 함수에서 IAC escaping 수행
// 0xFF → 0xFF 0xFF 변환
```

**문제**: lsz가 `telnet_prepare_output()`을 거치지 않고 **socket에 직접 write**

#### telnet_server (Server)

**파일**: (server code path)

**수신 경로**:
```c
// IAC processing 수행
// 0xFF 0xFF → 0xFF 변환 (unescape)
// 0xFF <cmd> → 명령 처리
```

**문제**: lsz가 보낸 0xFF가 escape되지 않아서, **서버가 0xFF를 명령으로 해석하고 버림**

---

## 3. 해결 방안 (3가지 접근)

### Option 1: Pipe-Based IAC Escaping (권장)

**개념**: lsz/lrz와 socket 사이에 **IAC escaping/unescaping layer** 삽입

#### Client Side (otelnet):

```
┌──────────────┐
│   otelnet    │
│   (parent)   │
└──────┬───────┘
       │
       │ fork()
       │
       ├─────────────────────────────────────────┐
       │                                         │
       ▼                                         ▼
┌─────────────────┐                    ┌──────────────────┐
│  lsz (child)    │                    │  IAC Escape      │
│  - writes to    │  ─── pipe ───>     │  Thread/Process  │
│    stdout       │                    │  - 0xFF→0xFF0xFF │
└─────────────────┘                    │  - write to      │
                                       │    socket        │
                                       └────────┬─────────┘
                                                │
                                                ▼
                                           [Network]
```

**장점**:
- ✅ lsz 수정 불필요 (기존 바이너리 그대로 사용)
- ✅ 완전한 텔넷 프로토콜 호환성
- ✅ 다른 프로토콜 (YMODEM, XMODEM)에도 적용 가능

**단점**:
- ⚠️ 추가 버퍼링 및 복사 (성능 영향 ~5-10%)
- ⚠️ 복잡한 pipe/thread 관리

#### Server Side (telnet_server):

```
   [Network]
       │
       ▼
┌──────────────────┐
│  IAC Unescape    │
│  - 0xFF0xFF→0xFF │
│  - relay to pipe │
└────────┬─────────┘
         │
         ▼
┌─────────────────┐
│  lrz (child)    │
│  - reads from   │
│    stdin (pipe) │
└─────────────────┘
```

### Option 2: Direct Socket IAC Handling

**개념**: lsz/lrz를 수정하여 직접 IAC escaping 수행

**장점**:
- ✅ 최고 성능 (추가 버퍼링 없음)
- ✅ 단순한 데이터 경로

**단점**:
- ❌ lrzsz-lite 소스 수정 필요
- ❌ 유지보수 부담 증가
- ❌ 다른 프로그램 (kermit)에는 적용 불가

**평가**: 권장하지 않음 (lrzsz-lite는 범용 도구로 유지)

### Option 3: Raw Socket Mode (Protocol Bypass)

**개념**: 파일 전송 중 텔넷 프로토콜 완전 우회

**방법**:
1. Client와 Server가 special escape sequence 협상
2. "Raw mode" 진입 → IAC processing 중단
3. 바이너리 데이터 직접 전송 (0xFF escape 없이)
4. 전송 완료 후 "Normal mode" 복귀

**장점**:
- ✅ 최고 성능 (IAC escaping overhead 제거)
- ✅ 단순한 구현 (파일 전송 중 IAC processing skip)

**단점**:
- ❌ 표준 텔넷 프로토콜 위반
- ❌ 기존 텔넷 서버와 호환성 문제
- ❌ 전송 중 telnet 명령 처리 불가 (Ctrl+C 등)

**평가**: 특수 목적 서버에만 적용 가능

---

## 4. 선택된 해결 방안: Option 1 (Pipe-Based IAC Escaping)

### 4.1 구현 개요

**핵심 아이디어**:
- `relay_data_pipes()` 함수 내부에 IAC escaping/unescaping 로직 추가
- Socket → Pipe (to child): IAC unescape (0xFF 0xFF → 0xFF)
- Pipe → Socket (from child): IAC escape (0xFF → 0xFF 0xFF)

### 4.2 Client Side 상세 설계 (otelnet)

#### A. 현재 구조 분석

**파일**: `src/transfer.c`

**현재 `relay_data_pipes()` 함수** (lines 552-807):

```c
int relay_data_pipes(int socket_fd, int child_stdin_fd, int child_stdout_fd,
                     TransferState *state, int timeout_sec) {
    // ...
    while (!state->cancel_transfer && time(NULL) - start_time < timeout_sec) {
        // select() on socket_fd and child_stdout_fd

        // Socket → Child (stdin)
        if (FD_ISSET(socket_fd, &readfds)) {
            ssize_t n = recv(socket_fd, buffer, sizeof(buffer), 0);
            // 현재: buffer를 그대로 child_stdin_fd로 write
            write(child_stdin_fd, buffer, n);  // ← IAC unescaping 필요!
        }

        // Child (stdout) → Socket
        if (FD_ISSET(child_stdout_fd, &readfds)) {
            ssize_t n = read(child_stdout_fd, buffer, sizeof(buffer));
            // 현재: buffer를 그대로 socket_fd로 send
            send(socket_fd, buffer, n, 0);  // ← IAC escaping 필요!
        }
    }
}
```

#### B. 수정된 구조

**새로운 helper 함수 추가**:

```c
/**
 * Escape IAC bytes (0xFF) in binary data for telnet transmission
 *
 * @param input      Input buffer (original data)
 * @param input_len  Length of input data
 * @param output     Output buffer (escaped data)
 * @param output_max Maximum size of output buffer
 * @return           Length of escaped data, or -1 if output buffer too small
 *
 * Example: {0x12, 0xFF, 0x34} → {0x12, 0xFF, 0xFF, 0x34}
 */
ssize_t telnet_escape_iac(const unsigned char *input, size_t input_len,
                          unsigned char *output, size_t output_max) {
    size_t out_idx = 0;

    for (size_t i = 0; i < input_len; i++) {
        if (input[i] == 0xFF) {
            // Check buffer space for two bytes
            if (out_idx + 2 > output_max) {
                return -1;  // Buffer overflow
            }
            output[out_idx++] = 0xFF;
            output[out_idx++] = 0xFF;  // IAC escape
        } else {
            if (out_idx + 1 > output_max) {
                return -1;
            }
            output[out_idx++] = input[i];
        }
    }

    return out_idx;
}

/**
 * Unescape IAC bytes (0xFF 0xFF → 0xFF) from telnet stream
 *
 * @param input      Input buffer (escaped telnet data)
 * @param input_len  Length of input data
 * @param output     Output buffer (unescaped binary data)
 * @param output_max Maximum size of output buffer
 * @param iac_state  Pointer to IAC state (0=normal, 1=saw 0xFF)
 * @return           Length of unescaped data
 *
 * Example: {0x12, 0xFF, 0xFF, 0x34} → {0x12, 0xFF, 0x34}
 *
 * Note: This function handles partial IAC sequences across buffer boundaries.
 *       The caller must maintain iac_state between calls.
 */
ssize_t telnet_unescape_iac(const unsigned char *input, size_t input_len,
                            unsigned char *output, size_t output_max,
                            int *iac_state) {
    size_t out_idx = 0;

    for (size_t i = 0; i < input_len; i++) {
        unsigned char byte = input[i];

        if (*iac_state == 1) {
            // Previous byte was 0xFF
            if (byte == 0xFF) {
                // 0xFF 0xFF → single 0xFF (escape sequence)
                if (out_idx + 1 > output_max) {
                    return -1;
                }
                output[out_idx++] = 0xFF;
                *iac_state = 0;
            } else {
                // 0xFF <other> → telnet command, skip both bytes
                // (In BINARY mode, we should not receive commands, but handle it)
                MB_LOG_WARNING("Unexpected telnet command during binary transfer: IAC 0x%02X", byte);
                *iac_state = 0;
                // Do not output anything
            }
        } else {
            // Normal state
            if (byte == 0xFF) {
                *iac_state = 1;  // Wait for next byte
            } else {
                if (out_idx + 1 > output_max) {
                    return -1;
                }
                output[out_idx++] = byte;
            }
        }
    }

    return out_idx;
}
```

**수정된 `relay_data_pipes()` 함수**:

```c
int relay_data_pipes(int socket_fd, int child_stdin_fd, int child_stdout_fd,
                     TransferState *state, int timeout_sec) {
    unsigned char recv_buffer[BUFFER_SIZE];
    unsigned char send_buffer[BUFFER_SIZE];
    unsigned char escaped_buffer[BUFFER_SIZE * 2];  // Worst case: all 0xFF

    int iac_state = 0;  // State for IAC unescaping (0=normal, 1=saw 0xFF)

    // ... (existing code)

    while (!state->cancel_transfer && time(NULL) - start_time < timeout_sec) {
        // ... (select setup)

        // Socket → Child (stdin): Unescape IAC
        if (FD_ISSET(socket_fd, &readfds)) {
            ssize_t n = recv(socket_fd, recv_buffer, sizeof(recv_buffer), 0);

            if (n > 0) {
                // Unescape IAC bytes before sending to child
                ssize_t unescaped_len = telnet_unescape_iac(recv_buffer, n,
                                                             send_buffer, sizeof(send_buffer),
                                                             &iac_state);

                if (unescaped_len < 0) {
                    MB_LOG_ERROR("IAC unescape buffer overflow");
                    return -1;
                }

                if (unescaped_len > 0) {
                    ssize_t written = write(child_stdin_fd, send_buffer, unescaped_len);
                    if (written != unescaped_len) {
                        MB_LOG_ERROR("Incomplete write to child stdin");
                        return -1;
                    }

                    MB_LOG_DEBUG("Socket→Child: %zd bytes (raw) → %zd bytes (unescaped)",
                                 n, unescaped_len);
                }
            }
        }

        // Child (stdout) → Socket: Escape IAC
        if (FD_ISSET(child_stdout_fd, &readfds)) {
            ssize_t n = read(child_stdout_fd, recv_buffer, sizeof(recv_buffer));

            if (n > 0) {
                // Escape IAC bytes before sending to socket
                ssize_t escaped_len = telnet_escape_iac(recv_buffer, n,
                                                         escaped_buffer, sizeof(escaped_buffer));

                if (escaped_len < 0) {
                    MB_LOG_ERROR("IAC escape buffer overflow");
                    return -1;
                }

                ssize_t sent = send(socket_fd, escaped_buffer, escaped_len, 0);
                if (sent != escaped_len) {
                    MB_LOG_ERROR("Incomplete send to socket");
                    return -1;
                }

                MB_LOG_DEBUG("Child→Socket: %zd bytes (raw) → %zd bytes (escaped)",
                             n, escaped_len);
            }
        }
    }

    // ... (existing cleanup code)
}
```

#### C. 헤더 파일 수정

**파일**: `include/transfer.h`

```c
/* IAC Escaping Functions */

/**
 * Escape IAC bytes (0xFF → 0xFF 0xFF) for telnet transmission
 */
ssize_t telnet_escape_iac(const unsigned char *input, size_t input_len,
                          unsigned char *output, size_t output_max);

/**
 * Unescape IAC bytes (0xFF 0xFF → 0xFF) from telnet stream
 */
ssize_t telnet_unescape_iac(const unsigned char *input, size_t input_len,
                            unsigned char *output, size_t output_max,
                            int *iac_state);
```

### 4.3 Server Side 상세 설계 (telnet_server)

**Server 코드 위치**: `/mnt/USERS/onion/DATA_ORIGN/Workspace/02_pge_project/telnet_server_simple_test`

**현재 상태 확인 필요**:
1. 서버가 BINARY mode에서 IAC unescaping을 올바르게 수행하는지?
2. `relay_data_pipes()` 유사 함수에서 IAC 처리가 있는지?

**예상 수정 사항**:
- Client와 동일한 `telnet_escape_iac()` / `telnet_unescape_iac()` 함수 추가
- File transfer relay 코드에서:
  - Socket → lrz: IAC unescape
  - lrz → Socket: IAC escape

**Note**: Server 코드 분석 후 구체적인 수정 계획 추가 필요

---

## 5. 구현 계획

### Phase 1: Helper 함수 구현 및 단위 테스트

**Task 1.1**: IAC escaping 함수 구현
- [ ] `src/transfer.c`에 `telnet_escape_iac()` 함수 추가
- [ ] `src/transfer.c`에 `telnet_unescape_iac()` 함수 추가
- [ ] `include/transfer.h`에 함수 선언 추가

**Task 1.2**: 단위 테스트 작성
- [ ] 테스트 프로그램 작성: `tests/test_iac_escaping.c`
- [ ] 테스트 케이스:
  - 일반 데이터 (0xFF 없음): `{0x12, 0x34, 0x56}` → 변화 없음
  - 단일 0xFF: `{0xFF}` → `{0xFF, 0xFF}`
  - 연속 0xFF: `{0xFF, 0xFF}` → `{0xFF, 0xFF, 0xFF, 0xFF}`
  - Mixed: `{0x12, 0xFF, 0x34, 0xFF, 0x56}` → `{0x12, 0xFF, 0xFF, 0x34, 0xFF, 0xFF, 0x56}`
  - Buffer boundary: 부분 IAC sequence 처리
  - Worst case: 1024 bytes all 0xFF → 2048 bytes
- [ ] Makefile에 테스트 타겟 추가: `make test-iac`

**Task 1.3**: 성능 측정
- [ ] Benchmark: 1MB random data escaping/unescaping 시간 측정
- [ ] 다양한 0xFF 비율에서 성능 테스트 (0%, 0.4%, 2%, 10%, 100%)

### Phase 2: Client Side 통합 (otelnet)

**Task 2.1**: `relay_data_pipes()` 수정
- [ ] `src/transfer.c` 백업: `cp src/transfer.c src/transfer.c.phase1`
- [ ] Socket → Child 경로에 `telnet_unescape_iac()` 추가
- [ ] Child → Socket 경로에 `telnet_escape_iac()` 추가
- [ ] 디버그 로그 추가 (escaped/unescaped byte counts)

**Task 2.2**: Buffer 크기 검증
- [ ] Worst-case buffer 크기 계산 (all 0xFF scenario)
- [ ] 현재 `BUFFER_SIZE` 확인 및 필요시 조정
- [ ] Buffer overflow 방지 로직 확인

**Task 2.3**: 빌드 및 기본 테스트
- [ ] `make clean && make DEBUG=1`
- [ ] 컴파일 오류 해결
- [ ] Warning 제거
- [ ] 기본 실행 테스트: `./build/otelnet localhost 9091`

### Phase 3: Server Side 통합 (telnet_server)

**Task 3.1**: Server 코드 분석
- [ ] `/mnt/USERS/onion/DATA_ORIGN/Workspace/02_pge_project/telnet_server_simple_test` 검토
- [ ] 현재 IAC 처리 로직 파악
- [ ] File transfer relay 함수 식별

**Task 3.2**: Server 수정
- [ ] Server에 `telnet_escape_iac()` / `telnet_unescape_iac()` 함수 추가
- [ ] File transfer relay에 IAC processing 통합
- [ ] 디버그 로그 추가

**Task 3.3**: Server 빌드
- [ ] Server 빌드 및 기본 테스트

### Phase 4: 통합 테스트

**Test Environment Setup**:
```bash
# Server 실행 (별도 터미널)
cd /mnt/USERS/onion/DATA_ORIGN/Workspace/02_pge_project/telnet_server_simple_test
./build/telnet_server 9091

# Client 실행
cd /mnt/USERS/onion/DATA_ORIGN/Workspace/otelnet
./build/otelnet localhost 9091
```

**Test 4.1**: 작은 텍스트 파일 (0xFF 없음)
- [ ] 테스트 파일 생성: `echo "Hello World" > test_text.txt`
- [ ] Client에서 전송: `sz test_text.txt`
- [ ] Server에서 수신 확인
- [ ] MD5 checksum 비교
- [ ] Exit code 0 확인

**Test 4.2**: 바이너리 파일 (0xFF 포함)
- [ ] 테스트 파일: `dd if=/dev/urandom of=test_1K.bin bs=1024 count=1`
- [ ] `hexdump -C test_1K.bin | grep "ff"` (0xFF 확인)
- [ ] MD5 checksum 생성: `md5sum test_1K.bin > original.md5`
- [ ] Client에서 전송: `sz test_1K.bin`
- [ ] Server에서 수신 후 MD5 비교
- [ ] **Exit code 0 확인** (이전: 128)

**Test 4.3**: JPEG 파일 (많은 0xFF 포함)
- [ ] `sz sample.jpg` 전송 (원래 실패했던 파일)
- [ ] Server에서 수신 확인
- [ ] MD5 checksum 비교
- [ ] File size 확인 (lrz가 파일을 삭제하지 않았는지)
- [ ] `lrz: sample.jpg removed.` 메시지 **나타나지 않음** 확인

**Test 4.4**: 대용량 파일
- [ ] `dd if=/dev/urandom of=test_1M.bin bs=1024 count=1024` (1MB)
- [ ] Client 전송
- [ ] 전송 시간 측정
- [ ] MD5 checksum 검증

**Test 4.5**: 최악의 경우 (All 0xFF)
- [ ] `perl -e 'print "\xFF" x 1024' > test_all_ff.bin`
- [ ] Client 전송
- [ ] **예상**: 1024 bytes → 2048 bytes (wire format)
- [ ] Server 수신 후 1024 bytes로 복원 확인
- [ ] MD5 검증

**Test 4.6**: 양방향 전송
- [ ] Client → Server: `sz test.bin`
- [ ] Server → Client: `rz` 후 server에서 `sz test.bin`
- [ ] 양방향 모두 MD5 검증

**Test 4.7**: 다양한 프로토콜
- [ ] ZMODEM: `sz -Z test.bin`
- [ ] YMODEM: `sy test.bin`
- [ ] XMODEM: `sx test.bin`
- [ ] 모든 프로토콜에서 MD5 검증

### Phase 5: 로그 분석 및 검증

**Task 5.1**: Debug log 확인
- [ ] Client log: `client_debug.log`
  - `Socket→Child: X bytes (raw) → Y bytes (unescaped)` 메시지 확인
  - `Child→Socket: X bytes (raw) → Y bytes (escaped)` 메시지 확인
  - Y >= X (escaping으로 증가) 확인
- [ ] Server log: `server_debug.log`
  - `After IAC processing: X bytes clean data` 메시지 확인
  - **데이터 손실 없음** 확인 (예: 1024 bytes → 1024 bytes)
  - `lrz: sample.jpg removed.` **나타나지 않음** 확인

**Task 5.2**: Wire format 검증
- [ ] tcpdump로 네트워크 패킷 캡처
- [ ] 0xFF 바이트가 0xFF 0xFF로 escape 되었는지 확인:
  ```bash
  sudo tcpdump -i lo -X port 9091 -w transfer.pcap
  # 전송 후
  tcpdump -r transfer.pcap -X | grep -A 10 "0xff 0xff"
  ```

**Task 5.3**: 성능 측정
- [ ] 이전 버전과 전송 속도 비교
- [ ] CPU 사용률 측정 (`top` / `htop`)
- [ ] Memory 사용량 확인
- [ ] **예상 성능 저하**: ~5-10% (IAC escaping overhead)

### Phase 6: Edge Cases 및 오류 처리

**Test 6.1**: Buffer boundary IAC sequence
- [ ] 정확히 buffer 경계에서 0xFF 발생 시나리오
- [ ] `iac_state` 변수가 버퍼 간 올바르게 유지되는지 확인

**Test 6.2**: Partial transfer 중단
- [ ] 전송 중 Ctrl+C
- [ ] 네트워크 끊김 시뮬레이션
- [ ] Timeout 발생
- [ ] 모든 경우 clean shutdown 확인

**Test 6.3**: Concurrent transfers
- [ ] 여러 파일 연속 전송
- [ ] `iac_state` reset 확인

**Test 6.4**: Malformed telnet commands
- [ ] 전송 중 텔넷 명령 수신 시 처리
- [ ] Warning log 확인
- [ ] 전송 계속 진행 확인

---

## 6. 검증 기준 (Success Criteria)

### 6.1 기능 검증

- ✅ **Exit code 0**: sample.jpg 전송 성공 (이전: 128)
- ✅ **MD5 일치**: 모든 테스트 파일 checksum 100% 일치
- ✅ **파일 유지**: `lrz: sample.jpg removed.` 메시지 나타나지 않음
- ✅ **데이터 무손실**: Server log에서 "After IAC processing" 바이트 손실 없음
- ✅ **모든 프로토콜**: ZMODEM, YMODEM, XMODEM 모두 정상 동작

### 6.2 성능 검증

- ✅ **전송 속도**: 이전 대비 5-10% 이내 성능 저하
- ✅ **메모리 사용**: 추가 메모리 사용 < 100KB (buffer 증가분)
- ✅ **CPU 사용**: IAC escaping으로 인한 CPU 증가 < 10%

### 6.3 안정성 검증

- ✅ **No crashes**: 모든 테스트 시나리오에서 segfault 없음
- ✅ **No memory leaks**: valgrind 검사 통과
- ✅ **Graceful shutdown**: 모든 오류 상황에서 clean exit
- ✅ **Buffer safety**: 모든 buffer overflow 방지 로직 동작

---

## 7. 코드 변경 요약

### 7.1 새로 추가되는 파일

```
tests/test_iac_escaping.c       # IAC escaping 단위 테스트
MODEMPLAN_2.md                  # 이 문서
```

### 7.2 수정되는 파일

```
src/transfer.c                  # telnet_escape_iac(), telnet_unescape_iac() 추가
                                # relay_data_pipes() 수정
include/transfer.h              # 함수 선언 추가
Makefile                        # 테스트 타겟 추가
```

### 7.3 예상 코드 변경량

```
src/transfer.c:
  + ~150 lines (새로운 함수)
  + ~50 lines (relay_data_pipes 수정)

include/transfer.h:
  + ~20 lines (선언)

tests/test_iac_escaping.c:
  + ~300 lines (새 파일)

Total: ~520 lines
```

---

## 8. 리스크 관리

### 8.1 잠재적 위험

**Risk 1**: Buffer overflow
- **완화**: Worst-case buffer 크기 사전 계산 (2x original)
- **검증**: 단위 테스트에서 all-0xFF 시나리오 테스트
- **모니터링**: Debug log에서 buffer 사용량 추적

**Risk 2**: 성능 저하
- **완화**: Tight loop에서 최적화된 escaping 로직
- **검증**: 벤치마크 테스트 (1MB file transfer time)
- **Fallback**: 성능이 critical하면 inline assembly 고려

**Risk 3**: IAC state 동기화 오류
- **완화**: State variable을 명시적으로 관리
- **검증**: Buffer boundary 테스트 케이스
- **디버깅**: State transition 로그 추가

**Risk 4**: Server 호환성
- **완화**: 표준 RFC 854 준수
- **검증**: 다양한 telnet 서버에서 테스트
- **문서화**: 호환성 매트릭스 작성

### 8.2 Rollback 계획

**Rollback Trigger**:
- MD5 checksum 불일치가 10% 이상 발생
- 성능 저하가 20% 이상
- Critical bug 발견 (data corruption)

**Rollback Procedure**:
```bash
# Phase 1 코드로 복원
cp src/transfer.c.phase1 src/transfer.c
make clean && make
```

**검증**:
```bash
# 기존 테스트 suite 재실행
./tests/run_all_tests.sh
```

---

## 9. 향후 최적화 (Post-MVP)

### 9.1 Phase 2 개선

**Optimization 1**: SIMD를 이용한 고속 IAC escaping
- SSE2/AVX2 intrinsics 사용
- 16 bytes 또는 32 bytes 동시 처리
- 예상 성능 향상: 2-4x

**Optimization 2**: Zero-copy buffer 관리
- Scatter-gather I/O (`writev()` / `readv()`)
- Buffer chaining으로 복사 제거

**Optimization 3**: Adaptive buffering
- 0xFF 빈도 감지하여 buffer 크기 동적 조정
- 일반 파일: 작은 버퍼
- JPEG/PNG: 큰 버퍼

### 9.2 Phase 3 개선

**Feature 1**: IAC escaping statistics
- 전송 중 escape된 0xFF 바이트 개수 추적
- Wire format overhead 계산 및 표시
- 예: "Transferred 1.05MB (5% overhead due to IAC escaping)"

**Feature 2**: Compression integration
- IAC escaping 전에 데이터 압축
- 압축된 데이터는 0xFF 빈도 감소 가능성
- ZLIB integration

**Feature 3**: Protocol negotiation extension
- Custom telnet option으로 "raw binary mode" 협상
- 지원하는 서버: IAC escaping skip
- 미지원 서버: Fallback to standard escaping

---

## 10. 참고 자료

### 10.1 RFC 및 표준

- **RFC 854**: Telnet Protocol Specification
  - Section: IAC (Interpret As Command)
  - URL: https://tools.ietf.org/html/rfc854

- **RFC 856**: Telnet Binary Transmission
  - Section: BINARY option
  - URL: https://tools.ietf.org/html/rfc856

- **ZMODEM Protocol**:
  - Original spec: Chuck Forsberg's ZMODEM.DOC
  - URL: https://gallium.inria.fr/~doligez/zmodem/zmodem.txt

### 10.2 관련 코드 및 문서

- **otelnet CLAUDE.md**: `/mnt/USERS/onion/DATA_ORIGN/Workspace/otelnet/CLAUDE.md`
- **MODEMPLAN.md** (Phase 1): `/mnt/USERS/onion/DATA_ORIGN/Workspace/otelnet/MODEMPLAN.md`
- **lrzsz-lite TEST.md**: `/mnt/USERS/onion/DATA_ORIGN/Workspace/lrzsz-lite/TEST.md`
- **telnet_server source**: `/mnt/USERS/onion/DATA_ORIGN/Workspace/02_pge_project/telnet_server_simple_test`

### 10.3 디버깅 로그

- **Client debug log**: `client_debug.log` (이 이슈 발견)
- **Server debug log**: `server_debug.log` (데이터 손실 확인)

---

## 11. 결론

### 11.1 문제 요약

**Root Cause**: ZMODEM 바이너리 데이터의 0xFF 바이트가 텔넷 IAC 명령으로 오인되어 손실됨

**Impact**:
- File transfer 실패 (exit code 128)
- 수신 파일 자동 삭제 (CRC error)
- JPEG 등 0xFF가 많은 파일 전송 불가

### 11.2 해결 방안 요약

**Solution**: Pipe-based IAC escaping/unescaping layer

**Key Components**:
1. `telnet_escape_iac()`: 0xFF → 0xFF 0xFF (송신)
2. `telnet_unescape_iac()`: 0xFF 0xFF → 0xFF (수신)
3. `relay_data_pipes()` 통합

**Expected Outcome**:
- ✅ Exit code 0 (success)
- ✅ 100% MD5 checksum match
- ✅ All file types supported (JPEG, PNG, binary, etc.)
- ✅ RFC 854 compliant

### 11.3 Next Steps

**Immediate** (Week 1):
1. Phase 1: Helper 함수 구현 및 단위 테스트
2. Phase 2: Client side 통합
3. Phase 3: Server side 통합

**Short-term** (Week 2):
4. Phase 4: 통합 테스트 (모든 시나리오)
5. Phase 5: 로그 분석 및 검증
6. Phase 6: Edge cases 테스트

**Documentation**:
7. CLAUDE.md 업데이트
8. Test report 작성
9. Performance benchmark 문서화

---

**Plan Status**: Ready for Implementation
**Estimated Effort**: 2-3 weeks
**Priority**: P0 (Critical - File transfer broken)

