// GlassStack - the section window: the graph chosen in the overview in large, plus every related graph of its
// section (Memory, GPU, Disk 1, Disk 2, Wi-Fi, CPU), live details and the processes behind the numbers.
#pragma once

static const int g_ranges[5] = { 60, 120, 300, 900, 3600 };
static const wchar_t* g_rangeNames[5] = { L"1 m", L"2 m", L"5 m", L"15 m", L"1 h" };
static bool g_renderTest = false;

// the window's composition: the whole frame in one surface (drawn when the data or the layout changes) and, over
// it, the mouse-over of the large chart as small visuals that only move (the tooltip is redrawn per sample)
struct SecComp {
    IDCompositionDesktopDevice* dev = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual2* root = nullptr;
    IDCompositionVisual2* frame = nullptr; IDCompositionSurface* frameS = nullptr; UINT fw = 0, fh = 0;
    IDCompositionVisual2* hov = nullptr; IDCompositionVisual3* hov3 = nullptr;
    IDCompositionVisual2* line = nullptr; IDCompositionSurface* lineS = nullptr; UINT lineH = 0;
    IDCompositionVisual2* dot[2] = {}; IDCompositionVisual3* dot3[2] = {}; IDCompositionSurface* dotS[2] = {}; UINT32 dotKey[2] = {};
    IDCompositionVisual2* tip = nullptr; IDCompositionSurface* tipS = nullptr; UINT tipW = 0, tipH = 0;
    int tipAgo = -1, tipLines = 0;
    bool lost = false;
    int fails = 0;
};
struct SectionState {
    HWND hwnd = nullptr;
    SecComp comp;
    RCtx c;
    int sec = SEC_MEM;
    int mainC = C_MEM;                 // the section's large chart (C_*)
    D2D1_RECT_F miniR[16] = {}; int miniC[16] = {}; int miniN = 0, hoverMini = -1;   // the small charts, clickable
    float wDip = 0, hDip = 0;
    D2D1_RECT_F tabR[SEC_COUNT] = {}, rangeR[5] = {}, pinR = {}, ovR = {}, chartR = {}, procR = {};
    D2D1_RECT_F rowR[64] = {}; int rowIdx[64] = {}; int rowN = 0;
    int hoverTab = -1, hoverRange = -1, hoverRow = -1; bool hoverPin = false, hoverOv = false;
    float mx = -1, my = -1; bool mouseIn = false, tracking = false;
    int scroll = 0;
    std::vector<int> order;
    GSSnap snap = {};
    bool cacheOk = false, hoverWasChart = false;      // cacheOk: the frame surface shows the current data and layout
    bool tickDrawn = false;                           // this sample was drawn before its details arrived
};
static SectionState g_sec;
static float g_secOffX = 0, g_secOffY = 0;            // where the surface being drawn sits in its atlas (DIPs)
static int g_secFrames = 0;                           // full frames drawn (bench)
// what the crosshair of the large chart needs, recorded while the frame is drawn
struct ChartHover { bool on; D2D1_RECT_F plot; int sA, sB; UnitKind u; Col ca, cb; double maxV; int samples; };
static ChartHover g_hv = {};
static void Sec_Dirty() { g_sec.cacheOk = false; if (g_sec.hwnd) InvalidateRect(g_sec.hwnd, nullptr, FALSE); }
static IDWriteTextFormat* g_tfIcon = nullptr;

static int Sec_RangeIdx() { for (int i = 0; i < 5; i++) if (g_ranges[i] == g_cfg.rangeSec) return i; return 1; }
static int Sec_Samples() { return std::max(10, (int)((double)g_cfg.rangeSec * 1000.0 / g_cfg.sampleMs)); }
static void FmtAgo(wchar_t* b, size_t n, double sec) {
    if (sec < 90) swprintf(b, n, L"%.0f s", sec);
    else if (sec < 5400) swprintf(b, n, L"%.0f min", sec / 60);
    else swprintf(b, n, L"%.1f h", sec / 3600);
}
static void FmtCount(wchar_t* b, size_t n, double v) {
    if (v >= 1e6) swprintf(b, n, L"%.2f M", v / 1e6);
    else if (v >= 1e4) swprintf(b, n, L"%.0f k", v / 1e3);
    else swprintf(b, n, L"%.0f", v);
}

static void Sec_UpdateWants() {
    int w = 0;
    if (g_sec.hwnd || g_renderTest) {
        switch (g_sec.sec) {
        case SEC_NET: w = DET_NET | DET_PROCS; break;
        case SEC_GPU: w = DET_PROCS | DET_GPU; break;
        default: w = DET_PROCS; break;
        }
    }
    g_detWant = w;
    if (g_sh && !g_renderTest) {
        g_sh->fastGpu = (g_sec.hwnd && g_sec.sec == SEC_GPU) ? 1 : 0;
        g_sh->viewSec = g_sec.hwnd ? g_sec.sec : -1;
    }
    Det_Kick();
}
static void Sec_SortProcs() {
    auto& P = g_det.procs;
    g_sec.order.clear();
    for (size_t i = 0; i < P.size(); i++) if (g_sec.sec != SEC_NET || P[i].conns > 0) g_sec.order.push_back((int)i);
    int sec = g_sec.sec;
    std::sort(g_sec.order.begin(), g_sec.order.end(), [&](int a, int b) {
        const ProcRow& x = P[a]; const ProcRow& y = P[b];
        switch (sec) {
        case SEC_MEM: return x.mem > y.mem;
        case SEC_GPU: if (fabs(x.gpu - y.gpu) > 0.05) return x.gpu > y.gpu; return x.gpuMem > y.gpuMem;
        case SEC_DISK1: case SEC_DISK2: if (fabs(x.io - y.io) > 1) return x.io > y.io; return x.mem > y.mem;
        case SEC_NET: if (x.conns != y.conns) return x.conns > y.conns; return x.io > y.io;
        default: if (fabs(x.cpu - y.cpu) > 0.01) return x.cpu > y.cpu; return x.mem > y.mem;
        }
    });
}

