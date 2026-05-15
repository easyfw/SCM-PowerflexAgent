# SCM-PowerFlex Agent Service (MVP v0.2 — 5-axis)

**Target**: Morbidelli PWX100 + TPA Albatros 3.1.9 SP 9c
**Build**: Borland C++ Builder 6 (Windows Service)
**Phase**: MVP (Tier 1 축상태 5축 + Tier 4 가동통계)
**ID Range**: 1~19 (19개 데이터 포인트, 일부 ID 비연속)

---

## 1. 무엇을 하는가

SCM 그룹의 Morbidelli PWX100 머신 PC에 설치되는 Windows 서비스입니다. Albatros HMI 가 디스크에 남기는 **진단 로그 파일을 읽어** 머신 상태를 추출하고, **시리얼 포트로 ESP32 게이트웨이에 전송**합니다.

```
Albatros (TPA HMI)
   │
   │  파일 출력 (Read-Only 접근)
   ▼
┌─────────────────────────────┐
│  C:\Albatros\Report\ErrAsseXGVS\YYYYMMDD_ErrAsseXGVS.txt
│  C:\Albatros\Tmp\MONTH##.TER
└─────────────────────────────┘
   │
   │  ① File polling (5초 / 60초)
   ▼
┌─────────────────────────────┐
│  SCM_PowerflexAgent.exe     │  ← 본 서비스
│  (BCB6 Windows Service)     │
└─────────────────────────────┘
   │
   │  ② TVaComm 시리얼 송신 (115200 bps)
   │     패킷: [STX][LEN][CNT][ID][Q][VAL]...[CHK][ETX]
   ▼
┌─────────────────────────────┐
│  ESP32 게이트웨이            │
└─────────────────────────────┘
   │
   │  ③ Wi-Fi/MQTT (별도)
   ▼
   클라우드 / SCADA
```

머신을 **읽기 전용**으로만 관찰합니다. PLC 에 쓰기 명령을 보내거나 HMI 동작을 변경하지 않습니다.

---

## 2. 어떻게 동작하는가 — 왜 이렇게 설계했는가

### 2.1 파일 폴링을 선택한 이유

PowerFlex PC 에서 다음을 모두 확인한 결과 (사용자 제공 스크린샷):

