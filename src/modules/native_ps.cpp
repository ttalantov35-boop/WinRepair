// Native (no PowerShell) replacements of the PowerShell cmdlets used in the command catalogue.
#include "catalog.h"
#include "../win/offreg.h"

#include <cstring>

#define PSAPI_VERSION 2
#include <psapi.h>
#include <tlhelp32.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>

namespace wr {

namespace {

std::string PadU(const std::string& s, size_t w, bool right = false) {
    size_t cp = 0, bytes = 0;
    for (; bytes < s.size(); ++bytes) if (((unsigned char)s[bytes] & 0xC0) != 0x80) { if (cp == w) break; ++cp; }
    std::string t = s.substr(0, bytes);
    if (cp < w) { std::string sp(w - cp, ' '); t = right ? sp + t : t + sp; }
    return t;
}

std::string SockStr(const sockaddr* sa) {
    char b[64] = {0};
    if (!sa) return "";
    if (sa->sa_family == AF_INET) InetNtopA(AF_INET, (void*)&((const sockaddr_in*)sa)->sin_addr, b, sizeof b);
    else if (sa->sa_family == AF_INET6) InetNtopA(AF_INET6, (void*)&((const sockaddr_in6*)sa)->sin6_addr, b, sizeof b);
    return b;
}

struct PRow { DWORD pid, ppid, thr; std::string name; double ws, cpu; };

void ProcessList(Ctx& c, OutBuf& o, bool sortCpu) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) { c.error("CreateToolhelp32Snapshot: " + ErrMsg(GetLastError())); return; }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    std::vector<PRow> v;
    if (Process32FirstW(s, &pe)) {
        do {
            PRow r{pe.th32ProcessID, pe.th32ParentProcessID, pe.cntThreads, U(pe.szExeFile), 0, -1};
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.pid);
            if (h) {
                FILETIME cr, ex, k, u;
                if (GetProcessTimes(h, &cr, &ex, &k, &u)) {
                    ULARGE_INTEGER a, b;
                    a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime;
                    b.LowPart = u.dwLowDateTime; b.HighPart = u.dwHighDateTime;
                    r.cpu = (double)(a.QuadPart + b.QuadPart) / 1e7;
                }
                PROCESS_MEMORY_COUNTERS pmc{};
                pmc.cb = sizeof pmc;
                if (GetProcessMemoryInfo(h, &pmc, sizeof pmc)) r.ws = (double)pmc.WorkingSetSize / 1048576.0;
                CloseHandle(h);
            }
            v.push_back(r);
        } while (Process32NextW(s, &pe));
    }
    CloseHandle(s);
    if (sortCpu) std::sort(v.begin(), v.end(), [](const PRow& a, const PRow& b) { return a.cpu > b.cpu; });
    else std::sort(v.begin(), v.end(), [](const PRow& a, const PRow& b) { return toLowerUtf8(a.name) < toLowerUtf8(b.name); });
    o.add(PadU("PID", 7, true) + PadU("PPID", 7, true) + "  " + PadU("Имя процесса", 34) + PadU("Потоки", 8, true) + PadU("WS, МБ", 10, true) + PadU("CPU, с", 12, true));
    for (const auto& r : v)
        o.add(PadU(std::to_string(r.pid), 7, true) + PadU(std::to_string(r.ppid), 7, true) + "  " + PadU(r.name, 34) + PadU(std::to_string(r.thr), 8, true) +
              PadU(r.ws > 0 ? fmt("%.1f", r.ws) : "-", 10, true) + PadU(r.cpu >= 0 ? fmt("%.2f", r.cpu) : "-", 12, true));
    o.add(fmt("Процессов: %zu", v.size()));
}

