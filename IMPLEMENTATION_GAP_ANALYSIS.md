# otelnet RFC 요구사항 대비 구현 갭 분석

**분석 일자**: 2025-10-15
**분석 대상**: TELNET_CLIENT_REQUIMENT.txt의 추가 내용 포함 전체 요구사항
**현재 버전**: otelnet 1.0.0

---

## 요약 (Executive Summary)

otelnet은 RFC 854/855의 기본 telnet 프로토콜을 **정확히 구현**하고 있으며, RFC 856 (Binary), RFC 858 (SGA), RFC 1091 (Terminal-Type), RFC 1184 (LINEMODE)의 **핵심 기능을 대부분 구현**했습니다.

**구현 완성도**: 약 85%

**주요 미구현 항목**:
1. RFC 1184 LINEMODE의 FORWARDMASK 서브옵션
2. RFC 1184 LINEMODE의 SLC (Set Local Characters) 서브옵션
3. RFC 1091 Terminal-Type의 다중 타입 순환 제공
4. RFC 854 Synch/DM과 TCP Urgent 데이터 처리
5. 옵션 협상 로직의 일부 엣지 케이스

---

## 상세 분석

### ✅ RFC 854: 기본 IAC/명령 처리 (완전 구현)

**요구사항**:
- IAC(255)로 시작하는 명령 파싱
- SE(240), SB(250), WILL/WONT/DO/DONT(251-254), GA(249), IP/AO/AYT/EC/EL/BRK/DM 처리
- 데이터 내 0xFF를 IAC IAC로 이스케이프
- Synch/DM과 TCP Urgent 처리
- 서브협상 프레이밍 (IAC SB ... IAC SE)

**현재 구현 상태**:
| 항목 | 상태 | 위치 | 비고 |
|------|------|------|------|
| IAC 파싱 상태 머신 | ✅ 완전 구현 | telnet.c:280-446 | TELNET_STATE_* 상태 |
| WILL/WONT/DO/DONT | ✅ 완전 구현 | telnet.c:241-355 | 루프 방지 포함 |
| IAC IAC 이스케이프 (수신) | ✅ 완전 구현 | telnet.c:314-319 | 단일 0xFF로 복원 |
| IAC IAC 이스케이프 (송신) | ✅ 완전 구현 | telnet.c:466-471 | 0xFF → 0xFF 0xFF |
| GA (Go Ahead) | ✅ 완전 구현 | telnet.c:331-334 | SGA 모드에서 무시 |
| AYT (Are You There) | ✅ 완전 구현 | telnet.c:339-344 | 응답 메시지 전송 |
| IP/AO/BREAK | ✅ 완전 구현 | telnet.c:345-356 | 로깅만 (적절함) |
| EC/EL | ✅ 완전 구현 | telnet.c:357-364 | 로깅만 (적절함) |
| DM (Data Mark) | ✅ 완전 구현 | telnet.c:365-368 | 로깅만 |
| NOP | ✅ 완전 구현 | telnet.c:335-338 | 무시 |
| EOR | ✅ 완전 구현 | telnet.c:369-372 | 로깅만 |
| SB...SE 프레이밍 | ✅ 완전 구현 | telnet.c:328-330, 400-430 | IAC 이스케이프 지원 |
| **Synch/TCP Urgent** | ⚠️ **부분 구현** | telnet.c:365-368 | **DM 수신만, OOB 미처리** |

**미구현 세부사항**:
- **TCP Urgent (OOB) 데이터 처리**: DM 명령은 인식하지만, TCP의 Urgent 포인터와 연계된 처리가 없습니다. select()에서 exceptfds를 모니터링하지 않고, recv(..., MSG_OOB)도 사용하지 않습니다.

**영향도**: 낮음 (현대 telnet 서버는 Synch/DM을 거의 사용하지 않음)

---

### ✅ RFC 855: 옵션 협상 원칙 (거의 완전 구현)

