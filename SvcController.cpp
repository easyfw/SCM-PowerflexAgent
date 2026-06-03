//---------------------------------------------------------------------------
// SvcController.cpp - SCM-PowerFlex Agent Service Implementation (MVP)
//
// Target: Morbidelli PWX100 + TPA Albatros 3.1.9 SP 9c
//
// [What changed from AH221 version]
//   - Removed: ADODB.hpp, TADOConnection, TADOQuery, ConnectSQL/DisconnectSQL
//   - Removed: 4 SQL poll functions (PollDailyReport etc.)
//   - Removed: STRING packet support (BuildStringPacket) - re-enable later
//   - Added:   File polling for Albatros\Report\ErrAsseXGVS\*.txt
//   - Added:   File polling for Albatros\Tmp\MONTH##.TER
//   - Added:   ParseAxisLine() - semicolon-separated key=value parser
//   - Changed: LoadSettings() - [Albatros] / [Communication] / [PollIntervals]
//   - Changed: Items registered in ServiceStart() (13 items, MVP scope)
//
// [Kept identical to AH221 (verified patterns)]
//   - TVaComm serial communication
//   - Packet format [STX][LEN][CNT][ID][Q][VAL]...[CHK][ETX]
//   - InitPollGroups/ShouldPollGroup time-tiered polling
//   - Change detection, Heartbeat, ACK/NAK, retry, log rotation
//   - WaitForResponse(), HandleSendFailure()
//
// [Verified vs Unverified items - per user preference]
//   VERIFIED:
//     - Packet protocol, TVaComm serial, log/retry/heartbeat logic
//       (all proven in AH221 and GA3 production)
//     - Tail-read file polling pattern (proven in GA3 WinCutPlus integration)
//     - File path / format / field names (verified against Albatros FUN.FUN source)
//     - 5-axis configuration (verified via Com_Interface_WSCM.FUN PRESENZA_GVS_* constants)
//   UNVERIFIED (참조용, 실 PC 테스트 전):
//     - Italian date format "DD/MM/YYYY" assumption in MONTH##.TER
//     - Albatros 가 추가쓰기(append) 만 한다는 가정 (실측 확인 필요)
//     - StatoAsse 코드 의미 (4=In Position 으로 추정)
//     - MachineState 1/2 매핑 (1=RUN, 2=STOP ? ModAddr 컬럼 추정)
//
// [v0.2 - 5-axis extension]
//   - GVS4, GVS5 추가 (PRESENZA_GVS_4=1, PRESENZA_GVS_5=1 확인됨)
//   - 신규 ID 14~19 사용 (기존 Tier 4 ID 10~13 보존)
//   - ID 매핑 비연속 - axisBaseID[] 룩업 테이블 사용
//
// [ASCII Only]
//---------------------------------------------------------------------------
#include "SvcController.h"
#include <stdio.h>      // sscanf - locale-safe float parsing for Albatros files
//---------------------------------------------------------------------------
#pragma package(smart_init)
#pragma link "VaClasses"
#pragma link "VaComm"
#pragma resource "*.dfm"

TSCM_PowerflexAgent *SCM_PowerflexAgent;

// Log file management (same as AH221)
static int  g_LogFileIndex = 0;
static bool g_bFirstRun    = true;

//---------------------------------------------------------------------------
// Constructor
//---------------------------------------------------------------------------
__fastcall TSCM_PowerflexAgent::TSCM_PowerflexAgent(TComponent* Owner)
    : TService(Owner)
{
    this->OnStart = ServiceStart;
    this->OnStop  = ServiceStop;
    lstrcpy(gbuf, "[SCM-PowerFlex Service Log]\r\n");

    m_ItemCount = 0;
    m_bCommOpened = false;
    m_bFirstSend = true;

    // Response/Retry
    m_nRetryCount = 0;
    m_nMaxRetries = 3;
    m_bWaitingResponse = false;

    // Heartbeat
    m_dwLastSendTick = 0;
    m_dwHeartbeatInterval = 5000;

    // No-change counter
    m_nNoChangeCount = 0;

    // Default settings (overridden by INI)
    m_sAlbatrosBase = "C:\\Albatros\\";  // 일반적 설치 위치 (INI 에서 덮어씀)
    m_nComPort = 3;
    m_nBaudRate = 115200;
    m_nTimeInterval = 5000;

    // PowerProductionReport (Tier4 신규 소스) 기본값 - INI 에서 덮어씀
    m_sPprBase = "C:\\Report_PI\\PowerProductionReport\\";
    m_bPprEnabled = true;

    // File tracking initial state
    m_sLastAxisFile = "";
    m_lLastAxisFilePos = 0;
    m_sCurrentMonthFile = "";

    // Worker thread
    m_hWorkerThread = NULL;
    m_bWorkerStop = false;

    for (int i = 0; i < pgCOUNT; i++)
    {
        m_Groups[i].nCycleCount = 0;
        m_Groups[i].bDataReady = false;
    }
}

//---------------------------------------------------------------------------
TServiceController __fastcall TSCM_PowerflexAgent::GetServiceController(void)
{
    return (TServiceController) ServiceController;
}

void __stdcall ServiceController(unsigned CtrlCode)
{
    SCM_PowerflexAgent->Controller(CtrlCode);
}

