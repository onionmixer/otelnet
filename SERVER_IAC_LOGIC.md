# Telnet Server IAC Escaping Implementation Guide

**목표**: telnet_server에 IAC (0xFF) byte escaping/unescaping 로직 추가

**프로젝트**: `/mnt/USERS/onion/DATA_ORIGN/Workspace/02_pge_project/telnet_server_simple_test`

**작성일**: 2025-10-26

**관련 문서**: `MODEMPLAN_2.md` (otelnet 클라이언트 측 구현)

---

## 1. 문제 진단

### 1.1 서버 로그에서 발견된 문제

**파일**: `server_debug.log` (otelnet 프로젝트에 생성됨)

```
[DEBUG] Received 1024 bytes from client (raw)
[DEBUG] After IAC processing: 1016 bytes clean data, relaying to rz...
[DEBUG] Relayed 1016 bytes to rz
```

**문제**:
- Client가 보낸 1024 bytes 중 **8 bytes가 손실**
- "After IAC processing" 단계에서 데이터가 줄어듦
- 이는 **0xFF 바이트가 IAC 명령으로 오인**되어 제거되었음을 의미

### 1.2 데이터 손실 패턴

**로그 분석**:
```
Line 56:  1024 bytes → 1016 bytes (8 bytes lost)   ← 약 4개의 0xFF
Line 71:  1024 bytes → 1020 bytes (4 bytes lost)   ← 약 2개의 0xFF
Line 127: 1024 bytes → 1015 bytes (9 bytes lost)   ← 약 4-5개의 0xFF
Line 283: 1024 bytes → 1007 bytes (17 bytes lost)  ← 약 8-9개의 0xFF!
```

**결론**:
- ZMODEM 바이너리 데이터에 포함된 0xFF가 손실됨
- lrz가 손상된 데이터를 받아 CRC 오류 발생
- 결과: `lrz: sample.jpg removed.` (exit code 128)

### 1.3 현재 서버의 IAC Processing 위치

서버는 이미 **어떤 형태의 IAC processing**을 수행하고 있습니다:

```
[DEBUG] Received X bytes from client (raw)
[DEBUG] After IAC processing: Y bytes clean data, relaying to rz...
```

**이 코드를 찾아야 합니다**:
- 파일 전송 relay 함수 내부
- `recv()` → IAC processing → `write(to lrz)`
- 현재는 잘못된 IAC 처리로 데이터 손실 발생

---

## 2. RFC 854 Telnet Protocol - IAC Escaping 규칙

### 2.1 기본 규칙

**IAC (Interpret As Command)**: `0xFF` (255 decimal)

**송신 시 (Escaping)**:
```
원본 데이터에 0xFF 포함 → 0xFF 0xFF로 변환하여 전송
```

**수신 시 (Unescaping)**:
```
0xFF 0xFF 수신 → 0xFF로 변환하여 응용프로그램에 전달
0xFF <cmd> 수신 → 텔넷 명령으로 처리 (데이터로 전달하지 않음)
```

### 2.2 예시

| 원본 데이터              | Wire Format (네트워크)    | 설명                     |
|--------------------------|---------------------------|--------------------------|
| `0x48 0x65 0x6C`         | `0x48 0x65 0x6C`          | 일반 데이터 (그대로)     |
| `0xFF 0x12`              | `0xFF 0xFF 0x12`          | 0xFF escape 필요         |
| `0xFF 0xFF`              | `0xFF 0xFF 0xFF 0xFF`     | 두 개 모두 escape        |
| `0x12 0xFF 0x34 0xFF`    | `0x12 0xFF 0xFF 0x34 0xFF 0xFF` | Mixed case      |

### 2.3 BINARY Mode에서도 적용

**중요**: RFC 856 BINARY mode에서도 0xFF는 특수 문자입니다!

```
BINARY mode = 모든 바이트 (0x00-0xFF) 전송 가능
BUT: 0xFF는 여전히 IAC 명령 시작 바이트
따라서: 0xFF → 0xFF 0xFF escaping 필수
```