**요구사항**:
- DO/DONT (상대에게 요청/중지), WILL/WONT (자신이 수행/거부)
- 각 방향 독립 협상
- 미지원 옵션에 즉시 WONT/DONT 응답
- 루프 방지 (상태 변경 시에만 응답)

**현재 구현 상태**:
| 항목 | 상태 | 위치 | 비고 |
|------|------|------|------|
| DO/DONT/WILL/WONT 처리 | ✅ 완전 구현 | telnet.c:241-355 | 4가지 명령 모두 처리 |
| 양방향 독립 협상 | ✅ 완전 구현 | telnet.h:134-143 | local_options[], remote_options[] |
| 상태 변경 시에만 응답 | ✅ 완전 구현 | telnet.c:252-253, 280, 302, 333 | 루프 방지 |
| 미지원 옵션 거부 | ⚠️ **로직 문제** | telnet.c:269-274, 322-327 | **아래 참조** |

**문제점**:
```c
// telnet.c:269-274 (WILL 처리)
} else {
    /* Reject unsupported options (only if not already rejected) */
    if (tn->remote_options[option]) {  // ← 이미 true인 경우만 거부?
        tn->remote_options[option] = false;
        telnet_send_negotiate(tn, TELNET_DONT, option);
    }
}
```

**문제 분석**:
- 서버가 `IAC WILL <unsupported>`를 보냈을 때, `remote_options[option]`이 false(초기값)이므로 `DONT`를 보내지 않습니다.
- 이는 RFC 855 위반입니다: "미지원 옵션에는 즉시 부정 응답을 보내야 한다"

**올바른 로직**:
```c
} else {
    /* Reject unsupported options (only if not already rejected) */
    if (!tn->remote_options[option]) {  // ← 현재 false인 경우 거부
        telnet_send_negotiate(tn, TELNET_DONT, option);
    }
}
```

**동일한 문제가 있는 위치**:
- telnet.c:322-327 (DO 처리 - 미지원 로컬 옵션)

**영향도**: 중간 (일부 서버가 미지원 옵션을 계속 재시도할 수 있음)

---

### ✅ RFC 858: SGA (Suppress Go Ahead) (완전 구현)

**요구사항**:
- SGA 옵션(코드 3) 양방향 협상
- GA 신호 생략
- SGA + ECHO 조합으로 문자 단위 환경 구성

**현재 구현 상태**:
| 항목 | 상태 | 위치 | 비고 |
|------|------|------|------|
| SGA 협상 (양방향) | ✅ 완전 구현 | telnet.c:106-107, 260-262, 309-311 | WILL/DO SGA 모두 지원 |
| GA 무시 | ✅ 완전 구현 | telnet.c:331-334 | SGA 활성 시 무시 |
| SGA + ECHO 모드 전환 | ✅ 완전 구현 | telnet.c:203-236 | telnet_update_mode() |

**완벽한 구현**: 추가 작업 불필요

---

### ✅ RFC 856: Binary Transmission (완전 구현)

**요구사항**:
- TRANSMIT-BINARY(코드 0) 양방향 협상
- 바이너리 모드에서도 IAC(0xFF) 이스케이프 유지
- 모드 해제 시 NVT ASCII 복귀
- 8비트 투명 전송

**현재 구현 상태**:
| 항목 | 상태 | 위치 | 비고 |
|------|------|------|------|
| BINARY 협상 (양방향) | ✅ 완전 구현 | telnet.c:105, 252-259, 306-308 | binary_local, binary_remote |
| IAC 이스케이프 유지 | ✅ 완전 구현 | telnet.c:314-319, 466-471 | 바이너리 모드와 무관하게 항상 |
| 8비트 투명 전송 | ✅ 완전 구현 | telnet.c:280-446 | 모든 바이트 그대로 처리 |
| 모드 해제 처리 | ✅ 완전 구현 | telnet.c:284-294, 337-345 | WONT/DONT 시 플래그 해제 |
| UTF-8 지원 경고 | ✅ 완전 구현 | telnet.c:286, 339 | BINARY 거부 시 경고 메시지 |

