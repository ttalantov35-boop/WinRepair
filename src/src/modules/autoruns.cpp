#include "autoruns.h"

#include <cstring>

namespace wr {

namespace {

bool ReplaceCI(std::string& s, const std::string& what, const std::string& with) {
    std::string ls = toLowerUtf8(s), lw = toLowerUtf8(what);
    size_t p = ls.find(lw);
    if (p == std::string::npos) return false;
    s.replace(p, what.size(), with);
    return true;
}

std::string ExtractExe(const std::string& cmd) {
    std::string c = trim(cmd);
    if (c.empty()) return "";
    if (c[0] == '"') { size_t e = c.find('"', 1); return e == std::string::npos ? c.substr(1) : c.substr(1, e - 1); }
    std::string l = toLowerUtf8(c);
    size_t best = std::string::npos;
    for (const char* ext : {".exe", ".dll", ".sys", ".bat", ".cmd", ".com", ".scr", ".msc", ".cpl", ".vbs", ".js", ".lnk", ".ocx"}) {
        size_t p = l.find(ext);
        while (p != std::string::npos) {
            size_t end = p + strlen(ext);
            if (end >= l.size() || l[end] == ' ' || l[end] == ',' || l[end] == '"') { if (best == std::string::npos || end < best) best = end; break; }
            p = l.find(ext, p + 1);
        }
    }
    if (best != std::string::npos) return c.substr(0, best);
    size_t sp = c.find(' ');
    return sp == std::string::npos ? c : c.substr(0, sp);
}

bool IsSystemPath(const std::string& cmd) {
    std::string l = toLowerUtf8(cmd);
    return containsAny(l, {"\\windows\\system32\\", "%systemroot%\\system32", "\\systemroot\\system32", "system32\\drivers\\", "\\windows\\syswow64\\",
                           "%windir%\\system32", "\\windows\\microsoft.net\\", "\\windows defender\\", "\\windows\\servicing\\"}) ||
           l.rfind("system32\\", 0) == 0;
}

std::string Flag(const AutoItem& it) {
    std::string l = toLowerUtf8(it.command);
    if (!it.file.empty() && !it.fileExists && it.op != AOp::EditOnly) return "ФАЙЛ НЕ НАЙДЕН";
    if (containsAny(l, {"mshta", "wscript", "cscript", "powershell", "-enc", "http://", "https://", "javascript:", "cmd /c", "cmd.exe /c", "\\users\\public\\"})) return "ПОДОЗРИТЕЛЬНО";
    if (containsAny(l, {"\\appdata\\", "\\temp\\", "\\programdata\\"}) ) return "проверить";
    return "";
}

void Finish(AutoItem& it, const Target& t, const std::string& profile) {
    std::string exe = ExtractExe(it.command);
    it.file = exe.empty() ? "" : ResolveOfflinePath(exe, t, profile);
    it.fileExists = !it.file.empty() && PathExists(it.file);
    it.flag = Flag(it);
}

bool ApprovedDisabled(HKEY root, const std::string& approvedSub, const std::string& name) {
    if (approvedSub.empty()) return false;
    RegVal v;
    if (!QueryVal(root, approvedSub, name, v) || v.data.empty()) return false;
    return (v.data[0] & 1) != 0;
}

void AddRun(std::vector<AutoItem>& out, const Target& t, const std::string& mount, HKEY root, const std::string& sub, const std::string& kind,
            const std::string& approved, bool once, const std::string& profile) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, W(sub).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    std::vector<RegVal> vals;
    EnumValues(k, vals);
    RegCloseKey(k);
    for (const auto& v : vals) {
        if (v.type != REG_SZ && v.type != REG_EXPAND_SZ) continue;
        AutoItem it;
        it.kind = kind; it.name = v.name; it.command = RegText(v); it.location = (mount.rfind("@HKU\\", 0) == 0 ? "HKU\\" + mount.substr(5) : "HKLM\\" + mount) + "\\" + sub; it.once = once;
        it.op = AOp::RunValue; it.mount = mount; it.sub = sub; it.value = v.name; it.approvedSub = once ? "" : approved;
        it.state = ApprovedDisabled(root, approved, v.name) ? "Отключено" : "Включено";
        Finish(it, t, profile);
        out.push_back(it);
    }
}

}  // namespace

