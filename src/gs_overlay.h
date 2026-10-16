// GlassStack - the isometric 3D overview: one frosted-glass card per graph, stacked like a deck (viewer process).
// Hold Ctrl+Shift and tap Space to move to the next card; release everything to open the focused card's section.
// A quick tap that is released within ~0.1 s skips the overview and opens the last section directly.
//
// Drawing: the backdrop, every card face and the text panels are drawn once into DirectComposition surfaces (in
// software, only when their content changes). Moving, skewing and fading the cards is then done by the desktop
// compositor on the GPU, so an animation frame costs this process a few transform updates. Ov_Render draws the
// same scene in one pass for the render test.
#pragma once

static void Section_Open(int graph);
static void Viewer_CheckIdle();
static void Viewer_DeviceLost();

static const float ISO_C = 0.8944272f;   // 2:1 isometric (26.57 degrees)
static const float ISO_S = 0.4472136f;
#define OV_T_ANIM 11
#define OV_T_KEYS 12
#define OV_T_SHOW 13
#define OV_T_HANDOFF 14
#define OV_SHOW_DELAY 110
#define OV_BG_DIV 4                       // the blurred backdrop is drawn at a quarter of the screen resolution
static ULONGLONG g_ovHandoff = 0;

struct DeckLayout { float k, ox, oy, CW, CH, sp, rise, slide, lift, thick, deckL, deckR, deckT, deckB; int n; };

// the composition tree, back to front: backdrop, focus-coloured glow, the cards (with the outline of the card that
// just lost focus fading out above it), the focused card and its outline, the three text panels
struct OvComp {
    IDCompositionDesktopDevice* dev = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual2* root = nullptr;
    IDCompositionVisual2* bg = nullptr;   IDCompositionVisual3* bg3 = nullptr;   IDCompositionSurface* bgS = nullptr;
    IDCompositionVisual2* blob = nullptr; IDCompositionVisual3* blob3 = nullptr; IDCompositionSurface* blobS = nullptr;
    IDCompositionVisual2* card[G_COUNT] = {}; IDCompositionVisual3* card3[G_COUNT] = {}; IDCompositionSurface* cardS[G_COUNT] = {};
    IDCompositionVisual2* glow[2] = {}; IDCompositionVisual3* glow3[2] = {}; IDCompositionSurface* glowS[2] = {};
    IDCompositionVisual2* hud[3] = {}; IDCompositionVisual3* hud3[3] = {}; IDCompositionSurface* hudS[3] = {};
    int hudX[3] = {}, hudY[3] = {};
    UINT hudW[3] = {}, hudH[3] = {};
    UINT bgW = 0, bgH = 0;
    int glowFor[2] = { -1, -1 };      // slot 0: the focused card, slot 1: the card that lost focus last
    float P = 1, m = 26;              // card surfaces: pixels per card unit, margin around the card in card units
    UINT cw = 0, ch = 0;
    int topCard = -1;
    bool lost = false;
};

struct OverlayState {
    HWND hwnd = nullptr;
    OvComp comp;
    RCtx c;
    bool hold = false, animating = false, committing = false, shown = false;
    LONG seenTaps = 0;
    int focus = 0, hover = -1;
    float pull[G_COUNT] = {}, hov[G_COUNT] = {}, appear = 0;
    LONGLONG lastQpc = 0;
    int bw = 0, bh = 0;
    std::vector<DWORD> back, glass;
    ID2D1Bitmap* bmpBack = nullptr; ID2D1Bitmap* bmpGlass = nullptr; ID2D1BitmapBrush* glassBrush = nullptr;
    RECT mon = {};
    float wDip = 0, hDip = 0, sc = 1;
    DeckLayout L = {};
    D2D1_MATRIX_3X2_F cardM[G_COUNT];
    D2D1_RECT_F idxR[G_COUNT] = {};
};
static OverlayState g_ov;

// ---------------------------------------------------------------- backdrop: a small, blurred copy of the screen
static void BoxBlur(std::vector<DWORD>& img, int w, int h, int r, int passes) {
    if (r < 1 || w < 2 || h < 2) return;
    std::vector<DWORD> tmp(img.size());
    const int cnt = 2 * r + 1;
    for (int p = 0; p < passes; p++) {
        for (int y = 0; y < h; y++) {
            const DWORD* row = &img[(size_t)y * w];
            DWORD* out = &tmp[(size_t)y * w];
            int sr = 0, sg = 0, sb = 0;
            for (int i = -r; i <= r; i++) { DWORD px = row[std::max(0, std::min(w - 1, i))]; sr += (px >> 16) & 255; sg += (px >> 8) & 255; sb += px & 255; }
            for (int x = 0; x < w; x++) {
                out[x] = 0xFF000000u | ((DWORD)(sr / cnt) << 16) | ((DWORD)(sg / cnt) << 8) | (DWORD)(sb / cnt);
                DWORD a = row[std::max(0, std::min(w - 1, x - r))], b = row[std::max(0, std::min(w - 1, x + r + 1))];
                sr += (int)((b >> 16) & 255) - (int)((a >> 16) & 255);
                sg += (int)((b >> 8) & 255) - (int)((a >> 8) & 255);
                sb += (int)(b & 255) - (int)(a & 255);
            }
        }
        for (int x = 0; x < w; x++) {
            int sr = 0, sg = 0, sb = 0;
            for (int i = -r; i <= r; i++) { DWORD px = tmp[(size_t)std::max(0, std::min(h - 1, i)) * w + x]; sr += (px >> 16) & 255; sg += (px >> 8) & 255; sb += px & 255; }
            for (int y = 0; y < h; y++) {
                img[(size_t)y * w + x] = 0xFF000000u | ((DWORD)(sr / cnt) << 16) | ((DWORD)(sg / cnt) << 8) | (DWORD)(sb / cnt);
                DWORD a = tmp[(size_t)std::max(0, std::min(h - 1, y - r)) * w + x], b = tmp[(size_t)std::max(0, std::min(h - 1, y + r + 1)) * w + x];
                sr += (int)((b >> 16) & 255) - (int)((a >> 16) & 255);
                sg += (int)((b >> 8) & 255) - (int)((a >> 8) & 255);
                sb += (int)(b & 255) - (int)(a & 255);
            }
        }
    }
}
static void Ov_FinishBackdrop() {
    for (auto& p : g_ov.back) p |= 0xFF000000u;
    g_ov.glass = g_ov.back;
    BoxBlur(g_ov.back, g_ov.bw, g_ov.bh, 2, 3);
    BoxBlur(g_ov.glass, g_ov.bw, g_ov.bh, 5, 3);
}
static void Ov_Capture() {
    int W = g_ov.mon.right - g_ov.mon.left, H = g_ov.mon.bottom - g_ov.mon.top;
    int bw = std::max(32, W / 8), bh = std::max(20, H / 8);
    g_ov.bw = bw; g_ov.bh = bh;
    g_ov.back.assign((size_t)bw * bh, 0xFF808080u);
    HDC scr = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = bw; bi.bmiHeader.biHeight = -bh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ o = SelectObject(mem, bmp);
        SetStretchBltMode(mem, HALFTONE);
        SetBrushOrgEx(mem, 0, 0, nullptr);
        StretchBlt(mem, 0, 0, bw, bh, scr, g_ov.mon.left, g_ov.mon.top, W, H, SRCCOPY);
        GdiFlush();
        memcpy(g_ov.back.data(), bits, (size_t)bw * bh * 4);
        SelectObject(mem, o);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    Ov_FinishBackdrop();
}
// render test and bench: a colourful fake desktop instead of the real screen
static void Ov_SynthBackdrop(int W, int H) {
    int bw = W / 8, bh = H / 8;
    g_ov.bw = bw; g_ov.bh = bh;
    g_ov.back.assign((size_t)bw * bh, 0);
    struct B { float x, y, r; UINT32 c; } blobs[] = {
        { 0.15f, 0.20f, 0.42f, 0x5B8CFF }, { 0.82f, 0.18f, 0.36f, 0xC07CFF },
        { 0.62f, 0.86f, 0.45f, 0x2CC9A8 }, { 0.30f, 0.82f, 0.30f, 0xFFA45C } };
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            float u = (float)x / bw, v = (float)y / bh;
            Col c = Mix(Hex(0xDCE6F6), Hex(0xEDE3F4), u * 0.7f + v * 0.3f);
            if (g_dark) c = Mix(Hex(0x101A33), Hex(0x2A1236), u * 0.7f + v * 0.3f);
            for (auto& b : blobs) {
                float dx = (u - b.x) * 1.6f, dy = v - b.y;
                float d = sqrtf(dx * dx + dy * dy) / b.r;
                if (d < 1) c = Mix(c, Hex(b.c), (1 - d) * (1 - d) * 0.75f);
            }
            if (u > 0.08f && u < 0.46f && v > 0.12f && v < 0.62f) c = Mix(c, g_dark ? Hex(0x202228) : WHITE, 0.85f);   // a window
            if (v > 0.955f) c = g_dark ? Hex(0x1C1C1C) : Hex(0xEEEEEE);                                               // taskbar
            g_ov.back[(size_t)y * bw + x] = 0xFF000000u | ToHex(c);
        }
    Ov_FinishBackdrop();
}
static void Ov_MakeBitmaps(RCtx& c) {
    SafeRelease(g_ov.glassBrush); SafeRelease(g_ov.bmpBack); SafeRelease(g_ov.bmpGlass);
    if (g_ov.back.empty() || !c.rt) return;
    D2D1_BITMAP_PROPERTIES bp;
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM; bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    bp.dpiX = bp.dpiY = 96.f;
    D2D1_SIZE_U sz = { (UINT32)g_ov.bw, (UINT32)g_ov.bh };
    c.rt->CreateBitmap(sz, g_ov.back.data(), (UINT32)g_ov.bw * 4, &bp, &g_ov.bmpBack);
    c.rt->CreateBitmap(sz, g_ov.glass.data(), (UINT32)g_ov.bw * 4, &bp, &g_ov.bmpGlass);
    if (g_ov.bmpGlass) {
        D2D1_BITMAP_BRUSH_PROPERTIES bbp = { D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR };
        c.rt->CreateBitmapBrush(g_ov.bmpGlass, &bbp, nullptr, &g_ov.glassBrush);
    }
}