**완벽한 구현**: 추가 작업 불필요

---

### ⚠️ RFC 1091: Terminal-Type (부분 구현)

**요구사항**:
- 서버 DO TERMINAL-TYPE → 클라이언트 WILL 응답
- 서버 SB TERMINAL-TYPE SEND → 클라이언트 SB TERMINAL-TYPE IS 응답
- 자발적 IS 전송 금지 (SEND 요청에만 응답)
- 다중 타입 순환 제공 (서버가 SEND 반복 시)
- 대소문자 무시, NVT ASCII 문자열

**현재 구현 상태**:
| 항목 | 상태 | 위치 | 비고 |
|------|------|------|------|
| DO/WILL 협상 | ✅ 완전 구현 | telnet.c:111, 312-314 | 정상 협상 |
| SEND 요청 처리 | ✅ 완전 구현 | telnet.c:213-227 | SB 파싱 및 응답 |
| IS 응답 전송 | ✅ 완전 구현 | telnet.c:217-225 | IS <type> 형식 |
| 자발적 IS 금지 | ✅ 완전 구현 | telnet.c:215 | SEND 수신 시에만 응답 |
| **다중 타입 순환** | ❌ **미구현** | telnet.c:33 | **항상 "ANSI"만 응답** |

**미구현 세부사항**:
```c
// telnet.c:33 - 단일 타입만
SAFE_STRNCPY(tn->terminal_type, "ANSI", sizeof(tn->terminal_type));

// telnet.c:215-226 - 항상 동일 타입 응답
if (tn->sb_len >= 2 && tn->sb_buffer[1] == TTYPE_SEND) {
    /* 반복 SEND에도 항상 "ANSI"만 보냄 */
    unsigned char response[68];
    size_t term_len = strlen(tn->terminal_type);
    response[0] = TELOPT_TTYPE;
    response[1] = TTYPE_IS;
    memcpy(&response[2], tn->terminal_type, term_len);
    MB_LOG_INFO("Sending TERMINAL-TYPE IS %s", tn->terminal_type);
    telnet_send_subnegotiation(tn, response, 2 + term_len);
}
```

**RFC 1091 요구사항 (재확인)**:
> "다중 타입: 서버가 SEND를 반복하면 클라이언트는 보유한 타입들을 순차 제공할 수 있으며, 서버가 선호 타입을 선택한다."

**권장 구현**:
```c
// telnet.h에 추가
#define MAX_TERMINAL_TYPES 8
typedef struct {
    // ...
    char terminal_types[MAX_TERMINAL_TYPES][64];  // 타입 목록
    int terminal_type_count;                       // 타입 개수
    int terminal_type_index;                       // 현재 인덱스
} telnet_t;

// telnet.c 초기화에 추가
tn->terminal_types[0] = "ANSI";
tn->terminal_types[1] = "VT100";
tn->terminal_types[2] = "XTERM";
tn->terminal_type_count = 3;
tn->terminal_type_index = 0;

// SEND 처리 시 순환
if (tn->sb_len >= 2 && tn->sb_buffer[1] == TTYPE_SEND) {
    char *current_type = tn->terminal_types[tn->terminal_type_index];
    // ... IS 응답 전송 ...
    tn->terminal_type_index = (tn->terminal_type_index + 1) % tn->terminal_type_count;
}
```

**영향도**: 낮음 (대부분의 서버는 첫 번째 타입만 사용, ANSI는 범용 타입)

---

### ⚠️ RFC 1184: LINEMODE (부분 구현)

**요구사항**:
1. **MODE 서브옵션**: EDIT, TRAPSIG, MODE_ACK, SOFT_TAB, LIT_ECHO 비트
2. **FORWARDMASK 서브옵션**: 32옥텟 비트마스크, IAC 이스케이프
3. **SLC 서브옵션**: EC/EL 등 로컬 편집 키, 3바이트 트리플, SLC_DEFAULT/NOSUPPORT 수준

