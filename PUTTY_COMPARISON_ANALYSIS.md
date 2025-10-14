# PuTTY vs otelnet Telnet 구현 비교 분석

**분석 일자**: 2025-10-15
**PuTTY 소스**: `/putty_source/otherbackends/telnet.c` (957 lines)
**otelnet 소스**: `src/telnet.c` (614 lines)

---

## 요약 (Executive Summary)

PuTTY는 **완전한 RFC 호환 telnet 클라이언트**로, 50개 이상의 telnet 옵션과 고급 기능을 지원합니다.
otelnet은 **핵심 기능에 집중한 경량 구현**으로, 실용적인 telnet 사용에 필요한 기본 옵션만 지원합니다.

**주요 차이점**:
- PuTTY: 10개 옵션 (NAWS, TSPEED, TTYPE, ENVIRON, ECHO, SGA, BINARY)
- otelnet: 4개 옵션 (TTYPE, LINEMODE, ECHO, SGA, BINARY)

**미구현 핵심 기능**:
1. ✅ **NAWS (윈도우 크기 협상)** - 중요도: 높음
2. ✅ **TSPEED (터미널 속도)** - 중요도: 중간
3. ✅ **ENVIRON (환경 변수)** - 중요도: 중간
4. ⚠️ **특수 명령어 전송** (AYT, BRK, SYNCH 등) - 중요도: 낮음
5. ⚠️ **TCP Urgent 데이터 처리** - 중요도: 낮음

---

## 상세 비교 분석

### 1. 지원 옵션 비교

| 옵션 | otelnet | PuTTY | RFC | 중요도 | 비고 |
|------|---------|-------|-----|--------|------|
| **BINARY** | ✅ | ✅ | 856 | 필수 | 양방향, UTF-8 지원 |
| **ECHO** | ✅ | ✅ | 857 | 필수 | 문자/라인 모드 전환 |
| **SGA** | ✅ | ✅ | 858 | 필수 | Suppress Go Ahead |
| **TTYPE** | ✅ (단일) | ✅ (완전) | 1091 | 높음 | otelnet: 단일 타입만 |
| **LINEMODE** | ✅ (부분) | ❌ | 1184 | 중간 | PuTTY는 미지원! |
| **NAWS** | ❌ | ✅ | 1073 | **높음** | **윈도우 크기 협상** |
| **TSPEED** | ❌ | ✅ | 1079 | 중간 | 터미널 속도 |
| **ENVIRON** | ❌ | ✅ | 1572 | 중간 | 환경 변수 (NEW/OLD) |

**주목할 점**:
- PuTTY는 LINEMODE를 지원하지 않고, 대신 ECHO/SGA 조합으로 처리
- otelnet은 LINEMODE를 부분 지원 (MODE만, FORWARDMASK/SLC 미지원)
- otelnet은 NAWS(윈도우 크기)를 **지원하지 않음** ← **가장 큰 차이점**

---

### 2. 상태 머신 비교

#### PuTTY 상태 머신 (telnet.c:192-195)
```c
enum {
    TOP_LEVEL,    // 일반 데이터
    SEENIAC,      // IAC 수신
    SEENWILL, SEENWONT, SEENDO, SEENDONT,  // 옵션 협상
    SEENSB,       // 서브협상 시작
    SUBNEGOT,     // 서브협상 데이터
    SUBNEG_IAC,   // 서브협상 내 IAC
    SEENCR        // CR 처리
} state;
```

#### otelnet 상태 머신 (telnet.h:107-116)
```c
typedef enum {
    TELNET_STATE_DATA,      // 일반 데이터
    TELNET_STATE_IAC,       // IAC 수신
    TELNET_STATE_WILL, TELNET_STATE_WONT,
    TELNET_STATE_DO, TELNET_STATE_DONT,
    TELNET_STATE_SB,        // 서브협상
    TELNET_STATE_SB_IAC     // 서브협상 내 IAC
} telnet_state_t;
```

**차이점**:
- PuTTY: `SEENCR` 상태로 CR/LF 처리 (바이너리 모드 고려)
- otelnet: CR 처리 상태 없음 ← **잠재적 문제**

---

### 3. 옵션 관리 구조