// ---------------------------------------------------------------- deck geometry
// World: card i stands upright in the y-z plane at depth x = i*sp (later cards are nearer the viewer), raised by
// (n-1-i)*rise so every card's header shows above the one in front. Card local (lx, ly) -> world
// (x_i, CW - lx + yOff, z0 + CH - ly + lift) -> screen (2:1 isometric). Pulling a card lifts it straight up out of its
// own slot (slide 0), high enough that the empty slot shows below it.
static D2D1_MATRIX_3X2_F Ov_CardMatrix(const DeckLayout& L, int i, float pull, float k, float ox, float oy, float extraLift, float dx = 0) {
    float xi = i * L.sp + dx, z0 = (L.n - 1 - i) * L.rise;
    float yOff = -pull * L.slide, lift = pull * L.lift + extraLift;
    return MMake(k * ISO_C, -k * ISO_S, 0, k,
                 ox + k * ISO_C * (xi - L.CW - yOff),
                 oy + k * ((xi + L.CW + yOff) * ISO_S - z0 - L.CH - lift));
}
// card i -> screen DIPs for a pull amount and a drop (the pulled card also grows by 4 %)
static D2D1_MATRIX_3X2_F Ov_CardPose(int i, float pull, float drop) {
    const DeckLayout& L = g_ov.L;
    D2D1_MATRIX_3X2_F M = Ov_CardMatrix(L, i, pull, L.k, L.ox, L.oy, -drop);
    float s = 1.0f + 0.04f * pull;
    if (s != 1.0f) M = MMul(MMul(MMul(MTrans(-L.CW * 0.5f, -L.CH * 0.5f), MScale(s, s)), MTrans(L.CW * 0.5f, L.CH * 0.5f)), M);
    return M;
}
// where card i is now: while the overview appears, the cards drop in from above
static D2D1_MATRIX_3X2_F Ov_CardNow(int i) { return Ov_CardPose(i, g_ov.pull[i], (1 - g_ov.appear) * (60 + i * 14)); }
// the slab's back face sits a fixed distance behind the front face: in card units that is a constant shift
static D2D1_MATRIX_3X2_F Ov_BackShift() {
    const DeckLayout& L = g_ov.L;
    return MMul(Ov_CardMatrix(L, 0, 0, 1, 0, 0, 0, -L.thick), MInv(Ov_CardMatrix(L, 0, 0, 1, 0, 0, 0)));
}
static void Ov_Layout(float W, float H) {
    DeckLayout& L = g_ov.L;
    L.n = g_cfg.nGraphs;
    L.CW = 560; L.CH = 250; L.sp = 52; L.rise = 16; L.slide = 0; L.lift = 0.92f * L.CH; L.thick = 7;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < L.n; i++)
        for (int p = 0; p < 2; p++) {
            D2D1_MATRIX_3X2_F M = Ov_CardMatrix(L, i, (float)p, 1.0f, 0, 0, 0);
            float cx[4] = { 0, L.CW, 0, L.CW }, cy[4] = { 0, 0, L.CH, L.CH };
            for (int q = 0; q < 4; q++) {
                D2D1_POINT_2F pt = MApply(M, cx[q], cy[q]);
                minx = std::min(minx, pt.x); maxx = std::max(maxx, pt.x);
                miny = std::min(miny, pt.y); maxy = std::max(maxy, pt.y);
            }
        }
    float topM = 64, botM = 92;
    float availW = W * 0.54f, availH = H - topM - botM;
    L.k = std::min(availW / (maxx - minx), availH / (maxy - miny));
    if (L.k > 1.4f) L.k = 1.4f;
    L.ox = W * 0.5f - L.k * (minx + maxx) * 0.5f;
    L.oy = topM + availH * 0.5f - L.k * (miny + maxy) * 0.5f;
    L.deckL = L.ox + L.k * minx; L.deckR = L.ox + L.k * maxx;
    L.deckT = L.oy + L.k * miny; L.deckB = L.oy + L.k * maxy;
}

