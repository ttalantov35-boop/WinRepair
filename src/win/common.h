// WinRepair GUI - Windows layer: common helpers (UTF-8 strings at our API, UTF-16 at Win32 boundary).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../core/json.h"
#include "lang.h"
#include "../core/textutil.h"

namespace wr {

std::wstring W(const std::string& s);     // UTF-8 -> UTF-16; in English mode known Russian texts are translated
std::wstring WRaw(const std::string& s);  // plain conversion (no translation)
std::string U(const wchar_t* s, int len = -1);
inline std::string U(const std::wstring& s) { return U(s.c_str(), (int)s.size()); }

std::string ExeDir();            // with trailing backslash
std::string DataDir();           // writable data root (exe dir\Data or %TEMP%\WinRepair\), trailing backslash
std::string SysDir();            // %SystemRoot%\System32 (no trailing backslash)
std::string EnvVar(const char* name);
std::string ErrMsg(DWORD err);

bool PathExists(const std::string& p);
bool IsDir(const std::string& p);
bool MakeDirs(const std::string& p);
bool WriteText(const std::string& path, const std::string& utf8, bool bom = true);
bool ReadBytes(const std::string& path, std::vector<uint8_t>& out, size_t maxBytes);
std::vector<std::string> ListFiles(const std::string& dir, const std::string& pattern);  // names only

std::string Stamp();    // yyyymmdd_HHMMSS
std::string IsoNow();   // yyyy-mm-ddTHH:MM:SS
std::string NiceNow();  // yyyy-mm-dd HH:MM:SS
std::string FileTimeStr(unsigned long long ft);

std::string RegStr(HKEY root, const char* sub, const char* val, REGSAM wow = 0);
bool RegDword(HKEY root, const char* sub, const char* val, DWORD& out, REGSAM wow = 0);
bool RegKeyExists(HKEY root, const char* sub);
bool RegValueExists(HKEY root, const char* sub, const char* val);

std::string FileVersion(const std::string& path);  // "10.0.22631.2861" or ""
std::string FindInPath(const char* exeName);       // full path or ""
std::string SysTool(const char* name);             // System32\name when present, else name

// ---- process execution -------------------------------------------------
struct ProcResult {
    bool started = false;
    DWORD exitCode = (DWORD)-1;
    bool cancelled = false, timedOut = false;
    std::string output;  // UTF-8 (OEM / UTF-16 converted)
    std::string error;
};
using LineSink = std::function<void(const std::string& line, bool progress)>;
ProcResult RunProcess(const std::string& cmdline, const std::string& workDir, const LineSink& sink,
                      const std::atomic<bool>* cancel, int timeoutSec, const std::string& input = "");

// ---- logging -----------------------------------------------------------
enum class Lvl { Info, Warn, Error, Out, Ok };
class Logger {
public:
    static Logger& I();
    bool init(const std::string& dir);
    void attachUi(HWND h, UINT logMsg, UINT statusMsg);
    void log(Lvl l, const std::string& msg);
    void progress(const std::string& s);
    std::string path() const { return path_; }

private:
    std::mutex m_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::string path_;
    HWND hwnd_ = nullptr;
    UINT logMsg_ = 0, statusMsg_ = 0;
};

// translating wrappers for text that reaches the screen
int TrMessageBox(HWND h, const wchar_t* text, const wchar_t* caption, UINT flags);
BOOL TrSetWindowText(HWND h, const wchar_t* text);
HWND TrCreateWindowEx(DWORD ex, LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y, int w, int hgt, HWND parent, HMENU menu, HINSTANCE inst, LPVOID lp);

}  // namespace wr

#define MessageBoxW(h, t, c, f) ::wr::TrMessageBox(h, t, c, f)
#define SetWindowTextW(h, t) ::wr::TrSetWindowText(h, t)
#define CreateWindowExW(ex, cls, txt, st, x, y, w, hg, par, menu, inst, lp) ::wr::TrCreateWindowEx(ex, cls, txt, st, x, y, w, hg, par, menu, inst, lp)
