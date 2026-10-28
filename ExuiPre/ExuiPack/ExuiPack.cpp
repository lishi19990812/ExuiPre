// ============================================================
// ExuiPack.cpp —— 易语言静态编译“链接后动作”自动打包工具
// ------------------------------------------------------------
// 用法：
//   ExuiPack.exe <exe路径>            打包（链接后动作自动调用）
//   ExuiPack.exe --verify <exe路径>   诊断模式：只读检查，不修改任何文件
//   ExuiPack.exe                      手动模式（弹文件选择框）
// 注意：.exuicb 节内变量的排放顺序由编译器决定，不可假设，
//       回调数组和元数据槽均按特征扫描定位
// ============================================================
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

#define PAYLOAD_MAGIC 0x45555849
#define PAYLOAD_TAIL  8
#define LIB_REL_PATH  "\\lib\\ExuiKrnln\\ExuiKrnln_Win32.lib"
#define INI_NAME      "link.ini"
#define SELF_KEYWORD  "ExuiPack.exe"
#define EXUIPRE_MARK  "EXUIPRE_CLEAN"
#define CB_SECTION_NAME ".exuicb"

// ---------------- 输出总开关 ----------------
// 0 = 正常发布：只输出“编译成功/编译失败”等必要提示（发布用这个）
// 1 = 调试模式：额外输出全部诊断日志（排查问题/调参数时改 1 重编即可）
#define EXUIPACK_LOG 0

// Out/OutFmt：不受开关影响的必要输出（verify 模式、成败提示都走这里）
static void Out(const char* s)
{
    printf("[ExuiPack] %s\r\n", s);
    fflush(stdout);
}

static void OutFmt(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    printf("[ExuiPack] %s\r\n", buf);
    fflush(stdout);
}

// Log/LogFmt：诊断日志，EXUIPACK_LOG = 0 时不参与编译（零开销）
#if EXUIPACK_LOG
static void Log(const char* s) { Out(s); }
static void LogFmt(const char* fmt, ...) { OutFmt(fmt); }
#else
static void Log(const char* s) { (void)s; }
static void LogFmt(const char* fmt, ...) { (void)fmt; }
#endif

// ---------------- 基础工具 ----------------
static unsigned char* ReadWholeFile(const char* path, unsigned long* outSize)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    unsigned char* buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *outSize = (unsigned long)sz;
    return buf;
}

static const unsigned char* BinSearch(const unsigned char* hay, unsigned long hayLen, const char* needle)
{
    unsigned long nlen = (unsigned long)strlen(needle);
    if (hayLen < nlen) return NULL;
    for (unsigned long i = 0; i <= hayLen - nlen; i++) {
        unsigned long j = 0;
        while (j < nlen && hay[i + j] == (unsigned char)needle[j]) j++;
        if (j == nlen) return hay + i;
    }
    return NULL;
}

// ---------------- PE 解析 ----------------
static unsigned long RvaToOffset(const unsigned char* exe, unsigned long rva)
{
    unsigned long e_lfanew = *(const unsigned long*)(exe + 0x3C);
    const unsigned char* coff = exe + e_lfanew + 4;
    unsigned short numSections = *(const unsigned short*)(coff + 2);
    unsigned short sizeOpt = *(const unsigned short*)(coff + 16);
    const unsigned char* sec = exe + e_lfanew + 24 + sizeOpt;
    for (unsigned short i = 0; i < numSections; i++, sec += 40) {
        unsigned long va = *(const unsigned long*)(sec + 12);
        unsigned long vsz = *(const unsigned long*)(sec + 8);
        unsigned long rsz = *(const unsigned long*)(sec + 16);
        unsigned long raw = *(const unsigned long*)(sec + 20);
        unsigned long span = vsz > rsz ? vsz : rsz;
        if (span == 0) continue;
        if (rva >= va && rva < va + span) return rva - va + raw;
    }
    return 0;
}

