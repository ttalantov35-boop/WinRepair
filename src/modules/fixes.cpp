#include "fixes.h"

namespace wr {

namespace {

bool OpenRc(Ctx& c, RegCtx& rc) {
    if (!c.needTarget()) return false;
    std::string err;
    if (!rc.open(c.target, err)) { c.error("Реестр: " + err); return false; }
    return true;
}

void BackupKey(Ctx& c, HKEY base, const std::string& sub, const std::string& tag) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(base, W(sub).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    std::string reg;
    ExportRegKey(k, "HKEY_LOCAL_MACHINE\\" + sub, reg);
    RegCloseKey(k);
    std::string f = Hives().backupDir() + tag + "_" + Stamp() + ".reg";
    WriteText(f, reg);
    c.info("Резервная копия ветки: " + f);
}

// delete the listed values; returns number deleted
int DelValues(Ctx& c, HKEY base, const std::string& sub, const std::vector<const char*>& names, const std::string& tag) {
    int n = 0;
    bool backedUp = false;
    for (const char* nm : names) {
        RegVal v;
        if (!QueryVal(base, sub, nm, v)) continue;
        if (!backedUp) { BackupKey(c, base, sub, tag); backedUp = true; }
        if (RegDelValue(base, sub, nm) == ERROR_SUCCESS) { AuditChange(fmt("Удалено значение %s\\%s = %s", sub.c_str(), nm, RegValueText(v, 120).c_str())); ++n; }
    }
    return n;
}

bool SetStr(Ctx& c, HKEY base, const std::string& sub, const std::string& name, const std::string& val) {
    RegVal v;
    bool had = QueryVal(base, sub, name, v);
    if (had && RegText(v) == val) return false;
    if (RegSetString(base, sub, name, val) != ERROR_SUCCESS) { c.error("Не удалось записать " + sub + "\\" + name); return false; }
    AuditChange(fmt("%s\\%s: '%s' -> '%s'", sub.c_str(), name.c_str(), had ? RegText(v).c_str() : "(нет)", val.c_str()));
    return true;
}

uint64_t DeleteContents(const std::string& dir, int& files, bool removeSelf) {
    uint64_t bytes = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::string p = dir + "\\" + U(fd.cFileName);
            SetFileAttributesW(W(p).c_str(), FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) bytes += DeleteContents(p, files, true);
                else RemoveDirectoryW(W(p).c_str());
            } else {
                uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                if (DeleteFileW(W(p).c_str())) { bytes += sz; ++files; }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (removeSelf) RemoveDirectoryW(W(dir).c_str());
    return bytes;
}

bool RenameBak(Ctx& c, const std::string& path) {
    if (!PathExists(path)) return false;
    std::string to = path + ".bak_" + Stamp();
    if (MoveFileW(W(path).c_str(), W(to).c_str())) { AuditChange("Переименовано " + path + " -> " + to); return true; }
    c.warn("Не удалось переименовать " + path + ": " + ErrMsg(GetLastError()));
    return false;
}

std::vector<std::string> Profiles(const Ctx& c) {
    std::vector<std::string> v;
    std::string users = c.target.root + "Users\\";
    for (const auto& u : ListDirs(users)) {
        std::string l = toLowerUtf8(u);
        if (l == "public" || l == "all users" || l == "default user") continue;
        v.push_back(users + u + "\\");
    }
    return v;
}

// ---------------------------------------------------------------------------------------------
void FxWinlogon(Ctx& c) {
    RegCtx rc;
    if (!OpenRc(c, rc)) return;
    std::string sub = "Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    RegVal v;
    std::string cur = QueryVal(rc.software, sub, "Userinit", v) ? RegText(v) : "";
    std::string drv = (cur.size() > 2 && cur[1] == ':') ? cur.substr(0, 2) : "C:";
    int n = 0;
    n += SetStr(c, rc.software, sub, "Shell", "explorer.exe");
    n += SetStr(c, rc.software, sub, "Userinit", drv + "\\Windows\\system32\\userinit.exe,");
    n += DelValues(c, rc.software, sub, {"Taskman", "AppSetup", "GinaDLL"}, "winlogon");
    c.ok(fmt("Winlogon: изменено значений — %d.", n));
}

void FxIfeo(Ctx& c) {
    RegCtx rc;
    if (!OpenRc(c, rc)) return;
    std::string base = "Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\";
    int n = 0;
    for (const char* e : {"taskmgr.exe", "regedit.exe", "cmd.exe", "msconfig.exe", "mmc.exe", "explorer.exe", "rstrui.exe", "iexplore.exe", "sethc.exe", "utilman.exe",
                          "osk.exe", "magnify.exe", "narrator.exe", "displayswitch.exe", "atbroker.exe", "procexp.exe", "autoruns.exe", "mbam.exe", "msmpeng.exe",
                          "mpcmdrun.exe", "taskkill.exe", "tasklist.exe", "powershell.exe", "mshta.exe", "wscript.exe", "cscript.exe", "rundll32.exe", "regsvr32.exe", "svchost.exe"})
        n += DelValues(c, rc.software, base + e, {"Debugger"}, std::string("ifeo_") + e);
    c.ok(fmt("IFEO: удалено подмен запуска — %d (учтены также sethc/utilman).", n));
}

void FxAppInit(Ctx& c) {
    RegCtx rc;
    if (!OpenRc(c, rc)) return;
    int n = 0;
    for (const char* b : {"Microsoft\\Windows NT\\CurrentVersion\\Windows", "WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows"}) {
        RegVal v;
        if (QueryVal(rc.software, b, "AppInit_DLLs", v) && !trim(RegText(v)).empty()) { BackupKey(c, rc.software, b, "appinit"); n += SetStr(c, rc.software, b, "AppInit_DLLs", ""); }
        RegVal l;
        if (QueryVal(rc.software, b, "LoadAppInit_DLLs", l) && l.data.size() >= 4 && *(DWORD*)l.data.data() != 0) { RegSetDword(rc.software, b, "LoadAppInit_DLLs", 0); AuditChange(std::string("LoadAppInit_DLLs=0 в ") + b); ++n; }
    }
    c.ok(fmt("AppInit_DLLs: изменено — %d.", n));
}

void FxPolicies(Ctx& c) {
    RegCtx rc;
    if (!OpenRc(c, rc)) return;
    const std::vector<const char*> sys = {"DisableTaskMgr", "DisableRegistryTools", "DisableCMD", "DisableLockWorkstation", "DisableChangePassword", "NoDispCPL"};
    const std::vector<const char*> exp = {"NoRun", "NoFind", "NoFolderOptions", "NoControlPanel", "NoDesktop", "NoViewContextMenu", "NoSetFolders", "NoSetTaskbar",
                                          "NoTrayContextMenu", "NoDrives", "NoViewOnDrive", "NoClose", "NoWinKeys", "NoNetHood", "NoRecentDocsMenu", "NoStartMenuMorePrograms"};
    int n = 0;
    n += DelValues(c, rc.software, "Microsoft\\Windows\\CurrentVersion\\Policies\\System", sys, "pol_hklm_system");
    n += DelValues(c, rc.software, "Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", exp, "pol_hklm_explorer");
    n += DelValues(c, rc.software, "Policies\\Microsoft\\Windows\\System", {"DisableCMD"}, "pol_hklm_wsys");
    n += DelValues(c, rc.software, "Policies\\Microsoft\\Windows Defender", {"DisableAntiSpyware", "DisableRealtimeMonitoring"}, "pol_defender");
    for (auto& u : rc.users) {
        n += DelValues(c, u.root, "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", sys, "pol_" + u.name + "_system");
        n += DelValues(c, u.root, "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", exp, "pol_" + u.name + "_explorer");
        n += DelValues(c, u.root, "Software\\Policies\\Microsoft\\Windows\\System", {"DisableCMD"}, "pol_" + u.name + "_wsys");
    }
    c.ok(fmt("Политики: удалено значений — %d.", n));
}

void FxProxy(Ctx& c) {
    RegCtx rc;
    if (!OpenRc(c, rc)) return;
    int n = 0;
    const std::vector<const char*> names = {"ProxyServer", "ProxyOverride", "AutoConfigURL", "ProxyHttp1.1"};
    auto reset = [&](HKEY root, const std::string& sub, const std::string& tag) {
        n += DelValues(c, root, sub, names, tag);
        RegVal v;
        if (QueryVal(root, sub, "ProxyEnable", v) && v.data.size() >= 4 && *(DWORD*)v.data.data() != 0) { RegSetDword(root, sub, "ProxyEnable", 0); AuditChange("ProxyEnable=0 в " + sub); ++n; }
    };
    reset(rc.software, "Microsoft\\Windows\\CurrentVersion\\Internet Settings", "proxy_hklm");
    reset(rc.software, "Policies\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", "proxy_hklm_pol");
    for (auto& u : rc.users) reset(u.root, "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", "proxy_" + u.name);
    c.ok(fmt("Прокси: изменено — %d.", n));
}

void FxHosts(Ctx& c) {
    if (!c.needTarget()) return;
    std::string p = c.target.winDir + "\\System32\\drivers\\etc\\hosts";
    if (PathExists(p)) { std::string b = FileBackup(p, "hosts"); c.info("Копия hosts: " + (b.empty() ? "не удалась" : b)); }
    SetFileAttributesW(W(p).c_str(), FILE_ATTRIBUTE_NORMAL);
    MakeDirs(c.target.winDir + "\\System32\\drivers\\etc\\");
    std::string def = "# Copyright (c) 1993-2009 Microsoft Corp.\r\n#\r\n# This is a sample HOSTS file used by Microsoft TCP/IP for Windows.\r\n#\r\n"
                      "# localhost name resolution is handled within DNS itself.\r\n#\t127.0.0.1       localhost\r\n#\t::1             localhost\r\n";
    if (WriteText(p, def, false)) { AuditChange("Файл hosts сброшен к значениям по умолчанию: " + p); c.ok("hosts восстановлен (копия сохранена)."); }
    else c.error("Не удалось записать hosts: " + ErrMsg(GetLastError()));
}

void FxServices(Ctx& c) {
    RegCtx rc;
    if (!OpenRc(c, rc)) return;
    struct D { const char* n; DWORD start; };
    static const D defs[] = {{"wuauserv", 3}, {"BITS", 3}, {"CryptSvc", 2}, {"TrustedInstaller", 3}, {"Winmgmt", 2}, {"RpcSs", 2}, {"RpcEptMapper", 2}, {"DcomLaunch", 2},
                             {"EventLog", 2}, {"Dnscache", 2}, {"Dhcp", 2}, {"LanmanWorkstation", 2}, {"mpssvc", 2}, {"WinDefend", 2}, {"wscsvc", 2}, {"Schedule", 2},
                             {"Themes", 2}, {"AudioSrv", 2}, {"SamSs", 2}, {"nsi", 2}, {"NlaSvc", 2}, {"ProfSvc", 2}, {"UserManager", 2}, {"gpsvc", 2}, {"SENS", 2},
                             {"VSS", 3}, {"Netman", 3}, {"netprofm", 3}, {"StateRepository", 3}, {"lmhosts", 3}, {"msiserver", 3}, {"swprv", 3}};
    int n = 0;
    for (const auto& d : defs) {
        std::string sub = rc.cs + "\\Services\\" + d.n;
        RegVal v;
        if (!QueryVal(rc.system, sub, "Start", v) || v.data.size() < 4) continue;
        if (*(DWORD*)v.data.data() != 4) continue;
        RegSetDword(rc.system, sub, "Start", d.start);
        AuditChange(fmt("Служба %s была отключена (Start=4) -> Start=%lu", d.n, (unsigned long)d.start));
        ++n;
    }
    c.ok(fmt("Ключевые службы: включено обратно — %d.", n));
}

void FxNetwork(Ctx& c) {
    c.run("netsh winsock reset", "\"" + SysTool("netsh.exe") + "\" winsock reset", 60);
    c.run("netsh int ip reset", "\"" + SysTool("netsh.exe") + "\" int ip reset", 60);
    c.run("netsh winhttp reset proxy", "\"" + SysTool("netsh.exe") + "\" winhttp reset proxy", 60);
    c.run("ipconfig /flushdns", "\"" + SysTool("ipconfig.exe") + "\" /flushdns", 60);
    c.warn("Для применения сброса сети требуется перезагрузка.");
}

void FxWinUpdate(Ctx& c) {
    if (!c.needTarget()) return;
    if (c.target.online) for (const char* s : {"wuauserv", "cryptSvc", "bits", "msiserver"}) c.run(std::string("net stop ") + s, "\"" + SysTool("net.exe") + "\" stop " + s, 60);
    int n = 0;
    n += RenameBak(c, c.target.winDir + "\\SoftwareDistribution");
    n += RenameBak(c, c.target.winDir + "\\System32\\catroot2");
    if (c.target.online) for (const char* s : {"cryptSvc", "bits", "wuauserv", "msiserver"}) c.run(std::string("net start ") + s, "\"" + SysTool("net.exe") + "\" start " + s, 60);
    c.ok(fmt("Центр обновления: переименовано папок — %d (Windows пересоздаст их).", n));
}

void FxTemp(Ctx& c) {
    if (!c.needTarget()) return;
    int files = 0;
    uint64_t bytes = DeleteContents(c.target.winDir + "\\Temp", files, false);
    for (const auto& p : Profiles(c)) bytes += DeleteContents(p + "AppData\\Local\\Temp", files, false);
    AuditChange(fmt("Очистка временных файлов: удалено %d файлов, %s", files, fmtBytes(bytes).c_str()));
    c.ok(fmt("Временные файлы: удалено %d, освобождено %s.", files, fmtBytes(bytes).c_str()));
}

void FxIcons(Ctx& c) {
    if (!c.needTarget()) return;
    int n = 0;
    for (const auto& p : Profiles(c)) {
        std::string l = p + "AppData\\Local\\";
        if (DeleteFileW(W(l + "IconCache.db").c_str())) ++n;
        std::string e = l + "Microsoft\\Windows\\Explorer\\";
        for (const char* pat : {"iconcache_*.db", "thumbcache_*.db"})
            for (const auto& f : ListFiles(e, pat)) if (DeleteFileW(W(e + f).c_str())) ++n;
    }
    AuditChange(fmt("Сброс кэша значков/миниатюр: удалено файлов %d", n));
    c.ok(fmt("Кэш значков и миниатюр: удалено файлов — %d (пересоздаются при входе).", n));
}

void FxPending(Ctx& c) {
    if (!c.needTarget()) return;
    if (c.target.online) { c.warn("Для работающей ОС отмена отложенных операций недоступна из этой программы (используйте WinRE/WinPE)."); return; }
    std::string scratch = c.sub("scratch");
    scratch.pop_back();
    c.run("DISM /RevertPendingActions", "\"" + SysTool("dism.exe") + "\" /Image:" + c.target.root + " /Cleanup-Image /RevertPendingActions /ScratchDir:\"" + scratch + "\"");
    RenameBak(c, c.target.winDir + "\\WinSxS\\pending.xml");
    c.ok("Отложенные операции обработаны. Загрузите Windows и проверьте целостность.");
}

void FxRegBack(Ctx& c) {
    if (!c.needTarget()) return;
    if (c.target.online) { c.error("Только для офлайн-цели."); return; }
    Hives().unloadAll();
    std::string cfg = c.target.winDir + "\\System32\\config\\";
    int n = 0;
    for (const char* h : {"SYSTEM", "SOFTWARE", "SAM", "SECURITY", "DEFAULT"}) {
        std::string src = cfg + "RegBack\\" + h, dst = cfg + h;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(W(src).c_str(), GetFileExInfoStandard, &fa) || (fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 0)) { c.warn(std::string("RegBack\\") + h + " отсутствует или пуст (автокопии отключены с Windows 10 1803)."); continue; }
        std::string b = FileBackup(dst, std::string("regback_") + h);
        if (b.empty()) { c.warn(std::string("Не удалось сохранить текущий ") + h + " — пропуск."); continue; }
        if (CopyFileW(W(src).c_str(), W(dst).c_str(), FALSE)) { AuditChange("Куст " + dst + " заменён копией из RegBack (старый: " + b + ")"); ++n; }
        else c.error("Копирование не удалось: " + ErrMsg(GetLastError()));
    }
    c.ok(fmt("RegBack: восстановлено кустов — %d.", n));
}

void FxWmi(Ctx& c) {
    if (!c.target.online) { c.warn("Восстановление WMI доступно только в работающей Windows."); return; }
    ProcResult r = c.run("winmgmt /verifyrepository", "\"" + SysTool("wbem\\winmgmt.exe") + "\" /verifyrepository", 120);
    std::string l = toLowerUtf8(r.output);
    if (containsAny(l, {"inconsistent", "not consistent", "несогласован", "не согласован"})) c.run("winmgmt /salvagerepository", "\"" + SysTool("wbem\\winmgmt.exe") + "\" /salvagerepository", 300);
    else c.ok("Репозиторий WMI согласован.");
}

void FxRegister(Ctx& c) {
    if (!c.target.online) { c.warn("Перерегистрация компонентов — только для работающей Windows."); return; }
    int n = 0;
    for (const char* d : {"atl.dll", "urlmon.dll", "mshtml.dll", "shdocvw.dll", "browseui.dll", "jscript.dll", "vbscript.dll", "scrrun.dll", "msxml3.dll", "msxml6.dll", "actxprxy.dll",
                          "oleaut32.dll", "wuapi.dll", "wuaueng.dll", "wups.dll", "wups2.dll", "wuweb.dll", "qmgr.dll", "qmgrprxy.dll", "wucltux.dll", "muweb.dll", "wuwebv.dll", "shell32.dll"}) {
        ProcResult r = RunProcess("\"" + SysTool("regsvr32.exe") + "\" /s " + d, "", nullptr, c.cancel, 30);
        if (r.started && r.exitCode == 0) ++n; else c.warn(std::string("regsvr32 ") + d + ": код " + std::to_string(r.exitCode));
    }
    AuditChange(fmt("Перерегистрировано системных DLL: %d", n));
    c.ok(fmt("Перерегистрировано компонентов: %d.", n));
}

void FxAssoc(Ctx& c) {
    RegCtx rc;
    if (!OpenRc(c, rc)) return;
    struct A { const char* ext; const char* cls; const char* cmd; };
    static const A a[] = {{".exe", "exefile", "\"%1\" %*"}, {".bat", "batfile", "\"%1\" %*"}, {".cmd", "cmdfile", "\"%1\" %*"}, {".com", "comfile", "\"%1\" %*"},
                          {".reg", "regfile", "regedit.exe \"%1\""}};
    int n = 0;
    BackupKey(c, rc.software, "Classes\\exefile", "assoc_exefile");
    for (const auto& x : a) {
        n += SetStr(c, rc.software, std::string("Classes\\") + x.ext, "", x.cls);
        n += SetStr(c, rc.software, std::string("Classes\\") + x.cls + "\\shell\\open\\command", "", x.cmd);
    }
    n += SetStr(c, rc.software, "Classes\\.lnk", "", "lnkfile");
    for (auto& u : rc.users)
        for (const char* e : {".exe", ".bat", ".cmd", ".com", ".lnk", ".reg"}) {
            std::string sub = std::string("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\") + e;
            HKEY k = nullptr;
            if (RegOpenKeyExW(u.root, W(sub).c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) { RegCloseKey(k); RegDelTree(u.root, sub); AuditChange("Удалены пользовательские переопределения " + sub + " (" + u.name + ")"); ++n; }
        }
    c.ok(fmt("Ассоциации файлов: исправлено — %d.", n));
}

void FxSafeBoot(Ctx& c) {
    RegCtx rc;
    if (!OpenRc(c, rc)) return;
    int n = 0;
    for (const char* k : {"Minimal", "Network"}) {
        std::string sub = rc.cs + "\\Control\\SafeBoot\\" + k;
        HKEY h = nullptr;
        if (RegOpenKeyExW(rc.system, W(sub).c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) { RegCloseKey(h); continue; }
        RegMakeKey(rc.system, sub);
        RegSetString(rc.system, sub, "", "Driver Group");
        AuditChange("Создан отсутствующий ключ SafeBoot\\" + std::string(k) + " (безопасный режим был недоступен)");
        ++n;
    }
    c.ok(fmt("SafeBoot: восстановлено ключей — %d (0 = всё в порядке).", n));
}

}  // namespace

const std::vector<FixDef>& FixList() {
    static const std::vector<FixDef> v = {
        {1, "Сброс Winlogon: Shell и Userinit", "Возвращает Shell=explorer.exe и Userinit по умолчанию, убирает Taskman/AppSetup/GinaDLL (типичная подмена вредоносами — чёрный экран после входа).", Risk::Changing, false, true, FxWinlogon},
        {2, "Удалить подмену запуска программ (IFEO Debugger)", "Удаляет Debugger для taskmgr, regedit, cmd, msconfig, explorer и др. (подмена запуска; включая sethc/utilman).", Risk::Changing, false, true, FxIfeo},
        {3, "Очистить AppInit_DLLs", "Отключает внедрение DLL во все процессы (AppInit_DLLs, 64 и 32 бит).", Risk::Changing, false, true, FxAppInit},
        {4, "Удалить политики, оставленные заражением", "Снимает блокировки диспетчера задач, regedit, cmd, «Выполнить», панели управления, Defender и т.п. (HKLM и профили).", Risk::Changing, false, true, FxPolicies},
        {5, "Сбросить прокси-настройки", "Удаляет ProxyServer/ProxyOverride/AutoConfigURL и выключает ProxyEnable (система и профили).", Risk::Changing, false, true, FxProxy},
        {6, "Восстановить файл hosts", "Сохраняет копию и записывает hosts по умолчанию.", Risk::Changing, false, true, FxHosts},
        {7, "Включить обратно ключевые службы", "Если важные службы (WU, BITS, Crypt, WMI, RPC, Defender, Firewall…) отключены — возвращает стандартный тип запуска.", Risk::Changing, false, true, FxServices},
        {8, "Исправить ассоциации .exe/.bat/.cmd/.com/.reg/.lnk", "Восстанавливает классы и команды открытия, удаляет пользовательские переопределения (после заражения «ничего не запускается»).", Risk::Changing, false, true, FxAssoc},
        {9, "Проверить/создать ключи SafeBoot", "Если вредонос удалил ключи Minimal/Network, безопасный режим не работает — создаёт их.", Risk::Changing, false, true, FxSafeBoot},
        {10, "Сброс Центра обновления (SoftwareDistribution, catroot2)", "Переименовывает папки кэша обновлений (онлайн — с остановкой/запуском служб).", Risk::Changing, true, true, FxWinUpdate},
        {11, "Очистить временные файлы", "Windows\\Temp и AppData\\Local\\Temp профилей.", Risk::Changing, true, true, FxTemp},
        {12, "Сбросить кэш значков и миниатюр", "Удаляет IconCache.db, iconcache_*.db, thumbcache_*.db.", Risk::Safe, true, true, FxIcons},
        {13, "Сброс сетевого стека (Winsock/TCP-IP/DNS/WinHTTP)", "netsh winsock reset, int ip reset, winhttp reset proxy, flushdns. Нужна перезагрузка.", Risk::Changing, true, false, FxNetwork},
        {14, "WMI: проверка и восстановление репозитория", "winmgmt /verifyrepository, при несогласованности /salvagerepository.", Risk::Changing, true, false, FxWmi},
        {15, "Перерегистрировать системные компоненты", "regsvr32 /s для набора DLL (скрипты, MSXML, Центр обновления, браузерные компоненты).", Risk::Changing, true, false, FxRegister},
        {16, "Отменить отложенные операции обновления (pending.xml)", "DISM /RevertPendingActions + переименование pending.xml. Лечит зависание на «Отмена изменений».", Risk::Danger, false, true, FxPending},
        {17, "Восстановить кусты реестра из RegBack", "Заменяет SYSTEM/SOFTWARE/SAM/SECURITY/DEFAULT копиями из config\\RegBack (текущие сохраняются). Работает, если RegBack не пуст.", Risk::Danger, false, true, FxRegBack},
    };
    return v;
}

void ModFixes(Ctx& c) {
    c.info("=== Мастер исправлений ===");
    const auto& all = FixList();
    int done = 0;
    for (int id : c.opt.fixIds) {
        if (c.cancelled()) { c.warn("Прервано пользователем."); break; }
        const FixDef* f = nullptr;
        for (const auto& x : all) if (x.id == id) f = &x;
        if (!f) continue;
        bool applicable = c.target.valid && (c.target.online ? f->online : f->offline);
        c.info(fmt("--- Исправление %d: %s ---", f->id, f->title));
        if (!applicable) { c.warn(c.target.online ? "Пропущено: применимо только к офлайн-ОС (WinPE)." : "Пропущено: применимо только к работающей Windows."); continue; }
        AuditChange(fmt("FIX #%d %s (target %s)", f->id, f->title, c.target.root.c_str()));
        try { f->fn(c); ++done; }
        catch (const std::exception& e) { c.error(std::string("Ошибка: ") + e.what()); }
    }
    c.ok(fmt("Мастер исправлений завершён. Выполнено: %d. Резервные копии: %s", done, Hives().backupDir().c_str()));
}

}  // namespace wr
