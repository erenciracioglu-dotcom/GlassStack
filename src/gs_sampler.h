// GlassStack - resident sampler. Only cheap kernel queries (measured on this PC, per second):
//   CPU     NtQuerySystemInformation per-core times        ~0.02 ms
//   memory  GlobalMemoryStatusEx (+ GetPerformanceInfo /5s) ~0.02 ms
//   GPU     D3DKMT node running times + segment usage       ~3 ms
//   disks   IOCTL_DISK_PERFORMANCE                          ~0.01 ms
//   network GetIfEntry2                                     ~0.1 ms
// No PDH, DXGI, WMI or graphics driver is ever loaded into this process.
#pragma once

struct DiskEntry { int num; wchar_t model[80]; wchar_t letters[32]; double size; int ssd; int bus; };
static std::vector<DiskEntry> g_diskList;

struct Sampler {
    LARGE_INTEGER freq = {};
    std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> cpuPrev, cpuCur;
    bool cpuPrimed = false;
    bool gpuOk = false; UINT nodes = 0, segs = 0;
    signed char nodeClass[64] = {};
    ULONGLONG nodePrev[64] = {};
    LONGLONG nodeQpc[64] = {};
    float nodeUtil[64] = {};
    bool gpuPrimed = false;
    double costGpu = 0;
    HANDLE disk[2] = { INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };
    DISK_PERFORMANCE dPrev[2] = {}; bool dPrimed[2] = {};
    NET_LUID wifiLuid = {}, ethLuid = {}; bool wifiFound = false, ethFound = false;
    wchar_t wifiName[96] = {}, ethName[96] = {};
    ULONG64 wIn = 0, wOut = 0, eIn = 0, eOut = 0; bool wPrimed = false, ePrimed = false;
    LONGLONG netQpc = 0;
    unsigned tick = 0;
    double cached = NAN, paged = NAN, nonPaged = NAN;
    DWORD procs = 0, threads = 0, handles = 0;
};
static Sampler g_smp;

static void TrimW(wchar_t* s) {
    size_t n = wcslen(s);
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == L' ' || s[i] == L'\t') i++;
    if (i) memmove(s, s + i, (wcslen(s + i) + 1) * sizeof(wchar_t));
}
static bool ContainsI(const wchar_t* hay, const wchar_t* needle) {
    wchar_t a[128], b[64];
    size_t i = 0;
    for (; hay[i] && i < 127; i++) a[i] = (wchar_t)towlower(hay[i]);
    a[i] = 0;
    for (i = 0; needle[i] && i < 63; i++) b[i] = (wchar_t)towlower(needle[i]);
    b[i] = 0;
    return wcsstr(a, b) != nullptr;
}

// ---------------------------------------------------------------- static information
static void Info_Cpu(GSInfo& in) {
    DWORD sz = sizeof(in.cpuName);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, in.cpuName, &sz) != ERROR_SUCCESS) wcscpy_s(in.cpuName, L"Processor");
    TrimW(in.cpuName);
    in.logical = (int)std::min<DWORD>(GS_MAXCORES, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<BYTE> b(len);
    if (len && GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)b.data(), &len)) {
        BYTE maxC = 0, minC = 255;
        for (DWORD off = 0; off < len;) {
            auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(b.data() + off);
            maxC = std::max(maxC, e->Processor.EfficiencyClass);
            minC = std::min(minC, e->Processor.EfficiencyClass);
            off += e->Size;
        }
        in.hybrid = maxC != minC;
        in.cores = in.pCores = in.eCores = 0;
        for (DWORD off = 0; off < len;) {
            auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(b.data() + off);
            bool perf = e->Processor.EfficiencyClass == maxC;
            in.cores++;
            if (in.hybrid) { if (perf) in.pCores++; else in.eCores++; }
            if (e->Processor.GroupCount >= 1 && e->Processor.GroupMask[0].Group == 0) {
                KAFFINITY mask = e->Processor.GroupMask[0].Mask;
                for (int bit = 0; bit < GS_MAXCORES; bit++) if (mask & ((KAFFINITY)1 << bit)) in.coreClass[bit] = perf ? 1 : 0;
            }
            off += e->Size;
        }
    }
    MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    in.memTotal = (double)ms.ullTotalPhys;
}

