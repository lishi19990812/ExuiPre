//EXUI前置文件，编译为：ExuiPre_static.lib
#define __GCC_
#include <windows.h>
#include "lib2.h"

#define PAYLOAD_MAGIC 0x45555849
#define PAYLOAD_TAIL  8
#define LIB_NAME      "ExuiKrnln_Win32.lib"
#define CLEAN_TOKEN   "EXUIPRE_CLEAN"
#define LIB_ATTRS     (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY)
#define INSTANCE_MUTEX "ExuiPre_Instance_Mutex"

// 加到文件顶部 define 区：
#define SCAN_BACK_MAX  (16 * 1024 * 1024)   // 从文件尾向前扫描 magic 的上限
#define SCAN_CHUNK     (64 * 1024)

// ---- 黑匣子总开关：0 = 关闭日志输出（保留代码备用），1 = 开启 ----
// 以后需要检验运行时序时改回 1 重编即可
#define EXUIPRE_LOG 0

// 节定义必须出现在所有 allocate 引用之前（含下方的前置声明）
#pragma section(".exuicb", long, read)

// 前置声明：.exuicb 元数据槽定义在文件末尾，ReleasePayload 要先用
extern "C" __declspec(allocate(".exuicb")) volatile unsigned long pExuiPreTlsMeta[2];

static int g_isCleaner = 0;
static int g_cleanerSpawned = 0;
static HANDLE g_hInstanceMutex = NULL;

// ---- 黑匣子：测试用，总开关关闭时静默返回，代码保留备用 ----
static void Mark(const char* tag)
{
#if EXUIPRE_LOG
    char logPath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, logPath, MAX_PATH);
    char* pLast = NULL;
    for (char* q = logPath; *q; q++)
        if (*q == '\\') pLast = q;
    if (pLast) *(pLast + 1) = 0;
    lstrcatA(logPath, "_exuipre_log.txt");
    HANDLE h = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, tag, lstrlenA(tag), &w, NULL);
        WriteFile(h, "\r\n", 2, &w, NULL);
        CloseHandle(h);
    }
#else
    (void)tag;   // 关闭时消除“未使用参数”警告
#endif
}

// ---- 路径工具 ----
static void GetExeDir(char* out)
{
    GetModuleFileNameA(NULL, out, MAX_PATH);
    char* pLast = NULL;
    for (char* q = out; *q; q++)
        if (*q == '\\') pLast = q;
    if (pLast) *(pLast + 1) = 0;
}

static void GetLibPath(char* out)
{
    GetExeDir(out);
    lstrcatA(out, LIB_NAME);
}

// ---- 手写子串查找（零CRT），返回 token 后面的位置，没找到返回 NULL ----
static const char* FindToken(const char* cl)
{
    const char* tok = CLEAN_TOKEN;
    for (const char* p = cl; *p; p++) {
        const char* a = p;
        const char* b = tok;
        while (*b && *a == *b) { a++; b++; }
        if (!*b) return a;
    }
    return NULL;
}

// ---- 尝试删除 lib（关键改进）----
// DeleteFile 只被"只读"位阻挡，隐藏/系统位不阻挡删除。
// 所以只清只读位，隐藏+系统全程保留 → lib 永远不会"裸奔"变完全可见。
// 删除失败时恢复只读位。返回 1=已删除 0=删除失败
static int TryDeleteLib(const char* libPath)
{
    DWORD a = GetFileAttributesA(libPath);
    if (a == INVALID_FILE_ATTRIBUTES) return 1;   // 本来就不存在 = 目的达成
    SetFileAttributesA(libPath, a & ~FILE_ATTRIBUTE_READONLY);
    if (DeleteFileA(libPath)) return 1;
    SetFileAttributesA(libPath, a | FILE_ATTRIBUTE_READONLY);   // 失败恢复只读
    return 0;
}

// ---- 看门娃模式：等主进程退出 → 删除 lib ----
static void DoCleanupMode(const char* afterTok)
{
    const char* p = afterTok;
    while (*p == ' ') p++;

    unsigned long pid = 0;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (*p - '0');
        p++;
    }

    while (*p && *p != '"') p++;
    if (*p != '"') ExitProcess(0);
    p++;
    char path[MAX_PATH];
    int i = 0;
    while (*p && *p != '"' && i < MAX_PATH - 1) path[i++] = *p++;
    path[i] = 0;

    HANDLE hProc = NULL;
    if (pid) hProc = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (hProc) {
        WaitForSingleObject(hProc, INFINITE);
        CloseHandle(hProc);
    }

    // 主进程已退出。三种情况：
    // 1) lib 已不存在 → 立即退出
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        ExitProcess(0);

    // 2) 还有别的 Exui 实例在运行 → lib 归它管，立即退出不抢删
    HANDLE hOther = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, INSTANCE_MUTEX);
    if (hOther) {
        CloseHandle(hOther);
        ExitProcess(0);
    }

    // 3) 正常清理：只清只读位（保持隐藏），上限约 15 秒
    for (int t = 0; t < 30; t++) {
        if (TryDeleteLib(path)) break;
        Sleep(500);
    }
    ExitProcess(0);
}