const char* SvcState(DWORD s) {
    switch (s) {
        case SERVICE_STOPPED: return "Stopped";
        case SERVICE_START_PENDING: return "StartPending";
        case SERVICE_STOP_PENDING: return "StopPending";
        case SERVICE_RUNNING: return "Running";
        case SERVICE_CONTINUE_PENDING: return "ContinuePending";
        case SERVICE_PAUSE_PENDING: return "PausePending";
        case SERVICE_PAUSED: return "Paused";
        default: return "?";
    }
}
const char* StartType(DWORD t) {
    switch (t) {
        case SERVICE_BOOT_START: return "Boot";
        case SERVICE_SYSTEM_START: return "System";
        case SERVICE_AUTO_START: return "Automatic";
        case SERVICE_DEMAND_START: return "Manual";
        case SERVICE_DISABLED: return "Disabled";
        default: return "?";
    }
}

void AdapterList(Ctx& c, OutBuf& o, bool ipView) {
    ULONG sz = 32768;
    std::vector<uint8_t> buf(sz);
    ULONG fl = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, fl, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &sz);
    if (rc == ERROR_BUFFER_OVERFLOW) { buf.resize(sz); rc = GetAdaptersAddresses(AF_UNSPEC, fl, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &sz); }
    if (rc != NO_ERROR) { c.error("GetAdaptersAddresses: " + ErrMsg(rc)); return; }
    if (!ipView) o.add(PadU("Имя", 22) + PadU("Описание", 40) + PadU("Idx", 5, true) + "  " + PadU("Статус", 8) + PadU("Скорость", 12) + "MAC");
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        std::string name = U(a->FriendlyName), desc = U(a->Description);
        bool up = a->OperStatus == IfOperStatusUp;
        if (!ipView) {
            char mac[32] = "";
            if (a->PhysicalAddressLength == 6)
                snprintf(mac, sizeof mac, "%02X-%02X-%02X-%02X-%02X-%02X", a->PhysicalAddress[0], a->PhysicalAddress[1], a->PhysicalAddress[2], a->PhysicalAddress[3], a->PhysicalAddress[4], a->PhysicalAddress[5]);
            std::string sp = a->TransmitLinkSpeed == (ULONG64)-1 || a->TransmitLinkSpeed == 0 ? "-" : (a->TransmitLinkSpeed >= 1000000000ULL ? fmt("%.0f Гбит/с", a->TransmitLinkSpeed / 1e9) : fmt("%.0f Мбит/с", a->TransmitLinkSpeed / 1e6));
            o.add(PadU(name, 22) + PadU(desc, 40) + PadU(std::to_string(a->IfIndex), 5, true) + "  " + PadU(up ? "Up" : "Down", 8) + PadU(sp, 12) + mac);
        } else {
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            std::string v4, v6, gw, dns;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                std::string s = SockStr(u->Address.lpSockaddr);
                if (u->Address.lpSockaddr->sa_family == AF_INET) v4 += (v4.empty() ? "" : ", ") + s; else v6 += (v6.empty() ? "" : ", ") + s;
            }
            for (auto* g = a->FirstGatewayAddress; g; g = g->Next) gw += (gw.empty() ? "" : ", ") + SockStr(g->Address.lpSockaddr);
            for (auto* d = a->FirstDnsServerAddress; d; d = d->Next) dns += (dns.empty() ? "" : ", ") + SockStr(d->Address.lpSockaddr);
            o.add("InterfaceAlias       : " + name);
            o.add(fmt("InterfaceIndex       : %lu", (unsigned long)a->IfIndex));
            o.add("InterfaceDescription : " + desc);
            o.add("Status               : " + std::string(up ? "Up" : "Down"));
            o.add("IPv4Address          : " + v4);
            o.add("IPv6Address          : " + v6);
            o.add("IPv4DefaultGateway   : " + gw);
            o.add("DNSServer            : " + dns);
            o.add("");
        }
    }
}

}  // namespace

void NatGetProcess(Ctx& c, OutBuf& o) { ProcessList(c, o, false); }
void NatGetProcessCpu(Ctx& c, OutBuf& o) { ProcessList(c, o, true); }