static std::vector<std::wstring>* g_nodeNames = nullptr;   // filled only by --selftest
// engine class: 0 3D, 1 Copy, 2 Video decode, 3 Video encode, 4 Compute, -1 other
static int EngineClass(int type, const wchar_t* name) {
    switch (type) {
    case 1: return 0;
    case 6: return 1;
    case 2: return 2;
    case 3: return 3;
    case 0: if (ContainsI(name, L"compute") || ContainsI(name, L"cuda")) return 4; return -1;
    default: return -1;
    }
}
static void Info_Gpu(GSInfo& in) {
    Sampler& m = g_smp;
    D3DKMT_ENUMADAPTERS2 ea = {};
    if (D3DKMTEnumAdapters2(&ea) != 0 || ea.NumAdapters == 0) return;
    std::vector<D3DKMT_ADAPTERINFO> ai(ea.NumAdapters);
    ea.pAdapters = ai.data();
    if (D3DKMTEnumAdapters2(&ea) != 0) return;
    int best = -1; double bestDed = -1;
    for (UINT i = 0; i < ea.NumAdapters; i++) {
        D3DKMT_ADAPTERREGISTRYINFO ri = {};
        D3DKMT_QUERYADAPTERINFO qa = {};
        qa.hAdapter = ai[i].hAdapter; qa.Type = KMTQAITYPE_ADAPTERREGISTRYINFO;
        qa.pPrivateDriverData = &ri; qa.PrivateDriverDataSize = sizeof(ri);
        if (D3DKMTQueryAdapterInfo(&qa) != 0 || !ri.AdapterString[0]) continue;   // skips the software renderer
        D3DKMT_SEGMENTSIZEINFO ss = {};
        qa.Type = KMTQAITYPE_GETSEGMENTSIZE; qa.pPrivateDriverData = &ss; qa.PrivateDriverDataSize = sizeof(ss);
        if (D3DKMTQueryAdapterInfo(&qa) != 0) continue;
        double ded = (double)ss.DedicatedVideoMemorySize;
        if (ded > bestDed) {
            bestDed = ded; best = (int)i;
            wcsncpy_s(in.gpuName, ri.AdapterString, _TRUNCATE);
            TrimW(in.gpuName);
            in.gpuLuid = ai[i].AdapterLuid;
            in.gpuDedTotal = ded;
            in.gpuShTotal = (double)ss.SharedSystemMemorySize;
        }
    }
    if (best >= 0) {
        D3DKMT_QUERYSTATISTICS qs = {};
        qs.Type = D3DKMT_QUERYSTATISTICS_ADAPTER; qs.AdapterLuid = in.gpuLuid;
        if (D3DKMTQueryStatistics(&qs) == 0) {
            m.nodes = std::min<UINT>(64, qs.QueryResult.AdapterInformation.NodeCount);
            m.segs = qs.QueryResult.AdapterInformation.NbSegments;
            in.gpuNodes = (int)m.nodes;
            for (UINT n = 0; n < m.nodes; n++) {
                D3DKMT_NODEMETADATA md = {};
                md.NodeOrdinalAndAdapterIndex = MAKELONG(n, 0);
                D3DKMT_QUERYADAPTERINFO qa = {};
                qa.hAdapter = ai[best].hAdapter; qa.Type = KMTQAITYPE_NODEMETADATA;
                qa.pPrivateDriverData = &md; qa.PrivateDriverDataSize = sizeof(md);
                int cls = -1;
                if (D3DKMTQueryAdapterInfo(&qa) == 0) {
                    cls = EngineClass((int)md.NodeData.EngineType, md.NodeData.FriendlyName);
                    if (g_nodeNames) { wchar_t t[96]; swprintf(t, 96, L"type %d '%s'", (int)md.NodeData.EngineType, md.NodeData.FriendlyName); g_nodeNames->push_back(t); }
                } else if (g_nodeNames) g_nodeNames->push_back(L"(no metadata)");
                m.nodeClass[n] = (signed char)cls;
                if (cls >= 0) in.engPresent[cls] = 1;
            }
            m.gpuOk = m.nodes > 0;
            in.gpuFound = 1;
        }
    }
    for (UINT i = 0; i < ea.NumAdapters; i++) { D3DKMT_CLOSEADAPTER ca = {}; ca.hAdapter = ai[i].hAdapter; D3DKMTCloseAdapter(&ca); }
}

