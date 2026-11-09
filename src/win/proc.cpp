// Child process runner: hidden console, merged stdout/stderr, OEM/UTF-16 decoding, job object, cancel/timeout.
#include "common.h"

namespace wr {

namespace {

struct Decoder {
    bool decided = false, utf16 = false;
    std::string odd;
    std::string feed(const char* p, size_t n) {
        if (!decided) { utf16 = looksLikeUtf16le((const uint8_t*)p, n); decided = true; }
        if (utf16) {
            std::string d = odd + std::string(p, n);
            odd.clear();
            if (d.size() & 1) { odd = d.substr(d.size() - 1); d.pop_back(); }
            return utf16leToUtf8((const uint8_t*)d.data(), d.size());
        }
        int wl = MultiByteToWideChar(CP_OEMCP, 0, p, (int)n, nullptr, 0);
        if (wl <= 0) return std::string(p, n);
        std::wstring w((size_t)wl, L'\0');
        MultiByteToWideChar(CP_OEMCP, 0, p, (int)n, &w[0], wl);
        return U(w);
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