// headline value + one-line context of a graph
static void Ov_ValueTexts(int g, wchar_t* v, size_t nv, wchar_t* sub, size_t ns) {
    const GraphDef& d = g_graphs[g];
    float a = SER(d.sA).at(0);
    sub[0] = 0;
    if (d.sB < 0) {
        FmtVal(v, nv, d.unit, a);
        double fx = SeriesFixedMax(d.sA);
        if (d.unit == U_GB && !isnan(a) && fx > 0) swprintf(sub, ns, L"of %.1f GB  ·  %.0f%% used", fx, a / fx * 100);
        else if (d.unit == U_PCT) { double avg, mx; SeriesStats(d.sA, 120, avg, mx); if (!isnan(mx)) swprintf(sub, ns, L"2-min average %.0f%%  ·  peak %.0f%%", avg, mx); }
        return;
    }
    float b = SER(d.sB).at(0);
    double tot = (isnan(a) ? 0 : a) + (isnan(b) ? 0 : b);
    FmtVal(v, nv, d.unit, (isnan(a) && isnan(b)) ? NAN : tot);
}
// "● Read 12 MB/s   ● Write 3 MB/s" with coloured markers (the legend of a dual graph)
static void DualLegend(RCtx& c, int g, float x, float y0, float y1, float maxX, int tf, float op) {
    const GraphDef& d = g_graphs[g];
    wchar_t va[48], vb[48], t[96];
    FmtVal(va, 48, d.unit, SER(d.sA).at(0));
    FmtVal(vb, 48, d.unit, SER(d.sB).at(0));
    float cy = (y0 + y1) * 0.5f;
    swprintf(t, 96, L"%s %s", d.labA, va);
    FillCircle(c, x + 4, cy, 4, GraphCol(g), op);
    Txt(c, t, tf, x + 13, y0, maxX, y1, Ink2Col(), op, 0, 1);
    x += 13 + TextW(t, tf) + 16;
    swprintf(t, 96, L"%s %s", d.labB, vb);
    FillCircle(c, x + 4, cy, 4, GraphColB(g), op);
    Txt(c, t, tf, x + 13, y0, maxX, y1, Ink2Col(), op, 0, 1);
}

// ---------------------------------------------------------------- card drawing (card-local units: 0..CW x 0..CH)
// the coloured outline of the focused card (the current transform is the card face's)
static void Ov_DrawGlow(RCtx& c, Col col, float a) {
    const DeckLayout& L = g_ov.L;
    D2D1_ROUNDED_RECT rr = { RF(0, 0, L.CW, L.CH), 18, 18 };
    c.rt->DrawRoundedRectangle(&rr, SB(c, col, 0.95f * a), 2.2f, nullptr);
    D2D1_ROUNDED_RECT g1 = { RF(-4, -4, L.CW + 4, L.CH + 4), 22, 22 };
    c.rt->DrawRoundedRectangle(&g1, SB(c, col, 0.20f * a), 5.0f, nullptr);
    D2D1_ROUNDED_RECT g2 = { RF(-10, -10, L.CW + 10, L.CH + 10), 28, 28 };
    c.rt->DrawRoundedRectangle(&g2, SB(c, col, 0.07f * a), 9.0f, nullptr);
}
// M: card -> target, Mback: the slab's back face -> target, screenM: card -> screen DIPs (where the frosted glass
// samples the blurred backdrop; kx, ky: backdrop pixels -> screen DIPs)
static void Ov_DrawCardFace(RCtx& c, int idx, const D2D1_MATRIX_3X2_F& M, const D2D1_MATRIX_3X2_F& Mback, const D2D1_MATRIX_3X2_F& screenM,
                            float op, float focusAmt, float hoverAmt, float kx, float ky, bool glow) {
    const DeckLayout& L = g_ov.L;
    int g = g_cfg.order[idx];
    const GraphDef& d = g_graphs[g];
    Col col = GraphCol(g);
    float CW = L.CW, CH = L.CH;
    ID2D1RenderTarget* rt = c.rt;
    D2D1_ROUNDED_RECT rr = { RF(0, 0, CW, CH), 18, 18 };

    // soft shadow below the slab
    rt->SetTransform(&Mback);
    for (int i = 0; i < 3; i++) {
        D2D1_ROUNDED_RECT sr = { RF(3.f + i * 3, 9.f + i * 6, CW + 3 + i * 3, CH + 9 + i * 6), 20, 20 };
        rt->FillRoundedRectangle(&sr, SB(c, g_dark ? BLACK : Hex(0x1B2240), (g_dark ? 0.16f : 0.045f) * op));
    }
    // back face: gives the glass slab its thickness
    rt->FillRoundedRectangle(&rr, SB(c, g_dark ? Hex(0x2A2E3A) : WHITE, (g_dark ? 0.70f : 0.62f) * op));
    rt->DrawRoundedRectangle(&rr, SB(c, WHITE, (g_dark ? 0.22f : 0.95f) * op), 1.0f, nullptr);

    // front face: frosted glass = blurred backdrop sampled at the card's screen position, then a tint
    rt->SetTransform(&M);
    if (g_ov.glassBrush) {
        D2D1_MATRIX_3X2_F bt = MMul(MScale(kx, ky), MInv(screenM));
        g_ov.glassBrush->SetTransform(&bt);
        g_ov.glassBrush->SetOpacity(op);
        rt->FillRoundedRectangle(&rr, g_ov.glassBrush);
    }
    if (g_dark) rt->FillRoundedRectangle(&rr, SB(c, Hex(0x14161D), (0.50f - 0.08f * focusAmt) * op));
    else rt->FillRoundedRectangle(&rr, SB(c, WHITE, (0.50f + 0.12f * focusAmt) * op));
    rt->FillRoundedRectangle(&rr, GradAt(c.whiteGrad, 0, 0, 0, CH * 0.65f, (g_dark ? 0.10f : 0.40f) * op));
    rt->FillRoundedRectangle(&rr, GradAt(RC_Grad(c, col), 0, 0, 0, CH * 0.9f, ((g_dark ? 0.12f : 0.08f) + 0.08f * focusAmt) * op));
    if (!g_dark) {
        D2D1_ROUNDED_RECT outer = { RF(-0.5f, -0.5f, CW + 0.5f, CH + 0.5f), 18.5f, 18.5f };
        rt->DrawRoundedRectangle(&outer, SB(c, Hex(0x1B2240), 0.10f * op), 1.0f, nullptr);
    }
    rt->DrawRoundedRectangle(&rr, GradAt(c.whiteGrad, 0, 0, CW * 0.5f, CH, ((g_dark ? 0.38f : 0.95f) + 0.05f * hoverAmt) * op), 1.3f, nullptr);
    if (glow && focusAmt > 0.01f) Ov_DrawGlow(c, col, focusAmt * op);

    // header
    FillCircle(c, 27, 30, 5.5f, col, op);
    Txt(c, GraphTitle(g), TF_TITLE, 41, 13, CW * 0.62f, 47, InkCol(), op, 0, 1);
    wchar_t v[96], sub[160];
    Ov_ValueTexts(g, v, 96, sub, 160);
    Txt(c, v, TF_VALUE, CW * 0.45f, 11, CW - 22, 47, InkCol(), op, 2, 1);
    if (d.sB >= 0) {
        DualLegend(c, g, 22, 47, 67, CW - 80, TF_SMALL, op);
        Txt(c, L"total", TF_SMALL, CW - 120, 47, CW - 22, 67, Ink3Col(), op, 2, 1);
    } else Txt(c, sub, TF_SMALL, 22, 47, CW - 22, 67, Ink2Col(), op, 0, 1);

    // chart: the last 2 minutes
    D2D1_RECT_F cr = RF(22, 80, CW - 22, CH - 20);
    const int samples = 120;
    if (SER(d.sA).count < 2) {
        Txt(c, L"collecting…", TF_SMALL, cr.left, cr.top, cr.right, cr.bottom, Ink3Col(), op, 1, 1);
        return;
    }
    bool missing = isnan(SER(d.sA).at(0)) && (d.sB < 0 || isnan(SER(d.sB).at(0)));
    double mx = ChartMax(d.sA, d.sB, d.unit, samples);
    Col gridc = g_dark ? WHITE : Hex(0x1B2240);
    for (int i = 0; i <= 3; i++) {
        float y = cr.top + (cr.bottom - cr.top) * i / 3;
        Line(c, cr.left, y, cr.right, y, gridc, (i == 3 ? 0.16f : 0.07f) * op, 1.0f);
    }
    wchar_t ax[48]; FmtAxis(ax, 48, d.unit, mx);
    Txt(c, ax, TF_TINY, cr.left + 2, cr.top + 2, cr.right, cr.top + 18, Ink3Col(), op);
    SeriesChart(c, cr, SER(d.sA), samples, mx, col, 2.2f, g_dark ? 0.42f : 0.34f, op);
    if (d.sB >= 0) SeriesChart(c, cr, SER(d.sB), samples, mx, GraphColB(g), 1.8f, g_dark ? 0.20f : 0.14f, op);
    if (missing) Txt(c, (g == G_DISK1 || g == G_DISK2) ? L"disk not found" : L"not available", TF_SMALL, cr.left, cr.top, cr.right, cr.bottom, Ink3Col(), op, 1, 1);
}

