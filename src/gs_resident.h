// GlassStack - resident process: hidden main window, sampling timer, tray icon + menu, global hotkey,
// and the launcher/messenger for the on-demand viewer process.
#pragma once

#define TIMER_SAMPLE     1
#define TIMER_FULLSCREEN 2
#define TIMER_TRIM       3
#define TIMER_RESAMPLE   4

enum {
    IDM_OVERVIEW = 100, IDM_SEC_BASE = 110,
    IDM_STRIP = 130, IDM_STRIPH_BASE = 140,
    IDM_NETBITS = 160, IDM_NETBYTES = 161,
    IDM_DISK1_BASE = 170, IDM_DISK2_BASE = 190,
    IDM_THEME_BASE = 210,
    IDM_STARTUP = 220, IDM_SETTINGS = 221, IDM_RESTART_VIEWER = 222, IDM_EXIT = 230,
};

static HWND g_hMain = nullptr;
static UINT g_msgTaskbarCreated = 0;
static NOTIFYICONDATAW g_nid;
static bool g_trayOk = false, g_hotkeyOk = false;
static HICON g_iconSmall = nullptr, g_iconBig = nullptr;
// Two short-lived helper processes run the windows. The overview uses the GPU for smooth animation and
// exits the moment it closes (the graphics driver's ~110 MB go with it); the section window renders in
// software (~45 MB) and lingers ViewerLingerSeconds after it closes, so reopening it is instant.
enum { ROLE_SECTION = 0, ROLE_OVERVIEW = 1 };
struct PendingCmd { int cmd, arg; };
struct ViewerProc { HANDLE proc = nullptr; DWORD pid = 0; ULONGLONG launchTick = 0; std::vector<PendingCmd> pending; };
static ViewerProc g_vp[2];

static void AllowDarkMenus() {
    typedef int(WINAPI* PFN_SetPreferredAppMode)(int);
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) ux = LoadLibraryW(L"uxtheme.dll");
    PFN_SetPreferredAppMode f = ux ? (PFN_SetPreferredAppMode)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : nullptr;
    if (f) f(1);   // AllowDark: popup menus follow the Windows app theme
}

// ---------------------------------------------------------------- start with Windows
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool Startup_Get() {
    wchar_t v[MAX_PATH * 2]; DWORD sz = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, nullptr, v, &sz) == ERROR_SUCCESS;
}
static void Startup_Set(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t v[MAX_PATH + 4];
        swprintf(v, MAX_PATH + 4, L"\"%s\"", g_exePath);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE*)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, APP_NAME);
    RegCloseKey(k);
}