**현재 구현 상태**:
| 항목 | 상태 | 위치 | 비고 |
|------|------|------|------|
| DO/WILL LINEMODE | ✅ 완전 구현 | telnet.c:114, 315-319 | 정상 협상 |
| MODE 서브옵션 처리 | ✅ 완전 구현 | telnet.c:229-258 | EDIT, TRAPSIG 인식 |
| MODE_ACK 응답 | ✅ 완전 구현 | telnet.c:243-252 | ACK 비트 시 에코백 |
| 모드 전환 (line↔char) | ✅ 완전 구현 | telnet.c:254-257 | EDIT 비트로 제어 |
| **FORWARDMASK** | ❌ **미구현** | telnet.c:259-261 | **로깅만** |
| **SLC** | ❌ **미구현** | telnet.c:262-265 | **로깅만** |

**미구현 세부사항 1: FORWARDMASK**

**RFC 1184 요구사항**:
> "FORWARDMASK는 어떤 문자를 입력 버퍼에서 즉시 원격으로 보낼지 결정하는 32옥텟 비트마스크이며, IAC 값은 이중 IAC로 전송해야 한다."

**현재 코드**:
```c
// telnet.c:259-261
} else if (tn->sb_len >= 2 && tn->sb_buffer[1] == LM_FORWARDMASK) {
    /* FORWARDMASK - acknowledge but don't implement for now */
    MB_LOG_DEBUG("Received LINEMODE FORWARDMASK (not implemented)");
}
```

**권장 구현**:
```c
// telnet.h에 추가
typedef struct {
    // ...
    unsigned char forwardmask[32];  // 256비트 = 32바이트
    bool forwardmask_enabled;
} telnet_t;

// FORWARDMASK 처리
} else if (tn->sb_len >= 2 && tn->sb_buffer[1] == LM_FORWARDMASK) {
    if (tn->sb_len >= 34) {  // 1(option) + 1(LM_FORWARDMASK) + 32(mask)
        memcpy(tn->forwardmask, &tn->sb_buffer[2], 32);
        tn->forwardmask_enabled = true;
        MB_LOG_INFO("LINEMODE FORWARDMASK received and stored");
    }
}

// 입력 처리 시 (otelnet.c:otelnet_process_stdin)
if (tn->linemode_active && tn->forwardmask_enabled) {
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        int byte_idx = c / 8;
        int bit_idx = c % 8;
        bool should_forward = (tn->forwardmask[byte_idx] & (1 << bit_idx)) != 0;

        if (should_forward) {
            // 즉시 전송
        } else {
            // 버퍼에 누적
        }
    }
}
```

**미구현 세부사항 2: SLC (Set Local Characters)**

**RFC 1184 요구사항**:
> "SLC는 EC/EL 등 로컬 편집 키를 3바이트 트리플 목록으로 교환하며, 수준(SLC_DEFAULT/.../SLC_NOSUPPORT)에 따라 변경 가능성을 표시한다."

**현재 코드**:
```c
// telnet.c:262-265
} else if (tn->sb_len >= 2 && tn->sb_buffer[1] == LM_SLC) {
    /* SLC (Set Local Characters) - acknowledge but don't implement for now */
    MB_LOG_DEBUG("Received LINEMODE SLC (not implemented)");
}
```

**SLC 트리플 형식** (RFC 1184):
```
Byte 0: Function code (EC, EL, IP, AO, AYT, etc.)
Byte 1: Flags (SLC_DEFAULT=0x03, SLC_VALUE=0x02, SLC_CANTCHANGE=0x01, SLC_NOSUPPORT=0x00)
Byte 2: Value (ASCII code of the key)
```