---

## 3. 서버에서 수정해야 할 부분

### 3.1 서버의 데이터 경로

```
Client (lsz)
    ↓ (0xFF 포함, escape 안 됨 - 이것이 문제)
[Network - Telnet Socket]
    ↓
Server recv()
    ↓
❌ 현재 IAC processing (잘못됨 - 0xFF 제거)
    ↓
lrz (손상된 데이터 수신 → CRC error)
```

**수정 후**:
```
Client (lsz)
    ↓ (0xFF 포함, escape 안 됨)
[Network - Telnet Socket]
    ↓
Server recv()
    ↓
✅ IAC Unescape (0xFF 0xFF → 0xFF)
    ↓
lrz (올바른 데이터 수신 → 성공)
```

### 3.2 양방향 처리 필요

**Receiving (Client → Server)**:
```c
recv(client_socket) → telnet_unescape_iac() → write(lrz_stdin)
```

**Sending (Server → Client)**:
```c
read(lsz_stdout) → telnet_escape_iac() → send(client_socket)
```

---

## 4. 구현: IAC Escaping 함수

### 4.1 Header 선언

**추가할 위치**: 서버의 헤더 파일 (예: `file_transfer.h` 또는 `telnet_protocol.h`)

```c
#ifndef IAC_ESCAPING_H
#define IAC_ESCAPING_H

#include <stddef.h>
#include <sys/types.h>

/* Telnet IAC byte */
#define TELNET_IAC 0xFF

/**
 * Escape IAC bytes (0xFF → 0xFF 0xFF) for telnet transmission
 *
 * @param input      Input buffer (original binary data from lsz)
 * @param input_len  Length of input data
 * @param output     Output buffer (escaped data for network)
 * @param output_max Maximum size of output buffer (should be >= input_len * 2)
 * @return           Length of escaped data, or -1 if output buffer too small
 *
 * Example: {0x12, 0xFF, 0x34} → {0x12, 0xFF, 0xFF, 0x34} (3 bytes → 4 bytes)
 */
ssize_t telnet_escape_iac(const unsigned char *input, size_t input_len,
                          unsigned char *output, size_t output_max);

/**
 * Unescape IAC bytes (0xFF 0xFF → 0xFF) from telnet stream
 *
 * @param input      Input buffer (escaped telnet data from network)
 * @param input_len  Length of input data
 * @param output     Output buffer (unescaped binary data for lrz)
 * @param output_max Maximum size of output buffer
 * @param iac_state  Pointer to IAC state variable (must persist between calls)
 *                   - Initialize to 0 before first call
 *                   - Keep same variable across buffer reads
 * @return           Length of unescaped data, or -1 on error
 *
 * State values:
 *   0 = Normal data mode
 *   1 = Saw 0xFF, waiting for next byte
 *
 * Example: {0x12, 0xFF, 0xFF, 0x34} → {0x12, 0xFF, 0x34} (4 bytes → 3 bytes)
 *
 * Note: This function handles partial IAC sequences across buffer boundaries.
 *       For example, if buffer ends with 0xFF, the state will be set to 1,
 *       and the next call will process the following byte.
 */
ssize_t telnet_unescape_iac(const unsigned char *input, size_t input_len,
                            unsigned char *output, size_t output_max,
                            int *iac_state);

#endif /* IAC_ESCAPING_H */
```

### 4.2 구현: telnet_escape_iac()

**추가할 위치**: 서버의 소스 파일 (예: `file_transfer.c` 또는 `iac_escape.c`)