std::string ResolveOfflinePath(const std::string& cmdIn, const Target& t, const std::string& profile) {
    std::string p = trim(cmdIn);
    if (p.rfind("\\??\\", 0) == 0) p = p.substr(4);
    std::string rootNoSlash = t.root.substr(0, 2);
    ReplaceCI(p, "\\SystemRoot", t.winDir);
    while (ReplaceCI(p, "%SystemRoot%", t.winDir)) {}
    while (ReplaceCI(p, "%windir%", t.winDir)) {}
    while (ReplaceCI(p, "%SystemDrive%", rootNoSlash)) {}
    while (ReplaceCI(p, "%ProgramFiles(x86)%", t.root + "Program Files (x86)")) {}
    while (ReplaceCI(p, "%ProgramFiles%", t.root + "Program Files")) {}
    while (ReplaceCI(p, "%ProgramData%", t.root + "ProgramData")) {}
    if (!profile.empty()) {
        std::string pr = profile.back() == '\\' ? profile.substr(0, profile.size() - 1) : profile;
        while (ReplaceCI(p, "%UserProfile%", pr)) {}
        while (ReplaceCI(p, "%LocalAppData%", pr + "\\AppData\\Local")) {}
        while (ReplaceCI(p, "%AppData%", pr + "\\AppData\\Roaming")) {}
    }
    std::string l = toLowerUtf8(p);
    if (l.rfind("system32\\", 0) == 0 || l.rfind("syswow64\\", 0) == 0) p = t.winDir + "\\" + p;
    else if (p.size() >= 3 && p[1] == ':' && p[2] == '\\') {
        // registry paths refer to the offline OS's own drive letters (normally C:)
        if (toupper((unsigned char)p[0]) != toupper((unsigned char)t.root[0]) && toupper((unsigned char)p[0]) == 'C') p[0] = t.root[0];
    } else if (!p.empty() && p[0] == '\\') {
        p = t.root.substr(0, 2) + p;
    }
    return p;
}