// ---------------------------------------------------------------- viewer process
static HWND Viewer_Hwnd(int role) { return (HWND)(LONG_PTR)(role == ROLE_OVERVIEW ? g_sh->overviewHwnd : g_sh->viewerHwnd); }
static DWORD Viewer_Pid(int role) { return (DWORD)(role == ROLE_OVERVIEW ? g_sh->overviewPid : g_sh->viewerPid); }
static bool Viewer_Alive(int role) { HWND v = Viewer_Hwnd(role); return v && IsWindow(v); }
static bool Viewer_Launching(int role) {
    ViewerProc& p = g_vp[role];
    if (!p.proc) return false;
    if (WaitForSingleObject(p.proc, 0) == WAIT_OBJECT_0) { CloseHandle(p.proc); p.proc = nullptr; p.pending.clear(); return false; }
    return !Viewer_Alive(role) && GetTickCount64() - p.launchTick < 15000;
}
static void Viewer_Post(int role, int cmd, int arg) {
    AllowSetForegroundWindow(Viewer_Pid(role));
    PostMessageW(Viewer_Hwnd(role), WM_GS_CMD, (WPARAM)cmd, (LPARAM)arg);
}
static void Viewer_Send(int cmd, int arg) {
    if (cmd == CMD_QUIT || cmd == CMD_RELOAD) {          // for whoever runs; never starts a process
        for (int r = 0; r < 2; r++) if (Viewer_Alive(r)) Viewer_Post(r, cmd, arg);
        return;
    }
    int role = (cmd == CMD_HOLD || cmd == CMD_OVERVIEW) ? ROLE_OVERVIEW : ROLE_SECTION;
    if (Viewer_Alive(role)) { Viewer_Post(role, cmd, arg); return; }
    ViewerProc& p = g_vp[role];
    if (Viewer_Launching(role)) { p.pending.push_back({ cmd, arg }); AllowSetForegroundWindow(p.pid); return; }
    wchar_t cl[MAX_PATH + 64];
    swprintf(cl, MAX_PATH + 64, L"\"%s\" --viewer %d %d", g_exePath, cmd, arg);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(g_exePath, cl, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        if (p.proc) CloseHandle(p.proc);
        p.proc = pi.hProcess; p.pid = pi.dwProcessId; p.launchTick = GetTickCount64();
    }
}
// CMD_SECTION argument: a graph (G_*), 100 + a chart (C_*), or 200 + a section (opens with its chosen chart)
static void Res_OpenSection(int slot) { Viewer_Send(CMD_SECTION, 100 + StripChart(slot)); }
// the extras thread collects what the strip shows (an open section window measures for itself)
static void Res_UpdateExtras() {
    int w = 0;
    if (g_bar)
        for (int i = 0; i < g_cfg.nGraphs; i++) {
            int s = g_charts[StripChart(i)].sA;
            if (s == S_GPU_TEMP) w |= EX_TEMP; else if (s == S_PING) w |= EX_PING; else if (s == S_WSIG) w |= EX_WSIG;
        }
    Ex_SetWant(w);
}
static void Res_OnHotkey() {
    LONG t = InterlockedIncrement(&g_sh->taps);
    if (g_sh->overlayHold && Viewer_Alive(ROLE_OVERVIEW)) return;     // the held-open overview reads the tap counter itself
    Viewer_Send(CMD_HOLD, (int)t);
}

// ---------------------------------------------------------------- tray icon
static void Tray_Tip() {
    if (!g_trayOk) return;
    GSSnap s; Sh_ReadSnap(s);
    wchar_t cpu[16], gpu[16];
    FmtVal(cpu, 16, U_PCT, s.cpu); FmtVal(gpu, 16, U_PCT, s.gpu);
    swprintf(g_nid.szTip, 128, L"GlassStack\nCPU %s  ·  RAM %.1f / %.0f GB\nGPU %s  ·  VRAM %.1f / %.0f GB",
             cpu, s.memUsed / GIB, g_sh->info.memTotal / GIB, gpu, s.gpuDed / GIB, g_sh->info.gpuDedTotal / GIB);
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void Tray_Add() {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hMain; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_GS_TRAY;
    g_nid.hIcon = g_iconSmall;
    wcscpy_s(g_nid.szTip, APP_NAME);
    g_trayOk = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}
static void Tray_Remove() { if (g_trayOk) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_trayOk = false; } }
static void Tray_Balloon(const wchar_t* title, const wchar_t* text) {
    if (!g_trayOk) return;
    g_nid.uFlags = NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, text, _TRUNCATE);
    g_nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void Res_RegisterHotkey() {
    UnregisterHotKey(g_hMain, 1);
    g_hotkeyOk = RegisterHotKey(g_hMain, 1, g_cfg.hkMods | MOD_NOREPEAT, g_cfg.hkVk) != FALSE;
    if (!g_hotkeyOk) {
        wchar_t t[256];
        swprintf(t, 256, L"%s is already used by another app. Set another Hotkey= in GlassStack.ini (tray menu → Edit settings).", g_cfg.hkText);
        Tray_Balloon(L"GlassStack: shortcut unavailable", t);
    }
}