#### PuTTY의 고급 옵션 관리 (telnet.c:120-129)
```c
struct Opt {
    int send;         // 초기 전송 명령 (WILL/DO)
    int nsend;        // 부정 명령 (-ve send)
    int ack, nak;     // 긍정/부정 ACK
    int option;       // 옵션 코드
    int index;        // 상태 배열 인덱스
    enum {
        REQUESTED, ACTIVE, INACTIVE, REALLY_INACTIVE
    } initial_state;
};
```

**PuTTY 옵션 상태**:
- `REQUESTED`: 요청 전송됨, 응답 대기
- `ACTIVE`: 옵션 활성
- `INACTIVE`: 옵션 비활성
- `REALLY_INACTIVE`: 강제 비활성 (재협상 금지)

#### otelnet의 단순 관리 (telnet.h:134-143)
```c
bool local_options[256];   // 로컬 옵션 상태
bool remote_options[256];  // 원격 옵션 상태

// 개별 플래그
bool binary_local, binary_remote;
bool echo_local, echo_remote;
bool sga_local, sga_remote;
```

**차이점**:
- PuTTY: 4단계 상태 관리 (REQUESTED/ACTIVE/INACTIVE/REALLY_INACTIVE)
- otelnet: 2단계 관리 (true/false)
- PuTTY: 옵션별로 구조화된 관리
- otelnet: 배열 + 개별 플래그 혼용

---

### 4. 서브협상 (Subnegotiation) 비교

#### PuTTY 지원 서브협상 (telnet.c:210-347)

**TSPEED (터미널 속도)** - `telnet.c:216-235`
```c
case TELOPT_TSPEED:
    if (sb_buf[0] == TELQUAL_SEND) {
        // 설정 파일에서 termspeed 읽기
        char *termspeed = conf_get_str(conf, CONF_termspeed);
        // IAC SB TSPEED IS <speed> IAC SE 전송
    }
```

**TTYPE (터미널 타입)** - `telnet.c:236-260`
```c
case TELOPT_TTYPE:
    if (sb_buf[0] == TELQUAL_SEND) {
        // 대문자 변환 후 전송
        for (n = 0; termtype[n]; n++)
            put_byte(sb, (termtype[n] >= 'a' && termtype[n] <= 'z' ?
                          termtype[n] + 'A' - 'a' : termtype[n]));
    }
```

**ENVIRON (환경 변수)** - `telnet.c:261-347`
```c
case TELOPT_OLD_ENVIRON:
case TELOPT_NEW_ENVIRON:
    if (sb_buf[0] == TELQUAL_SEND) {
        // VAR/VALUE 형식으로 환경 변수 전송
        // USER 변수 자동 추가
        put_byte(sb, var);
        put_datalit(sb, "USER");
        put_byte(sb, value);
        put_datapl(sb, ptrlen_from_asciz(user));
    }
```

**NAWS (윈도우 크기)** - `telnet.c:744-772`
```c
static void telnet_size(Backend *be, int width, int height) {
    // 윈도우 크기 변경 시 자동 전송
    b[n++] = IAC; b[n++] = SB; b[n++] = TELOPT_NAWS;
    b[n++] = width >> 8;
    if (b[n-1] == IAC) b[n++] = IAC;   // IAC 이스케이프!
    b[n++] = width & 0xFF;
    if (b[n-1] == IAC) b[n++] = IAC;
    b[n++] = height >> 8;
    if (b[n-1] == IAC) b[n++] = IAC;
    b[n++] = height & 0xFF;
    if (b[n-1] == IAC) b[n++] = IAC;
    b[n++] = IAC; b[n++] = SE;
}
```

#### otelnet 지원 서브협상 (telnet.c:200-275)

**TTYPE (터미널 타입)** - `telnet.c:213-227`
```c
case TELOPT_TTYPE:
    if (sb_len >= 2 && sb_buffer[1] == TTYPE_SEND) {
        response[0] = TELOPT_TTYPE;
        response[1] = TTYPE_IS;
        memcpy(&response[2], terminal_type, term_len);
        telnet_send_subnegotiation(tn, response, 2 + term_len);
    }
```