// ---- 释放 payload 为隐藏文件 ----
// 返回 1 = 本次成功释放；-1 = lib 已存在且被锁定（残留）；0 = 无 payload/损

// ---- 从 certStart 向前扫描 payload 尾标记 ----
// 找到返回 1，*outTail = magic 所在文件偏移，*outLen = lib 长度
static int FindPayloadTail(HANDLE hFile, DWORD certStart, DWORD* outTail, DWORD* outLen)
{
    if (certStart < 8 + 1024) return 0;
    DWORD limit = (certStart > SCAN_BACK_MAX) ? (certStart - SCAN_BACK_MAX) : 0;
    static BYTE buf[SCAN_CHUNK];
    DWORD windowEnd = certStart;   // 本窗口只看 < windowEnd 的字节

    while (windowEnd > limit) {
        DWORD chunkStart = (windowEnd - limit > SCAN_CHUNK) ? (windowEnd - SCAN_CHUNK) : limit;
        DWORD toRead = windowEnd - chunkStart;
        SetFilePointer(hFile, chunkStart, NULL, FILE_BEGIN);
        DWORD rd = 0;
        if (!ReadFile(hFile, buf, toRead, &rd, NULL) || rd != toRead) return 0;

        // 从高往低找：最靠近 certStart 的 magic 就是 payload 尾
        for (DWORD i = toRead - 8 + 1; i-- > 0; ) {
            if (*(DWORD*)(buf + i) == PAYLOAD_MAGIC) {
                DWORD abs = chunkStart + i;
                DWORD libLen = *(DWORD*)(buf + i + 4);
                if (libLen >= 1024 && libLen <= abs && libLen < SCAN_BACK_MAX) {
                    *outTail = abs;
                    *outLen = libLen;
                    return 1;
                }
            }
        }
        // 未命中：窗口下移（重叠 7 字节，防止标记跨块边界被切漏）
        windowEnd = (chunkStart + 7 > windowEnd) ? limit : chunkStart + 7;
    }
    return 0;
}

// ---- 释放 payload 为隐藏文件（v1.9：动态定位，兼容 UPX）----
// 返回 1 = 本次成功释放；-1 = lib 已存在且被锁定（残留）；0 = 无 payload/损坏
static int ReleasePayload(const char* libPath)
{
    if (!TryDeleteLib(libPath)) {
        Mark("6: leftover locked (another instance?), keep");
        return -1;
    }

    // meta[0] 是编译期初值，UPX 压缩不影响它 —— 只当"已打包"标志用
    if (pExuiPreTlsMeta[0] != PAYLOAD_MAGIC) {
        Mark("X: not packed");
        return 0;
    }

    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, path, MAX_PATH);

    HANDLE hFile = INVALID_HANDLE_VALUE;
    for (int t = 0; t < 20; t++) {
        hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile != INVALID_HANDLE_VALUE) break;
        Sleep(500);
    }
    if (hFile == INVALID_HANDLE_VALUE) {
        Mark("X: open self fail (retried 10s)");
        return 0;
    }

    DWORD fileSize = GetFileSize(hFile, NULL);

    // 定位证书块起点（PE 数据目录[4]，未签名则视为文件尾）
    DWORD certStart = fileSize;
    IMAGE_DOS_HEADER dos;
    SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
    DWORD rd = 0;
    if (ReadFile(hFile, &dos, sizeof(dos), &rd, NULL) && rd == sizeof(dos)
        && dos.e_magic == IMAGE_DOS_SIGNATURE
        && (DWORD)dos.e_lfanew + 256 <= fileSize) {
        BYTE nt[256];
        SetFilePointer(hFile, dos.e_lfanew, NULL, FILE_BEGIN);
        if (ReadFile(hFile, nt, sizeof(nt), &rd, NULL) && rd == sizeof(nt)
            && *(DWORD*)nt == 0x00004550                      // "PE\0\0"
            && *(WORD*)(nt + 24) == 0x010B) {                 // PE32
            DWORD certOff = *(DWORD*)(nt + 24 + 96 + 4 * 8);  // DataDirectory[4]
            if (certOff != 0 && certOff < fileSize) certStart = certOff;
        }
    }

    // 向前扫描 payload 尾标记（不再依赖 ExuiPack 埋的固定偏移）
    DWORD tailPos = 0, libLen = 0;
    if (!FindPayloadTail(hFile, certStart, &tailPos, &libLen)) {
        CloseHandle(hFile);
        Mark("X: payload tail not found (repacked/stripped?)");
        return 0;
    }

    HANDLE hHeap = GetProcessHeap();
    char* buf2 = (char*)HeapAlloc(hHeap, 0, libLen);
    if (!buf2) { CloseHandle(hFile); Mark("X: alloc fail"); return 0; }

    SetFilePointer(hFile, tailPos - libLen, NULL, FILE_BEGIN);
    if (!ReadFile(hFile, buf2, libLen, &rd, NULL) || rd != libLen) {
        HeapFree(hHeap, 0, buf2); CloseHandle(hFile); Mark("X: read lib fail"); return 0;
    }
    CloseHandle(hFile);

    HANDLE hOut = CreateFileA(libPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, NULL);
    if (hOut == INVALID_HANDLE_VALUE) {
        HeapFree(hHeap, 0, buf2); Mark("X: write fail"); return 0;
    }
    DWORD wr = 0;
    WriteFile(hOut, buf2, libLen, &wr, NULL);
    CloseHandle(hOut);
    HeapFree(hHeap, 0, buf2);

    SetFileAttributesA(libPath, LIB_ATTRS);
    Mark("5: lib released (hidden+sys+ro)");
    return 1;
}