// ---------------------------------------------------------------- drawing pieces
static void Sec_Background(RCtx& c, float W, float H) {
    D2D1_COLOR_F clr = g_dark ? D2D1_COLOR_F{ 0.066f, 0.070f, 0.086f, 1 } : D2D1_COLOR_F{ 0.945f, 0.953f, 0.972f, 1 };
    c.rt->Clear(&clr);
    Col sc = SecCol(g_sec.sec);
    Blob(c, W * 0.18f, -H * 0.05f, W * 0.55f, sc, g_dark ? 0.20f : 0.16f);
    Blob(c, W * 0.95f, H * 1.0f, W * 0.45f, Hex(0x3987E5), g_dark ? 0.10f : 0.08f);
    Blob(c, W * 0.70f, H * 0.10f, W * 0.30f, Mix(sc, WHITE, 0.3f), g_dark ? 0.06f : 0.07f);
}
static void IconText(RCtx& c, const wchar_t* glyph, D2D1_RECT_F r, Col col, float a) {
    if (!g_tfIcon) return;
    g_tfIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    c.rt->DrawText(glyph, (UINT32)wcslen(glyph), g_tfIcon, &r, SB(c, col, a), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
}
static void Sec_TopBar(RCtx& c, float W) {
    float x = 20, y0 = 14, y1 = 50;
    for (int s = 0; s < SEC_COUNT; s++) {
        const wchar_t* t = SectionTitle(s);
        bool act = s == g_sec.sec;
        float w = TextW(t, act ? TF_BODYB : TF_BODY) + 42;
        D2D1_RECT_F r = RF(x, y0, x + w, y1);
        g_sec.tabR[s] = r;
        Col sc = SecCol(s);
        if (act) {
            FillRR(c, r, 18, g_dark ? Hex(0x26272D) : WHITE, g_dark ? 0.95f : 0.92f);
            FillRR(c, r, 18, sc, 0.10f);
            StrokeRR(c, r, 18, sc, 0.55f, 1.2f);
        } else if (s == g_sec.hoverTab) FillRR(c, r, 18, g_dark ? WHITE : Hex(0x1B2240), 0.06f);
        FillCircle(c, r.left + 17, (y0 + y1) * 0.5f, 4.5f, sc, 1.0f);
        Txt(c, t, act ? TF_BODYB : TF_BODY, r.left + 28, y0, r.right - 8, y1, act ? InkCol() : Ink2Col(), 1.0f, 0, 1);
        x += w + 6;
    }
    // right side: [3D view]  [1m 2m 5m 15m 1h]  [pin]
    float rx = W - 20;
    D2D1_RECT_F pin = RF(rx - 36, y0 + 1, rx, y1 - 1);
    g_sec.pinR = pin;
    if (g_cfg.secPinned) FillRR(c, pin, 10, SecCol(g_sec.sec), 0.16f);
    else if (g_sec.hoverPin) FillRR(c, pin, 10, g_dark ? WHITE : Hex(0x1B2240), 0.07f);
    IconText(c, g_cfg.secPinned ? L"" : L"", pin, g_cfg.secPinned ? InkCol() : Ink2Col(), 1.0f);
    rx = pin.left - 10;
    float pw = 44, gw = pw * 5 + 8;
    D2D1_RECT_F grp = RF(rx - gw, y0 + 1, rx, y1 - 1);
    FillRR(c, grp, 17, g_dark ? WHITE : Hex(0x1B2240), g_dark ? 0.06f : 0.05f);
    int ri = Sec_RangeIdx();
    for (int i = 0; i < 5; i++) {
        D2D1_RECT_F r = RF(grp.left + 4 + i * pw, grp.top + 3, grp.left + 4 + (i + 1) * pw, grp.bottom - 3);
        g_sec.rangeR[i] = r;
        if (i == ri) { FillRR(c, r, 14, g_dark ? Hex(0x2E2F36) : WHITE, 1.0f); StrokeRR(c, r, 14, g_dark ? WHITE : Hex(0x1B2240), 0.10f, 1.0f); }
        else if (i == g_sec.hoverRange) FillRR(c, r, 14, g_dark ? WHITE : Hex(0x1B2240), 0.06f);
        Txt(c, g_rangeNames[i], i == ri ? TF_SMALLB : TF_SMALL, r.left, r.top, r.right, r.bottom, i == ri ? InkCol() : Ink2Col(), 1.0f, 1, 1);
    }
    rx = grp.left - 10;
    const wchar_t* ovt = L"3D view";
    float ow = TextW(ovt, TF_SMALLB) + 46;
    D2D1_RECT_F ov = RF(rx - ow, y0 + 1, rx, y1 - 1);
    g_sec.ovR = ov;
    FillRR(c, ov, 17, g_dark ? WHITE : Hex(0x1B2240), g_sec.hoverOv ? 0.10f : 0.05f);
    IconText(c, L"", RF(ov.left + 6, ov.top, ov.left + 34, ov.bottom), Ink2Col(), 1.0f);
    Txt(c, ovt, TF_SMALLB, ov.left + 32, ov.top, ov.right - 8, ov.bottom, InkCol(), 1.0f, 0, 1);
}

// a chart with y-axis labels and a time axis; used by the large panel
static void Sec_AxisChart(RCtx& c, D2D1_RECT_F plot, int sA, int sB, UnitKind u, Col ca, Col cb, double maxV, int samples, bool hover) {
    for (int i = 0; i <= 4; i++) {
        float y = roundf(plot.top + (plot.bottom - plot.top) * i / 4) + 0.5f;
        Line(c, plot.left, y, plot.right, y, i == 4 ? AxisCol() : GridCol(), 1.0f, 1.0f);
        wchar_t lb[48]; FmtAxis(lb, 48, u, maxV * (4 - i) / 4.0);
        Txt(c, lb, TF_TINY, plot.left - 64, y - 9, plot.left - 8, y + 9, Ink3Col(), 1.0f, 2, 1);
    }
    double secs = (double)samples * g_cfg.sampleMs / 1000.0;
    wchar_t t0[32], t1[32]; FmtAgo(t0, 32, secs); FmtAgo(t1, 32, secs / 2);
    wchar_t l0[40], l1[40]; swprintf(l0, 40, L"−%s", t0); swprintf(l1, 40, L"−%s", t1);
    Txt(c, l0, TF_TINY, plot.left, plot.bottom + 6, plot.left + 120, plot.bottom + 22, Ink3Col(), 1.0f, 0);
    Txt(c, l1, TF_TINY, (plot.left + plot.right) * 0.5f - 60, plot.bottom + 6, (plot.left + plot.right) * 0.5f + 60, plot.bottom + 22, Ink3Col(), 1.0f, 1);
    Txt(c, L"now", TF_TINY, plot.right - 120, plot.bottom + 6, plot.right, plot.bottom + 22, Ink3Col(), 1.0f, 2);
    SeriesChart(c, plot, SER(sA), samples, maxV, ca, 2.0f, g_dark ? 0.40f : 0.28f);
    if (sB >= 0) SeriesChart(c, plot, SER(sB), samples, maxV, cb, 1.8f, g_dark ? 0.18f : 0.12f);
    g_hv.on = hover; g_hv.plot = plot; g_hv.sA = sA; g_hv.sB = sB; g_hv.u = u; g_hv.ca = ca; g_hv.cb = cb; g_hv.maxV = maxV; g_hv.samples = samples;
}
// the mouse-over of the large chart: the sample under the mouse and the x where it is drawn
static bool Sec_HoverOn() { return g_hv.on && g_sec.mouseIn && InRect(g_hv.plot, g_sec.mx, g_sec.my); }
static int Sec_HoverSample(float& x) {
    D2D1_RECT_F plot = g_hv.plot;
    int samples = g_hv.samples;
    float frac = (g_sec.mx - plot.left) / (plot.right - plot.left);
    int ago = (int)lroundf((1.0f - frac) * (samples - 1));
    if (ago < 0) ago = 0;
    if (ago > samples - 1) ago = samples - 1;
    x = plot.right - (float)ago / (samples - 1) * (plot.right - plot.left);
    return ago;
}
static float Sec_HoverY(float v) { return g_hv.plot.bottom - (float)ClampD(v / g_hv.maxV, 0, 1) * (g_hv.plot.bottom - g_hv.plot.top); }
static void Sec_HoverDot(RCtx& c, float x, float y, Col col) {
    FillCircle(c, x, y, 5.5f, g_dark ? Hex(0x1A1A19) : WHITE, 1);
    FillCircle(c, x, y, 3.8f, col, 1);
}
// where the tooltip goes for a crosshair at x (it stays inside the plot)
static void Sec_TipPos(float x, int lines, float& tx, float& ty) {
    float tw = 210, th = 14.f + lines * 20.f;
    tx = x + 14; ty = std::max(g_hv.plot.top, g_sec.my - th - 10);
    if (tx + tw > g_hv.plot.right) tx = x - 14 - tw;
}
// the tooltip box for the sample `ago` samples back, its top-left corner at (tx, ty)
static void Sec_DrawTip(RCtx& c, float tx, float ty, int ago) {
    int sB = g_hv.sB;
    UnitKind u = g_hv.u;
    float va = SER(g_hv.sA).at(ago), vb = sB >= 0 ? SER(sB).at(ago) : NAN;
    wchar_t when[64], ag[32];
    double secAgo = (double)ago * g_cfg.sampleMs / 1000.0;
    FmtAgo(ag, 32, secAgo);
    SYSTEMTIME st; GetLocalTime(&st);
    FILETIME ft; SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u64; u64.LowPart = ft.dwLowDateTime; u64.HighPart = ft.dwHighDateTime;
    u64.QuadPart -= (ULONGLONG)(secAgo * 1e7);
    ft.dwLowDateTime = u64.LowPart; ft.dwHighDateTime = u64.HighPart;
    FileTimeToSystemTime(&ft, &st);
    swprintf(when, 64, ago == 0 ? L"%02d:%02d:%02d  ·  now" : L"%02d:%02d:%02d  ·  %s ago", st.wHour, st.wMinute, st.wSecond, ag);
    wchar_t a1[48], b1[48];
    FmtVal(a1, 48, u, va);
    int lines = sB >= 0 ? 3 : 2;
    float tw = 210, th = 14.f + lines * 20.f;
    D2D1_RECT_F tr = RF(tx, ty, tx + tw, ty + th);
    FillRR(c, RF(tr.left + 1, tr.top + 3, tr.right + 1, tr.bottom + 3), 10, BLACK, g_dark ? 0.30f : 0.08f);
    FillRR(c, tr, 10, g_dark ? Hex(0x26272D) : WHITE, 0.98f);
    StrokeRR(c, tr, 10, g_dark ? WHITE : Hex(0x1B2240), 0.12f, 1.0f);
    Txt(c, when, TF_SMALL, tr.left + 12, tr.top + 7, tr.right - 8, tr.top + 25, Ink2Col(), 1.0f, 0, 1);
    const ChartDef& d = g_charts[g_sec.mainC];
    FillCircle(c, tr.left + 16, tr.top + 37, 4, g_hv.ca, 1);
    Txt(c, d.labA ? d.labA : ChartTitle(g_sec.mainC), TF_SMALL, tr.left + 26, tr.top + 27, tr.right - 80, tr.top + 47, Ink2Col(), 1.0f, 0, 1);
    Txt(c, a1, TF_SMALLB, tr.left + 90, tr.top + 27, tr.right - 12, tr.top + 47, InkCol(), 1.0f, 2, 1);
    if (sB >= 0) {
        FmtVal(b1, 48, u, vb);
        FillCircle(c, tr.left + 16, tr.top + 57, 4, g_hv.cb, 1);
        Txt(c, d.labB, TF_SMALL, tr.left + 26, tr.top + 47, tr.right - 80, tr.top + 67, Ink2Col(), 1.0f, 0, 1);
        Txt(c, b1, TF_SMALLB, tr.left + 90, tr.top + 47, tr.right - 12, tr.top + 67, InkCol(), 1.0f, 2, 1);
    }
}
// crosshair, dots and tooltip in one pass (render test; the window moves them as separate visuals)
static void Sec_ChartHover(RCtx& c) {
    if (!Sec_HoverOn()) return;
    float x; int ago = Sec_HoverSample(x);
    DashLine(c, x, g_hv.plot.top, x, g_hv.plot.bottom, Ink3Col(), 0.8f, 1.0f);
    float va = SER(g_hv.sA).at(ago), vb = g_hv.sB >= 0 ? SER(g_hv.sB).at(ago) : NAN;
    if (!isnan(va)) Sec_HoverDot(c, x, Sec_HoverY(va), g_hv.ca);
    if (!isnan(vb)) Sec_HoverDot(c, x, Sec_HoverY(vb), g_hv.cb);
    float tx, ty; Sec_TipPos(x, g_hv.sB >= 0 ? 3 : 2, tx, ty);
    Sec_DrawTip(c, tx, ty, ago);
}
// value and context line of a chart in large type
static void Sec_ChartTexts(int ch, wchar_t* v, size_t nv, wchar_t* sub, size_t ns) {
    const ChartDef& d = g_charts[ch];
    sub[0] = 0;
    if (d.graph >= 0) { Ov_ValueTexts(d.graph, v, nv, sub, ns); return; }
    float a = SER(d.sA).at(0);
    if (d.sB >= 0) {
        float b = SER(d.sB).at(0);
        FmtVal(v, nv, d.unit, (isnan(a) && isnan(b)) ? NAN : (isnan(a) ? 0 : a) + (isnan(b) ? 0 : b));
        return;
    }
    FmtVal(v, nv, d.unit, a);
    double fx = ChartFixedMax(ch);
    if (d.unit == U_GB && !isnan(a) && fx > 0) swprintf(sub, ns, L"of %.1f GB  ·  %.0f%% used", fx, a / fx * 100);
    else if (ch == C_GPUTEMP) { float p = SER(S_GPU_POWER).at(0); if (!isnan(p)) swprintf(sub, ns, L"%s  ·  %.0f W", g_sh->info.gpuName, p); else wcscpy_s(sub, ns, g_sh->info.gpuName); }
    else if (d.engine) wcscpy_s(sub, ns, g_sh->info.gpuName);
}
// "● Read 12 MB/s   ● Write 3 MB/s" for a dual chart
static void Sec_Legend(RCtx& c, int ch, float x, float y0, float y1, float maxX) {
    const ChartDef& d = g_charts[ch];
    wchar_t va[48], vb[48], t[96];
    FmtVal(va, 48, d.unit, SER(d.sA).at(0));
    FmtVal(vb, 48, d.unit, SER(d.sB).at(0));
    float cy = (y0 + y1) * 0.5f;
    swprintf(t, 96, L"%s %s", d.labA, va);
    FillCircle(c, x + 4, cy, 4, ChartColA(ch), 1);
    Txt(c, t, TF_SMALL, x + 13, y0, maxX, y1, Ink2Col(), 1, 0, 1);
    x += 13 + TextW(t, TF_SMALL) + 16;
    swprintf(t, 96, L"%s %s", d.labB, vb);
    FillCircle(c, x + 4, cy, 4, ChartColB(ch), 1);
    Txt(c, t, TF_SMALL, x + 13, y0, maxX, y1, Ink2Col(), 1, 0, 1);
}
static void Sec_MainPanel(RCtx& c, D2D1_RECT_F r) {
    Panel(c, r, 16);
    int ch = g_sec.mainC;
    const ChartDef& d = g_charts[ch];
    Col col = ChartColA(ch);
    FillCircle(c, r.left + 26, r.top + 31, 6, col, 1);
    Txt(c, ChartTitle(ch), TF_TITLE, r.left + 40, r.top + 14, r.right - 260, r.top + 48, InkCol(), 1.0f, 0, 1);
    wchar_t v[96], sub[160];
    Sec_ChartTexts(ch, v, 96, sub, 160);
    Txt(c, v, TF_BIG, r.right - 330, r.top + 10, r.right - 22, r.top + 52, InkCol(), 1.0f, 2, 1);
    if (d.sB >= 0) {
        float vw = TextW(v, TF_BIG);
        Txt(c, L"total", TF_SMALL, r.right - 22 - vw - 100, r.top + 25, r.right - 22 - vw - 10, r.top + 45, Ink3Col(), 1.0f, 2, 1);
        Sec_Legend(c, ch, r.left + 22, r.top + 48, r.top + 70, r.right - 340);
    } else {
        const wchar_t* dev = (d.graph >= 0 && d.unit == U_PCT) ? GraphDevice(d.graph) : nullptr;
        Txt(c, dev ? dev : sub, TF_SMALL, r.left + 22, r.top + 48, r.right - 340, r.top + 70, Ink2Col(), 1.0f, 0, 1);
    }
    int samples = Sec_Samples();
    double avg, mx; SeriesStats(d.sA, samples, avg, mx);
    wchar_t a1[48], a2[48], st[160]; FmtVal(a1, 48, d.unit, avg); FmtVal(a2, 48, d.unit, mx);
    if (d.sB >= 0) swprintf(st, 160, L"%s  avg %s  ·  peak %s", d.labA, a1, a2);
    else swprintf(st, 160, L"avg %s  ·  peak %s", a1, a2);
    Txt(c, st, TF_SMALL, r.right - 330, r.top + 48, r.right - 22, r.top + 70, Ink3Col(), 1.0f, 2, 1);
    D2D1_RECT_F plot = RF(r.left + 84, r.top + 88, r.right - 22, r.bottom - 34);
    g_sec.chartR = plot;
    double maxV = ChartScale(ch, samples);
    if (SER(d.sA).count < 2) { Txt(c, L"collecting…", TF_BODY, plot.left, plot.top, plot.right, plot.bottom, Ink3Col(), 1, 1, 1); return; }
    Sec_AxisChart(c, plot, d.sA, d.sB, d.unit, col, ChartColB(ch), maxV, samples, true);
}

// small chart panel: title + current value + area chart (+ optional second series)
static void Sec_Mini(RCtx& c, D2D1_RECT_F r, const wchar_t* title, int sA, int sB, UnitKind u, Col ca, Col cb, double maxV, const wchar_t* value) {
    Panel(c, r, 14);
    wchar_t vb[64];
    if (!value) { FmtVal(vb, 64, u, SER(sA).at(0)); value = vb; }
    float vw = std::min(TextW(value, TF_BODYB), (r.right - r.left) * 0.6f);
    Txt(c, value, TF_BODYB, r.right - 18 - vw, r.top + 8, r.right - 14, r.top + 32, InkCol(), 1.0f, 2, 1);
    FillCircle(c, r.left + 18, r.top + 20, 4, ca, 1);
    Txt(c, title, TF_SMALLB, r.left + 28, r.top + 8, r.right - 26 - vw, r.top + 32, Ink2Col(), 1.0f, 0, 1);
    D2D1_RECT_F plot = RF(r.left + 14, r.top + 56, r.right - 14, r.bottom - 12);
    int samples = Sec_Samples();
    if (maxV <= 0) maxV = ChartMax(sA, sB, u, samples);
    // the scale sits above its gridline so a full chart never covers it
    Line(c, plot.left, plot.top + 0.5f, plot.right, plot.top + 0.5f, GridCol(), 1.0f, 1.0f);
    Line(c, plot.left, plot.bottom + 0.5f, plot.right, plot.bottom + 0.5f, AxisCol(), 1.0f, 1.0f);
    wchar_t ax[48]; FmtAxis(ax, 48, u, maxV);
    Txt(c, ax, TF_TINY, plot.left, plot.top - 17, plot.right, plot.top - 2, Ink3Col(), 1.0f, 0, 1);
    if (SER(sA).count < 2 && (sB < 0 || SER(sB).count < 2)) { Txt(c, L"collecting…", TF_SMALL, plot.left, plot.top, plot.right, plot.bottom, Ink3Col(), 1, 1, 1); return; }
    SeriesChart(c, plot, SER(sA), samples, maxV, ca, 1.6f, g_dark ? 0.38f : 0.26f);
    if (sB >= 0) SeriesChart(c, plot, SER(sB), samples, maxV, cb, 1.5f, g_dark ? 0.16f : 0.10f);
}
// a clickable chart: remembered so a click makes it the large chart; outlined while the mouse is over it
static void Sec_AddMini(int ch, D2D1_RECT_F r) {
    if (g_sec.miniN < 16) { g_sec.miniR[g_sec.miniN] = r; g_sec.miniC[g_sec.miniN] = ch; g_sec.miniN++; }
}
static void Sec_MiniHover(RCtx& c, int ch, D2D1_RECT_F r, float rad) {
    if (g_sec.hoverMini >= 0 && g_sec.hoverMini < g_sec.miniN && g_sec.miniC[g_sec.hoverMini] == ch)
        StrokeRR(c, RF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad, ChartColA(ch), 0.75f, 1.5f);
}
static void Sec_MiniChart(RCtx& c, D2D1_RECT_F r, int ch) {
    const ChartDef& d = g_charts[ch];
    wchar_t v[64]; ChartValueText(ch, v, 64);
    const wchar_t* title = ch == C_PING ? ChartTitle(ch) : d.shortT;
    Sec_Mini(c, r, title, d.sA, d.sB, d.unit, ChartColA(ch), ChartColB(ch), ChartFixedMax(ch), v);
    Sec_AddMini(ch, r);
    Sec_MiniHover(c, ch, r, 14);
}

static void Sec_EnginePanel(RCtx& c, D2D1_RECT_F r) {
    Panel(c, r, 14);
    Txt(c, L"GPU engines", TF_SMALLB, r.left + 16, r.top + 8, r.right - 16, r.top + 32, Ink2Col(), 1.0f, 0, 1);
    int ids[5], n = 0;
    for (int ch = C_ENG3D; ch <= C_ENGCOMP; ch++) if (ch != g_sec.mainC && ChartAvailable(ch)) ids[n++] = ch;
    if (!n) { Txt(c, L"no engine data", TF_SMALL, r.left, r.top, r.right, r.bottom, Ink3Col(), 1, 1, 1); return; }
    float gap = 10, x0 = r.left + 14, w = (r.right - r.left - 28 - gap * (n - 1)) / n;
    int samples = Sec_Samples();
    for (int i = 0; i < n; i++) {
        int ch = ids[i];
        Col col = ChartColA(ch);
        D2D1_RECT_F t = RF(x0 + i * (w + gap), r.top + 38, x0 + i * (w + gap) + w, r.bottom - 12);
        FillRR(c, t, 10, g_dark ? WHITE : Hex(0x1B2240), g_dark ? 0.04f : 0.035f);
        Txt(c, g_charts[ch].shortT, TF_SMALL, t.left + 10, t.top + 6, t.right - 50, t.top + 24, Ink2Col(), 1.0f, 0, 1);
        wchar_t v[16]; FmtVal(v, 16, U_PCT, SER(g_charts[ch].sA).at(0));
        Txt(c, v, TF_SMALLB, t.right - 60, t.top + 6, t.right - 10, t.top + 24, InkCol(), 1.0f, 2, 1);
        SeriesChart(c, RF(t.left + 8, t.top + 30, t.right - 8, t.bottom - 8), SER(g_charts[ch].sA), samples, 100, col, 1.4f, g_dark ? 0.36f : 0.24f);
        Sec_AddMini(ch, t);
        Sec_MiniHover(c, ch, t, 10);
    }
}

static void Sec_CorePanel(RCtx& c, D2D1_RECT_F r) {
    Panel(c, r, 14);
    const GSInfo& in = g_sh->info;
    Txt(c, L"Logical processors", TF_SMALLB, r.left + 16, r.top + 8, r.right - 16, r.top + 32, Ink2Col(), 1.0f, 0, 1);
    if (in.hybrid) Txt(c, L"P = performance core   E = efficiency core", TF_TINY, r.left + 16, r.top + 8, r.right - 16, r.top + 32, Ink3Col(), 1.0f, 2, 1);
    int n = std::max(1, g_sec.snap.nCores > 0 ? g_sec.snap.nCores : in.logical);
    float gw = r.right - r.left - 28, gh = r.bottom - r.top - 50;
    int cols = n, rows = 1;
    for (int cc = 1; cc <= n; cc++) {   // pick the column count that makes tiles closest to 1.6:1
        int rr = (n + cc - 1) / cc;
        float tw = gw / cc, th = gh / rr;
        float best = fabsf(logf((gw / cols) / (gh / rows) / 1.6f));
        if (fabsf(logf(tw / th / 1.6f)) < best) { cols = cc; rows = rr; }
    }
    float gap = 6, tw = (gw - gap * (cols - 1)) / cols, th = (gh - gap * (rows - 1)) / rows;
    Col col = GraphCol(G_CPU);
    Col tileBase = g_dark ? Hex(0x202127) : WHITE;
    // P-cores first, then E-cores, numbered in their own class
    std::vector<int> ord; for (int i = 0; i < n; i++) if (!in.hybrid || in.coreClass[i]) ord.push_back(i);
    if (in.hybrid) for (int i = 0; i < n; i++) if (!in.coreClass[i]) ord.push_back(i);
    int pN = 0, eN = 0;
    for (int k = 0; k < (int)ord.size(); k++) {
        int i = ord[k];
        int cx = k % cols, cy = k / cols;
        D2D1_RECT_F t = RF(r.left + 14 + cx * (tw + gap), r.top + 40 + cy * (th + gap), r.left + 14 + cx * (tw + gap) + tw, r.top + 40 + cy * (th + gap) + th);
        float v = i < GS_MAXCORES ? g_sec.snap.core[i] : 0;
        FillRR(c, t, 7, Mix(tileBase, col, Clamp01(v / 100.f) * 0.30f), g_dark ? 0.9f : 0.85f);
        wchar_t lb[16];
        if (in.hybrid) swprintf(lb, 16, in.coreClass[i] ? L"P%d" : L"E%d", in.coreClass[i] ? pN++ : eN++);
        else swprintf(lb, 16, L"%d", i);
        Txt(c, lb, TF_TINY, t.left + 7, t.top + 3, t.right, t.top + 17, Ink3Col(), 1.0f, 0, 1);
        wchar_t vs[16]; swprintf(vs, 16, L"%.0f%%", v);
        Txt(c, vs, TF_TINY, t.left, t.top + 3, t.right - 6, t.top + 17, InkCol(), 1.0f, 2, 1);
        // per-core sparkline from the shared 2-minute core history
        D2D1_RECT_F p = RF(t.left + 5, t.top + 19, t.right - 5, t.bottom - 4);
        if (p.bottom - p.top > 6 && g_sh->coreCount > 1) {
            int cnt = std::min((int)g_sh->coreCount, GS_COREHIST);
            ID2D1PathGeometry* geo = nullptr; ID2D1GeometrySink* sk = nullptr;
            if (SUCCEEDED(g_d2d->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sk))) {
                bool begun = false;
                for (int a = cnt - 1; a >= 0; a--) {
                    float cv = CoreAt(i, a);
                    if (isnan(cv)) continue;
                    D2D1_POINT_2F pt = { p.right - (float)a / (GS_COREHIST - 1) * (p.right - p.left), p.bottom - Clamp01(cv / 100.f) * (p.bottom - p.top) };
                    if (!begun) { sk->BeginFigure(pt, D2D1_FIGURE_BEGIN_HOLLOW); begun = true; } else sk->AddLine(pt);
                }
                if (begun) sk->EndFigure(D2D1_FIGURE_END_OPEN);
                sk->Close(); sk->Release();
                c.rt->DrawGeometry(geo, SB(c, col, 0.9f), 1.2f, c.round);
                geo->Release();
            }
        }
    }
}

