// GlassStack - the slim strip docked on top of the taskbar (resident process).
// Registered as an appbar so maximized windows end above it. Drawn with plain GDI into a DIB:
// no GPU, no Direct2D. One slot per graph, colour = graph identity, no text.
#pragma once

static void Res_OpenSection(int slot);
static void Res_ShowMenu();

// optional cost log (Diag=1 in the ini): milliseconds spent per part, written once a minute
static bool g_diagOn = false;
static double g_dg[8];
static inline double DiagNow() { LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f); return (double)q.QuadPart * 1000.0 / (double)f.QuadPart; }

static HWND g_bar = nullptr, g_pop = nullptr;
static bool g_barReg = false, g_barFsHidden = false, g_barTrack = false, g_barInRepos = false, g_barAutoHideTb = false;
static int g_barHover = -1;
static HDC g_barDC = nullptr; static HBITMAP g_barBmp = nullptr; static HGDIOBJ g_barOld = nullptr;
static DWORD* g_barPx = nullptr; static int g_barBW = 0, g_barBH = 0;
static UINT g_barEdge = ABE_BOTTOM;
static Col g_barBg = { 0.95f, 0.95f, 0.95f };
static bool g_barBgSampled = false;
static std::vector<Col> g_barCols;          // the taskbar's colour under every pixel column (its Mica tint varies along it)
static int g_barColsX0 = 0, g_barColOff = 0;