static unsigned long FindSectionRva(const unsigned char* exe, unsigned long exeSize, const char* name)
{
    if (exeSize < 0x40 || exe[0] != 'M' || exe[1] != 'Z') return 0xFFFFFFFF;
    unsigned long e_lfanew = *(const unsigned long*)(exe + 0x3C);
    if (e_lfanew == 0 || e_lfanew + 24 > exeSize) return 0xFFFFFFFF;
    const unsigned char* coff = exe + e_lfanew + 4;
    unsigned short numSections = *(const unsigned short*)(coff + 2);
    unsigned short sizeOpt = *(const unsigned short*)(coff + 16);
    const unsigned char* sec = exe + e_lfanew + 24 + sizeOpt;
    size_t nlen = strlen(name);
    for (unsigned short i = 0; i < numSections; i++, sec += 40) {
        if (sec + 40 > exe + exeSize) break;
        if (memcmp(sec, name, nlen) == 0 && sec[nlen] == 0)
            return *(const unsigned long*)(sec + 12);
    }
    return 0xFFFFFFFF;
}

// 判断 VA 是否落在镜像内某个“可执行节”里（回调指针必须是代码地址）
static int IsCodeVa(const unsigned char* exe, unsigned long exeSize, unsigned long imgBase, unsigned long va)
{
    if (va < imgBase || va - imgBase > exeSize) return 0;
    unsigned long rva = va - imgBase;
    unsigned long e_lfanew = *(const unsigned long*)(exe + 0x3C);
    const unsigned char* coff = exe + e_lfanew + 4;
    unsigned short numSections = *(const unsigned short*)(coff + 2);
    unsigned short sizeOpt = *(const unsigned short*)(coff + 16);
    const unsigned char* sec = exe + e_lfanew + 24 + sizeOpt;
    for (unsigned short i = 0; i < numSections; i++, sec += 40) {
        unsigned long sva = *(const unsigned long*)(sec + 12);
        unsigned long vsz = *(const unsigned long*)(sec + 8);
        unsigned long flags = *(const unsigned long*)(sec + 36);
        if (vsz && rva >= sva && rva < sva + vsz && (flags & 0x20000000))
            return 1; // IMAGE_SCN_MEM_EXECUTE
    }
    return 0;
}

// 在 .exuicb 里定位回调数组槽位 { 代码指针, NULL }
// 节内变量顺序编译器说了算（实测元数据可能排在数组前），不能假设数组在节首
// 特征：槽位值非 0、指向可执行节、下一个槽位为 0
// 返回该槽位的文件偏移；找不到返回 0
static unsigned long FindCallbackSlot(const unsigned char* exe, unsigned long exeSize,
    unsigned long imgBase, unsigned long cbRva)
{
    unsigned long arrOff = RvaToOffset(exe, cbRva);
    if (arrOff == 0) return 0;
    for (unsigned long o = arrOff; o + 8 <= arrOff + 64; o += 4) {
        unsigned long v = *(const unsigned long*)(exe + o);
        if (v == 0) continue;
        if (*(const unsigned long*)(exe + o + 4) != 0) continue;
        if (!IsCodeVa(exe, exeSize, imgBase, v)) continue;
        return o;
    }
    return 0;
}

// 定位 .exuicb 里的元数据槽（magic 所在文件偏移）
static int FindMeta(const unsigned char* exe, unsigned long exeSize, unsigned long* metaOff)
{
    unsigned long cbRva = FindSectionRva(exe, exeSize, CB_SECTION_NAME);
    if (cbRva == 0xFFFFFFFF) return 0;
    unsigned long arrOff = RvaToOffset(exe, cbRva);
    if (arrOff == 0 || arrOff + 64 > exeSize) return 0;
    for (unsigned long o = arrOff; o + 8 <= arrOff + 64; o += 4) {
        if (*(const unsigned long*)(exe + o) == PAYLOAD_MAGIC) { *metaOff = o; return 1; }
    }
    return 0;
}