| 후보 인터페이스 | 가능성 | 사유 |
|---|---|---|
| SQL DB | ❌ | SQL Server 서비스 없음 (AH221 의 `SCMGROUPEXPRESS` 없음) |
| OPC DA/UA | ❌ | `OpcEnum` 등 OPC 서비스 없음 |
| HTTP REST | ❌ | IIS 는 기본 환영 페이지만 호스팅 |
| Modbus 직접 | ⚠ | `ModbusDrv.exe` 가 Albatros 전용으로 사용 중 — 충돌 위험 |
| **파일 폴링** | ✅ | Albatros 가 매 사이클 `Report\`, `Tmp\` 에 텍스트 로그 출력 |

### 2.2 골격은 AH221, 데이터 수집은 GA3 패턴

| 컴포넌트 | 출처 | 변경 사항 |
|---|---|---|
| TService 베이스 | AH221 | 클래스명만 `TSCM_PowerflexAgent` 로 변경 |
| TVaComm 시리얼 | AH221/GA3 | 동일 |
| 패킷 프로토콜 | AH221/GA3 | 동일 (STRING 패킷은 MVP 미사용) |
| 변화 감지/하트비트 | AH221/GA3 | 동일 |
| 로그 60KB 로테이션 | AH221/GA3 | 동일 |
| Time-tiered 폴링 | AH221 | 동일 구조, 그룹 수만 2개로 축소 |
| 데이터 수집 | GA3 (WinCutPlus) | tail-read 패턴 차용, 파일명·포맷만 PowerFlex 용 |
| SQL/ADO | AH221 → **제거** | DB 없음 |

### 2.3 등록된 19개 데이터 아이템 (5축 확장 후)

> ⚠ **ID 비연속**: Tier 4 ID(10~13)와 충돌 회피를 위해 GVS4/5는 ID 14~19로 매핑. ESP32 펌웨어는 ID 1~9 + 10~13 + 14~19 처리 필요.

| ID | 이름 | 소스 | 단위 | 갱신주기 | 비고 |
|---|---|---|---|---|---|
| 1 | GVS1_QuotaReale | ErrAsseXGVS | mm × 1000 | 5초 | EtherCAT CN(3) = 34UX21 |
| 2 | GVS1_StatoAsse | ErrAsseXGVS | int | 5초 | |
| 3 | GVS1_ErroreAnello | ErrAsseXGVS | mm × 1000 | 5초 | |
| 4 | GVS2_QuotaReale | ErrAsseXGVS | mm × 1000 | 5초 | EtherCAT CN(7) = 34UX22 |
| 5 | GVS2_StatoAsse | ErrAsseXGVS | int | 5초 | |
| 6 | GVS2_ErroreAnello | ErrAsseXGVS | mm × 1000 | 5초 | |
| 7 | GVS3_QuotaReale | ErrAsseXGVS | mm × 1000 | 5초 | EtherCAT CN(11) = 34UX23 |
| 8 | GVS3_StatoAsse | ErrAsseXGVS | int | 5초 | |
| 9 | GVS3_ErroreAnello | ErrAsseXGVS | mm × 1000 | 5초 | |
| 10 | MachineState | MONTH.TER | 1=RUN/2=STOP/3=ALARM | 60초 | |
| 11 | DailyRunTimeMin | MONTH.TER | 분 | 60초 | |
| 12 | DailyStartCount | MONTH.TER | 회 | 60초 | |
| 13 | DailyAlarmCount | MONTH.TER | 회 | 60초 | |
| **14** | **GVS4_QuotaReale** | ErrAsseXGVS | mm × 1000 | 5초 | ⭐ **신규 (v0.2)** drive 34UX24 |
| **15** | **GVS4_StatoAsse** | ErrAsseXGVS | int | 5초 | ⭐ 신규 |
| **16** | **GVS4_ErroreAnello** | ErrAsseXGVS | mm × 1000 | 5초 | ⭐ 신규 |
| **17** | **GVS5_QuotaReale** | ErrAsseXGVS | mm × 1000 | 5초 | ⭐ **신규** drive 34UX25 |
| **18** | **GVS5_StatoAsse** | ErrAsseXGVS | int | 5초 | ⭐ 신규 |
| **19** | **GVS5_ErroreAnello** | ErrAsseXGVS | mm × 1000 | 5초 | ⭐ 신규 |

### 2.4 5축 확장 근거 (v0.2)

`Albatros\Mod.0\PLC\Com_Interface_WSCM.FUN` 의 컴파일 상수 발견:

```gpl
Const PRESENZA_GVS_1 = 1
Const PRESENZA_GVS_2 = 1
Const PRESENZA_GVS_3 = 1
Const PRESENZA_GVS_4 = 1    ; ⭐ 활성
Const PRESENZA_GVS_5 = 1    ; ⭐ 활성
```

추가로 `Mod.0\CONFIG\GVS4_X.csv`, `GVS5_X.csv` 파일도 존재 (모두 -550~3600mm 행정).

상세 분석은 `../reference/wscm_analysis.md`, `../reference/machine_topology.md` 참조.

---

## 3. 빌드 절차 (BCB6)

1. **Borland C++ Builder 6** 실행 (개발 PC).
2. `File > Open Project...` → `SCM_PowerflexAgent.bpr` 열기.
3. `Project > Build SCM_PowerflexAgent`.
4. 빌드 성공 시 동일 폴더에 `SCM_PowerflexAgent.exe` 생성.

### 의존 라이브러리

- **VaComm** (`vacommb6.lib`, `vacommb6.bpi`) — AH221/GA3 와 동일하게 시리얼 통신에 사용.
- 그 외 모두 BCB6 표준 VCL/RTL.
- **ADO 라이브러리 제거됨** (`adortl.lib`, `dbrtl.lib`) — 파일 폴링이므로 불필요.

빌드 머신에 VaComm 컴포넌트가 설치돼 있어야 합니다 (AH221/GA3 빌드에 사용한 동일 IDE).

---

## 4. 설치 절차 (PowerFlex PC)

### 4.1 사전 작업

1. **빈 COM 포트 확인**
   ```cmd
   mode
   ```
   현재 사용 중이지 않은 COM 번호 확인 (PowerFlex PC 는 TeamViewer/Kaspersky 가 일부 점유 가능).

2. **Kaspersky 화이트리스트 등록**
   - 설치 폴더와 `SCM_PowerflexAgent.exe` 를 Kaspersky 신뢰 영역에 추가.
   - 등록하지 않으면 서비스가 자동 시작 시 차단될 수 있음.

3. **Albatros 경로 확인**
   - 사용자 PC: `D:\...\04_SCM-PowerFlex\02_Installed_Program\Albatros\` 형태
   - 실제 운영 PC 의 경로를 확인하여 INI 의 `BasePath` 에 정확히 기입.

### 4.2 파일 배치

다음 파일들을 `C:\SCM-PowerFlexAgent\` 폴더에 복사:

```
C:\SCM-PowerFlexAgent\
├── SCM_PowerflexAgent.exe
├── oem_setting.ini      ← 환경에 맞게 수정 (BasePath, COM_Port)
├── oem_param.csv        ← 참조용 (런타임에는 코드 내 정의 사용)
└── logsave\             ← 자동 생성됨
```

### 4.3 INI 편집

`oem_setting.ini` 의 다음 항목을 운영 환경에 맞게 수정:

```ini
[Albatros]
BasePath=C:\Albatros\         ; 실 경로로 변경