// ---------------------------------------------------------------- text around the deck (screen DIPs)
struct HudLayout { float lx, lw, y, rx, rw, rowH, ry; };
static HudLayout Ov_HudLayout(float W, float H) {
    const DeckLayout& L = g_ov.L;
    HudLayout h;
    h.lx = 56; h.lw = std::min(430.f, std::max(220.f, L.deckL - 86)); h.y = H * 0.5f - 128;
    h.rw = 310; h.rx = W - 56 - h.rw;
    if (h.rx < L.deckR + 24) { h.rx = L.deckR + 24; h.rw = std::max(200.f, W - 32 - h.rx); }
    h.rowH = 38; h.ry = H * 0.5f - h.rowH * g_cfg.nGraphs * 0.5f;
    return h;
}
// the boxes the three text panels draw in: left = the focused graph, right = the index, bottom = the hint
static void Ov_HudBoxes(float W, float H, D2D1_RECT_F box[3]) {
    HudLayout h = Ov_HudLayout(W, H);
    box[0] = RF(h.lx - 16, h.y - 12, h.lx + h.lw + 16, h.y + 196);
    box[1] = RF(h.rx - 12, h.ry - 12, h.rx + h.rw + 12, h.ry + h.rowH * g_cfg.nGraphs + 6);
    box[2] = RF(W * 0.5f - 340, H - 76, W * 0.5f + 340, H - 28);
}
static void Ov_HudLeft(RCtx& c, float W, float H, float ap) {
    HudLayout hl = Ov_HudLayout(W, H);
    int g = g_cfg.order[g_ov.focus];
    const GraphDef& d = g_graphs[g];
    float lx = hl.lx, lw = hl.lw, y = hl.y;
    Txt(c, L"GLASSSTACK", TF_SMALLB, lx, y, lx + lw, y + 18, Ink3Col(), ap);
    FillCircle(c, lx + 7, y + 47, 6.5f, GraphCol(g), ap);
    Txt(c, GraphTitle(g), TF_BIG, lx + 22, y + 24, lx + lw, y + 70, InkCol(), ap, 0, 1);
    wchar_t v[96], sub[160], st[200];
    Ov_ValueTexts(g, v, 96, sub, 160);
    Txt(c, v, TF_HUGE, lx, y + 70, lx + lw, y + 134, InkCol(), ap, 0, 1);
    if (d.sB >= 0) {
        float vw = TextW(v, TF_HUGE);
        Txt(c, L"total", TF_BODY, lx + vw + 10, y + 103, lx + lw, y + 123, Ink3Col(), ap, 0, 1);
        DualLegend(c, g, lx, y + 138, y + 160, lx + lw, TF_BODY, ap);
    } else {
        const wchar_t* dev = d.unit == U_PCT ? GraphDevice(g) : nullptr;
        Txt(c, dev ? dev : sub, TF_BODY, lx, y + 138, lx + lw, y + 160, Ink2Col(), ap, 0, 1);
    }
    double avg, mx; SeriesStats(d.sA, 120, avg, mx);
    wchar_t a1[48], a2[48]; FmtVal(a1, 48, d.unit, avg); FmtVal(a2, 48, d.unit, mx);
    if (d.sB >= 0) swprintf(st, 200, L"2 min  ·  %s average %s  ·  peak %s", d.labA, a1, a2);
    else swprintf(st, 200, L"2 min  ·  average %s  ·  peak %s", a1, a2);
    Txt(c, st, TF_BODY, lx, y + 162, lx + lw, y + 184, Ink3Col(), ap, 0, 1);
}
static void Ov_HudIndex(RCtx& c, float W, float H, float ap) {
    HudLayout hl = Ov_HudLayout(W, H);
    float rx = hl.rx, rw = hl.rw, rowH = hl.rowH, ry = hl.ry;
    for (int i = 0; i < g_cfg.nGraphs; i++) {
        int gi = g_cfg.order[i];
        Col ci = GraphCol(gi);
        D2D1_RECT_F r = RF(rx, ry + i * rowH, rx + rw, ry + i * rowH + rowH - 6);
        g_ov.idxR[i] = r;
        bool f = i == g_ov.focus;
        if (f) {
            FillRR(c, r, 10, g_dark ? Hex(0x1A1A19) : WHITE, (g_dark ? 0.55f : 0.70f) * ap);
            FillRR(c, r, 10, ci, 0.12f * ap);
            FillRR(c, RF(r.left + 4, r.top + 8, r.left + 7, r.bottom - 8), 1.5f, ci, ap);
        } else if (i == g_ov.hover) FillRR(c, r, 10, g_dark ? WHITE : Hex(0x1B2240), 0.06f * ap);
        FillCircle(c, r.left + 19, (r.top + r.bottom) * 0.5f, 4.5f, ci, ap);
        Txt(c, GraphTitle(gi), f ? TF_BODYB : TF_BODY, r.left + 33, r.top, r.left + rw * 0.56f, r.bottom, f ? InkCol() : Ink2Col(), ap, 0, 1);
        wchar_t vv[96]; GraphValueText(gi, vv, 96);
        Txt(c, vv, TF_SMALL, r.left + rw * 0.40f, r.top, r.right - 12, r.bottom, f ? InkCol() : Ink2Col(), ap, 2, 1);
    }
}
static void Ov_HudHint(RCtx& c, float W, float H, float ap) {
    wchar_t mods[64]; wcscpy_s(mods, g_cfg.hkText);
    wchar_t* plus = wcsrchr(mods, L'+');
    const wchar_t* key = plus ? plus + 1 : L"Space";
    if (plus) *plus = 0;
    wchar_t hint[260];
    if (g_ov.hold) swprintf(hint, 260, L"Keep holding %s and tap %s for the next graph  ·  release to open it  ·  Esc cancels", mods, key);
    else wcscpy_s(hint, L"↑ ↓, mouse wheel or 1–9 to choose  ·  Enter or click to open  ·  Esc to close");
    FillRR(c, RF(W * 0.5f - 330, H - 70, W * 0.5f + 330, H - 34), 18, g_dark ? Hex(0x1A1A19) : WHITE, (g_dark ? 0.45f : 0.55f) * ap);
    Txt(c, hint, TF_BODY, W * 0.5f - 320, H - 70, W * 0.5f + 320, H - 34, Ink2Col(), ap, 1, 1);
}
// the blurred, dimmed screen behind the deck (Ov_Layout must have run)
static void Ov_Backdrop(RCtx& c, float W, float H, float ap) {
    D2D1_COLOR_F clr = g_dark ? D2D1_COLOR_F{ 0.03f, 0.035f, 0.05f, 1 } : D2D1_COLOR_F{ 0.92f, 0.93f, 0.95f, 1 };
    c.rt->Clear(&clr);
    D2D1_RECT_F full = RF(0, 0, W, H);
    if (g_ov.bmpBack) c.rt->DrawBitmap(g_ov.bmpBack, &full, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    if (g_dark) FillR(c, full, Hex(0x05060B), 0.25f + 0.38f * ap);
    else FillR(c, full, Hex(0xF4F6FA), 0.18f + 0.30f * ap);
    const DeckLayout& L = g_ov.L;
    Blob(c, L.deckL, L.deckB, W * 0.22f, Hex(0x3987E5), (g_dark ? 0.10f : 0.07f) * ap);
}

// the whole overview in one pass (render test)
static void Ov_Render(RCtx& c, float W, float H) {
    D2D1_MATRIX_3X2_F I = MIdent();
    c.rt->SetTransform(&I);
    if (g_cfg.nGraphs <= 0) return;
    Ov_Layout(W, H);
    float ap = g_ov.appear;
    Ov_Backdrop(c, W, H, ap);
    const DeckLayout& L = g_ov.L;
    Blob(c, (L.deckL + L.deckR) * 0.5f, (L.deckT + L.deckB) * 0.5f, W * 0.34f, GraphCol(g_cfg.order[g_ov.focus]), (g_dark ? 0.18f : 0.14f) * ap);
    float kx = W / (float)std::max(1, g_ov.bw), ky = H / (float)std::max(1, g_ov.bh);
    D2D1_MATRIX_3X2_F shift = Ov_BackShift();
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < L.n; i++) {
            bool isF = i == g_ov.focus;
            if ((pass == 1) != isF) continue;
            D2D1_MATRIX_3X2_F M = Ov_CardNow(i);
            g_ov.cardM[i] = M;
            float op = ap * (isF ? 1.0f : (0.86f + 0.14f * g_ov.hov[i]));
            Ov_DrawCardFace(c, i, M, MMul(shift, M), M, op, g_ov.pull[i], g_ov.hov[i], kx, ky, true);
        }
    c.rt->SetTransform(&I);
    Ov_HudLeft(c, W, H, ap);
    Ov_HudIndex(c, W, H, ap);
    Ov_HudHint(c, W, H, ap);
}

