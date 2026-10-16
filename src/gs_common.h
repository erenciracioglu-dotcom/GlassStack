// GlassStack - definitions shared by both processes:
//   resident  (GlassStack.exe)           sampler + taskbar strip + tray + hotkey, never loads a graphics driver
//   viewer    (GlassStack.exe --viewer)  3D overview + section window, started on demand, exits when idle
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <winioctl.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wlanapi.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwrite.h>
#include <wincodec.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>
#include <limits.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <atomic>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dcomp.lib")

#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "delayimp.lib")

#define APP_NAME     L"GlassStack"
#define APP_VERSION  L"1.0"
#define SHARED_NAME  L"Local\\GlassStack.Shared.v1"
#define MUTEX_NAME   L"Local\\GlassStack.Resident.v1"
#define CLS_RESIDENT L"GlassStackResident"
#define CLS_VIEWER   L"GlassStackViewerCtl"

#define WM_GS_SAMPLE (WM_APP + 1)   // new sample available
#define WM_GS_TRAY   (WM_APP + 2)   // tray icon callback
#define WM_GS_CMD    (WM_APP + 3)   // resident -> viewer: wParam = CMD_*, lParam = argument
#define WM_GS_APPBAR (WM_APP + 4)   // appbar notifications for the strip
#define WM_GS_VIEWER (WM_APP + 5)   // viewer -> resident: wParam = VEV_*, lParam = viewer control HWND
#define WM_GS_DETAIL (WM_APP + 6)   // viewer detail thread -> viewer UI

enum { CMD_HOLD = 1, CMD_OVERVIEW, CMD_SECTION, CMD_QUIT, CMD_RELOAD };
enum { VEV_READY = 1, VEV_EXIT, VEV_SECTION, VEV_READY_OV, VEV_STRIP };   // VEV_SECTION: the overview asks for a section (lParam = graph)

#define GX(lp) ((int)(short)LOWORD(lp))
#define GY(lp) ((int)(short)HIWORD(lp))
static const double GIB = 1024.0 * 1024.0 * 1024.0;

enum GraphId { G_MEM, G_GPUMEM, G_GPU, G_GPUSH, G_DISK1, G_DISK2, G_WIFI, G_CPU, G_COUNT };
enum SectionId { SEC_MEM, SEC_GPU, SEC_DISK1, SEC_DISK2, SEC_NET, SEC_CPU, SEC_COUNT };
enum SeriesId {
    S_CPU,
    S_MEM, S_MEM_COMMIT, S_MEM_CACHE,
    S_GPU, S_GPU_DED, S_GPU_SH, S_GPU_3D, S_GPU_COPY, S_GPU_VDEC, S_GPU_VENC, S_GPU_COMP,
    S_DK1_R, S_DK1_W, S_DK1_ACT, S_DK1_RESP,
    S_DK2_R, S_DK2_W, S_DK2_ACT, S_DK2_RESP,
    S_NET_RX, S_NET_TX, S_ETH_RX, S_ETH_TX,
    // the resident's extras thread fills these only while a section window or a strip slot shows them
    S_PING, S_WSIG, S_GPU_TEMP, S_GPU_POWER,
    S_SHARED_COUNT,
    S_COUNT = S_SHARED_COUNT
};
enum UnitKind { U_PCT, U_GB, U_DISK, U_NET, U_MS, U_TEMP, U_WATT };

// ---------------------------------------------------------------- history ring (1 sample per tick, 1 hour)
#define HIST 3600
struct Series {
    volatile LONG head;
    volatile LONG count;
    float v[HIST];
    void init() { head = 0; count = 0; for (int i = 0; i < HIST; i++) v[i] = NAN; }
    void push(float x) {
        LONG h = head;
        v[h] = x;
        _ReadWriteBarrier();
        head = (h + 1) % HIST;
        if (count < HIST) count = count + 1;
    }
    float at(int ago) const {
        LONG c = count, h = head;
        if (ago < 0 || ago >= c) return NAN;
        return v[(h - 1 - ago + 2 * HIST) % HIST];
    }
};
static Series* g_ser[S_COUNT];
static inline const Series& SER(int s) { return *g_ser[s]; }

