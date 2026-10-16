// GlassStack - the shared-memory block written by the resident sampler and read by the viewer.
#pragma once

#define GS_MAXCORES 64
#define GS_COREHIST 120
#define GS_MAGIC 0x31534C47u

struct GSInfo {
    wchar_t cpuName[96];
    int logical, cores, pCores, eCores, hybrid;
    BYTE coreClass[GS_MAXCORES];          // 1 = performance core, 0 = efficiency core
    double memTotal;                      // bytes
    int gpuFound;
    wchar_t gpuName[96];
    LUID gpuLuid;
    double gpuDedTotal, gpuShTotal;       // bytes
    int gpuNodes;
    int engPresent[5];                    // 3D, Copy, Video decode, Video encode, Compute
    int diskPresent[2], diskNum[2], diskSsd[2], diskBus[2];
    wchar_t diskModel[2][80];
    wchar_t diskLetters[2][32];
    wchar_t diskTitle[2][40];
    double diskSize[2];
};
struct GSSnap {
    DWORD tick;
    ULONGLONG uptimeMs;
    float cpu; int nCores; float core[GS_MAXCORES];
    double memUsed, memAvail, commit, commitLimit, cached, paged, nonPaged;
    DWORD procs, threads, handles;
    double gpu, gpuDed, gpuSh, eng[5];
    double dR[2], dW[2], dAct[2], dResp[2]; int dOk[2];
    double netRx, netTx, ethRx, ethTx;
    int wifiFound, wifiUp, ethFound, ethUp;
    ULONG64 wifiLink, ethLink, wifiTotRx, wifiTotTx, ethTotRx, ethTotTx;
    ULONG64 wifiLuid, ethLuid;
    wchar_t wifiName[96], ethName[96];
    double costMs;
};
struct GSShared {
    DWORD magic, size;
    volatile LONG seq;            // seqlock around snap + history writes (odd while writing)
    volatile LONG taps;           // incremented by the resident on every hotkey press
    volatile LONG lastFocus;      // graph index (G_*) last chosen in the overview
    volatile LONG overlayHold;    // viewer: overview is open in hold-to-switch mode
    DWORD residentPid;
    volatile LONGLONG residentHwnd;
    volatile LONGLONG viewerHwnd;
    volatile LONG viewerPid;
    volatile LONG fastGpu;        // viewer: the GPU section is open, read every engine on every tick
    volatile LONGLONG overviewHwnd;   // control window of the overview process (it lives only while it shows)
    volatile LONG overviewPid;
    volatile LONG secChart[8];        // the large chart chosen in each section (C_*), set by the section window
    volatile LONG viewSec;            // section the section window shows, -1 when it is closed
    // latest temperature / power / latency / signal measured by the section window (exTick: when), so the resident
    // can keep their history without loading NVML or pinging itself
    volatile float exTemp, exPower, exPing, exSig;
    volatile LONG exTick;
    GSInfo info;
    GSSnap snap;
    volatile LONG coreHead, coreCount;
    float coreHist[GS_MAXCORES][GS_COREHIST];
    Series hist[S_SHARED_COUNT];
};

static GSShared* g_sh = nullptr;
static HANDLE g_shMap = nullptr;
static Series g_localSeries[S_COUNT - S_SHARED_COUNT + 1];

