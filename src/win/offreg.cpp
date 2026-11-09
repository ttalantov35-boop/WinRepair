#include "offreg.h"
#include "backups.h"

#include <cstring>

namespace wr {

namespace {
std::string Sanitize(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += (isalnum(c) ? (char)c : '_');
    return o;
}
std::wstring WK(const std::string& s) { return W(s); }
}  // namespace

// ---- privileges ------------------------------------------------------------------------------------------
static void EnablePriv(const wchar_t* name) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof tp, nullptr, nullptr);
    }
    CloseHandle(tok);
}

void EnablePrivilegeByName(const wchar_t* name) { EnablePriv(name); }

std::vector<std::string> ListDirs(const std::string& dir) {
    std::vector<std::string> v;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) v.push_back(U(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return v;
}

std::vector<HiveInfo> ListHives(const Target& t, bool includeUsers) {
    std::vector<HiveInfo> v;
    std::string cfg = t.winDir + "\\System32\\config\\";
    for (const char* n : {"SYSTEM", "SOFTWARE", "SAM", "SECURITY", "DEFAULT", "COMPONENTS", "DRIVERS"}) {
        if (!PathExists(cfg + n)) continue;
        HiveInfo h;
        h.label = n;
        h.file = cfg + n;
        h.mount = std::string("WRO_") + n;
        v.push_back(h);
    }
    if (includeUsers) {
        std::string users = t.root + "Users\\";
        for (const auto& u : ListDirs(users)) {
            std::string lu = toLowerUtf8(u);
            if (lu == "public" || lu == "all users" || lu == "default user") continue;
            std::string prof = users + u + "\\";
            if (PathExists(prof + "NTUSER.DAT")) {
                HiveInfo h;
                h.label = "NTUSER (" + u + ")";
                h.file = prof + "NTUSER.DAT";
                h.mount = "WRO_NU_" + Sanitize(u);
                h.owner = u; h.profileDir = prof; h.user = true;
                v.push_back(h);
            }
            std::string uc = prof + "AppData\\Local\\Microsoft\\Windows\\UsrClass.dat";
            if (PathExists(uc)) {
                HiveInfo h;
                h.label = "UsrClass (" + u + ")";
                h.file = uc;
                h.mount = "WRO_UC_" + Sanitize(u);
                h.owner = u; h.profileDir = prof; h.user = false;
                v.push_back(h);
            }
        }
    }
    return v;
}

// ---- hive manager ------------------------------------------------------------------------------------------
HiveManager& Hives() {
    static HiveManager m;
    return m;
}

std::string HiveManager::backupDir() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (backupDir_.empty()) backupDir_ = BackupDirFor("", "hives");   // before any hive is loaded: the system volume
    return backupDir_;
}

bool HiveManager::isLoaded(const std::string& mount) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    return loaded_.count(mount) != 0;
}

std::vector<std::string> HiveManager::mounts() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<std::string> v;
    for (auto& p : loaded_) v.push_back(p.first);
    return v;
}

bool HiveManager::load(const HiveInfo& h, std::string& err) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (loaded_.count(h.mount)) return true;
    EnablePriv(SE_BACKUP_NAME);
    EnablePriv(SE_RESTORE_NAME);
    backupDir_ = BackupDirFor(h.file, "hives");        // one folder per examined volume
    std::string bk = backupDir_ + h.mount + ".hiv";    // copy BEFORE the registry locks the file
    if (!BackupOncePerSession("hive:" + bk)) Logger::I().log(Lvl::Info, "Копия куста уже сохранена в этом сеансе: " + bk);
    else if (!CopyFileW(W(h.file).c_str(), W(bk).c_str(), FALSE)) Logger::I().log(Lvl::Warn, "Не удалось сделать копию куста " + h.file + ": " + ErrMsg(GetLastError()));
    else Logger::I().log(Lvl::Info, "Резервная копия куста (одна на систему, заменяет прежнюю): " + bk);
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, WK(h.mount).c_str());  // stale mount from a previous crash
    LONG r = RegLoadKeyW(HKEY_LOCAL_MACHINE, WK(h.mount).c_str(), W(h.file).c_str());
    if (r != ERROR_SUCCESS) { err = h.label + ": RegLoadKey — " + ErrMsg((DWORD)r); return false; }
    loaded_[h.mount] = h.file;
    Logger::I().log(Lvl::Info, "Загружен куст " + h.label + " -> HKLM\\" + h.mount);
    return true;
}

