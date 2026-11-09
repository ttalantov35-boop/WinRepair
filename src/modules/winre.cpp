#include "winre.h"
#include "catalog.h"

#include <winioctl.h>

#include <cstring>

namespace wr {

namespace {

std::string XmlAttr(const std::string& x, const std::string& tag, const std::string& attr) {
    size_t p = x.find("<" + tag);
    if (p == std::string::npos) return "";
    size_t e = x.find('>', p);
    if (e == std::string::npos) return "";
    std::string t = x.substr(p, e - p);
    size_t a = t.find(attr + "=\"");
    if (a == std::string::npos) return "";
    a += attr.size() + 2;
    size_t q = t.find('"', a);
    return q == std::string::npos ? "" : t.substr(a, q - a);
}

std::string Guid(const std::string& out) {
    size_t a = out.find('{');
    size_t b = a == std::string::npos ? a : out.find('}', a);
    return b == std::string::npos ? "" : out.substr(a, b - a + 1);
}

bool IsUefiStore(const std::string& store) {
    std::string l = toLowerUtf8(store);
    if (!store.empty()) return l.find("\\efi\\") != std::string::npos;
    typedef BOOL(WINAPI * PGetFirmwareType)(DWORD*);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    PGetFirmwareType f = k ? (PGetFirmwareType)GetProcAddress(k, "GetFirmwareType") : nullptr;
    DWORD t = 0;
    return f && f(&t) && t == 2;
}

std::string Dir(const std::string& p) { size_t i = p.find_last_of('\\'); return i == std::string::npos ? "" : p.substr(0, i + 1); }

std::string NoSlash(std::string s) { while (!s.empty() && s.back() == '\\') s.pop_back(); return s; }

void RemoveTree(const std::string& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::string p = dir + "\\" + U(fd.cFileName);
            SetFileAttributesW(W(p).c_str(), FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) RemoveTree(p); else RemoveDirectoryW(W(p).c_str()); }
            else DeleteFileW(W(p).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(W(dir).c_str());
}

struct CopyCtx { const std::atomic<bool>* cancel; ULONGLONG last; };
DWORD CALLBACK CopyProgress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID p) {
    auto* cc = (CopyCtx*)p;
    if (cc->cancel && cc->cancel->load()) return PROGRESS_CANCEL;
    ULONGLONG now = GetTickCount64();
    if (now - cc->last > 500 && total.QuadPart > 0) {
        cc->last = now;
        Logger::I().progress(fmt("Копирование: %.0f%%", 100.0 * (double)done.QuadPart / (double)total.QuadPart));
    }
    return PROGRESS_CONTINUE;
}

bool CopyBig(const std::string& src, const std::string& dst, const std::atomic<bool>* cancel) {
    CopyCtx cc{cancel, 0};
    BOOL ok = CopyFileExW(W(src).c_str(), W(dst).c_str(), CopyProgress, &cc, nullptr, 0);
    Logger::I().progress("");
    return ok != 0;
}

unsigned long long FileSize(const std::string& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(W(p).c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

std::string ReadSmall(const std::string& p) {
    std::vector<uint8_t> raw;
    if (!ReadBytes(p, raw, 1 << 20) || raw.empty()) return "";
    if (raw.size() > 2 && raw[0] == 0xFF && raw[1] == 0xFE) return utf16leToUtf8(raw.data(), raw.size());
    return std::string((const char*)raw.data(), raw.size());
}

const char* kRel = "Recovery\\WindowsRE\\Winre.wim";

}  // namespace

bool CopyTree(const std::string& src, const std::string& dst, int& files, const std::atomic<bool>* cancel) {
    MakeDirs(dst + "\\");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(src + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (cancel && cancel->load()) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::string n = U(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CopyTree(src + "\\" + n, dst + "\\" + n, files, cancel);
        else if (CopyFileW(W(src + "\\" + n).c_str(), W(dst + "\\" + n).c_str(), FALSE)) ++files;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

std::string RebootCommand() {
    return GetEnv().isWinPE ? "\"" + SysTool("wpeutil.exe") + "\" reboot" : "\"" + SysTool("shutdown.exe") + "\" /r /t 0";
}

static std::vector<std::string> FindWims(const Target& t) {
    std::vector<std::pair<int, std::string>> found;  // (0 = same disk as target, 1 = other)
    int tdisk = t.valid ? VolumeDiskNumber(t.letter()) : -1;
    auto consider = [&](char L) {
        std::string p = std::string(1, L) + ":\\" + kRel;
        if (PathExists(p)) found.push_back({VolumeDiskNumber(L) == tdisk && tdisk >= 0 ? 0 : 1, p});
    };
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char L = (char)('A' + i);
        UINT dt = GetDriveTypeW(W(std::string(1, L) + ":\\").c_str());
        if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE) consider(L);
    }
    if (t.valid && PathExists(t.winDir + "\\System32\\Recovery\\Winre.wim")) found.push_back({0, t.winDir + "\\System32\\Recovery\\Winre.wim"});
    // hidden recovery volumes: mount temporarily
    wchar_t vol[MAX_PATH];
    HANDLE h = FindFirstVolumeW(vol, MAX_PATH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            DWORD need = 0;
            GetVolumePathNamesForVolumeNameW(vol, nullptr, 0, &need);
            std::vector<wchar_t> names(need + 2, 0);
            GetVolumePathNamesForVolumeNameW(vol, names.data(), need, &need);
            if (names[0]) continue;
            wchar_t fs[32] = {0};
            if (!GetVolumeInformationW(vol, nullptr, 0, nullptr, nullptr, nullptr, fs, 32)) continue;
            if (toLowerUtf8(U(fs)) != "ntfs") continue;
            ULARGE_INTEGER fb{}, tb{}, tf{};
            GetDiskFreeSpaceExW(vol, &fb, &tb, &tf);
            if (tb.QuadPart > 8ULL * 1024 * 1024 * 1024) continue;
            char L = TempMountVolume(vol);
            if (!L) continue;
            std::string p = std::string(1, L) + ":\\" + kRel;
            if (PathExists(p)) found.push_back({VolumeDiskNumber(L) == tdisk && tdisk >= 0 ? 0 : 1, p});
            else TempUnmountVolume(L);
        } while (FindNextVolumeW(h, vol, MAX_PATH));
        FindVolumeClose(h);
    }
    std::stable_sort(found.begin(), found.end(), [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) { return a.first < b.first; });
    std::vector<std::string> out;
    for (auto& f : found) if (std::find(out.begin(), out.end(), f.second) == out.end()) out.push_back(f.second);
    return out;
}

WinReInfo ReadWinReInfo(const Target& t) {
    WinReInfo r;
    r.xmlPath = t.winDir + "\\System32\\Recovery\\ReAgent.xml";
    std::string x = ReadSmall(r.xmlPath);
    r.xmlFound = !x.empty();
    if (r.xmlFound) {
        r.bcdId = XmlAttr(x, "WinreBCD", "id");
        r.location = XmlAttr(x, "WinreLocation", "path");
        r.installState = XmlAttr(x, "InstallState", "state");
    }
    r.wimCandidates = FindWims(t);
    if (!r.wimCandidates.empty()) { r.wimPath = r.wimCandidates[0]; r.wimSize = FileSize(r.wimPath); }
    return r;
}

std::string FindWinReEntry(const std::vector<BcdEntry>& entries, const std::string& preferId, bool custom) {
    std::string any;
    for (const auto& e : entries) {
        std::string dev = toLowerUtf8(e.get("device") + " " + e.get("osdevice")), d = toLowerUtf8(e.desc());
        bool isCustom = d.find("winrepair") != std::string::npos || dev.find("winrepairre") != std::string::npos;
        if (custom != isCustom) continue;
        if (custom) return e.id();
        if (dev.find("winre.wim") == std::string::npos) continue;
        if (!preferId.empty() && toLowerUtf8(e.id()) == toLowerUtf8(preferId)) return e.id();
        if (any.empty()) any = e.id();
    }
    return custom ? "" : any;
}

std::string WinReSdiFor(const std::string& wimPath, const Target& t) {
    std::string d = Dir(wimPath), sdi = d + "boot.sdi";
    if (PathExists(sdi)) return sdi;
    for (const char* rel : {"\\Boot\\DVD\\PCAT\\boot.sdi", "\\Boot\\DVD\\EFI\\boot.sdi", "\\Boot\\PCAT\\boot.sdi", "\\Boot\\EFI\\boot.sdi"}) {
        std::string src = t.winDir + rel;
        if (PathExists(src) && CopyFileW(W(src).c_str(), W(sdi).c_str(), FALSE)) { AuditChange("boot.sdi скопирован из " + src + " в " + sdi); return sdi; }
    }
    return "";
}

bool CreateRamdiskEntry(Ctx& c, const std::string& store, const std::string& desc, const std::string& wimPath, const std::string& sdiPath, std::string& outId) {
    std::string pre = BcdPrefix(store);
    char vol = wimPath[0];
    std::string wimRel = wimPath.substr(2), sdiRel = sdiPath.substr(2);
    ProcResult o = c.run("Создать параметры RAM-диска", pre + " /create /d \"" + desc + " (ramdisk options)\" /device", 30);
    std::string opt = Guid(o.output);
    if (opt.empty()) { c.error("Не удалось создать объект параметров RAM-диска."); return false; }
    AuditChange("BCD: создан объект ramdisk options " + opt);
    c.run("ramdisksdidevice", pre + " /set " + opt + " ramdisksdidevice partition=" + std::string(1, vol) + ":", 30);
    c.run("ramdisksdipath", pre + " /set " + opt + " ramdisksdipath " + sdiRel, 30);
    ProcResult e = c.run("Создать запись загрузчика", pre + " /create /d \"" + desc + "\" /application osloader", 30);
    std::string id = Guid(e.output);
    if (id.empty()) { c.error("Не удалось создать запись osloader."); return false; }
    AuditChange("BCD: создана запись " + id + " (" + desc + ")");
    std::string dev = "ramdisk=[" + std::string(1, vol) + ":]" + wimRel + "," + opt;
    bool ok = true;
    auto set = [&](const std::string& el, const std::string& val) {
        ProcResult r = c.run("/set " + el, pre + " /set " + id + " " + el + " " + val, 30);
        if (r.exitCode != 0) ok = false;
    };
    set("device", dev);
    set("osdevice", dev);
    set("path", IsUefiStore(store) ? "\\windows\\system32\\winload.efi" : "\\windows\\system32\\winload.exe");
    set("systemroot", "\\windows");
    set("winpe", "yes");
    set("detecthal", "yes");
    set("nx", "OptIn");
    set("bootmenupolicy", "Standard");
    set("displaymessage", "Recovery");
    set("inherit", "{bootloadersettings}");
    if (!ok) c.warn("Часть параметров записи не установилась — проверьте журнал.");
    outId = id;
    return true;
}

void JobBootOnce(Ctx& c, const std::string& store, const std::string& id, bool reboot) {
    AuditChange("BCD: одноразовая загрузка (bootsequence) -> " + id + " в хранилище " + (store.empty() ? "текущей системы" : store));
    ProcResult r = c.run("bcdedit /bootsequence " + id, BcdPrefix(store) + " /bootsequence " + id, 30);
    if (r.exitCode != 0) { c.error("Не удалось установить одноразовую загрузку."); return; }
    c.ok("Готово: при СЛЕДУЮЩЕЙ загрузке компьютер войдёт в " + id + ", затем вернётся к обычной загрузке.");
    if (reboot) { AuditChange("Перезагрузка после bootsequence"); c.run("Перезагрузка", RebootCommand(), 30); }
}

void JobBuildWinRepairRE(Ctx& c, const std::string& store, const std::string& srcWim) {
    c.info("=== Создание «Windows RE + WinRepair» ===");
    if (!c.needTarget()) return;
    const Target& t = c.target;
    if (!PathExists(srcWim)) { c.error("Winre.wim не найден: " + srcWim); return; }
    std::string exeSrc;
    {
        std::wstring b(32768, L'\0');
        DWORD n = GetModuleFileNameW(nullptr, &b[0], (DWORD)b.size());
        exeSrc = U(b.c_str(), (int)n);
    }
    unsigned long long need = FileSize(srcWim) * 2 + (300ULL << 20);
    ULARGE_INTEGER fb{}, tb{}, tf{};
    GetDiskFreeSpaceExW(W(t.root).c_str(), &fb, &tb, &tf);
    if (fb.QuadPart < need) { c.error(fmt("На %s свободно %s, нужно около %s.", t.root.c_str(), fmtBytes(fb.QuadPart).c_str(), fmtBytes(need).c_str())); return; }
    std::string dest = t.root + "Recovery\\WinRepairRE\\", mnt = dest + "mnt\\", dst = dest + "WinRE_WR.wim", scr = dest + "scratch\\";
    MakeDirs(dest); MakeDirs(mnt); MakeDirs(scr);
    std::string sdi = WinReSdiFor(srcWim, t);
    if (sdi.empty()) { c.error("Не найден boot.sdi (ни рядом с Winre.wim, ни в Windows\\Boot)."); return; }
    if (!CopyFileW(W(sdi).c_str(), W(dest + "boot.sdi").c_str(), FALSE)) { c.error("Не удалось скопировать boot.sdi: " + ErrMsg(GetLastError())); return; }
    c.info("Копирование Winre.wim (" + fmtBytes(FileSize(srcWim)) + ") ...");
    if (!CopyBig(srcWim, dst, c.cancel)) { c.error("Копирование не удалось/отменено: " + ErrMsg(GetLastError())); DeleteFileW(W(dst).c_str()); return; }
    SetFileAttributesW(W(dst).c_str(), FILE_ATTRIBUTE_NORMAL);
    std::string dism = "\"" + SysTool("dism.exe") + "\"";
    ProcResult m = c.run("DISM /Mount-Image", dism + " /Mount-Image /ImageFile:\"" + dst + "\" /Index:1 /MountDir:\"" + NoSlash(mnt) + "\" /ScratchDir:\"" + NoSlash(scr) + "\"");
    if (m.exitCode != 0) { c.error("Монтирование образа не удалось (нужен драйвер wimmount; в WinPE он обычно есть)."); RemoveTree(NoSlash(dest)); return; }
    bool injected = false;
    do {
        std::string sys = mnt + "Windows\\System32\\";
        if (!PathExists(sys)) { c.error("В смонтированном образе нет Windows\\System32."); break; }
        if (!CopyFileW(W(exeSrc).c_str(), W(sys + "WinRepair.exe").c_str(), FALSE)) { c.error("Не удалось скопировать WinRepair.exe в образ: " + ErrMsg(GetLastError())); break; }
        WriteText(sys + "WinRepair.winre", "WinRepair shell marker\r\n", false);
        // winpeshl.ini: WinRepair first, then the original recovery shell (when WinRepair exits the standard WinRE menu starts)
        std::string ini = sys + "winpeshl.ini", orig = ReadSmall(ini);
        std::string lines;
        if (!orig.empty()) {
            CopyFileW(W(ini).c_str(), W(ini + ".orig").c_str(), FALSE);
            size_t p = 0;
            while (p < orig.size()) {
                size_t e = orig.find('\n', p);
                if (e == std::string::npos) e = orig.size();
                std::string l = trim(orig.substr(p, e - p));
                p = e + 1;
                if (!l.empty() && l[0] != '[' && l[0] != ';') lines += l + "\r\n";
            }
        }
        if (lines.empty()) {
            for (const char* rec : {"%SYSTEMDRIVE%\\sources\\recovery\\RecEnv.exe", "%SYSTEMROOT%\\System32\\Recovery\\RecEnv.exe"}) {
                std::string probe = mnt + (strstr(rec, "sources") ? "sources\\recovery\\RecEnv.exe" : "Windows\\System32\\Recovery\\RecEnv.exe");
                if (PathExists(probe)) { lines = std::string(rec) + "\r\n"; break; }
            }
        }
        SetFileAttributesW(W(ini).c_str(), FILE_ATTRIBUTE_NORMAL);
        WriteText(ini, "[LaunchApps]\r\n%SYSTEMROOT%\\System32\\WinRepair.exe\r\n" + lines, false);
        c.info("winpeshl.ini: WinRepair → затем: " + (lines.empty() ? std::string("(исходной оболочки не найдено)") : trim(lines)));
        injected = true;
    } while (false);
    ProcResult u = c.run(injected ? "DISM /Unmount-Image /Commit" : "DISM /Unmount-Image /Discard",
                         dism + " /Unmount-Image /MountDir:\"" + NoSlash(mnt) + "\" " + (injected ? "/Commit" : "/Discard"));
    if (!injected || u.exitCode != 0) {
        if (u.exitCode != 0) c.run("DISM /Unmount-Image /Discard", dism + " /Unmount-Image /MountDir:\"" + NoSlash(mnt) + "\" /Discard");
        c.error("Образ не создан. Файлы удалены.");
        RemoveTree(NoSlash(dest));
        return;
    }
    RemoveTree(NoSlash(mnt));
    RemoveTree(NoSlash(scr));
    AuditChange("Создан образ " + dst + " (WinRE + WinRepair)");
    std::string id;
    if (!CreateRamdiskEntry(c, store, "Windows RE + WinRepair", dst, dest + "boot.sdi", id)) { c.error("Образ создан, но запись BCD не создана."); return; }
    c.ok("Готово. Запись BCD: " + id + ". Нажмите «Войти в WinRE + WinRepair при следующей загрузке».");
}

void JobRemoveWinRepairRE(Ctx& c, const std::string& store) {
    c.info("=== Удаление «Windows RE + WinRepair» ===");
    std::string raw;
    auto entries = BcdEnum(store, raw);
    int n = 0;
    for (const auto& e : entries) {
        std::string dev = toLowerUtf8(e.get("device") + " " + e.get("osdevice") + " " + e.get("ramdisksdipath")), d = toLowerUtf8(e.desc());
        if (d.find("winrepair") == std::string::npos && dev.find("winrepairre") == std::string::npos) continue;
        AuditChange("BCD: удаление записи " + e.id() + " (" + e.desc() + ")");
        c.run("bcdedit /delete " + e.id(), BcdPrefix(store) + " /delete " + e.id() + " /f", 30);
        ++n;
    }
    c.run("Сбросить разовую загрузку", BcdPrefix(store) + " /deletevalue {bootmgr} bootsequence", 30);
    if (c.target.valid) {
        std::string dest = c.target.root + "Recovery\\WinRepairRE";
        if (PathExists(dest)) {
            c.run("DISM /Unmount-Image /Discard", "\"" + SysTool("dism.exe") + "\" /Unmount-Image /MountDir:\"" + dest + "\\mnt\" /Discard", 120);
            RemoveTree(dest);
            AuditChange("Удалена папка " + dest);
        }
    }
    c.ok(fmt("Удалено записей BCD: %d. Исходный WinRE не затрагивался.", n));
}

void NatWinReInfo(Ctx& c, OutBuf& o) {
    if (!c.needTarget()) return;
    WinReInfo r = ReadWinReInfo(c.target);
    o.add("ReAgent.xml            : " + (r.xmlFound ? r.xmlPath : std::string("не найден (WinRE не настроен)")));
    o.add("Состояние WinRE        : " + (r.installState == "1" ? std::string("включён") : r.installState == "0" ? std::string("выключен") : std::string("не определено")));
    o.add("Расположение (по OS)   : " + r.location);
    o.add("BCD-идентификатор      : " + r.bcdId);
    o.add("Winre.wim              : " + (r.wimPath.empty() ? std::string("НЕ НАЙДЕН") : r.wimPath + "  (" + fmtBytes(r.wimSize) + ")"));
    for (size_t i = 1; i < r.wimCandidates.size(); ++i) o.add("  другой кандидат     : " + r.wimCandidates[i]);
    if (r.wimPath.empty()) o.add("Подсказка: образ WinRE можно взять из install.wim (Windows\\System32\\Recovery\\Winre.wim) установочного носителя.");
}

}  // namespace wr
