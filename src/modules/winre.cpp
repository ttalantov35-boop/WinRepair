#include "winre.h"
#include "catalog.h"
#include "access.h"

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
    bool uefi = IsUefiStore(store);
    c.info(uefi ? "Прошивка: UEFI — загрузчик winload.efi" : "Прошивка: BIOS (Legacy) — загрузчик winload.exe");
    set("path", uefi ? "\\windows\\system32\\winload.efi" : "\\windows\\system32\\winload.exe");
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

// Файл RecEnv.exe в образе принадлежит TrustedInstaller, поэтому переименование может
// давать «Отказано в доступе» (0x5). Отдаём владение администраторам и выдаём им полные
// права. SID S-1-5-32-544 — встроенная группа «Администраторы», одинаковая на любом языке.
static void TakeOwnFull(Ctx& c, const std::string& path) {
    c.run("takeown /f RecEnv.exe", "\"" + SysTool("takeown.exe") + "\" /f \"" + path + "\" /a", 60);
    c.run("icacls /grant", "\"" + SysTool("icacls.exe") + "\" \"" + path + "\" /grant *S-1-5-32-544:F /c", 60);
    SetFileAttributesW(W(path).c_str(), FILE_ATTRIBUTE_NORMAL);
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
    // Загрузчик не умеет читать WIM с тома, зашифрованного BitLocker (поэтому штатный WinRE живёт
    // на отдельном незашифрованном разделе). Если целевой том зашифрован — ищем другой незашифрованный.
    std::string destRoot = t.root;
    if (BlToolPresent()) {
        std::vector<BlVolume> bl = BlScan();
        auto enc = [&](char L) { for (const auto& v : bl) if (v.letter == L && v.known) return v.encrypted || v.locked; return false; };
        if (enc(destRoot[0])) {
            c.warn(fmt("Том %c: зашифрован BitLocker: загрузчик не сможет прочитать с него образ. Ищу незашифрованный том…", destRoot[0]));
            std::string alt;
            DWORD mask = GetLogicalDrives();
            for (char L = 'C'; L <= 'Z'; ++L) {
                if (!(mask & (1u << (L - 'A')))) continue;
                std::string r = std::string(1, L) + ":\\";
                if (GetDriveTypeW(W(r).c_str()) != DRIVE_FIXED || enc(L)) continue;
                ULARGE_INTEGER fb2{}, tb2{}, tf2{};
                if (!GetDiskFreeSpaceExW(W(r).c_str(), &fb2, &tb2, &tf2) || fb2.QuadPart < need) continue;
                alt = r;
                break;
            }
            if (alt.empty()) { c.error("Все подходящие тома зашифрованы или малы. Разместить образ негде: отключите шифрование BitLocker (manage-bde -off) или подключите незашифрованный диск."); return; }
            destRoot = alt;
            c.info("Образ будет размещён на " + destRoot);
        }
    }
    ULARGE_INTEGER fb{}, tb{}, tf{};
    GetDiskFreeSpaceExW(W(destRoot).c_str(), &fb, &tb, &tf);
    if (fb.QuadPart < need) { c.error(fmt("На %s свободно %s, нужно около %s.", destRoot.c_str(), fmtBytes(fb.QuadPart).c_str(), fmtBytes(need).c_str())); return; }
    std::string dest = destRoot + "Recovery\\WinRepairRE\\", mnt = dest + "mnt\\", dst = dest + "WinRE_WR.wim", scr = dest + "scratch\\";
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

        // Запуск первым при ЛЮБОМ способе старта WinRE: подменяем RecEnv.exe на WinRepair,
        // оригинал сохраняем рядом как RecEnvOrig.exe (его запускает кнопка «Выйти в штатное меню WinRE»).
        // Нужен, когда оболочка в реестре образа вызывает RecEnv напрямую и winpeshl.ini не читается.
        bool replacedRecEnv = false;
        for (const char* sub : {"sources\\recovery\\", "Windows\\System32\\Recovery\\"}) {
            std::string rec = mnt + sub + "RecEnv.exe", orig2 = mnt + sub + "RecEnvOrig.exe";
            if (!PathExists(rec)) continue;
            SetFileAttributesW(W(rec).c_str(), FILE_ATTRIBUTE_NORMAL);
            // Переименование вместо копирования: читать файлы из смонтированного WIM система
            // может запрещать (0x780), а переименование — операция над именем, оно проходит.
            if (!PathExists(orig2) && !MoveFileW(W(rec).c_str(), W(orig2).c_str())) {
                TakeOwnFull(c, rec);   // владелец — TrustedInstaller, 0x5: берём права и пробуем ещё раз
                if (!MoveFileW(W(rec).c_str(), W(orig2).c_str())) {
                    c.warn("Не удалось переименовать RecEnv.exe (" + ErrMsg(GetLastError()) + ") — подмена пропущена.");
                    continue;
                }
            }
            if (CopyFileW(W(exeSrc).c_str(), W(rec).c_str(), FALSE)) {
                replacedRecEnv = true;
                c.info("RecEnv.exe подменён на WinRepair (оригинал сохранён как RecEnvOrig.exe) — программа запустится первой при любом способе старта WinRE.");
                AuditChange("WinRE+WinRepair: " + rec + " -> WinRepair, оригинал RecEnvOrig.exe");
            } else c.warn("Не удалось подменить RecEnv.exe (" + ErrMsg(GetLastError()) + ").");
        }
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
        if (replacedRecEnv) {   // штатное меню теперь живёт в RecEnvOrig.exe
            std::string low = toLowerUtf8(lines);
            size_t rp = low.find("recenv.exe");
            if (rp != std::string::npos) lines = lines.substr(0, rp) + "RecEnvOrig.exe" + lines.substr(rp + 10);
        }
        SetFileAttributesW(W(ini).c_str(), FILE_ATTRIBUTE_NORMAL);
        WriteText(ini, "[LaunchApps]\r\n%SYSTEMROOT%\\System32\\WinRepair.exe\r\n" + lines, false);
        c.info("winpeshl.ini: WinRepair → затем: " + (lines.empty() ? std::string("(исходной оболочки не найдено)") : trim(lines)));

        // В ряде сборок WinRE оболочка задана в реестре образа напрямую (SYSTEM\Setup\CmdLine = RecEnv),
        // и тогда winpeshl.ini игнорируется — именно поэтому WinRepair не запускался первым.
        // Приводим CmdLine к winpeshl.exe: он выполнит winpeshl.ini (WinRepair, затем штатное меню WinRE).
        {
            EnablePrivilegeByName(L"SeBackupPrivilege");
            EnablePrivilegeByName(L"SeRestorePrivilege");
            const wchar_t* MNT = L"WR_WINRE_SYS";
            RegUnLoadKeyW(HKEY_LOCAL_MACHINE, MNT);   // хвост от прошлого запуска
            // Куст в смонтированном WIM реестр открывать отказывается (0x780): работаем с копией куста,
            // правим её и кладём обратно в образ до фиксации.
            std::string hiveSrc = sys + "config\\SYSTEM", hiveTmp = dest + "SYSTEM.edit";
            bool hiveChanged = false;
            DeleteFileW(W(hiveTmp).c_str());
            bool copied = CopyFileW(W(hiveSrc).c_str(), W(hiveTmp).c_str(), FALSE) != 0;
            LONG lr = copied ? RegLoadKeyW(HKEY_LOCAL_MACHINE, MNT, W(hiveTmp).c_str()) : (LONG)GetLastError();
            if (!copied) c.warn("Не удалось скопировать куст SYSTEM из образа (" + ErrMsg((DWORD)lr) + ") — оболочка не проверена; подмена RecEnv это компенсирует.");
            else if (lr != ERROR_SUCCESS) c.warn("Не удалось загрузить копию куста SYSTEM (" + ErrMsg((DWORD)lr) + ") — оболочка не проверена; подмена RecEnv это компенсирует.");
            else {
                HKEY k = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"WR_WINRE_SYS\\Setup", 0, KEY_READ | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
                    wchar_t buf[1024] = {0};
                    DWORD cb = sizeof buf - sizeof(wchar_t), type = 0;
                    std::string cur;
                    if (RegQueryValueExW(k, L"CmdLine", nullptr, &type, (BYTE*)buf, &cb) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) cur = U(buf);
                    c.info("Оболочка образа (Setup\\CmdLine): " + (cur.empty() ? std::string("(не задана)") : cur));
                    if (toLowerUtf8(cur).find("winpeshl") == std::string::npos) {
                        static const wchar_t kShell[] = L"winpeshl.exe";
                        if (RegSetValueExW(k, L"CmdLine", 0, REG_SZ, (const BYTE*)kShell, sizeof kShell) == ERROR_SUCCESS) {
                            hiveChanged = true;
                            c.info("Setup\\CmdLine: " + (cur.empty() ? std::string("(не задана)") : cur) + " → winpeshl.exe (иначе winpeshl.ini не выполняется)");
                            AuditChange("WinRE+WinRepair: Setup\\CmdLine '" + cur + "' -> winpeshl.exe в " + dst);
                        } else c.warn("Не удалось записать Setup\\CmdLine — автозапуск может не сработать.");
                    }
                    RegCloseKey(k);
                } else c.warn("В кусте SYSTEM образа нет раздела Setup — проверка CmdLine пропущена.");
                RegUnLoadKeyW(HKEY_LOCAL_MACHINE, MNT);
                if (hiveChanged) {
                    SetFileAttributesW(W(hiveSrc).c_str(), FILE_ATTRIBUTE_NORMAL);
                    if (CopyFileW(W(hiveTmp).c_str(), W(hiveSrc).c_str(), FALSE)) c.info("Куст SYSTEM с исправленной оболочкой записан в образ.");
                    else c.warn("Не удалось записать исправленный куст SYSTEM в образ (" + ErrMsg(GetLastError()) + ") — автозапуск может не сработать.");
                }
            }
            DeleteFileW(W(hiveTmp).c_str());
        }
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

