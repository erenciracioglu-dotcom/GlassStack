// GlassStack - lightweight system monitor for Windows 11.
//   GlassStack.exe                         resident: sampler + taskbar strip + tray + Ctrl+Shift+Space
//   GlassStack.exe --viewer <cmd> <arg>    viewer (started by the resident): 3D overview and section windows
//   GlassStack.exe --quit                  closes the running copy (strip, tray icon, viewer)
//   GlassStack.exe --selftest <file>       prints what the sampler sees on this PC
//   GlassStack.exe --render-test <dir>     renders every view to PNG files with synthetic data
//   GlassStack.exe --bench <file> <mt> [hw] [parts]   memory and CPU of the windows, drawn off screen (needs the resident)
// The whole program is one translation unit; each part lives in its own header.
#include "gs_common.h"
#include "gs_shared.h"
#include "gs_sampler.h"
#include "gs_bar.h"
#include "gs_resident.h"
#if __has_include("gs_viewer.h")
#include "gs_d2d.h"
#include "gs_detail.h"
#include "gs_overlay.h"
#include "gs_section.h"
#include "gs_viewer.h"
#else
static int Viewer_Main(int, int) { return 0; }
static int RenderTest(const wchar_t*) { return 0; }
static int Bench(const wchar_t*, bool, bool, int) { return 0; }
#endif

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    g_hInst = hi;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Cfg_Paths();
    Cfg_Load();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc;
    if (argc >= 2 && !wcscmp(argv[1], L"--viewer"))
        rc = Viewer_Main(argc >= 3 ? _wtoi(argv[2]) : CMD_OVERVIEW, argc >= 4 ? _wtoi(argv[3]) : 0);
    else if (argc >= 2 && !wcscmp(argv[1], L"--quit")) {
        HWND other = FindWindowW(CLS_RESIDENT, nullptr);
        DWORD pid = 0;
        if (other) GetWindowThreadProcessId(other, &pid);
        HANDLE p = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
        if (other) PostMessageW(other, WM_CLOSE, 0, 0);
        if (p) { WaitForSingleObject(p, 5000); CloseHandle(p); }
        rc = other ? 0 : 1;
    }
    else if (argc >= 3 && !wcscmp(argv[1], L"--selftest")) rc = SelfTest(argv[2]);
    else if (argc >= 3 && !wcscmp(argv[1], L"--render-test")) rc = RenderTest(argv[2]);
    else if (argc >= 4 && !wcscmp(argv[1], L"--bench")) rc = Bench(argv[2], _wtoi(argv[3]) != 0, argc >= 5 && _wtoi(argv[4]) != 0, argc >= 6 ? _wtoi(argv[5]) : 3);
    else rc = Resident_Main();
    if (argv) LocalFree(argv);
    return rc;
}