// ---------------------------------------------------------------- menu
static void Res_ResetDiskSeries(int k) {
    Sh_BeginWrite();
    int base = k == 0 ? S_DK1_R : S_DK2_R;
    for (int s = base; s < base + 4; s++) g_sh->hist[s].init();
    Sh_EndWrite();
}
static void Res_Command(UINT id) {
    if (id == IDM_OVERVIEW) { Viewer_Send(CMD_OVERVIEW, 0); return; }
    if (id >= IDM_SEC_BASE && id < IDM_SEC_BASE + SEC_COUNT) { Viewer_Send(CMD_SECTION, 200 + (int)(id - IDM_SEC_BASE)); return; }
    if (id == IDM_STRIP) {
        g_cfg.showStrip = !g_cfg.showStrip;
        IniPutInt(L"ShowStrip", g_cfg.showStrip);
        if (g_cfg.showStrip) Bar_Create(); else Bar_Destroy();
        return;
    }
    if (id >= IDM_STRIPH_BASE && id < IDM_STRIPH_BASE + 20) {
        g_cfg.stripHeight = (int)(id - IDM_STRIPH_BASE);
        IniPutInt(L"StripHeight", g_cfg.stripHeight);
        Bar_Reposition();
        return;
    }
    if (id == IDM_NETBITS || id == IDM_NETBYTES) {
        g_cfg.netBits = id == IDM_NETBITS;
        IniPut(L"NetworkUnits", g_cfg.netBits ? L"bits" : L"bytes");
        Viewer_Send(CMD_RELOAD, 0);
        return;
    }
    for (int k = 0; k < 2; k++) {
        UINT base = k == 0 ? IDM_DISK1_BASE : IDM_DISK2_BASE;
        if (id >= base && id < base + 16) {
            g_cfg.disk[k] = (int)(id - base);
            IniPutInt(k == 0 ? L"Disk1" : L"Disk2", g_cfg.disk[k]);
            Info_DiskList();
            Smp_OpenDisks();
            Res_ResetDiskSeries(k);
            Viewer_Send(CMD_RELOAD, 0);
            return;
        }
    }
    if (id >= IDM_THEME_BASE && id < IDM_THEME_BASE + 3) {
        g_cfg.theme = (int)(id - IDM_THEME_BASE);
        IniPut(L"Theme", g_cfg.theme == 1 ? L"light" : g_cfg.theme == 2 ? L"dark" : L"auto");
        Viewer_Send(CMD_RELOAD, 0);
        return;
    }
    if (id == IDM_STARTUP) { Startup_Set(!Startup_Get()); return; }
    if (id == IDM_SETTINGS) { ShellExecuteW(nullptr, L"open", L"notepad.exe", g_iniPath, nullptr, SW_SHOWNORMAL); return; }
    if (id == IDM_RESTART_VIEWER) { Viewer_Send(CMD_QUIT, 0); return; }
    if (id == IDM_EXIT) { DestroyWindow(g_hMain); return; }
}
static void Res_ShowMenu() {
    HMENU m = CreatePopupMenu();
    wchar_t t[256];
    swprintf(t, 256, L"3D overview\t%s", g_cfg.hkText);
    AppendMenuW(m, MF_STRING, IDM_OVERVIEW, t);
    HMENU sec = CreatePopupMenu();
    for (int s = 0; s < SEC_COUNT; s++) AppendMenuW(sec, MF_STRING, IDM_SEC_BASE + s, SectionTitle(s));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sec, L"Details");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_cfg.showStrip ? MF_CHECKED : 0), IDM_STRIP, L"Show taskbar strip");
    HMENU sh = CreatePopupMenu();
    static const int hs[] = { 3, 4, 6, 8, 10, 12, 16 };
    for (int h : hs) {
        swprintf(t, 256, h == 6 ? L"%d (default)" : L"%d", h);
        AppendMenuW(sh, MF_STRING | (g_cfg.stripHeight == h ? MF_CHECKED : 0), IDM_STRIPH_BASE + h, t);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sh, L"Strip height");
    for (int k = 0; k < 2; k++) {
        HMENU dm = CreatePopupMenu();
        for (auto& e : g_diskList) {
            wchar_t sz[32]; FmtBytes(sz, 32, e.size);
            swprintf(t, 256, L"Drive %d   %s   %s   %s", e.num, e.letters[0] ? e.letters : L"(no letter)", e.model, sz);
            AppendMenuW(dm, MF_STRING | (g_cfg.disk[k] == e.num ? MF_CHECKED : 0), (k == 0 ? IDM_DISK1_BASE : IDM_DISK2_BASE) + e.num, t);
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)dm, k == 0 ? L"Disk 1 is" : L"Disk 2 is");
    }
    HMENU nm = CreatePopupMenu();
    AppendMenuW(nm, MF_STRING | (g_cfg.netBits ? MF_CHECKED : 0), IDM_NETBITS, L"Bits per second (Kbps, Mbps)");
    AppendMenuW(nm, MF_STRING | (!g_cfg.netBits ? MF_CHECKED : 0), IDM_NETBYTES, L"Bytes per second (KB/s, MB/s)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)nm, L"Network units");
    HMENU tm = CreatePopupMenu();
    AppendMenuW(tm, MF_STRING | (g_cfg.theme == 0 ? MF_CHECKED : 0), IDM_THEME_BASE + 0, L"Follow Windows");
    AppendMenuW(tm, MF_STRING | (g_cfg.theme == 1 ? MF_CHECKED : 0), IDM_THEME_BASE + 1, L"Light");
    AppendMenuW(tm, MF_STRING | (g_cfg.theme == 2 ? MF_CHECKED : 0), IDM_THEME_BASE + 2, L"Dark");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)tm, L"Window theme");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (Startup_Get() ? MF_CHECKED : 0), IDM_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Edit settings…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit GlassStack");
    SetForegroundWindow(g_hMain);
    POINT pt; GetCursorPos(&pt);
    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hMain, nullptr);
    PostMessageW(g_hMain, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd) Res_Command(cmd);
}

