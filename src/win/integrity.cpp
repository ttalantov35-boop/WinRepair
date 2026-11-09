#include "integrity.h"
#include "../core/sha256.h"
#include "../core/textutil.h"
#include <cstring>
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>

namespace wr {

std::string FileSha256(const std::string& path) {
    HANDLE h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    Sha256 s; std::vector<char> buf(1 << 16); DWORD n = 0; bool ok = true;
    for (;;) {
        if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &n, nullptr)) { ok = false; break; }
        if (!n) break;
        s.update(buf.data(), n);
    }
    CloseHandle(h);
    return ok ? s.hex() : "";
}

static SigState CheckSignature(const std::wstring& file) {
    // wintrust.dll is loaded dynamically: a missing/trimmed WinPE must not stop the program from starting.
    HMODULE m = LoadLibraryW(L"wintrust.dll");
    if (!m) return SigState::Unknown;
    typedef LONG(WINAPI * WVT)(HWND, GUID*, LPVOID);
    WVT fn = (WVT)GetProcAddress(m, "WinVerifyTrust");
    if (!fn) { FreeLibrary(m); return SigState::Unknown; }
    WINTRUST_FILE_INFO fi{}; fi.cbStruct = sizeof fi; fi.pcwszFilePath = file.c_str();
    WINTRUST_DATA wd{}; wd.cbStruct = sizeof wd; wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE; wd.pFile = &fi; wd.dwStateAction = WTD_STATEACTION_VERIFY; wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID act = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG r = fn(nullptr, &act, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE; fn(nullptr, &act, &wd);
    FreeLibrary(m);
    if (r == 0) return SigState::Valid;
    if ((DWORD)r == TRUST_E_NOSIGNATURE || (DWORD)r == TRUST_E_PROVIDER_UNKNOWN || (DWORD)r == TRUST_E_SUBJECT_FORM_UNKNOWN) return SigState::Unsigned;
    // Untrusted root in WinPE (no cert store) is not tampering: report Unknown unless the hash itself is broken.
    if ((DWORD)r == CERT_E_UNTRUSTEDROOT || (DWORD)r == CERT_E_CHAINING || (DWORD)r == CRYPT_E_NOT_FOUND) return SigState::Unknown;
    if ((DWORD)r == TRUST_E_BAD_DIGEST || (DWORD)r == TRUST_E_SUBJECT_NOT_TRUSTED) return SigState::Invalid;
    return SigState::Unknown;
}

IntegrityReport RunIntegrityCheck() {
    IntegrityReport r;
    wchar_t self[MAX_PATH * 2]; DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    std::string dir = ExeDir(), selfPath = n ? U(self) : "";
    std::string selfName = selfPath.substr(selfPath.find_last_of("\\/") == std::string::npos ? 0 : selfPath.find_last_of("\\/") + 1);

    if (n) r.sig = CheckSignature(self);

    std::string man;
    {   // MANIFEST.sha256 next to the exe
        HANDLE h = CreateFileW(W(dir + "MANIFEST.sha256").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(h, nullptr), rd = 0;
            if (sz != INVALID_FILE_SIZE && sz < (4u << 20)) { man.resize(sz); if (!ReadFile(h, &man[0], sz, &rd, nullptr)) rd = 0; man.resize(rd); }
            CloseHandle(h);
            r.manifestFound = !man.empty();
        }
    }
    if (!r.manifestFound) return r;
    if (man.size() >= 3 && (uint8_t)man[0] == 0xEF && (uint8_t)man[1] == 0xBB && (uint8_t)man[2] == 0xBF) man.erase(0, 3);

    // walk lines: every listed file is checked (paths relative to the exe folder, '/' or '\')
    size_t pos = 0;
    while (pos < man.size()) {
        size_t e = man.find('\n', pos); if (e == std::string::npos) e = man.size();
        std::string ln = man.substr(pos, e - pos); pos = e + 1;
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
        if (ln.size() < 66) continue;
        size_t k = 64; while (k < ln.size() && (ln[k] == ' ' || ln[k] == '*')) k++;
        std::string rel = ln.substr(k);
        if (rel.find("..") != std::string::npos || rel.find(':') != std::string::npos || rel.empty()) continue;   // no path escapes
        std::string want = ManifestLookup(ln + "\n", rel), full = dir, relw = rel;
        for (auto& c : relw) if (c == '/') c = '\\';
        full += relw;
        std::string got = FileSha256(full);
        bool isSelf = _stricmp(relw.c_str(), selfName.c_str()) == 0;
        if (got.empty()) { if (!isSelf) r.missing.push_back(rel); continue; }
        r.checked++;
        if (isSelf) { r.selfListed = true; r.selfHash = got; r.selfOk = (got == want); }
        if (got != want) {
            std::string ext = relw.size() > 4 ? relw.substr(relw.size() - 4) : relw;
            for (auto& c : ext) if (c >= 'A' && c <= 'Z') c += 32;
            (ext == ".exe" || ext == ".dll" || ext == ".sys" ? r.mismatched : r.modifiedData).push_back(rel);
        }
    }
    return r;
}

std::string IntegritySummary(const IntegrityReport& r) {
    std::string s = "Целостность: ";
    if (!r.manifestFound) s += "MANIFEST.sha256 не найден (проверка файлов пропущена)";
    else {
        s += fmt("проверено файлов: %d, изменённых программ: %d, изменённых данных: %d, отсутствующих: %d", r.checked, (int)r.mismatched.size(), (int)r.modifiedData.size(), (int)r.missing.size());
        if (r.selfListed) s += r.selfOk ? "; сам exe — совпадает" : "; сам exe — НЕ совпадает";
    }
    s += "; подпись: ";
    s += r.sig == SigState::Valid ? "действительна" : r.sig == SigState::Unsigned ? "нет" : r.sig == SigState::Invalid ? "НАРУШЕНА" : "не удалось проверить";
    return s;
}

}  // namespace wr