static void Info_DiskList() {
    g_diskList.clear();
    wchar_t letters[16][32] = {};
    for (wchar_t L = L'A'; L <= L'Z'; L++) {
        wchar_t root[4] = { L, L':', L'\\', 0 };
        UINT t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        wchar_t path[8] = { L'\\', L'\\', L'.', L'\\', L, L':', 0 };
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        BYTE buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)] = {};
        DWORD br = 0;
        if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf, sizeof(buf), &br, nullptr)) {
            VOLUME_DISK_EXTENTS* x = (VOLUME_DISK_EXTENTS*)buf;
            for (DWORD i = 0; i < x->NumberOfDiskExtents && i < 9; i++) {
                DWORD d = x->Extents[i].DiskNumber;
                if (d >= 16) continue;
                wchar_t tag[4] = { L, L':', 0 };
                if (wcsstr(letters[d], tag)) continue;
                if (letters[d][0]) wcscat_s(letters[d], L" ");
                wcscat_s(letters[d], tag);
            }
        }
        CloseHandle(h);
    }
    for (int n = 0; n < 16; n++) {
        wchar_t path[32];
        swprintf(path, 32, L"\\\\.\\PhysicalDrive%d", n);
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DiskEntry e = {};
        e.num = n; e.ssd = -1;
        wcscpy_s(e.letters, letters[n]);
        STORAGE_PROPERTY_QUERY q = {};
        q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
        BYTE buf[2048] = {}; DWORD br = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &br, nullptr) && br >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            STORAGE_DEVICE_DESCRIPTOR* d = (STORAGE_DEVICE_DESCRIPTOR*)buf;
            char model[200] = "";
            if (d->VendorIdOffset && d->VendorIdOffset < br) { strncat_s(model, (const char*)buf + d->VendorIdOffset, _TRUNCATE); strcat_s(model, " "); }
            if (d->ProductIdOffset && d->ProductIdOffset < br) strncat_s(model, (const char*)buf + d->ProductIdOffset, _TRUNCATE);
            MultiByteToWideChar(CP_ACP, 0, model, -1, e.model, 80);
            e.model[79] = 0;
            TrimW(e.model);
            e.bus = (int)d->BusType;
        }
        q.PropertyId = StorageDeviceSeekPenaltyProperty;
        DEVICE_SEEK_PENALTY_DESCRIPTOR sp = {};
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &sp, sizeof(sp), &br, nullptr) && sp.Size >= sizeof(sp))
            e.ssd = sp.IncursSeekPenalty ? 0 : 1;
        ZeroMemory(buf, sizeof(buf));
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, buf, sizeof(buf), &br, nullptr))
            e.size = (double)((DISK_GEOMETRY_EX*)buf)->DiskSize.QuadPart;
        CloseHandle(h);
        g_diskList.push_back(e);
    }
}
static void Smp_OpenDisks() {
    Sampler& m = g_smp;
    GSInfo& in = g_sh->info;
    for (int k = 0; k < 2; k++) {
        if (m.disk[k] != INVALID_HANDLE_VALUE) { CloseHandle(m.disk[k]); m.disk[k] = INVALID_HANDLE_VALUE; }
        m.dPrimed[k] = false;
        in.diskPresent[k] = 0; in.diskNum[k] = g_cfg.disk[k];
        in.diskModel[k][0] = 0; in.diskLetters[k][0] = 0; in.diskSize[k] = 0; in.diskSsd[k] = -1; in.diskBus[k] = 0;
        for (auto& e : g_diskList) {
            if (e.num != g_cfg.disk[k]) continue;
            in.diskPresent[k] = 1;
            wcscpy_s(in.diskModel[k], e.model);
            wcscpy_s(in.diskLetters[k], e.letters);
            in.diskSize[k] = e.size; in.diskSsd[k] = e.ssd; in.diskBus[k] = e.bus;
        }
        if (in.diskLetters[k][0]) swprintf(in.diskTitle[k], 40, L"Disk %d (%s)", k + 1, in.diskLetters[k]);
        else swprintf(in.diskTitle[k], 40, L"Disk %d", k + 1);
        if (in.diskPresent[k]) {
            wchar_t path[32];
            swprintf(path, 32, L"\\\\.\\PhysicalDrive%d", g_cfg.disk[k]);
            m.disk[k] = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        }
    }
}