// ---------------------------------------------------------------- settings edited by hand (tray menu > Edit settings)
static FILETIME g_iniTime = {};
static bool Ini_Changed() {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(g_iniPath, GetFileExInfoStandard, &a)) return false;
    bool changed = CompareFileTime(&a.ftLastWriteTime, &g_iniTime) != 0;
    g_iniTime = a.ftLastWriteTime;
    return changed;
}
// applies whatever changed; the viewers reload only for settings they show, not for the window state they save
static void Res_ReloadSettings() {
    Config o = g_cfg;
    Cfg_Load();
    const Config& n = g_cfg;
    if (o.hkMods != n.hkMods || o.hkVk != n.hkVk) Res_RegisterHotkey();
    if (o.sampleMs != n.sampleMs) SetTimer(g_hMain, TIMER_SAMPLE, (UINT)n.sampleMs, nullptr);
    bool disks = o.disk[0] != n.disk[0] || o.disk[1] != n.disk[1];
    if (disks) {
        Info_DiskList();
        Smp_OpenDisks();
        for (int k = 0; k < 2; k++) if (o.disk[k] != n.disk[k]) Res_ResetDiskSeries(k);
    }
    if (memcmp(o.secChart, n.secChart, sizeof(o.secChart)) != 0) {
        for (int s = 0; s < SEC_COUNT; s++) g_sh->secChart[s] = n.secChart[s];
        if (g_bar) InvalidateRect(g_bar, nullptr, FALSE);
    }
    bool graphs = o.nGraphs != n.nGraphs || memcmp(o.order, n.order, sizeof(o.order)) != 0;
    if (o.showStrip != n.showStrip) { if (n.showStrip) Bar_Create(); else Bar_Destroy(); }
    else if (g_bar && (graphs || o.stripHeight != n.stripHeight || o.stripPps != n.stripPps)) { Bar_Reposition(); InvalidateRect(g_bar, nullptr, FALSE); }
    if (graphs || disks || o.netBits != n.netBits || o.theme != n.theme || wcscmp(o.hkText, n.hkText) || o.sampleMs != n.sampleMs ||
        o.lingerSec != n.lingerSec || o.useNvml != n.useNvml || wcscmp(o.pingHost, n.pingHost))
        Viewer_Send(CMD_RELOAD, 0);
}