// ---------------------------------------------------------------- colours
struct Col { float r, g, b; };
static inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline double ClampD(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline Col Hex(UINT32 c) { Col o = { ((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f }; return o; }
static inline Col Mix(Col a, Col b, float t) { Col o = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; return o; }
static inline UINT32 ToHex(Col c) {
    return ((UINT32)(Clamp01(c.r) * 255 + 0.5f) << 16) | ((UINT32)(Clamp01(c.g) * 255 + 0.5f) << 8) | (UINT32)(Clamp01(c.b) * 255 + 0.5f);
}
static inline COLORREF ToCref(Col c) {
    return RGB((int)(Clamp01(c.r) * 255 + 0.5f), (int)(Clamp01(c.g) * 255 + 0.5f), (int)(Clamp01(c.b) * 255 + 0.5f));
}
static const Col WHITE = { 1, 1, 1 };
static const Col BLACK = { 0, 0, 0 };

// theme of the current process' UI (viewer: app theme, resident strip: taskbar theme)
static bool g_dark = false;
static Col InkCol()   { return g_dark ? Hex(0xF0EFEC) : Hex(0x0B0B0B); }   // primary text
static Col Ink2Col()  { return g_dark ? Hex(0xC3C2B7) : Hex(0x52514E); }   // secondary text
static Col Ink3Col()  { return Hex(0x898781); }                            // muted / axis labels
static Col GridCol()  { return g_dark ? Hex(0x2C2C2A) : Hex(0xE1E0D9); }
static Col AxisCol()  { return g_dark ? Hex(0x383835) : Hex(0xC3C2B7); }
static Col CriticalCol() { return Hex(0xD03B3B); }

static bool RegDword(HKEY root, const wchar_t* key, const wchar_t* val, DWORD& out) {
    DWORD sz = sizeof(out);
    return RegGetValueW(root, key, val, RRF_RT_REG_DWORD, nullptr, &out, &sz) == ERROR_SUCCESS;
}
static bool AppsDark() {
    DWORD v = 1;
    RegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", v);
    return v == 0;
}
static bool TaskbarDark() {
    DWORD v = 1;
    RegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", v);
    return v == 0;
}

// ---------------------------------------------------------------- graphs
struct GraphDef {
    const wchar_t* title;
    const wchar_t* id;
    int sA, sB;              // series; sB = -1 for single-series graphs
    UnitKind unit;
    UINT32 light, dark;      // validated categorical palette (adjacent CVD dE >= 8, normal-vision dE >= 15, both modes)
    const wchar_t* labA;
    const wchar_t* labB;
    int section;
};
static const GraphDef g_graphs[G_COUNT] = {
    { L"Memory",               L"mem",       S_MEM,     -1,       U_GB,   0x6250D6, 0x9085E9, nullptr,    nullptr, SEC_MEM },
    { L"Dedicated GPU memory", L"gpumem",    S_GPU_DED, -1,       U_GB,   0xEB6834, 0xD95926, nullptr,    nullptr, SEC_GPU },
    { L"GPU workload",         L"gpu",       S_GPU,     -1,       U_PCT,  0x1BAF7A, 0x199E70, nullptr,    nullptr, SEC_GPU },
    { L"Shared GPU memory",    L"gpushared", S_GPU_SH,  -1,       U_GB,   0xEDA100, 0xC98500, nullptr,    nullptr, SEC_GPU },
    { L"Disk 1",               L"disk1",     S_DK1_R,   S_DK1_W,  U_DISK, 0xE87BA4, 0xD55181, L"Read",    L"Write", SEC_DISK1 },
    { L"Disk 2",               L"disk2",     S_DK2_R,   S_DK2_W,  U_DISK, 0x008300, 0x008300, L"Read",    L"Write", SEC_DISK2 },
    { L"Wi-Fi",                L"wifi",      S_NET_RX,  S_NET_TX, U_NET,  0x2A78D6, 0x3987E5, L"Receive", L"Send",  SEC_NET },
    { L"CPU",                  L"cpu",       S_CPU,     -1,       U_PCT,  0xE34948, 0xE66767, nullptr,    nullptr, SEC_CPU },
};
static const int g_secMain[SEC_COUNT] = { G_MEM, G_GPUMEM, G_DISK1, G_DISK2, G_WIFI, G_CPU };
static const wchar_t* g_secNames[SEC_COUNT] = { L"Memory", L"GPU", L"Disk 1", L"Disk 2", L"Wi-Fi", L"CPU" };
static inline UINT32 GraphHex(int g) { return g_dark ? g_graphs[g].dark : g_graphs[g].light; }
static inline Col GraphCol(int g) { return Hex(GraphHex(g)); }
static inline Col SecCol(int s) { return GraphCol(g_secMain[s]); }
// second series of a dual graph: same hue, stepped toward the ink so it separates by lightness
static inline Col GraphColB(int g) { return g_dark ? Mix(GraphCol(g), WHITE, 0.45f) : Mix(GraphCol(g), Hex(0x10131C), 0.42f); }

// ---------------------------------------------------------------- charts: every graph a section window shows (the strip
// graphs plus each section's own). Any of them can become the section's large chart; the section's slot on the strip
// then shows it too.
enum ChartId { C_MEM, C_COMMIT, C_CACHE, C_GPUMEM, C_GPU, C_GPUSH, C_GPUTEMP, C_ENG3D, C_ENGCOPY, C_ENGVDEC, C_ENGVENC, C_ENGCOMP,
               C_DISK1, C_D1ACT, C_D1RESP, C_DISK2, C_D2ACT, C_D2RESP, C_WIFI, C_PING, C_WSIG, C_ETH, C_CPU, C_COUNT };
struct ChartDef {
    const wchar_t* id;
    const wchar_t* title;      // large chart / popup
    const wchar_t* shortT;     // small panel
    int sec, graph;            // graph: the strip/overview graph this is (G_*), else -1
    int sA, sB;
    UnitKind unit;
    int colG; bool second;     // colour: that graph's first or second shade
    const wchar_t* labA; const wchar_t* labB;
    bool engine;               // lives in the GPU engines panel
};
static const ChartDef g_charts[C_COUNT] = {
    { L"mem",       L"Memory",                L"Memory",           SEC_MEM,   G_MEM,    S_MEM,        -1,       U_GB,   G_MEM,    false, nullptr,    nullptr, false },
    { L"commit",    L"Committed memory",      L"Committed",        SEC_MEM,   -1,       S_MEM_COMMIT, -1,       U_GB,   G_MEM,    false, nullptr,    nullptr, false },
    { L"cache",     L"Cached memory",         L"Cached",           SEC_MEM,   -1,       S_MEM_CACHE,  -1,       U_GB,   G_MEM,    true,  nullptr,    nullptr, false },
    { L"gpumem",    L"Dedicated GPU memory",  L"Dedicated memory", SEC_GPU,   G_GPUMEM, S_GPU_DED,    -1,       U_GB,   G_GPUMEM, false, nullptr,    nullptr, false },
    { L"gpu",       L"GPU workload",          L"Workload",         SEC_GPU,   G_GPU,    S_GPU,        -1,       U_PCT,  G_GPU,    false, nullptr,    nullptr, false },
    { L"gpushared", L"Shared GPU memory",     L"Shared memory",    SEC_GPU,   G_GPUSH,  S_GPU_SH,     -1,       U_GB,   G_GPUSH,  false, nullptr,    nullptr, false },
    { L"gputemp",   L"GPU temperature",       L"Temperature",      SEC_GPU,   -1,       S_GPU_TEMP,   -1,       U_TEMP, G_GPU,    true,  nullptr,    nullptr, false },
    { L"eng3d",     L"GPU 3D engine",         L"3D",               SEC_GPU,   -1,       S_GPU_3D,     -1,       U_PCT,  G_GPU,    false, nullptr,    nullptr, true },
    { L"engcopy",   L"GPU copy engine",       L"Copy",             SEC_GPU,   -1,       S_GPU_COPY,   -1,       U_PCT,  G_GPU,    false, nullptr,    nullptr, true },
    { L"engvdec",   L"GPU video decode",      L"Video decode",     SEC_GPU,   -1,       S_GPU_VDEC,   -1,       U_PCT,  G_GPU,    false, nullptr,    nullptr, true },
    { L"engvenc",   L"GPU video encode",      L"Video encode",     SEC_GPU,   -1,       S_GPU_VENC,   -1,       U_PCT,  G_GPU,    false, nullptr,    nullptr, true },
    { L"engcomp",   L"GPU compute engine",    L"Compute",          SEC_GPU,   -1,       S_GPU_COMP,   -1,       U_PCT,  G_GPU,    false, nullptr,    nullptr, true },
    { L"disk1",     L"Disk 1",                L"Read / write",     SEC_DISK1, G_DISK1,  S_DK1_R,      S_DK1_W,  U_DISK, G_DISK1,  false, L"Read",    L"Write", false },
    { L"disk1act",  L"Disk 1 active time",    L"Active time",      SEC_DISK1, -1,       S_DK1_ACT,    -1,       U_PCT,  G_DISK1,  false, nullptr,    nullptr, false },
    { L"disk1resp", L"Disk 1 response time",  L"Average response time", SEC_DISK1, -1,  S_DK1_RESP,   -1,       U_MS,   G_DISK1,  true,  nullptr,    nullptr, false },
    { L"disk2",     L"Disk 2",                L"Read / write",     SEC_DISK2, G_DISK2,  S_DK2_R,      S_DK2_W,  U_DISK, G_DISK2,  false, L"Read",    L"Write", false },
    { L"disk2act",  L"Disk 2 active time",    L"Active time",      SEC_DISK2, -1,       S_DK2_ACT,    -1,       U_PCT,  G_DISK2,  false, nullptr,    nullptr, false },
    { L"disk2resp", L"Disk 2 response time",  L"Average response time", SEC_DISK2, -1,  S_DK2_RESP,   -1,       U_MS,   G_DISK2,  true,  nullptr,    nullptr, false },
    { L"wifi",      L"Wi-Fi",                 L"Receive / send",   SEC_NET,   G_WIFI,   S_NET_RX,     S_NET_TX, U_NET,  G_WIFI,   false, L"Receive", L"Send",  false },
    { L"ping",      L"Latency",               L"Latency",          SEC_NET,   -1,       S_PING,       -1,       U_MS,   G_WIFI,   false, nullptr,    nullptr, false },
    { L"wsig",      L"Wi-Fi signal",          L"Signal",           SEC_NET,   -1,       S_WSIG,       -1,       U_PCT,  G_WIFI,   true,  nullptr,    nullptr, false },
    { L"eth",       L"Ethernet",              L"Ethernet",         SEC_NET,   -1,       S_ETH_RX,     S_ETH_TX, U_NET,  G_WIFI,   false, L"Receive", L"Send",  false },
    { L"cpu",       L"CPU",                   L"CPU",              SEC_CPU,   G_CPU,    S_CPU,        -1,       U_PCT,  G_CPU,    false, nullptr,    nullptr, false },
};
static const int g_secDefault[SEC_COUNT] = { C_MEM, C_GPUMEM, C_DISK1, C_DISK2, C_WIFI, C_CPU };
static inline Col ChartColA(int c) { return g_charts[c].second ? GraphColB(g_charts[c].colG) : GraphCol(g_charts[c].colG); }
static inline Col ChartColB(int c) { return GraphColB(g_charts[c].colG); }
static int ChartFromId(const wchar_t* s) { for (int c = 0; c < C_COUNT; c++) if (!_wcsicmp(s, g_charts[c].id)) return c; return -1; }
static int ChartOfGraph(int g) { for (int c = 0; c < C_COUNT; c++) if (g_charts[c].graph == g) return c; return C_MEM; }

// ---------------------------------------------------------------- configuration (GlassStack.ini next to the exe)
struct Config {
    UINT hkMods, hkVk; wchar_t hkText[64];
    int order[G_COUNT]; int nGraphs;
    bool showStrip; int stripHeight; int stripPps;
    int disk[2];
    bool netBits;
    int sampleMs;
    wchar_t pingHost[64];
    int theme;              // 0 follow Windows, 1 light, 2 dark
    int lingerSec;
    bool useNvml;
    int rangeSec;
    int lastGraph;
    RECT secRect; bool secRectValid;
    bool secPinned;
    int secChart[SEC_COUNT];   // the large chart chosen in each section (C_*)
};
static Config g_cfg;
static wchar_t g_exePath[MAX_PATH];
static wchar_t g_iniPath[MAX_PATH];
static HINSTANCE g_hInst;
static const wchar_t* INI_SEC = L"GlassStack";

static UINT KeyFromName(const wchar_t* t) {
    if (!t[0]) return 0;
    if (!t[1]) {
        wchar_t c = (wchar_t)towupper(t[0]);
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return c;
        if (c == L'`') return VK_OEM_3;
        return 0;
    }
    if ((t[0] == L'F' || t[0] == L'f') && iswdigit(t[1])) { int n = _wtoi(t + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
    struct K { const wchar_t* n; UINT vk; } keys[] = {
        { L"Space", VK_SPACE }, { L"Tab", VK_TAB }, { L"Enter", VK_RETURN }, { L"Insert", VK_INSERT }, { L"Delete", VK_DELETE },
        { L"Home", VK_HOME }, { L"End", VK_END }, { L"PageUp", VK_PRIOR }, { L"PageDown", VK_NEXT }, { L"Pause", VK_PAUSE },
        { L"Up", VK_UP }, { L"Down", VK_DOWN }, { L"Left", VK_LEFT }, { L"Right", VK_RIGHT }, { L"Backquote", VK_OEM_3 },
    };
    for (auto& k : keys) if (!_wcsicmp(t, k.n)) return k.vk;
    return 0;
}
static void KeyName(UINT vk, wchar_t* out, size_t n) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { swprintf(out, n, L"%c", (wchar_t)vk); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { swprintf(out, n, L"F%u", vk - VK_F1 + 1); return; }
    struct K { const wchar_t* n; UINT vk; } keys[] = {
        { L"Space", VK_SPACE }, { L"Tab", VK_TAB }, { L"Enter", VK_RETURN }, { L"Insert", VK_INSERT }, { L"Delete", VK_DELETE },
        { L"Home", VK_HOME }, { L"End", VK_END }, { L"PageUp", VK_PRIOR }, { L"PageDown", VK_NEXT }, { L"Pause", VK_PAUSE },
        { L"Up", VK_UP }, { L"Down", VK_DOWN }, { L"Left", VK_LEFT }, { L"Right", VK_RIGHT }, { L"Backquote", VK_OEM_3 },
    };
    for (auto& k : keys) if (k.vk == vk) { wcscpy_s(out, n, k.n); return; }
    swprintf(out, n, L"0x%02X", vk);
}
static bool ParseHotkey(const wchar_t* s, UINT& mods, UINT& vk) {
    mods = 0; vk = 0;
    wchar_t buf[96]; wcsncpy_s(buf, s, _TRUNCATE);
    wchar_t* ctx = nullptr;
    for (wchar_t* t = wcstok_s(buf, L"+", &ctx); t; t = wcstok_s(nullptr, L"+", &ctx)) {
        while (*t == L' ') t++;
        size_t l = wcslen(t); while (l && t[l - 1] == L' ') t[--l] = 0;
        if (!_wcsicmp(t, L"Ctrl") || !_wcsicmp(t, L"Control")) mods |= MOD_CONTROL;
        else if (!_wcsicmp(t, L"Shift")) mods |= MOD_SHIFT;
        else if (!_wcsicmp(t, L"Alt")) mods |= MOD_ALT;
        else if (!_wcsicmp(t, L"Win")) mods |= MOD_WIN;
        else vk = KeyFromName(t);
    }
    return vk != 0 && mods != 0;
}
static void FormatHotkey(UINT mods, UINT vk, wchar_t* out, size_t n) {
    out[0] = 0;
    if (mods & MOD_CONTROL) wcscat_s(out, n, L"Ctrl+");
    if (mods & MOD_SHIFT) wcscat_s(out, n, L"Shift+");
    if (mods & MOD_ALT) wcscat_s(out, n, L"Alt+");
    if (mods & MOD_WIN) wcscat_s(out, n, L"Win+");
    wchar_t k[32]; KeyName(vk, k, 32);
    wcscat_s(out, n, k);
}
static int GraphFromId(const wchar_t* s) {
    for (int g = 0; g < G_COUNT; g++) if (!_wcsicmp(s, g_graphs[g].id)) return g;
    return -1;
}
static void Cfg_Paths() {
    GetModuleFileNameW(nullptr, g_exePath, MAX_PATH);
    wcscpy_s(g_iniPath, g_exePath);
    wchar_t* sl = wcsrchr(g_iniPath, L'\\');
    if (sl) sl[1] = 0;
    wcscat_s(g_iniPath, L"GlassStack.ini");
    HANDLE h = CreateFileW(g_iniPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return; }
    // exe folder is read-only (e.g. Program Files): keep settings in %APPDATA%\GlassStack
    wchar_t* appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        swprintf(g_iniPath, MAX_PATH, L"%s\\GlassStack", appdata);
        CreateDirectoryW(g_iniPath, nullptr);
        wcscat_s(g_iniPath, L"\\GlassStack.ini");
        CoTaskMemFree(appdata);
    }
}
static int IniInt(const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(INI_SEC, key, def, g_iniPath); }
static void IniStr(const wchar_t* key, const wchar_t* def, wchar_t* out, DWORD n) { GetPrivateProfileStringW(INI_SEC, key, def, out, n, g_iniPath); }
static void IniPut(const wchar_t* key, const wchar_t* val) { WritePrivateProfileStringW(INI_SEC, key, val, g_iniPath); }
static void IniPutInt(const wchar_t* key, int v) { wchar_t b[32]; swprintf(b, 32, L"%d", v); IniPut(key, b); }

static void Cfg_Load() {
    Config& c = g_cfg;
    wchar_t b[512];
    IniStr(L"Hotkey", L"Ctrl+Shift+Space", b, 512);
    if (!ParseHotkey(b, c.hkMods, c.hkVk)) { c.hkMods = MOD_CONTROL | MOD_SHIFT; c.hkVk = VK_SPACE; }
    FormatHotkey(c.hkMods, c.hkVk, c.hkText, 64);
    IniStr(L"Graphs", L"mem,gpumem,gpu,gpushared,disk1,disk2,wifi,cpu", b, 512);
    c.nGraphs = 0;
    bool used[G_COUNT] = {};
    wchar_t* ctx = nullptr;
    for (wchar_t* t = wcstok_s(b, L", ", &ctx); t && c.nGraphs < G_COUNT; t = wcstok_s(nullptr, L", ", &ctx)) {
        int g = GraphFromId(t);
        if (g >= 0 && !used[g]) { used[g] = true; c.order[c.nGraphs++] = g; }
    }
    if (c.nGraphs == 0) { for (int g = 0; g < G_COUNT; g++) c.order[g] = g; c.nGraphs = G_COUNT; }
    c.showStrip = IniInt(L"ShowStrip", 1) != 0;
    c.stripHeight = std::max(2, std::min(24, IniInt(L"StripHeight", 8)));
    c.stripPps = std::max(1, std::min(8, IniInt(L"StripPixelsPerSecond", 2)));
    c.disk[0] = std::max(0, std::min(15, IniInt(L"Disk1", 0)));
    c.disk[1] = std::max(0, std::min(15, IniInt(L"Disk2", 1)));
    IniStr(L"NetworkUnits", L"bits", b, 64);
    c.netBits = _wcsicmp(b, L"bytes") != 0;
    c.sampleMs = std::max(250, std::min(5000, IniInt(L"SampleMs", 1000)));
    IniStr(L"PingHost", L"1.1.1.1", c.pingHost, 64);
    IniStr(L"Theme", L"auto", b, 64);
    c.theme = !_wcsicmp(b, L"light") ? 1 : !_wcsicmp(b, L"dark") ? 2 : 0;
    c.lingerSec = std::max(0, std::min(3600, IniInt(L"ViewerLingerSeconds", 60)));
    c.useNvml = IniInt(L"UseNvml", 1) != 0;
    int r = IniInt(L"SectionRangeSeconds", 120);
    c.rangeSec = r <= 60 ? 60 : r <= 120 ? 120 : r <= 300 ? 300 : r <= 900 ? 900 : 3600;
    c.lastGraph = std::max(0, std::min(G_COUNT - 1, IniInt(L"LastGraph", 0)));
    IniStr(L"SectionWindow", L"", b, 128);
    c.secRectValid = swscanf_s(b, L"%ld,%ld,%ld,%ld", &c.secRect.left, &c.secRect.top, &c.secRect.right, &c.secRect.bottom) == 4 &&
                     c.secRect.right - c.secRect.left > 200 && c.secRect.bottom - c.secRect.top > 150;
    c.secPinned = IniInt(L"SectionPinned", 0) != 0;
    for (int s = 0; s < SEC_COUNT; s++) c.secChart[s] = g_secDefault[s];
    IniStr(L"SectionCharts", L"", b, 512);
    ctx = nullptr;
    for (wchar_t* t = wcstok_s(b, L", ", &ctx); t; t = wcstok_s(nullptr, L", ", &ctx)) {
        int ch = ChartFromId(t);
        if (ch >= 0) c.secChart[g_charts[ch].sec] = ch;
    }
}
static void Cfg_SaveSectionCharts(const int* sc) {
    wchar_t b[256] = L"";
    for (int s = 0; s < SEC_COUNT; s++) { if (s) wcscat_s(b, L","); wcscat_s(b, g_charts[sc[s]].id); }
    IniPut(L"SectionCharts", b);
}
static void Cfg_SaveGraphs() {
    wchar_t b[256] = L"";
    for (int i = 0; i < g_cfg.nGraphs; i++) { if (i) wcscat_s(b, L","); wcscat_s(b, g_graphs[g_cfg.order[i]].id); }
    IniPut(L"Graphs", b);
}
static void Cfg_WriteDefaults() {
    // write every key once so the file documents itself
    wchar_t b[64];
    IniPut(L"Hotkey", g_cfg.hkText);
    Cfg_SaveGraphs();
    IniPutInt(L"ShowStrip", g_cfg.showStrip);
    IniPutInt(L"StripHeight", g_cfg.stripHeight);
    IniPutInt(L"StripPixelsPerSecond", g_cfg.stripPps);
    IniPutInt(L"Disk1", g_cfg.disk[0]);
    IniPutInt(L"Disk2", g_cfg.disk[1]);
    IniPut(L"NetworkUnits", g_cfg.netBits ? L"bits" : L"bytes");
    IniPutInt(L"SampleMs", g_cfg.sampleMs);
    IniPut(L"PingHost", g_cfg.pingHost);
    IniPut(L"Theme", g_cfg.theme == 1 ? L"light" : g_cfg.theme == 2 ? L"dark" : L"auto");
    IniPutInt(L"ViewerLingerSeconds", g_cfg.lingerSec);
    IniPutInt(L"UseNvml", g_cfg.useNvml);
    IniPutInt(L"SectionRangeSeconds", g_cfg.rangeSec);
    swprintf(b, 64, L"%d", g_cfg.lastGraph); IniPut(L"LastGraph", b);
    Cfg_SaveSectionCharts(g_cfg.secChart);
}
static bool ViewerDark() { return g_cfg.theme == 2 || (g_cfg.theme == 0 && AppsDark()); }

// ---------------------------------------------------------------- formatting
static void FmtBytes(wchar_t* b, size_t n, double v) {
    if (isnan(v)) { wcscpy_s(b, n, L"—"); return; }
    if (v >= GIB) swprintf(b, n, v >= 100 * GIB ? L"%.0f GB" : L"%.1f GB", v / GIB);
    else if (v >= 1024.0 * 1024) swprintf(b, n, L"%.0f MB", v / (1024.0 * 1024));
    else swprintf(b, n, L"%.0f KB", v / 1024.0);
}
// decimal units, like drive makers and network gear; the unit adapts to the size
static void FmtRate(wchar_t* b, size_t n, double v, bool bits) {
    if (isnan(v)) { wcscpy_s(b, n, L"—"); return; }
    if (v < 0) v = 0;
    if (bits) v *= 8;
    const wchar_t* u;
    if (v >= 1e9) { v /= 1e9; u = bits ? L"Gbps" : L"GB/s"; }
    else if (v >= 1e6) { v /= 1e6; u = bits ? L"Mbps" : L"MB/s"; }
    else { v /= 1e3; u = bits ? L"Kbps" : L"KB/s"; }
    swprintf(b, n, v < 10 ? L"%.1f %s" : L"%.0f %s", v, u);
}
static void FmtVal(wchar_t* b, size_t n, UnitKind u, double v) {
    if (isnan(v)) { wcscpy_s(b, n, L"—"); return; }
    switch (u) {
    case U_PCT:  swprintf(b, n, L"%.0f%%", v); break;
    case U_GB:   swprintf(b, n, v < 10 ? L"%.2f GB" : L"%.1f GB", v); break;
    case U_DISK: FmtRate(b, n, v, false); break;
    case U_NET:  FmtRate(b, n, v, g_cfg.netBits); break;
    case U_MS:   swprintf(b, n, v < 10 ? L"%.1f ms" : L"%.0f ms", v); break;
    case U_TEMP: swprintf(b, n, L"%.0f °C", v); break;
    case U_WATT: swprintf(b, n, L"%.0f W", v); break;
    }
}
static void FmtAxis(wchar_t* b, size_t n, UnitKind u, double v) {
    if (v <= 0) {
        switch (u) {
        case U_PCT: wcscpy_s(b, n, L"0%"); return;
        case U_GB: wcscpy_s(b, n, L"0 GB"); return;
        case U_MS: wcscpy_s(b, n, L"0 ms"); return;
        default: wcscpy_s(b, n, L"0"); return;
        }
    }
    if (u == U_GB) { swprintf(b, n, v >= 10 || v == floor(v) ? L"%.0f GB" : L"%.1f GB", v); return; }
    if (u == U_MS) { swprintf(b, n, v == floor(v) ? L"%.0f ms" : L"%.1f ms", v); return; }
    FmtVal(b, n, u, v);
}
static void FmtDuration(wchar_t* b, size_t n, ULONGLONG ms) {
    ULONGLONG s = ms / 1000, d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
    if (d) swprintf(b, n, L"%llud %lluh %llum", d, h, m);
    else swprintf(b, n, L"%lluh %llum", h, m);
}
static double NiceCeil(double v) {
    if (v <= 0) return 1;
    double e = pow(10.0, floor(log10(v)));
    double m = v / e;
    // steps that still divide into four readable gridlines
    static const double steps[] = { 1, 1.2, 1.6, 2, 2.4, 3, 4, 5, 6, 8, 10 };
    double nm = 10;
    for (double s : steps) if (m <= s + 1e-9) { nm = s; break; }
    return nm * e;
}
static UINT DpiOf(HWND h) { UINT d = h ? GetDpiForWindow(h) : 0; return d ? d : 96; }
static UINT MonitorDpi(HMONITOR m) {
    typedef HRESULT(WINAPI* PFN)(HMONITOR, int, UINT*, UINT*);
    static PFN fn = (PFN)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    UINT x = 96, y = 96;
    if (fn && SUCCEEDED(fn(m, 0, &x, &y))) return x;
    return 96;
}
template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void TrimMemory() { SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }
