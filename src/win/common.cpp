#include "common.h"

#include <cstdio>
#include <cstring>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")

namespace wr {

static bool Dict_hasCyr(const std::string& s) { for (size_t i = 0; i + 1 < s.size(); ++i) { unsigned char c = (unsigned char)s[i]; if (c == 0xD0 || c == 0xD1) return true; } return false; }

std::wstring W(const std::string& s) {
    if (CurrentLang() == Lang::EN && Dict_hasCyr(s)) return WRaw(Tr(s));
    return WRaw(s);
}

std::wstring WRaw(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string U(const wchar_t* s, int len) {
    if (!s) return "";
    if (len < 0) len = (int)wcslen(s);
    if (len == 0) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string o((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, &o[0], n, nullptr, nullptr);
    return o;
}

std::string EnvVar(const char* name) {
    std::wstring wn = W(name);
    DWORD n = GetEnvironmentVariableW(wn.c_str(), nullptr, 0);
    if (!n) return "";
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(wn.c_str(), &v[0], n);
    v.resize(n);
    return U(v);
}

std::string ExeDir() {
    std::wstring b(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &b[0], (DWORD)b.size());
    b.resize(n);
    size_t p = b.find_last_of(L'\\');
    if (p != std::wstring::npos) b.resize(p + 1);
    return U(b);
}

std::string SysDir() {
    wchar_t b[MAX_PATH];
    UINT n = GetSystemDirectoryW(b, MAX_PATH);
    return U(b, (int)n);
}

std::string ErrMsg(DWORD err) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
    std::string s = n ? trim(U(buf, (int)n)) : "";
    if (buf) LocalFree(buf);
    return fmt("%s (0x%08lX)", s.c_str(), (unsigned long)err);
}

bool PathExists(const std::string& p) { return GetFileAttributesW(W(p).c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsDir(const std::string& p) {
    DWORD a = GetFileAttributesW(W(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool MakeDirs(const std::string& p) {
    std::wstring w = W(p);
    for (size_t i = 3; i < w.size(); ++i) {
        if (w[i] == L'\\' || w[i] == L'/') {
            std::wstring part = w.substr(0, i);
            CreateDirectoryW(part.c_str(), nullptr);
        }
    }
    if (!w.empty() && w.back() != L'\\') CreateDirectoryW(w.c_str(), nullptr);
    return IsDir(p);
}

bool WriteText(const std::string& path, const std::string& utf8, bool bom) {
    HANDLE h = CreateFileW(W(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = true;
    if (bom) { const unsigned char b[3] = {0xEF, 0xBB, 0xBF}; ok = WriteFile(h, b, 3, &w, nullptr) != 0; }
    size_t off = 0;
    while (ok && off < utf8.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(utf8.size() - off, 1u << 20);
        ok = WriteFile(h, utf8.data() + off, chunk, &w, nullptr) != 0;
        off += chunk;
    }
    CloseHandle(h);
    return ok;
}

bool ReadBytes(const std::string& path, std::vector<uint8_t>& out, size_t maxBytes) {
    HANDLE h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    size_t want = (size_t)std::min<unsigned long long>((unsigned long long)sz.QuadPart, maxBytes);
    out.assign(want, 0);
    size_t got = 0;
    while (got < want) {
        DWORD r = 0;
        if (!ReadFile(h, out.data() + got, (DWORD)std::min<size_t>(want - got, 1u << 20), &r, nullptr) || r == 0) break;
        got += r;
    }
    out.resize(got);
    CloseHandle(h);
    return true;
}

std::vector<std::string> ListFiles(const std::string& dir, const std::string& pattern) {
    std::vector<std::string> v;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(dir + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) v.push_back(U(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return v;
}

std::string DataDir() {
    static std::string dir;
    if (!dir.empty()) return dir;
    std::string cand = ExeDir() + "Data\\";
    MakeDirs(cand);
    std::string probe = cand + ".w" + Stamp();
    if (WriteText(probe, "x", false)) {
        DeleteFileW(W(probe).c_str());
        dir = cand;
    } else {
        std::string t = EnvVar("TEMP");
        if (t.empty()) t = "X:\\Windows\\Temp";
        if (t.back() != '\\') t += '\\';
        dir = t + "WinRepair\\";
        MakeDirs(dir);
    }
    return dir;
}

std::string Stamp() {
    SYSTEMTIME t; GetLocalTime(&t);
    return fmt("%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}
std::string IsoNow() {
    SYSTEMTIME t; GetLocalTime(&t);
    return fmt("%04d-%02d-%02dT%02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}
std::string NiceNow() {
    SYSTEMTIME t; GetLocalTime(&t);
    return fmt("%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}
std::string FileTimeStr(unsigned long long ft) {
    FILETIME f; f.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFF); f.dwHighDateTime = (DWORD)(ft >> 32);
    SYSTEMTIME u, l;
    if (!FileTimeToSystemTime(&f, &u)) return "";
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &u, &l)) l = u;
    return fmt("%04d-%02d-%02d %02d:%02d:%02d", l.wYear, l.wMonth, l.wDay, l.wHour, l.wMinute, l.wSecond);
}

// ---- registry -------------------------------------------------------------
std::string RegStr(HKEY root, const char* sub, const char* val, REGSAM wow) {
    HKEY k;
    if (RegOpenKeyExW(root, W(sub).c_str(), 0, KEY_READ | wow, &k) != ERROR_SUCCESS) return "";
    DWORD type = 0, sz = 0;
    std::wstring wv = W(val);
    std::string res;
    if (RegQueryValueExW(k, wv.c_str(), nullptr, &type, nullptr, &sz) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && sz) {
        std::wstring b(sz / 2 + 1, L'\0');
        DWORD sz2 = sz;
        if (RegQueryValueExW(k, wv.c_str(), nullptr, &type, (LPBYTE)&b[0], &sz2) == ERROR_SUCCESS) res = U(b.c_str());
    }
    RegCloseKey(k);
    return res;
}

bool RegDword(HKEY root, const char* sub, const char* val, DWORD& out, REGSAM wow) {
    HKEY k;
    if (RegOpenKeyExW(root, W(sub).c_str(), 0, KEY_READ | wow, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, sz = sizeof(DWORD), v = 0;
    bool ok = RegQueryValueExW(k, W(val).c_str(), nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS && type == REG_DWORD;
    RegCloseKey(k);
    if (ok) out = v;
    return ok;
}

bool RegKeyExists(HKEY root, const char* sub) {
    HKEY k;
    if (RegOpenKeyExW(root, W(sub).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

bool RegValueExists(HKEY root, const char* sub, const char* val) {
    HKEY k;
    if (RegOpenKeyExW(root, W(sub).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool ok = RegQueryValueExW(k, W(val).c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

std::string FileVersion(const std::string& path) {
    std::wstring wp = W(path);
    DWORD h = 0;
    DWORD sz = GetFileVersionInfoSizeW(wp.c_str(), &h);
    if (!sz) return "";
    std::vector<char> b(sz);
    if (!GetFileVersionInfoW(wp.c_str(), 0, sz, b.data())) return "";
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(b.data(), L"\\", (LPVOID*)&fi, &len) || !fi) return "";
    return fmt("%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
}

std::string FindInPath(const char* exeName) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = SearchPathW(nullptr, W(exeName).c_str(), nullptr, MAX_PATH * 2, buf, nullptr);
    return n ? U(buf, (int)n) : "";
}

std::string SysTool(const char* name) {
    std::string p = SysDir() + "\\" + name;
    return PathExists(p) ? p : std::string(name);
}

// ---- Logger -------------------------------------------------------------------
Logger& Logger::I() {
    static Logger l;
    return l;
}

bool Logger::init(const std::string& dir) {
    std::lock_guard<std::mutex> g(m_);
    MakeDirs(dir);
    SYSTEMTIME t; GetLocalTime(&t);
    path_ = dir + fmt("WinRepair_%04d%02d%02d.log", t.wYear, t.wMonth, t.wDay);
    file_ = CreateFileW(W(path_).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        const unsigned char b[3] = {0xEF, 0xBB, 0xBF};
        DWORD w;
        WriteFile(file_, b, 3, &w, nullptr);
    }
    return true;
}

void Logger::attachUi(HWND h, UINT logMsg, UINT statusMsg) {
    std::lock_guard<std::mutex> g(m_);
    hwnd_ = h; logMsg_ = logMsg; statusMsg_ = statusMsg;
}

void Logger::log(Lvl l, const std::string& msg) {
    static const char* tag[] = {"INFO ", "WARN ", "ERROR", "OUT  ", "OK   "};
    std::string line = NiceNow() + " [" + tag[(int)l] + "] " + Tr(msg);
    for (size_t i = 0; i < line.size();) { if (line[i] == '\r') line.erase(i, 1); else ++i; }
    std::lock_guard<std::mutex> g(m_);
    if (file_ != INVALID_HANDLE_VALUE) {
        std::string f = line + "\r\n";
        DWORD w;
        WriteFile(file_, f.data(), (DWORD)f.size(), &w, nullptr);
    }
    if (hwnd_) PostMessageW(hwnd_, logMsg_, (WPARAM)l, (LPARAM) new std::string(line));
}

void Logger::progress(const std::string& s) {
    std::lock_guard<std::mutex> g(m_);
    if (hwnd_) PostMessageW(hwnd_, statusMsg_, 0, (LPARAM) new std::string(s));
}

}  // namespace wr