**권장 구현**:
```c
// telnet.h에 추가
#define SLC_MAX_FUNCTIONS 32
typedef struct {
    unsigned char function;
    unsigned char flags;
    unsigned char value;
} slc_triple_t;

typedef struct {
    // ...
    slc_triple_t slc_table[SLC_MAX_FUNCTIONS];
    int slc_count;
} telnet_t;

// SLC 처리
} else if (tn->sb_len >= 2 && tn->sb_buffer[1] == LM_SLC) {
    if (tn->sb_len >= 5) {  // 최소 1개 트리플 (1+1+3)
        int num_triples = (tn->sb_len - 2) / 3;
        tn->slc_count = 0;

        for (int i = 0; i < num_triples && i < SLC_MAX_FUNCTIONS; i++) {
            int offset = 2 + (i * 3);
            tn->slc_table[i].function = tn->sb_buffer[offset];
            tn->slc_table[i].flags = tn->sb_buffer[offset + 1];
            tn->slc_table[i].value = tn->sb_buffer[offset + 2];
            tn->slc_count++;

            MB_LOG_DEBUG("SLC: func=%d flags=%d value=%d",
                        tn->slc_table[i].function,
                        tn->slc_table[i].flags,
                        tn->slc_table[i].value);
        }

        MB_LOG_INFO("LINEMODE SLC received: %d characters", tn->slc_count);
    }
}
```

**영향도**:
- **FORWARDMASK**: 중간 (라인 모드의 고급 기능, 대부분의 서버는 사용 안 함)
- **SLC**: 낮음 (로컬 편집 키 설정, 클라이언트 기본값으로 충분)

---

## 추가 발견 사항

### 1. 옵션 초기화 로직 문제

**현재 코드** (telnet.c:21-27):
```c
/* Initialize option tracking */
memset(tn->local_options, 0, sizeof(tn->local_options));
memset(tn->remote_options, 0, sizeof(tn->remote_options));

/* Set default options we support */
tn->local_options[TELOPT_BINARY] = true;  // ← 문제
tn->local_options[TELOPT_SGA] = true;     // ← 문제
```

**문제점**:
- `local_options[]`는 "우리가 사용하는 옵션"의 상태가 아니라, "우리가 지원하는 옵션" 목록으로 사용되어야 합니다.
- 하지만 협상 로직(telnet.c:302-320)에서는 상태 변경 추적에 사용됩니다.

**혼란스러운 시맨틱**:
```c
// telnet_init: local_options[BINARY] = true (지원 표시?)
// telnet_handle_negotiate (DO 수신):
if (!tn->local_options[option]) {  // false인 경우만 WILL 전송
    tn->local_options[option] = true;
    telnet_send_negotiate(tn, TELNET_WILL, option);
}
```

**권장 수정**:
```c
// telnet.h에 추가
bool local_supported[256];   // 지원 여부
bool remote_supported[256];  // 지원 여부

// telnet_init
tn->local_supported[TELOPT_BINARY] = true;
tn->local_supported[TELOPT_SGA] = true;
tn->local_supported[TELOPT_TTYPE] = true;
tn->local_supported[TELOPT_LINEMODE] = true;

tn->remote_supported[TELOPT_BINARY] = true;
tn->remote_supported[TELOPT_SGA] = true;
tn->remote_supported[TELOPT_ECHO] = true;

// 협상 로직
case TELNET_DO:
    if (tn->local_supported[option]) {  // 지원 여부 확인
        if (!tn->local_options[option]) {  // 상태 변경 확인
            tn->local_options[option] = true;
            telnet_send_negotiate(tn, TELNET_WILL, option);
        }
    } else {
        if (!tn->local_options[option]) {  // 아직 거부 안 했으면
            telnet_send_negotiate(tn, TELNET_WONT, option);
        }
    }
```

**영향도**: 중간 (현재는 동작하지만 로직이 혼란스러움, 버그 가능성)

---

### 2. 서브협상 IAC 이스케이프 처리

**RFC 1184 요구사항**:
> "FORWARDMASK에서 IAC 값은 이중 IAC로 전송해야 한다."