HKEY HiveManager::open(const std::string& mount, REGSAM acc) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, WK(mount).c_str(), 0, acc, &k) != ERROR_SUCCESS) {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, WK(mount).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return nullptr;
    }
    return k;
}

void HiveManager::unload(const std::string& mount) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!loaded_.count(mount)) return;
    LONG r = ERROR_SUCCESS;
    for (int i = 0; i < 5; ++i) {
        r = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, WK(mount).c_str());
        if (r == ERROR_SUCCESS) break;
        Sleep(200);
    }
    if (r == ERROR_SUCCESS) { loaded_.erase(mount); Logger::I().log(Lvl::Info, "Выгружен куст HKLM\\" + mount); }
    else Logger::I().log(Lvl::Warn, "Не удалось выгрузить HKLM\\" + mount + ": " + ErrMsg((DWORD)r) + " (закройте окна, использующие реестр)");
}

void HiveManager::unloadAll() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<std::string> m;
    for (auto& p : loaded_) m.push_back(p.first);
    for (auto& x : m) unload(x);
}

void HiveManager::pin() { std::lock_guard<std::recursive_mutex> g(mu_); ++pins_; }
void HiveManager::unpin() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (pins_ > 0 && --pins_ == 0) unloadAll();
}

// ---- RegCtx ------------------------------------------------------------------------------------------------
bool RegCtx::open(const Target& t, std::string& err) {
    online = t.online;
    if (online) {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_READ | KEY_WRITE, &software) != ERROR_SUCCESS ||
            RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM", 0, KEY_READ | KEY_WRITE, &system) != ERROR_SUCCESS) { err = "Не удалось открыть HKLM\\SOFTWARE/SYSTEM"; return false; }
        opened_.push_back(software); opened_.push_back(system);
        cs = "CurrentControlSet";
        UserRoot u;
        u.name = GetEnv().computer;
        u.profile = EnvVar("USERPROFILE") + "\\";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"", 0, KEY_READ | KEY_WRITE, &u.root) == ERROR_SUCCESS) { opened_.push_back(u.root); users.push_back(u); }
        return true;
    }
    Hives().pin();
    pinned_ = true;
    for (const auto& h : ListHives(t, true)) {
        bool need = h.label == "SYSTEM" || h.label == "SOFTWARE" || h.label.rfind("NTUSER", 0) == 0;
        if (!need) continue;
        std::string e;
        if (!Hives().load(h, e)) { if (h.label == "SYSTEM" || h.label == "SOFTWARE") { err = e; return false; } continue; }
        HKEY k = Hives().open(h.mount);
        if (!k) continue;
        opened_.push_back(k);
        if (h.label == "SYSTEM") system = k;
        else if (h.label == "SOFTWARE") software = k;
        else { UserRoot u; u.name = h.owner; u.profile = h.profileDir; u.root = k; users.push_back(u); }
    }
    if (!system || !software) { err = "Кусты SYSTEM/SOFTWARE не найдены в " + t.winDir; return false; }
    RegVal v;
    if (QueryVal(system, "Select", "Current", v) && v.type == REG_DWORD && v.data.size() >= 4) cs = fmt("ControlSet%03lu", (unsigned long)*(DWORD*)v.data.data());
    return true;
}

RegCtx::~RegCtx() {
    for (HKEY k : opened_) { RegFlushKey(k); RegCloseKey(k); }
    if (pinned_) Hives().unpin();
}