// the section's other charts (a click makes one the large chart); the GPU engines get a panel of their own and the
// CPU its cores
static void Sec_SmallPanels(RCtx& c, D2D1_RECT_F r) {
    float gap = 14;
    if (g_sec.sec == SEC_CPU) { Sec_CorePanel(c, r); return; }
    int list[C_COUNT], n = 0;
    for (int ch = 0; ch < C_COUNT; ch++) {
        const ChartDef& d = g_charts[ch];
        if (d.sec != g_sec.sec || ch == g_sec.mainC || d.engine || !ChartAvailable(ch)) continue;
        list[n++] = ch;
    }
    bool engines = g_sec.sec == SEC_GPU;
    float h1 = engines ? (r.bottom - r.top - gap) * 0.52f : (r.bottom - r.top);
    if (n > 0) {
        float w = (r.right - r.left - gap * (n - 1)) / n;
        for (int k = 0; k < n; k++)
            Sec_MiniChart(c, RF(r.left + k * (w + gap), r.top, k == n - 1 ? r.right : r.left + k * (w + gap) + w, r.top + h1), list[k]);
    }
    if (engines) Sec_EnginePanel(c, RF(r.left, r.top + h1 + gap, r.right, r.bottom));
}

// ---------------------------------------------------------------- details list
struct KV { wchar_t k[48]; wchar_t v[128]; wchar_t sub[128]; bool warn; };
static void KvAdd(std::vector<KV>& kv, const wchar_t* k, const wchar_t* v, bool warn = false, const wchar_t* sub = nullptr) {
    KV e; wcsncpy_s(e.k, k, _TRUNCATE); wcsncpy_s(e.v, v, _TRUNCATE); wcsncpy_s(e.sub, sub ? sub : L"", _TRUNCATE); e.warn = warn; kv.push_back(e);
}
static void Sec_StatsRows(std::vector<KV>& kv) {
    const GSSnap& s = g_sec.snap;
    const GSInfo& in = g_sh->info;
    wchar_t b[160], b2[48], b3[48], b4[48];
    int samples = Sec_Samples();
    auto nowAvgPeak = [&](int sid, UnitKind u, wchar_t* out, size_t n) {
        double avg, mx; SeriesStats(sid, samples, avg, mx);
        wchar_t x1[48], x2[48], x3[48];
        FmtVal(x1, 48, u, SER(sid).at(0)); FmtVal(x2, 48, u, avg); FmtVal(x3, 48, u, mx);
        swprintf(out, n, L"%s  ·  avg %s  ·  peak %s", x1, x2, x3);
    };
    switch (g_sec.sec) {
    case SEC_MEM:
        swprintf(b, 160, L"%.1f GB  (%.0f%%)", s.memUsed / GIB, in.memTotal > 0 ? s.memUsed / in.memTotal * 100 : 0); KvAdd(kv, L"In use", b);
        swprintf(b, 160, L"%.1f GB", s.memAvail / GIB); KvAdd(kv, L"Available", b);
        swprintf(b, 160, L"%.1f / %.1f GB", s.commit / GIB, s.commitLimit / GIB); KvAdd(kv, L"Committed", b);
        swprintf(b, 160, L"%.1f GB", s.cached / GIB); KvAdd(kv, L"Cached", b);
        if (!isnan(g_det.compressed)) { FmtBytes(b2, 48, g_det.compressed); KvAdd(kv, L"Compressed", b2); }
        swprintf(b, 160, L"%.2f GB  /  %.2f GB", s.paged / GIB, s.nonPaged / GIB); KvAdd(kv, L"Paged / non-paged pool", b, s.nonPaged > 1.0 * GIB);
        swprintf(b, 160, L"%.1f GB", in.memTotal / GIB); KvAdd(kv, L"Total", b);
        break;
    case SEC_GPU:
        KvAdd(kv, L"Adapter", in.gpuName);
        nowAvgPeak(S_GPU, U_PCT, b, 160); KvAdd(kv, L"Workload", b);
        swprintf(b, 160, L"%.2f / %.1f GB", s.gpuDed / GIB, in.gpuDedTotal / GIB); KvAdd(kv, L"Dedicated memory", b);
        swprintf(b, 160, L"%.2f / %.1f GB", s.gpuSh / GIB, in.gpuShTotal / GIB); KvAdd(kv, L"Shared memory", b);
        if (!isnan(g_det.gpuTemp)) { swprintf(b, 160, L"%.0f °C", g_det.gpuTemp); KvAdd(kv, L"Temperature", b); }
        if (!isnan(g_det.gpuPower)) { swprintf(b, 160, L"%.0f W", g_det.gpuPower); KvAdd(kv, L"Power", b); }
        swprintf(b, 160, L"%d", in.gpuNodes); KvAdd(kv, L"Engines", b);
        break;
    case SEC_DISK1: case SEC_DISK2: {
        int k = g_sec.sec == SEC_DISK1 ? 0 : 1;
        if (!in.diskPresent[k]) { KvAdd(kv, L"Status", L"no disk with this number (tray menu → Disk)"); break; }
        KvAdd(kv, L"Model", in.diskModel[k]);
        FmtBytes(b2, 48, in.diskSize[k]);
        const wchar_t* bus = in.diskBus[k] == 17 ? L"NVMe" : in.diskBus[k] == 11 ? L"SATA" : in.diskBus[k] == 7 ? L"USB" : L"";
        swprintf(b, 160, L"%s  ·  %s %s", b2, bus, in.diskSsd[k] == 1 ? L"SSD" : in.diskSsd[k] == 0 ? L"HDD" : L"");
        KvAdd(kv, L"Capacity", b);
        KvAdd(kv, L"Volumes", in.diskLetters[k][0] ? in.diskLetters[k] : L"—");
        swprintf(b, 160, L"PhysicalDrive%d", in.diskNum[k]); KvAdd(kv, L"Device", b);
        nowAvgPeak(k == 0 ? S_DK1_R : S_DK2_R, U_DISK, b, 160); KvAdd(kv, L"Read", b);
        nowAvgPeak(k == 0 ? S_DK1_W : S_DK2_W, U_DISK, b, 160); KvAdd(kv, L"Write", b);
        FmtVal(b, 160, U_PCT, s.dAct[k]); KvAdd(kv, L"Active time", b);
        FmtVal(b, 160, U_MS, s.dResp[k]); KvAdd(kv, L"Average response", b);
        break;
    }
    case SEC_NET: {
        KvAdd(kv, L"Adapter", s.wifiName[0] ? s.wifiName : L"no Wi-Fi adapter");
        KvAdd(kv, L"Network", g_det.ssid[0] ? g_det.ssid : (s.wifiUp ? L"…" : L"disconnected"));
        if (!isnan(g_det.wsig)) { swprintf(b, 160, L"%.0f%%", g_det.wsig); KvAdd(kv, L"Signal", b, g_det.wsig < 40); }
        if (g_det.wPhy >= 0) {
            const wchar_t* phy = g_det.wPhy == 11 ? L"Wi-Fi 7" : g_det.wPhy == 10 ? L"Wi-Fi 6" : g_det.wPhy == 8 ? L"Wi-Fi 5" : g_det.wPhy == 7 ? L"Wi-Fi 4" : L"802.11";
            if (g_det.wChannel) swprintf(b, 160, L"%s  ·  channel %d", phy, g_det.wChannel); else wcscpy_s(b, phy);
            KvAdd(kv, L"Standard", b);
        }
        if (!isnan(g_det.wRx)) { swprintf(b, 160, L"↓ %.0f Mbps  ·  ↑ %.0f Mbps", g_det.wRx, g_det.wTx); KvAdd(kv, L"Link rate", b); }
        else { swprintf(b, 160, L"%.0f Mbps", s.wifiLink / 1e6); KvAdd(kv, L"Link rate", b); }
        KvAdd(kv, L"IPv4", g_det.ipv4[0] ? g_det.ipv4 : L"—");
        if (!isnan(g_det.ping)) { swprintf(b, 160, L"%.0f ms", g_det.ping); KvAdd(kv, L"Latency", b, g_det.ping > 150); }
        FmtBytes(b2, 48, (double)s.wifiTotRx); FmtBytes(b3, 48, (double)s.wifiTotTx);
        swprintf(b, 160, L"↓ %s  ·  ↑ %s", b2, b3); KvAdd(kv, L"Since boot", b);
        if (s.ethFound) {
            if (s.ethUp) { FmtRate(b2, 48, s.ethRx, g_cfg.netBits); FmtRate(b3, 48, s.ethTx, g_cfg.netBits); swprintf(b, 160, L"%.0f Mbps link  ·  ↓ %s  ↑ %s", s.ethLink / 1e6, b2, b3); }
            else wcscpy_s(b, L"cable not connected");
            KvAdd(kv, L"Ethernet", b);
        }
        break;
    }
    case SEC_CPU:
        KvAdd(kv, L"Processor", in.cpuName);
        nowAvgPeak(S_CPU, U_PCT, b, 160); KvAdd(kv, L"Utilization", b);
        if (in.hybrid) swprintf(b, 160, L"%d  (%d performance + %d efficiency)", in.cores, in.pCores, in.eCores);
        else swprintf(b, 160, L"%d", in.cores);
        KvAdd(kv, L"Cores", b);
        swprintf(b, 160, L"%d", in.logical); KvAdd(kv, L"Logical processors", b);
        FmtCount(b2, 48, s.procs); FmtCount(b3, 48, s.threads); FmtCount(b4, 48, s.handles);
        swprintf(b, 160, L"%s  ·  %s  ·  %s", b2, b3, b4); KvAdd(kv, L"Processes · threads · handles", b, s.handles > 1000000);
        FmtDuration(b, 160, s.uptimeMs); KvAdd(kv, L"Up time", b);
        break;
    }
    if ((g_sec.sec == SEC_MEM || g_sec.sec == SEC_CPU) && g_det.leakHandles > 100000) {
        wchar_t hc[48]; FmtCount(hc, 48, g_det.leakHandles);
        swprintf(b, 160, L"%s handles", hc);
        KvAdd(kv, L"Possible handle leak", b, true, g_det.leakName.c_str());
    }
}
static void Sec_Stats(RCtx& c, D2D1_RECT_F r) {
    Panel(c, r, 16);
    Txt(c, L"Details", TF_BODYB, r.left + 18, r.top + 12, r.right - 18, r.top + 38, InkCol(), 1.0f, 0, 1);
    std::vector<KV> kv;
    Sec_StatsRows(kv);
    float y = r.top + 46, rowH = 27;
    for (auto& e : kv) {
        float h = e.sub[0] ? rowH + 20 : rowH;
        if (y + h > r.bottom - 8) break;
        float kx = r.left + (e.warn ? 36 : 18);
        if (e.warn) {
            FillRR(c, RF(r.left + 10, y + 1, r.right - 10, y + h - 1), 8, CriticalCol(), g_dark ? 0.16f : 0.08f);
            IconText(c, L"", RF(r.left + 12, y, r.left + 32, y + rowH), CriticalCol(), 1.0f);
        }
        // key on the left, value right-aligned and free to use whatever the key leaves
        float kw = TextW(e.k, TF_SMALL);
        Txt(c, e.k, TF_SMALL, kx, y, kx + kw + 4, y + rowH, e.warn ? CriticalCol() : Ink2Col(), 1.0f, 0, 1);
        Txt(c, e.v, TF_SMALLB, kx + kw + 18, y, r.right - 18, y + rowH, e.warn ? CriticalCol() : InkCol(), 1.0f, 2, 1);
        if (e.sub[0]) Txt(c, e.sub, TF_SMALL, kx, y + rowH - 6, r.right - 18, y + h - 4, e.warn ? CriticalCol() : Ink2Col(), 1.0f, 0, 1);
        y += h;
    }
}