static void OfflineServices(Ctx& c, OutBuf& o) {
    RegCtx rc;
    std::string err;
    if (!rc.open(c.target, err)) { c.error("Реестр: " + err); return; }
    HKEY k = nullptr;
    if (RegOpenKeyExW(rc.system, W(rc.cs + "\\Services").c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) { c.error("Нет раздела Services"); return; }
    std::vector<std::string> names;
    EnumSubKeys(k, names);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return toLowerUtf8(a) < toLowerUtf8(b); });
    o.add("(офлайн: данные из реестра целевой ОС)");
    o.add(PadU("Запуск", 11) + PadU("Тип", 10) + PadU("Имя", 36) + "Образ");
    static const char* st[] = {"Boot", "System", "Automatic", "Manual", "Disabled"};
    int cnt = 0;
    for (const auto& n : names) {
        RegVal s, t, ip;
        if (!QueryVal(k, n, "Start", s) || s.data.size() < 4) continue;
        DWORD start = *(DWORD*)s.data.data(), type = (QueryVal(k, n, "Type", t) && t.data.size() >= 4) ? *(DWORD*)t.data.data() : 0;
        QueryVal(k, n, "ImagePath", ip);
        o.add(PadU(st[start > 4 ? 4 : start], 11) + PadU((type & 0x30) ? "Service" : "Driver", 10) + PadU(n, 36) + RegText(ip));
        ++cnt;
    }
    RegCloseKey(k);
    o.add(fmt("Записей: %d", cnt));
}

void NatGetService(Ctx& c, OutBuf& o) {
    if (c.target.valid && !c.target.online) { OfflineServices(c, o); return; }
    SC_HANDLE m = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!m) { c.error("OpenSCManager: " + ErrMsg(GetLastError())); return; }
    struct Row { std::string state, start, name, disp; };
    std::vector<Row> rows;
    std::vector<uint8_t> buf(1 << 16);
    DWORD resume = 0;
    for (;;) {
        DWORD need = 0, cnt = 0;
        BOOL ok = EnumServicesStatusExW(m, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf.data(), (DWORD)buf.size(), &need, &cnt, &resume, nullptr);
        if (!ok && GetLastError() != ERROR_MORE_DATA) { c.error("EnumServicesStatusEx: " + ErrMsg(GetLastError())); break; }
        auto* s = (ENUM_SERVICE_STATUS_PROCESSW*)buf.data();
        for (DWORD i = 0; i < cnt; ++i) {
            Row r{SvcState(s[i].ServiceStatusProcess.dwCurrentState), "?", U(s[i].lpServiceName), U(s[i].lpDisplayName)};
            SC_HANDLE sv = OpenServiceW(m, s[i].lpServiceName, SERVICE_QUERY_CONFIG);
            if (sv) {
                std::vector<uint8_t> cb(8192);
                DWORD n = 0;
                if (QueryServiceConfigW(sv, (QUERY_SERVICE_CONFIGW*)cb.data(), (DWORD)cb.size(), &n)) r.start = StartType(((QUERY_SERVICE_CONFIGW*)cb.data())->dwStartType);
                CloseServiceHandle(sv);
            }
            rows.push_back(r);
        }
        if (ok) break;
    }
    CloseServiceHandle(m);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return toLowerUtf8(a.name) < toLowerUtf8(b.name); });
    o.add(PadU("Состояние", 16) + PadU("Запуск", 11) + PadU("Имя", 36) + "Отображаемое имя");
    for (const auto& r : rows) o.add(PadU(r.state, 16) + PadU(r.start, 11) + PadU(r.name, 36) + r.disp);
    o.add(fmt("Служб: %zu", rows.size()));
}