static void Smp_FindNics() {
    Sampler& m = g_smp;
    PMIB_IF_TABLE2 t = nullptr;
    if (GetIfTable2(&t) != NO_ERROR || !t) return;
    int bw = -1, be = -1; NET_LUID wl = {}, el = {}; wchar_t wn[96] = L"", en[96] = L"";
    for (ULONG i = 0; i < t->NumEntries; i++) {
        MIB_IF_ROW2& r = t->Table[i];
        if (!r.InterfaceAndOperStatusFlags.HardwareInterface || r.InterfaceAndOperStatusFlags.FilterInterface) continue;
        int score = r.OperStatus == IfOperStatusUp ? 3 : (r.OperStatus == IfOperStatusNotPresent ? 0 : 1);
        if (r.Type == IF_TYPE_IEEE80211) {
            if (score > bw) { bw = score; wl = r.InterfaceLuid; wcsncpy_s(wn, r.Description, _TRUNCATE); }
        } else if (r.Type == IF_TYPE_ETHERNET_CSMACD && r.PhysicalMediumType != NdisPhysicalMediumBluetooth &&
                   r.PhysicalMediumType != NdisPhysicalMediumWirelessWan) {
            if (score > be) { be = score; el = r.InterfaceLuid; wcsncpy_s(en, r.Description, _TRUNCATE); }
        }
    }
    FreeMibTable(t);
    if (bw >= 0) {
        if (!m.wifiFound || m.wifiLuid.Value != wl.Value) { m.wifiLuid = wl; m.wPrimed = false; }
        m.wifiFound = true; wcscpy_s(m.wifiName, wn);
    } else { m.wifiFound = false; m.wifiName[0] = 0; }
    if (be >= 0) {
        if (!m.ethFound || m.ethLuid.Value != el.Value) { m.ethLuid = el; m.ePrimed = false; }
        m.ethFound = true; wcscpy_s(m.ethName, en);
    } else { m.ethFound = false; m.ethName[0] = 0; }
}