//---------------------------------------------------------------------------
// LogMessage (identical to AH221)
//   60KB 단위 로테이션, ASCII timestamp [HH:MM:SS] prefix
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::LogMessage(String msg)
{
    HANDLE hFile;
    DWORD dwBytesWritten;
    DWORD dwFileSize;
    SYSTEMTIME st;
    String logFileName;

    String exePath = ExtractFilePath(ParamStr(0));
    String logBasePath = exePath + "logsave";

    while (true)
    {
        if (g_LogFileIndex == 0) logFileName = logBasePath + ".txt";
        else logFileName = logBasePath + "_" + IntToStr(g_LogFileIndex) + ".txt";

        hFile = CreateFile(
            logFileName.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            NULL,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );

        if (hFile == INVALID_HANDLE_VALUE) return;

        dwFileSize = GetFileSize(hFile, NULL);

        if (dwFileSize > 60000)
        {
            CloseHandle(hFile);
            g_LogFileIndex++;
            g_bFirstRun = false;
            continue;
        }
        break;
    }

    SetFilePointer(hFile, 0, NULL, FILE_END);

    if (g_bFirstRun)
    {
        if (dwFileSize > 0)
        {
            String blankLine = "\r\n";
            WriteFile(hFile, blankLine.c_str(), blankLine.Length(),
                      &dwBytesWritten, NULL);
        }
        g_bFirstRun = false;
    }

    GetLocalTime(&st);

    String timeStr;
    timeStr.printf("[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

    AnsiString finalMsg = timeStr + msg + "\r\n";
    WriteFile(hFile, finalMsg.c_str(), finalMsg.Length(),
              &dwBytesWritten, NULL);
    CloseHandle(hFile);
}

//---------------------------------------------------------------------------
// WriteStatusFile (identical to AH221) ? 변화 없는 사이클의 하트비트 상태
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::WriteStatusFile(String msg)
{
    String exePath = ExtractFilePath(ParamStr(0));
    String statusPath = exePath + "logsave_status.txt";

    HANDLE hFile = CreateFile(
        statusPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hFile == INVALID_HANDLE_VALUE)
    {
        // status 파일 생성 실패 시에만 기록(조용한 실패 방지)
        LogMessage("STATUS write fail err=" + IntToStr((int)GetLastError()));
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);

    String timeStr;
    timeStr.printf("[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);

    AnsiString finalMsg = timeStr + msg + "\r\n";
    DWORD dwBytesWritten;
    WriteFile(hFile, finalMsg.c_str(), finalMsg.Length(),
              &dwBytesWritten, NULL);
    CloseHandle(hFile);
}

//---------------------------------------------------------------------------
// LoadSettings
//   [Albatros] BasePath
//   [Communication] COM_Port, BaudRate
//   [Agent] TimeInterval
//   [PollIntervals] AxisStatus, MonthlyReport
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::LoadSettings()
{
    String IniPath = ExtractFilePath(ParamStr(0)) + "oem_setting.ini";

    TIniFile *ini = new TIniFile(IniPath);
    try
    {
        // [Albatros] - 설치 경로
        m_sAlbatrosBase = ini->ReadString("Albatros", "BasePath",
                                          "C:\\Albatros\\");
        if (m_sAlbatrosBase[m_sAlbatrosBase.Length()] != '\\')
            m_sAlbatrosBase += "\\";

        m_sErrAsseXGVSDir = m_sAlbatrosBase + "Report\\ErrAsseXGVS\\";
        m_sTmpDir         = m_sAlbatrosBase + "Tmp\\";

        // [Communication] (same as AH221/GA3)
        String comStr = ini->ReadString("Communication", "COM_Port", "COM3");
        if (comStr.UpperCase().Pos("COM") == 1)
            m_nComPort = StrToIntDef(
                comStr.SubString(4, comStr.Length() - 3), 3);
        else
            m_nComPort = StrToIntDef(comStr, 3);

        m_nBaudRate = ini->ReadInteger("Communication", "BaudRate", 115200);

        // [Agent]
        m_nTimeInterval = ini->ReadInteger("Agent", "TimeInterval", 5000);

        // [PowerProductionReport] - Tier4 신규 소스 (실 생산데이터 CSV)
        //   라이브 머신마다 경로가 다를 수 있어 INI 로 둔다(하드코딩 금지).
        m_sPprBase = ini->ReadString("PowerProductionReport", "BasePath",
                                     "C:\\Report_PI\\PowerProductionReport\\");
        if (m_sPprBase.Length() > 0 &&
            m_sPprBase[m_sPprBase.Length()] != '\\')
            m_sPprBase += "\\";
        m_bPprEnabled = ini->ReadBool("PowerProductionReport", "Enabled", true);

        // [PollIntervals]
        // 베이스 인터벌이 5초이므로:
        //   AxisStatus    : 5초 (매 사이클)
        //   MonthlyReport : 60초 (12사이클마다)
        int defaults[pgCOUNT] = { 5, 60 };
        String names[pgCOUNT] = { "AxisStatus", "MonthlyReport" };
        int baseIntervalSec = m_nTimeInterval / 1000;
        if (baseIntervalSec < 1) baseIntervalSec = 1;

        for (int i = 0; i < pgCOUNT; i++)
        {
            m_Groups[i].sName = names[i];
            m_Groups[i].nIntervalSec = ini->ReadInteger(
                "PollIntervals", names[i], defaults[i]);
            m_Groups[i].bEnabled = ini->ReadBool(
                "PollIntervals", names[i] + "_Enabled", true);
            m_Groups[i].nCyclesNeeded =
                m_Groups[i].nIntervalSec / baseIntervalSec;
            if (m_Groups[i].nCyclesNeeded < 1)
                m_Groups[i].nCyclesNeeded = 1;
            // 시작 즉시 첫 폴링 발생하도록 카운트 채워둠
            m_Groups[i].nCycleCount = m_Groups[i].nCyclesNeeded;
            m_Groups[i].bDataReady = false;
        }

        LogMessage("CFG: Albatros=" + m_sAlbatrosBase
                 + " COM" + IntToStr(m_nComPort)
                 + " " + IntToStr(m_nBaudRate)
                 + " T:" + IntToStr(m_nTimeInterval));
    }
    __finally
    {
        delete ini;
    }
}

//---------------------------------------------------------------------------
// InitPollGroups (identical to AH221)
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::InitPollGroups()
{
    for (int i = 0; i < pgCOUNT; i++)
    {
        LogMessage("  Poll " + m_Groups[i].sName
                 + ": " + IntToStr(m_Groups[i].nIntervalSec) + "s"
                 + " cyc=" + IntToStr(m_Groups[i].nCyclesNeeded)
                 + (m_Groups[i].bEnabled ? " ON" : " OFF"));
    }
}

//---------------------------------------------------------------------------
bool __fastcall TSCM_PowerflexAgent::ShouldPollGroup(int grpIdx)
{
    if (!m_Groups[grpIdx].bEnabled) return false;
    m_Groups[grpIdx].nCycleCount++;
    if (m_Groups[grpIdx].nCycleCount >= m_Groups[grpIdx].nCyclesNeeded)
    {
        m_Groups[grpIdx].nCycleCount = 0;
        return true;
    }
    return false;
}

//---------------------------------------------------------------------------
// File path helpers
//---------------------------------------------------------------------------
String __fastcall TSCM_PowerflexAgent::GetTodayAxisFilePath()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    String s;
    s.printf("%04d%02d%02d_ErrAsseXGVS.txt", st.wYear, st.wMonth, st.wDay);
    return m_sErrAsseXGVSDir + s;
}

String __fastcall TSCM_PowerflexAgent::GetCurrentMonthFilePath()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    String s;
    s.printf("MONTH%02d.TER", st.wMonth);
    return m_sTmpDir + s;
}

String __fastcall TSCM_PowerflexAgent::GetTodayDateString()
{
    // [FIX #3 - 2026-06-03 실데이터 검증] 날짜 포맷은 MM/DD/YYYY 확정.
    //   근거: 실 MONTH02.TER(2월 파일)에 02/02, 02/09, 02/23 출현.
    //         앞자리가 전부 02(=월), 23은 일 -> MM/DD/YYYY 확정.
    //   기존 코드는 DD/MM/YYYY 가정이라 day!=month 인 날 매칭이 전부 빗나가
    //   Tier 4 통계가 통째로 0/STOP 으로 나갔음.
    SYSTEMTIME st;
    GetLocalTime(&st);
    String s;
    s.printf("%02d/%02d/%04d", st.wMonth, st.wDay, st.wYear);  // MM/DD/YYYY
    return s;
}

//---------------------------------------------------------------------------
// UpdateItemByID - 데이터 갱신 헬퍼 (배열 순회 회피)
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::UpdateItemByID(
    int itemID, long value, int quality)
{
    for (int i = 0; i < m_ItemCount; i++)
    {
        if (m_Items[i].ItemID == itemID)
        {
            m_Items[i].lPrevValue = m_Items[i].lValue;
            m_Items[i].lValue = value;
            m_Items[i].Quality = quality;
            return;
        }
    }
    // ID 미등록 ? 무시 (LogMessage 하면 폴링마다 스팸)
}