// ---------------------------------------------------------------- process list
static void Sec_ProcMetric(const ProcRow& p, wchar_t* a, size_t na, wchar_t* b, size_t nb, double& bar) {
    b[0] = 0;
    switch (g_sec.sec) {
    case SEC_MEM: FmtBytes(a, na, p.mem); bar = p.mem; break;
    case SEC_GPU:
        swprintf(a, na, L"%.0f%%", p.gpu); bar = p.gpu;
        if (p.gpuMem > 0) FmtBytes(b, nb, p.gpuMem); else wcscpy_s(b, nb, L"—");
        break;
    case SEC_DISK1: case SEC_DISK2: FmtRate(a, na, p.io, false); bar = p.io; break;
    case SEC_NET: swprintf(a, na, L"%d", p.conns); bar = p.conns; break;
    default: swprintf(a, na, p.cpu >= 10 ? L"%.0f%%" : L"%.1f%%", p.cpu); bar = p.cpu; break;
    }
}
static void Sec_Procs(RCtx& c, D2D1_RECT_F r) {
    Panel(c, r, 16);
    g_sec.procR = r;
    const wchar_t* by = g_sec.sec == SEC_MEM ? L"by memory (private working set)" : g_sec.sec == SEC_GPU ? L"by GPU use · dedicated memory" :
                        g_sec.sec == SEC_CPU ? L"by CPU" : g_sec.sec == SEC_NET ? L"by open TCP connections" : L"by I/O (all devices)";
    Txt(c, L"Processes", TF_BODYB, r.left + 18, r.top + 12, r.right - 18, r.top + 38, InkCol(), 1.0f, 0, 1);
    Txt(c, by, TF_TINY, r.left + 100, r.top + 12, r.right - 18, r.top + 38, Ink3Col(), 1.0f, 2, 1);
    g_sec.rowN = 0;
    if (!g_det.procsValid) { Txt(c, L"collecting…", TF_SMALL, r.left, r.top + 40, r.right, r.bottom, Ink3Col(), 1, 1, 1); return; }
    Sec_SortProcs();
    if (g_sec.order.empty()) { Txt(c, L"no open connections", TF_SMALL, r.left, r.top + 40, r.right, r.bottom, Ink3Col(), 1, 1, 1); return; }
    auto& P = g_det.procs;
    float y = r.top + 46, rowH = 30;
    int maxRows = (int)((r.bottom - 10 - y) / rowH);
    int total = (int)g_sec.order.size();
    g_sec.scroll = std::max(0, std::min(g_sec.scroll, total - maxRows));
    double top = 0;
    for (int i = 0; i < total && i < 1; i++) { wchar_t a[32], b[32]; double bv; Sec_ProcMetric(P[g_sec.order[i]], a, 32, b, 32, bv); top = bv; }
    if (g_sec.sec == SEC_GPU) { top = 0; for (int i : g_sec.order) top = std::max(top, P[i].gpu); }
    Col sc = SecCol(g_sec.sec);
    for (int k = 0; k < maxRows && g_sec.scroll + k < total && k < 64; k++) {
        int idx = g_sec.order[g_sec.scroll + k];
        const ProcRow& p = P[idx];
        D2D1_RECT_F rr = RF(r.left + 10, y, r.right - 10, y + rowH - 2);
        g_sec.rowR[k] = rr; g_sec.rowIdx[k] = idx; g_sec.rowN = k + 1;
        wchar_t a[48], b[48]; double bv;
        Sec_ProcMetric(p, a, 48, b, 48, bv);
        if (top > 0 && bv > 0) FillRR(c, RF(rr.left, rr.top, rr.left + (rr.right - rr.left) * (float)Clamp01((float)(bv / top)), rr.bottom), 7, sc, g_dark ? 0.16f : 0.10f);
        if (k == g_sec.hoverRow) FillRR(c, rr, 7, g_dark ? WHITE : Hex(0x1B2240), 0.06f);
        std::wstring nm = p.name;
        if (nm.size() > 4 && !_wcsicmp(nm.c_str() + nm.size() - 4, L".exe")) nm.resize(nm.size() - 4);
        float valueW = b[0] ? 150.f : 90.f;
        Txt(c, nm.c_str(), TF_SMALL, rr.left + 10, rr.top, rr.right - valueW - 34, rr.bottom, InkCol(), 1.0f, 0, 1);
        if (p.count > 1) {
            wchar_t cnt[16]; swprintf(cnt, 16, L"×%d", p.count);
            float nw = std::min(TextW(nm.c_str(), TF_SMALL), rr.right - valueW - 44 - rr.left - 10);
            Txt(c, cnt, TF_TINY, rr.left + 14 + nw, rr.top, rr.right - valueW, rr.bottom, Ink3Col(), 1.0f, 0, 1);
        }
        if (b[0]) {
            Txt(c, a, TF_SMALLB, rr.right - 150, rr.top, rr.right - 80, rr.bottom, InkCol(), 1.0f, 2, 1);
            Txt(c, b, TF_SMALL, rr.right - 78, rr.top, rr.right - 10, rr.bottom, Ink2Col(), 1.0f, 2, 1);
        } else Txt(c, a, TF_SMALLB, rr.right - 100, rr.top, rr.right - 10, rr.bottom, InkCol(), 1.0f, 2, 1);
        y += rowH;
    }
}

