// GlassStack - Direct2D / DirectWrite helpers (viewer process only; the resident never loads these DLLs).
#pragma once

static ID2D1Factory* g_d2d = nullptr;
static IDWriteFactory* g_dw = nullptr;

enum { TF_TINY, TF_SMALL, TF_SMALLB, TF_BODY, TF_BODYB, TF_TITLE, TF_VALUE, TF_BIG, TF_HUGE, TF_COUNT };
static IDWriteTextFormat* g_tf[TF_COUNT];

static bool D2D_Init() {
    if (g_d2d) return true;
    D2D1_FACTORY_OPTIONS o = {};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &o, (void**)&g_d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw))) return false;
    struct Spec { const wchar_t* fam; DWRITE_FONT_WEIGHT w; float sz; };
    static const Spec specs[TF_COUNT] = {
        { L"Segoe UI Variable Text",    DWRITE_FONT_WEIGHT_NORMAL,     10.5f },
        { L"Segoe UI Variable Text",    DWRITE_FONT_WEIGHT_NORMAL,     12.0f },
        { L"Segoe UI Variable Text",    DWRITE_FONT_WEIGHT_SEMI_BOLD,  12.0f },
        { L"Segoe UI Variable Text",    DWRITE_FONT_WEIGHT_NORMAL,     13.5f },
        { L"Segoe UI Variable Text",    DWRITE_FONT_WEIGHT_SEMI_BOLD,  13.5f },
        { L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD,  17.0f },
        { L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD,  22.0f },
        { L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD,  30.0f },
        { L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_LIGHT, 46.0f },
    };
    for (int i = 0; i < TF_COUNT; i++) {
        if (FAILED(g_dw->CreateTextFormat(specs[i].fam, nullptr, specs[i].w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          specs[i].sz, L"en-us", &g_tf[i]))) return false;
        g_tf[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING tr = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject* ell = nullptr;
        if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(g_tf[i], &ell))) { g_tf[i]->SetTrimming(&tr, ell); ell->Release(); }
    }
    return true;
}
// ---------------------------------------------------------------- the device behind the windows
// Measured on the target laptop (Intel iGPU drives the display), section 1860x1200 / overview 2560x1600:
//   hardware D3D11 : 115-135 MB private (driver heaps + shader compiler), little CPU
//   WARP, 1 thread : 45 MB with the section open, 30 ms of CPU per section frame (once a second); the overview
//                    opens in ~70 ms and an animation frame costs ~0.01 ms
//   WARP, threaded : 75-105 MB, and several times the CPU per frame, spread over 24 threads
// So both windows draw with single-threaded WARP into DirectComposition surfaces and leave the rest to the desktop
// compositor: the section window redraws one surface about once a second (its mouse-over is a few small visuals
// that only move), the overview draws each card once and the compositor animates them (gs_overlay.h).
static bool g_warpMT = false;          // WARP may spread rasterization over worker threads (more CPU in total)
static bool g_gfxHardware = false;     // draw on the GPU instead (only for --bench comparisons)
static bool g_roleOverview = false;    // this viewer process shows the overview (else: the section window)
static ID3D11Device* g_d3d = nullptr;
static ID2D1Device* g_d2dDev = nullptr;
static ID2D1DeviceContext* g_dc = nullptr;

static void Gfx_Release() { SafeRelease(g_dc); SafeRelease(g_d2dDev); SafeRelease(g_d3d); }
static bool Gfx_Ensure() {
    if (g_dc) return true;
    D3D_FEATURE_LEVEL fl; ID3D11DeviceContext* ctx = nullptr;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = E_FAIL;
    if (g_gfxHardware) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &g_d3d, &fl, &ctx);
    if (FAILED(hr)) {
        if (!g_warpMT) flags |= D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &g_d3d, &fl, &ctx);
    }
    if (FAILED(hr)) return false;
    SafeRelease(ctx);
    IDXGIDevice* xd = nullptr; ID2D1Factory1* f1 = nullptr;
    bool ok = SUCCEEDED(g_d3d->QueryInterface(IID_PPV_ARGS(&xd))) && SUCCEEDED(g_d2d->QueryInterface(IID_PPV_ARGS(&f1))) &&
              SUCCEEDED(f1->CreateDevice(xd, &g_d2dDev)) && SUCCEEDED(g_d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_dc));
    SafeRelease(f1); SafeRelease(xd);
    if (!ok) { Gfx_Release(); return false; }
    g_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}