//---------------------------------------------------------------------------
// ParseAxisLine - ErrAsseXGVS 한 줄 파싱
//
// 입력 형식 (실파일 샘플):
//   GVS1_X; time=4032596; quotaEncoder=199.945; quotaReale=200;
//   quotaTeorica=200; statoAsse=4; erroreAnello=0; velocita=0.00000183;
//   velocitaReale=0; goneSpace=100; resSpace=0;
//
// 반환:
//   true  : 정상 파싱
//   false : 라인이 손상되었거나 GVS{n}_X 형식이 아님
//
// 출력:
//   nAxisIdx : 0(GVS1), 1(GVS2), 2(GVS3), -1(unknown)
//   dQuotaReale, nStatoAsse, dErroreAnello
//---------------------------------------------------------------------------
//
// [BCB6 구문 주의]
//   try { } catch { } __finally { } 동시 사용 불가.
//   따라서 외부 try+__finally(자원 정리) / 내부 try+catch(예외 변환) 로 중첩.
//
// [로케일 주의]
//   StrToFloatDef 는 시스템 DecimalSeparator (이탈리아 PC=',') 를 따름.
//   Albatros 파일은 항상 '.' 로 출력되므로 sscanf("%lf") 사용 (C locale).
//
bool __fastcall TSCM_PowerflexAgent::ParseAxisLine(
    const AnsiString& line,
    int& nAxisIdx,
    double& dQuotaReale,
    int& nStatoAsse,
    double& dErroreAnello)
{
    nAxisIdx = -1;
    dQuotaReale = 0.0;
    nStatoAsse = 0;
    dErroreAnello = 0.0;

    if (line.Length() < 10) return false;

    //
    // [BCB6 호환성 주의]
    //   BCB6 의 TStringList 는 StrictDelimiter 속성이 없음 (Delphi 7+ 에서 추가).
    //   따라서 TStringList::DelimitedText 를 사용하면 공백도 구분자로 인식해
    //   "key=value" 가 깨짐. 수동 split 으로 처리한다.
    //

    bool result = false;
    try
    {
        // 1) 첫 세미콜론 위치 찾기 ? 그 앞이 축 ID
        int firstSemi = line.Pos(";");
        if (firstSemi <= 0) return false;

        AnsiString axisID = line.SubString(1, firstSemi - 1);
        axisID = axisID.Trim();
        if (axisID.SubString(1, 3) != "GVS") return false;

        // GVS 뒤 숫자 (1~5) - PWX100 은 5 GVS 그룹 모두 활성
        // (확인: Com_Interface_WSCM.FUN 의 PRESENZA_GVS_4=1, PRESENZA_GVS_5=1)
        int idx = StrToIntDef(axisID.SubString(4, 1), 0);
        if (idx >= 1 && idx <= 5) nAxisIdx = idx - 1;
        else return false;

        // 2) 나머지 부분을 ';' 로 수동 split 하며 key=value 추출
        AnsiString remain = line.SubString(firstSemi + 1, line.Length() - firstSemi);

        while (remain.Length() > 0)
        {
            // 다음 세미콜론 위치
            int sp = remain.Pos(";");
            AnsiString token;
            if (sp > 0)
            {
                token = remain.SubString(1, sp - 1);
                remain = remain.SubString(sp + 1, remain.Length() - sp);
            }
            else
            {
                token = remain;
                remain = "";
            }

            token = token.Trim();
            if (token.IsEmpty()) continue;

            int eq = token.Pos("=");
            if (eq <= 0) continue;

            AnsiString key = token.SubString(1, eq - 1);
            AnsiString val = token.SubString(eq + 1, token.Length() - eq);
            key = key.Trim();
            val = val.Trim();

            if (key == "quotaReale")
            {
                // sscanf 는 C locale ('.' 고정) - 이탈리아 로케일 무관하게 동작
                double d = 0.0;
                sscanf(val.c_str(), "%lf", &d);
                dQuotaReale = d;
            }
            else if (key == "statoAsse")
            {
                nStatoAsse = StrToIntDef(val, 0);
            }
            else if (key == "erroreAnello")
            {
                double d = 0.0;
                sscanf(val.c_str(), "%lf", &d);
                dErroreAnello = d;
            }
        }
        result = true;
    }
    catch (Exception&) { result = false; }

    return result;
}

//---------------------------------------------------------------------------
// PollErrAsseXGVS - Tier 1 (5s) 축 상태 폴링
//
// 동작 방식 (검증된 패턴 - GA3 WinCutPlus 와 동일):
//   1. 오늘 날짜 파일 경로 계산
//   2. 파일이 바뀌었으면(자정 경계) 위치 카운터 리셋
//   3. FILE_SHARE_READ|WRITE 모드로 열어 마지막 위치부터 끝까지 읽기
//   4. 완전한 라인(\r\n 종료) 만 파싱, 불완전 라인은 다음 폴링까지 보류
//   5. 라인마다 ParseAxisLine 호출 → UpdateItemByID 로 갱신
//
// 주의:
//   - Albatros 가 파일을 쓰는 동안 동시 읽기 가능해야 함 (FILE_SHARE 플래그)
//   - 파일이 아직 없으면(머신 가동 전) Quality=Bad 만 마크하고 정상 리턴
//---------------------------------------------------------------------------
bool __fastcall TSCM_PowerflexAgent::PollErrAsseXGVS()
{
    String filePath = GetTodayAxisFilePath();

    // 파일 변경 감지 (자정 경계 / 신규 파일)
    if (filePath != m_sLastAxisFile)
    {
        m_sLastAxisFile = filePath;
        m_lLastAxisFilePos = 0;
        LogMessage("AXIS: new file " + ExtractFileName(filePath));
    }

    HANDLE hFile = CreateFile(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,  // Albatros 동시쓰기 허용
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        // 파일 없음 ? 머신 아직 가동 안 했거나 경로 오류
        // 5축 × 3필드 = 15개 축 ID 모두 Quality=Bad 로 마크
        //   GVS1_X: ID 1, 2, 3
        //   GVS2_X: ID 4, 5, 6
        //   GVS3_X: ID 7, 8, 9
        //   GVS4_X: ID 14, 15, 16  (★ Tier 4 ID 10~13 회피)
        //   GVS5_X: ID 17, 18, 19
        static const int axisIDsForBadQuality[15] = {
            1, 2, 3,
            4, 5, 6,
            7, 8, 9,
            14, 15, 16,
            17, 18, 19
        };
        for (int i = 0; i < 15; i++)
            UpdateItemByID(axisIDsForBadQuality[i], 0, 0x00);
        if (HK_DEBUG)
            LogMessage("AXIS: file not found");
        return false;
    }

    try
    {
        DWORD fileSize = GetFileSize(hFile, NULL);
        if (fileSize == INVALID_FILE_SIZE) { CloseHandle(hFile); return false; }

        // 파일이 잘렸으면(혹시?) 처음부터
        if ((long)fileSize < m_lLastAxisFilePos)
            m_lLastAxisFilePos = 0;

        DWORD bytesToRead = fileSize - m_lLastAxisFilePos;
        if (bytesToRead == 0) { CloseHandle(hFile); return true; } // 변화 없음

        // 안전 한계 (1MB) ? 폭주 방지
        if (bytesToRead > 1024 * 1024)
        {
            // 너무 많으면 끝쪽 1MB 만 읽기 (이력 손실은 감수)
            m_lLastAxisFilePos = fileSize - (1024 * 1024);
            bytesToRead = 1024 * 1024;
            LogMessage("AXIS: skipped, file too large");
        }

        SetFilePointer(hFile, m_lLastAxisFilePos, NULL, FILE_BEGIN);

        // 동적 버퍼 할당
        char* buf = new char[bytesToRead + 1];
        try
        {
            DWORD nRead = 0;
            if (!ReadFile(hFile, buf, bytesToRead, &nRead, NULL) || nRead == 0)
            {
                CloseHandle(hFile);
                delete[] buf;
                return false;
            }
            buf[nRead] = '\0';

            // [FIX #1 - 2026-06-03 실데이터 검증] 레코드 구분자는 CR(0x0D) 단독.
            //   근거: 실파일 20260512_ErrAsseXGVS.txt 는 LF(0x0A) 0개, CR 3개.
            //         "...resSpace=0; \rGVS2_X;..." 처럼 CR 로만 레코드 구분.
            //   기존 코드는 '\n' 만 라인 경계로 인정 -> lastNewline 이 항상 -1 이 되어
            //   읽기 위치를 전진시키지 못하고 축 데이터를 영원히 한 줄도 파싱 못 했음.
            //   => CR 또는 LF 어느 쪽이든 마지막 경계로 인정한다.
            int lastNewline = -1;
            for (int i = (int)nRead - 1; i >= 0; i--)
            {
                if (buf[i] == '\n' || buf[i] == '\r')
                {
                    lastNewline = i;
                    break;
                }
            }

            if (lastNewline < 0)
            {
                // 새 데이터가 있지만 줄바꿈 없음 ? 다음 폴링까지 대기
                delete[] buf;
                CloseHandle(hFile);
                return true;
            }

            // 완전 라인만 처리, 위치 갱신
            int parseLen = lastNewline + 1;
            m_lLastAxisFilePos += parseLen;

            // [FIX #1 - 2026-06-03] CR-only 레코드를 안전하게 분해하기 위해
            //   buf 의 CR(0x0D) 을 모두 LF(0x0A) 로 정규화한 뒤 TStringList 에 넘긴다.
            //   (BCB6 TStringList::Text 의 CR-only 분리 동작에 의존하지 않기 위한
            //    방어적 처리. CRLF 였다면 CR->LF 로 빈 줄이 생기나 아래에서 skip 됨.)
            AnsiString chunk(buf, parseLen);
            for (int ci = 1; ci <= chunk.Length(); ci++)
                if (chunk[ci] == '\r') chunk[ci] = '\n';
            TStringList* lines = new TStringList();
            try
            {
                lines->Text = chunk;

                int parsedCount = 0;
                for (int li = 0; li < lines->Count; li++)
                {
                    AnsiString line = AnsiString(lines->Strings[li]).Trim();
                    if (line.IsEmpty()) continue;

                    int    nAxis;
                    double dQuota, dErr;
                    int    nState;

                    if (ParseAxisLine(line, nAxis, dQuota, nState, dErr))
                    {
                        if (nAxis < 0 || nAxis > 4) continue;

                        // ID 매핑 (비연속 ? Tier 4 ID 10~13 회피):
                        //   GVS1_X (nAxis=0): ID 1 (QuotaReale), 2 (StatoAsse), 3 (ErroreAnello)
                        //   GVS2_X (nAxis=1): ID 4, 5, 6
                        //   GVS3_X (nAxis=2): ID 7, 8, 9
                        //   GVS4_X (nAxis=3): ID 14, 15, 16  ★ 신규
                        //   GVS5_X (nAxis=4): ID 17, 18, 19  ★ 신규
                        // FLOAT 는 ×1000 정수 인코딩 (mm 0.001 단위)
                        static const int axisBaseID[5] = { 1, 4, 7, 14, 17 };
                        int baseID = axisBaseID[nAxis];
                        UpdateItemByID(baseID,     (long)(dQuota * 1000), 0xC0);
                        UpdateItemByID(baseID + 1, (long)nState,          0xC0);
                        UpdateItemByID(baseID + 2, (long)(dErr   * 1000), 0xC0);
                        parsedCount++;
                    }
                }

                if (HK_DEBUG && parsedCount > 0)
                    LogMessage("AXIS: parsed=" + IntToStr(parsedCount));
            }
            __finally { delete lines; }
        }
        __finally { delete[] buf; }
    }
    catch (Exception &e)
    {
        LogMessage("AXIS E: " + e.Message);
        CloseHandle(hFile);
        return false;
    }

    CloseHandle(hFile);
    return true;
}

