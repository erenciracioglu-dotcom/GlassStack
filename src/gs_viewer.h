// GlassStack - viewer process: a hidden control window that takes commands from the resident, owns the overview
// and the section window, and exits after ViewerLingerSeconds without any open window (freeing every byte the
// graphics driver allocated). Also hosts --render-test.
#pragma once

#define VT_LINGER 31

static void Viewer_CheckIdle() {
    if (g_renderTest || !g_vCtl) return;
    if (g_ov.hwnd || g_sec.hwnd) return;
    if (g_roleOverview) PostQuitMessage(0);     // its graphics driver memory leaves with the process
    else SetTimer(g_vCtl, VT_LINGER, (UINT)std::max(1, g_cfg.lingerSec) * 1000, nullptr);
}
static void Viewer_DeviceLost() {
    Ov_ReleaseRT();
    Sec_ReleaseRT();
    Gfx_Release();
    if (g_ov.hwnd) InvalidateRect(g_ov.hwnd, nullptr, FALSE);
    if (g_sec.hwnd) InvalidateRect(g_sec.hwnd, nullptr, FALSE);
}
static void Viewer_Reload() {
    Cfg_Load();
    g_dark = ViewerDark();
    if (g_sec.hwnd) { Sec_ApplyTheme(); Sec_SetTitle(); }
    if (g_ov.hwnd) { Ov_ReleaseRT(); InvalidateRect(g_ov.hwnd, nullptr, FALSE); }
}
static void Viewer_Command(int cmd, int arg) {
    if (g_vCtl) KillTimer(g_vCtl, VT_LINGER);
    switch (cmd) {
    case CMD_HOLD: if (g_roleOverview) Overlay_Hold((LONG)arg); break;
    case CMD_OVERVIEW: if (g_roleOverview) Overlay_OpenPersistent(); break;
    case CMD_SECTION:
        if (!g_roleOverview) Section_Open(arg);
        else { HWND res = (HWND)(LONG_PTR)g_sh->residentHwnd; if (res) PostMessageW(res, WM_GS_VIEWER, VEV_SECTION, arg); }
        break;
    case CMD_QUIT:
        if (g_ov.hwnd) Ov_Close();
        if (g_sec.hwnd) DestroyWindow(g_sec.hwnd);
        PostQuitMessage(0);
        return;
    case CMD_RELOAD: Viewer_Reload(); break;
    }
    Viewer_CheckIdle();
}
static LRESULT CALLBACK VCtl_Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_GS_CMD: Viewer_Command((int)wp, (int)lp); return 0;
    case WM_GS_SAMPLE: Det_Kick(); Overlay_OnSample(); Section_OnSample(); return 0;
    case WM_GS_DETAIL: Det_Fetch(); Section_OnDetail(); return 0;
    case WM_TIMER:
        if (wp == VT_LINGER) { KillTimer(h, VT_LINGER); if (!g_ov.hwnd && !g_sec.hwnd) PostQuitMessage(0); }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void Viewer_Fonts() {
    if (g_tfIcon) return;
    const wchar_t* fams[2] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
    for (auto f : fams) {
        IDWriteFontCollection* fc = nullptr; UINT32 idx = 0; BOOL exists = FALSE;
        if (SUCCEEDED(g_dw->GetSystemFontCollection(&fc, FALSE)) && fc) { fc->FindFamilyName(f, &idx, &exists); fc->Release(); }
        if (exists && SUCCEEDED(g_dw->CreateTextFormat(f, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"en-us", &g_tfIcon))) break;
    }
}
static void Viewer_RegisterClasses() {
    HICON big = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON sm = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc); wc.hInstance = g_hInst;
    wc.lpfnWndProc = VCtl_Proc; wc.lpszClassName = CLS_VIEWER;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = Ov_Proc; wc.lpszClassName = L"GlassStackOverlay"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    wc.lpfnWndProc = Sec_Proc; wc.lpszClassName = L"GlassStackSection"; wc.hIcon = big; wc.hIconSm = sm;
    RegisterClassExW(&wc);
}
static int Viewer_Main(int cmd, int arg) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    AllowDarkMenus();
    if (!Sh_Open()) {
        MessageBoxW(nullptr, L"GlassStack is not running. Start GlassStack.exe first.", APP_NAME, MB_ICONINFORMATION);
        return 1;
    }
    g_dark = ViewerDark();
    g_roleOverview = cmd == CMD_HOLD || cmd == CMD_OVERVIEW;
    g_gfxHardware = false;            // both roles draw in software; the compositor animates the overview
    if (!D2D_Init()) return 2;
    Viewer_Fonts();
    Viewer_RegisterClasses();
    g_vCtl = CreateWindowExW(0, CLS_VIEWER, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hInst, nullptr);
    if (!g_vCtl) return 3;
    volatile LONGLONG* slot = g_roleOverview ? &g_sh->overviewHwnd : &g_sh->viewerHwnd;
    if (g_roleOverview) g_sh->overviewPid = (LONG)GetCurrentProcessId(); else g_sh->viewerPid = (LONG)GetCurrentProcessId();
    InterlockedExchange64(slot, (LONGLONG)(LONG_PTR)g_vCtl);
    HWND res = (HWND)(LONG_PTR)g_sh->residentHwnd;
    if (res) PostMessageW(res, WM_GS_VIEWER, g_roleOverview ? VEV_READY_OV : VEV_READY, (LPARAM)g_vCtl);
    Sh_ReadSnap(g_sec.snap);
    if (!g_roleOverview) Det_Start();
    Viewer_Command(cmd, arg);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (g_ov.hwnd) Ov_Close();
    if (g_sec.hwnd) DestroyWindow(g_sec.hwnd);
    InterlockedCompareExchange64(slot, 0, (LONGLONG)(LONG_PTR)g_vCtl);
    if (g_roleOverview) g_sh->overlayHold = 0; else { g_sh->fastGpu = 0; g_sh->viewSec = -1; }
    if (res) PostMessageW(res, WM_GS_VIEWER, VEV_EXIT, 0);
    Det_Stop();
    SafeRelease(g_tfIcon);
    D2D_Shutdown();
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------- --render-test <dir>: every view as PNG, synthetic data
static unsigned g_rtSeed = 12345;
static float Rnd() { g_rtSeed = g_rtSeed * 1664525u + 1013904223u; return (g_rtSeed >> 8) / 16777216.0f; }
static float Burst(int t, int period, int width, float amp, int phase) {
    int k = (t + phase) % period;
    if (k >= width) return 0;
    float x = (float)k / width;
    return amp * sinf(x * 3.14159f);
}
static void RT_Synth() {
    GSInfo& in = g_sh->info;
    wcscpy_s(in.cpuName, L"Intel(R) Core(TM) Ultra 9 275HX");
    in.logical = 24; in.cores = 24; in.pCores = 8; in.eCores = 16; in.hybrid = 1;
    int pcs[] = { 0, 1, 10, 11, 12, 13, 22, 23 };
    for (int p : pcs) in.coreClass[p] = 1;
    in.memTotal = 127.43 * GIB;
    in.gpuFound = 1; wcscpy_s(in.gpuName, L"NVIDIA GeForce RTX 5080 Laptop GPU");
    in.gpuDedTotal = 15.63 * GIB; in.gpuShTotal = 101.95 * GIB; in.gpuNodes = 16;
    in.engPresent[0] = in.engPresent[1] = in.engPresent[2] = in.engPresent[3] = 1;
    for (int k = 0; k < 2; k++) {
        in.diskPresent[k] = 1; in.diskNum[k] = k; in.diskSsd[k] = 1; in.diskBus[k] = 17; in.diskSize[k] = 1024209543168.0;
        wcscpy_s(in.diskModel[k], k == 0 ? L"NVMe HFS001TFM9X186N" : L"NVMe HFS001TEJ9X125N");
        wcscpy_s(in.diskLetters[k], k == 0 ? L"C:" : L"D:");
        swprintf(in.diskTitle[k], 40, L"Disk %d (%s)", k + 1, in.diskLetters[k]);
    }
    auto P = [](int id, double v) { g_sh->hist[id].push((float)v); };
    for (int t = 0; t < HIST; t++) {
        float n = Rnd();
        float cpu = 7 + 4 * sinf(t / 37.f) + 3 * n + Burst(t, 97, 14, 46, 0) + Burst(t, 251, 30, 22, 40);
        float mem = 36.6f + 0.6f * sinf(t / 400.f) + (t > 3420 ? 2.4f : 0) + 0.05f * n;
        float gpu = Burst(t, 41, 26, 88, 7) * (t > 3300 ? 1.0f : 0.25f) + 2 * Rnd();
        float ded = 9.6f + (t > 3460 ? 2.2f : 0) + 0.02f * n;
        P(S_CPU, std::min(100.f, cpu));
        P(S_MEM, mem); P(S_MEM_COMMIT, mem + 15.1f); P(S_MEM_CACHE, 50.2f + 0.4f * sinf(t / 600.f));
        P(S_GPU, std::min(100.f, gpu)); P(S_GPU_DED, ded); P(S_GPU_SH, 9.1f + 0.15f * sinf(t / 90.f));
        P(S_GPU_3D, std::min(100.f, gpu)); P(S_GPU_COPY, gpu * 0.18f); P(S_GPU_VDEC, 4 + 3 * sinf(t / 23.f) + 2 * Rnd()); P(S_GPU_VENC, 0); P(S_GPU_COMP, NAN);
        float r1 = (Burst(t, 33, 5, 1, 3) > 0 ? powf(10, 5.5f + 2.2f * Rnd()) : powf(10, 3.5f + 1.5f * Rnd()));
        float w1 = (t % 9 < 2 ? powf(10, 5.8f + 1.0f * Rnd()) : powf(10, 4.2f + 0.8f * Rnd()));
        P(S_DK1_R, r1); P(S_DK1_W, w1); P(S_DK1_ACT, std::min(100.f, (r1 + w1) / 4e6f * 100)); P(S_DK1_RESP, 0.15f + 0.3f * Rnd());
        float r2 = t > 3500 && t < 3540 ? 1.8e9f * (0.7f + 0.3f * Rnd()) : 0.f;
        P(S_DK2_R, r2); P(S_DK2_W, t % 61 == 0 ? 8e4f : 0.f); P(S_DK2_ACT, r2 > 0 ? 96.f : 0.3f * Rnd()); P(S_DK2_RESP, r2 > 0 ? 0.9f : 0.f);
        float rx = powf(10, 4.0f + 1.2f * Rnd()) + Burst(t, 57, 18, 11.5e6f, 11);
        P(S_NET_RX, rx); P(S_NET_TX, rx * 0.06f + 3000 * Rnd());
        P(S_ETH_RX, NAN); P(S_ETH_TX, NAN);
        g_ser[S_PING]->push(13 + 4 * Rnd() + (t % 47 == 0 ? 60.f : 0.f));
        g_ser[S_WSIG]->push(70 + 4 * sinf(t / 50.f));
        g_ser[S_GPU_TEMP]->push(52 + gpu * 0.12f);
        g_ser[S_GPU_POWER]->push(18 + gpu * 0.9f);
    }
    for (int k = 0; k < GS_COREHIST; k++) {
        for (int c = 0; c < in.logical; c++) {
            float base = in.coreClass[c] ? 9.f : 4.f;
            g_sh->coreHist[c][k] = std::min(100.f, base + 10 * Rnd() + Burst(k + c * 7, 40, 8, in.coreClass[c] ? 70.f : 35.f, c));
        }
    }
    g_sh->coreHead = 0; g_sh->coreCount = GS_COREHIST;
    GSSnap& s = g_sh->snap;
    s.tick = HIST; s.uptimeMs = (ULONGLONG)(3 * 86400 + 5 * 3600 + 17 * 60) * 1000;
    s.cpu = SER(S_CPU).at(0); s.nCores = in.logical;
    for (int c = 0; c < in.logical; c++) s.core[c] = CoreAt(c, 0);
    s.memUsed = SER(S_MEM).at(0) * GIB; s.memAvail = in.memTotal - s.memUsed; s.commit = SER(S_MEM_COMMIT).at(0) * GIB; s.commitLimit = 144.43 * GIB;
    s.cached = SER(S_MEM_CACHE).at(0) * GIB; s.paged = 2.58 * GIB; s.nonPaged = 1.74 * GIB; s.procs = 368; s.threads = 5164; s.handles = 2688759;
    s.gpu = SER(S_GPU).at(0); s.gpuDed = SER(S_GPU_DED).at(0) * GIB; s.gpuSh = SER(S_GPU_SH).at(0) * GIB;
    for (int k = 0; k < 2; k++) { s.dOk[k] = 1; s.dR[k] = SER(k ? S_DK2_R : S_DK1_R).at(0); s.dW[k] = SER(k ? S_DK2_W : S_DK1_W).at(0); s.dAct[k] = SER(k ? S_DK2_ACT : S_DK1_ACT).at(0); s.dResp[k] = SER(k ? S_DK2_RESP : S_DK1_RESP).at(0); }
    s.netRx = SER(S_NET_RX).at(0); s.netTx = SER(S_NET_TX).at(0); s.ethRx = s.ethTx = NAN;
    s.wifiFound = 1; s.wifiUp = 1; s.ethFound = 1; s.ethUp = 0; s.wifiLink = 144000000; s.wifiTotRx = 48ull * 1024 * 1024 * 1024; s.wifiTotTx = 3ull * 1024 * 1024 * 1024;
    wcscpy_s(s.wifiName, L"Killer(TM) Wi-Fi 7 BE1750x 320MHz Wireless Network Adapter (BE200NGW)");
    wcscpy_s(s.ethName, L"Killer E5000B 5 Gigabit Ethernet Controller");
    // detail data
    DetailData& d = g_det;
    struct PR { const wchar_t* n; int cnt; double mem, cpu, io, gpu, gpuMem; DWORD h; } pr[] = {
        { L"llama-server.exe", 1, 3.2 * GIB, 4.1, 2.1e6, 81, 11.2 * GIB, 410 },
        { L"chrome.exe", 31, 4.6 * GIB, 2.3, 4.2e5, 3, 0.31 * GIB, 9800 },
        { L"explorer.exe", 1, 0.56 * GIB, 0.4, 1.2e4, 1, 0.05 * GIB, 10081 },
        { L"dwm.exe", 1, 0.58 * GIB, 1.1, 0, 6, 0.42 * GIB, 1995 },
        { L"Code.exe", 12, 1.9 * GIB, 1.6, 8.0e4, 0, 0.09 * GIB, 6100 },
        { L"python.exe", 3, 2.4 * GIB, 6.8, 6.1e6, 12, 1.6 * GIB, 900 },
        { L"MsMpEng.exe", 1, 0.31 * GIB, 0.9, 2.3e6, 0, 0, 1200 },
        { L"Telegram.exe", 1, 0.42 * GIB, 0.2, 1.0e3, 0, 0.02 * GIB, 1400 },
        { L"AcerSystemCentralService.exe", 1, 0.0014 * GIB, 0.1, 0, 0, 0, 1613173 },
        { L"AcerCentralService.exe", 1, 0.0023 * GIB, 0.0, 0, 0, 0, 922973 },
        { L"Memory Compression", 1, 0.0 * GIB, 0.0, 0, 0, 0, 0 },
        { L"svchost.exe", 88, 1.2 * GIB, 0.7, 3.0e4, 0, 0, 30000 },
        { L"LM Studio.exe", 6, 0.9 * GIB, 0.3, 0, 1, 0.12 * GIB, 2200 },
        { L"System", 1, 0.0001 * GIB, 0.6, 9.0e5, 0, 0, 7305 },
    };
    static const int conns[] = { 0, 46, 2, 0, 9, 3, 1, 4, 1, 0, 0, 14, 0, 0 };
    d.procs.clear();
    for (auto& p : pr) {
        ProcRow r; r.name = p.n; r.count = p.cnt; r.mem = p.mem; r.cpu = p.cpu; r.io = p.io; r.gpu = p.gpu; r.gpuMem = p.gpuMem; r.handles = p.h;
        r.conns = conns[d.procs.size() % 14];
        r.pids.push_back(1000 + (DWORD)d.procs.size()); r.bigPid = r.pids[0]; r.bigMem = p.mem;
        d.procs.push_back(r);
    }
    d.procsValid = true; d.compressed = 0.52 * GIB;
    d.leakName = L"AcerSystemCentralService.exe"; d.leakHandles = 1613173;
    d.gpuTemp = SER(S_GPU_TEMP).at(0); d.gpuPower = SER(S_GPU_POWER).at(0);
    d.ping = SER(S_PING).at(0); d.wsig = SER(S_WSIG).at(0); d.wRx = 1201; d.wTx = 960; d.wPhy = 11; d.wChannel = 37;
    wcscpy_s(d.ssid, L"HomeNet 6E"); wcscpy_s(d.ipv4, L"192.168.1.34");
    g_sec.snap = s;
}
static bool RT_Render(IWICImagingFactory* wic, const wchar_t* dir, const wchar_t* name, float wDip, float hDip, int kind) {
    const float dpi = 144.f;
    UINT W = (UINT)lroundf(wDip * dpi / 96.f), H = (UINT)lroundf(hDip * dpi / 96.f);
    IWICBitmap* bmp = nullptr;
    if (FAILED(wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return false;
    D2D1_RENDER_TARGET_PROPERTIES rp = {};
    rp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    rp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM; rp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    rp.dpiX = rp.dpiY = dpi;
    ID2D1RenderTarget* rt = nullptr;
    if (FAILED(g_d2d->CreateWicBitmapRenderTarget(bmp, &rp, &rt))) { bmp->Release(); return false; }
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    RCtx c; RC_Init(c, rt);
    rt->BeginDraw();
    if (kind == 0) { Ov_SynthBackdrop((int)W, (int)H); Ov_MakeBitmaps(c); Ov_Render(c, wDip, hDip); }
    else { Sec_Render(c, wDip, hDip); Sec_ChartHover(c); }
    HRESULT hr = rt->EndDraw();
    SafeRelease(g_ov.glassBrush); SafeRelease(g_ov.bmpBack); SafeRelease(g_ov.bmpGlass);
    RC_Release(c);
    rt->Release();
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%s\\%s.png", dir, name);
    bool ok = SUCCEEDED(hr) && SavePng(wic, bmp, path);
    bmp->Release();
    return ok;
}
static bool RT_Strip(IWICImagingFactory* wic, const wchar_t* dir, const wchar_t* name, int zoom) {
    const int W = 2560, H = 12;
    std::vector<DWORD> px((size_t)W * H);
    g_dark = !wcsstr(name, L"light") ? true : false;
    g_barBg = g_dark ? Hex(0x1C1C1C) : Hex(0xEEEEEE);
    Bar_RenderPixels(px.data(), W, H, 1.5f, wcsstr(name, L"hover") ? 1 : -1);
    int ZW = W, ZH = H * zoom;
    std::vector<DWORD> z((size_t)ZW * ZH);
    for (int y = 0; y < ZH; y++) memcpy(&z[(size_t)y * ZW], &px[(size_t)(y / zoom) * W], W * 4);
    IWICBitmap* bmp = nullptr;
    if (FAILED(wic->CreateBitmapFromMemory(ZW, ZH, GUID_WICPixelFormat32bppBGR, ZW * 4, (UINT)(z.size() * 4), (BYTE*)z.data(), &bmp))) return false;
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%s\\%s.png", dir, name);
    bool ok = SavePng(wic, bmp, path);
    bmp->Release();
    return ok;
}
// the strip's hover readout, as it shows above the strip
static bool RT_Pop(IWICImagingFactory* wic, const wchar_t* dir, const wchar_t* name, int hover) {
    const float sc = 1.5f;
    int W, H; Pop_Size(sc, W, H);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    HBITMAP bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ o = SelectObject(mem, bmp);
        Pop_PaintTo(mem, W, H, sc, hover);
        GdiFlush();
        SelectObject(mem, o);
        IWICBitmap* wb = nullptr;
        if (SUCCEEDED(wic->CreateBitmapFromMemory(W, H, GUID_WICPixelFormat32bppBGR, W * 4, (UINT)W * H * 4, (BYTE*)bits, &wb))) {
            wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%s\\%s.png", dir, name);
            ok = SavePng(wic, wb, path);
            wb->Release();
        }
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem); ReleaseDC(nullptr, scr);
    return ok;
}
static int RenderTest(const wchar_t* dir) {
    g_renderTest = true;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CreateDirectoryW(dir, nullptr);
    if (!Sh_Private() || !D2D_Init()) return 2;
    Viewer_Fonts();
    RT_Synth();
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return 3;
    int fails = 0;
    for (int theme = 0; theme < 2; theme++) {
        g_dark = theme == 1;
        const wchar_t* tn = g_dark ? L"dark" : L"light";
        wchar_t nm[64];
        // overview, held open with Memory focused, then with GPU workload focused
        int focusSet[2] = { 0, 1 };
        for (int fi = 0; fi < 2; fi++) {
            g_ov.focus = focusSet[fi]; g_ov.hover = -1; g_ov.appear = 1; g_ov.hold = fi == 0;
            for (int i = 0; i < G_COUNT; i++) { g_ov.pull[i] = i == g_ov.focus ? 1.f : 0.f; g_ov.hov[i] = 0; }
            swprintf(nm, 64, L"overview_%s_%d", tn, fi);
            if (!RT_Render(wic, dir, nm, 1706.67f, 1066.67f, 0)) fails++;
        }
        // section windows
        struct SV { int g; const wchar_t* n; } sv[] = { { G_MEM, L"mem" }, { G_GPU, L"gpu" }, { G_GPUMEM, L"gpumem" }, { G_DISK1, L"disk1" }, { G_DISK2, L"disk2" }, { G_WIFI, L"wifi" }, { G_CPU, L"cpu" } };
        for (auto& v : sv) {
            if (g_dark && v.g != G_MEM && v.g != G_GPU && v.g != G_CPU) continue;
            g_sec.sec = g_graphs[v.g].section; g_sec.mainC = v.g == G_GPU ? g_secDefault[g_sec.sec] : ChartOfGraph(v.g); g_sec.scroll = 0; g_sec.hoverRow = -1; g_sec.hoverTab = -1;
            g_sec.mouseIn = v.g == G_GPU; g_sec.mx = 560; g_sec.my = 300;
            swprintf(nm, 64, L"section_%s_%s", v.n, tn);
            if (!RT_Render(wic, dir, nm, 1240, 760, 1)) fails++;
        }
    }
    for (int t = 0; t < 2; t++) {
        g_dark = t == 1;
        if (!RT_Pop(wic, dir, g_dark ? L"popup_dark" : L"popup_light", 1)) fails++;
    }
    g_dark = false;
    if (!RT_Strip(wic, dir, L"strip_light_hover", 1)) fails++;
    if (!RT_Strip(wic, dir, L"strip_dark_hover", 1)) fails++;
    if (!RT_Strip(wic, dir, L"strip_light", 1)) fails++;
    if (!RT_Strip(wic, dir, L"strip_light_zoom", 8)) fails++;
    if (!RT_Strip(wic, dir, L"strip_dark_zoom", 8)) fails++;
    if (!RT_Strip(wic, dir, L"strip_light_hover_zoom", 8)) fails++;
    wic->Release();
    SafeRelease(g_tfIcon);
    D2D_Shutdown();
    CoUninitialize();
    return fails;
}

// ---------------------------------------------------------------- --bench <file> <mt> [hw]: CPU and memory of real frames
// Renders the section window (GPU section, live data) and the overview in off-screen windows, so nothing shows up.
static double Bench_CpuMs() {
    FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULARGE_INTEGER a, b; a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime; b.LowPart = u.dwLowDateTime; b.HighPart = u.dwHighDateTime;
    return (double)(a.QuadPart + b.QuadPart) / 1e4;
}
static double Bench_Thread() { ULONG64 c = 0; QueryThreadCycleTime(GetCurrentThread(), &c); return (double)c; }
static void Bench_Pump(int ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(5);
    }
}
static void Bench_Mem(FILE* f, const wchar_t* tag) {
    PROCESS_MEMORY_COUNTERS_EX pmc = {}; pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    DWORD thr = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof(te) };
    if (Thread32First(s, &te)) do { if (te.th32OwnerProcessID == GetCurrentProcessId()) thr++; } while (Thread32Next(s, &te));
    CloseHandle(s);
    fwprintf(f, L"%-28s private %6.1f MB  working set %6.1f MB  threads %lu\n", tag, pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0, thr);
}
// what the compositor shows for a window (PrintWindow asks DWM for the full content), saved next to the report
static void Bench_Shot(FILE* f, HWND h, const wchar_t* out, const wchar_t* tag) {
    RECT rc; GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    HBITMAP bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ o = SelectObject(mem, bmp);
        ok = PrintWindow(h, mem, 2 /* PW_RENDERFULLCONTENT */) != 0;
        GdiFlush();
        SelectObject(mem, o);
        IWICImagingFactory* wic = nullptr; IWICBitmap* wb = nullptr;
        if (ok && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateBitmapFromMemory(W, H, GUID_WICPixelFormat32bppBGR, W * 4, (UINT)W * H * 4, (BYTE*)bits, &wb))) {
            wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%s.%s.png", out, tag);
            ok = SavePng(wic, wb, path);
        }
        SafeRelease(wb); SafeRelease(wic);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem); ReleaseDC(nullptr, scr);
    fwprintf(f, L"%s picture: %s\n", tag, ok ? L"saved" : L"failed");
}
// CPU time of every thread of this process, with the module its thread function lives in
static void Bench_Threads(FILE* f, const wchar_t* tag) {
    typedef LONG(WINAPI* QIT)(HANDLE, int, PVOID, ULONG, PULONG);
    QIT qit = (QIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof(te) };
    fwprintf(f, L"%s threads:", tag);
    if (Thread32First(s, &te)) do {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE t = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        FILETIME c, e, k, u; double ms = 0;
        if (GetThreadTimes(t, &c, &e, &k, &u)) ms = (((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 1e4;
        void* start = nullptr; wchar_t mod[MAX_PATH] = L"?";
        if (qit) qit(t, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr);
        HMODULE hm = nullptr;
        if (start && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)start, &hm)) {
            wchar_t full[MAX_PATH]; GetModuleFileNameW(hm, full, MAX_PATH);
            const wchar_t* b = wcsrchr(full, (wchar_t)92); wcscpy_s(mod, b ? b + 1 : full);
        }
        fwprintf(f, L"  %lu %s %.0f ms", te.th32ThreadID, mod, ms);
        CloseHandle(t);
    } while (Thread32Next(s, &te));
    CloseHandle(s);
    fwprintf(f, L"\n");
}
// the largest committed private blocks of this process (what the device and the surfaces allocate)
static void Bench_Regions(FILE* f, const wchar_t* tag) {
    std::vector<std::pair<SIZE_T, void*>> big;
    SIZE_T total = 0, heapish = 0;
    MEMORY_BASIC_INFORMATION mb;
    for (BYTE* p = nullptr; VirtualQuery(p, &mb, sizeof(mb)) == sizeof(mb); p = (BYTE*)mb.BaseAddress + mb.RegionSize) {
        if (mb.State == MEM_COMMIT && mb.Type == MEM_PRIVATE) {
            total += mb.RegionSize;
            if (mb.RegionSize >= 512 * 1024) big.push_back({ mb.RegionSize, mb.BaseAddress }); else heapish += mb.RegionSize;
        }
        if ((BYTE*)mb.BaseAddress + mb.RegionSize < p) break;
    }
    std::sort(big.begin(), big.end(), [](auto& a, auto& b) { return a.first > b.first; });
    fwprintf(f, L"%s: committed private %.1f MB, in blocks < 512 KB %.1f MB, larger blocks:", tag, total / 1048576.0, heapish / 1048576.0);
    for (size_t i = 0; i < big.size() && i < 14; i++) fwprintf(f, L" %.2f", big[i].first / 1048576.0);
    fwprintf(f, L" (%zu blocks)\n", big.size());
}
// parts: 1 = the section window, 2 = the overview, 3 = both (each in an off-screen window)
static int Bench(const wchar_t* out, bool mt, bool hw, int parts) {
    g_renderTest = false;
    g_warpMT = mt;
    g_gfxHardware = hw;
    FILE* f = nullptr;
    if (_wfopen_s(&f, out, L"w, ccs=UTF-8") != 0 || !f) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!Sh_Open()) { fwprintf(f, L"resident not running\n"); fclose(f); return 3; }
    g_dark = ViewerDark();
    if (!D2D_Init()) return 4;
    Viewer_Fonts();
    Viewer_RegisterClasses();
    g_vCtl = CreateWindowExW(0, CLS_VIEWER, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hInst, nullptr);
    fwprintf(f, L"%s\n", hw ? L"hardware device" : mt ? L"WARP multi-threaded" : L"WARP single-threaded");
    Bench_Mem(f, L"start");
    Gfx_Ensure();
    Bench_Mem(f, L"software device");
    LARGE_INTEGER q0 = {}, q1 = {}, fq; QueryPerformanceFrequency(&fq);
    auto msq = [&]() { return (q1.QuadPart - q0.QuadPart) * 1000.0 / fq.QuadPart; };
    double c0 = 0, c1 = 0;

    if (parts & 1) {
        // section window, off screen, 20 s of live updates
        UINT dpi = 144;
        g_sec.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, L"GlassStackSection", L"bench", WS_POPUP,
                                     -12000, -12000, MulDiv(1240, dpi, 96), MulDiv(800, dpi, 96), nullptr, nullptr, g_hInst, nullptr);
        ShowWindow(g_sec.hwnd, SW_SHOWNOACTIVATE);
        Det_Start();
        Sh_ReadSnap(g_sec.snap);
        Sec_SwitchTo(SEC_GPU, C_GPUMEM);
        UpdateWindow(g_sec.hwnd);
        Bench_Pump(2500);
        Bench_Mem(f, L"section open");
        c0 = Bench_CpuMs(); ULONGLONG t0 = GetTickCount64(); int fr0 = g_secFrames;
        for (int i = 0; i < 20; i++) {
            Section_OnSample();
            Det_Kick();
            Bench_Pump(1000);
        }
        c1 = Bench_CpuMs(); double el = (GetTickCount64() - t0) / 1000.0;
        fwprintf(f, L"section, live updates: %.1f ms CPU per second (incl. detail thread), %d frames in %.0f s\n", (c1 - c0) / el, g_secFrames - fr0, el);
        Bench_Threads(f, L"after live updates");
        Bench_Mem(f, L"after 20 s");
        Bench_Regions(f, L"section");
        // hover: 40 back-to-back frames with the crosshair moving
        g_sec.mouseIn = true;
        c0 = Bench_CpuMs(); QueryPerformanceCounter(&q0);
        for (int i = 0; i < 40; i++) {
            g_sec.mx = g_sec.chartR.left + (g_sec.chartR.right - g_sec.chartR.left) * (0.2f + i * 0.015f);
            g_sec.my = (g_sec.chartR.top + g_sec.chartR.bottom) * 0.5f;
            InvalidateRect(g_sec.hwnd, nullptr, FALSE);
            UpdateWindow(g_sec.hwnd);
        }
        QueryPerformanceCounter(&q1); c1 = Bench_CpuMs();
        fwprintf(f, L"section hover frame: %.1f ms wall, %.1f ms CPU\n", msq() / 40, (c1 - c0) / 40);
        Bench_Shot(f, g_sec.hwnd, out, L"section");
        g_sec.mouseIn = false;
        c0 = Bench_CpuMs(); QueryPerformanceCounter(&q0);
        for (int i = 0; i < 10; i++) { Sec_Dirty(); UpdateWindow(g_sec.hwnd); }
        QueryPerformanceCounter(&q1); c1 = Bench_CpuMs();
        fwprintf(f, L"section full frame: %.1f ms wall, %.1f ms CPU\n", msq() / 10, (c1 - c0) / 10);
    }

    if (parts & 2) {
        // overview (2560x1600), off screen: opening draws every surface once; an animation frame only moves them;
        // a focus change redraws two cards, the outline, the colour glow and two text panels; new data (once a
        // second) redraws the cards and the text panels
        g_ov.mon = { -12000, -12000, -12000 + 2560, -12000 + 1600 };
        c0 = Bench_CpuMs(); QueryPerformanceCounter(&q0);
        Ov_SynthBackdrop(2560, 1600);
        g_ov.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, L"GlassStackOverlay", L"bench", WS_POPUP,
                                    -12000, -12000, 2560, 1600, nullptr, nullptr, g_hInst, nullptr);
        ShowWindow(g_ov.hwnd, SW_SHOWNOACTIVATE);
        g_ov.hold = false; g_ov.focus = 0; g_ov.hover = -1; g_ov.appear = 0;
        for (int k = 0; k < G_COUNT; k++) { g_ov.pull[k] = 0; g_ov.hov[k] = 0; }
        bool ovOk = Ov_CreateRT();
        QueryPerformanceCounter(&q1); c1 = Bench_CpuMs();
        fwprintf(f, L"overview open%s: %.1f ms wall, %.1f ms CPU\n", ovOk ? L"" : L" FAILED", msq(), c1 - c0);
        Bench_Mem(f, L"overview open");
        g_ov.shown = true;
        c0 = Bench_CpuMs(); QueryPerformanceCounter(&q0);
        for (int i = 0; i < 120; i++) {
            float t = (float)i / 119.f;
            g_ov.appear = std::min(1.f, t * 2.f);
            for (int k = 0; k < G_COUNT; k++) g_ov.pull[k] = k == g_ov.focus ? std::min(1.f, t * 1.5f) : 0.f;
            Ov_CompUpdate();
        }
        QueryPerformanceCounter(&q1); c1 = Bench_CpuMs();
        fwprintf(f, L"overview animation frame: %.3f ms wall, %.3f ms CPU\n", msq() / 120, (c1 - c0) / 120);
        c0 = Bench_CpuMs(); QueryPerformanceCounter(&q0);
        for (int i = 1; i <= 11; i++) { Ov_SetFocus(g_ov.focus + 1); Ov_CompUpdate(); }
        QueryPerformanceCounter(&q1); c1 = Bench_CpuMs();
        fwprintf(f, L"overview focus change: %.1f ms wall, %.1f ms CPU\n", msq() / 11, (c1 - c0) / 11);
        c0 = Bench_CpuMs(); QueryPerformanceCounter(&q0);
        for (int i = 0; i < 5; i++) Overlay_OnSample();
        QueryPerformanceCounter(&q1); c1 = Bench_CpuMs();
        fwprintf(f, L"overview data refresh: %.1f ms wall, %.1f ms CPU\n", msq() / 5, (c1 - c0) / 5);
        for (int i = 0; i < 20; i++) Overlay_OnSample();
        Bench_Pump(1500);
        Bench_Mem(f, L"overview after use");
        Bench_Regions(f, L"overview");
        Bench_Shot(f, g_ov.hwnd, out, L"overview");
        Ov_Close();
    }

    if (g_sec.hwnd) DestroyWindow(g_sec.hwnd);
    Bench_Pump(500);
    Bench_Mem(f, L"closed");
    Det_Stop();
    D2D_Shutdown();
    fclose(f);
    return 0;
}