void NatOperatingSystem(Ctx& c, OutBuf& o) {
    if (c.target.valid && !c.target.online) {
        RegCtx rc;
        std::string err;
        if (!rc.open(c.target, err)) { c.error("Реестр: " + err); return; }
        const char* cv = "Microsoft\\Windows NT\\CurrentVersion";
        auto g = [&](const char* n) { RegVal v; return QueryVal(rc.software, cv, n, v) ? RegValueText(v) : std::string(); };
        RegVal cn;
        std::string comp = QueryVal(rc.system, rc.cs + "\\Control\\ComputerName\\ComputerName", "ComputerName", cn) ? RegText(cn) : "";
        std::string idate;
        RegVal iv;
        if (QueryVal(rc.software, cv, "InstallDate", iv) && iv.data.size() >= 4) idate = FileTimeStr(((unsigned long long)*(DWORD*)iv.data.data() + 11644473600ULL) * 10000000ULL);
        o.add("(офлайн: данные из реестра целевой ОС " + c.target.winDir + ")");
        o.add("ProductName            : " + g("ProductName"));
        o.add("DisplayVersion         : " + g("DisplayVersion"));
        o.add("CurrentBuild           : " + g("CurrentBuild"));
        o.add("EditionID              : " + g("EditionID"));
        o.add("InstallDate            : " + idate);
        o.add("RegisteredOwner        : " + g("RegisteredOwner"));
        o.add("ProductId              : " + g("ProductId"));
        o.add("ComputerName           : " + comp);
        o.add(std::string("OSArchitecture         : ") + (PathExists(c.target.winDir + "\\SysWOW64") ? "x64" : "x86"));
        o.add("WindowsDirectory       : " + c.target.winDir);
        return;
    }
    (void)c;
    const Env& e = GetEnv();
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    const char* cv = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    DWORD inst = 0;
    std::string idate;
    if (RegDword(HKEY_LOCAL_MACHINE, cv, "InstallDate", inst)) idate = FileTimeStr(((unsigned long long)inst + 11644473600ULL) * 10000000ULL);
    ULONGLONG up = GetTickCount64();
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    unsigned long long nowFt = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
    std::string boot = FileTimeStr(nowFt - up * 10000ULL);
    wchar_t loc[LOCALE_NAME_MAX_LENGTH] = {0};
    GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH);
    o.add("Caption                : " + e.osName);
    o.add(fmt("Version                : %u.%u.%u", e.major, e.minor, e.build));
    o.add("OSArchitecture         : " + e.arch);
    o.add("CSName                 : " + e.computer);
    o.add("InstallDate            : " + idate);
    o.add("LastBootUpTime         : " + boot);
    o.add(fmt("TotalVisibleMemory(КБ): %llu", (unsigned long long)(ms.ullTotalPhys / 1024)));
    o.add(fmt("FreePhysicalMemory(КБ): %llu", (unsigned long long)(ms.ullAvailPhys / 1024)));
    o.add("SystemDirectory        : " + SysDir());
    o.add("WindowsDirectory       : " + e.winDir);
    o.add("SystemDrive            : " + e.sysDrive);
    o.add("RegisteredUser         : " + RegStr(HKEY_LOCAL_MACHINE, cv, "RegisteredOwner"));
    o.add("SerialNumber (ProductId): " + RegStr(HKEY_LOCAL_MACHINE, cv, "ProductId"));
    o.add("Locale                 : " + U(loc));
    o.add(std::string("WinPE                  : ") + (e.isWinPE ? "да" : "нет"));
}

void NatEventLogSystem(Ctx& c, OutBuf& o) {
    if (!c.needTarget()) return;
    std::vector<EvtRow> rows;
    std::string err;
    if (!QueryEvents(c, "System", "*", 50, true, false, rows, err)) c.warn("Журнал System: " + err);
    for (const auto& r : rows)
        o.add(r.time + "  " + PadU(r.level, 14) + PadU(r.provider, 36) + PadU(std::to_string(r.id), 6, true) + "  " + truncUtf8(r.message, 140));
    o.add(fmt("Событий: %zu", rows.size()));
}

void NatNetAdapter(Ctx& c, OutBuf& o) { AdapterList(c, o, false); }
void NatNetIPConfiguration(Ctx& c, OutBuf& o) { AdapterList(c, o, true); }

void NatGetDisk(Ctx& c, OutBuf& o) {
    (void)c;
    auto d = CollectDisks();
    o.add(PadU("Number", 7, true) + "  " + PadU("FriendlyName", 38) + PadU("Serial", 22) + PadU("Health", 10) + PadU("Partition", 10) + PadU("Size", 12, true));
    for (const auto& x : d)
        o.add(PadU(std::to_string(x.number), 7, true) + "  " + PadU(x.model, 38) + PadU(x.serial, 22) + PadU(x.health, 10) + PadU(x.partStyle, 10) + PadU(fmtBytes(x.size), 12, true));
    if (d.empty()) o.add("Физические диски не обнаружены.");
}