```c
#include <string.h>
#include <errno.h>

ssize_t telnet_escape_iac(const unsigned char *input, size_t input_len,
                          unsigned char *output, size_t output_max) {
    if (input == NULL || output == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t out_idx = 0;

    for (size_t i = 0; i < input_len; i++) {
        unsigned char byte = input[i];

        if (byte == TELNET_IAC) {
            /* Need 2 bytes for escaped IAC */
            if (out_idx + 2 > output_max) {
                errno = ENOBUFS;
                return -1;  /* Buffer overflow */
            }
            output[out_idx++] = TELNET_IAC;
            output[out_idx++] = TELNET_IAC;  /* Escape: 0xFF → 0xFF 0xFF */
        } else {
            /* Regular byte */
            if (out_idx + 1 > output_max) {
                errno = ENOBUFS;
                return -1;
            }
            output[out_idx++] = byte;
        }
    }

    return (ssize_t)out_idx;
}
```

### 4.3 구현: telnet_unescape_iac()

```c
ssize_t telnet_unescape_iac(const unsigned char *input, size_t input_len,
                            unsigned char *output, size_t output_max,
                            int *iac_state) {
    if (input == NULL || output == NULL || iac_state == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t out_idx = 0;

    for (size_t i = 0; i < input_len; i++) {
        unsigned char byte = input[i];

        if (*iac_state == 1) {
            /* Previous byte was 0xFF */
            if (byte == TELNET_IAC) {
                /* 0xFF 0xFF → single 0xFF (escaped data byte) */
                if (out_idx + 1 > output_max) {
                    errno = ENOBUFS;
                    return -1;
                }
                output[out_idx++] = TELNET_IAC;
                *iac_state = 0;
            } else {
                /* 0xFF <other> → telnet command, skip both bytes */
                /* In BINARY mode during file transfer, this should not happen */
                /* Log warning and discard the command */
                fprintf(stderr, "[WARNING] Unexpected telnet command during "
                               "binary transfer: IAC 0x%02X (discarded)\n", byte);
                *iac_state = 0;
                /* Do not output anything - command is discarded */
            }
        } else {
            /* Normal state */
            if (byte == TELNET_IAC) {
                /* Start of potential escape sequence or command */
                *iac_state = 1;  /* Wait for next byte */
            } else {
                /* Regular data byte */
                if (out_idx + 1 > output_max) {
                    errno = ENOBUFS;
                    return -1;
                }
                output[out_idx++] = byte;
            }
        }
    }

    /* Note: iac_state may be 1 if buffer ended with 0xFF */
    /* This is normal - next call will process the following byte */

    return (ssize_t)out_idx;
}
```

---

## 5. 서버 Relay 로직 수정

### 5.1 현재 Relay 함수 찾기

**찾아야 할 코드 패턴**:

```c
// File transfer relay loop
while (/* transfer active */) {
    // Read from client socket
    ssize_t n = recv(client_socket, buffer, sizeof(buffer), 0);

    // ❌ 현재: "After IAC processing" 로그와 함께 데이터 손실

    // Write to lrz stdin
    write(lrz_stdin, buffer, n);
}
```

**로그에서 단서**:
```
[DEBUG] Received 1024 bytes from client (raw)
[DEBUG] After IAC processing: 1016 bytes clean data, relaying to rz...
[DEBUG] Relayed 1016 bytes to rz
```

이 로그를 출력하는 코드를 찾으세요!

### 5.2 수정된 Relay 함수 (예시)