// ============================================================================================================
// Свой загрузочный WinPE с WinRepair: независимый инструмент на флешке.
// База — winpe.wim из ADK (если установлен), иначе winre.wim текущей системы.
// Все загрузочные файлы и BCD берутся из РАБОТАЮЩЕЙ Windows: читать из смонтированного образа не нужно.
// Чистый WinPE честно выполняет winpeshl.ini, поэтому WinRepair гарантированно запускается первым.

namespace {
#if defined(_M_ARM64)
const char* kPeArch = "arm64";  const char* kEfiBootName = "bootaa64.efi";
#elif defined(_M_IX86)
const char* kPeArch = "x86";    const char* kEfiBootName = "bootia32.efi";
#else
const char* kPeArch = "amd64";  const char* kEfiBootName = "bootx64.efi";
#endif

std::string EnvDir(const wchar_t* name) {
    wchar_t b[MAX_PATH] = {0};
    GetEnvironmentVariableW(name, b, MAX_PATH);
    return U(b);
}

// Корень WinPE в ADK для нашей архитектуры ("...\Windows Preinstallation Environment\amd64\")
std::string AdkPeRoot() {
    for (const wchar_t* ev : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        std::string pf = EnvDir(ev);
        if (pf.empty()) continue;
        std::string base = pf + "\\Windows Kits\\10\\Assessment and Deployment Kit\\Windows Preinstallation Environment\\" + kPeArch + "\\";
        if (IsDir(base)) return base;
    }
    return "";
}

std::string AdkWinPeWim(const std::string& peRoot) {
    std::string w = peRoot + "en-us\\winpe.wim";
    if (PathExists(w)) return w;
    for (const std::string& d : ListDirs(peRoot)) {
        w = peRoot + d + "\\winpe.wim";
        if (PathExists(w)) return w;
    }
    return "";
}

// Совпадает ли «родная» архитектура этой Windows с архитектурой собираемого PE
bool HostMatchesPeArch() {
    USHORT pm = 0, nm = 0;
    auto f = (BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*))(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
    if (!f || !f(GetCurrentProcess(), &pm, &nm) || !nm) return true;  // не узнали — считаем, что совпадает
    if (!strcmp(kPeArch, "amd64")) return nm == 0x8664;
    if (!strcmp(kPeArch, "x86"))   return nm == 0x014c;
    return nm == 0xAA64;
}

std::string WinDirLive() {
    wchar_t b[MAX_PATH] = {0};
    GetWindowsDirectoryW(b, MAX_PATH);
    return U(b);
}

bool CopyOne(Ctx& c, const std::string& src, const std::string& dst, bool required) {
    if (!PathExists(src)) { if (required) c.error("Не найден файл: " + src); return !required; }
    size_t k = dst.find_last_of('\\');
    if (k != std::string::npos) MakeDirs(dst.substr(0, k + 1));
    SetFileAttributesW(W(dst).c_str(), FILE_ATTRIBUTE_NORMAL);
    if (CopyFileW(W(src).c_str(), W(dst).c_str(), FALSE)) return true;
    if (required) c.error("Не удалось скопировать " + src + " -> " + dst + ": " + ErrMsg(GetLastError()));
    return !required;
}
}  // namespace