// ---------------------------------------------------------------- composition
static void Ov_CompRelease() {
    OvComp& K = g_ov.comp;
    SafeRelease(K.bg3); SafeRelease(K.bg); SafeRelease(K.bgS);
    SafeRelease(K.blob3); SafeRelease(K.blob); SafeRelease(K.blobS);
    for (int i = 0; i < G_COUNT; i++) { SafeRelease(K.card3[i]); SafeRelease(K.card[i]); SafeRelease(K.cardS[i]); }
    for (int i = 0; i < 2; i++) { SafeRelease(K.glow3[i]); SafeRelease(K.glow[i]); SafeRelease(K.glowS[i]); K.glowFor[i] = -1; }
    for (int i = 0; i < 3; i++) { SafeRelease(K.hud3[i]); SafeRelease(K.hud[i]); SafeRelease(K.hudS[i]); }
    if (K.target) K.target->SetRoot(nullptr);
    SafeRelease(K.root);
    SafeRelease(K.target);
    if (K.dev) K.dev->Commit();
    SafeRelease(K.dev);
    K.topCard = -1; K.lost = false;
}
static bool Ov_NewVisual(IDCompositionVisual2*& v, IDCompositionVisual3*& v3) {
    if (FAILED(g_ov.comp.dev->CreateVisual(&v))) { v = nullptr; return false; }
    if (FAILED(v->QueryInterface(IID_PPV_ARGS(&v3)))) v3 = nullptr;
    return true;
}
// redraws a whole w x h surface: `draw` runs with the shared context pointed at the surface, the transform set to
// `toSurface` and the surface's offset in its atlas (passed as a matrix for drawing code that sets its own transforms)
template <class F> static void Ov_Paint(IDCompositionSurface* s, UINT w, UINT h, const D2D1_MATRIX_3X2_F& toSurface, F draw) {
    OvComp& K = g_ov.comp;
    if (!s || K.lost) return;
    ID2D1DeviceContext* dc = nullptr; POINT off = {};
    if (FAILED(s->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), (void**)&dc, &off)) || !dc) { K.lost = true; return; }
    D2D1_MATRIX_3X2_F offT = MTrans((float)off.x, (float)off.y);
    dc->SetDpi(96, 96);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->SetTransform(&offT);
    D2D1_RECT_F clip = RF(0, 0, (float)w, (float)h);
    dc->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_ALIASED);
    D2D1_COLOR_F none = { 0, 0, 0, 0 };
    dc->Clear(&none);
    D2D1_MATRIX_3X2_F T = MMul(toSurface, offT);
    dc->SetTransform(&T);
    ID2D1RenderTarget* keep = g_ov.c.rt;
    g_ov.c.rt = dc;
    draw(offT);
    g_ov.c.rt = keep;
    dc->PopAxisAlignedClip();
    dc->Release();
    if (FAILED(s->EndDraw())) K.lost = true;
}
static D2D1_MATRIX_3X2_F Ov_CardToSurface() { const OvComp& K = g_ov.comp; return MMul(MTrans(K.m, K.m), MScale(K.P, K.P)); }
// a card as it looks at rest (pulled out when focused, else in the deck), with the frosted glass sampled there
static void Ov_PaintCard(int i) {
    OvComp& K = g_ov.comp;
    if (i < 0 || i >= g_ov.L.n) return;
    float pull = i == g_ov.focus ? 1.0f : 0.0f;
    float kx = g_ov.wDip / (float)std::max(1, g_ov.bw), ky = g_ov.hDip / (float)std::max(1, g_ov.bh);
    D2D1_MATRIX_3X2_F screen = Ov_CardPose(i, pull, 0), shift = Ov_BackShift(), toS = Ov_CardToSurface();
    Ov_Paint(K.cardS[i], K.cw, K.ch, toS, [&](const D2D1_MATRIX_3X2_F& offT) {
        D2D1_MATRIX_3X2_F M = MMul(toS, offT);
        Ov_DrawCardFace(g_ov.c, i, M, MMul(shift, M), screen, 1.0f, pull, 0, kx, ky, false);
    });
}
static void Ov_PaintGlow(int slot) {
    OvComp& K = g_ov.comp;
    int i = K.glowFor[slot];
    if (i < 0) return;
    Col col = GraphCol(g_cfg.order[i]);
    Ov_Paint(K.glowS[slot], K.cw, K.ch, Ov_CardToSurface(), [&](const D2D1_MATRIX_3X2_F&) { Ov_DrawGlow(g_ov.c, col, 1.0f); });
}
static void Ov_PaintHud(int part) {
    OvComp& K = g_ov.comp;
    float sc = g_ov.sc;
    Ov_Paint(K.hudS[part], K.hudW[part], K.hudH[part], MMul(MScale(sc, sc), MTrans(-(float)K.hudX[part], -(float)K.hudY[part])),
             [&](const D2D1_MATRIX_3X2_F&) {
                 if (part == 0) Ov_HudLeft(g_ov.c, g_ov.wDip, g_ov.hDip, 1.0f);
                 else if (part == 1) Ov_HudIndex(g_ov.c, g_ov.wDip, g_ov.hDip, 1.0f);
                 else Ov_HudHint(g_ov.c, g_ov.wDip, g_ov.hDip, 1.0f);
             });
}
static void Ov_PaintBlob() {
    OvComp& K = g_ov.comp;
    Col col = GraphCol(g_cfg.order[g_ov.focus]);
    Ov_Paint(K.blobS, 256, 256, MIdent(), [&](const D2D1_MATRIX_3X2_F&) { Blob(g_ov.c, 128, 128, 128, col, g_dark ? 0.18f : 0.14f); });
}
static void Ov_PaintBg() {
    OvComp& K = g_ov.comp;
    float s = g_ov.sc / OV_BG_DIV;
    Ov_Paint(K.bgS, K.bgW, K.bgH, MScale(s, s), [&](const D2D1_MATRIX_3X2_F&) { Ov_Backdrop(g_ov.c, g_ov.wDip, g_ov.hDip, 1.0f); });
}
// (AddVisual with no reference visual: FALSE puts the new visual on top of its siblings, TRUE below all of them)
static void Ov_CompOrder() {
    OvComp& K = g_ov.comp;
    K.root->RemoveAllVisuals();
    K.root->AddVisual(K.bg, FALSE, nullptr);
    K.root->AddVisual(K.blob, FALSE, nullptr);
    for (int i = 0; i < g_ov.L.n; i++) {
        if (i == g_ov.focus) continue;
        K.root->AddVisual(K.card[i], FALSE, nullptr);
        if (K.glowFor[1] == i) K.root->AddVisual(K.glow[1], FALSE, nullptr);
    }
    K.root->AddVisual(K.card[g_ov.focus], FALSE, nullptr);
    if (K.glowFor[0] == g_ov.focus) K.root->AddVisual(K.glow[0], FALSE, nullptr);
    for (int h = 0; h < 3; h++) K.root->AddVisual(K.hud[h], FALSE, nullptr);
    K.topCard = g_ov.focus;
}
// the focused card's outline takes slot 0; the card that just lost focus keeps its outline in slot 1 to fade it out
static void Ov_GlowFollowFocus() {
    OvComp& K = g_ov.comp;
    int f = g_ov.focus;
    if (K.glowFor[0] == f) return;
    std::swap(K.glow[0], K.glow[1]); std::swap(K.glow3[0], K.glow3[1]); std::swap(K.glowS[0], K.glowS[1]); std::swap(K.glowFor[0], K.glowFor[1]);
    if (K.glowFor[0] != f) { K.glowFor[0] = f; Ov_PaintGlow(0); }
    K.topCard = -1;
}
// one animation frame: transforms and opacities only, then hand the frame to the compositor
static void Ov_CompUpdate() {
    OvComp& K = g_ov.comp;
    if (!K.dev) return;
    if (K.lost) { Viewer_DeviceLost(); return; }
    const DeckLayout& L = g_ov.L;
    float sc = g_ov.sc, ap = g_ov.appear;
    if (K.topCard != g_ov.focus) Ov_CompOrder();
    D2D1_MATRIX_3X2_F fromSurface = MMul(MScale(1 / K.P, 1 / K.P), MTrans(-K.m, -K.m)), toPx = MScale(sc, sc);
    for (int i = 0; i < L.n; i++) {
        D2D1_MATRIX_3X2_F M = Ov_CardNow(i);
        g_ov.cardM[i] = M;
        D2D1_MATRIX_3X2_F V = MMul(MMul(fromSurface, M), toPx);
        K.card[i]->SetTransform(V);
        if (K.card3[i]) K.card3[i]->SetOpacity(ap * (i == g_ov.focus ? 1.0f : 0.86f + 0.14f * g_ov.hov[i]));
        for (int s = 0; s < 2; s++)
            if (K.glowFor[s] == i) { K.glow[s]->SetTransform(V); if (K.glow3[s]) K.glow3[s]->SetOpacity(ap * g_ov.pull[i]); }
    }
    // the colour glow behind the deck: a 256 px radial gradient, scaled up
    float R = g_ov.wDip * 0.34f * sc;
    K.blob->SetTransform(MMul(MMul(MTrans(-128, -128), MScale(R / 128, R / 128)),
                              MTrans((L.deckL + L.deckR) * 0.5f * sc, (L.deckT + L.deckB) * 0.5f * sc)));
    if (K.blob3) K.blob3->SetOpacity(ap);
    if (K.bg3) K.bg3->SetOpacity(ap);
    for (int h = 0; h < 3; h++) if (K.hud3[h]) K.hud3[h]->SetOpacity(ap);
    if (FAILED(K.dev->Commit())) K.lost = true;
}
// new data: redraw the cards and the two panels that show values
static void Ov_CompRefresh() {
    if (!g_ov.comp.dev) return;
    for (int i = 0; i < g_ov.L.n; i++) Ov_PaintCard(i);
    Ov_PaintHud(0);
    Ov_PaintHud(1);
    Ov_CompUpdate();
}
static bool Ov_CompCreate() {
    OvComp& K = g_ov.comp;
    if (FAILED(DCompositionCreateDevice2(g_d2dDev, IID_PPV_ARGS(&K.dev)))) { K.dev = nullptr; return false; }
    if (FAILED(K.dev->CreateTargetForHwnd(g_ov.hwnd, TRUE, &K.target)) || FAILED(K.dev->CreateVisual(&K.root))) { Ov_CompRelease(); return false; }
    K.target->SetRoot(K.root);
    K.root->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    RECT rc; GetClientRect(g_ov.hwnd, &rc);
    float sc = g_ov.sc;
    const DeckLayout& L = g_ov.L;
    K.P = L.k * sc * 1.04f;
    K.cw = (UINT)ceilf((L.CW + 2 * K.m) * K.P); K.ch = (UINT)ceilf((L.CH + 2 * K.m) * K.P);
    K.bgW = (UINT)std::max(1L, (rc.right + OV_BG_DIV - 1) / OV_BG_DIV); K.bgH = (UINT)std::max(1L, (rc.bottom + OV_BG_DIV - 1) / OV_BG_DIV);
    auto surf = [&](UINT w, UINT h, IDCompositionSurface** s) {
        return SUCCEEDED(K.dev->CreateSurface(std::max(1u, w), std::max(1u, h), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, s));
    };
    bool ok = Ov_NewVisual(K.bg, K.bg3) && surf(K.bgW, K.bgH, &K.bgS) && Ov_NewVisual(K.blob, K.blob3) && surf(256, 256, &K.blobS);
    for (int i = 0; ok && i < L.n; i++) ok = Ov_NewVisual(K.card[i], K.card3[i]) && surf(K.cw, K.ch, &K.cardS[i]);
    for (int i = 0; ok && i < 2; i++) ok = Ov_NewVisual(K.glow[i], K.glow3[i]) && surf(K.cw, K.ch, &K.glowS[i]);
    D2D1_RECT_F box[3];
    Ov_HudBoxes(g_ov.wDip, g_ov.hDip, box);
    for (int h = 0; ok && h < 3; h++) {
        K.hudX[h] = (int)floorf(box[h].left * sc); K.hudY[h] = (int)floorf(box[h].top * sc);
        K.hudW[h] = (UINT)std::max(1, (int)ceilf(box[h].right * sc) - K.hudX[h]);
        K.hudH[h] = (UINT)std::max(1, (int)ceilf(box[h].bottom * sc) - K.hudY[h]);
        ok = Ov_NewVisual(K.hud[h], K.hud3[h]) && surf(K.hudW[h], K.hudH[h], &K.hudS[h]);
        if (ok) { K.hud[h]->SetOffsetX((float)K.hudX[h]); K.hud[h]->SetOffsetY((float)K.hudY[h]); }
    }
    if (!ok) { Ov_CompRelease(); return false; }
    K.bg->SetContent(K.bgS);
    K.bg->SetTransform(MScale((float)OV_BG_DIV, (float)OV_BG_DIV));
    K.blob->SetContent(K.blobS);
    for (int i = 0; i < L.n; i++) K.card[i]->SetContent(K.cardS[i]);
    for (int i = 0; i < 2; i++) K.glow[i]->SetContent(K.glowS[i]);
    for (int h = 0; h < 3; h++) K.hud[h]->SetContent(K.hudS[h]);
    Ov_PaintBg();
    Ov_PaintBlob();
    K.glowFor[0] = g_ov.focus; K.glowFor[1] = -1;
    Ov_PaintGlow(0);
    Ov_PaintHud(2);
    Ov_CompRefresh();
    return !K.lost;
}