static void Sec_Render(RCtx& c, float W, float H) {
    D2D1_MATRIX_3X2_F I = MTrans(g_secOffX, g_secOffY);
    c.rt->SetTransform(&I);
    g_hv.on = false;
    g_sec.miniN = 0;
    Sec_Background(c, W, H);
    Sec_TopBar(c, W);
    float P = 20, top = 64;
    float leftW = (W - 3 * P) * 0.635f;
    float rightX = P + leftW + P;
    float contentH = H - top - P;
    float mainH = std::max(250.f, contentH * 0.58f);
    D2D1_RECT_F mainR = RF(P, top, P + leftW, top + mainH);
    D2D1_RECT_F smallR = RF(P, mainR.bottom + 14, P + leftW, H - P);
    Sec_MainPanel(c, mainR);
    if (smallR.bottom - smallR.top > 80) Sec_SmallPanels(c, smallR);
    float statsH = std::min(contentH * 0.46f, 46.f + 27.f * 9 + 8);
    Sec_Stats(c, RF(rightX, top, W - P, top + statsH));
    Sec_Procs(c, RF(rightX, top + statsH + 14, W - P, H - P));
}

// ---------------------------------------------------------------- window
static void Sec_ReleaseRT() {
    SecComp& K = g_sec.comp;
    SafeRelease(K.tipS); SafeRelease(K.tip);
    for (int i = 0; i < 2; i++) { SafeRelease(K.dotS[i]); SafeRelease(K.dot3[i]); SafeRelease(K.dot[i]); K.dotKey[i] = 0; }
    SafeRelease(K.lineS); SafeRelease(K.line);
    SafeRelease(K.hov3); SafeRelease(K.hov);
    SafeRelease(K.frameS); SafeRelease(K.frame);
    if (K.target) K.target->SetRoot(nullptr);
    SafeRelease(K.root); SafeRelease(K.target);
    if (K.dev) K.dev->Commit();
    SafeRelease(K.dev);
    K.fw = K.fh = K.lineH = K.tipW = K.tipH = 0; K.tipAgo = -1; K.lost = false;
    g_sec.cacheOk = false;
    RC_Release(g_sec.c);
}
static bool Sec_CreateRT() {
    SecComp& K = g_sec.comp;
    if (!g_sec.hwnd || !Gfx_Ensure()) return false;
    if (!RC_Init(g_sec.c, g_dc)) { Sec_ReleaseRT(); return false; }
    bool ok = SUCCEEDED(DCompositionCreateDevice2(g_d2dDev, IID_PPV_ARGS(&K.dev)));
    if (!ok) K.dev = nullptr;
    ok = ok && SUCCEEDED(K.dev->CreateTargetForHwnd(g_sec.hwnd, TRUE, &K.target));
    ok = ok && SUCCEEDED(K.dev->CreateVisual(&K.root)) && SUCCEEDED(K.dev->CreateVisual(&K.frame)) && SUCCEEDED(K.dev->CreateVisual(&K.hov)) &&
         SUCCEEDED(K.dev->CreateVisual(&K.line)) && SUCCEEDED(K.dev->CreateVisual(&K.dot[0])) && SUCCEEDED(K.dev->CreateVisual(&K.dot[1])) &&
         SUCCEEDED(K.dev->CreateVisual(&K.tip));
    if (!ok) { Sec_ReleaseRT(); return false; }
    K.target->SetRoot(K.root);
    K.root->AddVisual(K.frame, FALSE, nullptr);          // FALSE with no reference visual: on top of the ones added before
    K.root->AddVisual(K.hov, FALSE, nullptr);
    K.hov->AddVisual(K.line, FALSE, nullptr);
    K.hov->AddVisual(K.dot[0], FALSE, nullptr);
    K.hov->AddVisual(K.dot[1], FALSE, nullptr);
    K.hov->AddVisual(K.tip, FALSE, nullptr);
    if (FAILED(K.hov->QueryInterface(IID_PPV_ARGS(&K.hov3)))) K.hov3 = nullptr;
    for (int i = 0; i < 2; i++) if (FAILED(K.dot[i]->QueryInterface(IID_PPV_ARGS(&K.dot3[i])))) K.dot3[i] = nullptr;
    if (K.hov3) K.hov3->SetVisible(FALSE);
    return true;
}
// draws a whole w x h surface at the window's DPI; Sec_Render starts from g_secOffX/Y, the surface's place in its atlas
template <class F> static bool Sec_Draw(IDCompositionSurface* s, UINT w, UINT h, float dpi, bool clear, F draw) {
    SecComp& K = g_sec.comp;
    ID2D1DeviceContext* dc = nullptr; POINT off = {};
    if (!s || FAILED(s->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), (void**)&dc, &off)) || !dc) { K.lost = true; return false; }
    float k = 96.0f / dpi;
    dc->SetDpi(dpi, dpi);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_secOffX = off.x * k; g_secOffY = off.y * k;
    D2D1_MATRIX_3X2_F T = MTrans(g_secOffX, g_secOffY);
    dc->SetTransform(&T);
    D2D1_RECT_F clip = RF(0, 0, w * k, h * k);
    dc->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_ALIASED);
    if (clear) { D2D1_COLOR_F none = { 0, 0, 0, 0 }; dc->Clear(&none); }
    ID2D1RenderTarget* keep = g_sec.c.rt;
    g_sec.c.rt = dc;
    draw();
    g_sec.c.rt = keep;
    dc->PopAxisAlignedClip();
    g_secOffX = g_secOffY = 0;
    dc->Release();
    if (FAILED(s->EndDraw())) { K.lost = true; return false; }
    return true;
}
static bool Sec_NewSurface(UINT w, UINT h, bool opaque, IDCompositionSurface** s) {
    if (SUCCEEDED(g_sec.comp.dev->CreateSurface(std::max(1u, w), std::max(1u, h), DXGI_FORMAT_B8G8R8A8_UNORM,
                                                 opaque ? DXGI_ALPHA_MODE_IGNORE : DXGI_ALPHA_MODE_PREMULTIPLIED, s))) return true;
    *s = nullptr;
    g_sec.comp.lost = true;
    return false;
}
// the mouse-over of the large chart: the dashed line and the dots only move; the tooltip is redrawn only when it
// shows another sample (or new data came in)
static void Sec_HoverLayer(float dpi) {
    SecComp& K = g_sec.comp;
    bool on = Sec_HoverOn();
    if (K.hov3) K.hov3->SetVisible(on ? TRUE : FALSE);
    if (!on) return;
    float sc = dpi / 96.0f, k = 96.0f / dpi;
    float x; int ago = Sec_HoverSample(x);
    const D2D1_RECT_F plot = g_hv.plot;
    UINT lh = (UINT)ceilf((plot.bottom - plot.top) * sc);
    if (!K.lineS || K.lineH != lh) {
        SafeRelease(K.lineS);
        if (!Sec_NewSurface(4, lh, false, &K.lineS)) return;
        K.lineH = lh; K.line->SetContent(K.lineS);
        Sec_Draw(K.lineS, 4, lh, dpi, true, [&]() { DashLine(g_sec.c, 2 * k, 0, 2 * k, lh * k, Ink3Col(), 0.8f, 1.0f); });
    }
    K.line->SetOffsetX(roundf(x * sc) - 2); K.line->SetOffsetY(roundf(plot.top * sc));
    UINT dp = (UINT)ceilf(13 * sc);
    float vals[2] = { SER(g_hv.sA).at(ago), g_hv.sB >= 0 ? SER(g_hv.sB).at(ago) : NAN };
    Col cols[2] = { g_hv.ca, g_hv.cb };
    for (int i = 0; i < 2; i++) {
        bool show = !isnan(vals[i]);
        if (K.dot3[i]) K.dot3[i]->SetVisible(show ? TRUE : FALSE);
        if (!show) continue;
        UINT32 key = (ToHex(cols[i]) & 0xFFFFFFu) | (g_dark ? 1u << 24 : 0u) | (dp << 25);
        if (!K.dotS[i] || K.dotKey[i] != key) {
            SafeRelease(K.dotS[i]);
            if (!Sec_NewSurface(dp, dp, false, &K.dotS[i])) return;
            K.dot[i]->SetContent(K.dotS[i]);
            Col col = cols[i];
            Sec_Draw(K.dotS[i], dp, dp, dpi, true, [&]() { Sec_HoverDot(g_sec.c, dp * 0.5f * k, dp * 0.5f * k, col); });
            K.dotKey[i] = key;
        }
        K.dot[i]->SetOffsetX(roundf(x * sc - dp * 0.5f)); K.dot[i]->SetOffsetY(roundf(Sec_HoverY(vals[i]) * sc - dp * 0.5f));
    }
    int lines = g_hv.sB >= 0 ? 3 : 2;
    UINT tw = (UINT)ceilf(218 * sc), th = (UINT)ceilf((14 + 3 * 20 + 8) * sc);
    if (!K.tipS || K.tipW != tw || K.tipH != th) {
        SafeRelease(K.tipS);
        if (!Sec_NewSurface(tw, th, false, &K.tipS)) return;
        K.tipW = tw; K.tipH = th; K.tip->SetContent(K.tipS); K.tipAgo = -1;
    }
    if (K.tipAgo != ago || K.tipLines != lines) {
        if (!Sec_Draw(K.tipS, tw, th, dpi, true, [&]() { Sec_DrawTip(g_sec.c, 4, 4, ago); })) return;
        K.tipAgo = ago; K.tipLines = lines;
    }
    float tx, ty; Sec_TipPos(x, lines, tx, ty);
    K.tip->SetOffsetX(roundf((tx - 4) * sc)); K.tip->SetOffsetY(roundf((ty - 4) * sc));
}
// shows the window: redraws the frame when the data or the layout changed (about 30 ms in software), places the
// mouse-over and hands the result to the compositor
static void Sec_Present() {
    SecComp& K = g_sec.comp;
    HWND h = g_sec.hwnd;
    if (!h || IsIconic(h)) return;
    if (!K.dev && !Sec_CreateRT()) return;
    RECT rc; GetClientRect(h, &rc);
    UINT w = (UINT)std::max(1L, rc.right), ht = (UINT)std::max(1L, rc.bottom);
    float dpi = (float)DpiOf(h), k = 96.0f / dpi;
    g_sec.wDip = w * k; g_sec.hDip = ht * k;
    if (!K.lost && (!K.frameS || K.fw != w || K.fh != ht)) {
        SafeRelease(K.frameS);
        if (Sec_NewSurface(w, ht, true, &K.frameS)) { K.fw = w; K.fh = ht; K.frame->SetContent(K.frameS); g_sec.cacheOk = false; }
    }
    if (!K.lost && !g_sec.cacheOk && Sec_Draw(K.frameS, w, ht, dpi, false, [&]() { Sec_Render(g_sec.c, g_sec.wDip, g_sec.hDip); })) {
        g_sec.cacheOk = true;
        K.tipAgo = -1;
        g_secFrames++;
    }
    if (!K.lost) Sec_HoverLayer(dpi);
    if (!K.lost && FAILED(K.dev->Commit())) K.lost = true;
    if (!K.lost) K.fails = 0;
    else if (++K.fails <= 3) Viewer_DeviceLost();     // rebuilt on the next paint; give up after repeated failures
}
static void Sec_ApplyTheme() {
    if (!g_sec.hwnd) return;
    BOOL dark = g_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_sec.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int mica = 2;  /*DWMSBT_MAINWINDOW*/
    DwmSetWindowAttribute(g_sec.hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &mica, sizeof(mica));
    Sec_ReleaseRT();
    Sec_Dirty();
}
static void Sec_SetTitle() {
    if (!g_sec.hwnd) return;
    wchar_t t[128]; swprintf(t, 128, L"GlassStack  ·  %s", SectionTitle(g_sec.sec));
    SetWindowTextW(g_sec.hwnd, t);
}
// the strip graph a chart stands for (the overview starts on it): its own, or its section's default
static int Sec_ChartGraph(int ch) { return g_charts[ch].graph >= 0 ? g_charts[ch].graph : g_charts[g_secDefault[g_charts[ch].sec]].graph; }
// shows a section with a chart in large (chart < 0: the one chosen there last); the choice is kept, and the
// section's strip slot follows it
static void Sec_SwitchTo(int sec, int chart) {
    if (chart < 0 || chart >= C_COUNT || g_charts[chart].sec != sec || !ChartAvailable(chart)) {
        chart = (int)g_sh->secChart[sec];
        if (chart < 0 || chart >= C_COUNT || g_charts[chart].sec != sec || !ChartAvailable(chart)) chart = g_secDefault[sec];
    }
    bool same = g_sec.sec == sec;
    g_sec.sec = sec;
    g_sec.mainC = chart;
    if (!same) { g_sec.scroll = 0; g_sec.hoverRow = -1; }
    g_sec.hoverMini = -1;
    if (!g_renderTest && g_sh->secChart[sec] != chart) {
        g_sh->secChart[sec] = chart;
        g_cfg.secChart[sec] = chart;
        Cfg_SaveSectionCharts(g_cfg.secChart);
        HWND res = (HWND)(LONG_PTR)g_sh->residentHwnd;
        if (res) PostMessageW(res, WM_GS_VIEWER, VEV_STRIP, 0);
    }
    if (!g_renderTest) g_sh->lastFocus = Sec_ChartGraph(chart);
    Sec_UpdateWants();
    Sec_SetTitle();
    Sec_Dirty();
}
// arg: a graph (G_*, from the overview), 100 + a chart (C_*, from the strip) or 200 + a section (tray menu)
static void Section_Open(int arg) {
    int sec, chart;
    if (arg >= 200 && arg < 200 + SEC_COUNT) { sec = arg - 200; chart = -1; }
    else if (arg >= 100 && arg < 100 + C_COUNT) { chart = arg - 100; sec = g_charts[chart].sec; }
    else { if (arg < 0 || arg >= G_COUNT) arg = G_MEM; chart = ChartOfGraph(arg); sec = g_charts[chart].sec; }
    int graph = chart >= 0 ? Sec_ChartGraph(chart) : g_charts[g_secDefault[sec]].graph;
    IniPutInt(L"LastGraph", graph);
    if (!g_sec.hwnd) {
        HMONITOR mon = MonitorFromPoint([] { POINT p; GetCursorPos(&p); return p; }(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = {}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
        UINT dpi = MonitorDpi(mon);
        int w = MulDiv(1240, dpi, 96), h = MulDiv(800, dpi, 96);
        RECT wa = mi.rcWork;
        w = std::min(w, (int)(wa.right - wa.left) - 40); h = std::min(h, (int)(wa.bottom - wa.top) - 40);
        int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) / 2;
        if (g_cfg.secRectValid) {
            RECT r = g_cfg.secRect;
            HMONITOR m2 = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
            if (m2) { x = r.left; y = r.top; w = r.right - r.left; h = r.bottom - r.top; }
        }
        g_sec.hwnd = CreateWindowExW((g_cfg.secPinned ? WS_EX_TOPMOST : 0) | WS_EX_NOREDIRECTIONBITMAP, L"GlassStackSection", L"GlassStack", WS_OVERLAPPEDWINDOW,
                                     x, y, w, h, nullptr, nullptr, g_hInst, nullptr);
        if (!g_sec.hwnd) return;
        Sec_ApplyTheme();
    }
    Sec_SwitchTo(sec, chart);
    if (!IsWindowVisible(g_sec.hwnd)) Sec_Present();      // the first frame is ready before the window appears
    if (IsIconic(g_sec.hwnd)) ShowWindow(g_sec.hwnd, SW_RESTORE);
    else ShowWindow(g_sec.hwnd, SW_SHOW);
    SetForegroundWindow(g_sec.hwnd);
}
#define SEC_T_DATA 21
static void Section_OnSample() {
    Sh_ReadSnap(g_sec.snap);
    if (!g_sec.hwnd || IsIconic(g_sec.hwnd)) return;
    // the detail collector reports right after each sample: draw once, when it does (400 ms at the latest; details
    // that come later wait for the next sample, so a busy machine still gets one frame per second)
    g_sec.tickDrawn = false;
    if (g_detWant.load()) SetTimer(g_sec.hwnd, SEC_T_DATA, 400, nullptr);
    else Sec_Dirty();
}
static void Section_OnDetail() {
    // details came in: they show from the next frame
    // the resident keeps the history of what the detail collector measures here (one writer per series)
    g_sh->exPing = (float)g_det.ping; g_sh->exSig = (float)g_det.wsig;
    g_sh->exTemp = (float)g_det.gpuTemp; g_sh->exPower = (float)g_det.gpuPower;
    g_sh->exTick = (LONG)g_sh->snap.tick;
    if (g_sec.hwnd && !IsIconic(g_sec.hwnd)) { KillTimer(g_sec.hwnd, SEC_T_DATA); if (!g_sec.tickDrawn) Sec_Dirty(); }
}
static void Sec_SaveRect() {
    WINDOWPLACEMENT wp = {}; wp.length = sizeof(wp);
    if (!GetWindowPlacement(g_sec.hwnd, &wp)) return;
    RECT r = wp.rcNormalPosition;
    wchar_t b[96]; swprintf(b, 96, L"%ld,%ld,%ld,%ld", r.left, r.top, r.right, r.bottom);
    IniPut(L"SectionWindow", b);
    g_cfg.secRect = r; g_cfg.secRectValid = true;
}
static void Sec_ProcMenu(int row) {
    if (row < 0 || row >= g_sec.rowN) return;
    int idx = g_sec.rowIdx[row];
    if (idx < 0 || idx >= (int)g_det.procs.size()) return;
    ProcRow p = g_det.procs[idx];
    HMENU m = CreatePopupMenu();
    wchar_t t[160];
    swprintf(t, 160, p.count > 1 ? L"End task (%d processes)" : L"End task", p.count);
    AppendMenuW(m, MF_STRING, 1, t);
    AppendMenuW(m, MF_STRING, 2, L"Open file location");
    AppendMenuW(m, MF_STRING, 3, L"Copy name");
    POINT pt; GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_sec.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) {
        swprintf(t, 160, L"End %s%s?\n\nUnsaved work in it will be lost.", p.name.c_str(), p.count > 1 ? L" (every instance)" : L"");
        if (MessageBoxW(g_sec.hwnd, t, L"GlassStack", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
        int fail = 0;
        for (DWORD pid : p.pids) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (!h || !TerminateProcess(h, 1)) fail++;
            if (h) CloseHandle(h);
        }
        if (fail) {
            swprintf(t, 160, L"%d of %d could not be ended (it runs with higher rights than GlassStack).", fail, (int)p.pids.size());
            MessageBoxW(g_sec.hwnd, t, L"GlassStack", MB_OK | MB_ICONINFORMATION);
        }
    } else if (cmd == 2) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.bigPid);
        wchar_t path[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
        if (h && QueryFullProcessImageNameW(h, 0, path, &n)) {
            wchar_t args[MAX_PATH * 2 + 16]; swprintf(args, MAX_PATH * 2 + 16, L"/select,\"%s\"", path);
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args, nullptr, SW_SHOWNORMAL);
        }
        if (h) CloseHandle(h);
    } else if (cmd == 3) {
        if (OpenClipboard(g_sec.hwnd)) {
            EmptyClipboard();
            size_t bytes = (p.name.size() + 1) * sizeof(wchar_t);
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (g) { memcpy(GlobalLock(g), p.name.c_str(), bytes); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
            CloseClipboard();
        }
    }
}
static void Sec_HitHover(float x, float y) {
    int ht = -1, hr = -1, hrow = -1;
    for (int s = 0; s < SEC_COUNT; s++) if (InRect(g_sec.tabR[s], x, y)) ht = s;
    for (int i = 0; i < 5; i++) if (InRect(g_sec.rangeR[i], x, y)) hr = i;
    for (int k = 0; k < g_sec.rowN; k++) if (InRect(g_sec.rowR[k], x, y)) hrow = k;
    bool hp = InRect(g_sec.pinR, x, y), ho = InRect(g_sec.ovR, x, y);
    bool chart = InRect(g_sec.chartR, x, y);
    int hm = -1;
    for (int k = 0; k < g_sec.miniN; k++) if (InRect(g_sec.miniR[k], x, y)) hm = k;
    bool changed = ht != g_sec.hoverTab || hr != g_sec.hoverRange || hrow != g_sec.hoverRow || hp != g_sec.hoverPin || ho != g_sec.hoverOv || hm != g_sec.hoverMini;
    g_sec.hoverTab = ht; g_sec.hoverRange = hr; g_sec.hoverRow = hrow; g_sec.hoverPin = hp; g_sec.hoverOv = ho; g_sec.hoverMini = hm;
    if (changed) Sec_Dirty();
    else if (chart || g_sec.hoverWasChart) InvalidateRect(g_sec.hwnd, nullptr, FALSE);   // only the crosshair moves
    g_sec.hoverWasChart = chart;
}