**LINEMODE** - `telnet.c:229-265`
```c
case TELOPT_LINEMODE:
    if (sb_buffer[1] == LM_MODE) {
        // MODE만 처리
        linemode_edit = (mode & MODE_EDIT) != 0;
        // ACK 응답
    } else if (sb_buffer[1] == LM_FORWARDMASK) {
        // 미구현 (로깅만)
    } else if (sb_buffer[1] == LM_SLC) {
        // 미구현 (로깅만)
    }
```

**차이점 요약**:
| 서브협상 | PuTTY | otelnet | 비고 |
|----------|-------|---------|------|
| TTYPE | ✅ 대문자 변환 | ✅ 단순 전송 | PuTTY가 더 정확 |
| TSPEED | ✅ 완전 지원 | ❌ 미지원 | |
| ENVIRON | ✅ 완전 지원 | ❌ 미지원 | USER 변수 포함 |
| NAWS | ✅ 완전 지원 | ❌ **미지원** | **중요** |
| LINEMODE | ❌ 미지원 | ✅ 부분 지원 | otelnet만 지원 |

---

### 5. 특수 명령어 (Special Commands) 비교

#### PuTTY 특수 명령어 (telnet.c:777-857)
```c
static void telnet_special(Backend *be, SessionSpecialCode code, int arg) {
    switch (code) {
      case SS_AYT:    b[1] = AYT;   break;      // Are You There
      case SS_BRK:    b[1] = BREAK; break;      // Break
      case SS_EC:     b[1] = EC;    break;      // Erase Character
      case SS_EL:     b[1] = EL;    break;      // Erase Line
      case SS_GA:     b[1] = GA;    break;      // Go Ahead
      case SS_NOP:    b[1] = NOP;   break;      // No Operation
      case SS_ABORT:  b[1] = ABORT; break;      // Abort Process
      case SS_AO:     b[1] = AO;    break;      // Abort Output
      case SS_IP:     b[1] = IP;    break;      // Interrupt Process
      case SS_SUSP:   b[1] = SUSP;  break;      // Suspend Process
      case SS_EOR:    b[1] = EOR;   break;      // End Of Record
      case SS_EOF:    b[1] = xEOF;  break;      // End Of File
      case SS_SYNCH:                             // Synch (TCP Urgent)
        b[1] = DM;
        sk_write(s, b, 1);
        sk_write_oob(s, b + 1, 1);  // Out-of-Band 데이터!
        break;
    }
}
```

**PuTTY의 특수 명령어 메뉴** (telnet.c:859-879)
```c
static const SessionSpecial specials[] = {
    {"Are You There", SS_AYT},
    {"Break", SS_BRK},
    {"Synch", SS_SYNCH},
    {"Erase Character", SS_EC},
    {"Erase Line", SS_EL},
    {"Go Ahead", SS_GA},
    {"No Operation", SS_NOP},
    {NULL, SS_SEP},
    {"Abort Process", SS_ABORT},
    {"Abort Output", SS_AO},
    {"Interrupt Process", SS_IP},
    {"Suspend Process", SS_SUSP},
    {NULL, SS_SEP},
    {"End Of Record", SS_EOR},
    {"End Of File", SS_EOF},
    {NULL, SS_EXITMENU}
};
```

#### otelnet 특수 명령어
```c
// telnet_process_input()에서 수신만 처리
case TELNET_AYT:  // 수신 시 응답 전송
    const char *response = "\r\n[ModemBridge: Yes, I'm here]\r\n";
    telnet_send(tn, response, strlen(response));
    break;
case TELNET_IP/AO/BREAK/EC/EL:  // 로깅만
    MB_LOG_INFO("Received IAC <command>");
    break;
```

**차이점**:
- PuTTY: **능동적 전송** 가능 (UI 메뉴로 사용자가 선택)
- otelnet: **수동적 수신**만 처리
- PuTTY: TCP Urgent (`sk_write_oob`) 지원
- otelnet: Urgent 미지원

---

### 6. TCP Urgent (OOB) 데이터 처리

#### PuTTY의 Urgent 처리 (telnet.c:502-509)
```c
static void telnet_receive(
    Plug *plug, int urgent, const char *data, size_t len) {
    Telnet *telnet = container_of(plug, Telnet, plug);
    if (urgent)
        telnet->in_synch = true;  // Synch 모드 진입
    do_telnet_read(telnet, data, len);
}
```

