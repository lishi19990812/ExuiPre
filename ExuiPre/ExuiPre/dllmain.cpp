//EXUI前置文件，编译为：ExuiPre.fne
#define __GCC_
#include <windows.h>
#include <tchar.h>
#include "lang.h"
#include "lib2.h"

#define PAYLOAD_MAGIC   0x45555849   // 'EXUI'
#define PAYLOAD_TAIL    8

extern "C" const IMAGE_DOS_HEADER __ImageBase;

//==================== 尾部 payload 释放（fne 动态版用） ====================
static void ReleasePayload()
{
    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA((HMODULE)&__ImageBase, path, MAX_PATH);

    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= PAYLOAD_TAIL) {
        CloseHandle(hFile); return;
    }

    char tail[PAYLOAD_TAIL];
    LARGE_INTEGER pos;
    pos.QuadPart = fileSize.QuadPart - PAYLOAD_TAIL;
    SetFilePointerEx(hFile, pos, NULL, FILE_BEGIN);
    DWORD rd = 0;
    if (!ReadFile(hFile, tail, PAYLOAD_TAIL, &rd, NULL) || rd != PAYLOAD_TAIL) {
        CloseHandle(hFile); return;
    }

    DWORD magic = *(DWORD*)tail;
    DWORD libLen = *(DWORD*)(tail + 4);

    if (magic != PAYLOAD_MAGIC) { CloseHandle(hFile); return; }
    if (libLen == 0) { CloseHandle(hFile); return; }
    if (libLen > fileSize.QuadPart - PAYLOAD_TAIL) { CloseHandle(hFile); return; }

    char* buf = (char*)malloc(libLen);
    if (!buf) { CloseHandle(hFile); return; }

    pos.QuadPart = fileSize.QuadPart - PAYLOAD_TAIL - libLen;
    SetFilePointerEx(hFile, pos, NULL, FILE_BEGIN);
    if (!ReadFile(hFile, buf, libLen, &rd, NULL) || rd != libLen) {
        free(buf); CloseHandle(hFile); return;
    }
    CloseHandle(hFile);

    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* p = strrchr(exePath, '\\');
    if (p) *(p + 1) = 0;
    lstrcatA(exePath, "ExuiKrnln_Win32.lib");

    HANDLE hOut = CreateFileA(exePath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(hOut, buf, libLen, &wr, NULL);
        CloseHandle(hOut);
    }
    free(buf);
}

//==================== 静态编译用的函数名表（关键新增！） ====================
// 顺序必须和 m_pCmdsFunc / m_pBeginCmdInfo 一一对应
static const char* g_CmdFuncNames[] = { "ExuiPreInit", NULL };

//==================== 库通知函数（关键修复！） ====================
INT WINAPI ExuiPre_Notify(INT nMsg, DWORD dwParam1, DWORD dwParam2)
{
    switch (nMsg) {
    case NL_GET_CMD_FUNC_NAMES:          // 14: 告诉IDE命令实现函数的名字
        return (INT)g_CmdFuncNames;
    case NL_GET_NOTIFY_LIB_FUNC_NAME:    // 15: 告诉IDE通知函数的名字
        return (INT)"ExuiPre_Notify";
    case NL_GET_DEPENDENT_LIBS:          // 16: 无额外依赖
        return 0;
    }
    return 0;
}

//==================== 命令实现 ====================
static void Cmd_Init(PMDATA_INF pRetData, INT nArgCount, PMDATA_INF pArgInf)
{
    ReleasePayload();
}

static CMD_INFO s_CmdInfo = {
    (LPTSTR)"ExuiPre初始化",
    (LPTSTR)"ExuiPreInit",
    (LPTSTR)"必须最先调用",
    1,
    _CMD_OS(__OS_WIN),
    0,
    0,
    LVL_SIMPLE,
    0,
    0,
    0,
    NULL
};

static PFN_EXECUTE_CMD s_CmdFuncs[] = { Cmd_Init };

//==================== GetNewInf ====================
static LIB_INFO s_LibInfo;

PLIB_INFO WINAPI GetNewInf()
{
    s_LibInfo.m_dwLibFormatVer = LIB_FORMAT_VER;
    s_LibInfo.m_szGuid = (LPTSTR)"https://lsy041.com";
    s_LibInfo.m_nMajorVersion = 1;
    s_LibInfo.m_nMinorVersion = 0;
    s_LibInfo.m_nBuildNumber = 1;
    s_LibInfo.m_nRqSysMajorVer = 3;
    s_LibInfo.m_nRqSysMinorVer = 6;
    s_LibInfo.m_nRqSysKrnlLibMajorVer = 5;
    s_LibInfo.m_nRqSysKrnlLibMinorVer = 0;
    s_LibInfo.m_szName = (LPTSTR)"ExuiPre";
    s_LibInfo.m_nLanguage = __GBK_LANG_VER;
    s_LibInfo.m_szExplain = (LPTSTR)
        "ExuiPre 前置释放支持库：静态编译时自动将 Exui 支持库打包进 exe，"
        "运行时自解压、退出自清理，实现单文件交付。\r\n"
        "【注意】本插件与 UPX 等加壳软件互斥，二选一："
        "使用本插件请勿对编译出的 exe 做 UPX 加壳（加壳会使支持库无法释放，程序无法启动）；"
        "需要加壳请卸载本插件后重新编译，并将 lib\\ExuiKrnln\\ExuiKrnln_Win32.lib "
        "与 exe 放在同一目录随程序发布。";
    s_LibInfo.m_dwState = _LIB_OS(__OS_WIN);
    s_LibInfo.m_szAuthor = (LPTSTR)"041专属";
    s_LibInfo.m_szZipCode = (LPTSTR)"";
    s_LibInfo.m_szAddress = (LPTSTR)"";
    s_LibInfo.m_szPhoto = (LPTSTR)"";
    s_LibInfo.m_szFax = (LPTSTR)"";
    s_LibInfo.m_szEmail = (LPTSTR)"";
    s_LibInfo.m_szHomePage = (LPTSTR)"";
    s_LibInfo.m_szOther = (LPTSTR)"";
    s_LibInfo.m_nDataTypeCount = 0;
    s_LibInfo.m_pDataType = NULL;
    s_LibInfo.m_nCategoryCount = 1;
    s_LibInfo.m_szzCategory = (LPTSTR)"ExuiPre\0\0";
    s_LibInfo.m_nCmdCount = 1;
    s_LibInfo.m_pBeginCmdInfo = &s_CmdInfo;
    s_LibInfo.m_pCmdsFunc = s_CmdFuncs;
    s_LibInfo.m_pfnRunAddInFn = NULL;
    s_LibInfo.m_szzAddInFnInfo = NULL;
    s_LibInfo.m_pfnNotify = ExuiPre_Notify;
    s_LibInfo.m_pfnSuperTemplate = NULL;
    s_LibInfo.m_szzSuperTemplateInfo = NULL;
    s_LibInfo.m_nLibConstCount = 0;
    s_LibInfo.m_pLibConst = NULL;
    s_LibInfo.m_szzDependFiles = NULL;
    return &s_LibInfo;
}