static inline DWORD PackCol(Col c) {
    return 0xFF000000u | ((DWORD)(Clamp01(c.r) * 255 + 0.5f) << 16) | ((DWORD)(Clamp01(c.g) * 255 + 0.5f) << 8) | (DWORD)(Clamp01(c.b) * 255 + 0.5f);
}
static inline void BlendPx(DWORD* p, Col c, float a) {
    if (a <= 0) return;
    if (a > 1) a = 1;
    DWORD d = *p;
    float r = ((d >> 16) & 255) / 255.f, g = ((d >> 8) & 255) / 255.f, b = (d & 255) / 255.f;
    r += (c.r - r) * a; g += (c.g - g) * a; b += (c.b - b) * a;
    *p = 0xFF000000u | ((DWORD)(r * 255 + 0.5f) << 16) | ((DWORD)(g * 255 + 0.5f) << 8) | (DWORD)(b * 255 + 0.5f);
}
static float Lum(Col c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

// The taskbar is Mica: its tint follows the wallpaper and changes along its length. One BitBlt of a
// 3-row band just inside its edge gives the colour under every column; a wide running median removes
// icons and highlighted buttons, a blur smooths the rest. The strip then continues the taskbar's surface.
static void Bar_SampleTaskbar() {
    g_dark = TaskbarDark();
    g_barBg = g_dark ? Hex(0x1C1C1C) : Hex(0xEEEEEE);
    g_barBgSampled = false;
    std::vector<Col>().swap(g_barCols);
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT r, vis;
    if (!tray || !GetWindowRect(tray, &r)) return;
    HMONITOR mon = MonitorFromWindow(tray, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
    if (!IntersectRect(&vis, &r, &mi.rcMonitor) || vis.bottom - vis.top < 8 || vis.right - vis.left < 200) return;   // auto-hidden
    const int rows = 3;
    int border = (int)ceilf(MonitorDpi(mon) / 96.0f) + 1;
    bool atTop = r.top <= mi.rcMonitor.top && r.bottom < mi.rcMonitor.bottom;
    int W = vis.right - vis.left, y0 = atTop ? vis.bottom - border - rows : vis.top + border;
    std::vector<DWORD> px((size_t)W * rows);
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -rows;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ o = SelectObject(mem, bmp);
        ok = BitBlt(mem, 0, 0, W, rows, scr, vis.left, y0, SRCCOPY) != FALSE;
        GdiFlush();
        if (ok) memcpy(px.data(), bits, px.size() * 4);
        SelectObject(mem, o);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem); ReleaseDC(nullptr, scr);
    if (!ok) return;
    auto toCol = [](DWORD d) { Col c = { ((d >> 16) & 255) / 255.f, ((d >> 8) & 255) / 255.f, (d & 255) / 255.f }; return c; };
    std::vector<Col> col(W);
    for (int x = 0; x < W; x++) {          // middle of the three rows by luminance
        Col a = toCol(px[x]), b = toCol(px[(size_t)W + x]), c = toCol(px[(size_t)2 * W + x]);
        float la = Lum(a), lb = Lum(b), lc = Lum(c);
        col[x] = ((la <= lb) == (lb <= lc)) ? b : (((lb <= la) == (la <= lc)) ? a : c);
    }
    const int step = 4, half = 80;
    int n = (W + step - 1) / step;
    std::vector<Col> coarse(n);
    std::vector<std::pair<float, int>> win;
    for (int i = 0; i < n; i++) {
        int cx = i * step, a0 = std::max(0, cx - half), a1 = std::min(W - 1, cx + half);
        win.clear();
        for (int x = a0; x <= a1; x += step) win.push_back({ Lum(col[x]), x });
        std::nth_element(win.begin(), win.begin() + win.size() / 2, win.end());
        coarse[i] = col[win[win.size() / 2].second];
    }
    std::vector<float> ls(n);
    for (int i = 0; i < n; i++) ls[i] = Lum(coarse[i]);
    std::nth_element(ls.begin(), ls.begin() + n / 2, ls.end());
    float l = ls[n / 2];
    if (!((g_dark && l < 0.35f) || (!g_dark && l > 0.6f))) return;   // something covers the taskbar
    std::vector<Col> fine(W);
    for (int x = 0; x < W; x++) {
        float f = (float)x / step;
        int i0 = std::min(n - 1, (int)f), i1 = std::min(n - 1, i0 + 1);
        fine[x] = Mix(coarse[i0], coarse[i1], f - (float)i0);
    }
    const int rad = 24;                    // box blur
    g_barCols.resize(W);
    float sr = 0, sg = 0, sb = 0; int cnt = 0;
    for (int x = 0; x < std::min(W, rad); x++) { sr += fine[x].r; sg += fine[x].g; sb += fine[x].b; cnt++; }
    for (int x = 0; x < W; x++) {
        int add = x + rad, rem = x - rad - 1;
        if (add < W) { sr += fine[add].r; sg += fine[add].g; sb += fine[add].b; cnt++; }
        if (rem >= 0) { sr -= fine[rem].r; sg -= fine[rem].g; sb -= fine[rem].b; cnt--; }
        Col c = { sr / cnt, sg / cnt, sb / cnt };
        g_barCols[x] = c;
    }
    g_barColsX0 = vis.left;
    g_barBg = g_barCols[W / 2];
    g_barBgSampled = true;
}

// 0..1 height inside a slot. Rates use a log scale so magnitude reads without any text; the floor keeps
// background chatter flat: disk 100 KB/s .. 8 GB/s, network 32 KB/s (256 Kbps) .. 400 MB/s (3.2 Gbps)
static float BarNorm(int s, UnitKind u, float v, double autoMx = 0) {
    if (isnan(v)) return -1;
    if (autoMx > 0) return Clamp01((float)(v / autoMx));
    if (u == U_DISK) { if (v < 1) return 0; return Clamp01((log10f(v) - 5.0f) / (9.9f - 5.0f)); }
    if (u == U_NET)  { if (v < 1) return 0; return Clamp01((log10f(v) - 4.5f) / (8.6f - 4.5f)); }
    double mx = SeriesFixedMax(s);
    if (mx <= 0) mx = 100;
    return Clamp01((float)(v / mx));
}
static float SeriesLerp(int s, float ago) {
    int a0 = (int)ago; float t = ago - a0;
    float v0 = SER(s).at(a0);
    if (isnan(v0)) return NAN;
    float v1 = SER(s).at(a0 + 1);
    if (isnan(v1)) return v0;
    return v0 + (v1 - v0) * t;
}
static void Bar_Span(DWORD* px, int W, int H, int x, float ya, float yb, Col c, float a) {
    if (yb - ya <= 0.01f) return;
    int r0 = (int)floorf(ya), r1 = (int)ceilf(yb) - 1;
    if (r0 < 0) r0 = 0;
    if (r1 > H - 1) r1 = H - 1;
    for (int r = r0; r <= r1; r++) {
        float cov = std::min(yb, (float)r + 1) - std::max(ya, (float)r);
        if (cov > 0) BlendPx(&px[r * W + x], c, a * cov);
    }
}
static void Bar_Geometry(int W, float sc, int& gap, int& slotW) {
    int n = g_cfg.nGraphs;
    gap = std::max(2, (int)lroundf(3 * sc));
    slotW = n > 0 ? (W - gap * (n + 1)) / n : 0;
}
static bool ChartWarn(int c) {
    const ChartDef& d = g_charts[c];
    if (d.unit != U_GB) return false;
    float v = SER(d.sA).at(0);
    double mx = SeriesFixedMax(d.sA);
    return !isnan(v) && mx > 0 && v / mx > 0.9;
}
static inline Col Bar_BgAt(int x) {
    if (g_barCols.empty()) return g_barBg;
    int i = x + g_barColOff;
    return g_barCols[std::max(0, std::min((int)g_barCols.size() - 1, i))];
}
static void Bar_RenderPixels(DWORD* px, int W, int H, float sc, int hover) {
    for (int x = 0; x < W; x++) {
        DWORD p = PackCol(Bar_BgAt(x));
        for (int y = 0; y < H; y++) px[y * W + x] = p;
    }
    int n = g_cfg.nGraphs, gap, slotW;
    Bar_Geometry(W, sc, gap, slotW);
    if (n <= 0 || slotW < 8) return;
    int pps = std::max(1, (int)lroundf(g_cfg.stripPps * sc));
    float inset = std::max(1.0f, roundf(sc * 0.75f));
    for (int i = 0; i < n; i++) {
        int ch = StripChart(i);
        const ChartDef& d = g_charts[ch];
        int x0 = gap + i * (slotW + gap);
        Col c = ChartColA(ch);
        Col cB = ChartColB(ch);
        bool warn = ChartWarn(ch);
        // charts without a fixed scale (latency, response time) are scaled to what the slot shows
        double autoMx = (SeriesFixedMax(d.sA) > 0 || d.unit == U_DISK || d.unit == U_NET) ? 0 : ChartMax(d.sA, d.sB, d.unit, slotW / pps + 2);
        for (int x = 0; x < slotW; x++) {
            Col b = Bar_BgAt(x0 + x);
            Col sb = g_dark ? Mix(b, WHITE, 0.06f) : Mix(b, BLACK, 0.045f);
            if (i == hover) sb = Mix(sb, c, 0.18f);
            if (warn) sb = Mix(sb, CriticalCol(), 0.30f);
            DWORD sbp = PackCol(sb);
            for (int y = 0; y < H; y++) px[y * W + x0 + x] = sbp;
        }
        float top = inset, bot = (float)H - inset, hgt = bot - top;
        bool dual = d.sB >= 0;
        if (d.unit == U_GB && !dual) {
            // capacity (memory, video memory): a gauge filled as far as it is used. Its history is nearly flat, so as a
            // time graph it would read as a full bar whatever the level.
            float nv = BarNorm(d.sA, d.unit, SER(d.sA).at(0));
            if (nv < 0) continue;
            float fx = nv * slotW, ew = std::max(1.0f, sc);
            for (int x = 0; x < slotW; x++) {
                float fill = std::min(1.0f, std::max(0.0f, fx - x));
                float edge = std::max(0.0f, std::min((float)x + 1, fx) - std::max((float)x, fx - ew));
                if (fill > 0) Bar_Span(px, W, H, x0 + x, top, bot, c, (g_dark ? 0.52f : 0.44f) * fill);
                if (edge > 0) Bar_Span(px, W, H, x0 + x, top, bot, c, 0.95f * edge);
            }
            continue;
        }
        float mid = (top + bot) * 0.5f;
        if (dual) { int my = (int)mid; if (my >= 0 && my < H) for (int x = 0; x < slotW; x++) BlendPx(&px[my * W + x0 + x], c, 0.10f); }
        for (int x = 0; x < slotW; x++) {
            float ago = (float)(slotW - 1 - x) / pps;
            if (ago > HIST - 2) continue;
            int X = x0 + x;
            if (!dual) {
                float nv = BarNorm(d.sA, d.unit, SeriesLerp(d.sA, ago), autoMx);
                if (nv < 0) continue;
                float y = bot - nv * hgt;
                Bar_Span(px, W, H, X, y, bot, c, g_dark ? 0.45f : 0.38f);
                Bar_Span(px, W, H, X, y - 0.5f, std::min(y + 1.0f, bot), c, 0.95f);
            } else {
                float half = hgt * 0.5f;
                float na = BarNorm(d.sA, d.unit, SeriesLerp(d.sA, ago), autoMx);
                float nb = BarNorm(d.sB, d.unit, SeriesLerp(d.sB, ago), autoMx);
                if (na >= 0) { float y = mid - na * half; Bar_Span(px, W, H, X, y, mid, c, 0.45f); Bar_Span(px, W, H, X, y - 0.4f, std::min(y + 0.9f, mid), c, 0.95f); }
                if (nb >= 0) { float y = mid + nb * half; Bar_Span(px, W, H, X, mid, y, cB, 0.45f); Bar_Span(px, W, H, X, std::max(y - 0.9f, mid), y + 0.4f, cB, 0.95f); }
            }
        }
    }
}

static void Bar_FreeDib() {
    if (g_barDC) { SelectObject(g_barDC, g_barOld); DeleteObject(g_barBmp); DeleteDC(g_barDC); }
    g_barDC = nullptr; g_barBmp = nullptr; g_barPx = nullptr; g_barBW = g_barBH = 0;
}
static void Bar_EnsureDib(int w, int h) {
    if (g_barDC && g_barBW == w && g_barBH == h) return;
    Bar_FreeDib();
    HDC scr = GetDC(nullptr);
    g_barDC = CreateCompatibleDC(scr);
    ReleaseDC(nullptr, scr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_barBmp = CreateDIBSection(g_barDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    g_barPx = (DWORD*)bits;
    g_barOld = SelectObject(g_barDC, g_barBmp);
    g_barBW = w; g_barBH = h;
}
static void Bar_Paint(HDC dc) {
    RECT r; GetClientRect(g_bar, &r);
    int w = r.right, h = r.bottom;
    if (w <= 0 || h <= 0) return;
    Bar_EnsureDib(w, h);
    if (!g_barPx) return;
    RECT wr; GetWindowRect(g_bar, &wr);
    g_barColOff = wr.left - g_barColsX0;
    GdiFlush();
    double t0 = g_diagOn ? DiagNow() : 0;
    Bar_RenderPixels(g_barPx, w, h, DpiOf(g_bar) / 96.0f, g_barHover);
    double t1 = g_diagOn ? DiagNow() : 0;
    BitBlt(dc, 0, 0, w, h, g_barDC, 0, 0, SRCCOPY);
    if (g_diagOn) { g_dg[1] += t1 - t0; g_dg[5] += DiagNow() - t1; g_dg[6] += 1; }
}

static void Bar_Reposition() {
    if (!g_bar || g_barInRepos) return;
    g_barInRepos = true;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    HMONITOR mon = tray ? MonitorFromWindow(tray, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
    UINT dpi = MonitorDpi(mon);
    int h = std::max(3, (int)lroundf(g_cfg.stripHeight * dpi / 96.0f));
    APPBARDATA tb = {}; tb.cbSize = sizeof(tb);
    g_barEdge = ABE_BOTTOM;
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &tb) && tb.uEdge == ABE_TOP) g_barEdge = ABE_TOP;
    APPBARDATA st = {}; st.cbSize = sizeof(st);
    g_barAutoHideTb = (SHAppBarMessage(ABM_GETSTATE, &st) & ABS_AUTOHIDE) != 0;
    APPBARDATA ab = {}; ab.cbSize = sizeof(ab); ab.hWnd = g_bar; ab.uEdge = g_barEdge; ab.rc = mi.rcMonitor;
    if (g_barEdge == ABE_BOTTOM) ab.rc.top = ab.rc.bottom - h; else ab.rc.bottom = ab.rc.top + h;
    if (g_barReg) {
        SHAppBarMessage(ABM_QUERYPOS, &ab);
        if (g_barEdge == ABE_BOTTOM) ab.rc.top = ab.rc.bottom - h; else ab.rc.bottom = ab.rc.top + h;
        SHAppBarMessage(ABM_SETPOS, &ab);
    } else {
        ab.rc.left = mi.rcMonitor.left; ab.rc.right = mi.rcMonitor.right;
        if (g_barEdge == ABE_BOTTOM) { ab.rc.bottom = mi.rcWork.bottom; ab.rc.top = ab.rc.bottom - h; }
        else { ab.rc.top = mi.rcWork.top; ab.rc.bottom = ab.rc.top + h; }
    }
    // an auto-hidden taskbar needs its 2 px trigger line at the screen edge
    if (g_barAutoHideTb) {
        int off = std::max(2, (int)lroundf(2 * dpi / 96.0f));
        if (g_barEdge == ABE_BOTTOM && ab.rc.bottom > mi.rcMonitor.bottom - off) OffsetRect(&ab.rc, 0, -off);
        if (g_barEdge == ABE_TOP && ab.rc.top < mi.rcMonitor.top + off) OffsetRect(&ab.rc, 0, off);
    }
    SetWindowPos(g_bar, HWND_TOPMOST, ab.rc.left, ab.rc.top, ab.rc.right - ab.rc.left, ab.rc.bottom - ab.rc.top, SWP_NOACTIVATE);
    InvalidateRect(g_bar, nullptr, FALSE);
    g_barInRepos = false;
}
static void Bar_Create() {
    if (g_bar) return;
    Bar_SampleTaskbar();
    g_bar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"GlassStackBar", L"GlassStack strip",
                            WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g_hInst, nullptr);
    if (!g_bar) return;
    APPBARDATA ab = {}; ab.cbSize = sizeof(ab); ab.hWnd = g_bar; ab.uCallbackMessage = WM_GS_APPBAR;
    g_barReg = SHAppBarMessage(ABM_NEW, &ab) != 0;
    Bar_Reposition();
    ShowWindow(g_bar, SW_SHOWNOACTIVATE);
}
static void Pop_Hide() { if (g_pop) ShowWindow(g_pop, SW_HIDE); }
static void Bar_Destroy() {
    if (!g_bar) return;
    if (g_barReg) { APPBARDATA ab = {}; ab.cbSize = sizeof(ab); ab.hWnd = g_bar; SHAppBarMessage(ABM_REMOVE, &ab); g_barReg = false; }
    Pop_Hide();
    DestroyWindow(g_bar); g_bar = nullptr;
    if (g_pop) { DestroyWindow(g_pop); g_pop = nullptr; }
    Bar_FreeDib();
}
static void Bar_ReRegister() {   // after Explorer restarts
    if (!g_bar) return;
    APPBARDATA ab = {}; ab.cbSize = sizeof(ab); ab.hWnd = g_bar; ab.uCallbackMessage = WM_GS_APPBAR;
    if (g_barReg) SHAppBarMessage(ABM_REMOVE, &ab);
    g_barReg = SHAppBarMessage(ABM_NEW, &ab) != 0;
    Bar_SampleTaskbar();
    Bar_Reposition();
}
static int Bar_SlotAt(int x) {
    RECT r; GetClientRect(g_bar, &r);
    int n = g_cfg.nGraphs, gap, slotW;
    Bar_Geometry(r.right, DpiOf(g_bar) / 96.0f, gap, slotW);
    if (n <= 0 || slotW <= 0) return -1;
    int i = (x - gap / 2) / (slotW + gap);
    return std::max(0, std::min(n - 1, i));
}

// ---------------------------------------------------------------- hover readout: every value, the hovered one highlighted
static void Pop_PaintTo(HDC dc, int W, int H, float sc, int hover) {
    Col bgc = g_dark ? Hex(0x2B2B2B) : Hex(0xF9F9F9);
    HBRUSH hb = CreateSolidBrush(ToCref(bgc)); RECT all = { 0, 0, W, H }; FillRect(dc, &all, hb); DeleteObject(hb);
    HFONT f = CreateFontW(-(int)lroundf(12.5f * sc), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT fb = CreateFontW(-(int)lroundf(12.5f * sc), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT fs = CreateFontW(-(int)lroundf(11.0f * sc), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    int pad = (int)lroundf(12 * sc), rowH = (int)lroundf(25 * sc), headH = (int)lroundf(22 * sc);
    for (int i = 0; i < g_cfg.nGraphs; i++) {
        int ch = StripChart(i);
        Col c = ChartColA(ch);
        int y = pad + i * rowH;
        if (i == hover) {
            HBRUSH b2 = CreateSolidBrush(ToCref(Mix(bgc, c, 0.14f)));
            RECT rr = { (int)(5 * sc), y, W - (int)(5 * sc), y + rowH };
            FillRect(dc, &rr, b2); DeleteObject(b2);
        }
        HBRUSH sw = CreateSolidBrush(ToCref(c));
        int s = (int)lroundf(10 * sc);
        RECT rs = { pad, y + (rowH - s) / 2, pad + s, y + (rowH - s) / 2 + s };
        FillRect(dc, &rs, sw); DeleteObject(sw);
        SelectObject(dc, i == hover ? fb : f);
        SetTextColor(dc, ToCref(i == hover ? InkCol() : Ink2Col()));
        RECT rt = { pad + s + (int)(10 * sc), y, W / 2 + (int)(14 * sc), y + rowH };
        DrawTextW(dc, ChartTitle(ch), -1, &rt, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        wchar_t v[96]; ChartValueText(ch, v, 96);
        SelectObject(dc, i == hover ? fb : f);
        SetTextColor(dc, ToCref(InkCol()));
        RECT rv = { W / 2 - (int)(20 * sc), y, W - pad, y + rowH };
        DrawTextW(dc, v, -1, &rv, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }
    // footer hint
    SelectObject(dc, fs);
    SetTextColor(dc, ToCref(Ink3Col()));
    wchar_t hint[160];
    swprintf(hint, 160, L"Click for details  ·  %s for the 3D view", g_cfg.hkText);
    RECT rh = { pad, H - headH - (int)(6 * sc), W - pad, H - (int)(6 * sc) };
    DrawTextW(dc, hint, -1, &rh, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
    DeleteObject(f); DeleteObject(fb); DeleteObject(fs);
}
static void Pop_Size(float sc, int& W, int& H) {
    W = (int)lroundf(360 * sc);
    H = (int)lroundf(12 * sc) + (int)lroundf(25 * sc) * g_cfg.nGraphs + (int)lroundf(34 * sc);
}
static void Pop_Show(int slot) {
    if (!g_pop) {
        g_pop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"GlassStackPop", L"",
                                WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g_hInst, nullptr);
        if (!g_pop) return;
        DWORD corner = 2; DwmSetWindowAttribute(g_pop, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    }
    float sc = DpiOf(g_bar) / 96.0f;
    int W, H; Pop_Size(sc, W, H);
    RECT br; GetWindowRect(g_bar, &br);
    int n = std::max(1, g_cfg.nGraphs);
    int cx = br.left + (int)((slot + 0.5f) * (br.right - br.left) / n);
    HMONITOR mon = MonitorFromWindow(g_bar, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
    int x = std::max((int)mi.rcMonitor.left + 8, std::min(cx - W / 2, (int)mi.rcMonitor.right - W - 8));
    int y = g_barEdge == ABE_BOTTOM ? br.top - H - (int)(8 * sc) : br.bottom + (int)(8 * sc);
    SetWindowPos(g_pop, HWND_TOPMOST, x, y, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_pop, nullptr, FALSE);
}
static LRESULT CALLBACK Pop_Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        HGDIOBJ o = SelectObject(mem, bmp);
        Pop_PaintTo(mem, r.right, r.bottom, DpiOf(h) / 96.0f, g_barHover);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, o); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return HTTRANSPARENT;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void Bar_SetFullscreen(bool fs) {
    if (!g_bar || fs == g_barFsHidden) return;
    g_barFsHidden = fs;
    if (fs) { Pop_Hide(); ShowWindow(g_bar, SW_HIDE); }
    else { ShowWindow(g_bar, SW_SHOWNOACTIVATE); SetWindowPos(g_bar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
}
static void Bar_CheckFullscreen() {
    if (!g_bar) return;
    bool fs = false;
    HWND fg = GetForegroundWindow();
    if (fg && fg != g_bar) {
        DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
        wchar_t cls[64] = L""; GetClassNameW(fg, cls, 64);
        bool shell = !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd");
        bool ours = pid == GetCurrentProcessId() || (g_sh && (pid == (DWORD)g_sh->viewerPid || pid == (DWORD)g_sh->overviewPid));
        if (!shell && !ours) {
            HMONITOR m = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
            if (m && m == MonitorFromWindow(g_bar, MONITOR_DEFAULTTONEAREST)) {
                MONITORINFO mi = {}; mi.cbSize = sizeof(mi); GetMonitorInfoW(m, &mi);
                RECT r; GetWindowRect(fg, &r);
                fs = r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
            }
        }
    }
    Bar_SetFullscreen(fs);
}
static void Bar_OnSample() {
    if (!g_bar || g_barFsHidden) return;
    InvalidateRect(g_bar, nullptr, FALSE);
    if (g_pop && IsWindowVisible(g_pop)) InvalidateRect(g_pop, nullptr, FALSE);
}

enum { BAR_POP_TIMER = 0x6B01, BAR_POP_DELAY = 1000 };  // ms the cursor must rest on a slot before the popup shows

static LRESULT CALLBACK Bar_Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); Bar_Paint(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_GS_APPBAR:
        if (wp == ABN_POSCHANGED) Bar_Reposition();
        else if (wp == ABN_FULLSCREENAPP) { if (lp) Bar_SetFullscreen(true); else Bar_CheckFullscreen(); }
        else if (wp == ABN_STATECHANGE) Bar_Reposition();
        return 0;
    case WM_ACTIVATE:
        if (g_barReg) { APPBARDATA ab = {}; ab.cbSize = sizeof(ab); ab.hWnd = h; SHAppBarMessage(ABM_ACTIVATE, &ab); }
        return 0;
    case WM_WINDOWPOSCHANGED:
        if (g_barReg) { APPBARDATA ab = {}; ab.cbSize = sizeof(ab); ab.hWnd = h; SHAppBarMessage(ABM_WINDOWPOSCHANGED, &ab); }
        break;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        Bar_Reposition();
        return 0;
    case WM_MOUSEMOVE: {
        if (!g_barTrack) { TRACKMOUSEEVENT t = {}; t.cbSize = sizeof(t); t.dwFlags = TME_LEAVE; t.hwndTrack = h; TrackMouseEvent(&t); g_barTrack = true; }
        int s = Bar_SlotAt(GX(lp));
        bool popUp = g_pop && IsWindowVisible(g_pop);
        if (s != g_barHover) {
            g_barHover = s; InvalidateRect(h, nullptr, FALSE);
            if (popUp) Pop_Show(s);                       // already open: follow the cursor at once
            else if (s >= 0) SetTimer(h, BAR_POP_TIMER, BAR_POP_DELAY, nullptr);  // restart the hover delay
            else KillTimer(h, BAR_POP_TIMER);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == BAR_POP_TIMER) {
            KillTimer(h, BAR_POP_TIMER);
            POINT pt; GetCursorPos(&pt); RECT r; GetWindowRect(h, &r);
            if (g_barHover >= 0 && PtInRect(&r, pt)) Pop_Show(g_barHover);
            return 0;
        }
        break;
    case WM_MOUSELEAVE:
        g_barTrack = false; g_barHover = -1; KillTimer(h, BAR_POP_TIMER); Pop_Hide(); InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int s = Bar_SlotAt(GX(lp));
        KillTimer(h, BAR_POP_TIMER); Pop_Hide();
        if (s >= 0) Res_OpenSection(s);
        return 0;
    }
    case WM_RBUTTONUP:
        KillTimer(h, BAR_POP_TIMER); Pop_Hide();
        Res_ShowMenu();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