void NatPhysicalDisk(Ctx& c, OutBuf& o) {
    (void)c;
    auto d = CollectDisks();
    o.add(PadU("Id", 4, true) + "  " + PadU("FriendlyName", 36) + PadU("MediaType", 12) + PadU("BusType", 8) + PadU("Health", 9) + PadU("Size", 11, true) + "  Temp/Износ/Наработка");
    for (const auto& x : d) {
        std::string extra = (x.temp >= 0 ? fmt("%d°C", x.temp) : "-") + " / " + (x.wear >= 0 ? fmt("%d%%", x.wear) : "-") + " / " + (x.powerOnHours >= 0 ? fmt("%lld ч", x.powerOnHours) : "-");
        o.add(PadU(std::to_string(x.number), 4, true) + "  " + PadU(x.model, 36) + PadU(x.media, 12) + PadU(x.bus, 8) + PadU(x.health, 9) + PadU(fmtBytes(x.size), 11, true) + "  " + extra);
    }
}

void NatGetVolume(Ctx& c, OutBuf& o) {
    (void)c;
    o.add(PadU("Диск", 6) + PadU("Метка", 22) + PadU("ФС", 8) + PadU("Тип", 10) + PadU("Размер", 12, true) + PadU("Свободно", 12, true));
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = std::wstring(1, (wchar_t)('A' + i)) + L":\\";
        UINT dt = GetDriveTypeW(root.c_str());
        const char* tn = dt == DRIVE_FIXED ? "Fixed" : dt == DRIVE_REMOVABLE ? "Removable" : dt == DRIVE_CDROM ? "CD-ROM" : dt == DRIVE_REMOTE ? "Network" : dt == DRIVE_RAMDISK ? "RAM" : "Unknown";
        wchar_t label[128] = {0}, fs[64] = {0};
        GetVolumeInformationW(root.c_str(), label, 128, nullptr, nullptr, nullptr, fs, 64);
        ULARGE_INTEGER fb{}, tb{}, tf{};
        GetDiskFreeSpaceExW(root.c_str(), &fb, &tb, &tf);
        o.add(PadU(U(root.substr(0, 2)), 6) + PadU(U(label), 22) + PadU(U(fs), 8) + PadU(tn, 10) + PadU(tb.QuadPart ? fmtBytes(tb.QuadPart) : "-", 12, true) + PadU(tb.QuadPart ? fmtBytes(fb.QuadPart) : "-", 12, true));
    }
}

void NatRestorePoints(Ctx& c, OutBuf& o) {
    if (!c.needTarget()) return;
    RegCtx rc;
    std::string err;
    if (rc.open(c.target, err)) {
        auto show = [&](HKEY root, const std::string& sub, const char* name, const char* label) {
            RegVal v;
            o.add(std::string(label) + ": " + (QueryVal(root, sub, name, v) ? RegValueText(v) : std::string("(не задано)")));
        };
        const std::string k1 = "Microsoft\\Windows NT\\CurrentVersion\\SystemRestore", k2 = "Policies\\Microsoft\\Windows NT\\SystemRestore";
        show(rc.software, k1, "DisableSR", "SystemRestore\\DisableSR (1 = защита системы выключена)");
        show(rc.software, k1, "RPSessionInterval", "RPSessionInterval");
        show(rc.software, k1, "SystemRestorePointCreationFrequency", "SystemRestorePointCreationFrequency (минуты)");
        show(rc.software, k2, "DisableSR", "Политика DisableSR");
        show(rc.software, k2, "DisableConfig", "Политика DisableConfig");
    } else c.warn("Реестр целевой ОС недоступен: " + err);
    std::string svi = c.target.root + "System Volume Information\\";
    auto dirs = ListDirs(svi);
    int rpDirs = 0;
    for (const auto& d : dirs) if (toLowerUtf8(d).find("_restore") != std::string::npos) { o.add("Каталог XP-точек: " + d); ++rpDirs; }
    if (dirs.empty()) o.add("System Volume Information недоступна для чтения или пуста (нужен запуск от SYSTEM/администратора).");
    std::string vs = SysDir() + "\\vssadmin.exe";
    if (PathExists(vs)) {
        std::string dr = std::string(1, c.target.letter()) + ":";
        ProcResult r = c.run("vssadmin list shadows /for=" + dr, "\"" + vs + "\" list shadows /for=" + dr, 60);
        int n = 0;
        std::string l = toLowerUtf8(r.output);
        for (size_t p = 0; (p = l.find("shadow copy id", p)) != std::string::npos; ++p) ++n;
        for (size_t p = 0; (p = l.find("идентификатор теневой копии", p)) != std::string::npos; ++p) ++n;
        o.add(fmt("Теневых копий (точек восстановления) на %s: %d", dr.c_str(), n));
        if (n == 0) o.add("Точек восстановления нет: откат через rstrui невозможен. Рассмотрите RegBack/DISM/восстановление установкой.");
    } else o.add("vssadmin.exe нет в этой среде — список теневых копий недоступен (rstrui /offline покажет точки сам).");
}