**Synch 모드 동작** (telnet.c:364-378)
```c
if (!telnet->in_synch)
    put_byte(outbuf, c);  // 일반: 데이터 출력
else if (c == DM)
    telnet->in_synch = false;  // DM으로 Synch 해제
```

**Synch 전송** (telnet.c:843-847)
```c
case SS_SYNCH:
    b[1] = DM;
    sk_write(s, b, 1);          // IAC 전송
    sk_write_oob(s, b + 1, 1);  // DM을 Urgent로 전송
    break;
```

#### otelnet의 Urgent 처리
```c
// 전혀 없음
// select()에 exceptfds 없음
// recv(..., MSG_OOB) 없음
```

**영향**:
- Synch/DM 명령은 인식하지만 TCP Urgent와 연계되지 않음
- 실무에서는 거의 사용되지 않으므로 영향도 낮음

---

### 7. CR/LF 처리 비교

#### PuTTY의 CR 처리 (telnet.c:357-383)
```c
case TOP_LEVEL:
case SEENCR:
    if (c == NUL && state == SEENCR)
        state = TOP_LEVEL;  // CR NUL → CR로 변환
    else if (c == IAC)
        state = SEENIAC;
    else {
        if (c == CR && !binary_mode)
            state = SEENCR;  // CR 수신 시 SEENCR 상태
        else
            state = TOP_LEVEL;
    }
```

**송신 시** (telnet.c:702-730)
```c
#define iswritable(x) \
    ( (x) != IAC && \
      (binary_mode || (x) != CR))  // 바이너리 모드가 아니면 CR 특수 처리

while (p < end && !iswritable(*p)) {
    sk_write(s, *p == IAC ? iac : cr, 2);  // CR → CR NUL
    p++;
}
```

#### otelnet의 CR 처리
```c
// CR 특수 처리 없음
// 모든 데이터를 그대로 전송/수신
```

**차이점**:
- PuTTY: RFC 854 준수 (CR → CR NUL 변환, 바이너리 모드 제외)
- otelnet: CR을 그대로 처리
- **잠재적 문제**: 비바이너리 모드에서 CR 처리가 RFC 854를 완전히 따르지 않음

---

### 8. 옵션 협상 로직 비교

#### PuTTY의 정교한 협상 (telnet.c:158-208)
```c
static void proc_rec_opt(Telnet *telnet, int cmd, int option) {
    for (o = opts; *o; o++) {
        if ((*o)->option == option && (*o)->ack == cmd) {
            switch (opt_states[(*o)->index]) {
              case REQUESTED:
                opt_states[(*o)->index] = ACTIVE;
                activate_option(telnet, *o);
                break;
              case ACTIVE:
                break;  // 이미 활성, 무시
              case INACTIVE:
                opt_states[(*o)->index] = ACTIVE;
                send_opt(telnet, (*o)->send, option);  // 역제안
                activate_option(telnet, *o);
                break;
              case REALLY_INACTIVE:
                send_opt(telnet, (*o)->nsend, option);  // 거부
                break;
            }
            return;
        } else if ((*o)->option == option && (*o)->nak == cmd) {
            // NAK 처리 (WONT/DONT)
        }
    }
    // 미지원 옵션: 부정 응답
    if (cmd == WILL || cmd == DO)
        send_opt(telnet, (cmd == WILL ? DONT : WONT), option);
}
```