// ---------------- verify 诊断模式（只读，不受输出开关影响） ----------------
static int VerifyMode(const char* exePath)
{
    unsigned long exeSize = 0;
    unsigned char* exe = ReadWholeFile(exePath, &exeSize);
    if (!exe) { Out("verify 错误：无法读取文件"); return 1; }
    OutFmt("verify 目标：%s（%lu 字节）", exePath, exeSize);

    unsigned long e_lfanew = *(const unsigned long*)(exe + 0x3C);
    if (e_lfanew == 0 || e_lfanew + 24 + 96 + 80 > exeSize || exe[e_lfanew] != 'P' || exe[e_lfanew + 1] != 'E') {
        Out("verify 错误：PE 头不完整");
        free(exe); return 1;
    }
    const unsigned char* opt = exe + e_lfanew + 24;
    if (*(const unsigned short*)opt != 0x10B) { Out("verify 错误：非 PE32"); free(exe); return 1; }

    unsigned long imgBase = *(const unsigned long*)(opt + 28);
    unsigned long certOff = *(const unsigned long*)(opt + 96 + 4 * 8);
    unsigned long certSize = *(const unsigned long*)(opt + 96 + 4 * 8 + 4);
    if (certSize) OutFmt("签名：有（证书块 文件偏移 %lu，大小 %lu）", certOff, certSize);
    else          Out("签名：无");

    unsigned long tlsRva = *(const unsigned long*)(opt + 96 + 9 * 8);
    if (tlsRva == 0) { Out("verify 错误：无 TLS 目录"); free(exe); return 1; }
    unsigned long tlsOff = RvaToOffset(exe, tlsRva);
    if (tlsOff == 0 || tlsOff + 24 > exeSize) { Out("verify 错误：TLS 目录无法映射"); free(exe); return 1; }
    unsigned long cbVa = *(const unsigned long*)(exe + tlsOff + 12);

    unsigned long cbRva = FindSectionRva(exe, exeSize, CB_SECTION_NAME);
    if (cbRva == 0xFFFFFFFF) {
        Out("verify 错误：没有 .exuicb 节（可能已被 UPX 压缩，或 lib 版本过旧）");
        free(exe); return 1;
    }
    unsigned long arrOff = RvaToOffset(exe, cbRva);
    OutFmt(".exuicb：RVA %08lX，文件偏移 %08lX", cbRva, arrOff);
    if (arrOff + 16 <= exeSize) {
        OutFmt(".exuicb 前 16 字节：%08lX %08lX %08lX %08lX",
            *(const unsigned long*)(exe + arrOff),
            *(const unsigned long*)(exe + arrOff + 4),
            *(const unsigned long*)(exe + arrOff + 8),
            *(const unsigned long*)(exe + arrOff + 12));
    }

    // 判定1：TLS 指向真正的回调数组（特征扫描，不假设节内顺序）
    unsigned long cbSlot = FindCallbackSlot(exe, exeSize, imgBase, cbRva);
    if (cbSlot == 0) {
        Out("判定1：.exuicb 里找不到 { 代码指针, NULL } 回调数组 ✗（节内容异常）");
    }
    else {
        unsigned long slotRva = cbRva + (cbSlot - arrOff);
        unsigned long expectVa = imgBase + slotRva;
        OutFmt("判定1：回调数组槽位在文件偏移 %08lX（RVA %08lX），期望 VA = %08lX", cbSlot, slotRva, expectVa);
        if (cbVa == expectVa)
            Out("判定1：TLS AddressOfCallBacks 正确指向回调数组 ✓");
        else
            OutFmt("判定1：TLS AddressOfCallBacks = %08lX ≠ 期望 %08lX ✗（补丁丢失/被改写/指错位置）", cbVa, expectVa);
    }

    // 判定2：元数据埋点
    unsigned long metaOff = 0;
    if (!FindMeta(exe, exeSize, &metaOff)) {
        Out("判定2：.exuicb 里没找到元数据槽 ✗（lib 版本过旧？）");
    }
    else {
        unsigned long m0 = *(const unsigned long*)(exe + metaOff);
        unsigned long m1 = *(const unsigned long*)(exe + metaOff + 4);
        if (m0 != PAYLOAD_MAGIC) {
            Out("判定2：元数据 magic 不对 ✗");
        }
        else if (m1 == 0) {
            Out("判定2：元数据槽存在但未埋点（偏移=0）——该 exe 未被打包 ✗");
        }
        else if (m1 >= exeSize) {
            OutFmt("判定2：埋点偏移 %lu 越界（文件只有 %lu）✗（若是 UPX 后的文件，此告警属正常，以判定3为准）", m1, exeSize);
        }
        else {
            int ok = (m1 >= PAYLOAD_TAIL) && (*(const unsigned long*)(exe + m1) == PAYLOAD_MAGIC);
            OutFmt("判定2：已埋点，payload 尾部偏移 = %lu，magic 复核%s", m1, ok ? "通过 ✓" : "失败 ✗（该位置不是 payload！）");
        }
    }

    // 判定3：payload 标记位置（签名时贴证书块前面，未签名时贴文件尾）
    if (certSize && certOff >= PAYLOAD_TAIL && certOff <= exeSize) {
        if (*(const unsigned long*)(exe + certOff - PAYLOAD_TAIL) == PAYLOAD_MAGIC)
            OutFmt("判定3：证书块前紧贴 payload 标记，libLen = %lu ✓", *(const unsigned long*)(exe + certOff - 4));
        else
            Out("判定3：证书块前没有 payload 标记（未打包就签名了？）");
    }
    else if (exeSize > PAYLOAD_TAIL && *(const unsigned long*)(exe + exeSize - PAYLOAD_TAIL) == PAYLOAD_MAGIC) {
        OutFmt("判定3：文件尾是 payload 标记，libLen = %lu ✓", *(const unsigned long*)(exe + exeSize - 4));
    }
    else {
        Out("判定3：未签名且文件尾没有 payload 标记（未打包）");
    }

    Out("verify 完成（文件未被修改）");
    free(exe);
    return 0;
}