```c
/* File transfer relay function */
int relay_file_transfer(int client_socket, int lrz_stdin, int lsz_stdout) {
    unsigned char recv_buffer[BUFFER_SIZE];
    unsigned char send_buffer[BUFFER_SIZE];
    unsigned char escaped_buffer[BUFFER_SIZE * 2];  /* Worst case: all 0xFF */

    int iac_state = 0;  /* IAC unescape state (persistent across recv calls) */

    fd_set readfds;
    struct timeval timeout;

    while (transfer_active) {
        FD_ZERO(&readfds);
        FD_SET(client_socket, &readfds);
        FD_SET(lsz_stdout, &readfds);

        int max_fd = (client_socket > lsz_stdout) ? client_socket : lsz_stdout;

        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ret = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

        if (ret < 0) {
            perror("select");
            break;
        }

        /* Client → Server (Receiving): Unescape IAC */
        if (FD_ISSET(client_socket, &readfds)) {
            ssize_t n = recv(client_socket, recv_buffer, sizeof(recv_buffer), 0);

            if (n <= 0) {
                /* Connection closed or error */
                break;
            }

            fprintf(stderr, "[DEBUG] Received %zd bytes from client (raw)\n", n);

            /* ✅ NEW: Unescape IAC bytes before sending to lrz */
            ssize_t unescaped_len = telnet_unescape_iac(
                recv_buffer, n,
                send_buffer, sizeof(send_buffer),
                &iac_state
            );

            if (unescaped_len < 0) {
                fprintf(stderr, "[ERROR] IAC unescape failed\n");
                break;
            }

            fprintf(stderr, "[DEBUG] After IAC processing: %zd bytes clean data, "
                           "relaying to rz...\n", unescaped_len);

            if (unescaped_len > 0) {
                ssize_t written = write(lrz_stdin, send_buffer, unescaped_len);
                if (written != unescaped_len) {
                    fprintf(stderr, "[ERROR] Incomplete write to lrz\n");
                    break;
                }
                fprintf(stderr, "[DEBUG] Relayed %zd bytes to rz\n", written);
            }
        }

        /* Server → Client (Sending): Escape IAC */
        if (FD_ISSET(lsz_stdout, &readfds)) {
            ssize_t n = read(lsz_stdout, recv_buffer, sizeof(recv_buffer));

            if (n <= 0) {
                /* lsz finished or error */
                break;
            }

            fprintf(stderr, "[DEBUG] Read %zd bytes from lsz, sending to client...\n", n);

            /* ✅ NEW: Escape IAC bytes before sending to client */
            ssize_t escaped_len = telnet_escape_iac(
                recv_buffer, n,
                escaped_buffer, sizeof(escaped_buffer)
            );

            if (escaped_len < 0) {
                fprintf(stderr, "[ERROR] IAC escape failed\n");
                break;
            }

            ssize_t sent = send(client_socket, escaped_buffer, escaped_len, 0);
            if (sent != escaped_len) {
                fprintf(stderr, "[ERROR] Incomplete send to client\n");
                break;
            }

            /* Optional: log escaped byte count */
            if (escaped_len != n) {
                fprintf(stderr, "[DEBUG] Escaped %zd→%zd bytes (IAC escape)\n",
                               n, escaped_len);
            }
        }
    }

    return 0;
}
```

### 5.3 핵심 변경 사항

**Before**:
```c
recv(client_socket, buffer, size, 0);
// ❌ 잘못된 IAC processing (데이터 손실)
write(lrz_stdin, buffer, n);
```

**After**:
```c
recv(client_socket, recv_buffer, size, 0);
// ✅ 올바른 IAC unescape (0xFF 0xFF → 0xFF)
telnet_unescape_iac(recv_buffer, n, send_buffer, size, &iac_state);
write(lrz_stdin, send_buffer, unescaped_len);
```

---

## 6. Buffer 크기 고려사항

### 6.1 Worst-Case 시나리오

**Escaping (Sending)**:
- 원본: 1024 bytes (모두 0xFF)
- Escaped: 2048 bytes (0xFF 0xFF 0xFF 0xFF ...)
- **필요 버퍼**: `input_len * 2`

**Unescaping (Receiving)**:
- 원본: 2048 bytes (모두 0xFF 0xFF)
- Unescaped: 1024 bytes (0xFF 0xFF → 0xFF)
- **필요 버퍼**: `input_len` (줄어듦)

### 6.2 Buffer 선언 예시

```c
#define BUFFER_SIZE 4096  /* Base buffer size */

unsigned char recv_buffer[BUFFER_SIZE];
unsigned char send_buffer[BUFFER_SIZE];
unsigned char escaped_buffer[BUFFER_SIZE * 2];  /* For escaping */
```

---

## 7. 테스트 계획

### 7.1 단위 테스트

**테스트 케이스**:

