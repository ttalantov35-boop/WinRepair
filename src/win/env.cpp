#include "app.h"
#include <cctype>


namespace wr {

namespace {
Env g_env;
}

const Env& GetEnv() { return g_env; }

static bool IsElevated() {
    HANDLE tok = nullptr;
    bool res = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION te{};
        DWORD sz = 0;
        if (GetTokenInformation(tok, (TOKEN_INFORMATION_CLASS)20 /*TokenElevation*/, &te, sizeof te, &sz)) res = te.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    if (res) return true;
    // Fallback: membership in Administrators (pre-Vista style / SYSTEM in WinPE)
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admins = nullptr;
    BOOL member = FALSE;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) {
        CheckTokenMembership(nullptr, admins, &member);
        FreeSid(admins);
    }
    return member != 0;
}

void InitEnvironment() {
    Env& e = g_env;
    // 32-bit build on 64-bit OS: make System32 point to the real 64-bit tools (dism, sfc, ...)
    typedef BOOL(WINAPI * PIsWow64)(HANDLE, PBOOL);
    typedef BOOL(WINAPI * PDisableRedir)(PVOID*);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    PIsWow64 isw = k ? (PIsWow64)GetProcAddress(k, "IsWow64Process") : nullptr;
    BOOL w64 = FALSE;
    if (isw) isw(GetCurrentProcess(), &w64);
    e.isWow64 = w64 != 0;
    if (e.isWow64) {
        PDisableRedir dr = (PDisableRedir)GetProcAddress(k, "Wow64DisableWow64FsRedirection");
        PVOID old = nullptr;
        if (dr) dr(&old);
    }

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: e.arch = "x64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: e.arch = "x86"; break;
        case 12: e.arch = "ARM64"; break;
        default: e.arch = "?";
    }

    // true OS version (manifest lists supported OSes; RtlGetVersion never lies)
    typedef LONG(WINAPI * PRtlGetVersion)(OSVERSIONINFOW*);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    PRtlGetVersion rgv = nt ? (PRtlGetVersion)GetProcAddress(nt, "RtlGetVersion") : nullptr;
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof vi;
    if (rgv) rgv(&vi);
    e.major = vi.dwMajorVersion; e.minor = vi.dwMinorVersion; e.build = vi.dwBuildNumber;

    wchar_t wd[MAX_PATH];
    UINT n = GetWindowsDirectoryW(wd, MAX_PATH);
    e.winDir = U(wd, (int)n);
    e.sysDrive = e.winDir.substr(0, 2);
    if (!e.sysDrive.empty()) e.sysDrive[0] = (char)toupper((unsigned char)e.sysDrive[0]);

    wchar_t cn[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD cl = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(cn, &cl)) e.computer = U(cn, (int)cl);

    e.isWinPE = RegKeyExists(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\MiniNT") ||
                PathExists(e.winDir + "\\System32\\winpeshl.exe");
    e.isWinRE = e.isWinPE && (PathExists(e.sysDrive + "\\sources\\recovery\\RecEnv.exe") || PathExists(e.winDir + "\\System32\\Recovery\\RecEnv.exe"));
    e.winreShell = PathExists(ExeDir() + "WinRepair.winre");
    e.isAdmin = IsElevated();

    const char* cv = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string prod = RegStr(HKEY_LOCAL_MACHINE, cv, "ProductName");
    if (e.build >= 22000) {  // Windows 11 still reports "Windows 10 ..." in ProductName
        size_t p = prod.find("Windows 10");
        if (p != std::string::npos) prod.replace(p, 10, "Windows 11");
    }
    std::string dv = RegStr(HKEY_LOCAL_MACHINE, cv, "DisplayVersion");
    if (dv.empty()) dv = RegStr(HKEY_LOCAL_MACHINE, cv, "ReleaseId");
    DWORD ubr = 0;
    bool hasUbr = RegDword(HKEY_LOCAL_MACHINE, cv, "UBR", ubr);
    e.osName = prod.empty() ? fmt("Windows %u.%u", e.major, e.minor) : prod;
    if (!dv.empty()) e.osName += " " + dv;
    e.osName += hasUbr ? fmt(" (build %u.%lu)", e.build, (unsigned long)ubr) : fmt(" (build %u)", e.build);
}

std::vector<Target> FindTargets() {
    std::vector<Target> v;
    const Env& e = g_env;
    if (!e.isWinPE) {
        Target t;
        t.root = e.sysDrive + "\\";
        t.winDir = e.winDir;
        t.online = true;
        t.valid = true;
        t.label = e.sysDrive + "  " + e.osName + "  [текущая, онлайн]";
        v.push_back(t);
    }
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char L = (char)('A' + i);
        std::string root = std::string(1, L) + ":\\";
        if (!e.sysDrive.empty() && e.sysDrive[0] == L) continue;
        UINT dt = GetDriveTypeW(W(root).c_str());
        if (dt != DRIVE_FIXED && dt != DRIVE_REMOVABLE) continue;
        std::string win = root + "Windows";
        if (!PathExists(win + "\\System32\\config\\SYSTEM")) continue;
        Target t;
        t.root = root;
        t.winDir = win;
        t.online = false;
        t.valid = true;
        std::string ver = FileVersion(win + "\\System32\\ntoskrnl.exe");
        t.label = fmt("%c:  Windows %s  [офлайн]", L, ver.empty() ? "?" : ver.c_str());
        v.push_back(t);
    }
    return v;
}

}  // namespace wr