static void Overlay_OpenPersistent();
static LRESULT CALLBACK Sec_Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    float s = 96.0f / DpiOf(h);
    switch (msg) {
    case WM_PAINT:
        ValidateRect(h, nullptr);
        if (g_sec.hwnd == h) Sec_Present();
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (wp == SEC_T_DATA) { KillTimer(h, SEC_T_DATA); g_sec.tickDrawn = true; Sec_Dirty(); }
        return 0;
    case WM_SIZE:
        Sec_Dirty();            // the next paint makes a frame surface of the new size
        return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Sec_ReleaseRT();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        UINT dpi = DpiOf(h);
        mm->ptMinTrackSize.x = MulDiv(960, dpi, 96); mm->ptMinTrackSize.y = MulDiv(620, dpi, 96);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_sec.hoverMini >= 0) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_MOUSEMOVE:
        if (!g_sec.tracking) { TRACKMOUSEEVENT t = {}; t.cbSize = sizeof(t); t.dwFlags = TME_LEAVE; t.hwndTrack = h; TrackMouseEvent(&t); g_sec.tracking = true; }
        g_sec.mx = GX(lp) * s; g_sec.my = GY(lp) * s; g_sec.mouseIn = true;
        Sec_HitHover(g_sec.mx, g_sec.my);
        return 0;
    case WM_MOUSELEAVE:
        g_sec.tracking = false; g_sec.mouseIn = false;
        g_sec.hoverTab = g_sec.hoverRange = g_sec.hoverRow = -1; g_sec.hoverPin = g_sec.hoverOv = false;
        Sec_Dirty();
        return 0;
    case WM_LBUTTONUP: {
        float x = GX(lp) * s, y = GY(lp) * s;
        for (int t = 0; t < SEC_COUNT; t++) if (InRect(g_sec.tabR[t], x, y)) { Sec_SwitchTo(t, -1); return 0; }
        for (int k = 0; k < g_sec.miniN; k++) if (InRect(g_sec.miniR[k], x, y)) { Sec_SwitchTo(g_sec.sec, g_sec.miniC[k]); return 0; }
        for (int i = 0; i < 5; i++) if (InRect(g_sec.rangeR[i], x, y)) { g_cfg.rangeSec = g_ranges[i]; IniPutInt(L"SectionRangeSeconds", g_cfg.rangeSec); Sec_Dirty(); return 0; }
        if (InRect(g_sec.pinR, x, y)) {
            g_cfg.secPinned = !g_cfg.secPinned;
            IniPutInt(L"SectionPinned", g_cfg.secPinned);
            SetWindowPos(h, g_cfg.secPinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            Sec_Dirty();
            return 0;
        }
        if (InRect(g_sec.ovR, x, y)) {          // the overview runs in its own process, started by the resident
            AllowSetForegroundWindow(ASFW_ANY);
            HWND res = (HWND)(LONG_PTR)g_sh->residentHwnd;
            if (res) PostMessageW(res, WM_GS_CMD, CMD_OVERVIEW, 0);
            else Overlay_OpenPersistent();
            return 0;
        }

        return 0;
    }
    case WM_RBUTTONUP: {
        float x = GX(lp) * s, y = GY(lp) * s;
        for (int k = 0; k < g_sec.rowN; k++) if (InRect(g_sec.rowR[k], x, y)) { Sec_ProcMenu(k); return 0; }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt = { GX(lp), GY(lp) }; ScreenToClient(h, &pt);
        if (InRect(g_sec.procR, pt.x * s, pt.y * s)) {
            g_sec.scroll += GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -3 : 3;
            if (g_sec.scroll < 0) g_sec.scroll = 0;
            Sec_Dirty();
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || (wp == 'W' && (GetKeyState(VK_CONTROL) & 0x8000))) { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
        if (wp == VK_LEFT || (wp == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000))) { Sec_SwitchTo((g_sec.sec + SEC_COUNT - 1) % SEC_COUNT, -1); return 0; }
        if (wp == VK_RIGHT || wp == VK_TAB) { Sec_SwitchTo((g_sec.sec + 1) % SEC_COUNT, -1); return 0; }
        if (wp >= '1' && wp < '1' + SEC_COUNT) { Sec_SwitchTo((int)(wp - '1'), -1); return 0; }
        return 0;
    case WM_CLOSE:
        Sec_SaveRect();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_sec.hwnd == h) {
            Sec_ReleaseRT();
            g_sec.hwnd = nullptr;
            Sec_UpdateWants();
            Viewer_CheckIdle();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