std::vector<AutoItem> ScanAutoruns(const Target& t, std::string& err) {
    std::vector<AutoItem> out;
    RegCtx rc;
    if (!rc.open(t, err)) return out;
    const std::string cv = "Microsoft\\Windows\\CurrentVersion\\";
    const std::string apr = cv + "Explorer\\StartupApproved\\";
    std::string mSoft = HiveMountName(t, "SOFTWARE");
    std::string mSys = HiveMountName(t, "SYSTEM");

    AddRun(out, t, mSoft, rc.software, cv + "Run", "Run (HKLM)", apr + "Run", false, "");
    AddRun(out, t, mSoft, rc.software, cv + "RunOnce", "RunOnce (HKLM)", "", true, "");
    AddRun(out, t, mSoft, rc.software, cv + "Policies\\Explorer\\Run", "Policies\\Explorer\\Run", "", true, "");
    AddRun(out, t, mSoft, rc.software, "WOW6432Node\\" + cv + "Run", "Run (HKLM, 32-бит)", apr + "Run32", false, "");
    AddRun(out, t, mSoft, rc.software, "WOW6432Node\\" + cv + "RunOnce", "RunOnce (HKLM, 32-бит)", "", true, "");
    for (const auto& u : rc.users) {
        std::string mu = u.mount.empty() ? UserHiveMountName(t, u.name) : u.mount;
        std::string ucv = "Software\\" + cv;
        std::string uapr = "Software\\" + apr;
        AddRun(out, t, mu, u.root, ucv + "Run", "Run (" + u.name + ")", uapr + "Run", false, u.profile);
        AddRun(out, t, mu, u.root, ucv + "RunOnce", "RunOnce (" + u.name + ")", "", true, u.profile);
        AddRun(out, t, mu, u.root, ucv + "Policies\\Explorer\\Run", "Policies\\Explorer\\Run (" + u.name + ")", "", true, u.profile);
        // Startup folder (per user)
        std::string sf = u.profile + "AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\";
        for (const auto& f : ListFiles(sf, "*")) {
            if (toLowerUtf8(f) == "desktop.ini") continue;
            AutoItem it;
            it.kind = "Папка «Автозагрузка» (" + u.name + ")"; it.name = f; it.command = sf + f; it.location = sf; it.op = AOp::StartupFile;
            it.mount = mu; it.value = f; it.path = sf + f; it.approvedSub = "Software\\" + apr + "StartupFolder";
            it.state = ApprovedDisabled(u.root, it.approvedSub, f) ? "Отключено" : "Включено";
            it.file = it.path; it.fileExists = true; it.flag = Flag(it);
            out.push_back(it);
        }
    }
    {   // common Startup folder
        std::string sf = t.root + "ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\StartUp\\";
        for (const auto& f : ListFiles(sf, "*")) {
            if (toLowerUtf8(f) == "desktop.ini") continue;
            AutoItem it;
            it.kind = "Папка «Автозагрузка» (все)"; it.name = f; it.command = sf + f; it.location = sf; it.op = AOp::StartupFile;
            it.mount = mSoft; it.value = f; it.path = sf + f; it.approvedSub = apr + "StartupFolder";
            it.state = ApprovedDisabled(rc.software, it.approvedSub, f) ? "Отключено" : "Включено";
            it.file = it.path; it.fileExists = true; it.flag = Flag(it);
            out.push_back(it);
        }
    }
    // Winlogon
    {
        std::string sub = "Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
        for (const char* n : {"Shell", "Userinit", "Taskman", "AppSetup", "VmApplet", "GinaDLL"}) {
            RegVal v;
            if (!QueryVal(rc.software, sub, n, v)) continue;
            AutoItem it;
            it.kind = "Winlogon"; it.name = n; it.command = RegText(v); it.location = "HKLM\\" + mSoft + "\\" + sub; it.op = AOp::EditOnly;
            it.mount = mSoft; it.sub = sub; it.value = n; it.state = "Включено";
            Finish(it, t, "");
            std::string lc = toLowerUtf8(it.command);
            if (it.name == "Shell" && lc != "explorer.exe") it.flag = "ПОДОЗРИТЕЛЬНО (ожидается explorer.exe)";
            if (it.name == "Userinit" && lc.find("userinit.exe") == std::string::npos) it.flag = "ПОДОЗРИТЕЛЬНО";
            out.push_back(it);
        }
    }
    // IFEO Debugger
    {
        std::string sub = "Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";
        HKEY k = nullptr;
        if (RegOpenKeyExW(rc.software, W(sub).c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) {
            std::vector<std::string> subs;
            EnumSubKeys(k, subs);
            for (const auto& s : subs) {
                RegVal v;
                bool dis = false;
                if (!QueryVal(k, s, "Debugger", v)) { if (!QueryVal(k, s, "Debugger.WRDisabled", v)) continue; dis = true; }
                AutoItem it;
                it.kind = "Image File Execution Options"; it.name = s; it.command = RegText(v); it.location = "HKLM\\" + mSoft + "\\" + sub + "\\" + s;
                it.op = AOp::Ifeo; it.mount = mSoft; it.sub = sub + "\\" + s; it.state = dis ? "Отключено" : "Включено";
                Finish(it, t, "");
                if (it.flag.empty() && !dis) it.flag = "проверить (подмена запуска программы)";
                out.push_back(it);
            }
            RegCloseKey(k);
        }
    }
    // AppInit_DLLs
    for (const char* base : {"Microsoft\\Windows NT\\CurrentVersion\\Windows", "WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows"}) {
        RegVal v;
        bool dis = false;
        if (!QueryVal(rc.software, base, "AppInit_DLLs", v) || trim(RegText(v)).empty()) { if (!QueryVal(rc.software, base, "AppInit_DLLs.WRDisabled", v)) continue; dis = true; }
        if (trim(RegText(v)).empty()) continue;
        AutoItem it;
        it.kind = "AppInit_DLLs"; it.name = "AppInit_DLLs"; it.command = RegText(v); it.location = "HKLM\\" + mSoft + "\\" + base; it.op = AOp::MoveValue;
        it.mount = mSoft; it.sub = base; it.value = "AppInit_DLLs"; it.state = dis ? "Отключено" : "Включено";
        Finish(it, t, "");
        out.push_back(it);
    }
    // Session Manager execute lists
    {
        std::string sub = rc.cs + "\\Control\\Session Manager";
        for (const char* n : {"BootExecute", "SetupExecute", "PlatformExecute"}) {
            RegVal v;
            if (!QueryVal(rc.system, sub, n, v)) continue;
            std::string txt = RegValueText(v);
            if (txt.empty() || toLowerUtf8(txt) == "autocheck autochk *") continue;
            AutoItem it;
            it.kind = "Session Manager"; it.name = n; it.command = txt; it.location = "HKLM\\" + mSys + "\\" + sub; it.op = AOp::EditOnly; it.mount = mSys; it.sub = sub; it.value = n;
            it.state = "Включено"; it.flag = "проверить (по умолчанию: autocheck autochk *)";
            out.push_back(it);
        }
    }
    // Services / drivers
    {
        std::string sub = rc.cs + "\\Services";
        HKEY k = nullptr;
        if (RegOpenKeyExW(rc.system, W(sub).c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) {
            std::vector<std::string> names;
            EnumSubKeys(k, names);
            for (const auto& n : names) {
                RegVal st, ty, ip, dn, bk;
                if (!QueryVal(k, n, "Start", st) || st.data.size() < 4) continue;
                DWORD start = *(DWORD*)st.data.data();
                bool hasBk = QueryVal(k, n, "WRStartBackup", bk);
                if (start > 2 && !hasBk) continue;
                DWORD type = (QueryVal(k, n, "Type", ty) && ty.data.size() >= 4) ? *(DWORD*)ty.data.data() : 0;
                QueryVal(k, n, "ImagePath", ip);
                QueryVal(k, n, "DisplayName", dn);
                AutoItem it;
                it.kind = (type & 3) ? "Драйвер" : "Служба";
                it.name = n;
                std::string disp = RegText(dn);
                if (!disp.empty() && disp[0] != '@') it.name = n + " (" + disp + ")";
                it.command = RegText(ip);
                it.location = "HKLM\\" + mSys + "\\" + sub + "\\" + n; it.op = AOp::Service; it.mount = mSys; it.sub = sub + "\\" + n; it.value = n;
                it.origStart = start;
                static const char* sn[] = {"Boot", "System", "Авто", "Вручную", "Отключено"};
                it.state = start == 4 ? "Отключено" : std::string("Включено (") + sn[start > 4 ? 4 : start] + ")";
                if (it.command.empty() && (type & 3)) it.command = "System32\\drivers\\" + n + ".sys";
                Finish(it, t, "");
                it.thirdParty = !IsSystemPath(it.command) || (it.fileExists == false && !it.file.empty());
                out.push_back(it);
            }
            RegCloseKey(k);
        }
    }
    // Scheduled tasks created outside Microsoft\Windows (view only)
    {
        std::string td = t.winDir + "\\System32\\Tasks\\";
        for (const auto& f : ListFiles(td, "*")) {
            std::vector<uint8_t> raw;
            if (!ReadBytes(td + f, raw, 1 << 20) || raw.empty()) continue;
            std::string x = (raw.size() > 2 && raw[0] == 0xFF && raw[1] == 0xFE) ? utf16leToUtf8(raw.data(), raw.size()) : std::string((const char*)raw.data(), raw.size());
            size_t a = x.find("<Command>"), b = x.find("</Command>");
            if (a == std::string::npos || b == std::string::npos || b < a) continue;
            AutoItem it;
            it.kind = "Планировщик (только просмотр)"; it.name = f; it.command = trim(x.substr(a + 9, b - a - 9)); it.location = td + f; it.op = AOp::ReadOnly;
            it.state = x.find("<Enabled>false</Enabled>") != std::string::npos ? "Отключено" : "Включено";
            Finish(it, t, "");
            out.push_back(it);
        }
        for (const auto& d : ListDirs(td)) {
            if (toLowerUtf8(d) == "microsoft") continue;
            for (const auto& f : ListFiles(td + d + "\\", "*")) {
                std::vector<uint8_t> raw;
                if (!ReadBytes(td + d + "\\" + f, raw, 1 << 20) || raw.empty()) continue;
                std::string x = (raw.size() > 2 && raw[0] == 0xFF && raw[1] == 0xFE) ? utf16leToUtf8(raw.data(), raw.size()) : std::string((const char*)raw.data(), raw.size());
                size_t a = x.find("<Command>"), b = x.find("</Command>");
                if (a == std::string::npos || b == std::string::npos || b < a) continue;
                AutoItem it;
                it.kind = "Планировщик (только просмотр)"; it.name = d + "\\" + f; it.command = trim(x.substr(a + 9, b - a - 9)); it.location = td + d + "\\" + f; it.op = AOp::ReadOnly;
                it.state = x.find("<Enabled>false</Enabled>") != std::string::npos ? "Отключено" : "Включено";
                Finish(it, t, "");
                out.push_back(it);
            }
        }
    }
    return out;  // hives stay loaded while the owning window holds its pin
}

// ---- modifications ------------------------------------------------------------------------------------------
static bool WithKey(const AutoItem& it, const std::function<bool(HKEY)>& f, std::string& err) {
    HKEY root = Hives().open(it.mount);
    if (!root) { err = "Куст " + it.mount + " не загружен"; return false; }
    bool ok = f(root);
    RegFlushKey(root);
    RegCloseKey(root);
    return ok;
}

bool AutorunSetEnabled(const AutoItem& it, bool enable, std::string& err) {
    auto fail = [&](LONG r) { err = ErrMsg((DWORD)r); return false; };
    switch (it.op) {
        case AOp::RunValue:
        case AOp::StartupFile: {
            if (it.approvedSub.empty()) { err = "Для RunOnce отключение не поддерживается — используйте «Удалить»."; return false; }
            std::vector<uint8_t> d(12, 0);
            d[0] = enable ? 0x02 : 0x03;
            if (!enable) { FILETIME ft; GetSystemTimeAsFileTime(&ft); memcpy(&d[4], &ft, 8); }
            return WithKey(it, [&](HKEY r) {
                HKEY k = nullptr;
                LONG x = RegCreateKeyExW(r, W(it.approvedSub).c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr);
                if (x != ERROR_SUCCESS) return fail(x);
                x = RegSetRaw(k, it.value, REG_BINARY, d);
                RegCloseKey(k);
                if (x == ERROR_SUCCESS) AuditChange(std::string(enable ? "Включён" : "Отключён") + " элемент автозагрузки: " + it.location + " :: " + it.value);
                return x == ERROR_SUCCESS ? true : fail(x);
            }, err);
        }
        case AOp::Service: {
            return WithKey(it, [&](HKEY r) {
                HKEY k = nullptr;
                LONG x = RegOpenKeyExW(r, W(it.sub).c_str(), 0, KEY_READ | KEY_WRITE, &k);
                if (x != ERROR_SUCCESS) return fail(x);
                RegVal st, bk;
                QueryVal(k, "", "Start", st);
                DWORD cur = st.data.size() >= 4 ? *(DWORD*)st.data.data() : 3;
                DWORD nv;
                if (!enable) { if (cur == 4) { RegCloseKey(k); return true; } nv = 4; RegSetRaw(k, "WRStartBackup", REG_DWORD, std::vector<uint8_t>((uint8_t*)&cur, (uint8_t*)&cur + 4)); }
                else { nv = (QueryVal(k, "", "WRStartBackup", bk) && bk.data.size() >= 4) ? *(DWORD*)bk.data.data() : 3; RegDeleteValueW(k, L"WRStartBackup"); }
                x = RegSetRaw(k, "Start", REG_DWORD, std::vector<uint8_t>((uint8_t*)&nv, (uint8_t*)&nv + 4));
                RegCloseKey(k);
                if (x == ERROR_SUCCESS) AuditChange(fmt("Служба/драйвер %s: Start %lu -> %lu", it.value.c_str(), (unsigned long)cur, (unsigned long)nv));
                return x == ERROR_SUCCESS ? true : fail(x);
            }, err);
        }
        case AOp::Ifeo: {
            return WithKey(it, [&](HKEY r) {
                HKEY k = nullptr;
                LONG x = RegOpenKeyExW(r, W(it.sub).c_str(), 0, KEY_READ | KEY_WRITE, &k);
                if (x != ERROR_SUCCESS) return fail(x);
                RegVal v;
                const char* from = enable ? "Debugger.WRDisabled" : "Debugger";
                const char* to = enable ? "Debugger" : "Debugger.WRDisabled";
                if (QueryVal(k, "", from, v)) { RegSetRaw(k, to, v.type, v.data); RegDeleteValueW(k, W(from).c_str()); AuditChange(std::string("IFEO ") + it.sub + ": " + from + " -> " + to); }
                RegCloseKey(k);
                return true;
            }, err);
        }
        case AOp::MoveValue: {
            return WithKey(it, [&](HKEY r) {
                HKEY k = nullptr;
                LONG x = RegOpenKeyExW(r, W(it.sub).c_str(), 0, KEY_READ | KEY_WRITE, &k);
                if (x != ERROR_SUCCESS) return fail(x);
                RegVal v;
                if (!enable && QueryVal(k, "", "AppInit_DLLs", v)) { RegSetRaw(k, "AppInit_DLLs.WRDisabled", v.type, v.data); RegSetString(k, "", "AppInit_DLLs", ""); AuditChange("AppInit_DLLs отключён: " + RegText(v)); }
                else if (enable && QueryVal(k, "", "AppInit_DLLs.WRDisabled", v)) { RegSetRaw(k, "AppInit_DLLs", v.type, v.data); RegDeleteValueW(k, L"AppInit_DLLs.WRDisabled"); AuditChange("AppInit_DLLs включён: " + RegText(v)); }
                RegCloseKey(k);
                return true;
            }, err);
        }
        default: err = "Для этого типа записи отключение не поддерживается. Используйте «Изменить» или редактор реестра."; return false;
    }
}

bool AutorunDelete(const AutoItem& it, std::string& err) {
    switch (it.op) {
        case AOp::RunValue: {
            return WithKey(it, [&](HKEY r) {
                RegVal v;
                QueryVal(r, it.sub, it.value, v);
                LONG x = RegDelValue(r, it.sub, it.value);
                if (x == ERROR_SUCCESS) AuditChange("Удалена запись автозапуска " + it.location + " :: " + it.value + " = " + RegText(v));
                else err = ErrMsg((DWORD)x);
                return x == ERROR_SUCCESS;
            }, err);
        }
        case AOp::StartupFile: {
            if (!DeleteFileW(W(it.path).c_str())) { err = ErrMsg(GetLastError()); return false; }
            AuditChange("Удалён файл автозагрузки " + it.path);
            return true;
        }
        case AOp::Ifeo: {
            return WithKey(it, [&](HKEY r) {
                RegDelValue(r, it.sub, "Debugger");
                RegDelValue(r, it.sub, "Debugger.WRDisabled");
                AuditChange("Удалён IFEO Debugger: " + it.sub);
                return true;
            }, err);
        }
        case AOp::MoveValue: {
            return WithKey(it, [&](HKEY r) {
                RegSetString(r, it.sub, "AppInit_DLLs", "");
                RegDelValue(r, it.sub, "AppInit_DLLs.WRDisabled");
                AuditChange("Очищен AppInit_DLLs (было: " + it.command + ")");
                return true;
            }, err);
        }
        default: err = "Эту запись нельзя удалить из списка (служба/драйвер — отключите; планировщик — только просмотр)."; return false;
    }
}

bool AutorunSetCommand(const AutoItem& it, const std::string& cmd, std::string& err) {
    if (it.op != AOp::RunValue && it.op != AOp::EditOnly && it.op != AOp::MoveValue) { err = "Для этого типа изменение команды не поддерживается."; return false; }
    return WithKey(it, [&](HKEY r) {
        RegVal v;
        bool had = QueryVal(r, it.sub, it.value, v);
        std::vector<uint8_t> d;
        std::string e;
        DWORD type = had ? v.type : REG_SZ;
        if (!RegParse(type, cmd, d, e)) { err = e; return false; }
        HKEY k = nullptr;
        LONG x = RegOpenKeyExW(r, W(it.sub).c_str(), 0, KEY_READ | KEY_WRITE, &k);
        if (x != ERROR_SUCCESS) { err = ErrMsg((DWORD)x); return false; }
        x = RegSetRaw(k, it.value, type, d);
        RegCloseKey(k);
        if (x == ERROR_SUCCESS) AuditChange("Изменена команда " + it.location + " :: " + it.value + ": '" + it.command + "' -> '" + cmd + "'");
        else err = ErrMsg((DWORD)x);
        return x == ERROR_SUCCESS;
    }, err);
}

std::string AutorunsCsv(const std::vector<AutoItem>& v) {
    std::string s = "Состояние,Тип,Имя,Команда,Файл,Найден,Пометка,Расположение\r\n";
    for (const auto& i : v)
        s += csvQuote(i.state) + "," + csvQuote(i.kind) + "," + csvQuote(i.name) + "," + csvQuote(i.command) + "," + csvQuote(i.file) + "," + (i.fileExists ? "да" : "нет") + "," + csvQuote(i.flag) + "," + csvQuote(i.location) + "\r\n";
    return s;
}

}  // namespace wr