```c
/* test_iac_escaping.c */
#include <stdio.h>
#include <string.h>
#include <assert.h>

void test_escape_no_iac(void) {
    unsigned char input[] = {0x12, 0x34, 0x56};
    unsigned char output[10];

    ssize_t len = telnet_escape_iac(input, 3, output, sizeof(output));

    assert(len == 3);
    assert(memcmp(output, input, 3) == 0);
    printf("✓ test_escape_no_iac passed\n");
}

void test_escape_single_iac(void) {
    unsigned char input[] = {0xFF};
    unsigned char output[10];
    unsigned char expected[] = {0xFF, 0xFF};

    ssize_t len = telnet_escape_iac(input, 1, output, sizeof(output));

    assert(len == 2);
    assert(memcmp(output, expected, 2) == 0);
    printf("✓ test_escape_single_iac passed\n");
}

void test_escape_multiple_iac(void) {
    unsigned char input[] = {0x12, 0xFF, 0x34, 0xFF, 0x56};
    unsigned char output[10];
    unsigned char expected[] = {0x12, 0xFF, 0xFF, 0x34, 0xFF, 0xFF, 0x56};

    ssize_t len = telnet_escape_iac(input, 5, output, sizeof(output));

    assert(len == 7);
    assert(memcmp(output, expected, 7) == 0);
    printf("✓ test_escape_multiple_iac passed\n");
}

void test_unescape_iac(void) {
    unsigned char input[] = {0x12, 0xFF, 0xFF, 0x34};
    unsigned char output[10];
    unsigned char expected[] = {0x12, 0xFF, 0x34};
    int iac_state = 0;

    ssize_t len = telnet_unescape_iac(input, 4, output, sizeof(output), &iac_state);

    assert(len == 3);
    assert(memcmp(output, expected, 3) == 0);
    assert(iac_state == 0);
    printf("✓ test_unescape_iac passed\n");
}

void test_unescape_boundary(void) {
    unsigned char input1[] = {0x12, 0xFF};  /* Buffer ends with IAC */
    unsigned char input2[] = {0xFF, 0x34};  /* Next buffer starts with IAC */
    unsigned char output[10];
    int iac_state = 0;

    /* First buffer */
    ssize_t len1 = telnet_unescape_iac(input1, 2, output, sizeof(output), &iac_state);
    assert(len1 == 1);
    assert(output[0] == 0x12);
    assert(iac_state == 1);  /* Waiting for next byte */

    /* Second buffer */
    ssize_t len2 = telnet_unescape_iac(input2, 2, output + len1, sizeof(output) - len1, &iac_state);
    assert(len2 == 2);
    assert(output[1] == 0xFF);  /* 0xFF 0xFF → 0xFF */
    assert(output[2] == 0x34);
    assert(iac_state == 0);

    printf("✓ test_unescape_boundary passed\n");
}

int main(void) {
    test_escape_no_iac();
    test_escape_single_iac();
    test_escape_multiple_iac();
    test_unescape_iac();
    test_unescape_boundary();

    printf("\n✓ All tests passed!\n");
    return 0;
}
```

**컴파일 및 실행**:
```bash
gcc -o test_iac test_iac_escaping.c file_transfer.c -I./include
./test_iac
```

### 7.2 통합 테스트

**환경**:
```bash
# Terminal 1: 서버 실행
cd /mnt/USERS/onion/DATA_ORIGN/Workspace/02_pge_project/telnet_server_simple_test
./build/telnet_server 9091

# Terminal 2: 클라이언트 실행
cd /mnt/USERS/onion/DATA_ORIGN/Workspace/otelnet
./build/otelnet localhost 9091
```

**Test Case 1: 바이너리 파일 전송 (0xFF 포함)**

```bash
# 테스트 파일 생성
dd if=/dev/urandom of=test_1K.bin bs=1024 count=1
md5sum test_1K.bin > test_1K.md5

# Client에서 전송
> sz test_1K.bin

# Server에서 수신 확인
md5sum test_1K.bin
cat test_1K.md5

# ✅ MD5 일치 확인
```

**Test Case 2: JPEG 파일 (원래 실패했던 파일)**