static void D2D_Shutdown() {
    Gfx_Release();
    for (int i = 0; i < TF_COUNT; i++) SafeRelease(g_tf[i]);
    SafeRelease(g_dw);
    SafeRelease(g_d2d);
}

// per render target resources
struct RCtx {
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* sb = nullptr;
    ID2D1LinearGradientBrush* whiteGrad = nullptr;
    std::unordered_map<UINT32, ID2D1LinearGradientBrush*> grads;
    ID2D1StrokeStyle* round = nullptr;
    ID2D1StrokeStyle* dash = nullptr;
    std::vector<D2D1_POINT_2F> pts;
};

static D2D1_MATRIX_3X2_F MMake(float a, float b, float c, float d, float e, float f) {
    D2D1_MATRIX_3X2_F m; m._11 = a; m._12 = b; m._21 = c; m._22 = d; m._31 = e; m._32 = f; return m;
}
static D2D1_MATRIX_3X2_F MIdent() { return MMake(1, 0, 0, 1, 0, 0); }
static D2D1_MATRIX_3X2_F MScale(float sx, float sy) { return MMake(sx, 0, 0, sy, 0, 0); }
static D2D1_MATRIX_3X2_F MTrans(float x, float y) { return MMake(1, 0, 0, 1, x, y); }
// A then B (row vectors, like D2D)
static D2D1_MATRIX_3X2_F MMul(const D2D1_MATRIX_3X2_F& A, const D2D1_MATRIX_3X2_F& B) {
    return MMake(A._11 * B._11 + A._12 * B._21, A._11 * B._12 + A._12 * B._22,
                 A._21 * B._11 + A._22 * B._21, A._21 * B._12 + A._22 * B._22,
                 A._31 * B._11 + A._32 * B._21 + B._31, A._31 * B._12 + A._32 * B._22 + B._32);
}
static D2D1_MATRIX_3X2_F MInv(const D2D1_MATRIX_3X2_F& m) {
    float det = m._11 * m._22 - m._12 * m._21;
    if (fabsf(det) < 1e-12f) return MIdent();
    float id = 1.0f / det;
    float a = m._22 * id, b = -m._12 * id, c = -m._21 * id, d = m._11 * id;
    return MMake(a, b, c, d, -(m._31 * a + m._32 * c), -(m._31 * b + m._32 * d));
}
static D2D1_POINT_2F MApply(const D2D1_MATRIX_3X2_F& m, float x, float y) {
    D2D1_POINT_2F p = { x * m._11 + y * m._21 + m._31, x * m._12 + y * m._22 + m._32 }; return p;
}
static D2D1_RECT_F RF(float l, float t, float r, float b) { D2D1_RECT_F o = { l, t, r, b }; return o; }
static bool InRect(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static D2D1_RECT_F Inset(D2D1_RECT_F r, float d) { return RF(r.left + d, r.top + d, r.right - d, r.bottom - d); }

static ID2D1LinearGradientBrush* RC_MakeGrad(RCtx& c, Col col, float a0, float a1) {
    D2D1_GRADIENT_STOP st[2];
    st[0].position = 0.f; st[0].color = { col.r, col.g, col.b, a0 };
    st[1].position = 1.f; st[1].color = { col.r, col.g, col.b, a1 };
    ID2D1GradientStopCollection* sc = nullptr;
    if (FAILED(c.rt->CreateGradientStopCollection(st, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &sc))) return nullptr;
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp = { { 0, 0 }, { 0, 1 } };
    ID2D1LinearGradientBrush* b = nullptr;
    c.rt->CreateLinearGradientBrush(&lp, nullptr, sc, &b);
    sc->Release();
    return b;
}
static ID2D1LinearGradientBrush* RC_Grad(RCtx& c, Col col) {
    UINT32 key = ToHex(col);
    auto it = c.grads.find(key);
    if (it != c.grads.end()) return it->second;
    ID2D1LinearGradientBrush* b = RC_MakeGrad(c, col, 1.0f, 0.0f);
    c.grads[key] = b;
    return b;
}
static ID2D1Brush* GradAt(ID2D1LinearGradientBrush* b, float x0, float y0, float x1, float y1, float op) {
    D2D1_POINT_2F a = { x0, y0 }, e = { x1, y1 };
    b->SetStartPoint(a); b->SetEndPoint(e); b->SetOpacity(op);
    return b;
}
static bool RC_Init(RCtx& c, ID2D1RenderTarget* rt) {
    c.rt = rt;
    D2D1_COLOR_F w = { 1, 1, 1, 1 };
    if (FAILED(rt->CreateSolidColorBrush(&w, nullptr, &c.sb))) return false;
    c.whiteGrad = RC_MakeGrad(c, WHITE, 1.0f, 0.0f);
    D2D1_STROKE_STYLE_PROPERTIES sp = {};
    sp.startCap = sp.endCap = sp.dashCap = D2D1_CAP_STYLE_ROUND;
    sp.lineJoin = D2D1_LINE_JOIN_ROUND; sp.miterLimit = 10; sp.dashStyle = D2D1_DASH_STYLE_SOLID;
    g_d2d->CreateStrokeStyle(&sp, nullptr, 0, &c.round);
    sp.dashStyle = D2D1_DASH_STYLE_DASH;
    g_d2d->CreateStrokeStyle(&sp, nullptr, 0, &c.dash);
    return c.sb && c.whiteGrad && c.round;
}
static void RC_Release(RCtx& c) {
    SafeRelease(c.sb);
    SafeRelease(c.whiteGrad);
    for (auto& kv : c.grads) if (kv.second) kv.second->Release();
    c.grads.clear();
    SafeRelease(c.round);
    SafeRelease(c.dash);
    std::vector<D2D1_POINT_2F>().swap(c.pts);
    c.rt = nullptr;
}

static ID2D1SolidColorBrush* SB(RCtx& c, Col col, float a) {
    D2D1_COLOR_F f = { col.r, col.g, col.b, a };
    c.sb->SetColor(&f);
    c.sb->SetOpacity(1.0f);
    return c.sb;
}
static void FillR(RCtx& c, D2D1_RECT_F r, Col col, float a) { c.rt->FillRectangle(&r, SB(c, col, a)); }
static void FillRR(RCtx& c, D2D1_RECT_F r, float rad, Col col, float a) {
    D2D1_ROUNDED_RECT rr = { r, rad, rad }; c.rt->FillRoundedRectangle(&rr, SB(c, col, a));
}
static void StrokeRR(RCtx& c, D2D1_RECT_F r, float rad, Col col, float a, float w) {
    D2D1_ROUNDED_RECT rr = { r, rad, rad }; c.rt->DrawRoundedRectangle(&rr, SB(c, col, a), w, nullptr);
}
static void FillCircle(RCtx& c, float x, float y, float rad, Col col, float a) {
    D2D1_ELLIPSE e = { { x, y }, rad, rad }; c.rt->FillEllipse(&e, SB(c, col, a));
}
static void Line(RCtx& c, float x0, float y0, float x1, float y1, Col col, float a, float w) {
    D2D1_POINT_2F p0 = { x0, y0 }, p1 = { x1, y1 };
    c.rt->DrawLine(p0, p1, SB(c, col, a), w, c.round);
}
static void DashLine(RCtx& c, float x0, float y0, float x1, float y1, Col col, float a, float w) {
    D2D1_POINT_2F p0 = { x0, y0 }, p1 = { x1, y1 };
    c.rt->DrawLine(p0, p1, SB(c, col, a), w, c.dash);
}
// align: 0 left, 1 center, 2 right; valign: 0 top, 1 center, 2 bottom
static void Txt(RCtx& c, const wchar_t* s, int tf, float x0, float y0, float x1, float y1, Col col, float a, int align = 0, int valign = 0) {
    if (!s || !*s || x1 <= x0) return;
    IDWriteTextFormat* f = g_tf[tf];
    f->SetTextAlignment(align == 0 ? DWRITE_TEXT_ALIGNMENT_LEADING : align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
    f->SetParagraphAlignment(valign == 0 ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : valign == 1 ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    D2D1_RECT_F r = { x0, y0, x1, y1 };
    c.rt->DrawText(s, (UINT32)wcslen(s), f, &r, SB(c, col, a), D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}
static float TextW(const wchar_t* s, int tf) {
    IDWriteTextLayout* l = nullptr; float w = 0;
    g_tf[tf]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (SUCCEEDED(g_dw->CreateTextLayout(s, (UINT32)wcslen(s), g_tf[tf], 4000.f, 400.f, &l))) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
        l->Release();
    }
    return w;
}
static void Blob(RCtx& c, float cx, float cy, float rad, Col col, float a) {
    D2D1_GRADIENT_STOP st[3];
    st[0].position = 0.f;   st[0].color = { col.r, col.g, col.b, a };
    st[1].position = 0.55f; st[1].color = { col.r, col.g, col.b, a * 0.33f };
    st[2].position = 1.f;   st[2].color = { col.r, col.g, col.b, 0.f };
    ID2D1GradientStopCollection* sc = nullptr;
    if (FAILED(c.rt->CreateGradientStopCollection(st, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &sc))) return;
    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES rp = { { cx, cy }, { 0, 0 }, rad, rad };
    ID2D1RadialGradientBrush* b = nullptr;
    if (SUCCEEDED(c.rt->CreateRadialGradientBrush(&rp, nullptr, sc, &b))) {
        D2D1_ELLIPSE e = { { cx, cy }, rad, rad };
        c.rt->FillEllipse(&e, b);
        b->Release();
    }
    sc->Release();
}
// frosted panel of the section window
static void Panel(RCtx& c, D2D1_RECT_F r, float rad) {
    D2D1_ROUNDED_RECT rr = { r, rad, rad };
    if (g_dark) {
        c.rt->FillRoundedRectangle(&rr, SB(c, Hex(0x1A1A19), 0.78f));
        c.rt->FillRoundedRectangle(&rr, GradAt(c.whiteGrad, r.left, r.top, r.left, r.top + 90, 0.05f));
        c.rt->DrawRoundedRectangle(&rr, SB(c, WHITE, 0.09f), 1.0f, nullptr);
    } else {
        for (int i = 1; i <= 3; i++) {
            D2D1_ROUNDED_RECT sh = { RF(r.left - i * 0.5f, r.top + i * 1.2f, r.right + i * 0.5f, r.bottom + i * 2.0f), rad + i, rad + i };
            c.rt->FillRoundedRectangle(&sh, SB(c, Hex(0x1B2240), 0.022f));
        }
        c.rt->FillRoundedRectangle(&rr, SB(c, WHITE, 0.74f));
        c.rt->FillRoundedRectangle(&rr, GradAt(c.whiteGrad, r.left, r.top, r.left, r.top + 120, 0.35f));
        c.rt->DrawRoundedRectangle(&rr, SB(c, WHITE, 0.95f), 1.0f, nullptr);
    }
}
static void HGrid(RCtx& c, D2D1_RECT_F r, int lines) {
    for (int i = 0; i <= lines; i++) {
        float y = roundf(r.top + (r.bottom - r.top) * i / lines) + 0.5f;
        Line(c, r.left, y, r.right, y, i == lines ? AxisCol() : GridCol(), 1.0f, 1.0f);
    }
}
// value at sample index i (0 = oldest of `samples`), bucketed to `bucket` samples (max of the bucket)
static float BucketMax(const Series& S, int newestAgo, int bucket) {
    float m = NAN;
    for (int k = 0; k < bucket; k++) {
        float v = S.at(newestAgo + k);
        if (!isnan(v) && (isnan(m) || v > m)) m = v;
    }
    return m;
}
// area + line chart of one series over the last `samples` ticks, newest at the right edge.
// Long ranges are reduced to about one point per DIP using the bucket maximum, so spikes stay visible.
static void SeriesChart(RCtx& c, D2D1_RECT_F r, const Series& S, int samples, double maxV, Col col, float lineW, float areaOp, float op = 1.0f) {
    if (samples < 2 || maxV <= 0 || S.count < 2) return;
    float w = r.right - r.left, h = r.bottom - r.top;
    int bucket = std::max(1, (int)ceilf(samples / std::max(8.0f, w)));
    int pointsN = samples / bucket;
    if (pointsN < 2) return;
    float dx = w / (pointsN - 1);
    ID2D1PathGeometry* gl = nullptr; ID2D1PathGeometry* ga = nullptr;
    ID2D1GeometrySink* sl = nullptr; ID2D1GeometrySink* sa = nullptr;
    if (FAILED(g_d2d->CreatePathGeometry(&gl)) || FAILED(gl->Open(&sl))) { SafeRelease(gl); return; }
    if (areaOp > 0 && SUCCEEDED(g_d2d->CreatePathGeometry(&ga))) { if (FAILED(ga->Open(&sa))) SafeRelease(ga); }
    std::vector<D2D1_POINT_2F>& pts = c.pts;
    pts.clear();
    auto flush = [&]() {
        if (pts.size() >= 2) {
            sl->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
            sl->AddLines(&pts[1], (UINT32)pts.size() - 1);
            sl->EndFigure(D2D1_FIGURE_END_OPEN);
            if (sa) {
                D2D1_POINT_2F b0 = { pts[0].x, r.bottom }, b1 = { pts.back().x, r.bottom };
                sa->BeginFigure(b0, D2D1_FIGURE_BEGIN_FILLED);
                sa->AddLines(pts.data(), (UINT32)pts.size());
                sa->AddLine(b1);
                sa->EndFigure(D2D1_FIGURE_END_CLOSED);
            }
        }
        pts.clear();
    };
    for (int i = 0; i < pointsN; i++) {
        int ago = (pointsN - 1 - i) * bucket;
        if (ago >= S.count) continue;
        float v = BucketMax(S, ago, bucket);
        if (isnan(v)) { flush(); continue; }
        D2D1_POINT_2F p = { r.left + i * dx, r.bottom - (float)ClampD(v / maxV, 0, 1) * h };
        pts.push_back(p);
    }
    flush();
    sl->Close(); sl->Release();
    if (sa) { sa->Close(); sa->Release(); }
    if (ga) { c.rt->FillGeometry(ga, GradAt(RC_Grad(c, col), r.left, r.top, r.left, r.bottom, areaOp * op), nullptr); ga->Release(); }
    c.rt->DrawGeometry(gl, SB(c, col, op), lineW, c.round);
    gl->Release();
}

// save a WIC bitmap as PNG (render test)
static bool SavePng(IWICImagingFactory* wic, IWICBitmapSource* src, const wchar_t* path) {
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* props = nullptr;
    bool ok = false;
    if (SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, &props)) && SUCCEEDED(fr->Initialize(props))) {
        UINT w = 0, h = 0; src->GetSize(&w, &h);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        fr->SetSize(w, h); fr->SetPixelFormat(&fmt);
        IWICFormatConverter* conv = nullptr;
        IWICBitmapSource* use = src;
        if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(src, fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) use = conv;
        ok = SUCCEEDED(fr->WriteSource(use, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
        SafeRelease(conv);
    }
    SafeRelease(props); SafeRelease(fr); SafeRelease(enc); SafeRelease(st);
    return ok;
}