[Communication]
COM_Port=COM4                  ; mode 명령으로 확인한 빈 포트
BaudRate=115200
```

### 4.4 서비스 등록

관리자 권한 명령 프롬프트에서:

```cmd
cd C:\SCM-PowerFlexAgent
SCM_PowerflexAgent.exe /install
net start SCM_PowerflexAgent
```

> BCB6 의 `TService` 는 `/install`, `/uninstall` 인자에 응답합니다 (AH221/GA3 도 동일).

서비스 등록 확인:

```cmd
sc query SCM_PowerflexAgent
```

자동 시작 설정:

```cmd
sc config SCM_PowerflexAgent start= auto
```

### 4.5 동작 확인

1. `services.msc` 에서 **SCM-PowerFlex Agent Service** 가 "실행 중" 상태인지 확인.
2. `C:\SCM-PowerFlexAgent\logsave.txt` 에 다음과 같은 로그가 쌓이는지 확인:
   ```
   [HH:MM:SS] SVC START - SCM-PowerFlex (Morbidelli PWX100)
   [HH:MM:SS] CFG: Albatros=C:\Albatros\ COM4 115200 T:5000
   [HH:MM:SS]   Poll AxisStatus: 5s cyc=1 ON
   [HH:MM:SS]   Poll MonthlyReport: 60s cyc=12 ON
   [HH:MM:SS] Items: 19
   [HH:MM:SS] Serial port COM4 opened at 115200 bps
   [HH:MM:SS] AXIS: new file 20260515_ErrAsseXGVS.txt
   [HH:MM:SS] INIT AXIS OK
   [HH:MM:SS] INIT MONTH OK
   [HH:MM:SS] Timer1 enabled, Interval=5000
   [HH:MM:SS] SVC READY
   ```
3. 무변화 시 `logsave_status.txt` 에 하트비트가 갱신됨:
   ```
   [YYYY-MM-DD HH:MM:SS] NoChange:42 D(HB):13 TX:106 OK
   ```

---

## 5. 검증되지 않은 항목 (실 PC 테스트 전 주의)

사용자 선호에 따라 **검증된 부분과 검증되지 않은 부분**을 명확히 구분합니다.

### 5.1 검증된 부분 (AH221/GA3 운영에서 입증)

- `TService` + `TVaComm` 베이스 골격
- 패킷 포맷 (ESP32 펌웨어와 호환성 검증 완료)
- 변화감지/하트비트/재시도/로그로테이션 로직
- Time-tiered 폴링 (AH221 4그룹 시스템)
- 60KB 로그 로테이션

### 5.2 검증 필요 (실 PC 테스트 시 확인)

| 항목 | 가정 | 확인 방법 |
|---|---|---|
| **Albatros BasePath** | `C:\Albatros\` 가정 | 실 PC 의 정확한 설치 경로 확인 |
| **ErrAsseXGVS 파일명** | `YYYYMMDD_ErrAsseXGVS.txt` 가정 | 실파일명 확인, 다르면 `GetTodayAxisFilePath()` 수정 |
| **파일 추가쓰기 모드** | Albatros 가 append-only 로 가정 | 첫 5분간 파일 크기가 증가만 하는지 관찰 |
| **MONTH.TER 날짜 형식** | `DD/MM/YYYY` (이탈리아식) 가정 | 실 파일 라인을 `GetTodayDateString()` 결과와 매칭해 보기 |
| **StatoAsse 코드 의미** | `4 = In Position` 가정 | 머신 정지/이동 시 값 변화 관찰 |
| **MachineState 매핑** | `starts execution` → 1, `stops execution` → 2 가정 | 첫 1시간 가동 후 ID 10 값과 실제 상태 비교 |
| **알람 패턴** | "Not found", "not running", "System", "Error" 키워드 매칭 | 알람 발생 시 ID 13 카운트 증가 여부 |

### 5.3 의도적으로 미구현 (다음 단계)

- **Tier 2** ErrAxEthercat 드라이브 폴트 로그 (NODO, ERR, V_BUS 등 11개 필드)
- **Tier 3** ERRCAN.TXT / ERRECAT.TXT 통신 알람
- **STRING 패킷** (알람 메시지 텍스트, 머신 모델명 등)
- **MSMQ 청취** 옵션 (실시간 이벤트 - 큐 이름 확인 필요)
- **NESTCAD 주문 파싱** (Product\NESTCAD\ 내 네스팅 작업)

이들은 MVP 안정화 후 phase 2 에서 추가될 예정입니다.

---

## 6. 트러블슈팅

| 증상 | 원인 후보 | 조치 |
|---|---|---|
| 서비스 시작 즉시 멈춤 | Mycomm NULL / VaComm 미설치 | 빌드 PC 의 VaComm 컴포넌트 확인 |
| `SVC START` 로그 뒤 더 진행 안 됨 | `BasePath` 오류 또는 파일 없음 | `oem_setting.ini` 의 경로 확인, 폴더 실제 존재 여부 |
| `AXIS: file not found` 반복 | 오늘 날짜 파일이 아직 안 만들어짐 | 머신을 가동시켜 첫 사이클 발생 후 재확인 |
| 모든 ID Quality=0x00 (Bad) | 파일은 있으나 파싱 실패 | `HK_DEBUG=1` 로 빌드해 라인별 디버그 로그 확인 |
| `MachineState` 가 항상 STOP(2) | 날짜 형식 불일치 | `GetTodayDateString()` 를 `MM/DD/YYYY` 로 변경 후 재빌드 |
| `MONTH: file not found` | 신규 월 첫날 / 경로 오류 | `Albatros\Tmp\MONTH##.TER` 실존 확인 |
| `E:COM not ready` | 시리얼 포트 사용 중 / 미존재 | INI 의 `COM_Port` 변경 |
| Kaspersky 가 EXE 격리 | 백신 차단 | 신뢰 영역에 EXE 등록 |