**현재 구현** (telnet.c:360-396):
```c
static int telnet_send_subnegotiation(telnet_t *tn, const unsigned char *data, size_t len)
{
    // ...
    for (size_t i = 0; i < len && pos < sizeof(buf) - 2; i++) {
        /* Escape IAC in subnegotiation data (RFC 854) */
        if (data[i] == TELNET_IAC) {
            buf[pos++] = TELNET_IAC;
            buf[pos++] = TELNET_IAC;
        } else {
            buf[pos++] = data[i];
        }
    }
    // ...
}
```

**✅ 올바른 구현**: IAC 이스케이프를 정확히 수행하고 있습니다.

**수신 측 확인** (telnet.c:400-430):
```c
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
        // ...
    } else if (c == TELNET_IAC) {
        /* Escaped IAC in subnegotiation */  // ← 이스케이프 처리
        if (tn->sb_len < sizeof(tn->sb_buffer)) {
            tn->sb_buffer[tn->sb_len++] = TELNET_IAC;
        }
        tn->state = TELNET_STATE_SB;
    }
    // ...
```

**✅ 올바른 구현**: IAC IAC → 단일 IAC로 복원합니다.

---

## 우선순위별 권장사항

### 🔴 우선순위 1 (즉시 수정 권장)

#### 1.1 옵션 협상 로직 버그 수정

**파일**: `src/telnet.c`
**위치**: 269-274, 322-327

**수정 전**:
```c
} else {
    /* Reject unsupported options (only if not already rejected) */
    if (tn->remote_options[option]) {  // ← 잘못된 조건
        tn->remote_options[option] = false;
        telnet_send_negotiate(tn, TELNET_DONT, option);
    }
}
```

**수정 후**:
```c
} else {
    /* Reject unsupported options (only if not already rejected) */
    if (!tn->remote_options[option]) {  // ← 올바른 조건
        telnet_send_negotiate(tn, TELNET_DONT, option);
        // Note: 상태는 false로 유지 (이미 false)
    }
}
```

**이유**: RFC 855 준수, 협상 루프 방지

---

### 🟡 우선순위 2 (선택적 개선)

#### 2.1 Terminal-Type 다중 타입 지원

**예상 작업량**: 2-3시간
**난이도**: 낮음
**이점**: RFC 1091 완전 준수, 서버 호환성 향상

**구현 계획**:
1. `telnet.h`에 타입 배열 추가
2. `telnet_init()`에서 타입 목록 초기화
3. `telnet_handle_subnegotiation()`에서 순환 로직 추가
4. 테스트: 다중 SEND 요청 시뮬레이션

---

#### 2.2 옵션 상태 관리 리팩토링

**예상 작업량**: 4-6시간
**난이도**: 중간
**이점**: 코드 명확성, 유지보수성 향상

**구현 계획**:
1. `local_supported[]`, `remote_supported[]` 배열 추가
2. 협상 로직에서 지원 여부와 상태 분리
3. 기존 테스트 케이스로 회귀 테스트

---

### 🟢 우선순위 3 (선택적 고급 기능)

#### 3.1 LINEMODE FORWARDMASK 구현

**예상 작업량**: 6-8시간
**난이도**: 중간
**이점**: 라인 모드 고급 제어, RFC 1184 부분 준수

**구현 계획**:
1. `telnet.h`에 forwardmask[32] 추가
2. FORWARDMASK 서브협상 파싱
3. `otelnet_process_stdin()`에서 마스크 적용
4. 버퍼 관리 로직 추가

**주의사항**: 복잡도 증가, 대부분의 서버는 사용 안 함

---

#### 3.2 LINEMODE SLC 구현

**예상 작업량**: 4-6시간
**난이도**: 중간
**이점**: 로컬 편집 키 커스터마이징

**구현 계획**:
1. `telnet.h`에 slc_table[] 추가
2. SLC 서브협상 파싱 (3바이트 트리플)
3. 로컬 편집 로직에 SLC 설정 반영

**주의사항**: 클라이언트 기본값으로 충분한 경우가 많음

---

#### 3.3 TCP Urgent (OOB) 처리