**PuTTY 옵션 활성화 부작용** (telnet.c:102-128, 130-156)
```c
static void option_side_effects(Telnet *telnet, const struct Opt *o, bool enabled) {
    if (o->option == TELOPT_ECHO && o->send == DO)
        telnet->echoing = !enabled;  // 원격 에코 → 로컬 에코 끔
    else if (o->option == TELOPT_SGA && o->send == DO)
        telnet->editing = !enabled;  // 원격 SGA → 로컬 편집 끔
    if (telnet->ldisc)
        ldisc_echoedit_update(telnet->ldisc);  // ldisc에 통보

    // 최소 옵션 보장 (activated 플래그로 초기화만 수행)
    if (!telnet->activated) {
        if (opt_states[o_echo.index] == INACTIVE) {
            opt_states[o_echo.index] = REQUESTED;
            send_opt(telnet, o_echo.send, o_echo.option);
        }
        // SGA도 동일
        telnet->activated = true;
    }
}

static void activate_option(Telnet *telnet, const struct Opt *o) {
    if (o->send == WILL && o->option == TELOPT_NAWS)
        backend_size(&telnet->backend, width, height);  // NAWS 활성 시 크기 전송

    if (o->send == WILL && (o->option == TELOPT_NEW_ENVIRON ||
                            o->option == TELOPT_OLD_ENVIRON)) {
        // NEW/OLD ENVIRON 중 하나만 활성
        deactivate_option(telnet, o->option == TELOPT_NEW_ENVIRON ?
                          &o_oenv : &o_nenv);
    }
    option_side_effects(telnet, o, true);
}

static void refused_option(Telnet *telnet, const struct Opt *o) {
    if (o->send == WILL && o->option == TELOPT_NEW_ENVIRON &&
        opt_states[o_oenv.index] == INACTIVE) {
        // NEW_ENVIRON 거부 시 OLD_ENVIRON 시도
        send_opt(telnet, WILL, TELOPT_OLD_ENVIRON);
        opt_states[o_oenv.index] = REQUESTED;
    }
    option_side_effects(telnet, o, false);
}
```

#### otelnet의 단순 협상 (telnet.c:241-355)
```c
int telnet_handle_negotiate(telnet_t *tn, unsigned char command, unsigned char option) {
    switch (command) {
        case TELNET_WILL:
            if (option == TELOPT_BINARY || option == TELOPT_SGA || option == TELOPT_ECHO) {
                if (!tn->remote_options[option]) {  // 상태 변경만 확인
                    tn->remote_options[option] = true;
                    telnet_send_negotiate(tn, TELNET_DO, option);
                    // 개별 플래그 설정
                    if (option == TELOPT_BINARY) tn->binary_remote = true;
                    // ...
                }
            } else {
                // 미지원: DONT 전송 (버그 수정됨)
                telnet_send_negotiate(tn, TELNET_DONT, option);
            }
            telnet_update_mode(tn);
            break;
        // DO/WONT/DONT도 유사
    }
}
```

**차이점**:
- PuTTY: 4단계 상태 + 옵션별 부작용 처리 + Fallback 메커니즘
- otelnet: 2단계 상태 + 단순 플래그 설정
- PuTTY: `activated` 플래그로 최소 옵션 보장
- otelnet: 초기화 시 일괄 전송

---

### 9. 설정 (Configuration) 비교

#### PuTTY 설정 항목 (telnet.c에서 참조)
```c
conf_get_str(conf, CONF_termspeed);  // 터미널 속도
conf_get_str(conf, CONF_termtype);   // 터미널 타입
conf_get_str_strs(conf, CONF_environmt, ...);  // 환경 변수 목록
conf_get_bool(conf, CONF_rfc_environ);  // RFC ENVIRON 사용 여부
conf_get_bool(conf, CONF_passive_telnet);  // 수동 모드
conf_get_int(conf, CONF_width);      // 윈도우 너비
conf_get_int(conf, CONF_height);     // 윈도우 높이
```

#### otelnet 설정 항목 (otelnet.conf)
```conf
KERMIT=kermit
SEND_ZMODEM=sz
RECEIVE_ZMODEM=rz
LOG=1
LOG_FILE=otelnet.log
```

**차이점**:
- PuTTY: telnet 프로토콜 세부 설정 다수
- otelnet: 파일 전송 프로그램 경로만 설정
- otelnet에 없는 설정:
  - Terminal type (하드코딩: "ANSI")
  - Terminal speed
  - Window size
  - Environment variables

---

## 미구현 기능 우선순위

### 🔴 우선순위 1 (중요도: 높음)

#### 1. NAWS (Negotiate About Window Size) - RFC 1073

**중요도**: ⭐⭐⭐⭐⭐ (매우 높음)

**이유**:
- 터미널 윈도우 크기를 서버에 알려주는 표준 방법
- 많은 서버 애플리케이션(vim, emacs, less 등)이 의존
- 윈도우 크기 변경 시 실시간 업데이트 필요