static void Sh_BindSeries() {
    for (int i = 0; i < S_SHARED_COUNT; i++) g_ser[i] = &g_sh->hist[i];
    for (int i = S_SHARED_COUNT; i < S_COUNT; i++) { g_ser[i] = &g_localSeries[i - S_SHARED_COUNT]; g_ser[i]->init(); }
}
// resident: create (or take over) the named block
static bool Sh_Create() {
    g_shMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(GSShared), SHARED_NAME);
    if (!g_shMap) return false;
    g_sh = (GSShared*)MapViewOfFile(g_shMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GSShared));
    if (!g_sh) return false;
    ZeroMemory((void*)g_sh, sizeof(GSShared));
    for (int i = 0; i < S_SHARED_COUNT; i++) g_sh->hist[i].init();
    for (int c = 0; c < GS_MAXCORES; c++) for (int k = 0; k < GS_COREHIST; k++) g_sh->coreHist[c][k] = NAN;
    g_sh->size = sizeof(GSShared);
    g_sh->lastFocus = g_cfg.lastGraph;
    for (int s = 0; s < SEC_COUNT; s++) g_sh->secChart[s] = g_cfg.secChart[s];
    g_sh->viewSec = -1;
    MemoryBarrier();
    g_sh->magic = GS_MAGIC;
    Sh_BindSeries();
    return true;
}
// viewer: open the resident's block
static bool Sh_Open() {
    g_shMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, SHARED_NAME);
    if (!g_shMap) return false;
    g_sh = (GSShared*)MapViewOfFile(g_shMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(GSShared));
    if (!g_sh || g_sh->magic != GS_MAGIC || g_sh->size != sizeof(GSShared)) return false;
    Sh_BindSeries();
    return true;
}
// render test / standalone: a private block
static bool Sh_Private() {
    g_sh = (GSShared*)VirtualAlloc(nullptr, sizeof(GSShared), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_sh) return false;
    for (int i = 0; i < S_SHARED_COUNT; i++) g_sh->hist[i].init();
    for (int c = 0; c < GS_MAXCORES; c++) for (int k = 0; k < GS_COREHIST; k++) g_sh->coreHist[c][k] = NAN;
    for (int s = 0; s < SEC_COUNT; s++) g_sh->secChart[s] = g_cfg.secChart[s];
    g_sh->viewSec = -1;
    g_sh->size = sizeof(GSShared);
    g_sh->magic = GS_MAGIC;
    Sh_BindSeries();
    return true;
}
static void Sh_BeginWrite() { InterlockedIncrement(&g_sh->seq); }
static void Sh_EndWrite() { InterlockedIncrement(&g_sh->seq); }
static bool Sh_ReadSnap(GSSnap& out) {
    for (int tries = 0; tries < 200; tries++) {
        LONG s1 = g_sh->seq;
        if (s1 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        memcpy(&out, (const void*)&g_sh->snap, sizeof(GSSnap));
        MemoryBarrier();
        if (g_sh->seq == s1) return true;
    }
    memcpy(&out, (const void*)&g_sh->snap, sizeof(GSSnap));
    return false;
}
static float CoreAt(int core, int ago) {
    LONG c = g_sh->coreCount, h = g_sh->coreHead;
    if (core < 0 || core >= GS_MAXCORES || ago < 0 || ago >= c) return NAN;
    return g_sh->coreHist[core][(h - 1 - ago + 2 * GS_COREHIST) % GS_COREHIST];
}

// ---------------------------------------------------------------- chart helpers over the history
static double SeriesFixedMax(int s) {
    switch (s) {
    case S_CPU: case S_GPU: case S_GPU_3D: case S_GPU_COPY: case S_GPU_VDEC: case S_GPU_VENC: case S_GPU_COMP:
    case S_DK1_ACT: case S_DK2_ACT: case S_WSIG: return 100;
    case S_MEM: case S_MEM_CACHE: return g_sh->info.memTotal > 0 ? g_sh->info.memTotal / GIB : 16;
    case S_GPU_DED: return g_sh->info.gpuDedTotal > 0 ? g_sh->info.gpuDedTotal / GIB : 8;
    case S_GPU_SH: return g_sh->info.gpuShTotal > 0 ? g_sh->info.gpuShTotal / GIB : 8;
    case S_MEM_COMMIT: return g_sh->snap.commitLimit > 0 ? g_sh->snap.commitLimit / GIB : 0;
    case S_GPU_TEMP: return 100;
    default: return 0;
    }
}
static double AutoFloor(UnitKind u) {
    switch (u) {
    case U_DISK: return 1e6;          // 1 MB/s
    case U_NET: return 1.25e5;        // 1 Mbps
    case U_MS: return 10;
    case U_WATT: return 50;
    case U_TEMP: return 100;
    case U_GB: return 1;
    default: return 1;
    }
}
static double ChartMax(int sA, int sB, UnitKind u, int samples) {
    double fx = SeriesFixedMax(sA);
    if (fx > 0) return fx;
    double m = 0;
    for (int i = 0; i < samples; i++) {
        float a = SER(sA).at(i);
        if (!isnan(a) && a > m) m = a;
        if (sB >= 0) { float b = SER(sB).at(i); if (!isnan(b) && b > m) m = b; }
    }
    double fl = AutoFloor(u);
    if (m < fl) m = fl;
    if (u == U_NET && g_cfg.netBits) return NiceCeil(m * 8 * 1.08) / 8;
    return NiceCeil(m * 1.08);
}
static void SeriesStats(int s, int n, double& avg, double& mx) {
    double sum = 0; int c = 0; mx = NAN;
    for (int i = 0; i < n; i++) {
        float v = SER(s).at(i);
        if (isnan(v)) continue;
        sum += v; c++;
        if (isnan(mx) || v > mx) mx = v;
    }
    avg = c ? sum / c : NAN;
}
static const wchar_t* GraphTitle(int g) {
    if (g == G_DISK1 && g_sh && g_sh->info.diskTitle[0][0]) return g_sh->info.diskTitle[0];
    if (g == G_DISK2 && g_sh && g_sh->info.diskTitle[1][0]) return g_sh->info.diskTitle[1];
    return g_graphs[g].title;
}
// the device behind a percentage graph ("Intel Core Ultra 9 275HX"), used where an avg/peak line would repeat itself
static const wchar_t* GraphDevice(int g) {
    static wchar_t cpu[96];
    if (!g_sh) return nullptr;
    if (g == G_GPU && g_sh->info.gpuName[0]) return g_sh->info.gpuName;
    if (g == G_CPU && g_sh->info.cpuName[0]) {
        const wchar_t* src = g_sh->info.cpuName;
        size_t o = 0;
        for (size_t i = 0; src[i] && o + 1 < 96; i++) {
            if (!wcsncmp(src + i, L"(R)", 3)) { i += 2; continue; }
            if (!wcsncmp(src + i, L"(TM)", 4)) { i += 3; continue; }
            cpu[o++] = src[i];
        }
        cpu[o] = 0;
        return cpu;
    }
    return nullptr;
}
static const wchar_t* SectionTitle(int s) {
    if (s == SEC_DISK1) return GraphTitle(G_DISK1);
    if (s == SEC_DISK2) return GraphTitle(G_DISK2);
    return g_secNames[s];
}
// "12.3 / 127 GB" for capacity graphs, "R 1.2 MB/s  W 300 KB/s" for dual graphs
static void GraphValueText(int g, wchar_t* b, size_t n) {
    const GraphDef& d = g_graphs[g];
    float a = SER(d.sA).at(0);
    if (d.sB < 0) {
        double fx = SeriesFixedMax(d.sA);
        if (d.unit == U_GB && fx > 0 && !isnan(a)) swprintf(b, n, a < 10 ? L"%.2f / %.0f GB" : L"%.1f / %.0f GB", a, fx);
        else FmtVal(b, n, d.unit, a);
        return;
    }
    float c = SER(d.sB).at(0);
    wchar_t va[48], vb[48];
    FmtVal(va, 48, d.unit, a); FmtVal(vb, 48, d.unit, c);
    if (d.unit == U_NET) swprintf(b, n, L"↓ %s   ↑ %s", va, vb);
    else swprintf(b, n, L"R %s   W %s", va, vb);
}

// ---------------------------------------------------------------- charts (see g_charts)
static const wchar_t* ChartTitle(int c) {
    static wchar_t t[96];
    const ChartDef& d = g_charts[c];
    if (d.graph >= 0) return GraphTitle(d.graph);
    if (d.sec == SEC_DISK1 || d.sec == SEC_DISK2) { swprintf(t, 96, L"%s %s", SectionTitle(d.sec), d.shortT); t[0] = towupper(t[0]); return t; }
    if (c == C_PING) { swprintf(t, 96, L"Latency to %s", g_cfg.pingHost); return t; }
    return d.title;
}
// a fixed scale for the chart, 0 = scale to the data
static double ChartFixedMax(int c) { return SeriesFixedMax(g_charts[c].sA); }
static double ChartScale(int c, int samples) {
    double fx = ChartFixedMax(c);
    if (fx > 0) return fx;
    return ChartMax(g_charts[c].sA, g_charts[c].sB, g_charts[c].unit, samples);
}
static bool ContainsI(const wchar_t* hay, const wchar_t* needle);
// the chart exists on this PC (engines the GPU has, Ethernet that is connected, NVIDIA temperature)
static bool ChartAvailable(int c) {
    const ChartDef& d = g_charts[c];
    if (d.engine) return g_sh->info.engPresent[d.sA - S_GPU_3D] != 0;
    if (c == C_ETH) return g_sh->snap.ethFound && g_sh->snap.ethUp;
    if (c == C_GPUTEMP) return g_cfg.useNvml && ContainsI(g_sh->info.gpuName, L"nvidia");
    return true;
}
static void ChartValueText(int c, wchar_t* b, size_t n) {
    const ChartDef& d = g_charts[c];
    if (d.graph >= 0) { GraphValueText(d.graph, b, n); return; }
    float a = SER(d.sA).at(0);
    if (c == C_COMMIT && !isnan(a)) { swprintf(b, n, L"%.1f / %.0f GB", a, ChartFixedMax(c)); return; }
    if (c == C_GPUTEMP && !isnan(a)) {
        float p = SER(S_GPU_POWER).at(0);
        if (!isnan(p)) swprintf(b, n, L"%.0f °C  ·  %.0f W", a, p); else swprintf(b, n, L"%.0f °C", a);
        return;
    }
    if (d.sB >= 0) {
        wchar_t va[48], vb[48];
        FmtVal(va, 48, d.unit, a); FmtVal(vb, 48, d.unit, SER(d.sB).at(0));
        swprintf(b, n, L"↓ %s   ↑ %s", va, vb);
        return;
    }
    FmtVal(b, n, d.unit, a);
}
// The chart a strip slot shows. A section's slot (the slot of its default chart) shows the section's chosen chart,
// unless that one is a strip graph of its own; the other slots show their graph.
static bool StripHasGraph(int g) { for (int i = 0; i < g_cfg.nGraphs; i++) if (g_cfg.order[i] == g) return true; return false; }
static int StripChart(int slot) {
    int g = g_cfg.order[slot];
    int sec = g_graphs[g].section;
    int own = ChartOfGraph(g);
    if (g_charts[g_secDefault[sec]].graph != g) return own;
    int c = g_sh ? (int)g_sh->secChart[sec] : -1;
    if (c < 0 || c >= C_COUNT || g_charts[c].sec != sec) return own;
    if (g_charts[c].graph >= 0 && StripHasGraph(g_charts[c].graph)) return own;
    return c;
}