//---------------------------------------------------------------------------
// PollMonthlyReport - Tier 4 (60s) 가동 통계 폴링
//
// 동작 방식:
//   1. 현재 월의 MONTH##.TER 파일 전체 읽기 (작은 파일이라 매번 통째)
//   2. 라인별 순회하며 오늘 날짜 매칭 라인만 분석
//   3. "starts execution" / "stops execution" / 알람 패턴 카운트
//   4. 마지막 이벤트로 MachineState 결정
//   5. start/stop 페어로 가동시간 합산
//
// MONTH##.TER 라인 예 (실파일 확인됨, UTF-16LE):
//   "07:55:56 02/02/2026 Albatros starts execution (3.1.9 ...) ID:52923 [PWX100AA...] 1"
//   "07:57:00 02/02/2026 Albatros stops execution                       2"
//   "08:17:01 02/23/2026 TMSCan+ 2: Not found  System  1035  0  Morbidelli PWX100"
//
// [검증완료 2026-06-03] 날짜 형식 = MM/DD/YYYY (FIX #3 반영), 인코딩 = UTF-16LE(FIX #2 반영).
//
// [주의 #4 - 의미 재정의 필요] "Albatros starts/stops execution" 은 머신 가동이 아니라
//   Albatros HMI 소프트웨어의 실행/종료 이벤트다(2월에 21회 start/21회 stop = HMI 재시작 반복).
//   따라서 현재의 DailyRunTimeMin 은 "HMI 가 켜져있던 시간", DailyStartCount 는
//   "HMI 실행 횟수" 를 측정할 뿐 실제 머신 가동률이 아니다. 이 통계를 머신 가동으로
//   쓰려면 ErrAsseXGVS 의 statoAsse/velocita 변화 기반 추정 등으로 재설계가 필요하다.
//   (※ 이 함수의 RunTime/StartCount 산출 로직은 그 전까지 '참조용'으로만 볼 것)
//---------------------------------------------------------------------------
bool __fastcall TSCM_PowerflexAgent::PollMonthlyReport()
{
    String filePath = GetCurrentMonthFilePath();
    m_sCurrentMonthFile = filePath;
    String todayStr = GetTodayDateString();  // "DD/MM/YYYY"

    HANDLE hFile = CreateFile(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        // 월 파일 없음 ? 신규 월 첫날일 수 있음
        UpdateItemByID(10, 2, 0x00);   // MachineState 미상 → STOP 가정 + Bad
        UpdateItemByID(11, 0, 0x00);
        UpdateItemByID(12, 0, 0x00);
        UpdateItemByID(13, 0, 0x00);
        if (HK_DEBUG)
            LogMessage("MONTH: file not found " + ExtractFileName(filePath));
        return false;
    }

    bool ok = false;
    try
    {
        DWORD fileSize = GetFileSize(hFile, NULL);
        // 안전 한계 ? MONTH 파일은 보통 수십 KB ~ 수 MB
        if (fileSize == 0 || fileSize > 10 * 1024 * 1024)
        {
            CloseHandle(hFile);
            return false;
        }

        char* buf = new char[fileSize + 1];
        try
        {
            DWORD nRead = 0;
            if (!ReadFile(hFile, buf, fileSize, &nRead, NULL) || nRead == 0)
            {
                delete[] buf;
                CloseHandle(hFile);
                return false;
            }
            buf[nRead] = '\0';

            // [FIX #2 - 2026-06-03 실데이터 검증] MONTH##.TER 은 BOM 없는 UTF-16LE.
            //   근거: 실 MONTH02.TER 앞바이트 54 00 69 00 ... = 'T'\0'i'\0'm'\0'e'\0
            //         (글자마다 상위바이트 0x00 이 붙는 2바이트/문자 인코딩).
            //   기존 코드는 단일바이트 ASCII 로 파싱 -> 날짜/이벤트 문자열 매칭이
            //   글자 사이 0x00 때문에 절대 성립하지 않아 Tier 4 통계가 전부 0/STOP.
            //   => 로그 내용이 ASCII 범위이므로 하위바이트만 추출해
            //      단일바이트 문자열(asc)로 변환한 뒤 기존 파서를 그대로 사용한다.
            //      UTF-16 의 줄바꿈 0D 00 0A 00 은 변환 후 0D 0A(CRLF) 가 되어
            //      TStringList 가 정상 분리한다.
            DWORD u16Start = 0;
            if (nRead >= 2 && (BYTE)buf[0] == 0xFF && (BYTE)buf[1] == 0xFE)
                u16Start = 2;                 // BOM(FF FE) 이 있으면 건너뜀
            char* asc = new char[(nRead / 2) + 2];
            int   ascLen = 0;
            for (DWORD bi = u16Start; bi + 1 < nRead; bi += 2)
            {
                // 상위바이트(buf[bi+1])가 0 이 아니면 비-ASCII -> '?' 로 대체
                // (통계 파싱에 쓰는 토큰은 모두 ASCII 라 영향 없음)
                asc[ascLen++] = ((BYTE)buf[bi + 1] == 0x00) ? buf[bi] : '?';
            }
            asc[ascLen] = '\0';

            // 통계 변수
            int  startCount = 0;
            int  alarmCount = 0;
            int  lastEvent  = 0;       // 1=START, 2=STOP, 3=ALARM, 0=NONE
            long runTimeSec = 0;       // 가동시간(초) ? start/stop 페어 합

            DWORD lastStartTick = 0;   // HH:MM:SS 를 초로 변환한 값
            bool  bInStart      = false;

            // 라인 파서 ? 줄바꿈으로 split (UTF-16 변환된 asc 사용)
            AnsiString chunk(asc, ascLen);
            delete[] asc;   // chunk 가 자체 복사본을 가지므로 즉시 해제 안전
            TStringList* lines = new TStringList();
            try
            {
                lines->Text = chunk;

                for (int li = 0; li < lines->Count; li++)
                {
                    AnsiString line = AnsiString(lines->Strings[li]);
                    if (line.Length() < 19) continue;  // 너무 짧은 라인 무시

                    // 오늘 날짜 매칭
                    if (line.Pos(todayStr) <= 0) continue;

                    // 시각 추출 (라인 첫 8자 "HH:MM:SS")
                    int hh = StrToIntDef(line.SubString(1, 2), 0);
                    int mm = StrToIntDef(line.SubString(4, 2), 0);
                    int ss = StrToIntDef(line.SubString(7, 2), 0);
                    long secOfDay = hh * 3600 + mm * 60 + ss;

                    // 이벤트 종류 식별 (대소문자 무관 검색)
                    bool isStart = (line.Pos("starts execution") > 0);
                    bool isStop  = (line.Pos("stops execution")  > 0);
                    // 알람 패턴 ? 광범위하게 잡되 start/stop 라인은 제외
                    bool isAlarm = false;
                    if (!isStart && !isStop)
                    {
                        if (line.Pos("Not found")        > 0 ||
                            line.Pos("not running")      > 0 ||
                            line.Pos("System")           > 0 ||
                            line.Pos("Error")            > 0 ||
                            line.Pos("Alarm")            > 0)
                            isAlarm = true;
                    }

                    if (isStart)
                    {
                        startCount++;
                        lastEvent = 1;
                        lastStartTick = secOfDay;
                        bInStart = true;
                    }
                    else if (isStop)
                    {
                        lastEvent = 2;
                        if (bInStart && secOfDay >= lastStartTick)
                        {
                            runTimeSec += (secOfDay - lastStartTick);
                            bInStart = false;
                        }
                    }
                    else if (isAlarm)
                    {
                        alarmCount++;
                        if (lastEvent != 2) lastEvent = 3;
                        // ALARM 도 MachineState 에 영향 (기동 중 알람)
                    }
                }

                // 마지막이 START 인데 STOP 없으면 ? 지금까지의 시간 추가
                if (bInStart)
                {
                    SYSTEMTIME st;
                    GetLocalTime(&st);
                    long nowSec = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
                    if (nowSec >= lastStartTick)
                        runTimeSec += (nowSec - lastStartTick);
                }

                // 아이템 갱신
                // MachineState: 1=RUN, 2=STOP, 3=ALARM
                int state = 2;  // 기본 STOP
                if (lastEvent == 1) state = 1;
                else if (lastEvent == 3) state = 3;

                UpdateItemByID(10, (long)state,            0xC0);
                UpdateItemByID(11, (long)(runTimeSec / 60), 0xC0);  // 분 단위
                UpdateItemByID(12, (long)startCount,       0xC0);
                UpdateItemByID(13, (long)alarmCount,       0xC0);

                if (HK_DEBUG)
                {
                    LogMessage("MONTH: state=" + IntToStr(state)
                             + " run=" + IntToStr(runTimeSec / 60) + "m"
                             + " starts=" + IntToStr(startCount)
                             + " alarms=" + IntToStr(alarmCount));
                }
                ok = true;
            }
            __finally { delete lines; }
        }
        __finally { delete[] buf; }
    }
    catch (Exception &e)
    {
        LogMessage("MONTH E: " + e.Message);
        ok = false;
    }

    CloseHandle(hFile);
    return ok;
}