// ---------------------------------------------------------------- main window
static LRESULT CALLBACK Res_Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        Tray_Add();
        Bar_ReRegister();
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_SAMPLE) {
            double t0 = g_diagOn ? DiagNow() : 0;
            Res_UpdateExtras();
            Smp_Sample();
            double t1 = g_diagOn ? DiagNow() : 0;
            Bar_OnSample();
            for (int r = 0; r < 2; r++) if (Viewer_Alive(r)) PostMessageW(Viewer_Hwnd(r), WM_GS_SAMPLE, 0, 0);
            if (g_smp.tick % 2 == 0) Tray_Tip();
            if (g_diagOn) {
                g_dg[0] += t1 - t0; g_dg[2] += DiagNow() - t1;
                static FILETIME lastK, lastU; static double lastT = 0;
                if (g_smp.tick % 60 == 0) {
                    FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
                    auto ms = [](FILETIME a, FILETIME b) { ULARGE_INTEGER x, y; x.LowPart = a.dwLowDateTime; x.HighPart = a.dwHighDateTime; y.LowPart = b.dwLowDateTime; y.HighPart = b.dwHighDateTime; return (double)(x.QuadPart - y.QuadPart) / 1e4; };
                    double now = DiagNow(), el = lastT ? (now - lastT) / 1000.0 : 0;
                    wchar_t path[MAX_PATH]; wcscpy_s(path, g_iniPath);
                    wchar_t* dot = wcsrchr(path, L'.'); if (dot) wcscpy_s(dot, 16, L"-diag.txt");
                    FILE* f = nullptr;
                    if (lastT && _wfopen_s(&f, path, L"a") == 0 && f) {
                        fwprintf(f, L"%llu: per tick  sample %.3f  stripRender %.3f  stripBlt %.3f  rest-of-tick %.3f  fsTimer %.3f ms | paints %.0f | process cpu %.2f ms/s (kernel %.2f)\n",
                                 (unsigned long long)g_smp.tick, g_dg[0] / 60, g_dg[1] / 60, g_dg[5] / 60, g_dg[2] / 60, g_dg[4] / 60, g_dg[6],
                                 (ms(k, lastK) + ms(u, lastU)) / el, ms(k, lastK) / el);
                        fclose(f);
                    }
                    lastK = k; lastU = u; lastT = now;
                    for (double& d : g_dg) d = 0;
                }
            }
        } else if (wp == TIMER_FULLSCREEN) {
            double tf = g_diagOn ? DiagNow() : 0;
            Bar_CheckFullscreen();
            for (int r = 0; r < 2; r++) if (g_vp[r].proc && !Viewer_Alive(r)) Viewer_Launching(r);   // reaps a finished viewer
            if (g_sh->fastGpu && !Viewer_Alive(ROLE_SECTION)) g_sh->fastGpu = 0;
            if (g_sh->overlayHold && !Viewer_Alive(ROLE_OVERVIEW) && !Viewer_Launching(ROLE_OVERVIEW)) g_sh->overlayHold = 0;
            if (Ini_Changed()) Res_ReloadSettings();
            static int fsTicks = 0;
            if (g_bar && !g_barFsHidden && ++fsTicks % 30 == 0) {      // once a minute: follow the taskbar's tint
                Bar_SampleTaskbar();
                InvalidateRect(g_bar, nullptr, FALSE);
            }
            if (g_diagOn) g_dg[4] += DiagNow() - tf;
        } else if (wp == TIMER_RESAMPLE) {
            KillTimer(h, TIMER_RESAMPLE);
            if (g_bar && !g_barFsHidden) { Bar_SampleTaskbar(); InvalidateRect(g_bar, nullptr, FALSE); }
        } else if (wp == TIMER_TRIM) {
            KillTimer(h, TIMER_TRIM);
            TrimMemory();
        }
        return 0;
    case WM_HOTKEY:
        if (wp == 1) Res_OnHotkey();
        return 0;
    case WM_GS_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT: case NIN_KEYSELECT: Viewer_Send(CMD_OVERVIEW, 0); break;
        case WM_CONTEXTMENU: Res_ShowMenu(); break;
        }
        return 0;
    case WM_GS_VIEWER:
        if (wp == VEV_READY || wp == VEV_READY_OV) {
            int role = wp == VEV_READY_OV ? ROLE_OVERVIEW : ROLE_SECTION;
            std::vector<PendingCmd> pend;
            pend.swap(g_vp[role].pending);
            for (auto& p : pend) Viewer_Send(p.cmd, p.arg);
        } else if (wp == VEV_SECTION && lp >= 0 && lp < G_COUNT) {
            Viewer_Send(CMD_SECTION, (int)lp);
        } else if (wp == VEV_STRIP) {      // a section window chose another large chart: its strip slot follows
            if (g_bar) InvalidateRect(g_bar, nullptr, FALSE);
        }
        return 0;
    case WM_GS_CMD:     // a second launch of GlassStack.exe asks the running one to show itself
        Viewer_Send((int)wp, (int)lp);
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp((const wchar_t*)lp, L"ImmersiveColorSet")) {
            Bar_SampleTaskbar();
            if (g_bar) InvalidateRect(g_bar, nullptr, FALSE);
            SetTimer(h, TIMER_RESAMPLE, 1500, nullptr);     // the taskbar repaints its new tint a moment later
            Viewer_Send(CMD_RELOAD, 0);
        } else if (wp == SPI_SETDESKWALLPAPER && g_bar) SetTimer(h, TIMER_RESAMPLE, 1500, nullptr);
        else if (wp == SPI_SETWORKAREA && g_bar) Bar_Reposition();
        return 0;
    case WM_DISPLAYCHANGE:
        if (g_bar) { Bar_SampleTaskbar(); Bar_Reposition(); }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC) { Smp_FindNics(); if (g_bar) Bar_Reposition(); }
        return TRUE;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void Res_LoadIcons() {
    int sm = GetSystemMetrics(SM_CXSMICON), bg = GetSystemMetrics(SM_CXICON);
    g_iconSmall = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, sm, sm, LR_DEFAULTCOLOR);
    g_iconBig = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, bg, bg, LR_DEFAULTCOLOR);
    if (!g_iconSmall) g_iconSmall = LoadIconW(nullptr, IDI_APPLICATION);
    if (!g_iconBig) g_iconBig = g_iconSmall;
}

