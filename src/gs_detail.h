// GlassStack - viewer-side detail collector. Runs on its own thread and only gathers what the open
// section needs: the process list (all sections but Wi-Fi), per-process GPU use + NVML (GPU section),
// Wi-Fi link details + latency ping (Wi-Fi section). Everything is released when not needed.
#pragma once

static HWND g_vCtl = nullptr;     // viewer control window (defined in gs_viewer.h)

struct GS_SPI {   // SYSTEM_PROCESS_INFORMATION, full x64 layout
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
};
static_assert(sizeof(void*) != 8 || offsetof(GS_SPI, ReadTransferCount) == 232, "SYSTEM_PROCESS_INFORMATION layout");

struct ProcRow {
    std::wstring name;
    int count = 0;
    double mem = 0, cpu = 0, io = 0, gpu = 0, gpuMem = 0;
    DWORD handles = 0;
    int conns = 0;                    // established TCP connections to other machines (Wi-Fi section)
    double bigMem = -1; DWORD bigPid = 0;
    std::vector<DWORD> pids;
};
struct DetailData {
    std::vector<ProcRow> procs;
    bool procsValid = false;
    double compressed = NAN;
    std::wstring leakName; DWORD leakHandles = 0;
    double gpuTemp = NAN, gpuPower = NAN;
    double ping = NAN, wsig = NAN, wRx = NAN, wTx = NAN;
    int wPhy = -1, wChannel = 0;
    wchar_t ssid[40] = {}, ipv4[48] = {};
};
enum { DET_PROCS = 1, DET_GPU = 2, DET_NET = 4 };
static std::atomic<int> g_detWant{ 0 };
static SRWLOCK g_detLock = SRWLOCK_INIT;
static DetailData g_detShared;      // written by the detail thread
static DetailData g_det;            // UI copy
static HANDLE g_detThread = nullptr, g_detKick = nullptr, g_detStop = nullptr;

typedef int (*PFN_nvmlVoid)();
typedef int (*PFN_nvmlHandleByIndex)(unsigned int, void**);
typedef int (*PFN_nvmlTemp)(void*, int, unsigned int*);
typedef int (*PFN_nvmlPower)(void*, unsigned int*);
typedef DWORD(WINAPI* PFN_WlanOpenHandle)(DWORD, PVOID, PDWORD, PHANDLE);
typedef DWORD(WINAPI* PFN_WlanCloseHandle)(HANDLE, PVOID);
typedef DWORD(WINAPI* PFN_WlanEnumInterfaces)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
typedef DWORD(WINAPI* PFN_WlanQueryInterface)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*, PWLAN_OPCODE_VALUE_TYPE);
typedef VOID(WINAPI* PFN_WlanFreeMemory)(PVOID);

struct PdhWild {
    PDH_HCOUNTER h = nullptr;
    std::vector<BYTE> buf;
    DWORD n = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W* it() { return (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data(); }
    bool fetch() {
        n = 0;
        if (!h) return false;
        DWORD size = (DWORD)buf.size(), cnt = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(h, PDH_FMT_DOUBLE, &size, &cnt, buf.empty() ? nullptr : it());
        if (st == PDH_MORE_DATA) {
            buf.resize((size_t)size + 16384);
            size = (DWORD)buf.size();
            st = PdhGetFormattedCounterArrayW(h, PDH_FMT_DOUBLE, &size, &cnt, it());
        }
        if (st != ERROR_SUCCESS) return false;
        n = cnt;
        return true;
    }
};

struct DetState {
    LARGE_INTEGER freq = {};
    // processes
    std::vector<BYTE> pbuf;
    struct Prev { ULONGLONG cpu, io; LONGLONG create; };
    std::unordered_map<DWORD, Prev> prev;
    LONGLONG procQpc = 0;
    // GPU per process (PDH, only while the GPU section is open)
    PDH_HQUERY q = nullptr; PdhWild eng, mem; wchar_t luidKey[64] = {};
    std::unordered_map<DWORD, double> pidGpu, pidGpuMem;
    // NVML
    HMODULE nvml = nullptr; void* nvDev = nullptr; bool nvFailed = false;
    PFN_nvmlVoid nvShutdown = nullptr; PFN_nvmlTemp nvTemp = nullptr; PFN_nvmlPower nvPower = nullptr;
    // WLAN
    HMODULE wlan = nullptr; HANDLE wlanH = nullptr;
    PFN_WlanCloseHandle wClose = nullptr; PFN_WlanEnumInterfaces wEnum = nullptr; PFN_WlanQueryInterface wQuery = nullptr; PFN_WlanFreeMemory wFree = nullptr;
    unsigned tick = 0;
    wchar_t ipv4[48] = {};
    std::vector<BYTE> tcpBuf;
    // ping
    HANDLE pingThread = nullptr, pingEvt = nullptr;
    std::atomic<bool> pingStop{ false };
    std::atomic<float> pingMs{ NAN };
};
static DetState g_ds;

static void Det_Lower(const wchar_t* s, wchar_t* out, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) out[i] = (wchar_t)towlower(s[i]);
    out[i] = 0;
}