**현재 상태**:
- ❌ 완전 미구현
- otelnet은 윈도우 크기를 서버에 전달하지 않음
- 결과: 서버는 기본 크기(80x24)로 가정

**구현 방법** (PuTTY 참고):
```c
// telnet.h에 추가
#define TELOPT_NAWS 31

typedef struct {
    // ...
    int term_width;
    int term_height;
} telnet_t;

// 초기화 시
void telnet_init(telnet_t *tn) {
    // ...
    tn->term_width = 80;   // 기본값
    tn->term_height = 24;

    // NAWS 옵션 협상 제안
    telnet_send_negotiate(tn, TELNET_WILL, TELOPT_NAWS);
}

// 옵션 협상 시
case TELNET_DO:
    if (option == TELOPT_NAWS) {
        if (!tn->local_options[option]) {
            tn->local_options[option] = true;
            telnet_send_negotiate(tn, TELNET_WILL, option);
            // 즉시 윈도우 크기 전송
            telnet_send_naws(tn);
        }
    }

// 윈도우 크기 전송 함수
int telnet_send_naws(telnet_t *tn) {
    unsigned char buf[20];
    int n = 0;

    buf[n++] = TELNET_IAC;
    buf[n++] = TELNET_SB;
    buf[n++] = TELOPT_NAWS;

    // Width (2 bytes, network byte order)
    buf[n++] = (tn->term_width >> 8) & 0xFF;
    if (buf[n-1] == TELNET_IAC) buf[n++] = TELNET_IAC;  // IAC 이스케이프!
    buf[n++] = tn->term_width & 0xFF;
    if (buf[n-1] == TELNET_IAC) buf[n++] = TELNET_IAC;

    // Height (2 bytes, network byte order)
    buf[n++] = (tn->term_height >> 8) & 0xFF;
    if (buf[n-1] == TELNET_IAC) buf[n++] = TELNET_IAC;
    buf[n++] = tn->term_height & 0xFF;
    if (buf[n-1] == TELNET_IAC) buf[n++] = TELNET_IAC;

    buf[n++] = TELNET_IAC;
    buf[n++] = TELNET_SE;

    return telnet_send(tn, buf, n);
}

// 윈도우 크기 변경 API (외부에서 호출)
void telnet_resize(telnet_t *tn, int width, int height) {
    tn->term_width = width;
    tn->term_height = height;

    if (tn->is_connected && tn->local_options[TELOPT_NAWS]) {
        telnet_send_naws(tn);
    }
}
```

**통합 포인트**:
- `otelnet.c`에서 SIGWINCH 시그널 처리
- termios로 현재 윈도우 크기 조회: `ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)`

**예상 작업량**: 4-6시간

---

### 🟡 우선순위 2 (중요도: 중간)

#### 2. TSPEED (Terminal Speed) - RFC 1079

**중요도**: ⭐⭐⭐ (중간)

**이유**:
- 일부 레거시 서버가 요구
- 출력 속도 조절에 사용 (느린 터미널 대응)
- 현대 환경에서는 거의 무의미 (모두 고속)

**구현 방법**:
```c
// telnet.h
#define TELOPT_TSPEED 32

typedef struct {
    // ...
    char terminal_speed[32];  // 예: "38400,38400"
} telnet_t;

// 서브협상 처리
case TELOPT_TSPEED:
    if (sb_len >= 2 && sb_buffer[1] == TELQUAL_SEND) {
        unsigned char response[64];
        size_t speed_len = strlen(tn->terminal_speed);

        response[0] = TELOPT_TSPEED;
        response[1] = TELQUAL_IS;
        memcpy(&response[2], tn->terminal_speed, speed_len);

        telnet_send_subnegotiation(tn, response, 2 + speed_len);
    }
```

**예상 작업량**: 2-3시간

---

#### 3. ENVIRON (Environment Variables) - RFC 1572

**중요도**: ⭐⭐⭐ (중간)

**이유**:
- 일부 서버가 USER 환경 변수를 요구
- 쉘 환경 설정에 사용
- 보안 고려 필요 (어떤 변수를 전송할지 제한)