void NatPnpDevice(Ctx& c, OutBuf& o) {
    HDEVINFO h = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (h == INVALID_HANDLE_VALUE) { c.error("SetupDiGetClassDevs: " + ErrMsg(GetLastError())); return; }
    struct Row { std::string status, cls, name, id; };
    std::vector<Row> rows;
    SP_DEVINFO_DATA dd{};
    dd.cbSize = sizeof dd;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &dd); ++i) {
        auto prop = [&](DWORD id) {
            wchar_t b[512] = {0};
            DWORD sz = 0;
            SetupDiGetDeviceRegistryPropertyW(h, &dd, id, nullptr, (PBYTE)b, sizeof b - 2, &sz);
            return U(b);
        };
        ULONG st = 0, pr = 0;
        std::string status = "OK";
        if (CM_Get_DevNode_Status(&st, &pr, dd.DevInst, 0) == CR_SUCCESS && (st & DN_HAS_PROBLEM)) status = fmt("Error(%lu)", (unsigned long)pr);
        wchar_t id[MAX_DEVICE_ID_LEN] = {0};
        CM_Get_Device_IDW(dd.DevInst, id, MAX_DEVICE_ID_LEN, 0);
        std::string name = prop(SPDRP_FRIENDLYNAME);
        if (name.empty()) name = prop(SPDRP_DEVICEDESC);
        rows.push_back({status, prop(SPDRP_CLASS), name, U(id)});
    }
    SetupDiDestroyDeviceInfoList(h);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return toLowerUtf8(a.cls + a.name) < toLowerUtf8(b.cls + b.name); });
    int errs = 0;
    o.add(PadU("Статус", 11) + PadU("Класс", 18) + PadU("Имя", 52) + "InstanceId");
    for (const auto& r : rows) { o.add(PadU(r.status, 11) + PadU(r.cls, 18) + PadU(r.name, 52) + r.id); if (r.status != "OK") ++errs; }
    o.add(fmt("Устройств: %zu, с ошибкой: %d", rows.size(), errs));
}