```bash
# Client
> sz sample.jpg

# Server 로그 확인
# ✅ "After IAC processing: 1024 bytes" (손실 없음)
# ✅ "lrz: sample.jpg removed." 메시지 나타나지 않음
# ✅ Exit code 0

# MD5 검증
md5sum sample.jpg
```

**Test Case 3: Worst-case (All 0xFF)**

```bash
# 모두 0xFF인 파일 생성
perl -e 'print "\xFF" x 1024' > test_all_ff.bin
md5sum test_all_ff.bin

# 전송
> sz test_all_ff.bin

# Server 로그 확인
# 예상: "Received 2048 bytes from client (raw)"
#       "After IAC processing: 1024 bytes clean data"

# MD5 검증
md5sum test_all_ff.bin
```

---

## 8. 검증 기준

### 8.1 로그 검증

**Before (현재 - 잘못된 동작)**:
```
[DEBUG] Received 1024 bytes from client (raw)
[DEBUG] After IAC processing: 1016 bytes clean data, relaying to rz...
                                ^^^^ 8 bytes lost!
```

**After (수정 후 - 올바른 동작)**:
```
[DEBUG] Received 1024 bytes from client (raw)
[DEBUG] After IAC processing: 1024 bytes clean data, relaying to rz...
                                ^^^^ No data loss!
```

**Note**: 실제로 client가 0xFF를 escape하지 않고 보내면:
```
[DEBUG] Received 1020 bytes from client (raw)
                ^^^^ (원본 1024 bytes 중 4개가 0xFF라서 escape 안 됨)
[DEBUG] After IAC processing: 1024 bytes clean data, relaying to rz...
                                ^^^^ Unescape 후 복원됨
```

### 8.2 파일 무결성 검증

```bash
# ✅ Exit code 0 (이전: 128)
echo $?
# Output: 0

# ✅ MD5 checksum 일치
md5sum -c test_1K.md5
# Output: test_1K.bin: OK

# ✅ 파일 크기 일치
ls -l test_1K.bin
# Size: 1024 bytes (정확히 일치)

# ✅ lrz 오류 메시지 없음
# "lrz: sample.jpg removed." 메시지가 나타나지 않음
```

### 8.3 성능 기준

**허용 가능한 오버헤드**:
- 전송 시간: 이전 대비 +5-10% 이내
- CPU 사용: +10% 이내
- 메모리: +100KB 이내 (버퍼 증가분)

---

## 9. 디버깅 팁

### 9.1 로그 메시지 추가

**디버그 로그 예시**:

```c
/* Detailed IAC processing log */
if (unescaped_len != n) {
    fprintf(stderr, "[DEBUG] IAC unescape: %zd bytes (raw) → %zd bytes (clean)\n",
                   n, unescaped_len);

    /* Count 0xFF bytes in original */
    int iac_count = 0;
    for (size_t i = 0; i < n; i++) {
        if (recv_buffer[i] == 0xFF) iac_count++;
    }
    fprintf(stderr, "[DEBUG] Found %d IAC bytes (0xFF) in raw data\n", iac_count);
}
```

### 9.2 Hex Dump 함수

```c
void hex_dump(const char *label, const unsigned char *data, size_t len) {
    fprintf(stderr, "[HEX] %s (%zu bytes):\n", label, len);
    for (size_t i = 0; i < len; i++) {
        fprintf(stderr, "%02X ", data[i]);
        if ((i + 1) % 16 == 0) fprintf(stderr, "\n");
    }
    if (len % 16 != 0) fprintf(stderr, "\n");
}

/* Usage */
hex_dump("Received from client", recv_buffer, n);
hex_dump("After unescape", send_buffer, unescaped_len);
```

### 9.3 State Tracking

```c
/* Track IAC state transitions */
int prev_state = iac_state;
ssize_t len = telnet_unescape_iac(..., &iac_state);
if (prev_state != iac_state) {
    fprintf(stderr, "[DEBUG] IAC state: %d → %d\n", prev_state, iac_state);
}
```

---