**구현 방법**:
```c
// telnet.h
#define TELOPT_NEW_ENVIRON 39
#define TELOPT_OLD_ENVIRON 36

#define ENV_VAR 0
#define ENV_VALUE 1
#define ENV_ESC 2
#define ENV_USERVAR 3

typedef struct {
    // ...
    char env_user[256];
    // 필요 시 env_vars 배열 추가
} telnet_t;

// 서브협상 처리
case TELOPT_NEW_ENVIRON:
case TELOPT_OLD_ENVIRON:
    if (sb_len >= 2 && sb_buffer[1] == TELQUAL_SEND) {
        // USER 변수 전송
        unsigned char response[300];
        int pos = 0;

        response[pos++] = sb_opt;
        response[pos++] = TELQUAL_IS;
        response[pos++] = ENV_VAR;
        memcpy(&response[pos], "USER", 4); pos += 4;
        response[pos++] = ENV_VALUE;
        size_t user_len = strlen(tn->env_user);
        memcpy(&response[pos], tn->env_user, user_len); pos += user_len;

        telnet_send_subnegotiation(tn, response, pos);
    }
```

**예상 작업량**: 3-4시간

---

### 🟢 우선순위 3 (중요도: 낮음)

#### 4. 특수 명령어 전송 API

**중요도**: ⭐⭐ (낮음)

**이유**:
- 디버깅/테스팅에 유용
- 일부 고급 사용자가 요구
- otelnet은 콘솔 모드가 있으므로 낮은 우선순위

**구현 방법**:
```c
// otelnet.h에 추가
void otelnet_send_special(otelnet_ctx_t *ctx, const char *command);

// otelnet.c 구현
void otelnet_send_special(otelnet_ctx_t *ctx, const char *command) {
    unsigned char buf[2] = {TELNET_IAC, 0};

    if (strcmp(command, "ayt") == 0) buf[1] = TELNET_AYT;
    else if (strcmp(command, "brk") == 0) buf[1] = TELNET_BREAK;
    else if (strcmp(command, "ip") == 0) buf[1] = TELNET_IP;
    // ...
    else return;  // 알 수 없는 명령

    telnet_send(&ctx->telnet, buf, 2);
}

// 콘솔 모드에서 사용
if (strcmp(program, "special") == 0) {
    if (arg_count > 0) {
        otelnet_send_special(ctx, args[0]);
    } else {
        printf("Usage: special <ayt|brk|ip|ao|...>\r\n");
    }
    return SUCCESS;
}
```

**예상 작업량**: 2-3시간

---

#### 5. TCP Urgent (OOB) 데이터 처리

**중요도**: ⭐ (매우 낮음)

**이유**:
- 현대 telnet 서버는 거의 사용 안 함
- Winsock 문제로 PuTTY도 완벽하지 않음 (telnet.c:368-378 주석 참조)
- 구현 복잡도 높음

**구현 방법**:
```c
// select()에 exceptfds 추가
fd_set readfds, exceptfds;
FD_ZERO(&exceptfds);
FD_SET(telnet_fd, &exceptfds);
select(maxfd + 1, &readfds, NULL, &exceptfds, &timeout);

if (FD_ISSET(telnet_fd, &exceptfds)) {
    // OOB 데이터 수신
    char oob_buf[1];
    recv(telnet_fd, oob_buf, 1, MSG_OOB);
    if (oob_buf[0] == TELNET_DM) {
        tn->in_synch = false;
    }
}
```

**예상 작업량**: 4-6시간

---

#### 6. CR/LF 정확한 처리 (RFC 854 준수)

**중요도**: ⭐⭐ (낮음)

**이유**:
- 바이너리 모드에서는 문제없음
- 비바이너리 모드는 레거시
- 대부분의 현대 서버는 바이너리 모드 사용