// ---------------------------------------------------------------- extras: GPU temperature, latency (ping)
// and Wi-Fi signal. Collected only while a section window or a strip slot shows them, on a thread of their own so a
// slow ping or NVML's start never holds up the strip; the thread ends itself after half a minute without a use.
enum { EX_TEMP = 1, EX_PING = 2, EX_WSIG = 4 };
typedef DWORD(WINAPI* PFN_exWlanOpen)(DWORD, PVOID, PDWORD, PHANDLE);
typedef DWORD(WINAPI* PFN_exWlanClose)(HANDLE, PVOID);
typedef DWORD(WINAPI* PFN_exWlanEnum)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
typedef DWORD(WINAPI* PFN_exWlanQuery)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*, PWLAN_OPCODE_VALUE_TYPE);
typedef VOID(WINAPI* PFN_exWlanFree)(PVOID);
struct ExtraState {
    HANDLE thread = nullptr, wake = nullptr;
    std::atomic<int> want{ 0 };
    std::atomic<float> temp{ NAN }, power{ NAN }, ping{ NAN }, wsig{ NAN };
};
static ExtraState g_ex;
static DWORD WINAPI Ex_Thread(LPVOID) {
    D3DKMT_HANDLE kmt = 0;
    HMODULE wl = nullptr; HANDLE wlH = nullptr;
    PFN_exWlanClose wClose = nullptr; PFN_exWlanEnum wEnum = nullptr; PFN_exWlanQuery wQuery = nullptr; PFN_exWlanFree wFree = nullptr;
    HANDLE icmp = INVALID_HANDLE_VALUE; IN_ADDR addr = {}; bool wsa = false;
    int idle = 0;
    for (;;) {
        int w = g_ex.want.load();
        if (!w) { if (++idle > 30) break; } else idle = 0;
        ULONGLONG t0 = GetTickCount64();
        // temperature: the graphics kernel's adapter performance data (what Task Manager shows; no driver DLL
        // loaded here, so the resident stays small)
        if (w & EX_TEMP) {
            if (!kmt && g_sh->info.gpuFound) {
                D3DKMT_OPENADAPTERFROMLUID oa = {}; oa.AdapterLuid = g_sh->info.gpuLuid;
                if (D3DKMTOpenAdapterFromLuid(&oa) == 0) kmt = oa.hAdapter;
            }
            float tc = NAN;
            if (kmt) {
                D3DKMT_ADAPTER_PERFDATA pd = {};
                D3DKMT_QUERYADAPTERINFO qa = {}; qa.hAdapter = kmt; qa.Type = KMTQAITYPE_ADAPTERPERFDATA;
                qa.pPrivateDriverData = &pd; qa.PrivateDriverDataSize = sizeof(pd);
                if (D3DKMTQueryAdapterInfo(&qa) == 0 && pd.Temperature > 0) tc = pd.Temperature / 10.0f;
            }
            g_ex.temp = tc; g_ex.power = NAN;
        } else if (kmt) { D3DKMT_CLOSEADAPTER ca = {}; ca.hAdapter = kmt; D3DKMTCloseAdapter(&ca); kmt = 0; g_ex.temp = NAN; }
        // Wi-Fi signal quality
        if (w & EX_WSIG) {
            if (!wl) {
                wl = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                PFN_exWlanOpen open = wl ? (PFN_exWlanOpen)GetProcAddress(wl, "WlanOpenHandle") : nullptr;
                if (wl) {
                    wClose = (PFN_exWlanClose)GetProcAddress(wl, "WlanCloseHandle"); wEnum = (PFN_exWlanEnum)GetProcAddress(wl, "WlanEnumInterfaces");
                    wQuery = (PFN_exWlanQuery)GetProcAddress(wl, "WlanQueryInterface"); wFree = (PFN_exWlanFree)GetProcAddress(wl, "WlanFreeMemory");
                }
                DWORD ver = 0;
                if (!open || !wClose || !wEnum || !wQuery || !wFree || open(2, nullptr, &ver, &wlH) != ERROR_SUCCESS) { if (wl) FreeLibrary(wl); wl = nullptr; wlH = nullptr; }
            }
            float q = NAN;
            PWLAN_INTERFACE_INFO_LIST list = nullptr;
            if (wlH && wEnum(wlH, nullptr, &list) == ERROR_SUCCESS && list) {
                for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
                    if (list->InterfaceInfo[i].isState != wlan_interface_state_connected) continue;
                    DWORD sz = 0; PWLAN_CONNECTION_ATTRIBUTES ca = nullptr; WLAN_OPCODE_VALUE_TYPE vt;
                    if (wQuery(wlH, &list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &sz, (PVOID*)&ca, &vt) == ERROR_SUCCESS && ca) {
                        q = (float)ca->wlanAssociationAttributes.wlanSignalQuality; wFree(ca);
                    }
                    break;
                }
                wFree(list);
            }
            g_ex.wsig = q;
        } else if (wl) { if (wlH) wClose(wlH, nullptr); FreeLibrary(wl); wl = nullptr; wlH = nullptr; g_ex.wsig = NAN; }
        // latency (waits up to 900 ms for the reply)
        if (w & EX_PING) {
            if (icmp == INVALID_HANDLE_VALUE) {
                bool ok = InetPtonW(AF_INET, g_cfg.pingHost, &addr) == 1;
                if (!ok) {
                    WSADATA wd; if (!wsa) wsa = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
                    ADDRINFOW hints = {}; hints.ai_family = AF_INET; ADDRINFOW* res = nullptr;
                    if (wsa && GetAddrInfoW(g_cfg.pingHost, nullptr, &hints, &res) == 0 && res) { addr = ((sockaddr_in*)res->ai_addr)->sin_addr; ok = true; FreeAddrInfoW(res); }
                }
                if (ok) icmp = IcmpCreateFile();
            }
            float ms = NAN;
            if (icmp != INVALID_HANDLE_VALUE) {
                char data[32]; memset(data, 'g', sizeof(data));
                BYTE reply[sizeof(ICMP_ECHO_REPLY) + sizeof(data) + 64];
                if (IcmpSendEcho(icmp, addr.S_un.S_addr, data, sizeof(data), nullptr, reply, sizeof(reply), 900) > 0) {
                    ICMP_ECHO_REPLY* rep = (ICMP_ECHO_REPLY*)reply;
                    if (rep->Status == IP_SUCCESS) ms = (float)std::max(0.5, (double)rep->RoundTripTime);
                }
            }
            g_ex.ping = ms;
        } else if (icmp != INVALID_HANDLE_VALUE) { IcmpCloseHandle(icmp); icmp = INVALID_HANDLE_VALUE; g_ex.ping = NAN; }
        ULONGLONG el = GetTickCount64() - t0;
        WaitForSingleObject(g_ex.wake, el < 1000 ? (DWORD)(1000 - el) : 1);
    }
    if (kmt) { D3DKMT_CLOSEADAPTER ca = {}; ca.hAdapter = kmt; D3DKMTCloseAdapter(&ca); }
    if (wl) { if (wlH) wClose(wlH, nullptr); FreeLibrary(wl); }
    if (icmp != INVALID_HANDLE_VALUE) IcmpCloseHandle(icmp);
    if (wsa) WSACleanup();
    return 0;
}
// called once per tick by the resident with what is shown right now
static void Ex_SetWant(int w) {
    if (g_ex.thread && WaitForSingleObject(g_ex.thread, 0) == WAIT_OBJECT_0) { CloseHandle(g_ex.thread); g_ex.thread = nullptr; }
    int old = g_ex.want.exchange(w);
    if (w && !g_ex.thread) {
        if (!g_ex.wake) g_ex.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_ex.thread = CreateThread(nullptr, 128 * 1024, Ex_Thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    } else if (w & ~old) SetEvent(g_ex.wake);
}
static void Smp_Sample();
static void Smp_Init() {
    Sampler& m = g_smp;
    QueryPerformanceFrequency(&m.freq);
    GSInfo& in = g_sh->info;
    Info_Cpu(in);
    m.cpuPrev.resize(std::max(1, in.logical));
    m.cpuCur.resize(std::max(1, in.logical));
    Info_Gpu(in);
    Info_DiskList();
    Smp_OpenDisks();
    Smp_FindNics();
    Smp_Sample();       // primes every delta; the first real values arrive one tick later
}
static void Smp_Shutdown() {
    for (int k = 0; k < 2; k++) if (g_smp.disk[k] != INVALID_HANDLE_VALUE) { CloseHandle(g_smp.disk[k]); g_smp.disk[k] = INVALID_HANDLE_VALUE; }
}

static void Smp_Sample() {
    Sampler& m = g_smp;
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    GSSnap s;
    ZeroMemory(&s, sizeof(s));
    m.tick++;
    s.tick = m.tick;
    s.uptimeMs = GetTickCount64();

    // ---- CPU (busy time per logical processor)
    s.cpu = NAN;
    {
        ULONG ret = 0;
        ULONG bytes = (ULONG)(m.cpuCur.size() * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));
        NTSTATUS st = NtQuerySystemInformation(SystemProcessorPerformanceInformation, m.cpuCur.data(), bytes, &ret);
        int n = (int)(ret / sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));
        if (st >= 0 && n > 0) {
            if (m.cpuPrimed) {
                double sumIdle = 0, sumTot = 0;
                for (int i = 0; i < n && i < GS_MAXCORES; i++) {
                    const auto& c = m.cpuCur[i];
                    const auto& p = m.cpuPrev[i];
                    double idle = (double)(c.IdleTime.QuadPart - p.IdleTime.QuadPart);
                    double tot = (double)((c.KernelTime.QuadPart - p.KernelTime.QuadPart) + (c.UserTime.QuadPart - p.UserTime.QuadPart));
                    s.core[i] = tot > 0 ? (float)ClampD(100.0 * (1.0 - idle / tot), 0, 100) : 0.f;
                    sumIdle += idle; sumTot += tot;
                }
                s.nCores = std::min(n, GS_MAXCORES);
                if (sumTot > 0) s.cpu = (float)ClampD(100.0 * (1.0 - sumIdle / sumTot), 0, 100);
            }
            m.cpuPrev.swap(m.cpuCur);
            m.cpuPrimed = true;
        }
    }

    // ---- memory
    MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof(ms);
    s.memUsed = s.memAvail = s.commit = s.commitLimit = NAN;
    if (GlobalMemoryStatusEx(&ms)) {
        s.memUsed = (double)(ms.ullTotalPhys - ms.ullAvailPhys);
        s.memAvail = (double)ms.ullAvailPhys;
        s.commit = (double)(ms.ullTotalPageFile - ms.ullAvailPageFile);
        s.commitLimit = (double)ms.ullTotalPageFile;
    }
    if (m.tick % 5 == 1) {      // ~2.5 ms call, so only every 5 s
        PERFORMANCE_INFORMATION pi = {}; pi.cb = sizeof(pi);
        if (GetPerformanceInfo(&pi, sizeof(pi))) {
            double pg = (double)pi.PageSize;
            m.cached = pi.SystemCache * pg; m.paged = pi.KernelPaged * pg; m.nonPaged = pi.KernelNonpaged * pg;
            m.procs = pi.ProcessCount; m.threads = pi.ThreadCount; m.handles = pi.HandleCount;
        }
    }
    s.cached = m.cached; s.paged = m.paged; s.nonPaged = m.nonPaged;
    s.procs = m.procs; s.threads = m.threads; s.handles = m.handles;

    // ---- GPU (D3DKMT: the same scheduler statistics Task Manager reads)
    s.gpu = s.gpuDed = s.gpuSh = NAN;
    for (int k = 0; k < 5; k++) s.eng[k] = NAN;
    if (m.gpuOk) {
        LARGE_INTEGER g0; QueryPerformanceCounter(&g0);
        LUID luid = g_sh->info.gpuLuid;
        double mx = 0, cls[5] = {};
        bool fast = g_sh->fastGpu != 0;
        for (UINT n = 0; n < m.nodes; n++) {
            // Each node query costs ~0.22 ms (it reaches the driver), and a laptop RTX has 16 nodes. 3D and
            // compute engines carry the workload and are read every tick; video engines every 2nd, the six
            // copy engines every 4th and the rest (security, OFA, JPEG, VR) every 8th, staggered so each tick
            // costs about the same. A node's utilisation is measured over its own interval, so a slower node
            // reports a correct average. While the GPU section is open every classified engine is read each tick.
            int c0 = m.nodeClass[n];
            int period = (c0 == 0 || c0 == 4) ? 1 : (c0 == 2 || c0 == 3) ? 2 : c0 == 1 ? 4 : 8;
            if (fast && c0 >= 0) period = 1;
            bool due = !m.gpuPrimed || (m.tick + n) % (UINT)period == 0;
            if (due) {
                D3DKMT_QUERYSTATISTICS qs = {};
                qs.Type = D3DKMT_QUERYSTATISTICS_NODE; qs.AdapterLuid = luid; qs.QueryNode.NodeId = n;
                LARGE_INTEGER q; QueryPerformanceCounter(&q);
                if (D3DKMTQueryStatistics(&qs) == 0) {
                    ULONGLONG rt = (ULONGLONG)qs.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart;
                    double dt100 = m.nodeQpc[n] ? (double)(q.QuadPart - m.nodeQpc[n]) * 1e7 / (double)m.freq.QuadPart : 0;
                    if (dt100 > 0 && rt >= m.nodePrev[n]) m.nodeUtil[n] = (float)ClampD((double)(rt - m.nodePrev[n]) / dt100 * 100.0, 0, 100);
                    m.nodePrev[n] = rt;
                    m.nodeQpc[n] = q.QuadPart;
                }
            }
            double u = m.nodeUtil[n];
            if (u > mx) mx = u;
            int c = m.nodeClass[n];
            if (c >= 0 && u > cls[c]) cls[c] = u;
        }
        LARGE_INTEGER g1; QueryPerformanceCounter(&g1);
        m.costGpu = (double)(g1.QuadPart - g0.QuadPart) * 1000.0 / (double)m.freq.QuadPart;
        if (m.gpuPrimed) {
            s.gpu = mx;
            for (int k = 0; k < 5; k++) s.eng[k] = g_sh->info.engPresent[k] ? cls[k] : NAN;
        }
        m.gpuPrimed = true;
        double ded = 0, shr = 0; bool any = false;
        for (UINT sg = 0; sg < m.segs; sg++) {
            D3DKMT_QUERYSTATISTICS qs = {};
            qs.Type = D3DKMT_QUERYSTATISTICS_SEGMENT; qs.AdapterLuid = luid; qs.QuerySegment.SegmentId = sg;
            if (D3DKMTQueryStatistics(&qs) != 0) continue;
            const auto& si = qs.QueryResult.SegmentInformation;
            if (si.Aperture) shr += (double)si.BytesResident; else ded += (double)si.BytesResident;
            any = true;
        }
        if (any) { s.gpuDed = ded; s.gpuSh = shr; }
    }

    // ---- disks
    for (int k = 0; k < 2; k++) {
        s.dR[k] = s.dW[k] = s.dAct[k] = s.dResp[k] = NAN;
        if (m.disk[k] == INVALID_HANDLE_VALUE) continue;
        DISK_PERFORMANCE dp = {}; DWORD br = 0;
        if (!DeviceIoControl(m.disk[k], IOCTL_DISK_PERFORMANCE, nullptr, 0, &dp, sizeof(dp), &br, nullptr)) continue;
        s.dOk[k] = 1;
        if (m.dPrimed[k]) {
            const DISK_PERFORMANCE& p = m.dPrev[k];
            double dq = (double)(dp.QueryTime.QuadPart - p.QueryTime.QuadPart);
            if (dq > 0) {
                double sec = dq / 1e7;
                s.dR[k] = std::max(0.0, (double)(dp.BytesRead.QuadPart - p.BytesRead.QuadPart)) / sec;
                s.dW[k] = std::max(0.0, (double)(dp.BytesWritten.QuadPart - p.BytesWritten.QuadPart)) / sec;
                double idle = (double)(dp.IdleTime.QuadPart - p.IdleTime.QuadPart);
                s.dAct[k] = ClampD(100.0 - idle / dq * 100.0, 0, 100);
                double cnt = (double)(DWORD)(dp.ReadCount - p.ReadCount) + (double)(DWORD)(dp.WriteCount - p.WriteCount);
                double tm = (double)(dp.ReadTime.QuadPart - p.ReadTime.QuadPart) + (double)(dp.WriteTime.QuadPart - p.WriteTime.QuadPart);
                s.dResp[k] = cnt > 0 ? std::max(0.0, tm / cnt / 1e4) : 0.0;
            }
        }
        m.dPrev[k] = dp; m.dPrimed[k] = true;
    }

    // ---- network
    if (m.tick % (m.wifiFound ? 30u : 10u) == 1) Smp_FindNics();    // GetIfTable2 is not free: a PC without Wi-Fi retries every 10 s
    LARGE_INTEGER nq; QueryPerformanceCounter(&nq);
    double ndt = m.netQpc ? (double)(nq.QuadPart - m.netQpc) / (double)m.freq.QuadPart : 0;
    m.netQpc = nq.QuadPart;
    s.netRx = s.netTx = s.ethRx = s.ethTx = NAN;
    if (m.wifiFound) {
        MIB_IF_ROW2 r = {}; r.InterfaceLuid = m.wifiLuid;
        if (GetIfEntry2(&r) == NO_ERROR) {
            s.wifiFound = 1;
            s.wifiUp = r.OperStatus == IfOperStatusUp;
            s.wifiLink = r.ReceiveLinkSpeed; s.wifiTotRx = r.InOctets; s.wifiTotTx = r.OutOctets;
            if (m.wPrimed && ndt > 0) {
                s.netRx = (r.InOctets >= m.wIn ? (double)(r.InOctets - m.wIn) : 0) / ndt;
                s.netTx = (r.OutOctets >= m.wOut ? (double)(r.OutOctets - m.wOut) : 0) / ndt;
            }
            m.wIn = r.InOctets; m.wOut = r.OutOctets; m.wPrimed = true;
        } else m.wifiFound = false;
    }
    if (m.ethFound) {
        MIB_IF_ROW2 r = {}; r.InterfaceLuid = m.ethLuid;
        if (GetIfEntry2(&r) == NO_ERROR) {
            s.ethFound = 1;
            s.ethUp = r.OperStatus == IfOperStatusUp;
            s.ethLink = r.ReceiveLinkSpeed; s.ethTotRx = r.InOctets; s.ethTotTx = r.OutOctets;
            if (m.ePrimed && ndt > 0) {
                s.ethRx = (r.InOctets >= m.eIn ? (double)(r.InOctets - m.eIn) : 0) / ndt;
                s.ethTx = (r.OutOctets >= m.eOut ? (double)(r.OutOctets - m.eOut) : 0) / ndt;
            }
            m.eIn = r.InOctets; m.eOut = r.OutOctets; m.ePrimed = true;
        } else m.ethFound = false;
    }
    wcscpy_s(s.wifiName, m.wifiName);
    wcscpy_s(s.ethName, m.ethName);
    s.wifiLuid = m.wifiFound ? m.wifiLuid.Value : 0;
    s.ethLuid = m.ethFound ? m.ethLuid.Value : 0;

    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    s.costMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)m.freq.QuadPart;

    // ---- publish
    Sh_BeginWrite();
    memcpy((void*)&g_sh->snap, &s, sizeof(s));
    auto P = [](int id, double v) { g_sh->hist[id].push((float)v); };
    P(S_CPU, s.cpu);
    P(S_MEM, s.memUsed / GIB); P(S_MEM_COMMIT, s.commit / GIB); P(S_MEM_CACHE, s.cached / GIB);
    P(S_GPU, s.gpu); P(S_GPU_DED, s.gpuDed / GIB); P(S_GPU_SH, s.gpuSh / GIB);
    for (int k = 0; k < 5; k++) P(S_GPU_3D + k, s.eng[k]);
    P(S_DK1_R, s.dR[0]); P(S_DK1_W, s.dW[0]); P(S_DK1_ACT, s.dAct[0]); P(S_DK1_RESP, s.dResp[0]);
    P(S_DK2_R, s.dR[1]); P(S_DK2_W, s.dW[1]); P(S_DK2_ACT, s.dAct[1]); P(S_DK2_RESP, s.dResp[1]);
    P(S_NET_RX, s.netRx); P(S_NET_TX, s.netTx); P(S_ETH_RX, s.ethRx); P(S_ETH_TX, s.ethTx);
    // the extras: measured here when the strip shows them, else taken from an open section window
    int xw = g_ex.want.load();
    bool vf = (LONG)s.tick - g_sh->exTick <= 3 && g_sh->exTick > 0;
    P(S_PING, (xw & EX_PING) ? g_ex.ping.load() : vf ? g_sh->exPing : NAN);
    P(S_WSIG, (xw & EX_WSIG) ? g_ex.wsig.load() : vf ? g_sh->exSig : NAN);
    P(S_GPU_TEMP, (xw & EX_TEMP) ? g_ex.temp.load() : vf ? g_sh->exTemp : NAN);
    P(S_GPU_POWER, (xw & EX_TEMP) ? g_ex.power.load() : vf ? g_sh->exPower : NAN);
    if (s.nCores > 0) {
        LONG h = g_sh->coreHead;
        for (int c = 0; c < s.nCores; c++) g_sh->coreHist[c][h] = s.core[c];
        g_sh->coreHead = (h + 1) % GS_COREHIST;
        if (g_sh->coreCount < GS_COREHIST) g_sh->coreCount = g_sh->coreCount + 1;
    }
    Sh_EndWrite();
}