void JobBuildWinPE(Ctx& c, const std::string& destIn, bool baseWinre) {
    c.info("=== Свой WinPE с WinRepair ===");
    std::string dest = destIn;
    while (!dest.empty() && dest.back() == '\\') dest.pop_back();
    if (dest.size() < 2) { c.error("Укажите папку назначения (например, корень флешки E:\\)."); return; }
    dest += "\\";
    MakeDirs(dest);

    std::string exeSrc;
    {
        std::wstring b(32768, L'\0');
        DWORD n = GetModuleFileNameW(nullptr, &b[0], (DWORD)b.size());
        exeSrc = U(b.c_str(), (int)n);
    }

    // --- база: ADK или winre.wim текущей системы ---
    // Источники: образ-база (по выбору пользователя) и загрузочные файлы носителя (ADK Media, если есть).
    std::string peRoot = AdkPeRoot(), adkWim, media, baseWim;
    if (!peRoot.empty()) {
        adkWim = AdkWinPeWim(peRoot);
        media = peRoot + "Media\\";
        if (!IsDir(media)) media.clear();
    }
    bool baseAdk = !baseWinre;
    if (baseAdk) {
        baseWim = adkWim;
        if (baseWim.empty()) { c.error("ADK WinPE не установлен, а winre.wim не найден. Установите Windows ADK + дополнение WinPE (сайт Microsoft) и повторите."); return; }
        c.info("База: ADK WinPE (" + baseWim + ")");
    } else {
        if (!strcmp(kPeArch, "x86") && !EnvDir(L"ProgramFiles(x86)").empty()) { c.error("Запущена 32-битная сборка на 64-битной Windows: её winre.wim 64-битный, и WinRepair_x86 в нём не заработает. Запустите WinRepair_x64.exe или установите ADK WinPE (x86)."); return; }
        if (!HostMatchesPeArch()) { c.error("Разрядность этой Windows не совпадает с программой — её winre.wim не подойдёт как база. Установите ADK WinPE нужной архитектуры."); return; }
        WinReInfo wi = ReadWinReInfo(c.target);
        baseWim = wi.wimPath;
        if (baseWim.empty()) { c.error("winre.wim не найден — выбрать базой среду восстановления не получится. Установите ADK WinPE или восстановите WinRE."); return; }
        c.info("База: winre.wim (" + baseWim + ") — встроенное «Восстановление системы» (rstrui), как у DaRT.");
    }
    bool fromAdk = !media.empty();   // откуда берём загрузочные файлы носителя
    if (fromAdk) c.info("Загрузочные файлы носителя: ADK Media.");
    else if (!HostMatchesPeArch()) { c.error("ADK Media нет, а загрузочные файлы этой Windows другой архитектуры. Установите ADK с дополнением WinPE."); return; }

    unsigned long long wimSz = FileSize(baseWim);
    std::string wrk = DataDir() + "WinPEBuild\\", mnt = wrk + "mnt\\", scr = wrk + "scratch\\", wim = wrk + "boot.wim";
    RemoveTree(DataDir() + "WinPEBuild");
    MakeDirs(mnt); MakeDirs(scr);

    ULARGE_INTEGER fb{}, tb{}, tf{};
    GetDiskFreeSpaceExW(W(dest).c_str(), &fb, &tb, &tf);
    if (fb.QuadPart < wimSz + (100ULL << 20)) { c.error(fmt("На %s свободно %s, нужно около %s.", dest.c_str(), fmtBytes(fb.QuadPart).c_str(), fmtBytes(wimSz + (100ULL << 20)).c_str())); return; }

    c.info("Копирование базового образа (" + fmtBytes(wimSz) + ") ...");
    if (!CopyBig(baseWim, wim, c.cancel)) { c.error("Копирование не удалось/отменено."); RemoveTree(DataDir() + "WinPEBuild"); return; }
    SetFileAttributesW(W(wim).c_str(), FILE_ATTRIBUTE_NORMAL);

    std::string dism = "\"" + SysTool("dism.exe") + "\"";
    ProcResult m = c.run("DISM /Mount-Image", dism + " /Mount-Image /ImageFile:\"" + wim + "\" /Index:1 /MountDir:\"" + NoSlash(mnt) + "\" /ScratchDir:\"" + NoSlash(scr) + "\"");
    if (m.exitCode != 0) { c.error("Монтирование образа не удалось."); RemoveTree(DataDir() + "WinPEBuild"); return; }

    bool injected = false;
    do {
        std::string sys = mnt + "Windows\\System32\\";
        if (!IsDir(sys)) { c.error("В образе нет Windows\\System32."); break; }
        if (!CopyOne(c, exeSrc, sys + "WinRepair.exe", true)) break;
        int nf = 0;
        if (IsDir(ExeDir() + "Playbooks")) { CopyTree(NoSlash(ExeDir() + "Playbooks"), NoSlash(sys + "Playbooks"), nf, c.cancel); c.info(fmt("Playbooks добавлены в образ: %d файл(ов)", nf)); }
        if (IsDir(ExeDir() + "Lang")) { nf = 0; CopyTree(NoSlash(ExeDir() + "Lang"), NoSlash(sys + "Lang"), nf, c.cancel); }
        if (!baseAdk) {
            WriteText(sys + "WinRepair.winre", "WinRepair shell marker\r\n", false);
            // Плитка WinRepair в штатном меню WinRE («Устранение неполадок»), официальный механизм Microsoft:
            // инструмент и WinREConfig.xml в \sources\recovery\tools. Создаются только НОВЫЕ файлы —
            // RecEnv.exe в этом образе недоступен ни для чтения, ни для переименования, ни для перезаписи (0x780).
            std::string tools = mnt + "sources\\recovery\\tools\\";
            MakeDirs(tools);
            int tf = 0;
            bool toolOk = CopyOne(c, exeSrc, tools + "WinRepair.exe", false) && PathExists(tools + "WinRepair.exe");
            if (IsDir(ExeDir() + "Playbooks")) CopyTree(NoSlash(ExeDir() + "Playbooks"), NoSlash(tools + "Playbooks"), tf, c.cancel);
            if (IsDir(ExeDir() + "Lang")) CopyTree(NoSlash(ExeDir() + "Lang"), NoSlash(tools + "Lang"), tf, c.cancel);
            WriteText(tools + "WinRepair.winre", "WinRepair shell marker\r\n", false);
            bool cfgOk = WriteText(tools + "WinREConfig.xml",
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<!-- WinREConfig.xml -->\r\n<Recovery>\r\n  <RecoveryTools>\r\n    <RelativeFilePath>WinRepair.exe</RelativeFilePath>\r\n  </RecoveryTools>\r\n</Recovery>\r\n", false);
            if (toolOk && cfgOk) c.info("В меню WinRE добавлена плитка WinRepair: после выбора языка откройте «Поиск и устранение неисправностей» — WinRepair стоит рядом со штатными средствами.");
            else c.warn("Не удалось добавить плитку WinRepair в меню WinRE (" + ErrMsg(GetLastError()) + "). Запуск остаётся через командную строку: winrepair.");
        }
        // Восстановление системы: rstrui и компоненты из РАБОТАЮЩЕЙ Windows (в базовом WinPE их нет).
        // Копируем только отсутствующие файлы: в образе на базе winre.wim они уже есть.
        if (HostMatchesPeArch()) {
            std::string s32 = WinDirLive() + "\\System32\\";
            int added = 0;
            for (const char* fn : {"rstrui.exe", "srcore.dll", "srclient.dll", "vssapi.dll", "vsstrace.dll", "srhelper.dll", "spp.dll", "sppc.dll", "dsrole.dll"}) {
                if (PathExists(sys + fn) || !PathExists(s32 + fn)) continue;
                if (CopyOne(c, s32 + fn, sys + fn, false) && PathExists(sys + fn)) ++added;
            }
            for (const char* lang : {"ru-RU", "en-US"})
                for (const char* fn : {"rstrui.exe.mui", "srcore.dll.mui"}) {
                    std::string ms = s32 + lang + "\\" + fn, md = sys + lang + "\\" + fn;
                    if (PathExists(ms) && !PathExists(md)) CopyOne(c, ms, md, false);
                }
            if (added) c.info(fmt("Восстановление системы добавлено в образ: rstrui и компоненты (%d файл(ов)). В WinPE запускайте «Восстановление системы: rstrui…» — откат пойдёт офлайн (/offline).", added));
        } else c.warn("Разрядность этой Windows не совпадает с образом — rstrui не добавлен, откат точек восстановления в этом WinPE будет недоступен.");

        // Автозапуск WinRepair первой. На чистом WinPE (ADK) winpeshl.ini — наш новый файл и пишется свободно,
        // поэтому программа выходит сразу, без экрана языка и без RecEnv. На базе winre.wim этот файл уже есть
        // и защищён (0x780): перезапись молча не проходит, а оболочку запускает RecEnv — поэтому для автозапуска
        // нужна база ADK. Пишем и ПРОВЕРЯЕМ результат, а также дублируем через startnet.cmd.
        std::string ini = sys + "winpeshl.ini";
        bool preExisted = PathExists(ini);
        SetFileAttributesW(W(ini).c_str(), FILE_ATTRIBUTE_NORMAL);
        bool iniOk = WriteText(ini, "[LaunchApps]\r\n%SYSTEMROOT%\\System32\\wpeinit.exe\r\n%SYSTEMROOT%\\System32\\WinRepair.exe\r\n", false);
        // startnet.cmd запускается стандартным winpeshl, если наш winpeshl.ini не подхватится — как запасной путь.
        WriteText(sys + "startnet.cmd", "@echo off\r\nwpeinit\r\nstart \"\" %SYSTEMROOT%\\System32\\WinRepair.exe\r\n", false);
        if (iniOk && (!preExisted || !baseWinre))
            c.info("Автозапуск: winpeshl.ini → wpeinit → WinRepair (программа запустится первой).");
        else
            c.warn("winpeshl.ini в этом образе защищён от перезаписи (0x780) — автозапуск первой не гарантирован. Для гарантированного автозапуска соберите флешку на базе ADK WinPE (второй пункт в выборе базы).");
        injected = true;
    } while (false);

    ProcResult u = c.run(injected ? "DISM /Unmount-Image /Commit" : "DISM /Unmount-Image /Discard",
                         dism + " /Unmount-Image /MountDir:\"" + NoSlash(mnt) + "\" " + (injected ? "/Commit" : "/Discard"));
    if (!injected || u.exitCode != 0) {
        if (u.exitCode != 0) c.run("DISM /Unmount-Image /Discard", dism + " /Unmount-Image /MountDir:\"" + NoSlash(mnt) + "\" /Discard");
        c.error("Образ не собран.");
        RemoveTree(DataDir() + "WinPEBuild");
        return;
    }

    // --- загрузочные файлы носителя ---
    bool mediaOk = true;
    if (fromAdk) {
        ProcResult x = c.run("Копирование загрузочных файлов ADK", "\"" + SysTool("cmd.exe") + "\" /c xcopy /e /i /y /q \"" + NoSlash(media) + "\" \"" + NoSlash(dest) + "\\.\"", 300);
        if (x.exitCode != 0) { c.error("Не удалось скопировать загрузочные файлы ADK."); mediaOk = false; }
    } else {
        std::string win = WinDirLive();
        mediaOk = CopyOne(c, win + "\\Boot\\EFI\\bootmgfw.efi", dest + "EFI\\Boot\\" + kEfiBootName, true)
               && CopyOne(c, win + "\\Boot\\EFI\\bootmgfw.efi", dest + "EFI\\Microsoft\\Boot\\bootmgfw.efi", true);
        CopyOne(c, win + "\\Boot\\PCAT\\bootmgr", dest + "bootmgr", false);
        bool sdi = CopyOne(c, win + "\\Boot\\DVD\\EFI\\boot.sdi", dest + "Boot\\boot.sdi", false) && PathExists(dest + "Boot\\boot.sdi");
        if (!sdi) sdi = CopyOne(c, win + "\\Boot\\DVD\\PCAT\\boot.sdi", dest + "Boot\\boot.sdi", true);
        mediaOk = mediaOk && sdi;
        if (mediaOk) {   // BCD с нуля: {ramdiskoptions} + запись WinPE; копия для BIOS (winload.exe) и для UEFI (winload.efi)
            std::string st = wrk + "BCD", pre = "\"" + SysTool("bcdedit.exe") + "\" /store \"" + st + "\"";
            DeleteFileW(W(st).c_str());
            bool ok = c.run("bcdedit /createstore", "\"" + SysTool("bcdedit.exe") + "\" /createstore \"" + st + "\"", 30).exitCode == 0;
            ok = ok && c.run("ramdiskoptions", pre + " /create {ramdiskoptions} /d \"Ramdisk\"", 30).exitCode == 0;
            ok = ok && c.run("ramdisksdidevice", pre + " /set {ramdiskoptions} ramdisksdidevice boot", 30).exitCode == 0;
            ok = ok && c.run("ramdisksdipath", pre + " /set {ramdiskoptions} ramdisksdipath \\Boot\\boot.sdi", 30).exitCode == 0;
            ProcResult e = c.run("Создать запись WinPE", pre + " /create /d \"WinRepair WinPE\" /application osloader", 30);
            std::string id = Guid(e.output);
            ok = ok && !id.empty();
            if (ok) {
                auto set2 = [&](const std::string& el, const std::string& val) { ok = ok && c.run("/set " + el, pre + " /set " + id + " " + el + " " + val, 30).exitCode == 0; };
                set2("device", "ramdisk=[boot]\\sources\\boot.wim,{ramdiskoptions}");
                set2("osdevice", "ramdisk=[boot]\\sources\\boot.wim,{ramdiskoptions}");
                set2("path", "\\windows\\system32\\winload.exe");
                set2("systemroot", "\\windows");
                set2("winpe", "yes");
                set2("detecthal", "yes");
                ok = ok && c.run("{bootmgr}", pre + " /create {bootmgr} /d \"WinRepair WinPE\"", 30).exitCode == 0;
                ok = ok && c.run("{bootmgr} device", pre + " /set {bootmgr} device boot", 30).exitCode == 0;
                ok = ok && c.run("{bootmgr} timeout", pre + " /set {bootmgr} timeout 1", 30).exitCode == 0;
                ok = ok && c.run("displayorder", pre + " /displayorder " + id, 30).exitCode == 0;
                ok = ok && c.run("default", pre + " /default " + id, 30).exitCode == 0;
                ok = ok && CopyOne(c, st, dest + "Boot\\BCD", true);                       // BIOS: \Boot\BCD, winload.exe
                ok = ok && c.run("path winload.efi", pre + " /set " + id + " path \\windows\\system32\\winload.efi", 30).exitCode == 0;
                ok = ok && CopyOne(c, st, dest + "EFI\\Microsoft\\Boot\\BCD", true);       // UEFI: свой BCD, winload.efi
            }
            mediaOk = ok;
            if (!ok) c.error("Не удалось подготовить BCD носителя.");
        }
    }

    bool wimOk = mediaOk;
    if (mediaOk) {
        MakeDirs(dest + "sources\\");
        c.info("Запись boot.wim на носитель ...");
        SetFileAttributesW(W(dest + "sources\\boot.wim").c_str(), FILE_ATTRIBUTE_NORMAL);
        wimOk = CopyBig(wim, dest + "sources\\boot.wim", c.cancel);
        if (!wimOk) c.error("Не удалось записать boot.wim на носитель.");
    }
    RemoveTree(DataDir() + "WinPEBuild");
    if (!mediaOk || !wimOk) return;

    AuditChange("Создан носитель WinPE + WinRepair: " + dest + (baseAdk ? " (база ADK)" : " (база winre.wim)"));
    c.ok("Готово: " + dest);
    c.info("Как загрузиться: вставьте флешку, при включении откройте Boot Menu (обычно F12/F11/Esc/F9) и выберите её (пункт UEFI). Secure Boot выключать не нужно: загрузочные файлы подписаны Microsoft.");
    c.info("Для очень старых компьютеров с BIOS (не UEFI) флешку дополнительно нужно сделать активной и записать загрузочный код (bootsect /nt60 из установочного носителя Windows).");
    c.info("Обновление: при выходе новой версии программы просто повторите эту операцию в ту же папку.");
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