// ---------------- TLS 修补 + 埋点 ----------------
static int PatchTls(unsigned char* exe, unsigned long exeSize, unsigned long libSize)
{
    if (exeSize < 0x40 || exe[0] != 'M' || exe[1] != 'Z') { Log("错误：无 MZ 头，不是有效 PE"); return 1; }
    unsigned long e_lfanew = *(const unsigned long*)(exe + 0x3C);
    if (e_lfanew == 0 || e_lfanew + 24 + 96 + 80 > exeSize) { Log("错误：PE 头不完整"); return 1; }
    const unsigned char* pe = exe + e_lfanew;
    if (pe[0] != 'P' || pe[1] != 'E') { Log("错误：PE 签名错误"); return 1; }
    unsigned short machine = *(const unsigned short*)(pe + 4);
    if (machine != 0x014C) { Log("错误：非 32 位 PE"); return 1; }
    const unsigned char* opt = pe + 24;
    if (*(const unsigned short*)opt != 0x10B) { Log("错误：非 PE32 可选头"); return 1; }

    unsigned long tlsRva = *(const unsigned long*)(opt + 96 + 9 * 8);
    unsigned long tlsSize = *(const unsigned long*)(opt + 96 + 9 * 8 + 4);
    if (tlsRva == 0 || tlsSize == 0) { Log("错误：exe 无 TLS 目录（/INCLUDE:__tls_used 没生效？）"); return 1; }

    unsigned long tlsOff = RvaToOffset(exe, tlsRva);
    if (tlsOff == 0 || tlsOff + 24 > exeSize) { Log("错误：TLS 目录 RVA 无法映射到文件偏移"); return 1; }

    unsigned long imgBase = *(const unsigned long*)(opt + 28);
    unsigned long cbRva = FindSectionRva(exe, exeSize, CB_SECTION_NAME);
    if (cbRva == 0xFFFFFFFF) {
        Log("错误：exe 里没有 .exuicb 节（ExuiPre_static.lib 版本过旧，或已被 UPX 压缩）");
        Log("  请先更新 ExuiPre_static.cpp 并重编 ExuiPre_static.lib，再重编本项目");
        return 1;
    }

    // 1) 特征扫描定位回调数组槽位（不假设节内顺序！）
    unsigned long cbSlot = FindCallbackSlot(exe, exeSize, imgBase, cbRva);
    if (cbSlot == 0) { Log("错误：.exuicb 里找不到 { 代码指针, NULL } 回调数组，节内容异常"); return 1; }

    unsigned long arrOff = RvaToOffset(exe, cbRva);
    unsigned long slotRva = cbRva + (cbSlot - arrOff);
    unsigned long newVa = imgBase + slotRva;
    unsigned long oldVa = *(const unsigned long*)(exe + tlsOff + 12);
    if (oldVa == newVa) {
        Log("TLS 已指向回调数组，无需修补");
    }
    else {
        // 看一眼原数组（诊断信息）
        int oldCnt = 0;
        if (oldVa && oldVa >= imgBase && oldVa - imgBase < exeSize) {
            unsigned long o = RvaToOffset(exe, oldVa - imgBase);
            while (o && oldCnt < 64 && o + 4 * (oldCnt + 1) <= exeSize) {
                if (*(const unsigned long*)(exe + o + oldCnt * 4) == 0) break;
                oldCnt++;
            }
        }
        if (oldCnt > 0)
            LogFmt("注意：原回调数组有 %d 个回调，改向后它们将不再执行", oldCnt);
        *(unsigned long*)(exe + tlsOff + 12) = newVa;
        LogFmt("TLS 已修补：AddressOfCallBacks %08lX -> %08lX（指向回调数组，文件偏移 %08lX）",
            oldVa, newVa, cbSlot);
    }

    // 2) 埋点
    unsigned long metaOff = 0;
    if (!FindMeta(exe, exeSize, &metaOff)) {
        Log("错误：.exuicb 里没有元数据槽（ExuiPre_static.lib 版本过旧）");
        Log("  请先更新 ExuiPre_static.cpp 并重编 ExuiPre_static.lib，再重编本项目");
        return 1;
    }
    unsigned long tailPos = exeSize + libSize;
    *(unsigned long*)(exe + metaOff + 4) = tailPos;
    LogFmt("已埋点：payload 尾部偏移 = %lu (0x%08lX)，运行期按此定位", tailPos, tailPos);
    Log("抢跑+看门娃机制就位");
    return 0;
}