**구현 방법**:
```c
// 상태 머신에 SEENCR 추가
typedef enum {
    // ...
    TELNET_STATE_SEENCR
} telnet_state_t;

// 수신 처리
case TELNET_STATE_DATA:
    if (c == '\r' && !tn->binary_remote) {
        tn->state = TELNET_STATE_SEENCR;
    } else {
        output[out_pos++] = c;
    }
    break;

case TELNET_STATE_SEENCR:
    if (c == '\0') {
        // CR NUL → CR
        output[out_pos++] = '\r';
    } else if (c == '\n') {
        // CR LF → CR LF (그대로)
        output[out_pos++] = '\r';
        output[out_pos++] = '\n';
    } else {
        // CR <other> → CR <other>
        output[out_pos++] = '\r';
        output[out_pos++] = c;
    }
    tn->state = TELNET_STATE_DATA;
    break;

// 송신 처리
for (i = 0; i < input_len; i++) {
    if (input[i] == '\r' && !tn->binary_local) {
        // CR → CR NUL
        output[out_pos++] = '\r';
        output[out_pos++] = '\0';
    } else if (input[i] == TELNET_IAC) {
        // IAC → IAC IAC
        output[out_pos++] = TELNET_IAC;
        output[out_pos++] = TELNET_IAC;
    } else {
        output[out_pos++] = input[i];
    }
}
```

**예상 작업량**: 3-4시간

---

## otelnet의 고유 기능

PuTTY에 **없고** otelnet에만 **있는** 기능:

### 1. ✅ 콘솔 모드 (Ctrl+])
- 연결을 유지하면서 로컬 명령 실행
- 파일 탐색 (ls, pwd, cd)
- 파일 전송 (sz, rz, kermit)

### 2. ✅ 파일 전송 통합
- XMODEM/YMODEM/ZMODEM 자동 실행
- Kermit 프로토콜 지원
- 외부 프로그램 통합 (sz/rz)

### 3. ✅ LINEMODE 부분 지원
- PuTTY는 LINEMODE를 전혀 지원하지 않음
- otelnet은 MODE 서브옵션 처리

### 4. ✅ 세션 로깅
- Hex+ASCII 덤프 형식
- 송수신 데이터 분리 기록
- 타임스탬프

---

## 권장 사항

### 즉시 구현 권장 (우선순위 1)

1. **NAWS (윈도우 크기 협상)** ⭐⭐⭐⭐⭐
   - 작업량: 4-6시간
   - 영향: 매우 높음 (vim, emacs 등 화면 기반 애플리케이션)
   - PuTTY 코드 참고: `telnet.c:744-772`

### 선택적 구현 (우선순위 2)

2. **TSPEED (터미널 속도)** ⭐⭐⭐
   - 작업량: 2-3시간
   - 영향: 중간 (레거시 시스템 호환성)

3. **ENVIRON (환경 변수)** ⭐⭐⭐
   - 작업량: 3-4시간
   - 영향: 중간 (일부 서버 요구)

### 낮은 우선순위 (우선순위 3)

4. **특수 명령어 전송** ⭐⭐
   - 작업량: 2-3시간
   - 영향: 낮음 (디버깅 용도)

5. **CR/LF 정확한 처리** ⭐⭐
   - 작업량: 3-4시간
   - 영향: 낮음 (바이너리 모드로 회피 가능)

6. **TCP Urgent 처리** ⭐
   - 작업량: 4-6시간
   - 영향: 매우 낮음 (거의 사용 안 함)

---

## 결론

**otelnet의 현재 위치**:
- ✅ 핵심 telnet 기능 완벽 구현
- ✅ 파일 전송 통합 (PuTTY에 없는 기능)
- ⚠️ 고급 옵션 일부 미구현 (NAWS가 가장 큼)
- ✅ RFC 854/855 기본 준수

**권장 로드맵**:
1. **Phase 1** (4-6시간): NAWS 구현 → 화면 기반 앱 완벽 지원
2. **Phase 2** (5-7시간): TSPEED + ENVIRON → 레거시 호환성 향상
3. **Phase 3** (선택): 특수 명령어, CR/LF 정확한 처리

**현재 상태로도 충분한 경우**:
- 대부분의 telnet 사용 (SSH가 더 일반적)
- 파일 전송이 주 목적
- 간단한 BBS 접속
- 임베디드 디바이스 관리

**NAWS 구현이 필수인 경우**:
- vim/emacs/nano 등 화면 편집기 사용
- less/more 등 페이저 사용
- 화면 기반 TUI 애플리케이션
- 윈도우 크기 변경 빈번한 사용자
