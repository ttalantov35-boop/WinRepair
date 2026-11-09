#include "common.h"
#include "../core/i18n.h"

#undef MessageBoxW
#undef SetWindowTextW
#undef CreateWindowExW

namespace wr {

namespace {
Dict g_dict;
std::mutex g_mu;
Lang g_lang = Lang::RU;
std::unordered_map<std::string, std::string> g_cache;

bool WideHasCyr(const std::wstring& s) { for (wchar_t c : s) if (c >= 0x0400 && c <= 0x04FF) return true; return false; }

std::string ReadIniLang(const std::string& path) {
    std::vector<uint8_t> raw;
    if (!ReadBytes(path, raw, 4096)) return "";
    std::string t((const char*)raw.data(), raw.size());
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        std::string l = trim(t.substr(p, e - p));
        p = e + 1;
        std::string ll = toLowerUtf8(l);
        if (ll.rfind("lang", 0) == 0) { size_t eq = ll.find('='); if (eq != std::string::npos) return trim(ll.substr(eq + 1)); }
    }
    return "";
}
}  // namespace

Lang CurrentLang() { return g_lang; }

std::string Tr(const std::string& ru) {
    if (g_lang != Lang::EN || !Dict::hasCyrillic(ru)) return ru;
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_cache.find(ru);
    if (it != g_cache.end()) return it->second;
    std::string r = g_dict.tr(ru);
    if (ru.size() < 600 && g_cache.size() < 30000) g_cache[ru] = r;
    return r;
}

std::wstring TrW(const std::wstring& ru) {
    if (g_lang != Lang::EN || !WideHasCyr(ru)) return ru;
    return WRaw(Tr(U(ru)));
}

const wchar_t* TrP(const wchar_t* ru) {
    if (!ru || g_lang != Lang::EN) return ru;
    static std::map<std::wstring, std::wstring> store;
    std::lock_guard<std::mutex> g(g_mu);
    auto it = store.find(ru);
    if (it == store.end()) {
        std::wstring w(ru);
        it = store.emplace(w, WideHasCyr(w) ? WRaw(g_dict.tr(U(w))) : w).first;
    }
    return it->second.c_str();
}

void InitLanguage() {
    std::string want;
    std::wstring cl = GetCommandLineW();
    size_t p = cl.find(L"/lang:");
    if (p != std::wstring::npos) want = toLowerUtf8(U(cl.substr(p + 6, 2)));
    if (want.empty()) want = ReadIniLang(ExeDir() + "WinRepair.ini");
    if (want.empty()) want = ReadIniLang(DataDir() + "settings.ini");
    if (want.empty()) want = ((GetUserDefaultUILanguage() & 0x3FF) == 0x19) ? "ru" : "en";
    g_lang = want.rfind("ru", 0) == 0 ? Lang::RU : Lang::EN;   // any other code = translated mode (embedded English + Lang\\<code>.txt)
    if (g_lang != Lang::EN) return;
    std::string code = want.substr(0, 8);
    for (char& ch : code) if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '_')) ch = '_';
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(2), RT_RCDATA);
    if (r) {
        HGLOBAL h = LoadResource(nullptr, r);
        const char* d = h ? (const char*)LockResource(h) : nullptr;
        if (d) g_dict.load(std::string(d, SizeofResource(nullptr, r)));
    }
    std::vector<uint8_t> raw;
    for (const std::string& f : {std::string("en"), code}) {   // optional overrides / other languages: Lang\\<code>.txt
        raw.clear();
        if (ReadBytes(ExeDir() + "Lang\\" + f + ".txt", raw, 8u << 20) && !raw.empty()) g_dict.load(std::string((const char*)raw.data(), raw.size()));
    }
}

void SetLanguage(Lang l) {
    g_lang = l;
    std::string body = std::string("lang=") + (l == Lang::EN ? "en" : "ru") + "\r\n";
    if (!WriteText(ExeDir() + "WinRepair.ini", body, false)) WriteText(DataDir() + "settings.ini", body, false);
}

int TrMessageBox(HWND h, const wchar_t* text, const wchar_t* caption, UINT flags) {
    std::wstring t = TrW(text ? text : L""), c = TrW(caption ? caption : L"");
    return ::MessageBoxW(h, t.c_str(), c.c_str(), flags);
}

BOOL TrSetWindowText(HWND h, const wchar_t* text) {
    if (g_lang == Lang::EN && text && wcslen(text) < 3000 && WideHasCyr(text)) { std::wstring t = TrW(text); return ::SetWindowTextW(h, t.c_str()); }
    return ::SetWindowTextW(h, text);
}

HWND TrCreateWindowEx(DWORD ex, LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y, int w, int hgt, HWND parent, HMENU menu, HINSTANCE inst, LPVOID lp) {
    if (g_lang == Lang::EN && text && text[0] && WideHasCyr(text)) { std::wstring t = TrW(text); return ::CreateWindowExW(ex, cls, t.c_str(), style, x, y, w, hgt, parent, menu, inst, lp); }
    return ::CreateWindowExW(ex, cls, text, style, x, y, w, hgt, parent, menu, inst, lp);
}

}  // namespace wr