// ---------------- link.ini 自注册 ----------------
static void SelfRegister(void)
{
    char self[MAX_PATH], dir[MAX_PATH], iniPath[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    lstrcpyA(dir, self);
    char* p = strrchr(dir, '\\');
    if (!p) return;
    *p = 0;
    wsprintfA(iniPath, "%s\\%s", dir, INI_NAME);

    FILE* f = fopen(iniPath, "rb");
    if (!f) { LogFmt("未找到 %s，跳过自注册", iniPath); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f); Log("link.ini 尺寸异常，跳过自注册"); return;
    }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return; }
    fclose(f);
    buf[sz] = 0;

    if (strstr(buf, SELF_KEYWORD)) {
        Log("link.ini 配置检查：已包含 ExuiPack，跳过注册");
        free(buf);
        return;
    }

    int maxIdx = 0;
    char* line = buf;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) *next = 0;
        char* s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (*s != ';') {
            char* k = strstr(s, "post_link_action");
            if (k) {
                k += 16;
                int idx = 0;
                while (*k >= '0' && *k <= '9') { idx = idx * 10 + (*k - '0'); k++; }
                if (idx > maxIdx) maxIdx = idx;
            }
        }
        line = next ? next + 1 : NULL;
    }
    free(buf);

    int myIdx = maxIdx + 1;
    if (myIdx > 100) { Log("post_link_action 编号已达上限 100，无法注册"); return; }

    FILE* fa = fopen(iniPath, "ab");
    if (!fa) { Log("错误：无法写入 link.ini（只读？）"); return; }
    fprintf(fa, "\r\npost_link_action%d=\"%s\" $(TARGET)\r\n", myIdx, self);
    fclose(fa);
    LogFmt("已自动注册 link.ini -> post_link_action%d=\"%s\" $(TARGET)", myIdx, self);
}