static int Resident_Main() {
    HANDLE mtx = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(CLS_RESIDENT, nullptr);
        if (other) {
            DWORD pid = 0; GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_GS_CMD, CMD_OVERVIEW, 0);
        }
        if (mtx) CloseHandle(mtx);
        return 0;
    }
    AllowDarkMenus();
    if (!Sh_Create()) { MessageBoxW(nullptr, L"Could not create the shared memory block.", APP_NAME, MB_ICONERROR); return 1; }
    g_diagOn = IniInt(L"Diag", 0) != 0;

    // first run: write a commented-free default ini and turn on "Start with Windows"
    wchar_t probe[16];
    IniStr(L"Hotkey", L"", probe, 16);
    if (!probe[0]) { Cfg_WriteDefaults(); Startup_Set(true); }

    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc); wc.hInstance = g_hInst;
    wc.lpfnWndProc = Res_Proc; wc.lpszClassName = CLS_RESIDENT;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = Bar_Proc; wc.lpszClassName = L"GlassStackBar"; wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassExW(&wc);
    wc.lpfnWndProc = Pop_Proc; wc.lpszClassName = L"GlassStackPop"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_hMain = CreateWindowExW(WS_EX_TOOLWINDOW, CLS_RESIDENT, APP_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_hInst, nullptr);
    if (!g_hMain) return 1;
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_hMain, g_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    g_sh->residentPid = GetCurrentProcessId();
    g_sh->residentHwnd = (LONGLONG)(LONG_PTR)g_hMain;

    Smp_Init();
    Res_LoadIcons();
    Tray_Add();
    Res_RegisterHotkey();
    if (g_cfg.showStrip) Bar_Create();
    SetTimer(g_hMain, TIMER_SAMPLE, (UINT)g_cfg.sampleMs, nullptr);
    SetTimer(g_hMain, TIMER_FULLSCREEN, 2000, nullptr);
    SetTimer(g_hMain, TIMER_TRIM, 15000, nullptr);
    Ini_Changed();                    // from here on, a save of GlassStack.ini is applied within 2 seconds

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    Viewer_Send(CMD_QUIT, 0);
    UnregisterHotKey(g_hMain, 1);
    Bar_Destroy();
    Tray_Remove();
    Smp_Shutdown();
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}

