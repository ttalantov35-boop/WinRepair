#include "apps.h"

namespace wr {
namespace {

std::string Str(const std::map<std::string, RegVal>& m, const char* n) {
    auto it = m.find(toLowerUtf8(n));
    if (it == m.end()) return "";
    if (it->second.type == REG_SZ || it->second.type == REG_EXPAND_SZ) return trim(RegText(it->second));
    return "";
}
DWORD Dw(const std::map<std::string, RegVal>& m, const char* n) {
    auto it = m.find(toLowerUtf8(n));
    if (it == m.end() || it->second.type != REG_DWORD || it->second.data.size() < 4) return 0;
    return *(const DWORD*)it->second.data.data();
}

std::string StripQuotes(std::string s) {
    s = trim(s);
    if (!s.empty() && s[0] == '"') { size_t e = s.find('"', 1); s = e == std::string::npos ? s.substr(1) : s.substr(1, e - 1); }
    return s;
}

std::string DirOfExe(const std::string& cmd) {   // folder of the program named in an uninstall string / icon path
    std::string c = trim(cmd);
    std::string exe;
    if (!c.empty() && c[0] == '"') exe = StripQuotes(c);
    else {
        std::string l = toLowerUtf8(c);
        size_t e = l.find(".exe");
        exe = e == std::string::npos ? c : c.substr(0, e + 4);
    }
    size_t k = exe.find_last_of("\\/");
    return k == std::string::npos ? "" : exe.substr(0, k);
}

void ScanKey(std::vector<AppItem>& out, const Target& t, HKEY base, const std::string& sub, const std::string& user, const std::string& profile, const std::string& source) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(base, W(sub).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    std::vector<std::string> keys;
    EnumSubKeys(k, keys);
    RegCloseKey(k);
    for (const auto& kn : keys) {
        HKEY ak = nullptr;
        if (RegOpenKeyExW(base, W(sub + "\\" + kn).c_str(), 0, KEY_READ, &ak) != ERROR_SUCCESS) continue;
        std::vector<RegVal> vals;
        EnumValues(ak, vals);
        RegCloseKey(ak);
        std::map<std::string, RegVal> m;
        for (auto& v : vals) m[toLowerUtf8(v.name)] = v;
        AppItem a;
        a.name = Str(m, "DisplayName");
        if (a.name.empty()) continue;
        a.publisher = Str(m, "Publisher");
        a.version = Str(m, "DisplayVersion");
        a.date = Str(m, "InstallDate");
        if (a.date.size() == 8) a.date = a.date.substr(0, 4) + "-" + a.date.substr(4, 2) + "-" + a.date.substr(6, 2);
        a.uninstall = Str(m, "QuietUninstallString");
        if (a.uninstall.empty()) a.uninstall = Str(m, "UninstallString");
        a.sizeKB = Dw(m, "EstimatedSize");
        a.msi = Dw(m, "WindowsInstaller") == 1 || toLowerUtf8(a.uninstall).find("msiexec") != std::string::npos;
        a.system = Dw(m, "SystemComponent") == 1;
        std::string rel = toLowerUtf8(Str(m, "ReleaseType"));
        a.update = !Str(m, "ParentKeyName").empty() || rel == "update" || rel == "hotfix" || rel == "security update" || rel == "service pack" ||
                   (a.name.size() > 2 && a.name[0] == 'K' && a.name[1] == 'B') || toLowerUtf8(a.name).rfind("update for", 0) == 0 || toLowerUtf8(a.name).rfind("security update", 0) == 0;
        std::string loc = Str(m, "InstallLocation");
        if (loc.empty()) {   // derive from the icon or the uninstaller, but never from system folders
            std::string d = DirOfExe(Str(m, "DisplayIcon"));
            if (d.empty() && !a.msi) d = DirOfExe(a.uninstall);
            std::string l = toLowerUtf8(d);
            if (!d.empty() && l.find("\\windows\\") == std::string::npos && l.find("\\installer") == std::string::npos && l.find("system32") == std::string::npos) loc = d;
        }
        loc = StripQuotes(loc);
        while (loc.size() > 3 && (loc.back() == '\\' || loc.back() == '/')) loc.pop_back();
        if (!loc.empty()) {
            a.location = ResolveOfflinePath(loc, t, profile);
            a.dirExists = IsDir(a.location);
        }
        a.user = user;
        a.sub = sub + "\\" + kn;
        a.source = source;
        out.push_back(a);
    }
}

std::string Lower(const std::string& s) { return toLowerUtf8(s); }

}  // namespace

std::vector<AppItem> ScanApps(const Target& t, std::string& err) {
    std::vector<AppItem> out;
    RegCtx rc;
    if (!rc.open(t, err)) return out;
    const std::string un = "Microsoft\\Windows\\CurrentVersion\\Uninstall";
    ScanKey(out, t, rc.software, un, "", "", "HKLM");
    ScanKey(out, t, rc.software, "WOW6432Node\\" + un, "", "", "HKLM (32-бит)");
    for (const auto& u : rc.users) ScanKey(out, t, u.root, "Software\\" + un, u.name, u.profile, "Пользователь " + u.name);
    std::sort(out.begin(), out.end(), [](const AppItem& a, const AppItem& b) { return Lower(a.name) < Lower(b.name); });
    return out;
}

std::vector<AutoItem> RelatedStartup(const AppItem& a, const std::vector<AutoItem>& all) {
    std::vector<AutoItem> r;
    std::string loc = Lower(a.location);
    std::string token;   // first meaningful word of the program name, for services/drivers installed outside the program folder
    {
        std::string n = Lower(a.name);
        size_t e = 0;
        while (e < n.size() && (isalnum((unsigned char)n[e]) || (unsigned char)n[e] >= 0x80)) ++e;
        if (e >= 5) token = n.substr(0, e);
    }
    for (const auto& it : all) {
        if (it.op == AOp::ReadOnly) continue;
        std::string f = Lower(it.file.empty() ? it.command : it.file), c = Lower(it.command);
        bool byPath = loc.size() > 8 && (f.find(loc) != std::string::npos || c.find(loc) != std::string::npos);
        bool byName = !token.empty() && it.op == AOp::Service && Lower(it.name).find(token) != std::string::npos;
        if (byPath || byName) r.push_back(it);
    }
    return r;
}

bool AppFolderSafe(const Target& t, const std::string& dir, std::string& why) {
    std::string d = Lower(dir);
    while (d.size() > 3 && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    std::string root = Lower(t.root.substr(0, 2));
    if (d.size() < 4 || d.rfind(root, 0) != 0) { why = "Папка не находится на диске целевой системы."; return false; }
    int depth = 0;
    for (char c : d) if (c == '\\') ++depth;
    if (depth < 2) { why = "Слишком короткий путь — похоже на корневую или системную папку."; return false; }
    const char* banned[] = {"\\windows", "\\program files", "\\program files (x86)", "\\programdata", "\\users", "\\documents and settings", "\\system volume information", "\\recovery"};
    for (const char* b : banned) {
        std::string full = root + b;
        if (d == full) { why = "Это общая системная папка, а не папка программы."; return false; }
    }
    if (d.rfind(Lower(t.winDir), 0) == 0) { why = "Папка внутри каталога Windows — системные файлы удалять нельзя."; return false; }
    return true;
}

namespace {
std::string Sanitize(std::string s) {
    for (char& c : s) if (!(isalnum((unsigned char)c) || (unsigned char)c >= 0x80 || c == '.' || c == '-' || c == '_' || c == ' ')) c = '_';
    if (s.size() > 60) s.resize(60);
    return s;
}
bool TakeOwn(const std::string& dir) {
    std::string a = "\"" + SysTool("takeown.exe") + "\" /f \"" + dir + "\" /r /d y";
    std::string b = "\"" + SysTool("icacls.exe") + "\" \"" + dir + "\" /grant *S-1-5-32-544:(OI)(CI)F /t /c /q";
    std::atomic<bool> nc{false};
    RunProcess(a, "", nullptr, &nc, 600);
    ProcResult r = RunProcess(b, "", nullptr, &nc, 600);
    return r.started;
}
}  // namespace

bool AppDeleteEntry(const Target& t, const AppItem& a, std::string& err) {
    RegCtx rc;
    if (!rc.open(t, err)) return false;
    HKEY base = rc.software;
    if (!a.user.empty()) {
        base = nullptr;
        for (const auto& u : rc.users) if (u.name == a.user) base = u.root;
        if (!base) { err = "Куст пользователя не найден"; return false; }
    }
    HKEY k = nullptr;
    if (RegOpenKeyExW(base, W(a.sub).c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) {   // .reg copy first
        std::string reg, f;
        ExportRegKey(k, "HKEY_LOCAL_MACHINE\\" + a.sub, reg);
        RegCloseKey(k);
        BackupRegText(t.root, "apps_entry", reg, f);
    }
    LONG r = RegDelTree(base, a.sub);
    if (r != ERROR_SUCCESS) { err = ErrMsg((DWORD)r); return false; }
    AuditChange("APP ENTRY DELETED: " + a.name + " (" + a.sub + ")");
    return true;
}

bool AppRemove(const Target& t, const AppItem& a, const RemovePlan& plan, const std::vector<AutoItem>& related, std::vector<std::string>& log) {
    bool ok = true;
    if (plan.disableStartup) {
        for (const auto& it : related) {
            if (it.state == "Отключено") continue;
            std::string err;
            if (AutorunSetEnabled(it, false, err)) { log.push_back("Отключено: [" + it.kind + "] " + it.name); AuditChange("APP REMOVE: disabled " + it.kind + " " + it.name + " of " + a.name); }
            else { log.push_back("НЕ УДАЛОСЬ отключить: [" + it.kind + "] " + it.name + " — " + err); ok = false; }
        }
    }
    if (plan.files != 0 && !a.location.empty() && IsDir(a.location)) {
        std::string why;
        if (!AppFolderSafe(t, a.location, why)) { log.push_back("Файлы не тронуты: " + why + " (" + a.location + ")"); ok = false; }
        else if (plan.files == 1) {
            std::string qdir = t.root + "WinRepair_Removed\\" + Stamp() + "\\";
            MakeDirs(qdir);
            std::string dest = qdir + Sanitize(a.name);
            if (!MoveFileExW(W(a.location).c_str(), W(dest).c_str(), MOVEFILE_COPY_ALLOWED)) {
                TakeOwn(a.location);
                if (!MoveFileExW(W(a.location).c_str(), W(dest).c_str(), MOVEFILE_COPY_ALLOWED)) {
                    log.push_back("НЕ УДАЛОСЬ переместить папку: " + a.location + " — " + ErrMsg(GetLastError()));
                    ok = false;
                    goto entry;
                }
            }
            log.push_back("Папка перемещена в карантин (можно вернуть): " + dest);
            AuditChange("APP REMOVE: folder " + a.location + " -> " + dest);
        } else {
            TakeOwn(a.location);
            std::atomic<bool> nc{false};
            RunProcess("\"" + SysTool("cmd.exe") + "\" /c rd /s /q \"" + a.location + "\"", "", nullptr, &nc, 900);
            if (IsDir(a.location)) { log.push_back("Папка удалена не полностью: " + a.location); ok = false; }
            else { log.push_back("Папка удалена: " + a.location); AuditChange("APP REMOVE: folder deleted " + a.location); }
        }
    } else if (plan.files != 0) log.push_back("Папка программы не найдена — файлы не тронуты.");
entry:
    if (plan.deleteEntry) {
        std::string err;
        if (AppDeleteEntry(t, a, err)) log.push_back("Запись удалена из списка программ (копия .reg сохранена).");
        else { log.push_back("НЕ УДАЛОСЬ удалить запись: " + err); ok = false; }
    }
    return ok;
}

std::string AppsCsv(const std::vector<AppItem>& v) {
    auto q = [](const std::string& s) { std::string o = "\""; for (char c : s) { if (c == '"') o += '"'; o += c; } return o + "\""; };
    std::string s = "Название,Издатель,Версия,Дата,Размер КБ,Папка,Источник,Команда удаления\r\n";
    for (const auto& a : v) s += q(a.name) + "," + q(a.publisher) + "," + q(a.version) + "," + q(a.date) + "," + std::to_string(a.sizeKB) + "," + q(a.location) + "," + q(a.source) + "," + q(a.uninstall) + "\r\n";
    return s;
}

}  // namespace wr