//---------------------------------------------------------------------------
// [Tier4 재지향 - 2026-06-03] PowerProductionReport CSV 기반 가동통계
//
// 배경: 기존 Tier4(MONTH##.TER)는 HMI 실행/종료만 기록 -> 실 생산과 무관했음.
//       Albatros PowerInterface 가 생성하는 일별 생산 CSV 로 교체한다.
//       경로: <base>\pro\YYYY\YYYYMM\YYYYMMDD.csv  (평문 ASCII, CRLF, ISO 날짜)
//       파일명이 곧 날짜이므로 "오늘 파일"을 직접 열면 됨(in-file 날짜매칭 불필요).
//
// 산출 (실 CSV 로 검증: Sum(#PROD 피스) = 공식 #TOTALS 누적과 정확히 일치):
//   ID 10 MachineState   : 최신 #MAC 상태 -> 1=RUN 2=READY 3=NOT_READY 4=EMG 5=ALARM
//   ID 11 DailyRunTimeMin : Sum(#PROD 소요시간) / 60
//   ID 12 DailyPartCount  : Sum(#PROD 피스수)
//   ID 13 DailyAlarmCount : #MAC;ALARM_ON 발생 수
//
// 주의: #TOTALS 는 PowerInterface 세션마다 0 리셋되므로 #PROD 를 직접 합산한다.
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// GetTodayProductionCsvPath - <base>\pro\YYYY\YYYYMM\YYYYMMDD.csv
//---------------------------------------------------------------------------
String __fastcall TSCM_PowerflexAgent::GetTodayProductionCsvPath()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    String y, ym, ymd;
    y.printf("%04d", st.wYear);
    ym.printf("%04d%02d", st.wYear, st.wMonth);
    ymd.printf("%04d%02d%02d", st.wYear, st.wMonth, st.wDay);
    return m_sPprBase + "pro\\" + y + "\\" + ym + "\\" + ymd + ".csv";
}

//---------------------------------------------------------------------------
// ParseHmsToSec - "HH.MM.SS,cc" -> 초(정수). cc(1/100초)는 분단위 산출이라 버림.
//---------------------------------------------------------------------------
long __fastcall TSCM_PowerflexAgent::ParseHmsToSec(const AnsiString& t)
{
    int dot1 = t.Pos(".");
    if (dot1 <= 0) return 0;
    int hh = StrToIntDef(t.SubString(1, dot1 - 1), 0);
    AnsiString r1 = t.SubString(dot1 + 1, t.Length() - dot1);
    int dot2 = r1.Pos(".");
    if (dot2 <= 0) return 0;
    int mm = StrToIntDef(r1.SubString(1, dot2 - 1), 0);
    AnsiString r2 = r1.SubString(dot2 + 1, r1.Length() - dot2);
    int comma = r2.Pos(",");
    int ss = (comma > 0) ? StrToIntDef(r2.SubString(1, comma - 1), 0)
                         : StrToIntDef(r2, 0);
    return (long)hh * 3600 + (long)mm * 60 + (long)ss;
}