// ---------------------------------------------------------------- window
static void Ov_ReleaseRT() {
    Ov_CompRelease();
    SafeRelease(g_ov.glassBrush); SafeRelease(g_ov.bmpBack); SafeRelease(g_ov.bmpGlass);
    RC_Release(g_ov.c);
}
static bool Ov_CreateRT() {
    if (!g_ov.hwnd || !g_d2d || g_cfg.nGraphs <= 0) return false;
    if (!Gfx_Ensure()) return false;
    if (g_ov.focus >= g_cfg.nGraphs) g_ov.focus = 0;
    RECT rc; GetClientRect(g_ov.hwnd, &rc);
    g_ov.sc = DpiOf(g_ov.hwnd) / 96.0f;
    g_ov.wDip = rc.right / g_ov.sc; g_ov.hDip = rc.bottom / g_ov.sc;
    Ov_Layout(g_ov.wDip, g_ov.hDip);
    if (!RC_Init(g_ov.c, g_dc)) { Ov_ReleaseRT(); return false; }
    Ov_MakeBitmaps(g_ov.c);
    if (!Ov_CompCreate()) { Ov_ReleaseRT(); return false; }
    return true;
}
static void Ov_Animate() {
    if (g_ov.hwnd && g_ov.shown && !g_ov.animating) { g_ov.animating = true; g_ov.lastQpc = 0; SetTimer(g_ov.hwnd, OV_T_ANIM, 15, nullptr); }
}
static void Ov_Tick() {
    LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    float dt = g_ov.lastQpc ? (float)((q.QuadPart - g_ov.lastQpc) / (double)f.QuadPart) : 0.016f;
    g_ov.lastQpc = q.QuadPart;
    if (dt > 0.05f) dt = 0.05f;
    float k = 1.0f - expf(-dt * 14.0f);
    bool moving = false;
    auto step = [&](float& v, float t) { float d = t - v; if (fabsf(d) < 0.002f) v = t; else { v += d * k; moving = true; } };
    step(g_ov.appear, 1.0f);
    for (int i = 0; i < g_cfg.nGraphs; i++) {
        step(g_ov.pull[i], i == g_ov.focus ? 1.0f : 0.0f);
        step(g_ov.hov[i], i == g_ov.hover ? 1.0f : 0.0f);
    }
    if (!moving) { KillTimer(g_ov.hwnd, OV_T_ANIM); g_ov.animating = false; }
    Ov_CompUpdate();
}
static void Ov_SetFocus(int f) {
    int n = g_cfg.nGraphs;
    if (n <= 0) return;
    f = ((f % n) + n) % n;
    if (f == g_ov.focus) return;
    int old = g_ov.focus;
    g_ov.focus = f;
    if (g_ov.comp.dev) {
        // the two cards change their tint and the place their glass is sampled; that is redrawn now, the move animates
        Ov_PaintCard(old);
        Ov_PaintCard(f);
        Ov_GlowFollowFocus();
        Ov_PaintBlob();
        Ov_PaintHud(0);
        Ov_PaintHud(1);
    }
    Ov_Animate();
    if (!g_ov.animating) Ov_CompUpdate();
}
static void Ov_Show() {
    if (!g_ov.hwnd || g_ov.shown) return;
    g_ov.shown = true;
    KillTimer(g_ov.hwnd, OV_T_SHOW);
    Ov_CompUpdate();
    ShowWindow(g_ov.hwnd, SW_SHOW);
    SetForegroundWindow(g_ov.hwnd);
    SetFocus(g_ov.hwnd);
    Ov_Animate();
}
static void Ov_Close() {
    if (!g_ov.hwnd) return;
    HWND h = g_ov.hwnd;
    g_ov.hwnd = nullptr;
    g_sh->overlayHold = 0;
    KillTimer(h, OV_T_ANIM); KillTimer(h, OV_T_KEYS); KillTimer(h, OV_T_SHOW); KillTimer(h, OV_T_HANDOFF);
    g_ov.animating = false; g_ov.hold = false; g_ov.shown = false; g_ov.committing = false;
    Ov_ReleaseRT();
    DestroyWindow(h);
    std::vector<DWORD>().swap(g_ov.back);
    std::vector<DWORD>().swap(g_ov.glass);
    Viewer_CheckIdle();
}
static void Ov_Commit(int idx) {
    if (!g_ov.hwnd || idx < 0 || idx >= g_cfg.nGraphs || g_ov.committing) return;
    int g = g_cfg.order[idx];
    g_sh->lastFocus = g;
    g_ov.committing = true;
    KillTimer(g_ov.hwnd, OV_T_KEYS);
    if (!g_roleOverview) { Section_Open(g); Ov_Close(); return; }
    // The section window lives in the other viewer process. Pass the foreground on, ask the resident to open it,
    // and stay on screen until it is up so the desktop does not flash in between.
    AllowSetForegroundWindow(ASFW_ANY);
    IniPutInt(L"LastGraph", g);
    HWND res = (HWND)(LONG_PTR)g_sh->residentHwnd;
    if (res) PostMessageW(res, WM_GS_VIEWER, VEV_SECTION, g);
    if (!g_ov.shown) { Ov_Close(); return; }
    g_ovHandoff = GetTickCount64();
    SetTimer(g_ov.hwnd, OV_T_HANDOFF, 15, nullptr);
}
static bool Ov_KeysHeld() {
    UINT m = g_cfg.hkMods;
    if ((m & MOD_CONTROL) && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) return true;
    if ((m & MOD_SHIFT) && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) return true;
    if ((m & MOD_ALT) && (GetAsyncKeyState(VK_MENU) & 0x8000)) return true;
    if ((m & MOD_WIN) && ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)) return true;
    if (GetAsyncKeyState((int)g_cfg.hkVk) & 0x8000) return true;
    return false;
}
static int Ov_IndexOfGraph(int g) {
    for (int i = 0; i < g_cfg.nGraphs; i++) if (g_cfg.order[i] == g) return i;
    return 0;
}
static void Ov_Open(bool hold, LONG taps) {
    if (g_ov.hwnd || g_cfg.nGraphs <= 0) return;
    POINT pt; GetCursorPos(&pt);
    HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi); GetMonitorInfoW(m, &mi);
    g_ov.mon = mi.rcMonitor;
    Ov_Capture();                     // before our window covers the screen
    g_ov.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, L"GlassStackOverlay", L"GlassStack overview", WS_POPUP,
                                mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                                nullptr, nullptr, g_hInst, nullptr);
    if (!g_ov.hwnd) return;
    g_ov.hold = hold;
    g_ov.shown = false;
    g_ov.focus = Ov_IndexOfGraph(g_sh->lastFocus);
    g_ov.hover = -1;
    for (int i = 0; i < G_COUNT; i++) { g_ov.pull[i] = 0; g_ov.hov[i] = 0; }
    g_ov.appear = 0;
    if (hold) {
        g_sh->overlayHold = 1;
        g_ov.seenTaps = taps;
        SetTimer(g_ov.hwnd, OV_T_KEYS, 15, nullptr);
        SetTimer(g_ov.hwnd, OV_T_SHOW, OV_SHOW_DELAY, nullptr);
    }
    // the scene is drawn while the hold delay runs; without it, open the section instead of an empty screen
    if (!Ov_CreateRT()) { Ov_Commit(g_ov.focus); return; }
    if (!hold) Ov_Show();
}
static void Ov_PollKeys() {
    LONG t = g_sh->taps;
    if (t != g_ov.seenTaps) {
        int adv = (int)(t - g_ov.seenTaps);
        g_ov.seenTaps = t;
        if (adv > 0 && adv < 64) { Ov_SetFocus(g_ov.focus + adv); Ov_Show(); }
    }
    if (!Ov_KeysHeld()) {
        KillTimer(g_ov.hwnd, OV_T_KEYS);
        Ov_Commit(g_ov.focus);
    }
}
// in drawing order from the top: the lifted card, the cards in front of its slot, its empty slot (so the mouse can
// rest there without the card dropping back), the cards behind it
static int Ov_Hit(float x, float y) {
    for (int i = 0; i < g_cfg.nGraphs; i++) if (InRect(g_ov.idxR[i], x, y)) return i;
    const DeckLayout& L = g_ov.L;
    auto on = [&](const D2D1_MATRIX_3X2_F& M) {
        D2D1_POINT_2F p = MApply(MInv(M), x, y);
        return p.x >= 0 && p.x <= L.CW && p.y >= 0 && p.y <= L.CH;
    };
    int f = g_ov.focus;
    if (f < 0 || f >= L.n) return -1;
    if (on(g_ov.cardM[f])) return f;
    for (int i = L.n - 1; i > f; i--) if (on(g_ov.cardM[i])) return i;
    if (on(Ov_CardPose(f, 0, 0))) return f;
    for (int i = f - 1; i >= 0; i--) if (on(g_ov.cardM[i])) return i;
    return -1;
}