// ---------------------------------------------------------------- --selftest <file>: sampler values, costs and memory
static void ProcMem(DWORD pid, double& privWs, double& commit, ULONG& threads) {
    privWs = commit = 0; threads = 0;
    ULONG need = 0;
    std::vector<BYTE> buf(1 << 20);
    NTSTATUS st;
    while ((st = NtQuerySystemInformation(SystemProcessInformation, buf.data(), (ULONG)buf.size(), &need)) == (NTSTATUS)0xC0000004L)
        buf.resize((size_t)need + 65536);
    if (st < 0) return;
    for (BYTE* p = buf.data();;) {
        SYSTEM_PROCESS_INFORMATION* e = (SYSTEM_PROCESS_INFORMATION*)p;
        if ((DWORD)(ULONG_PTR)e->UniqueProcessId == pid) {
            // WorkingSetPrivateSize sits right after NextEntryOffset/NumberOfThreads (Task Manager's "Memory" column)
            privWs = (double)*(LONGLONG*)(p + 8);
            commit = (double)e->PrivatePageCount;
            threads = e->NumberOfThreads;
            return;
        }
        if (!e->NextEntryOffset) break;
        p += e->NextEntryOffset;
    }
}
static int SelfTest(const wchar_t* outPath) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, outPath, L"w, ccs=UTF-8") != 0 || !f) return 2;
    double pw, cm; ULONG th;
    ProcMem(GetCurrentProcessId(), pw, cm, th);
    fwprintf(f, L"start: private WS %.2f MB, commit %.2f MB, threads %lu\n", pw / 1048576, cm / 1048576, th);
    if (!Sh_Private()) { fwprintf(f, L"shared alloc failed\n"); fclose(f); return 3; }
    std::vector<std::wstring> names; g_nodeNames = &names;
    Smp_Init();
    g_nodeNames = nullptr;
    GSInfo& in = g_sh->info;
    fwprintf(f, L"CPU: %s | logical %d cores %d (P %d, E %d) hybrid %d\n", in.cpuName, in.logical, in.cores, in.pCores, in.eCores, in.hybrid);
    fwprintf(f, L"  P-core logical processors:");
    for (int i = 0; i < in.logical; i++) if (in.coreClass[i]) fwprintf(f, L" %d", i);
    fwprintf(f, L"\nRAM: %.2f GB\n", in.memTotal / GIB);
    fwprintf(f, L"GPU: %s | found %d nodes %d ded %.2f GB shared %.2f GB | engines 3D %d Copy %d VDec %d VEnc %d Compute %d\n",
             in.gpuName, in.gpuFound, in.gpuNodes, in.gpuDedTotal / GIB, in.gpuShTotal / GIB,
             in.engPresent[0], in.engPresent[1], in.engPresent[2], in.engPresent[3], in.engPresent[4]);
    for (auto& e : g_diskList) {
        wchar_t sz[32]; FmtBytes(sz, 32, e.size);
        fwprintf(f, L"Disk list: drive %d [%s] %s %s ssd=%d bus=%d\n", e.num, e.letters, e.model, sz, e.ssd, e.bus);
    }
    for (int k = 0; k < 2; k++) fwprintf(f, L"Slot %d: %s present=%d model=%s\n", k + 1, in.diskTitle[k], in.diskPresent[k], in.diskModel[k]);
    for (int i = 0; i < 4; i++) {
        Sleep(1000);
        Smp_Sample();
        GSSnap s; Sh_ReadSnap(s);
        fwprintf(f, L"\nsample %d (cost %.3f ms, of which GPU nodes %.3f ms)\n", i + 1, s.costMs, g_smp.costGpu);
        fwprintf(f, L"  CPU %.1f%% (%d cores:", s.cpu, s.nCores);
        for (int c = 0; c < s.nCores; c++) fwprintf(f, L" %.0f", s.core[c]);
        fwprintf(f, L")\n  RAM used %.2f GB, avail %.2f GB, commit %.2f / %.2f GB, cached %.2f GB, paged %.2f GB, nonpaged %.2f GB, procs %lu thr %lu handles %lu\n",
                 s.memUsed / GIB, s.memAvail / GIB, s.commit / GIB, s.commitLimit / GIB, s.cached / GIB, s.paged / GIB, s.nonPaged / GIB, s.procs, s.threads, s.handles);
        fwprintf(f, L"  GPU %.1f%% | 3D %.1f Copy %.1f VDec %.1f VEnc %.1f Comp %.1f | ded %.3f GB shared %.3f GB\n",
                 s.gpu, s.eng[0], s.eng[1], s.eng[2], s.eng[3], s.eng[4], s.gpuDed / GIB, s.gpuSh / GIB);
        for (int k = 0; k < 2; k++) {
            wchar_t r[32], w[32]; FmtRate(r, 32, s.dR[k], false); FmtRate(w, 32, s.dW[k], false);
            fwprintf(f, L"  %s: ok %d read %s write %s active %.1f%% resp %.2f ms\n", in.diskTitle[k], s.dOk[k], r, w, s.dAct[k], s.dResp[k]);
        }
        wchar_t rx[32], tx[32], ex[32], et[32];
        FmtRate(rx, 32, s.netRx, true); FmtRate(tx, 32, s.netTx, true); FmtRate(ex, 32, s.ethRx, true); FmtRate(et, 32, s.ethTx, true);
        fwprintf(f, L"  Wi-Fi [%s] found %d up %d link %.0f Mbps rx %s tx %s | Ethernet [%s] found %d up %d rx %s tx %s\n",
                 s.wifiName, s.wifiFound, s.wifiUp, s.wifiLink / 1e6, rx, tx, s.ethName, s.ethFound, s.ethUp, ex, et);
    }
    fwprintf(f, L"\nGPU nodes (class: 0 3D, 1 Copy, 2 VDec, 3 VEnc, 4 Compute, -1 other):\n");
    for (UINT n = 0; n < g_smp.nodes; n++) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        for (int r = 0; r < 20; r++) {
            D3DKMT_QUERYSTATISTICS qs = {};
            qs.Type = D3DKMT_QUERYSTATISTICS_NODE; qs.AdapterLuid = in.gpuLuid; qs.QueryNode.NodeId = n;
            D3DKMTQueryStatistics(&qs);
        }
        QueryPerformanceCounter(&b);
        double us = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)g_smp.freq.QuadPart / 20;
        fwprintf(f, L"  node %2u class %2d  %-40s %7.1f us/query  util %.1f%%\n", n, (int)g_smp.nodeClass[n],
                 n < names.size() ? names[n].c_str() : L"", us, g_smp.nodeUtil[n]);
    }
    ProcMem(GetCurrentProcessId(), pw, cm, th);
    fwprintf(f, L"\nafter sampling: private WS %.2f MB, commit %.2f MB, threads %lu\n", pw / 1048576, cm / 1048576, th);
    fclose(f);
    return 0;
}
