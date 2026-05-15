//---------------------------------------------------------------------------
// SvcController.h - SCM-PowerFlex (Morbidelli PWX100) Agent Service
//
// Target Machine : Morbidelli PWX100 (SCM Group)
// HMI Stack      : TPA Albatros 3.1.9 Service Pack 9c
// Build Tool     : Borland C++ Builder 6 (BCB6) - Windows Service
// Data Source    : Albatros\Report\ErrAsseXGVS\*.txt (axis status, 5s)
//                  Albatros\Tmp\MONTH##.TER         (event log, 60s)
//
// [Inherited from SCM-AH221Agent (verified patterns)]
//   - TService base class structure
//   - TVaComm serial + packet format [STX][LEN][CNT][ID][Q][VAL]...[CHK][ETX]
//   - Change detection, Heartbeat, ACK/NAK, retry, log rotation (60KB)
//   - InitPollGroups/ShouldPollGroup (time-tiered polling)
//
// [Replaced from AH221]
//   - SQL/ADO (TADOConnection, TADOQuery) -> File polling
//   - STRING packets (0xFE) -> Disabled in MVP (re-enable in next phase)
//
// [MVP Scope]
//   Tier 1 (5s):  9 items - 3 axes × {QuotaReale, StatoAsse, ErroreAnello}
//   Tier 4 (60s): 4 items - MachineState, RunTimeMin, StartCount, AlarmCount
//   Total       : 13 items, no STRING packets in this phase
//
// [ASCII Only]
//---------------------------------------------------------------------------
#ifndef SvcControllerH
#define SvcControllerH
//---------------------------------------------------------------------------
#include <SysUtils.hpp>
#include <Classes.hpp>
#include <SvcMgr.hpp>
#include <vcl.h>
#include "VaClasses.hpp"
#include "VaComm.hpp"
#include <ExtCtrls.hpp>
#include <IniFiles.hpp>

// Protocol constants (shared with ESP32 - same as AH221/GA3)
#define PROTO_STX       0x02
#define PROTO_ETX       0x03
#define MAX_PWX_ITEMS   100

// Response codes (ESP32 -> Agent - same as AH221/GA3)
#define RESP_CMD_ACK    0x01
#define RESP_CMD_NAK    0x02
#define RESP_STATUS_OK  0x00
#define RESP_STATUS_CHK 0x01
#define RESP_STATUS_LEN 0x02
#define RESP_STATUS_TMO 0x03
#define RESP_TIMEOUT_MS 5000

#define HK_DEBUG        0   // 1 = verbose hex dump in log

//---------------------------------------------------------------------------
// PowerFlex Data Item Structure
//
// Compared to AH221 TSqlItemInfo:
//   - Removed TableName (no SQL)
//   - Added Source ("ErrAsseXGVS" or "MONTH.TER")
//   - Removed STRING fields (MVP: no STRING packets)
//---------------------------------------------------------------------------
struct TPwxItemInfo
{
    int     ItemID;         // Unique ID for packet protocol
    String  VarName;        // Logical name (GVS1_QuotaReale, MachineState, ...)
    String  DataType;       // "INT" or "FLOAT" (FLOAT stored as int * scale)
    String  Description;    // Human-readable
    String  Source;         // "ErrAsseXGVS" or "MONTH.TER"
    int     Scale;          // For FLOAT: multiplier (e.g., 1000 = 3 decimal places)
    long    lValue;         // Current value (as long for packet)
    long    lPrevValue;     // Previous value (for change detection)
    int     Quality;        // 0xC0 = Good, 0x00 = Bad
    bool    Changed;        // Changed since last successful send
};

//---------------------------------------------------------------------------
// Polling Group IDs (time-tiered polling - identical concept to AH221)
//---------------------------------------------------------------------------
enum TPollGroup {
    pgAxisStatus = 0,       // Tier 1: ErrAsseXGVS (5s)
    pgMonthlyReport,        // Tier 4: MONTH##.TER (60s)
    pgCOUNT                 // sentinel (=2 in MVP)
};

struct TPollGroupConfig
{
    String  sName;          // INI key / log label
    int     nIntervalSec;   // Poll interval (seconds)
    int     nCycleCount;    // Cycles elapsed since last poll
    int     nCyclesNeeded;  // nIntervalSec / (BaseInterval/1000)
    bool    bEnabled;       // From INI
    bool    bDataReady;     // New data polled this cycle
};

//---------------------------------------------------------------------------
class TSCM_PowerflexAgent : public TService
{
__published:    // IDE-managed
    TVaComm *Mycomm;
    TTimer  *Timer1;