static void Overlay_Hold(LONG taps) {
    if (!g_ov.hwnd) { Ov_Open(true, taps); return; }
    if (!g_ov.hold) {
        g_ov.hold = true;
        g_sh->overlayHold = 1;
        g_ov.seenTaps = taps - 1;
        SetTimer(g_ov.hwnd, OV_T_KEYS, 15, nullptr);
        if (g_ov.comp.dev) { Ov_PaintHud(2); Ov_CompUpdate(); }
    }
}
static void Overlay_OpenPersistent() {
    if (!g_ov.hwnd) Ov_Open(false, 0);
    else { Ov_Show(); SetForegroundWindow(g_ov.hwnd); }
}
static void Overlay_OnSample() { if (g_ov.hwnd) Ov_CompRefresh(); }

static LRESULT CALLBACK Ov_Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        // the compositor shows the surfaces; a paint only arrives after the device was lost or the theme changed
        ValidateRect(h, nullptr);
        if (g_ov.hwnd == h && !g_ov.comp.dev) {
            if (Ov_CreateRT()) Ov_CompUpdate();
            else Ov_Close();
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (wp == OV_T_ANIM) Ov_Tick();
        else if (wp == OV_T_KEYS) { if (g_ov.hold) Ov_PollKeys(); }
        else if (wp == OV_T_SHOW) { KillTimer(h, OV_T_SHOW); if (g_ov.hold && Ov_KeysHeld()) Ov_Show(); }
        else if (wp == OV_T_HANDOFF) {
            wchar_t cls[32] = L"";
            HWND fg = GetForegroundWindow();
            if (fg) GetClassNameW(fg, cls, 32);
            if (!wcscmp(cls, L"GlassStackSection") || GetTickCount64() - g_ovHandoff > 900) Ov_Close();
        }
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE: Ov_Close(); return 0;
        case VK_RETURN: Ov_Commit(g_ov.focus); return 0;
        case VK_UP: case VK_LEFT: Ov_SetFocus(g_ov.focus - 1); return 0;
        case VK_DOWN: case VK_RIGHT: case VK_TAB: Ov_SetFocus(g_ov.focus + 1); return 0;
        case VK_SPACE: if (!g_ov.hold) Ov_SetFocus(g_ov.focus + 1); return 0;
        default:
            if (wp >= '1' && wp <= '9' && (int)(wp - '1') < g_cfg.nGraphs) Ov_SetFocus((int)(wp - '1'));
            return 0;
        }
    case WM_MOUSEWHEEL:
        Ov_SetFocus(g_ov.focus + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1));
        return 0;
    case WM_MOUSEMOVE: {
        int hit = Ov_Hit(GX(lp) / g_ov.sc, GY(lp) / g_ov.sc);
        if (hit != g_ov.hover) {
            g_ov.hover = hit;
            int before = g_ov.focus;
            if (!g_ov.hold && hit >= 0) Ov_SetFocus(hit);
            if (g_ov.focus == before && g_ov.comp.dev) Ov_PaintHud(1);   // the index shows the hovered row
            Ov_Animate();
            if (!g_ov.animating) Ov_CompUpdate();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int hit = Ov_Hit(GX(lp) / g_ov.sc, GY(lp) / g_ov.sc);
        if (hit >= 0) Ov_Commit(hit); else Ov_Close();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !g_ov.committing && g_ov.hwnd == h && g_ov.shown && !g_ov.hold) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        if (g_ov.hwnd == h) Ov_Close(); else DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