// ---- value helpers -----------------------------------------------------------------------------------------
bool EnumSubKeys(HKEY k, std::vector<std::string>& out) {
    DWORD n = 0, maxLen = 0;
    if (RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &n, &maxLen, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) return false;
    std::wstring buf(maxLen + 2, L'\0');
    for (DWORD i = 0;; ++i) {
        DWORD len = (DWORD)buf.size();
        LONG r = RegEnumKeyExW(k, i, &buf[0], &len, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        out.push_back(U(buf.c_str(), (int)len));
    }
    return true;
}

bool EnumValues(HKEY k, std::vector<RegVal>& out) {
    DWORD n = 0, maxName = 0, maxData = 0;
    if (RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &n, &maxName, &maxData, nullptr, nullptr) != ERROR_SUCCESS) return false;
    std::wstring nb(maxName + 2, L'\0');
    std::vector<uint8_t> db(maxData + 2);
    for (DWORD i = 0;; ++i) {
        DWORD nl = (DWORD)nb.size(), dl = (DWORD)db.size(), type = 0;
        LONG r = RegEnumValueW(k, i, &nb[0], &nl, nullptr, &type, db.data(), &dl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        RegVal v;
        v.name = U(nb.c_str(), (int)nl);
        v.type = type;
        v.data.assign(db.begin(), db.begin() + dl);
        out.push_back(std::move(v));
    }
    return true;
}

bool QueryVal(HKEY base, const std::string& sub, const std::string& name, RegVal& out) {
    HKEY k = nullptr;
    if (sub.empty()) k = base;
    else if (RegOpenKeyExW(base, W(sub).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, sz = 0;
    std::wstring wn = W(name);
    bool ok = false;
    if (RegQueryValueExW(k, wn.c_str(), nullptr, &type, nullptr, &sz) == ERROR_SUCCESS) {
        out.name = name;
        out.type = type;
        out.data.assign(sz + 2, 0);
        DWORD sz2 = sz;
        if (RegQueryValueExW(k, wn.c_str(), nullptr, &type, out.data.data(), &sz2) == ERROR_SUCCESS) { out.data.resize(sz2); ok = true; }
    }
    if (!sub.empty()) RegCloseKey(k);
    return ok;
}

std::string RegTypeName(DWORD t) {
    switch (t) {
        case REG_NONE: return "REG_NONE";
        case REG_SZ: return "REG_SZ";
        case REG_EXPAND_SZ: return "REG_EXPAND_SZ";
        case REG_BINARY: return "REG_BINARY";
        case REG_DWORD: return "REG_DWORD";
        case REG_MULTI_SZ: return "REG_MULTI_SZ";
        case REG_QWORD: return "REG_QWORD";
        default: return fmt("REG_(%lu)", (unsigned long)t);
    }
}

DWORD RegTypeFromName(const std::string& n) {
    for (DWORD t : {(DWORD)REG_SZ, (DWORD)REG_EXPAND_SZ, (DWORD)REG_BINARY, (DWORD)REG_DWORD, (DWORD)REG_MULTI_SZ, (DWORD)REG_QWORD})
        if (RegTypeName(t) == n) return t;
    return REG_SZ;
}

std::string RegText(const RegVal& v) {
    if (v.type != REG_SZ && v.type != REG_EXPAND_SZ && v.type != REG_MULTI_SZ) return "";
    const wchar_t* p = (const wchar_t*)v.data.data();
    size_t n = v.data.size() / 2;
    size_t len = 0;
    while (len < n && p[len]) ++len;
    return U(p, (int)len);
}

std::string RegValueText(const RegVal& v, size_t maxLen) {
    std::string s;
    switch (v.type) {
        case REG_SZ:
        case REG_EXPAND_SZ: s = RegText(v); break;
        case REG_DWORD: if (v.data.size() >= 4) { DWORD d = *(const DWORD*)v.data.data(); s = fmt("0x%08lX (%lu)", (unsigned long)d, (unsigned long)d); } break;
        case REG_QWORD: if (v.data.size() >= 8) { unsigned long long q = *(const unsigned long long*)v.data.data(); s = fmt("0x%llX (%llu)", q, q); } break;
        case REG_MULTI_SZ: {
            const wchar_t* p = (const wchar_t*)v.data.data();
            size_t n = v.data.size() / 2, i = 0;
            while (i < n && p[i]) {
                size_t j = i;
                while (j < n && p[j]) ++j;
                s += (s.empty() ? "" : " | ") + U(p + i, (int)(j - i));
                i = j + 1;
            }
            break;
        }
        default:
            for (size_t i = 0; i < v.data.size() && i < 64; ++i) s += fmt("%02X ", v.data[i]);
            if (v.data.size() > 64) s += fmt("… (%zu байт)", v.data.size());
    }
    return truncUtf8(s, maxLen);
}

static void PutWide(std::vector<uint8_t>& out, const std::string& s, bool term) {
    std::wstring w = W(s);
    const uint8_t* p = (const uint8_t*)w.data();
    out.insert(out.end(), p, p + w.size() * 2);
    if (term) { out.push_back(0); out.push_back(0); }
}

bool RegParse(DWORD type, const std::string& text, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    switch (type) {
        case REG_SZ:
        case REG_EXPAND_SZ: PutWide(out, text, true); return true;
        case REG_DWORD:
        case REG_QWORD: {
            std::string t = trim(text);
            if (t.empty()) { err = "Пустое число"; return false; }
            char* e = nullptr;
            int base = (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) ? 16 : 10;
            unsigned long long v = strtoull(t.c_str(), &e, base);
            if (e == t.c_str() || *e) { err = "Некорректное число (десятичное или 0x…)"; return false; }
            if (type == REG_DWORD) { if (v > 0xFFFFFFFFULL) { err = "Значение больше DWORD"; return false; } DWORD d = (DWORD)v; out.assign((uint8_t*)&d, (uint8_t*)&d + 4); }
            else out.assign((uint8_t*)&v, (uint8_t*)&v + 8);
            return true;
        }
        case REG_MULTI_SZ: {
            std::string t = text;
            for (char& c : t) if (c == '\r') c = '\n';
            size_t p = 0;
            while (p <= t.size()) {
                size_t e = t.find_first_of("\n|", p);
                if (e == std::string::npos) e = t.size();
                std::string part = trim(t.substr(p, e - p));
                if (!part.empty()) PutWide(out, part, true);
                p = e + 1;
            }
            out.push_back(0); out.push_back(0);
            return true;
        }
        default: {  // binary: hex bytes
            std::string hex;
            for (char c : text) if (isxdigit((unsigned char)c)) hex += c;
            if (hex.size() % 2) { err = "Нечётное число hex-цифр"; return false; }
            for (size_t i = 0; i < hex.size(); i += 2) out.push_back((uint8_t)strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
            return true;
        }
    }
}

LONG RegSetRaw(HKEY k, const std::string& name, DWORD type, const std::vector<uint8_t>& data) {
    return RegSetValueExW(k, W(name).c_str(), 0, type, data.data(), (DWORD)data.size());
}

LONG RegMakeKey(HKEY base, const std::string& sub) {
    HKEY k = nullptr;
    LONG r = RegCreateKeyExW(base, W(sub).c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr);
    if (r == ERROR_SUCCESS) RegCloseKey(k);
    return r;
}

static LONG SetUnder(HKEY base, const std::string& sub, const std::string& name, DWORD type, const std::vector<uint8_t>& d) {
    HKEY k = nullptr;
    LONG r = sub.empty() ? ERROR_SUCCESS : RegCreateKeyExW(base, W(sub).c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetRaw(sub.empty() ? base : k, name, type, d);
    if (k) RegCloseKey(k);
    return r;
}

LONG RegSetString(HKEY base, const std::string& sub, const std::string& name, const std::string& value, DWORD type) {
    std::vector<uint8_t> d;
    PutWide(d, value, true);
    return SetUnder(base, sub, name, type, d);
}

LONG RegSetDword(HKEY base, const std::string& sub, const std::string& name, DWORD value) {
    std::vector<uint8_t> d((uint8_t*)&value, (uint8_t*)&value + 4);
    return SetUnder(base, sub, name, REG_DWORD, d);
}

LONG RegDelValue(HKEY base, const std::string& sub, const std::string& name) {
    HKEY k = nullptr;
    if (sub.empty()) k = base;
    else { LONG r = RegOpenKeyExW(base, W(sub).c_str(), 0, KEY_SET_VALUE, &k); if (r != ERROR_SUCCESS) return r; }
    LONG r = RegDeleteValueW(k, W(name).c_str());
    if (!sub.empty()) RegCloseKey(k);
    return r;
}

LONG RegDelTree(HKEY base, const std::string& sub) { return RegDeleteTreeW(base, W(sub).c_str()); }

void ExportRegKey(HKEY k, const std::string& label, std::string& out, int depth) {
    if (depth == 0) out += "Windows Registry Editor Version 5.00\r\n\r\n";
    out += "[" + label + "]\r\n";
    std::vector<RegVal> vals;
    EnumValues(k, vals);
    auto esc = [](const std::string& s) { std::string o; for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; } return o; };
    for (const auto& v : vals) {
        std::string nm = v.name.empty() ? "@" : "\"" + esc(v.name) + "\"";
        if (v.type == REG_SZ) out += nm + "=\"" + esc(RegText(v)) + "\"\r\n";
        else if (v.type == REG_DWORD && v.data.size() >= 4) out += nm + fmt("=dword:%08lx\r\n", (unsigned long)*(const DWORD*)v.data.data());
        else {
            std::string h;
            for (size_t i = 0; i < v.data.size(); ++i) h += fmt(i ? ",%02x" : "%02x", v.data[i]);
            out += nm + (v.type == REG_BINARY ? "=hex:" : fmt("=hex(%lx):", (unsigned long)v.type)) + h + "\r\n";
        }
    }
    out += "\r\n";
    std::vector<std::string> subs;
    EnumSubKeys(k, subs);
    for (const auto& s : subs) {
        HKEY c = nullptr;
        if (RegOpenKeyExW(k, W(s).c_str(), 0, KEY_READ, &c) == ERROR_SUCCESS) { ExportRegKey(c, label + "\\" + s, out, depth + 1); RegCloseKey(c); }
    }
}

// ---- audit / file backup --------------------------------------------------------------------------------------
void AuditChange(const std::string& what) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string dir = DataDir() + "Logs\\";
    MakeDirs(dir);
    std::string path = dir + fmt("changes_%04d%02d%02d.log", t.wYear, t.wMonth, t.wDay);
    HANDLE h = CreateFileW(W(path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        if (GetLastError() != ERROR_ALREADY_EXISTS) { const unsigned char b[3] = {0xEF, 0xBB, 0xBF}; WriteFile(h, b, 3, &w, nullptr); }
        std::string l = NiceNow() + " | " + what + "\r\n";
        WriteFile(h, l.data(), (DWORD)l.size(), &w, nullptr);
        CloseHandle(h);
    }
    Logger::I().log(Lvl::Warn, "ИЗМЕНЕНИЕ: " + what);
}

std::string FileBackup(const std::string& path, const std::string& tag) {
    std::string dir = BackupDirFor(path, "files");
    std::string name = path.substr(path.find_last_of('\\') + 1);
    std::string dst = dir + tag + "_" + name;
    if (!BackupOncePerSession("file:" + dst)) return dst;   // the first copy of this session is the pre-change state; keep it
    if (!CopyFileW(W(path).c_str(), W(dst).c_str(), FALSE)) return "";
    return dst;
}

}  // namespace wr