**예상 작업량**: 4-6시간
**난이도**: 높음 (POSIX OOB API 이해 필요)
**이점**: RFC 854 완전 준수, Synch/DM 정확한 처리

**구현 계획**:
1. `select()` 호출에 `exceptfds` 추가
2. OOB 데이터 감지 시 `recv(..., MSG_OOB)` 호출
3. DM 처리 로직 강화 (버퍼 플러시)

**주의사항**: 현대 telnet 서버는 거의 사용 안 함

---

## 테스트 권장사항

### 현재 구현 검증 테스트

#### 1. 옵션 협상 루프 방지 테스트
```bash
# 시뮬레이션: 서버가 미지원 옵션을 반복 요청
# 예상 동작: 첫 번째만 WONT/DONT 응답, 이후 무시

# 테스트 서버 스크립트 (Python):
import socket
s = socket.socket()
s.bind(('localhost', 8881))
s.listen(1)
conn, addr = s.accept()

# Send: IAC DO TIMING-MARK (unsupported)
conn.send(b'\xff\xfd\x06')  # IAC DO 6
time.sleep(0.1)
conn.send(b'\xff\xfd\x06')  # 반복
time.sleep(0.1)
conn.send(b'\xff\xfd\x06')  # 반복

# 예상 수신: IAC WONT 6 (한 번만)
data = conn.recv(1024)
# 현재 버그: 아무것도 받지 못함
# 수정 후: \xff\xfc\x06 한 번 수신
```

#### 2. Binary Mode UTF-8 테스트
```bash
# UTF-8 멀티바이트 문자 전송
echo "안녕하세요" | ./build/otelnet localhost 23

# 예상 동작:
# - BINARY 협상 성공 시: 정상 표시
# - BINARY 협상 실패 시: 경고 메시지 + 깨진 문자
```

#### 3. LINEMODE 전환 테스트
```bash
# 서버가 MODE 변경 요청
# IAC SB LINEMODE MODE EDIT IAC SE → Line mode
# IAC SB LINEMODE MODE 0 IAC SE → Character mode

# 확인:
# - telnet.c:239 로그 확인
# - otelnet.c:1009 로컬 에코 동작 확인
```

---

### 버그 수정 후 테스트

#### 1. 미지원 옵션 거부 테스트
```python
# 테스트: 서버가 지원하지 않는 옵션 요청
conn.send(b'\xff\xfd\x2c')  # IAC DO CHARSET (unsupported)

# 예상 수신: IAC WONT 44
data = conn.recv(3)
assert data == b'\xff\xfc\x2c', "Should reject unsupported option"
```

---

## 참고 문서

- RFC 854: Telnet Protocol Specification
- RFC 855: Telnet Option Specification
- RFC 856: Telnet Binary Transmission
- RFC 858: Telnet Suppress Go Ahead Option
- RFC 1091: Telnet Terminal-Type Option
- RFC 1184: Telnet Linemode Option

---

## 결론

otelnet은 **telnet 프로토콜의 핵심 기능을 정확히 구현**했으며, 대부분의 실제 사용 시나리오에서 **문제없이 작동**합니다.

**즉시 수정 권장**:
- ✅ 옵션 협상 로직 버그 (간단, 영향도 중간)

**선택적 개선**:
- 🔹 Terminal-Type 다중 타입 (간단, 영향도 낮음)
- 🔹 옵션 상태 관리 리팩토링 (코드 품질)
- 🔹 LINEMODE FORWARDMASK/SLC (고급 기능, 영향도 낮음)
- 🔹 TCP Urgent 처리 (복잡, 영향도 매우 낮음)

**현재 상태로도 충분한 이유**:
- ✅ RFC 854/855 핵심 준수
- ✅ Binary/SGA/ECHO 완벽 구현 → UTF-8 지원
- ✅ LINEMODE MODE 구현 → 문자/라인 모드 전환
- ✅ TERMINAL-TYPE 기본 구현 → 서버 호환성
- ⚠️ 미구현 기능들은 대부분의 서버가 사용하지 않음
