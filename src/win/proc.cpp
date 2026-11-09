// Child process runner: hidden console, merged stdout/stderr, OEM/UTF-16 decoding, job object, cancel/timeout.
#include "common.h"

namespace wr {

namespace {

struct Decoder {
    // Mode is chosen on the first chunk(s) that contain enough non-ASCII bytes and then stays fixed for the process.
    enum Mode { Undecided, Oem, Ansi, Utf8, Utf16 } mode = Undecided;
    std::string odd;     // carried-over bytes (odd UTF-16 byte / split UTF-8 sequence)
    static std::wstring Conv(UINT cp, const char* p, size_t n) {
        int wl = MultiByteToWideChar(cp, 0, p, (int)n, nullptr, 0);
        if (wl <= 0) return std::wstring(n, L'?');
        std::wstring w((size_t)wl, L'\0');
        MultiByteToWideChar(cp, 0, p, (int)n, &w[0], wl);
        return w;
    }
    static int Score(const std::wstring& w) { return cyrillicScore(std::u16string(w.begin(), w.end())); }
    std::string feed(const char* p, size_t n) {
        if (mode == Undecided && looksLikeUtf16le((const uint8_t*)p, n)) mode = Utf16;
        if (mode == Utf16) {
            std::string d = odd + std::string(p, n);
            odd.clear();
            if (d.size() & 1) { odd = d.substr(d.size() - 1); d.pop_back(); }
            return utf16leToUtf8((const uint8_t*)d.data(), d.size());
        }
        std::string d = odd + std::string(p, n);
        odd.clear();
        size_t high = countHighBytes(d.data(), d.size());
        if (mode == Undecided && high > 0) {
            size_t multi = 0;
            bool russian = (GetOEMCP() == 866 || GetACP() == 1251);
            if (utf8Valid(d.data(), d.size(), &multi) && multi >= 3) { if (high >= 6) mode = Utf8; }
            else if (russian) {
                // OEM (866) vs ANSI (1251): pick the decoding that looks like Russian text.
                std::wstring o = Conv(CP_OEMCP, d.data(), d.size()), a = Conv(CP_ACP, d.data(), d.size());
                int so = Score(o), sa = Score(a);
                if (high >= 8) mode = (sa > so) ? Ansi : Oem;
                return U(sa > so ? a : o);
            } else if (high >= 8) mode = Oem;
        }
        if (mode == Utf8) {
            size_t ok = utf8CompletePrefix(d.data(), d.size());
            odd = d.substr(ok);
            return d.substr(0, ok);
        }
        return U(Conv(mode == Ansi ? CP_ACP : CP_OEMCP, d.data(), d.size()));
    }
};

struct Splitter {
    std::string cur;
    bool pendingCR = false;
    const LineSink* sink;
    void emit(bool progress) {
        std::string t = trim(cur);
        cur.clear();
        if (!t.empty() && sink && *sink) (*sink)(t, progress);
    }
    void feed(const std::string& s) {
        for (char c : s) {
            if (pendingCR) {
                pendingCR = false;
                if (c == '\n') { emit(false); continue; }
                emit(true);  // lone CR = in-place progress update
            }
            if (c == '\r') pendingCR = true;
            else if (c == '\n') emit(false);
            else cur += c;
        }
    }
    void flush() { if (pendingCR) { pendingCR = false; emit(false); } else emit(false); }
};

}  // namespace

ProcResult RunProcess(const std::string& cmdline, const std::string& workDir, const LineSink& sink,
                      const std::atomic<bool>* cancel, int timeoutSec, const std::string& input) {
    ProcResult r;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wrp = nullptr;
    if (!CreatePipe(&rd, &wrp, &sa, 0)) { r.error = ErrMsg(GetLastError()); return r; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = INVALID_HANDLE_VALUE, inR = nullptr, inW = nullptr;
    bool useIn = !input.empty();
    if (useIn) {
        if (CreatePipe(&inR, &inW, &sa, 0)) SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
        else useIn = false;
    }
    if (!useIn) nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = useIn ? inR : nul;
    si.hStdOutput = wrp;
    si.hStdError = wrp;

    std::wstring wc = W(cmdline);
    std::vector<wchar_t> buf(wc.begin(), wc.end());
    buf.push_back(0);
    std::wstring wd = W(workDir.empty() ? SysDir() : workDir);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl{};
        jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof jl);
    }

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, wd.c_str(), &si, &pi);
    DWORD cpErr = GetLastError();
    CloseHandle(wrp);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (useIn) {
        CloseHandle(inR);
        if (ok) { DWORD wrote = 0; WriteFile(inW, input.data(), (DWORD)input.size(), &wrote, nullptr); }
        CloseHandle(inW);
    }
    if (!ok) {
        r.error = ErrMsg(cpErr);
        CloseHandle(rd);
        if (job) CloseHandle(job);
        return r;
    }
    r.started = true;
    bool inJob = job && AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);

    auto killTree = [&]() {
        if (inJob) TerminateJobObject(job, 1);
        else TerminateProcess(pi.hProcess, 1);
    };

    Decoder dec;
    Splitter sp;
    sp.sink = &sink;
    ULONGLONG t0 = GetTickCount64();
    char chunk[8192];
    auto pump = [&]() -> bool {  // returns true if data was read
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (!avail) return false;
        DWORD got = 0;
        if (!ReadFile(rd, chunk, (DWORD)std::min<size_t>(avail, sizeof chunk), &got, nullptr) || !got) return false;
        std::string text = dec.feed(chunk, got);
        if (r.output.size() < (8u << 20)) r.output += text;
        sp.feed(text);
        return true;
    };

    for (;;) {
        if (pump()) continue;
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            while (pump()) {}
            break;
        }
        if (cancel && cancel->load()) { killTree(); r.cancelled = true; break; }
        if (timeoutSec > 0 && GetTickCount64() - t0 > (ULONGLONG)timeoutSec * 1000ULL) { killTree(); r.timedOut = true; break; }
        Sleep(25);
    }
    sp.flush();
    if (r.cancelled || r.timedOut) WaitForSingleObject(pi.hProcess, 3000);
    GetExitCodeProcess(pi.hProcess, &r.exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (job) CloseHandle(job);  // KILL_ON_JOB_CLOSE removes stragglers
    return r;
}

}  // namespace wr