// ---------------------------------------------------------------- processes
static void Det_Procs(DetailData& d, bool gpu) {
    DetState& m = g_ds;
    if (m.pbuf.size() < 768 * 1024) m.pbuf.resize(768 * 1024);
    NTSTATUS st = 0; ULONG need = 0;
    for (int a = 0; a < 5; a++) {
        st = NtQuerySystemInformation(SystemProcessInformation, m.pbuf.data(), (ULONG)m.pbuf.size(), &need);
        if (st == (NTSTATUS)0xC0000004L) { m.pbuf.resize((size_t)need + 256 * 1024); continue; }
        break;
    }
    if (st < 0) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = m.procQpc ? (double)(now.QuadPart - m.procQpc) / (double)m.freq.QuadPart : 0;
    bool havePrev = m.procQpc != 0 && dt > 0.2;
    m.procQpc = now.QuadPart;
    double ncpu = g_sh->info.logical > 0 ? g_sh->info.logical : 1;
    std::unordered_map<std::wstring, size_t> idx; idx.reserve(512);
    std::unordered_map<DWORD, DetState::Prev> next; next.reserve(m.prev.size() + 64);
    d.procs.reserve(320);
    DWORD maxH = 0; std::wstring maxHName;
    for (BYTE* p = m.pbuf.data();;) {
        GS_SPI* e = (GS_SPI*)p;
        DWORD pid = (DWORD)(ULONG_PTR)e->UniqueProcessId;
        ULONGLONG cpu = (ULONGLONG)e->KernelTime.QuadPart + (ULONGLONG)e->UserTime.QuadPart;
        ULONGLONG io = (ULONGLONG)e->ReadTransferCount.QuadPart + (ULONGLONG)e->WriteTransferCount.QuadPart;
        double cpuPct = 0, ioRate = 0;
        if (havePrev) {
            auto it = m.prev.find(pid);
            if (it != m.prev.end() && it->second.create == e->CreateTime.QuadPart) {
                if (cpu >= it->second.cpu) cpuPct = (double)(cpu - it->second.cpu) / (dt * 1e7 * ncpu) * 100.0;
                if (io >= it->second.io) ioRate = (double)(io - it->second.io) / dt;
            }
        }
        DetState::Prev pv = { cpu, io, e->CreateTime.QuadPart };
        next[pid] = pv;
        if (pid != 0) {
            std::wstring name = (e->ImageName.Buffer && e->ImageName.Length)
                ? std::wstring(e->ImageName.Buffer, e->ImageName.Length / sizeof(wchar_t))
                : std::wstring(pid == 4 ? L"System" : L"(unknown)");
            if (e->HandleCount > maxH) { maxH = e->HandleCount; maxHName = name; }
            if (name == L"Memory Compression") d.compressed = (double)e->WorkingSetSize;
            size_t k;
            auto f = idx.find(name);
            if (f == idx.end()) { k = d.procs.size(); idx.emplace(name, k); d.procs.emplace_back(); d.procs[k].name = name; }
            else k = f->second;
            ProcRow& r = d.procs[k];
            double mem = (double)e->WorkingSetPrivateSize.QuadPart;
            r.count++; r.mem += mem; r.cpu += cpuPct; r.io += ioRate; r.handles += e->HandleCount;
            r.pids.push_back(pid);
            if (mem > r.bigMem) { r.bigMem = mem; r.bigPid = pid; }
            if (gpu) {
                auto g1 = m.pidGpu.find(pid); if (g1 != m.pidGpu.end() && g1->second > r.gpu) r.gpu = g1->second;
                auto g2 = m.pidGpuMem.find(pid); if (g2 != m.pidGpuMem.end()) r.gpuMem += g2->second;
            }
        }
        if (!e->NextEntryOffset) break;
        p += e->NextEntryOffset;
    }
    m.prev.swap(next);
    d.procsValid = havePrev;
    if (maxH > 100000) { d.leakName = maxHName; d.leakHandles = maxH; }
}
static void Det_ProcsRelease() {
    DetState& m = g_ds;
    if (!m.prev.empty()) std::unordered_map<DWORD, DetState::Prev>().swap(m.prev);
    if (!m.pbuf.empty()) std::vector<BYTE>().swap(m.pbuf);
    m.procQpc = 0;
}