//---------------------------------------------------------------------------
// PollPowerProductionReport - 오늘자 생산 CSV 파싱 -> ID 10~13
//   반환: true=파싱성공(CSV 존재), false=CSV 없음/오류 -> PollTier4 가 폴백
//
// [구조] 기존 PollMonthlyReport 와 동일한 BCB6 안전 패턴:
//   외부 try/catch(Exception) + 내부 try/__finally(buf, lines 해제)
//   ASCII CRLF 이므로 UTF-16 디코딩 불필요(MONTH.TER 와 달리).
//---------------------------------------------------------------------------
bool __fastcall TSCM_PowerflexAgent::PollPowerProductionReport()
{
    String filePath = GetTodayProductionCsvPath();

    HANDLE hFile = CreateFile(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        // 오늘자 CSV 없음(PowerInterface 미실행) -> false 반환해 MONTH.TER 폴백 유도
        if (HK_DEBUG)
            LogMessage("PPR: no csv " + ExtractFileName(filePath));
        return false;
    }

    bool ok = false;
    try
    {
        DWORD fileSize = GetFileSize(hFile, NULL);
        if (fileSize == 0 || fileSize == INVALID_FILE_SIZE ||
            fileSize > 10 * 1024 * 1024)
        {
            CloseHandle(hFile);
            return false;
        }

        char* buf = new char[fileSize + 1];
        try
        {
            DWORD nRead = 0;
            if (!ReadFile(hFile, buf, fileSize, &nRead, NULL) || nRead == 0)
            {
                delete[] buf;
                CloseHandle(hFile);
                return false;
            }
            buf[nRead] = '\0';

            long pieces     = 0;     // Sum(#PROD 피스)
            long runSec     = 0;     // Sum(#PROD 소요시간[초])
            int  alarmCount = 0;     // #MAC;ALARM_ON 수

            // 상태머신 (우선순위 판정용)
            bool inEmergency = false;
            bool inAlarm     = false;
            bool running     = false;
            int  lastReady   = 0;    // 1=READY, 2=NOT_READY

            AnsiString chunk(buf, nRead);
            TStringList* lines = new TStringList();
            try
            {
                lines->Text = chunk;   // ASCII CRLF -> 정상 분리

                for (int li = 0; li < lines->Count; li++)
                {
                    AnsiString line = lines->Strings[li];
                    if (line.IsEmpty()) continue;

                    // ';' 수동 split (최대 16필드). 경로엔 ';' 없어 안전.
                    AnsiString fld[16];
                    int nf = 0;
                    AnsiString rem = line;
                    while (nf < 16)
                    {
                        int sp = rem.Pos(";");
                        if (sp > 0)
                        {
                            fld[nf++] = rem.SubString(1, sp - 1);
                            rem = rem.SubString(sp + 1, rem.Length() - sp);
                        }
                        else { fld[nf++] = rem; break; }
                    }
                    if (nf < 2) continue;

                    if (fld[0] == "#MAC")
                    {
                        AnsiString st = fld[1].Trim();
                        if      (st == "EMERGENCY")     inEmergency = true;
                        else if (st == "READY")       { inEmergency = false; lastReady = 1; }
                        else if (st == "NOT_READY")   { inEmergency = false; running = false; lastReady = 2; }
                        else if (st == "ALARM_ON")    { inAlarm = true; alarmCount++; }
                        else if (st == "ALARM_OFF")     inAlarm = false;
                        else if (st == "PROGRAM_START") running = true;
                        else if (st == "PROGRAM_END")   running = false;
                    }
                    else if (fld[0] == "#PROD")
                    {
                        // 0#PROD 1op 2prog 3X 4Y 5Z 6sd 7st 8ed 9et 10pcs 11dur 12sub 13sub
                        if (nf >= 12)
                        {
                            pieces += StrToIntDef(fld[10].Trim(), 0);
                            runSec += ParseHmsToSec(fld[11].Trim());
                        }
                    }
                    // #TOTALS 는 세션 리셋 때문에 사용하지 않음(위 #PROD 직접합산)
                }

                // MachineState 코드 도출 (우선순위)
                int state;
                if      (inEmergency)      state = 4;   // EMERGENCY
                else if (inAlarm)         state = 5;   // ALARM
                else if (running)         state = 1;   // RUN(프로그램 실행중)
                else if (lastReady == 1)  state = 2;   // READY/IDLE
                else                      state = 3;   // NOT_READY/STOP

                UpdateItemByID(10, (long)state,         0xC0);
                UpdateItemByID(11, (long)(runSec / 60), 0xC0);  // 분
                UpdateItemByID(12, (long)pieces,        0xC0);
                UpdateItemByID(13, (long)alarmCount,    0xC0);

                if (HK_DEBUG)
                {
                    LogMessage("PPR: state=" + IntToStr(state)
                             + " run=" + IntToStr(runSec / 60) + "m"
                             + " parts=" + IntToStr(pieces)
                             + " alarms=" + IntToStr(alarmCount));
                }
                ok = true;
            }
            __finally { delete lines; }
        }
        __finally { delete[] buf; }
    }
    catch (Exception &e)
    {
        LogMessage("PPR E: " + e.Message);
        ok = false;
    }

    CloseHandle(hFile);
    return ok;
}

//---------------------------------------------------------------------------
// PollTier4 - 하이브리드: PowerProductionReport CSV 우선, 없으면 MONTH.TER 폴백
//   PowerInterface 가 켜진 날은 실 생산데이터, 꺼진 날은 최소 HMI 가용성 유지.
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::PollTier4()
{
    if (m_bPprEnabled && PollPowerProductionReport())
        return;             // 실 생산 CSV 사용
    PollMonthlyReport();    // 폴백: MONTH##.TER (HMI 가용성)
}

//---------------------------------------------------------------------------
// InitSerialPort (identical to AH221/GA3)
//---------------------------------------------------------------------------
bool __fastcall TSCM_PowerflexAgent::InitSerialPort(int portNum, int baudRate)
{
    try
    {
        if (Mycomm == NULL)
        {
            LogMessage("Error: Mycomm component is NULL");
            return false;
        }

        if (Mycomm->Active()) Mycomm->Close();

        Mycomm->PortNum = portNum;

        switch (baudRate)
        {
            case 9600:   Mycomm->Baudrate = br9600;   break;
            case 19200:  Mycomm->Baudrate = br19200;  break;
            case 38400:  Mycomm->Baudrate = br38400;  break;
            case 57600:  Mycomm->Baudrate = br57600;  break;
            case 115200: Mycomm->Baudrate = br115200; break;
            default:     Mycomm->Baudrate = br115200; break;
        }

        Mycomm->Databits = db8;
        Mycomm->Stopbits = sb1;
        Mycomm->Parity = paNone;

        Mycomm->Open();

        if (Mycomm->Active())
        {
            m_bCommOpened = true;
            LogMessage("Serial port COM" + IntToStr(portNum)
                     + " opened at " + IntToStr(baudRate) + " bps");
            return true;
        }
        else
        {
            LogMessage("Failed to open COM" + IntToStr(portNum));
            return false;
        }
    }
    catch (Exception &ex)
    {
        LogMessage("Serial port error: " + ex.Message);
        return false;
    }
}

//---------------------------------------------------------------------------
// CloseSerialPort (identical to AH221/GA3)
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::CloseSerialPort()
{
    try
    {
        if (Mycomm && Mycomm->Active())
        {
            Mycomm->Close();
            m_bCommOpened = false;
            LogMessage("Serial port closed.");
        }
    }
    catch (Exception &ex)
    {
        LogMessage("Error closing serial port: " + ex.Message);
    }
}

//---------------------------------------------------------------------------
// CalcChecksum (identical to AH221/GA3)
//---------------------------------------------------------------------------
BYTE __fastcall TSCM_PowerflexAgent::CalcChecksum(BYTE* data, int len)
{
    BYTE checksum = 0;
    for (int i = 0; i < len; i++)
        checksum ^= data[i];
    return checksum;
}

//---------------------------------------------------------------------------
// IsValueChanged (INT only - STRING removed in MVP)
//---------------------------------------------------------------------------
bool __fastcall TSCM_PowerflexAgent::IsValueChanged(int index)
{
    if (index < 0 || index >= m_ItemCount) return false;
    return (m_Items[index].lValue != m_Items[index].lPrevValue);
}

//---------------------------------------------------------------------------
// BuildPacket - identical to AH221 regular packet
//   [STX][LEN_L][LEN_H][CNT][ID_L][ID_H][Q][V0][V1][V2][V3]...[CHK][ETX]
//---------------------------------------------------------------------------
int __fastcall TSCM_PowerflexAgent::BuildPacket(BYTE* buffer)
{
    int pos = 0;

    buffer[pos++] = PROTO_STX;

    int lenPos = pos;
    pos += 2;

    buffer[pos++] = (BYTE)m_ItemCount;

    for (int i = 0; i < m_ItemCount; i++)
    {
        WORD itemId = (WORD)m_Items[i].ItemID;
        buffer[pos++] = (BYTE)(itemId & 0xFF);
        buffer[pos++] = (BYTE)((itemId >> 8) & 0xFF);

        buffer[pos++] = (BYTE)m_Items[i].Quality;

        long value = m_Items[i].lValue;
        buffer[pos++] = (BYTE)(value & 0xFF);
        buffer[pos++] = (BYTE)((value >> 8) & 0xFF);
        buffer[pos++] = (BYTE)((value >> 16) & 0xFF);
        buffer[pos++] = (BYTE)((value >> 24) & 0xFF);
    }

    WORD dataLen = pos - 3;
    buffer[lenPos] = (BYTE)(dataLen & 0xFF);
    buffer[lenPos + 1] = (BYTE)((dataLen >> 8) & 0xFF);

    buffer[pos] = CalcChecksum(&buffer[1], pos - 1);
    pos++;

    buffer[pos++] = PROTO_ETX;

    return pos;
}