// ---------------- 定位官方静态库 ----------------
static int LocateLib(char* out, size_t cch)
{
    char self[MAX_PATH], root[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    lstrcpyA(root, self);
    char* p = strrchr(root, '\\');
    if (!p) return 0;
    *p = 0;
    p = strrchr(root, '\\');
    if (!p) return 0;
    *p = 0;
    _snprintf(out, cch - 1, "%s%s", root, LIB_REL_PATH);
    out[cch - 1] = 0;
    DWORD attr = GetFileAttributesA(out);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

// ---------------- 核心打包 ----------------
// 返回：0 = 打包成功；1 = 跳过（未使用 Exui 支持 / 已打包 / 已签名）；2 = 失败
static int PackFile(const char* exePath, const char* libPath)
{
    LogFmt("目标 exe：%s", exePath);

    unsigned long exeSize = 0, libSize = 0;
    unsigned char* exe = ReadWholeFile(exePath, &exeSize);
    if (!exe) {
        // 大 exe 可能被杀软短暂锁定，重试 5 秒
        for (int t = 0; t < 10 && !exe; t++) {
            Sleep(500);
            exe = ReadWholeFile(exePath, &exeSize);
        }
        if (!exe) { Log("错误：无法读取目标 exe（重试 5 秒仍失败）"); return 2; }
    }

    if (exeSize > PAYLOAD_TAIL) {
        if (*(const unsigned long*)(exe + exeSize - PAYLOAD_TAIL) == PAYLOAD_MAGIC) {
            Log("该 exe 尾部已有 payload 标记，判断为已打包，跳过");
            free(exe); return 1;
        }
    }

    if (!BinSearch(exe, exeSize, EXUIPRE_MARK)) {
        Log("未检测到 ExuiPre_static（本项目未使用 Exui 支持），跳过打包");
        free(exe); return 1;
    }

    {
        unsigned long metaOff = 0;
        if (FindMeta(exe, exeSize, &metaOff) && *(const unsigned long*)(exe + metaOff + 4) != 0) {
            Log("该 exe 元数据已埋点（偏移非零），判断为已打包，跳过");
            free(exe); return 1;
        }
    }

    {
        unsigned long e_lfanew = *(const unsigned long*)(exe + 0x3C);
        if (e_lfanew && e_lfanew + 24 + 96 + 40 <= exeSize && exe[e_lfanew] == 'P' && exe[e_lfanew + 1] == 'E') {
            const unsigned char* opt = exe + e_lfanew + 24;
            if (*(const unsigned short*)opt == 0x10B) {
                unsigned long certSize = *(const unsigned long*)(opt + 96 + 4 * 8 + 4);
                if (certSize != 0) {
                    Log("该 exe 已带签名：若是打包后签的名，属正常流程，跳过打包");
                    Log("  若尚未打包，请重新编译（自动打包）后再签名");
                    free(exe); return 1;
                }
            }
        }
    }

    unsigned char* lib = ReadWholeFile(libPath, &libSize);
    if (!lib) {
        LogFmt("错误：无法读取静态库：%s", libPath);
        free(exe); return 2;
    }
    LogFmt("静态库：%s（%lu 字节）", libPath, libSize);

    if (PatchTls(exe, exeSize, libSize)) { free(exe); free(lib); return 2; }

    unsigned long total = exeSize + libSize + PAYLOAD_TAIL;
    unsigned char* out = (unsigned char*)malloc((size_t)total);
    if (!out) { Log("错误：内存不足"); free(exe); free(lib); return 2; }
    memcpy(out, exe, exeSize);
    memcpy(out + exeSize, lib, libSize);
    *(unsigned long*)(out + exeSize + libSize) = PAYLOAD_MAGIC;
    *(unsigned long*)(out + exeSize + libSize + 4) = libSize;
    free(exe);
    free(lib);

    // 写回：大 exe 写盘瞬间可能被杀软独占，重试 5 秒
    FILE* f = NULL;
    for (int t = 0; t < 10 && !f; t++) {
        f = fopen(exePath, "wb");
        if (!f) Sleep(500);
    }
    if (!f) {
        Log("错误：无法写回目标 exe（重试 5 秒仍被占用，大概率是杀软在扫描）");
        free(out); return 2;
    }
    size_t w = fwrite(out, 1, (size_t)total, f);
    fclose(f);
    free(out);
    if (w != (size_t)total) { Log("错误：写回不完整！"); return 2; }

    LogFmt("打包完成：%s（追加 payload %lu 字节）", exePath, libSize + PAYLOAD_TAIL);
    return 0;
}

// ---------------- 手动模式 ----------------
static int ManualPick(char* exePath)
{
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    exePath[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "exe 文件\0*.exe\0所有文件\0*.*\0";
    ofn.lpstrFile = exePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    ofn.lpstrTitle = "ExuiPack - 选择要打包的 exe";
    return GetOpenFileNameA(&ofn);
}

// ---------------- 入口 ----------------
int main(int argc, char** argv)
{
    Out("v1.6 —— 编译后自动打包（输出精简版，诊断日志见 EXUIPACK_LOG 开关）");

    if (argc >= 3 && (lstrcmpiA(argv[1], "--verify") == 0 || lstrcmpiA(argv[1], "/verify") == 0)) {
        return VerifyMode(argv[2]);
    }

    SelfRegister();

    char exePath[MAX_PATH];
    if (argc >= 2 && argv[1] && argv[1][0]) {
        lstrcpynA(exePath, argv[1], MAX_PATH);
    }
    else {
        Log("未带参数：手动模式，请选择要打包的 exe（取消则退出）");
        if (!ManualPick(exePath)) return 0;
    }

    char libPath[MAX_PATH];
    if (!LocateLib(libPath, MAX_PATH)) {
        Log("错误：未找到官方静态库，应位于：易语言根\\lib\\ExuiKrnln\\ExuiKrnln_Win32.lib");
        Log("（本工具必须放在 易语言\\tools\\ 目录下才能定位易语言根）");
        Out("编译失败，请手动删除编译的文件重新编译！");
        return 2;
    }

    int r = PackFile(exePath, libPath);

    if (r == 0) {
        Out("编译成功\r\n"
            "【重要】单文件模式与 UPX 加壳互斥，二选一：\r\n"
            "◆ 使用本插件（推荐）：不要对 exe 做 UPX 加壳，直接签名即可发布。\r\n"
            "  （UPX 后再手动运行 ExuiPack 也无效——释放机制随 exe 一并被压缩，程序无法启动）\r\n"
            "◆ 需要 UPX 加壳：请勿安装本插件（卸载后重新编译），\r\n"
            "  将 lib\\ExuiKrnln\\ExuiKrnln_Win32.lib 与 exe 放在同一目录随程序发布。");
    }
    else if (r == 1) {
        Out("跳过打包（未使用 Exui 支持，或 exe 已打包/已签名）");
    }
    else {
        Out("编译失败，请手动删除编译的文件重新编译！");
    }
    return r;
}