// ---------------------------------------------------------------- GPU per process (PDH) + NVML
static void Det_GpuRelease() {
    DetState& m = g_ds;
    if (m.q) { PdhCloseQuery(m.q); m.q = nullptr; }
    m.eng = PdhWild(); m.mem = PdhWild();
    std::unordered_map<DWORD, double>().swap(m.pidGpu);
    std::unordered_map<DWORD, double>().swap(m.pidGpuMem);
    if (m.nvml) { if (m.nvShutdown) m.nvShutdown(); FreeLibrary(m.nvml); }
    m.nvml = nullptr; m.nvDev = nullptr; m.nvFailed = false;
}
static void Det_Gpu(DetailData& d) {
    DetState& m = g_ds;
    if (!g_sh->info.gpuFound) return;
    if (!m.q) {
        if (PdhOpenQueryW(nullptr, 0, &m.q) != ERROR_SUCCESS) { m.q = nullptr; return; }
        PdhAddEnglishCounterW(m.q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &m.eng.h);
        PdhAddEnglishCounterW(m.q, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &m.mem.h);
        swprintf(m.luidKey, 64, L"luid_0x%08x_0x%08x", (DWORD)g_sh->info.gpuLuid.HighPart, g_sh->info.gpuLuid.LowPart);
    }
    PdhCollectQueryData(m.q);
    m.pidGpu.clear(); m.pidGpuMem.clear();
    wchar_t low[256];
    if (m.eng.fetch()) {
        for (DWORD i = 0; i < m.eng.n; i++) {
            const PDH_FMT_COUNTERVALUE_ITEM_W& x = m.eng.it()[i];
            if (x.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && x.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            double v = x.FmtValue.doubleValue;
            if (v <= 0) continue;
            Det_Lower(x.szName, low, 256);
            if (wcsncmp(low, L"pid_", 4) || !wcsstr(low, m.luidKey)) continue;
            double& g = m.pidGpu[(DWORD)_wtoi(low + 4)];
            if (v > g) g = v;
        }
    }
    if (m.mem.fetch()) {
        for (DWORD i = 0; i < m.mem.n; i++) {
            const PDH_FMT_COUNTERVALUE_ITEM_W& x = m.mem.it()[i];
            if (x.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && x.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            Det_Lower(x.szName, low, 256);
            if (wcsncmp(low, L"pid_", 4) || !wcsstr(low, m.luidKey)) continue;
            m.pidGpuMem[(DWORD)_wtoi(low + 4)] += x.FmtValue.doubleValue;
        }
    }
    // NVML for temperature / power (NVIDIA only; loaded only while the GPU view is open)
    if (g_cfg.useNvml && !m.nvFailed && ContainsI(g_sh->info.gpuName, L"nvidia")) {
        if (!m.nvml) {
            m.nvml = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!m.nvml) {
                wchar_t p[MAX_PATH];
                ExpandEnvironmentStringsW(L"%ProgramFiles%\\NVIDIA Corporation\\NVSMI\\nvml.dll", p, MAX_PATH);
                m.nvml = LoadLibraryW(p);
            }
            PFN_nvmlVoid init = m.nvml ? (PFN_nvmlVoid)GetProcAddress(m.nvml, "nvmlInit_v2") : nullptr;
            PFN_nvmlHandleByIndex byIdx = m.nvml ? (PFN_nvmlHandleByIndex)GetProcAddress(m.nvml, "nvmlDeviceGetHandleByIndex_v2") : nullptr;
            if (m.nvml) {
                m.nvShutdown = (PFN_nvmlVoid)GetProcAddress(m.nvml, "nvmlShutdown");
                m.nvTemp = (PFN_nvmlTemp)GetProcAddress(m.nvml, "nvmlDeviceGetTemperature");
                m.nvPower = (PFN_nvmlPower)GetProcAddress(m.nvml, "nvmlDeviceGetPowerUsage");
            }
            if (!init || !byIdx || init() != 0 || byIdx(0, &m.nvDev) != 0) {
                if (m.nvml) FreeLibrary(m.nvml);
                m.nvml = nullptr; m.nvFailed = true;
            }
        }
        if (m.nvml) {
            unsigned int t = 0, pw = 0;
            if (m.nvTemp && m.nvTemp(m.nvDev, 0, &t) == 0) d.gpuTemp = t;
            if (m.nvPower && m.nvPower(m.nvDev, &pw) == 0) d.gpuPower = pw / 1000.0;
        }
    }
}

// ---------------------------------------------------------------- Wi-Fi link, IPv4 and latency
static DWORD WINAPI Det_PingThread(LPVOID) {
    DetState& m = g_ds;
    WSADATA wd; bool wsa = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    IN_ADDR addr = {}; bool ok = InetPtonW(AF_INET, g_cfg.pingHost, &addr) == 1;
    if (!ok && wsa) {
        ADDRINFOW hints = {}; hints.ai_family = AF_INET; ADDRINFOW* res = nullptr;
        if (GetAddrInfoW(g_cfg.pingHost, nullptr, &hints, &res) == 0 && res) { addr = ((sockaddr_in*)res->ai_addr)->sin_addr; ok = true; FreeAddrInfoW(res); }
    }
    HANDLE h = ok ? IcmpCreateFile() : INVALID_HANDLE_VALUE;
    char data[32]; memset(data, 'g', sizeof(data));
    BYTE reply[sizeof(ICMP_ECHO_REPLY) + sizeof(data) + 64];
    while (!m.pingStop.load()) {
        ULONGLONG t0 = GetTickCount64();
        float ms = NAN;
        if (h != INVALID_HANDLE_VALUE) {
            DWORD r = IcmpSendEcho(h, addr.S_un.S_addr, data, sizeof(data), nullptr, reply, sizeof(reply), 1000);
            if (r > 0) { ICMP_ECHO_REPLY* rep = (ICMP_ECHO_REPLY*)reply; if (rep->Status == IP_SUCCESS) ms = (float)std::max(0.5, (double)rep->RoundTripTime); }
        }
        m.pingMs = ms;
        ULONGLONG el = GetTickCount64() - t0;
        if (el < 1000) WaitForSingleObject(m.pingEvt, (DWORD)(1000 - el));
    }
    if (h != INVALID_HANDLE_VALUE) IcmpCloseHandle(h);
    if (wsa) WSACleanup();
    return 0;
}
static void Det_NetRelease() {
    DetState& m = g_ds;
    if (m.pingThread) {
        m.pingStop = true; SetEvent(m.pingEvt);
        WaitForSingleObject(m.pingThread, 3000);
        CloseHandle(m.pingThread); m.pingThread = nullptr;
    }
    m.pingMs = NAN;
    if (m.wlanH && m.wClose) m.wClose(m.wlanH, nullptr);
    if (m.wlan) FreeLibrary(m.wlan);
    m.wlan = nullptr; m.wlanH = nullptr; m.ipv4[0] = 0;
    if (!m.tcpBuf.empty()) std::vector<BYTE>().swap(m.tcpBuf);
}
// established TCP connections per process, loopback (local IPC) excluded; no admin rights needed
static void Det_Conns(DetailData& d) {
    DetState& m = g_ds;
    std::unordered_map<DWORD, int> per;
    for (int fam = 0; fam < 2; fam++) {
        ULONG af = fam ? AF_INET6 : AF_INET;
        DWORD size = (DWORD)m.tcpBuf.size();
        DWORD r = m.tcpBuf.empty() ? ERROR_INSUFFICIENT_BUFFER : GetExtendedTcpTable(m.tcpBuf.data(), &size, FALSE, af, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        if (r == ERROR_INSUFFICIENT_BUFFER) {
            if (m.tcpBuf.empty()) { size = 0; GetExtendedTcpTable(nullptr, &size, FALSE, af, TCP_TABLE_OWNER_PID_CONNECTIONS, 0); }
            m.tcpBuf.resize((size_t)size + 16384);
            size = (DWORD)m.tcpBuf.size();
            r = GetExtendedTcpTable(m.tcpBuf.data(), &size, FALSE, af, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        }
        if (r != NO_ERROR) continue;
        if (!fam) {
            const MIB_TCPTABLE_OWNER_PID* t = (const MIB_TCPTABLE_OWNER_PID*)m.tcpBuf.data();
            for (DWORD i = 0; i < t->dwNumEntries; i++) {
                const MIB_TCPROW_OWNER_PID& e = t->table[i];
                if (e.dwState != MIB_TCP_STATE_ESTAB || (e.dwRemoteAddr & 0xFF) == 127) continue;
                per[e.dwOwningPid]++;
            }
        } else {
            const MIB_TCP6TABLE_OWNER_PID* t = (const MIB_TCP6TABLE_OWNER_PID*)m.tcpBuf.data();
            static const UCHAR lo6[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
            for (DWORD i = 0; i < t->dwNumEntries; i++) {
                const MIB_TCP6ROW_OWNER_PID& e = t->table[i];
                if (e.dwState != MIB_TCP_STATE_ESTAB || !memcmp(e.ucRemoteAddr, lo6, 16)) continue;
                const UCHAR* a = e.ucRemoteAddr;   // IPv4-mapped loopback ::ffff:127.x.x.x
                bool mapped = !memcmp(a, lo6, 10) && a[10] == 0xFF && a[11] == 0xFF;
                if (mapped && a[12] == 127) continue;
                per[e.dwOwningPid]++;
            }
        }
    }
    for (auto& r : d.procs) {
        int s = 0;
        for (DWORD pid : r.pids) { auto f = per.find(pid); if (f != per.end()) s += f->second; }
        r.conns = s;
    }
}
static void Det_Net(DetailData& d) {
    DetState& m = g_ds;
    if (!m.pingThread) {
        m.pingStop = false; m.pingMs = NAN;
        if (!m.pingEvt) m.pingEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m.pingThread = CreateThread(nullptr, 64 * 1024, Det_PingThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    }
    d.ping = m.pingMs.load();
    if (!m.wlan) {
        m.wlan = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m.wlan) {
            PFN_WlanOpenHandle open = (PFN_WlanOpenHandle)GetProcAddress(m.wlan, "WlanOpenHandle");
            m.wClose = (PFN_WlanCloseHandle)GetProcAddress(m.wlan, "WlanCloseHandle");
            m.wEnum = (PFN_WlanEnumInterfaces)GetProcAddress(m.wlan, "WlanEnumInterfaces");
            m.wQuery = (PFN_WlanQueryInterface)GetProcAddress(m.wlan, "WlanQueryInterface");
            m.wFree = (PFN_WlanFreeMemory)GetProcAddress(m.wlan, "WlanFreeMemory");
            DWORD ver = 0;
            if (!open || !m.wClose || !m.wEnum || !m.wQuery || !m.wFree || open(2, nullptr, &ver, &m.wlanH) != ERROR_SUCCESS) {
                FreeLibrary(m.wlan); m.wlan = nullptr; m.wlanH = nullptr;
            }
        }
    }
    if (m.wlanH) {
        PWLAN_INTERFACE_INFO_LIST list = nullptr;
        if (m.wEnum(m.wlanH, nullptr, &list) == ERROR_SUCCESS && list) {
            for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
                WLAN_INTERFACE_INFO& ii = list->InterfaceInfo[i];
                if (ii.isState != wlan_interface_state_connected) continue;
                DWORD sz = 0; PWLAN_CONNECTION_ATTRIBUTES ca = nullptr; WLAN_OPCODE_VALUE_TYPE vt;
                if (m.wQuery(m.wlanH, &ii.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &sz, (PVOID*)&ca, &vt) == ERROR_SUCCESS && ca) {
                    d.wsig = ca->wlanAssociationAttributes.wlanSignalQuality;
                    d.wRx = ca->wlanAssociationAttributes.ulRxRate / 1000.0;
                    d.wTx = ca->wlanAssociationAttributes.ulTxRate / 1000.0;
                    d.wPhy = (int)ca->wlanAssociationAttributes.dot11PhyType;
                    DOT11_SSID& ss = ca->wlanAssociationAttributes.dot11Ssid;
                    int n = ss.uSSIDLength ? MultiByteToWideChar(CP_UTF8, 0, (const char*)ss.ucSSID, (int)ss.uSSIDLength, d.ssid, 39) : 0;
                    d.ssid[n > 0 ? n : 0] = 0;
                    m.wFree(ca);
                }
                ULONG* ch = nullptr;
                if (m.wQuery(m.wlanH, &ii.InterfaceGuid, wlan_intf_opcode_channel_number, nullptr, &sz, (PVOID*)&ch, &vt) == ERROR_SUCCESS && ch) {
                    d.wChannel = (int)*ch;
                    m.wFree(ch);
                }
                break;
            }
            m.wFree(list);
        }
    }
    if (m.tick % 10 == 1 || !m.ipv4[0]) {
        m.ipv4[0] = 0;
        ULONG64 luid = g_sh->snap.wifiLuid;
        ULONG size = 32768; std::vector<BYTE> b(size);
        ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, (PIP_ADAPTER_ADDRESSES)b.data(), &size);
        if (r == ERROR_BUFFER_OVERFLOW) { b.resize(size); r = GetAdaptersAddresses(AF_INET, flags, nullptr, (PIP_ADAPTER_ADDRESSES)b.data(), &size); }
        if (r == NO_ERROR && luid) {
            for (PIP_ADAPTER_ADDRESSES a = (PIP_ADAPTER_ADDRESSES)b.data(); a && !m.ipv4[0]; a = a->Next) {
                if (a->Luid.Value != luid) continue;
                for (PIP_ADAPTER_UNICAST_ADDRESS u = a->FirstUnicastAddress; u; u = u->Next)
                    if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET) {
                        InetNtopW(AF_INET, &((sockaddr_in*)u->Address.lpSockaddr)->sin_addr, m.ipv4, 48);
                        break;
                    }
            }
        }
    }
    wcscpy_s(d.ipv4, m.ipv4);
}

// ---------------------------------------------------------------- thread
static DWORD WINAPI Det_Thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    DetState& m = g_ds;
    QueryPerformanceFrequency(&m.freq);
    HANDLE hs[2] = { g_detStop, g_detKick };
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, 2000);
        if (w == WAIT_OBJECT_0) break;
        int want = g_detWant.load();
        if (!want) { Det_ProcsRelease(); Det_GpuRelease(); Det_NetRelease(); continue; }
        m.tick++;
        DetailData d;
        if (want & DET_GPU) Det_Gpu(d); else Det_GpuRelease();
        if (want & DET_PROCS) Det_Procs(d, (want & DET_GPU) != 0); else Det_ProcsRelease();
        if (want & DET_NET) { Det_Net(d); if (want & DET_PROCS) Det_Conns(d); } else Det_NetRelease();
        AcquireSRWLockExclusive(&g_detLock);
        g_detShared = std::move(d);
        ReleaseSRWLockExclusive(&g_detLock);
        if (g_vCtl) PostMessageW(g_vCtl, WM_GS_DETAIL, 0, 0);
    }
    Det_ProcsRelease(); Det_GpuRelease(); Det_NetRelease();
    return 0;
}
static void Det_Start() {
    g_detStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_detKick = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_detThread = CreateThread(nullptr, 256 * 1024, Det_Thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
}
static void Det_Stop() {
    if (!g_detThread) return;
    SetEvent(g_detStop);
    WaitForSingleObject(g_detThread, 5000);
    CloseHandle(g_detThread); g_detThread = nullptr;
}
static void Det_Kick() { if (g_detKick && g_detWant.load()) SetEvent(g_detKick); }
static void Det_Fetch() {
    AcquireSRWLockExclusive(&g_detLock);
    g_det = g_detShared;
    ReleaseSRWLockExclusive(&g_detLock);
}