    void __fastcall Timer1Timer(TObject *Sender);
    void __fastcall ServiceStart(TService *Sender, bool &Started);
    void __fastcall ServiceStop(TService *Sender, bool &Stopped);

private:
    // === INI Settings ===
    // Albatros 설치 경로 (예: "C:\\Albatros\\")
    String  m_sAlbatrosBase;
    // 파생 경로 (LoadSettings 에서 계산):
    //   m_sErrAsseXGVSDir = <base>/Report/ErrAsseXGVS/
    //   m_sTmpDir         = <base>/Tmp/
    // (주의: BCB6 는 // 주석 끝의 백슬래시를 라인 continuation 으로 해석하므로
    //  Windows 경로 슬래시는 코멘트에 쓰지 않는다)
    String  m_sErrAsseXGVSDir;
    String  m_sTmpDir;

    int     m_nComPort;
    int     m_nBaudRate;
    int     m_nTimeInterval;    // Base timer interval (ms) - 5000 = 5s

    // === Item array ===
    TPwxItemInfo m_Items[MAX_PWX_ITEMS];
    int          m_ItemCount;

    // === Polling groups ===
    TPollGroupConfig m_Groups[pgCOUNT];
    void __fastcall InitPollGroups();
    bool __fastcall ShouldPollGroup(int grpIdx);

    // === Serial Communication (same as AH221) ===
    bool        m_bCommOpened;
    BYTE        m_SendBuffer[4096];
    bool        m_bFirstSend;

    // === Response/Retry (same as AH221) ===
    int         m_nRetryCount;
    int         m_nMaxRetries;
    bool        m_bWaitingResponse;
    DWORD       m_dwLastSendTick;
    DWORD       m_dwHeartbeatInterval;

    // No-change counter (log suppression - same as AH221)
    int         m_nNoChangeCount;

    // === Log buffer ===
    TCHAR       gbuf[65535];

    // === Albatros file tracking ===
    //
    // ErrAsseXGVS:
    //   파일은 YYYYMMDD_ErrAsseXGVS.txt 형태로 일별 생성.
    //   Albatros 가 추가 쓰기 (append) 만 수행하므로 마지막 읽은 위치(byte)부터
    //   tail-read 방식이 가장 안전.
    //
    // MONTH##.TER:
    //   월별 1개 파일. 이벤트가 추가될 때마다 append.
    //   매 폴링마다 파일 전체를 다시 읽고 통계 재계산 (파일 크기가 작아서 OK).
    String      m_sLastAxisFile;        // 마지막으로 읽은 ErrAsseXGVS 파일 경로
    long        m_lLastAxisFilePos;     // 다음에 읽을 위치 (바이트)
    String      m_sCurrentMonthFile;    // 현재 월 MONTH##.TER 경로

    // --- Logging (identical to AH221) ---
    void __fastcall LogMessage(String msg);
    void __fastcall WriteStatusFile(String msg);

    // --- Settings ---
    void __fastcall LoadSettings();

    // --- Per-group poll functions (replaces AH221's SQL poll functions) ---
    bool __fastcall PollErrAsseXGVS();
    bool __fastcall PollMonthlyReport();

    // --- Albatros file helpers ---
    String __fastcall GetTodayAxisFilePath();    // <dir>\YYYYMMDD_ErrAsseXGVS.txt
    String __fastcall GetCurrentMonthFilePath(); // <Tmp>\MONTH##.TER
    String __fastcall GetTodayDateString();      // "DD/MM/YYYY" (Italian)

    // ErrAsseXGVS line parser
    //   입력: "GVS1_X; time=...; quotaReale=199.945; statoAsse=4; erroreAnello=0.001; ..."
    //   출력: nAxisIdx (0=GVS1, 1=GVS2, 2=GVS3), dQuotaReale, nStatoAsse, dErroreAnello
    bool __fastcall ParseAxisLine(const AnsiString& line,
                                  int& nAxisIdx,
                                  double& dQuotaReale,
                                  int& nStatoAsse,
                                  double& dErroreAnello);

    // Item update by ItemID (helper to avoid index hunting)
    void __fastcall UpdateItemByID(int itemID, long value, int quality);

    // --- Serial Port (identical to AH221/GA3) ---
    bool __fastcall InitSerialPort(int portNum, int baudRate);
    void __fastcall CloseSerialPort();

    // --- Packet Protocol (identical to AH221, no STRING in MVP) ---
    BYTE __fastcall CalcChecksum(BYTE* data, int len);
    int  __fastcall BuildPacket(BYTE* buffer);
    void __fastcall SendToESP32(int changeCount = 0, bool isHeartbeat = false);

    // --- Change Detection ---
    bool __fastcall IsValueChanged(int index);

    // --- Response Handling (identical to AH221/GA3) ---
    bool __fastcall WaitForResponse(int timeoutMs);
    void __fastcall HandleSendFailure();

public:
    __fastcall TSCM_PowerflexAgent(TComponent* Owner);
    TServiceController __fastcall GetServiceController(void);

    friend void __stdcall ServiceController(unsigned CtrlCode);
};
//---------------------------------------------------------------------------
extern PACKAGE TSCM_PowerflexAgent *SCM_PowerflexAgent;
//---------------------------------------------------------------------------
#endif