//---------------------------------------------------------------------------
// SendToESP32 (simplified from AH221 - no STRING packet in MVP)
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::SendToESP32(int changeCount,
                                                 bool isHeartbeat)
{
    if (!m_bCommOpened || Mycomm == NULL || !Mycomm->Active())
    {
        LogMessage("E:COM not ready");
        return;
    }

    try
    {
        int packetLen = BuildPacket(m_SendBuffer);

        // Purge RX buffer
        while (Mycomm->ReadBufUsed() > 0)
        {
            BYTE dummy;
            Mycomm->ReadBuf(&dummy, 1);
        }

#if HK_DEBUG
        String hexDump = "TX: ";
        for (int i = 0; i < packetLen; i++)
            hexDump += IntToHex(m_SendBuffer[i], 2) + " ";
        LogMessage(hexDump);
#endif

        Mycomm->WriteBuf(m_SendBuffer, packetLen);

        // Compact log
        String logMsg = "D";
        if (isHeartbeat) logMsg += "(HB)";
        logMsg += ":" + IntToStr(m_ItemCount);
        if (changeCount > 0) logMsg += "(C:" + IntToStr(changeCount) + ")";
        logMsg += " TX:" + IntToStr(packetLen);

        if (WaitForResponse(RESP_TIMEOUT_MS))
        {
            logMsg += " OK";

            for (int i = 0; i < m_ItemCount; i++)
            {
                m_Items[i].lPrevValue = m_Items[i].lValue;
                m_Items[i].Changed = false;
            }
            m_nRetryCount = 0;
        }
        else
        {
            logMsg += " FAIL";
            HandleSendFailure();
        }

        // 변화 있을 때만 로그 파일, 무변화는 status 파일에 덮어쓰기
        if (changeCount > 0 || logMsg.Pos("FAIL") > 0)
        {
            m_nNoChangeCount = 0;
            LogMessage(logMsg);
        }
        else
        {
            m_nNoChangeCount++;
            WriteStatusFile("NoChange:" + IntToStr(m_nNoChangeCount)
                          + " " + logMsg);
        }
    }
    catch (Exception &ex)
    {
        LogMessage("E:" + ex.Message);
    }
}

//---------------------------------------------------------------------------
// WaitForResponse (identical to AH221/GA3)
//---------------------------------------------------------------------------
bool __fastcall TSCM_PowerflexAgent::WaitForResponse(int timeoutMs)
{
    if (!m_bCommOpened || Mycomm == NULL || !Mycomm->Active())
        return false;

    BYTE respBuffer[5];
    int respIndex = 0;
    DWORD startTick = GetTickCount();

    m_bWaitingResponse = true;

    while (GetTickCount() - startTick < (DWORD)timeoutMs)
    {
        if (Mycomm->ReadBufUsed() > 0)
        {
            BYTE b;
            if (Mycomm->ReadBuf(&b, 1) == 1)
            {
                if (b == PROTO_STX && respIndex == 0)
                {
                    respBuffer[respIndex++] = b;
                }
                else if (respIndex > 0 && respIndex < 5)
                {
                    respBuffer[respIndex++] = b;

                    if (respIndex == 5)
                    {
                        m_bWaitingResponse = false;

                        if (respBuffer[4] != PROTO_ETX)
                            return false;

                        BYTE calcChk = respBuffer[1] ^ respBuffer[2];
                        if (calcChk != respBuffer[3])
                            return false;

                        BYTE cmd = respBuffer[1];
                        BYTE status = respBuffer[2];

                        return (cmd == RESP_CMD_ACK
                             && status == RESP_STATUS_OK);
                    }
                }
            }
        }
        Sleep(10);
    }

    m_bWaitingResponse = false;
    return false;
}

//---------------------------------------------------------------------------
// HandleSendFailure (identical to AH221/GA3)
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::HandleSendFailure()
{
    m_nRetryCount++;

    if (m_nRetryCount >= m_nMaxRetries)
    {
        LogMessage("Reconn...");

        CloseSerialPort();
        Sleep(1000);

        if (InitSerialPort(m_nComPort, m_nBaudRate))
        {
            LogMessage("COM OK");
            m_nRetryCount = 0;
        }
        else
        {
            LogMessage("COM FAIL");
        }
    }
}