void NatSecureBoot(Ctx& c, OutBuf& o) {
    (void)c;
    EnablePrivilegeByName(SE_SYSTEM_ENVIRONMENT_NAME);
    struct V { const wchar_t* name; const wchar_t* guid; const char* label; };
    static const V vars[] = {{L"PK", L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}", "PK"}, {L"KEK", L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}", "KEK"},
                             {L"db", L"{d719b2cb-3d3a-4596-a3bc-dad00e67656f}", "db"}, {L"dbx", L"{d719b2cb-3d3a-4596-a3bc-dad00e67656f}", "dbx"}};
    auto has = [](const std::vector<uint8_t>& b, const char* s) {
        size_t n = strlen(s);
        return std::search(b.begin(), b.end(), (const uint8_t*)s, (const uint8_t*)s + n) != b.end();
    };
    std::vector<std::vector<uint8_t>> data(4);
    bool uefi = true;
    for (int i = 0; i < 4; ++i) {
        std::vector<uint8_t> buf(512 * 1024);
        DWORD n = GetFirmwareEnvironmentVariableW(vars[i].name, vars[i].guid, buf.data(), (DWORD)buf.size());
        if (n == 0) {
            DWORD e = GetLastError();
            o.add(std::string(vars[i].label) + ": недоступно — " + (e == ERROR_INVALID_FUNCTION ? "Legacy BIOS (нет UEFI)" : ErrMsg(e)));
            if (e == ERROR_INVALID_FUNCTION) { uefi = false; break; }
            continue;
        }
        buf.resize(n);
        data[(size_t)i] = buf;
        o.add(fmt("%s: прочитано %lu байт", vars[i].label, (unsigned long)n));
    }
    if (!uefi) { o.add("Система загружена в режиме Legacy BIOS — Secure Boot не применяется."); return; }
    const auto& KEK = data[1];
    const auto& DB = data[2];
    struct Chk { const char* what; const std::vector<uint8_t>* v; const char* needle; bool newCert; };
    const Chk chk[] = {{"KEK: Microsoft Corporation KEK 2K CA 2023", &KEK, "Microsoft Corporation KEK 2K CA 2023", true},
                       {"KEK: Microsoft Corporation KEK CA 2011", &KEK, "Microsoft Corporation KEK CA 2011", false},
                       {"db : Windows UEFI CA 2023 (подпись загрузчика Windows)", &DB, "Windows UEFI CA 2023", true},
                       {"db : Microsoft UEFI CA 2023", &DB, "Microsoft UEFI CA 2023", true},
                       {"db : Microsoft Option ROM UEFI CA 2023", &DB, "Microsoft Option ROM UEFI CA 2023", true},
                       {"db : Microsoft Windows Production PCA 2011 (истёк 19.10.2026)", &DB, "Microsoft Windows Production PCA 2011", false},
                       {"db : Microsoft UEFI CA 2011", &DB, "Microsoft UEFI CA 2011", false}};
    bool missingNew = false;
    for (const auto& k : chk) {
        bool p = !k.v->empty() && has(*k.v, k.needle);
        o.add(std::string(p ? "[есть]    " : "[нет]     ") + k.what);
        if (k.newCert && !p && !k.v->empty()) missingNew = true;
    }
    o.add(std::string("dbx: ") + (data[3].empty() ? "не прочитано" : fmt("%zu байт", data[3].size())));
    const char* sb = "SYSTEM\\CurrentControlSet\\Control\\SecureBoot";
    RegVal v;
    auto showReg = [&](const char* sub, const char* name) {
        RegVal x;
        if (QueryVal(HKEY_LOCAL_MACHINE, sub, name, x)) o.add(std::string("Реестр ") + name + ": " + RegValueText(x));
    };
    showReg(sb, "AvailableUpdates");
    showReg((std::string(sb) + "\\Servicing").c_str(), "UEFICA2023Status");
    showReg((std::string(sb) + "\\Servicing").c_str(), "UEFICA2023Error");
    showReg((std::string(sb) + "\\Servicing").c_str(), "WindowsUEFICA2023Capable");
    o.add("");
    if (missingNew) {
        o.add("ВЫВОД: сертификаты 2023 установлены не полностью. Windows Production PCA 2011 (подпись загрузчика Windows) истёк 19.10.2026, KEK CA 2011 и UEFI CA 2011 — в июне 2026.");
        o.add("Что делать: в работающей Windows установить актуальные накопительные обновления; затем (по документации Microsoft) задать AvailableUpdates=0x5944,");
        o.add("запустить задачу \\Microsoft\\Windows\\PI\\Secure-Boot-Update и перезагрузить компьютер дважды; при необходимости обновить BIOS/UEFI производителя.");
        o.add("Для сервисной работы: загрузочные носители/WinPE/WinRE, подписанные только старым PCA 2011, проверяйте на устройствах с обновлённой db/dbx.");
    } else o.add("ВЫВОД: сертификаты 2023 присутствуют в KEK и db.");
}

}  // namespace wr