// ---- 拉起看门娃：exe 自身 + 暗号 ----
static void SpawnCleaner(const char* libPath)
{
    if (g_cleanerSpawned) return;

    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);

    char cmd[MAX_PATH * 2 + 64];
    char* c = cmd;
    *c++ = '"';
    for (char* s = self; *s && c < cmd + sizeof(cmd) - 1; ) *c++ = *s++;
    *c++ = '"';
    *c++ = ' ';
    for (const char* s = CLEAN_TOKEN; *s; ) *c++ = *s++;
    *c++ = ' ';

    char pidbuf[16];
    int n = 0;
    unsigned long pid = GetCurrentProcessId();
    do { pidbuf[n++] = (char)('0' + pid % 10); pid /= 10; } while (pid);
    while (n) *c++ = pidbuf[--n];

    *c++ = ' ';
    *c++ = '"';
    for (const char* s = libPath; *s && c < cmd + sizeof(cmd) - 1; ) *c++ = *s++;
    *c++ = '"';
    *c = 0;

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        g_cleanerSpawned = 1;
        Mark("7: cleaner spawned");
    }
    else {
        Mark("7: cleaner spawn FAILED");
    }
}

// ==================== TLS 回调 ====================
static void ExuiPreTlsProc(void* hModule, unsigned long reason, void* reserved)
{
    if (reason == 1) {   // PROCESS_ATTACH：抢在一切之前
        const char* afterTok = FindToken(GetCommandLineA());
        if (afterTok) {
            g_isCleaner = 1;
            DoCleanupMode(afterTok);   // 内部 ExitProcess，绝不创建互斥体
            return;
        }

        g_hInstanceMutex = CreateMutexA(NULL, TRUE, INSTANCE_MUTEX);

        Mark("0: early release (TLS)");
        char libPath[MAX_PATH];
        GetLibPath(libPath);
        if (ReleasePayload(libPath) != 0)
            SpawnCleaner(libPath);
    }
    else if (reason == 0 && !g_isCleaner) {
        char libPath[MAX_PATH];
        GetLibPath(libPath);

        if (g_hInstanceMutex) {
            ReleaseMutex(g_hInstanceMutex);
            CloseHandle(g_hInstanceMutex);
            g_hInstanceMutex = NULL;
        }

        // 还有别的实例在用 lib → 不删（隐藏属性原样保留），删除交给它的看门娃
        HANDLE hOther = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, INSTANCE_MUTEX);
        if (hOther) {
            CloseHandle(hOther);
            return;
        }

        // 自己删：只清只读位（保持隐藏），快速重试约 1.5 秒保退出速度
        for (int t = 0; t < 5; t++) {
            if (TryDeleteLib(libPath)) return;
            Sleep(300);
        }
        // 删不掉就留着（隐藏+系统+只读，不可见），下次启动或看门娃兜底清理
    }
}

#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_pExuiPreTlsCb")

// ---- .exuicb 节：回调数组 + 元数据槽 ----
// 注意：MSVC 不保证节内多个变量的排放顺序，ExuiPack 靠特征扫描定位
#pragma section(".exuicb", long, read)
typedef void (*EXUI_TLS_CB)(void*, unsigned long, void*);
extern "C" __declspec(allocate(".exuicb")) const EXUI_TLS_CB pExuiPreTlsCb[] = { ExuiPreTlsProc, 0 };
extern "C" __declspec(allocate(".exuicb")) volatile unsigned long pExuiPreTlsMeta[2] = { PAYLOAD_MAGIC, 0 };

// ==================== 命令实现 ====================
extern "C" void ExuiPreInit(PMDATA_INF pRetData, INT nArgCount, PMDATA_INF pArgInf)
{
    Mark("1: ExuiPreInit called");
    char libPath[MAX_PATH];
    GetLibPath(libPath);
    if (ReleasePayload(libPath) != 0)
        SpawnCleaner(libPath);
}

extern "C" INT WINAPI ExuiPre_Notify(INT nMsg, DWORD dwParam1, DWORD dwParam2)
{
    return 0;
}