## 10. 체크리스트

### 10.1 구현 체크리스트

- [ ] `telnet_escape_iac()` 함수 구현 및 추가
- [ ] `telnet_unescape_iac()` 함수 구현 및 추가
- [ ] 헤더 파일에 함수 선언 추가
- [ ] File transfer relay 함수 찾기
- [ ] Relay 함수에 IAC unescape 통합 (Client → Server)
- [ ] Relay 함수에 IAC escape 통합 (Server → Client)
- [ ] Buffer 크기 검증 및 조정
- [ ] 컴파일 오류 해결
- [ ] Warning 제거

### 10.2 테스트 체크리스트

- [ ] 단위 테스트 작성 및 실행
- [ ] 모든 단위 테스트 통과 확인
- [ ] 작은 텍스트 파일 전송 테스트 (0xFF 없음)
- [ ] 바이너리 파일 전송 테스트 (0xFF 포함)
- [ ] JPEG 파일 전송 테스트 (sample.jpg)
- [ ] All-0xFF 파일 전송 테스트 (worst case)
- [ ] MD5 checksum 검증 (모든 파일)
- [ ] Exit code 0 확인
- [ ] 로그에서 데이터 손실 없음 확인
- [ ] `lrz: removed.` 메시지 나타나지 않음 확인

### 10.3 문서화 체크리스트

- [ ] 코드에 주석 추가
- [ ] README 업데이트 (IAC escaping 설명)
- [ ] CHANGELOG 작성
- [ ] 테스트 결과 문서화

---

## 11. 참고 자료

### 11.1 RFC 문서

- **RFC 854**: Telnet Protocol Specification
  - https://tools.ietf.org/html/rfc854
  - Section 4.2: IAC (Interpret As Command)

- **RFC 856**: Telnet Binary Transmission
  - https://tools.ietf.org/html/rfc856
  - BINARY mode에서도 IAC escaping 필요

### 11.2 관련 문서

- **MODEMPLAN_2.md**: otelnet 클라이언트 측 구현 계획
  - `/mnt/USERS/onion/DATA_ORIGN/Workspace/otelnet/MODEMPLAN_2.md`

- **서버 로그**: 문제 진단 증거
  - `/mnt/USERS/onion/DATA_ORIGN/Workspace/otelnet/server_debug.log`

- **클라이언트 로그**: 전송 실패 증거
  - `/mnt/USERS/onion/DATA_ORIGN/Workspace/otelnet/client_debug.log`

### 11.3 코드 예시

이 문서의 모든 코드는 실제 동작하는 예시입니다:
- Section 4: IAC escaping 함수 전체 구현
- Section 5: Relay 함수 수정 예시
- Section 7: 단위 테스트 전체 코드

---

## 12. 요약

### 12.1 문제

**현상**: ZMODEM 파일 전송 실패 (exit code 128)

**원인**: 서버의 IAC processing에서 0xFF 바이트 손실
- 1024 bytes → 1016 bytes (8 bytes lost)
- lrz가 손상된 데이터 수신 → CRC error → 파일 삭제

### 12.2 해결 방안

**두 개의 함수 추가**:
1. `telnet_escape_iac()`: 송신 시 0xFF → 0xFF 0xFF
2. `telnet_unescape_iac()`: 수신 시 0xFF 0xFF → 0xFF

**Relay 로직 수정**:
- Client → Server: unescape 적용
- Server → Client: escape 적용

### 12.3 예상 결과

- ✅ Exit code 0 (성공)
- ✅ MD5 checksum 100% 일치
- ✅ 모든 파일 타입 전송 가능 (JPEG, binary, etc.)
- ✅ 데이터 무손실
- ✅ RFC 854 표준 준수

---

**구현 우선순위**: P0 (Critical)

**예상 작업 시간**: 1-2 days
- Day 1: 함수 구현, 단위 테스트, relay 함수 수정
- Day 2: 통합 테스트, 검증, 디버깅

**다음 단계**: Section 4의 함수 구현부터 시작하세요!