//---------------------------------------------------------------------------
// ServiceStart
//
// 1. Load INI
// 2. Register 13 items (Tier 1 + Tier 4)
// 3. Init serial port
// 4. Initial poll test
// 5. Enable timer
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::ServiceStart(TService *Sender,
                                                  bool &Started)
{
    if (Timer1) Timer1->Enabled = false;

    LogMessage("SVC START - SCM-PowerFlex (Morbidelli PWX100) v1.1");
    Started = true;

    try
    {
        // 0. Load INI
        LoadSettings();
        InitPollGroups();

        // 1. Register PowerFlex data items
        //    [Replaces AH221's SQL item registration]
        m_ItemCount = 0;

        // === Tier 1: ErrAsseXGVS (5 axes × 3 fields = 15 items) ===
        // QuotaReale: mm × 1000 (3 decimal places)
        // StatoAsse:  integer (4 = In Position, others = states)
        // ErroreAnello: mm × 1000
        //
        // ID 매핑 ? 비연속 (Tier 4 ID 10~13 보존):
        //   GVS1: 1, 2, 3
        //   GVS2: 4, 5, 6
        //   GVS3: 7, 8, 9
        //   [ID 10~13: Tier 4 가 사용]
        //   GVS4: 14, 15, 16
        //   GVS5: 17, 18, 19

        //
        // [BCB6 호환성 주의]
        //   BCB6 는 비-POD (예: String) 멤버를 가진 구조체의 aggregate
        //   초기화 { ... } 를 허용하지 않는다. 따라서 임시 struct 의
        //   문자열 필드를 const char* 로 두고, 등록 시 String 으로 변환.
        //
        struct {
            int          id;
            const char*  name;
            const char*  desc;
            int          scale;
        } axisItems[15] = {
            // GVS1 (EtherCAT CN(3), drive 34UX21)
            {  1, "GVS1_QuotaReale",   "Axis 1 X real pos (mm*1000)",    1000 },
            {  2, "GVS1_StatoAsse",    "Axis 1 state code",              1    },
            {  3, "GVS1_ErroreAnello", "Axis 1 loop error (mm*1000)",    1000 },
            // GVS2 (EtherCAT CN(7), drive 34UX22)
            {  4, "GVS2_QuotaReale",   "Axis 2 X real pos (mm*1000)",    1000 },
            {  5, "GVS2_StatoAsse",    "Axis 2 state code",              1    },
            {  6, "GVS2_ErroreAnello", "Axis 2 loop error (mm*1000)",    1000 },
            // GVS3 (EtherCAT CN(11), drive 34UX23)
            {  7, "GVS3_QuotaReale",   "Axis 3 X real pos (mm*1000)",    1000 },
            {  8, "GVS3_StatoAsse",    "Axis 3 state code",              1    },
            {  9, "GVS3_ErroreAnello", "Axis 3 loop error (mm*1000)",    1000 },
            // [주의 #5 - 2026-06-03 실데이터 검증] GVS4/GVS5 는 현재 실 로그에 없음.
            //   2018~2026.05.12 전체 ErrAsseXGVS 파일에 GVS4_X/GVS5_X 0건(GVS1~3 만 존재).
            //   PRESENZA_GVS_4/5=1 컴파일 상수와 실제 로그 출력은 별개였음.
            //   => ID 14~19 는 항상 Quality=Bad(0) 로 전송됨. 3축 운영을 권장하며,
            //      아래 GVS4/5 등록을 유지할지(미래 대비) 제거할지는 운영 판단 사항.
            // GVS4 (drive 34UX24)
            { 14, "GVS4_QuotaReale",   "Axis 4 X real pos (mm*1000)",    1000 },
            { 15, "GVS4_StatoAsse",    "Axis 4 state code",              1    },
            { 16, "GVS4_ErroreAnello", "Axis 4 loop error (mm*1000)",    1000 },
            // GVS5 (drive 34UX25)
            { 17, "GVS5_QuotaReale",   "Axis 5 X real pos (mm*1000)",    1000 },
            { 18, "GVS5_StatoAsse",    "Axis 5 state code",              1    },
            { 19, "GVS5_ErroreAnello", "Axis 5 loop error (mm*1000)",    1000 }
        };
        for (int i = 0; i < 15; i++)
        {
            m_Items[m_ItemCount].ItemID      = axisItems[i].id;
            m_Items[m_ItemCount].VarName     = String(axisItems[i].name);
            m_Items[m_ItemCount].DataType    = (axisItems[i].scale > 1)
                                               ? String("FLOAT") : String("INT");
            m_Items[m_ItemCount].Description = String(axisItems[i].desc);
            m_Items[m_ItemCount].Source      = String("ErrAsseXGVS");
            m_Items[m_ItemCount].Scale       = axisItems[i].scale;
            m_Items[m_ItemCount].lValue      = 0;
            m_Items[m_ItemCount].lPrevValue  = 0;
            m_Items[m_ItemCount].Quality     = 0x00;
            m_Items[m_ItemCount].Changed     = false;
            m_ItemCount++;
        }

        // === Tier 4: MONTH##.TER (machine uptime statistics) ===
        // (BCB6 호환: const char* 로 두어 aggregate 초기화 가능하게)
        struct {
            int          id;
            const char*  name;
            const char*  desc;
        } monthItems[4] = {
            { 10, "MachineState",     "Machine state (1=RUN 2=STOP 3=ALARM)" },
            { 11, "DailyRunTimeMin",  "Daily runtime (minutes)"              },
            { 12, "DailyStartCount",  "Daily start event count"              },
            { 13, "DailyAlarmCount",  "Daily alarm/error event count"        }
        };
        for (int i = 0; i < 4; i++)
        {
            m_Items[m_ItemCount].ItemID      = monthItems[i].id;
            m_Items[m_ItemCount].VarName     = String(monthItems[i].name);
            m_Items[m_ItemCount].DataType    = String("INT");
            m_Items[m_ItemCount].Description = String(monthItems[i].desc);
            m_Items[m_ItemCount].Source      = String("MONTH.TER");
            m_Items[m_ItemCount].Scale       = 1;
            m_Items[m_ItemCount].lValue      = 0;
            m_Items[m_ItemCount].lPrevValue  = 0;
            m_Items[m_ItemCount].Quality     = 0x00;
            m_Items[m_ItemCount].Changed     = false;
            m_ItemCount++;
        }

        LogMessage("Items: " + IntToStr(m_ItemCount));

        // 2. Serial port (TVaComm - same as AH221/GA3)
        if (!InitSerialPort(m_nComPort, m_nBaudRate))
        {
            LogMessage("COM FAIL");
        }

        // 3. Initial poll test
        if (PollErrAsseXGVS())   LogMessage("INIT AXIS OK");
        PollTier4();             LogMessage("INIT TIER4 done");  // CSV 우선/폴백

        m_bFirstSend = true;
        m_dwLastSendTick = 0;

        // 4. Start worker thread (TTimer 대체)
        //   서비스엔 메시지펌프가 없어 TTimer(WM_TIMER)가 안 도는 경우가 있다.
        //   COM 의존 없이 확실히 도는 워커 스레드로 주기 폴링을 수행한다.
        m_bWorkerStop = false;
        m_hWorkerThread = CreateThread(NULL, 0, WorkerThreadProc, this, 0, NULL);
        if (m_hWorkerThread != NULL)
            LogMessage("Worker thread started, Interval="
                     + IntToStr(m_nTimeInterval));
        else
            LogMessage("ERROR: Worker thread create FAIL err="
                     + IntToStr((int)GetLastError()));

        LogMessage("SVC READY");
    }
    catch (Exception &ex)
    {
        LogMessage("E:" + ex.Message);
    }
}

//---------------------------------------------------------------------------
// ServiceStop
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::ServiceStop(TService *Sender,
                                                 bool &Stopped)
{
    LogMessage("SVC STOP");

    // 워커 스레드 정지 (최대 10초 대기)
    m_bWorkerStop = true;
    if (m_hWorkerThread != NULL)
    {
        WaitForSingleObject(m_hWorkerThread, 10000);
        CloseHandle(m_hWorkerThread);
        m_hWorkerThread = NULL;
    }
    if (Timer1) Timer1->Enabled = false;
    CloseSerialPort();

    Stopped = true;
    LogMessage("SVC END");
}

//---------------------------------------------------------------------------
// Timer1Timer - Main polling loop
//
// 차이점 (AH221 대비):
//   - SQL 재연결 로직 제거 (파일 폴링은 영구 연결 없음)
//   - PollDailyReport 등 4개 SQL 함수 → PollErrAsseXGVS, PollMonthlyReport
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// WorkerThreadProc - 워커 스레드 진입점 (TTimer 대체)
//   m_nTimeInterval 주기로 DoPollCycle() 호출. 정지신호에 빠르게 반응하도록
//   대기를 100ms 로 쪼갠다. 첫 사이클은 즉시 실행(초기 데이터 빠르게).
//---------------------------------------------------------------------------
DWORD WINAPI TSCM_PowerflexAgent::WorkerThreadProc(LPVOID param)
{
    TSCM_PowerflexAgent* self = (TSCM_PowerflexAgent*)param;
    bool firstLoop = true;
    while (!self->m_bWorkerStop)
    {
        if (!firstLoop)
        {
            int waited = 0;
            while (waited < self->m_nTimeInterval && !self->m_bWorkerStop)
            {
                Sleep(100);
                waited += 100;
            }
            if (self->m_bWorkerStop) break;
        }
        firstLoop = false;
        self->DoPollCycle();
    }
    return 0;
}

//---------------------------------------------------------------------------
// DoPollCycle - 1주기 폴링+전송 (구 Timer1Timer 본문, 타이머 토글 제거)
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::DoPollCycle()
{
    try
    {
        if (m_ItemCount > 0)
        {
            // 1. Time-tiered file polling
            if (ShouldPollGroup(pgAxisStatus))
                PollErrAsseXGVS();

            if (ShouldPollGroup(pgMonthlyReport))
                PollTier4();

            // 2. Change detection
            int changeCount = 0;
            for (int i = 0; i < m_ItemCount; i++)
            {
                if (IsValueChanged(i))
                {
                    m_Items[i].Changed = true;
                    changeCount++;
                }
            }
            bool hasChanges = (changeCount > 0);

            // 3. Heartbeat timeout
            DWORD dwNow = GetTickCount();
            bool heartbeatTimeout = false;

            if (m_dwLastSendTick == 0)
            {
                heartbeatTimeout = true;
            }
            else
            {
                DWORD elapsed;
                if (dwNow >= m_dwLastSendTick)
                    elapsed = dwNow - m_dwLastSendTick;
                else
                    elapsed = (0xFFFFFFFF - m_dwLastSendTick) + dwNow + 1;

                if (elapsed >= m_dwHeartbeatInterval)
                    heartbeatTimeout = true;
            }

            // 4. Send condition
            if (m_bFirstSend || hasChanges || heartbeatTimeout)
            {
                bool isHB = heartbeatTimeout && !hasChanges && !m_bFirstSend;

                SendToESP32(changeCount, isHB);
                m_dwLastSendTick = GetTickCount();

                m_bFirstSend = false;
            }
        }
    }
    catch (Exception &e)
    {
        LogMessage("E:" + e.Message);
    }
}

//---------------------------------------------------------------------------
// Timer1Timer - 미사용(워커 스레드로 대체). .dfm 바인딩 유지를 위해 남겨둠.
//---------------------------------------------------------------------------
void __fastcall TSCM_PowerflexAgent::Timer1Timer(TObject *Sender)
{
    DoPollCycle();
}

//---------------------------------------------------------------------------