---

## 7. 다음 단계 (Phase 2 예고)

1. **Tier 2 추가** — `ErrAxEthercat` 드라이브 폴트 로그
   - 새 ID 15~17: LastDriveErrNode, LastDriveErrCode, LastDriveErrClass
2. **STRING 패킷 복원** — 알람 메시지 텍스트 송신
   - 새 ID 20 (0xFE 타입): LastAlarmMsg
3. **MSMQ 청취 옵션** — Albatros 가 MSMQ 를 사용한다면 실시간 이벤트 수신 가능
4. **MONTH.TER 컬럼 정밀 파싱** — Module, GroupCode 컬럼까지 분리해 알람 분류 강화

각 단계는 별도 PR/빌드로 분리해 회귀 위험을 최소화합니다.

---

## 8. 파일 목록 (이 디렉토리)

```
SCM-PowerflexAgent/
├── SvcController.h          ← 서비스 클래스 헤더
├── SvcController.cpp        ← 서비스 구현 (파일 폴링 + 패킷 전송)
├── SvcController.dfm        ← BCB6 폼 정의 (TVaComm + TTimer)
├── SCM_PowerflexAgent.cpp   ← 서비스 main 진입점
├── SCM_PowerflexAgent.bpr   ← BCB6 프로젝트 파일
├── oem_setting.ini          ← 운영 설정 (BasePath, COM, 폴링 인터벌)
├── oem_param.csv            ← ID 매핑 (참조용)
└── README.md                ← 이 파일
```

---

*문서 버전 0.1.0 — MVP 초기 릴리스*
