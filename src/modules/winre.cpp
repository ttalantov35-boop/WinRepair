#include "winre.h"
#include "catalog.h"
#include "access.h"

#include <winioctl.h>

#include <cstring>
#include <cwctype>
#include <cctype>

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

#ifndef COPY_FILE_NO_BUFFERING
#define COPY_FILE_NO_BUFFERING 0x00001000
#endif

// Копирование большого файла (образы WIM). Без кэша Windows (COPY_FILE_NO_BUFFERING): на флешку данные
// пишутся крупными блоками сразу, проценты соответствуют реально записанному, а не заполнению кэша.
// Копирование идёт в отдельном потоке: «Отмена» срабатывает даже если флешка «зависла» на записи
// (pbCancel + CancelSynchronousIo). Ход записи пишется в журнал каждые 10% со скоростью; если носитель
// не принимает данные больше минуты — предупреждение в журнале.
struct CopyCtx {
    std::wstring src, dst;
    volatile BOOL stop = FALSE;
    volatile LONGLONG done = 0, total = 0;
    BOOL ok = FALSE;
    DWORD err = 0;
};
DWORD CALLBACK CopyProgress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID p) {
    auto* cc = (CopyCtx*)p;
    cc->total = total.QuadPart;
    cc->done = done.QuadPart;
    return cc->stop ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}
DWORD WINAPI CopyThread(LPVOID p) {
    auto* cc = (CopyCtx*)p;
    cc->ok = CopyFileExW(cc->src.c_str(), cc->dst.c_str(), CopyProgress, cc, (LPBOOL)&cc->stop, COPY_FILE_NO_BUFFERING);
    cc->err = cc->ok ? 0 : GetLastError();
    return 0;
}

bool CopyBig(const std::string& src, const std::string& dst, const std::atomic<bool>* cancel, bool journal = false) {
    auto* cc = new CopyCtx;
    cc->src = W(src);
    cc->dst = W(dst);
    HANDLE th = CreateThread(nullptr, 0, CopyThread, cc, 0, nullptr);
    if (!th) { DWORD e = GetLastError(); delete cc; SetLastError(e); return false; }
    ULONGLONG t0 = GetTickCount64(), lastUi = 0, lastMove = t0, lastStallWarn = t0;
    LONGLONG lastDone = 0;
    int nextPct = 10;
    bool cancelSent = false;
    for (;;) {
        DWORD w = WaitForSingleObject(th, 250);
        ULONGLONG now = GetTickCount64();
        LONGLONG done = cc->done, total = cc->total;
        if (done != lastDone) { lastDone = done; lastMove = now; }
        if (total > 0 && now - lastUi > 500) {
            lastUi = now;
            Logger::I().progress(fmt("Копирование: %.0f%% (%s из %s)", 100.0 * (double)done / (double)total,
                                     fmtBytes((uint64_t)done).c_str(), fmtBytes((uint64_t)total).c_str()));
        }
        if (journal && total > 0) {
            int pct = (int)(100.0 * (double)done / (double)total);
            while (pct >= nextPct && nextPct <= 100) {
                double sec = (double)(now - t0) / 1000.0;
                Logger::I().logFile(Lvl::Info, fmt("  записано %d%% (%s), %.1f МБ/с", nextPct, fmtBytes((uint64_t)done).c_str(),
                                               sec > 0 ? (double)done / 1048576.0 / sec : 0.0));
                nextPct += 10;
            }
            if (!cancelSent && now - lastMove > 60000 && now - lastStallWarn > 60000) {
                lastStallWarn = now;
                Logger::I().log(Lvl::Warn, fmt("  носитель не принимает данные уже %u с (записано %s) — флешка очень медленная или неисправна; можно нажать «Отмена».",
                                               (unsigned)((now - lastMove) / 1000), fmtBytes((uint64_t)done).c_str()));
            }
        }
        if (w == WAIT_OBJECT_0) break;
        if (!cancelSent && cancel && cancel->load()) {
            cancelSent = true;
            cc->stop = TRUE;
            CancelSynchronousIo(th);   // прерывает зависшую запись на носитель
            Logger::I().log(Lvl::Warn, "  копирование прерывается по запросу отмены ...");
        }
        if (cancelSent) {
            CancelSynchronousIo(th);
            if (WaitForSingleObject(th, 30000) != WAIT_OBJECT_0) {
                // Поток не вышел (драйвер носителя не отвечает) — не ждём бесконечно. Контекст не освобождаем:
                // поток ещё может к нему обратиться.
                CloseHandle(th);
                Logger::I().progress("");
                Logger::I().log(Lvl::Warn, "  носитель не отвечает — копирование брошено; извлеките флешку и вставьте заново.");
                SetLastError(ERROR_OPERATION_ABORTED);
                return false;
            }
            break;
        }
    }
    CloseHandle(th);
    Logger::I().progress("");
    bool ok = cc->ok != 0;
    DWORD err = cancelSent && !ok ? ERROR_OPERATION_ABORTED : cc->err;
    delete cc;
    SetLastError(err);
    return ok;
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
#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000
#endif
#ifndef RRF_SUBKEY_WOW6432KEY
#define RRF_SUBKEY_WOW6432KEY 0x00020000
#endif
std::string AdkPeRoot() {
    // 1) Где установлен Windows Kits 10 — по реестру (ADK мог быть установлен не в Program Files, на другой диск).
    for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
        wchar_t buf[MAX_PATH * 2] = L"";
        DWORD cb = sizeof buf;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots", L"KitsRoot10",
                         RRF_RT_REG_SZ | (view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : RRF_SUBKEY_WOW6464KEY), nullptr, buf, &cb) == ERROR_SUCCESS && buf[0]) {
            std::string kits = U(buf);
            if (kits.back() != '\\') kits += "\\";
            std::string base = kits + "Assessment and Deployment Kit\\Windows Preinstallation Environment\\" + kPeArch + "\\";
            if (IsDir(base)) return base;
        }
    }
    // 2) Стандартные пути.
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

// Образ ISO подключается на время сборки и обязательно отключается (в том числе при ошибках)
// Строка для PowerShell в одинарных кавычках: апостроф внутри пути удваивается.
std::string PsQuote(const std::string& s) { std::string r; for (char ch : s) { r += ch; if (ch == '\'') r += '\''; } return r; }

std::string PowerShellExe() { return "\"" + WinDirLive() + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\""; }

// Образ ISO, выбранный для сборки, отключается в конце сборки (и при ошибке/отмене) — чтобы в системе
// не оставался лишний виртуальный DVD-диск.
struct IsoMount {
    Ctx& c; std::string iso; bool mounted = false; char letter = 0;
    explicit IsoMount(Ctx& cc) : c(cc) {}
    ~IsoMount() { Close(); }
    void Close() {
        if (!mounted) return;
        mounted = false;
        std::string what = letter ? std::string(1, letter) + ":" : iso;
        if (DismountIsoImage(iso)) c.info("Образ ISO отключён (" + what + ").");
        else c.warn("Не удалось отключить образ ISO (" + what + ") — отключите его вручную: правый клик по диску → «Извлечь».");
    }
};

// Корень установочного носителя Windows (с sources\boot.wim): буква диска, папка, файл boot.wim или файл .iso
std::string ResolveInstallMedia(Ctx& c, IsoMount& im, std::string p) {
    p = trim(p);
    while (!p.empty() && p.front() == '"') p.erase(p.begin());   // путь из «Копировать как путь» — в кавычках
    while (!p.empty() && p.back() == '"') p.pop_back();
    p = trim(p);
    while (p.size() > 3 && p.back() == '\\') p.pop_back();
    if (p.empty()) return "";
    std::string low = toLowerUtf8(p);
    if (low.size() > 4 && low.compare(low.size() - 4, 4, ".iso") == 0) {
        if (!PathExists(p)) { c.error("Файл образа не найден: " + p); return ""; }
        im.iso = p;
        c.info("Подключение образа ISO (ждём букву диска, до 30 с) ...");
        IsoMountResult r = MountIsoImage(p, 30);
        // Отключаем в конце сборки образ, выбранный для сборки, даже если не удалось узнать его букву —
        // иначе он так и останется подключённым.
        im.mounted = r.attachedByUs || r.ok;
        im.letter = r.letter;
        if (!r.ok) {
            c.error("Не удалось подключить образ ISO" + (r.error.empty() ? std::string(".") : ": " + r.error));
            c.info("Подключите образ вручную (правый клик по файлу → «Подключить») и выберите его букву в панели 3 — «Папка или диск».");
            return "";
        }
        c.info(std::string("Образ ISO подключён как ") + r.letter + ":\\ — после сборки будет отключён.");
        return std::string(1, r.letter) + ":\\";
    }
    if (low.size() > 8 && low.compare(low.size() - 8, 8, "boot.wim") == 0 && PathExists(p)) {
        size_t k = low.find("sources\\");
        return k == std::string::npos ? "" : p.substr(0, k);
    }
    if (p.size() == 2 && p[1] == ':') p += "\\";
    if (p.back() != '\\') p += "\\";
    return p;
}
}  // namespace

// Подключение ISO. Mount-DiskImage возвращается раньше, чем Windows присвоит образу букву диска, поэтому
// букву ждём (опрос каждые 0,5 с). Раньше программа спрашивала букву сразу, получала пусто, сообщала
// «не удалось подключить», а образ при этом оставался подключённым — со второго раза всё работало.
IsoMountResult MountIsoImage(const std::string& isoPath, int waitSec) {
    IsoMountResult res;
    if (waitSec < 5) waitSec = 5;
    if (!PathExists(isoPath) || IsDir(isoPath)) { res.error = "файл не найден: " + isoPath; return res; }
    if (!PathExists(WinDirLive() + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe")) {
        res.error = "в этой системе нет PowerShell (например, в WinPE) — подключить ISO нельзя; распакуйте ISO в папку или возьмите установочную флешку";
        return res;
    }
    DWORD before = GetLogicalDrives();
    std::string script =
        "$ErrorActionPreference='Stop'; $p='" + PsQuote(isoPath) + "'; "
        "$i=Get-DiskImage -ImagePath $p; "
        "if($i.Attached){'WAS=1'} else {'WAS=0'; Mount-DiskImage -ImagePath $p -StorageType ISO | Out-Null}; "
        "$l=$null; for($n=0; $n -lt " + std::to_string(waitSec * 2) + " -and -not $l; $n++){ "
        "$l=(Get-DiskImage -ImagePath $p | Get-Volume -ErrorAction SilentlyContinue | Select-Object -First 1).DriveLetter; "
        "if(-not $l){ Start-Sleep -Milliseconds 500 } }; "
        "'LETTER='+$l";
    ProcResult r = RunProcess(PowerShellExe() + " -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + script + "\"", "",
                              [](const std::string&, bool) {}, nullptr, waitSec + 90);
    const std::string& o = r.output;
    bool was = o.find("WAS=1") != std::string::npos, triedMount = o.find("WAS=0") != std::string::npos;
    size_t lp = o.find("LETTER=");
    if (lp != std::string::npos && lp + 7 < o.size() && isalpha((unsigned char)o[lp + 7])) res.letter = (char)toupper((unsigned char)o[lp + 7]);
    // Запасной путь: буква так и не пришла — ищем новый CD/DVD-диск, появившийся после подключения.
    if (!res.letter && triedMount) {
        for (int n = 0; n < 20 && !res.letter; ++n) {
            DWORD now = GetLogicalDrives(), added = now & ~before;
            for (int i = 2; i < 26 && !res.letter; ++i) {
                if (!(added & (1u << i))) continue;
                wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
                if (GetDriveTypeW(root) == DRIVE_CDROM) res.letter = (char)('A' + i);
            }
            if (!res.letter) Sleep(500);
        }
    }
    res.attachedByUs = triedMount;
    res.ok = res.letter != 0;
    if (!res.ok) {
        std::string tail = trim(o);
        size_t k = tail.find("LETTER=");
        if (k != std::string::npos) tail = trim(tail.substr(0, k));
        for (const char* w : {"WAS=0", "WAS=1"}) { size_t q = tail.find(w); if (q != std::string::npos) tail.erase(q, 5); }
        tail = trim(tail);
        if (tail.size() > 300) tail = tail.substr(tail.size() - 300);
        if (!r.started) res.error = "PowerShell не запустился: " + r.error;
        else if (r.timedOut) res.error = "PowerShell не ответил вовремя";
        else if (!tail.empty()) res.error = tail;
        else if (was || triedMount) res.error = "образ подключён, но Windows не присвоила ему букву диска";
        else res.error = "образ не подключился";
    }
    return res;
}

bool DismountIsoImage(const std::string& isoPath) {
    if (isoPath.empty()) return true;
    ProcResult r = RunProcess(PowerShellExe() + " -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Dismount-DiskImage -ImagePath '" + PsQuote(isoPath) + "' | Out-Null\"", "",
                              [](const std::string&, bool) {}, nullptr, 90);
    return r.started && !r.timedOut && r.exitCode == 0;
}

namespace {
// ---- Подготовка WinPE из чужого boot.wim (Windows 10/11, оригинальные и «авторские» сборки) ----
// Разные ISO устроены немного по-разному: в boot.wim может быть один или два образа, образ №1 бывает
// и программой установки, путь системы бывает задан как X:\$windows.~bt (тогда WinPE не стартует:
// «actual SYSTEMROOT (X:\windows) is different from the configured one»), оболочка в реестре — setup.exe.
// Всё это проверяется и приводится к виду обычного WinPE.

struct WimImageInfo { int index = 0; std::string name, desc; };

// Значение строки «Ключ : значение» из вывода DISM (ключи на английском или русском).
std::string DismField(const std::string& line, std::initializer_list<const char*> keys) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return "";
    std::string k = line.substr(0, colon);
    for (size_t q; (q = k.find("\xC2\xA0")) != std::string::npos;) k.replace(q, 2, " ");   // русский DISM: неразрывный пробел перед «:»
    k = trim(k);
    for (const char* key : keys)
        if (toLowerUtf8(k) == toLowerUtf8(key)) return trim(line.substr(colon + 1));
    return "\x01";   // ключ не тот
}

std::vector<WimImageInfo> ListWimImages(Ctx& c, const std::string& wimPath) {
    std::vector<WimImageInfo> v;
    ProcResult r = c.run("DISM /Get-WimInfo (список образов)", "\"" + SysTool("dism.exe") + "\" /Get-WimInfo /WimFile:\"" + wimPath + "\"", 120);
    size_t p = 0;
    const std::string& o = r.output;
    while (p < o.size()) {
        size_t e = o.find('\n', p);
        std::string line = o.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? o.size() : e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string s = DismField(line, {"Index", "Индекс"});
        if (s != "\x01" && !s.empty()) { WimImageInfo w; w.index = atoi(s.c_str()); if (w.index > 0) v.push_back(w); continue; }
        if (v.empty()) continue;
        s = DismField(line, {"Name", "Имя"});
        if (s != "\x01") { v.back().name = s; continue; }
        s = DismField(line, {"Description", "Описание"});
        if (s != "\x01") v.back().desc = s;
    }
    return v;
}

// Какой образ брать: первый, который не «Setup / Программа установки»; иначе №1.
int PickWinPeIndex(Ctx& c, const std::vector<WimImageInfo>& imgs) {
    if (imgs.empty()) return 1;
    for (const auto& w : imgs) c.info(fmt("  образ №%d: %s", w.index, (w.name.empty() ? std::string("(без имени)") : w.name).c_str()));
    for (const auto& w : imgs) {
        std::string n = toLowerUtf8(w.name + " " + w.desc);
        bool setup = n.find("setup") != std::string::npos || n.find("установк") != std::string::npos;
        if (!setup) return w.index;
    }
    c.warn("Все образы в boot.wim похожи на программу установки — берётся №1, при сборке он будет приведён к обычному WinPE.");
    return imgs.front().index;
}

// Удаление папки с файлами из образа Windows (владелец TrustedInstaller, только чтение для администраторов):
// открытие с FILE_FLAG_BACKUP_SEMANTICS при включённой SeRestorePrivilege даёт право DELETE мимо ACL.
void ForceDeleteOne(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), DELETE | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); if (!DeleteFileW(p.c_str())) RemoveDirectoryW(p.c_str()); return; }
    FILE_BASIC_INFO bi{};
    if (GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof bi)) {
        bi.FileAttributes = (bi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        SetFileInformationByHandle(h, FileBasicInfo, &bi, sizeof bi);
    }
    FILE_DISPOSITION_INFO di{};
    di.DeleteFile = TRUE;
    SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof di);
    CloseHandle(h);
}
void ForceRemoveTreeW(const std::wstring& dir, int depth) {
    if (depth > 128) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ForceRemoveTreeW(p, depth + 1);
            ForceDeleteOne(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}
void ForceRemoveTree(const std::string& dir) {
    EnablePrivilegeByName(L"SeBackupPrivilege");
    EnablePrivilegeByName(L"SeRestorePrivilege");
    EnablePrivilegeByName(L"SeTakeOwnershipPrivilege");
    std::wstring d = W(NoSlash(dir));
    ForceRemoveTreeW(d, 0);
    ForceDeleteOne(d);
    RemoveDirectoryW(d.c_str());
}

// Копирование с правами резервного копирования (SeBackup/SeRestore): кусты реестра и системные файлы образа
// закрыты ACL от администраторов — обычный CopyFile получает «Отказано в доступе» (0x5). Возвращает 0 или код ошибки.
DWORD BackupCopyFile(const std::string& src, const std::string& dst) {
    EnablePrivilegeByName(L"SeBackupPrivilege");
    EnablePrivilegeByName(L"SeRestorePrivilege");
    HANDLE in = CreateFileW(W(src).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) return GetLastError();
    SetFileAttributesW(W(dst).c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE out = CreateFileW(W(dst).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (out == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); CloseHandle(in); return e; }
    std::vector<char> buf(1 << 20);
    DWORD rd = 0, wr = 0, err = 0;
    for (;;) {
        if (!ReadFile(in, buf.data(), (DWORD)buf.size(), &rd, nullptr)) { err = GetLastError(); break; }
        if (!rd) break;
        if (!WriteFile(out, buf.data(), rd, &wr, nullptr) || wr != rd) { err = GetLastError(); if (!err) err = ERROR_WRITE_FAULT; break; }
    }
    if (!err && !SetEndOfFile(out)) err = GetLastError();
    CloseHandle(out);
    CloseHandle(in);
    return err;
}

// «…\$windows.~bt\…» → «…\…» (без учёта регистра). "X:\$windows.~bt\Windows" → "X:\Windows", "X:\$windows.~bt" → "X:\".
std::wstring StripBtPath(const std::wstring& s, int& n) {
    static const std::wstring kBt = L"\\$windows.~bt";
    std::wstring low = s;
    for (auto& ch : low) ch = (wchar_t)towlower(ch);
    std::wstring out;
    size_t p = 0;
    for (;;) {
        size_t f = low.find(kBt, p);
        if (f == std::wstring::npos) { out.append(s, p, std::wstring::npos); break; }
        out.append(s, p, f - p);
        size_t after = f + kBt.size();
        if (after >= s.size() || s[after] != L'\\') out += L'\\';
        p = after;
        ++n;
    }
    return out;
}

// Обход раздела реестра: все строковые значения (REG_SZ / EXPAND_SZ / MULTI_SZ) с $windows.~bt исправляются.
int FixBtInKey(HKEY k, int depth) {
    if (depth > 64) return 0;
    int n = 0;
    std::vector<wchar_t> name(16384);
    for (DWORD i = 0;; ++i) {
        DWORD nl = (DWORD)name.size(), type = 0, cb = 0;
        LONG r = RegEnumValueW(k, i, name.data(), &nl, nullptr, &type, nullptr, &cb);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        if ((type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) || cb < 8) continue;
        std::vector<BYTE> data((size_t)cb + 4, 0);
        DWORD cb2 = cb;
        nl = (DWORD)name.size();
        if (RegEnumValueW(k, i, name.data(), &nl, nullptr, &type, data.data(), &cb2) != ERROR_SUCCESS) continue;
        std::wstring v((const wchar_t*)data.data(), cb2 / sizeof(wchar_t));
        int c = 0;
        std::wstring fixed = StripBtPath(v, c);
        if (c && RegSetValueExW(k, name.data(), 0, type, (const BYTE*)fixed.data(), (DWORD)(fixed.size() * sizeof(wchar_t))) == ERROR_SUCCESS) n += c;
    }
    wchar_t sk[256];
    for (DWORD i = 0;; ++i) {
        DWORD sl = 256;
        LONG r = RegEnumKeyExW(k, i, sk, &sl, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        HKEY ck = nullptr;   // REG_OPTION_BACKUP_RESTORE: доступ по привилегиям резервного копирования, мимо ACL раздела
        if (RegCreateKeyExW(k, sk, 0, nullptr, REG_OPTION_BACKUP_RESTORE, KEY_ALL_ACCESS, nullptr, &ck, nullptr) == ERROR_SUCCESS) {
            n += FixBtInKey(ck, depth + 1);
            RegCloseKey(ck);
        }
    }
    return n;
}

// Правка куста из подключённого образа через копию: прямую загрузку куста из WIM система запрещает (0x780).
// Журналы куста (.LOG1/.LOG2) копируются вместе с ним — при загрузке они применяются; после записи исправленного
// куста журналы в образе удаляются, иначе при старте WinPE старый журнал мог бы откатить правку.
bool EditImageHive(Ctx& c, const std::string& hivePath, const std::string& tmpDir, const wchar_t* mountName, const std::function<bool(HKEY)>& edit, bool* loaded = nullptr) {
    if (loaded) *loaded = false;
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, mountName);
    std::string tmp = tmpDir + U(mountName) + ".hive";
    for (const char* x : {"", ".LOG1", ".LOG2"}) { SetFileAttributesW(W(tmp + x).c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(W(tmp + x).c_str()); }
    if (DWORD e = BackupCopyFile(hivePath, tmp)) { c.warn("Куст " + hivePath + " не прочитан (" + ErrMsg(e) + ")."); return false; }
    SetFileAttributesW(W(tmp).c_str(), FILE_ATTRIBUTE_NORMAL);
    for (const char* x : {".LOG1", ".LOG2"})
        if (PathExists(hivePath + x) && BackupCopyFile(hivePath + x, tmp + x) == 0) SetFileAttributesW(W(tmp + x).c_str(), FILE_ATTRIBUTE_NORMAL);
    LONG lr = RegLoadKeyW(HKEY_LOCAL_MACHINE, mountName, W(tmp).c_str());
    if (lr != ERROR_SUCCESS) { c.warn("Куст " + hivePath + " не загружен (" + ErrMsg((DWORD)lr) + ")."); return false; }
    if (loaded) *loaded = true;
    HKEY root = nullptr;
    bool changed = false;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, mountName, 0, nullptr, REG_OPTION_BACKUP_RESTORE, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS) {
        changed = edit(root);
        RegFlushKey(root);
        RegCloseKey(root);
    }
    LONG ur = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, mountName);
    if (!changed) return false;
    if (ur != ERROR_SUCCESS) { c.warn("Куст " + hivePath + " не выгружен (" + ErrMsg((DWORD)ur) + ") — правка не записана."); return false; }
    SetFileAttributesW(W(hivePath).c_str(), FILE_ATTRIBUTE_NORMAL);
    if (DWORD e = BackupCopyFile(tmp, hivePath)) { c.warn("Исправленный куст не записан в образ: " + hivePath + " (" + ErrMsg(e) + ")."); return false; }
    for (const char* x : {".LOG1", ".LOG2"}) {
        std::string lg = hivePath + x;
        if (!PathExists(lg)) continue;
        ForceDeleteOne(W(lg));   // журнал куста закрыт ACL — удаляем с правами восстановления
    }
    return true;
}

// Приведение чужого WinPE к обычному виду: путь системы X:\Windows и оболочка winpeshl.exe.
// Два независимых способа: DISM /Set-TargetPath и прямая правка реестра образа (SYSTEM, SOFTWARE, DEFAULT);
// затем проверка. Работает и для «авторских» сборок, где образ взят из программы установки ($windows.~bt).
bool NormalizeWinPeImage(Ctx& c, const std::string& mnt, const std::string& tmpDir) {
    std::string dism = "\"" + SysTool("dism.exe") + "\"", img = " /Image:\"" + NoSlash(mnt) + "\"";
    ProcResult g = c.run("DISM /Get-TargetPath", dism + img + " /Get-TargetPath", 120);
    bool bt = toLowerUtf8(g.output).find("~bt") != std::string::npos;
    if (bt) c.warn("В образе задан путь системы программы установки (X:\\$windows.~bt) — с ним WinPE не запускается. Исправляем на X:\\.");
    ProcResult s = c.run("DISM /Set-TargetPath:X:\\", dism + img + " /Set-TargetPath:X:\\", 120);
    if (s.exitCode == 0) c.info("Путь системы WinPE: X:\\ (SYSTEMROOT = X:\\Windows).");
    else c.warn("DISM /Set-TargetPath не выполнен — путь исправляется напрямую в реестре образа.");

    EnablePrivilegeByName(L"SeBackupPrivilege");
    EnablePrivilegeByName(L"SeRestorePrivilege");
    c.info("Проверка реестра образа на пути программы установки ($windows.~bt) ...");
    std::string cfg = mnt + "Windows\\System32\\config\\";
    int total = 0;
    bool hivesRead = true;   // все кусты образа удалось прочитать и проверить
    struct Hv { const char* file; const wchar_t* mount; };
    for (Hv h : {Hv{"SYSTEM", L"WR_PE_SYSTEM"}, Hv{"SOFTWARE", L"WR_PE_SOFTWARE"}, Hv{"DEFAULT", L"WR_PE_DEFAULT"}}) {
        if (!PathExists(cfg + h.file)) continue;
        bool isSys = !strcmp(h.file, "SYSTEM");
        int n = 0;
        bool loaded = false;
        EditImageHive(c, cfg + h.file, tmpDir, h.mount, [&](HKEY root) {
            n = FixBtInKey(root, 0);
            bool ch = n > 0;
            if (isSys) {   // оболочка: SYSTEM\Setup\CmdLine. В образе программы установки там setup.exe — winpeshl.ini не выполнится.
                HKEY k = nullptr;
                if (RegOpenKeyExW(root, L"Setup", 0, KEY_READ | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
                    wchar_t buf[1024] = {0};
                    DWORD cb = sizeof buf - sizeof(wchar_t), type = 0;
                    std::string cur;
                    if (RegQueryValueExW(k, L"CmdLine", nullptr, &type, (BYTE*)buf, &cb) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) cur = U(buf);
                    c.info("Оболочка образа (Setup\\CmdLine): " + (cur.empty() ? std::string("(не задана)") : cur));
                    if (toLowerUtf8(cur).find("winpeshl") == std::string::npos) {
                        static const wchar_t kShell[] = L"winpeshl.exe";
                        if (RegSetValueExW(k, L"CmdLine", 0, REG_SZ, (const BYTE*)kShell, sizeof kShell) == ERROR_SUCCESS) {
                            ch = true;
                            c.info("Setup\\CmdLine → winpeshl.exe (иначе winpeshl.ini и автозапуск WinRepair не выполняются).");
                        } else c.warn("Не удалось записать Setup\\CmdLine — автозапуск может не сработать.");
                    }
                    RegCloseKey(k);
                }
            }
            return ch;
        }, &loaded);
        if (!loaded) hivesRead = false;
        if (n) c.info(fmt("Куст %s: исправлено путей $windows.~bt → X:\\: %d", h.file, n));
        total += n;
    }
    if (hivesRead && total == 0) c.info("В реестре образа путей $windows.~bt нет.");

    ProcResult g2 = c.run("DISM /Get-TargetPath (проверка)", dism + img + " /Get-TargetPath", 120);
    bool dismOk = g2.exitCode == 0, btLeft = toLowerUtf8(g2.output).find("~bt") != std::string::npos;
    if (dismOk && btLeft) {
        c.error("После исправления DISM всё ещё сообщает путь $windows.~bt — с таким образом WinPE не загрузится.");
        return false;
    }
    if (dismOk || hivesRead) {
        c.info(dismOk ? "Проверка пройдена: путь системы образа — X:\\ (подтверждено DISM)." : "Проверка пройдена: реестр образа проверен, путей $windows.~bt не осталось.");
        return true;
    }
    c.error("Путь системы образа проверить не удалось: DISM не обслуживает образ, а реестр образа не читается.");
    return false;
}
}  // namespace

bool WinPeAdkInstalled() { return !AdkWinPeWim(AdkPeRoot()).empty(); }

// ---- Флешка для режима BIOS (Legacy): активный раздел + загрузочный код BOOTMGR ----
// Делается только для флешки/USB-диска и никогда — для диска, с которого работает Windows.
namespace {
struct UsbVol { bool ok = false; DWORD disk = 0; LONGLONG offset = 0; bool usb = false, removable = false; };

UsbVol QueryVolume(char letter) {
    UsbVol v;
    std::wstring dev = L"\\\\.\\" + std::wstring(1, (wchar_t)letter) + L":";
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return v;
    BYTE buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)] = {0};
    DWORD br = 0;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf, sizeof buf, &br, nullptr)) {
        auto* e = (VOLUME_DISK_EXTENTS*)buf;
        if (e->NumberOfDiskExtents == 1) { v.disk = e->Extents[0].DiskNumber; v.offset = e->Extents[0].StartingOffset.QuadPart; v.ok = true; }
    }
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE d[1024] = {0};
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, d, sizeof d, &br, nullptr)) {
        auto* sd = (STORAGE_DEVICE_DESCRIPTOR*)d;
        v.usb = sd->BusType == BusTypeUsb;
        v.removable = sd->RemovableMedia != 0;
    }
    CloseHandle(h);
    if (GetDriveTypeW((std::wstring(1, (wchar_t)letter) + L":\\").c_str()) == DRIVE_REMOVABLE) v.removable = true;
    return v;
}

// 0 — MBR, раздел активный; 1 — MBR, не активный; 2 — GPT; -1 — не удалось узнать (в т.ч. флешка без таблицы разделов)
int UsbPartitionState(DWORD disk, LONGLONG offset) {
    std::wstring dev = L"\\\\.\\PhysicalDrive" + std::to_wstring(disk);
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    std::vector<BYTE> b(sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 128 * sizeof(PARTITION_INFORMATION_EX));
    DWORD br = 0;
    int r = -1;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, b.data(), (DWORD)b.size(), &br, nullptr)) {
        auto* L = (DRIVE_LAYOUT_INFORMATION_EX*)b.data();
        if (L->PartitionStyle == PARTITION_STYLE_GPT) r = 2;
        else if (L->PartitionStyle == PARTITION_STYLE_MBR)
            for (DWORD i = 0; i < L->PartitionCount; ++i)
                if (L->PartitionEntry[i].StartingOffset.QuadPart == offset) { r = L->PartitionEntry[i].Mbr.BootIndicator ? 0 : 1; break; }
    }
    CloseHandle(h);
    return r;
}

// srcRoot — корень установочного носителя (там boot\bootsect.exe), если есть.
void PrepareUsbForBios(Ctx& c, const std::string& dest, const std::string& srcRoot) {
    if (dest.size() != 3 || dest[1] != ':') {
        c.info("WinPE записан в папку, а не в корень флешки — загрузочный код для BIOS не записывается.");
        return;
    }
    char L = (char)toupper((unsigned char)dest[0]);
    UsbVol v = QueryVolume(L);
    std::string wd = WinDirLive();
    UsbVol sys = wd.empty() ? UsbVol() : QueryVolume((char)toupper((unsigned char)wd[0]));
    if (!v.ok) { c.warn("Не удалось определить диск носителя " + dest + " — загрузочный код для BIOS не записан."); return; }
    if (sys.ok && sys.disk == v.disk) { c.warn("Носитель " + dest + " на том же диске, что и Windows, — активный раздел и загрузочный код не трогаем."); return; }
    if (!v.usb && !v.removable) {
        c.warn("Носитель " + dest + " — не флешка и не USB-диск: активный раздел и загрузочный код автоматически не меняются. Для BIOS: bootsect /nt60 " + std::string(1, L) + ": /mbr.");
        return;
    }
    c.info("Подготовка флешки для режима BIOS (Legacy): активный раздел и загрузочный код ...");
    int st = UsbPartitionState(v.disk, v.offset);
    if (st == 2) {
        c.warn("Флешка размечена в GPT: в режиме UEFI она загрузится, а в BIOS (Legacy) — нет. Для BIOS нужна разметка MBR (в «Управлении дисками» или diskpart: clean, convert mbr — ВСЕ данные на флешке удалятся), затем повторите сборку.");
        return;
    }
    if (st == 1) {
        std::string scr = DataDir() + "diskpart_active.txt";
        WriteText(scr, "select volume " + std::string(1, L) + "\r\nactive\r\nexit\r\n", false);
        c.run("diskpart: раздел флешки — активный", "\"" + SysTool("diskpart.exe") + "\" /s \"" + scr + "\"", 120);
        DeleteFileW(W(scr).c_str());
        if (UsbPartitionState(v.disk, v.offset) == 0) c.info("Раздел флешки сделан активным.");
        else c.warn("Не удалось сделать раздел флешки активным — в BIOS (Legacy) она может не загрузиться.");
    } else if (st == 0) c.info("Раздел флешки уже активный.");
    else c.info("Таблица разделов флешки не прочитана (возможно, флешка без разделов) — активность раздела не меняется.");

    std::vector<std::string> cand = {dest + "Boot\\bootsect.exe"};
    std::string pe = AdkPeRoot();
    if (!pe.empty()) cand.push_back(Dir(NoSlash(Dir(NoSlash(pe)))) + "Deployment Tools\\" + kPeArch + "\\BCDBoot\\bootsect.exe");
    if (!srcRoot.empty()) cand.push_back(srcRoot + "boot\\bootsect.exe");
    if (!wd.empty()) cand.push_back(wd + "\\System32\\bootsect.exe");
    std::string bs;
    for (const auto& p : cand) if (PathExists(p)) { bs = p; break; }
    if (bs.empty()) {
        c.warn("bootsect.exe не найден (нет ADK и установочного носителя) — загрузочный код не записан. Если флешка не загрузится в BIOS: bootsect /nt60 " + std::string(1, L) + ": /mbr (из папки boot установочного носителя Windows).");
        return;
    }
    ProcResult r = c.run("bootsect /nt60 — загрузочный код BOOTMGR для BIOS", "\"" + bs + "\" /nt60 " + std::string(1, L) + ": /force /mbr", 120);
    if (r.exitCode == 0) c.info("Загрузочный код BOOTMGR записан в загрузочный сектор раздела и в MBR флешки — флешка готова и для BIOS (Legacy).");
    else c.warn("bootsect завершился с ошибкой — в режиме BIOS (Legacy) флешка может не загрузиться (в UEFI — загрузится).");
}
}  // namespace

BootMediaInfo QueryBootMedia(const std::string& pathIn) {
    BootMediaInfo b;
    std::string p = trim(pathIn);
    while (!p.empty() && p.front() == '"') p.erase(p.begin());
    while (!p.empty() && p.back() == '"') p.pop_back();
    if (p.size() == 2 && p[1] == ':') p += "\\";
    b.isRoot = p.size() == 3 && p[1] == ':' && p[2] == '\\';
    if (!b.isRoot) { b.exists = !p.empty() && IsDir(NoSlash(p)); return b; }
    std::wstring root = W(p);
    UINT dt = GetDriveTypeW(root.c_str());
    b.exists = dt != DRIVE_NO_ROOT_DIR && dt != DRIVE_UNKNOWN;
    if (!b.exists) return b;
    wchar_t fs[64] = L"";
    if (GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs, 64)) b.fs = U(fs);
    ULARGE_INTEGER tot{};
    if (GetDiskFreeSpaceExW(root.c_str(), nullptr, &tot, nullptr)) b.size = tot.QuadPart;
    UsbVol v = QueryVolume((char)toupper((unsigned char)p[0]));
    std::string wd = WinDirLive();
    UsbVol s = wd.empty() ? UsbVol() : QueryVolume((char)toupper((unsigned char)wd[0]));
    b.usb = v.usb;
    b.removable = v.removable;
    b.systemDisk = v.ok && s.ok && v.disk == s.disk;
    if (v.ok) { int st = UsbPartitionState(v.disk, v.offset); b.gpt = st == 2; b.active = st == 0; }
    b.hasWinPe = PathExists(p + "bootmgr") && PathExists(p + "sources\\boot.wim");
    return b;
}

void JobPrepareBiosBoot(Ctx& c, const std::string& destIn) {
    c.info("=== Флешка WinPE: загрузка в режиме BIOS (Legacy) ===");
    std::string dest = trim(destIn);
    if (dest.size() == 2 && dest[1] == ':') dest += "\\";
    if (!dest.empty() && dest.back() != '\\') dest += "\\";
    if (!PathExists(dest + "bootmgr") || !PathExists(dest + "Boot\\BCD") || !PathExists(dest + "sources\\boot.wim")) {
        c.error("На " + dest + " нет файлов WinPE (bootmgr, Boot\\BCD, sources\\boot.wim). Сначала соберите WinPE на эту флешку или скопируйте его туда.");
        return;
    }
    PrepareUsbForBios(c, dest, "");
    AuditChange("Флешка " + dest + " подготовлена для загрузки в BIOS (Legacy)");
    c.ok("Готово: " + dest);
}

void JobBuildWinPE(Ctx& c, const std::string& destIn, int baseMode, const std::string& mediaSrc, const std::string& isoToDismount) {
    c.info("=== Свой WinPE с WinRepair ===");
    // На экране — только шаги, предупреждения и итог; команды DISM/bcdedit и их вывод — в лог-файле («Открыть лог-файл»).
    struct QuietGuard { Ctx& c; bool was; ~QuietGuard() { c.quiet = was; } } quietGuard{c, c.quiet};
    c.quiet = true;
    c.info("Подробности каждой команды пишутся в лог-файл (кнопка «Открыть лог-файл»).");
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
    bool baseWinre = baseMode == 0, baseMedia = baseMode == 2, baseAdk = baseMode == 1;
    IsoMount iso(c);
    if (!isoToDismount.empty()) {   // подключён заранее окном сборки — отключим в конце
        iso.iso = isoToDismount;
        iso.mounted = true;
        std::string m = trim(mediaSrc);
        if (m.size() >= 2 && m[1] == ':' && isalpha((unsigned char)m[0])) iso.letter = (char)toupper((unsigned char)m[0]);
    }
    std::string mediaWim;   // sources\boot.wim установочного носителя
    int peIndex = 1;        // какой образ из boot.wim взять (обычно №1 — WinPE, №2 — программа установки)
    if (baseMedia) {
        // Источник: файл .wim напрямую (boot.wim из распакованного ISO — где бы он ни лежал), .iso, диск или папка.
        std::string src = trim(mediaSrc);
        while (!src.empty() && src.front() == '"') src.erase(src.begin());
        while (!src.empty() && src.back() == '"') src.pop_back();
        std::string lowSrc = toLowerUtf8(src);
        if (lowSrc.size() > 4 && lowSrc.compare(lowSrc.size() - 4, 4, ".wim") == 0 && PathExists(src) && !IsDir(src)) {
            mediaWim = src;
            c.info("Источник: файл образа " + mediaWim);
        } else {
            std::string root = ResolveInstallMedia(c, iso, mediaSrc);
            if (root.empty()) { c.error("Укажите установочный образ Windows: файл .iso, букву диска с подключённым образом/установочной флешки или папку, где есть sources\\boot.wim."); return; }
            mediaWim = root + "sources\\boot.wim";
        }
        if (!PathExists(mediaWim)) { c.error("На носителе нет sources\\boot.wim: " + mediaWim); return; }
        peIndex = PickWinPeIndex(c, ListWimImages(c, mediaWim));
        ProcResult gi = c.run(fmt("DISM /Get-WimInfo (boot.wim, образ №%d)", peIndex), "\"" + SysTool("dism.exe") + "\" /Get-WimInfo /WimFile:\"" + mediaWim + "\"" + fmt(" /Index:%d", peIndex), 120);
        const char* want = !strcmp(kPeArch, "amd64") ? "x64" : (!strcmp(kPeArch, "x86") ? "x86" : "ARM64");
        std::string o = gi.output;
        size_t ap = o.find("Architecture");
        if (ap == std::string::npos) ap = o.find("Архитектура");   // DISM на русской Windows
        if (ap != std::string::npos) {
            size_t e = o.find('\n', ap);
            std::string line = o.substr(ap, e == std::string::npos ? std::string::npos : e - ap);
            if (line.find(want) == std::string::npos) { c.error("Разрядность образа Windows не совпадает с этой программой (нужна " + std::string(want) + "). Возьмите образ той же разрядности."); return; }
        }
        // Тип образа: нужен WindowsPE (install.wim с полной Windows сюда не подходит).
        size_t ip = o.find("Installation");
        if (ip == std::string::npos) ip = o.find("Установка");
        if (ip != std::string::npos) {
            size_t e = o.find('\n', ip);
            std::string line = o.substr(ip, e == std::string::npos ? std::string::npos : e - ip);
            if (line.find("WindowsPE") == std::string::npos) { c.error("Выбранный образ — не WinPE (" + trim(line) + "). Нужен boot.wim, а не install.wim/install.esd."); return; }
        }
        baseWim = mediaWim;
        c.info("База: установочный образ Windows (" + mediaWim + ", WinPE) — ADK не нужен.");
        c.warn("Образ Windows лучше брать той же версии, что и ваша система: «Восстановление системы» (rstrui) копируется из работающей Windows и может не заработать на сильно другой сборке.");
    }
    if (baseMedia) {
    } else if (baseAdk) {
        // winpe.wim можно указать вручную (если ADK стоит в нестандартном месте или скопирован с другого ПК).
        std::string manual = trim(mediaSrc);
        while (!manual.empty() && manual.front() == '"') manual.erase(manual.begin());
        while (!manual.empty() && manual.back() == '"') manual.pop_back();
        if (!manual.empty() && PathExists(manual) && !IsDir(manual)) {
            adkWim = manual;
            // ...\<arch>\en-us\winpe.wim → загрузочные файлы ...\<arch>\Media
            std::string up = Dir(NoSlash(Dir(manual)));
            if (!up.empty() && IsDir(up + "Media")) media = up + "Media\\";
            c.info("ADK WinPE указан вручную: " + adkWim + (media.empty() ? std::string(" (папки Media рядом нет — загрузочные файлы возьмём из этой Windows)") : std::string()));
        }
        baseWim = adkWim;
        if (baseWim.empty()) { c.error("ADK WinPE не установлен. Выберите вариант «Установочный образ Windows» (нужен ISO или установочная флешка Windows) или установите Windows ADK + дополнение WinPE (сайт Microsoft)."); return; }
        c.info("База: ADK WinPE (" + baseWim + ")");
    } else {
        if (!strcmp(kPeArch, "x86") && !EnvDir(L"ProgramFiles(x86)").empty()) { c.error("Запущена 32-битная сборка на 64-битной Windows: её winre.wim 64-битный, и WinRepair_x86 в нём не заработает. Запустите WinRepair_x64.exe или установите ADK WinPE (x86)."); return; }
        if (!HostMatchesPeArch()) { c.error("Разрядность этой Windows не совпадает с программой — её winre.wim не подойдёт как база. Установите ADK WinPE нужной архитектуры."); return; }
        WinReInfo wi = ReadWinReInfo(c.target);
        baseWim = wi.wimPath;
        if (baseWim.empty()) { c.error("winre.wim не найден — выбрать базой среду восстановления не получится. Установите ADK WinPE или восстановите WinRE."); return; }
        c.info("База: winre.wim (" + baseWim + ") — со встроенным «Восстановлением системы» (rstrui).");
    }
    // Откуда берём загрузочные файлы носителя (bootmgr, BCD, boot.sdi, EFI):
    // 1) из того же установочного носителя, что и boot.wim: версии загрузчика и WinPE совпадают,
    //    BCD — тот самый, с которым этот носитель грузится и в BIOS, и в UEFI; 2) ADK Media; 3) эта Windows + свой BCD.
    std::string ownRoot;
    if (baseMedia && !mediaWim.empty()) {
        std::string r = Dir(NoSlash(Dir(mediaWim)));   // ...\sources\boot.wim → корень носителя
        if (!r.empty() && PathExists(r + "bootmgr") && PathExists(r + "boot\\BCD") && PathExists(r + "boot\\boot.sdi") &&
            PathExists(r + "efi\\microsoft\\boot\\BCD") && PathExists(r + "efi\\boot\\" + kEfiBootName))
            ownRoot = r;
        else if (!r.empty()) c.info("В установочном носителе нет полного набора загрузочных файлов (bootmgr, boot\\BCD, boot\\boot.sdi, efi) — берём их из ADK или этой Windows.");
    }
    bool fromOwn = !ownRoot.empty();
    bool fromAdk = !fromOwn && !media.empty();
    if (peRoot.empty()) c.detail("ADK WinPE не найден (искали по реестру Windows Kits и в Program Files).");
    if (fromOwn) c.info("Загрузочные файлы носителя: из того же установочного образа (" + NoSlash(ownRoot) + ") — версии загрузчика и WinPE совпадают.");
    else if (fromAdk) c.info("Загрузочные файлы носителя: ADK Media.");
    else if (!HostMatchesPeArch()) { c.error("ADK Media нет, а загрузочные файлы этой Windows другой архитектуры. Установите ADK с дополнением WinPE."); return; }
    else c.info("Загрузочные файлы носителя: из этой Windows, BCD создаётся заново.");

    unsigned long long wimSz = FileSize(baseWim);
    std::string wrk = DataDir() + "WinPEBuild\\", mnt = wrk + "mnt\\", scr = wrk + "scratch\\", wim = wrk + "boot.wim";
    // Рабочая копия образа (~0.5–1 ГБ) нужна только на время сборки и НЕ хранится: папка WinPEBuild
    // удаляется при любом выходе из сборки — успех, ошибка или отмена. В Data остаются только журналы.
    // Если прошлая сборка оборвалась с подключённым образом — сначала отключаем его.
    auto cleanBuildDir = [wrk, mnt]() {
        if (IsDir(mnt + "Windows"))
            RunProcess("\"" + SysTool("dism.exe") + "\" /Unmount-Image /MountDir:\"" + NoSlash(mnt) + "\" /Discard", "",
                       [](const std::string&, bool) {}, nullptr, 600);
        if (!IsDir(NoSlash(wrk))) return;
        if (IsDir(mnt + "Windows")) { Logger::I().log(Lvl::Warn, "Образ в " + NoSlash(mnt) + " всё ещё подключён — рабочая папка не удалена. Выполните: dism /Cleanup-Mountpoints"); return; }
        ForceRemoveTree(NoSlash(wrk));   // в т.ч. распакованный образ (файлы TrustedInstaller)
        if (IsDir(NoSlash(wrk))) Logger::I().log(Lvl::Warn, "Не удалось полностью удалить рабочую папку сборки: " + NoSlash(wrk) + " — удалите её вручную.");
        else Logger::I().log(Lvl::Info, "Рабочая копия образа удалена (" + NoSlash(wrk) + "); сохранены только журналы.");
    };
    struct BuildDirGuard { std::function<void()> f; ~BuildDirGuard() { f(); } } buildDirGuard{cleanBuildDir};
    cleanBuildDir();
    MakeDirs(mnt); MakeDirs(scr);

    ULARGE_INTEGER fb{}, tb{}, tf{};
    GetDiskFreeSpaceExW(W(dest).c_str(), &fb, &tb, &tf);
    if (fb.QuadPart < wimSz + (100ULL << 20)) { c.error(fmt("На %s свободно %s, нужно около %s.", dest.c_str(), fmtBytes(fb.QuadPart).c_str(), fmtBytes(wimSz + (100ULL << 20)).c_str())); return; }

    std::string dism = "\"" + SysTool("dism.exe") + "\"";
    // Образ всегда ПЕРЕПАКОВЫВАЕТСЯ в обычное сжатие LZX (/Compress:max), а не копируется как есть.
    // Причина: «авторские» и новые образы бывают сжаты LZMS (recovery/solid, как ESD) — такой образ DISM подключает,
    // но драйвер wimmount не отдаёт из него файлы (0x780), а обслуживание DISM падает с 0xC1510114
    // («образ необходимо подключить повторно»). После перепаковки образ правится как обычный.
    {
        int srcIdx = baseMedia ? peIndex : 1;
        c.info(fmt("Извлечение и перепаковка образа №%d в обычное сжатие (LZX) ...", srcIdx));
        ProcResult ex = c.run(fmt("DISM /Export-Image (индекс %d, /Compress:max)", srcIdx),
                              dism + " /Export-Image /SourceImageFile:\"" + baseWim + "\"" + fmt(" /SourceIndex:%d", srcIdx) + " /DestinationImageFile:\"" + wim + "\" /Compress:max /ScratchDir:\"" + NoSlash(scr) + "\"", 1800);
        if (ex.exitCode != 0 || !PathExists(wim)) {
            DeleteFileW(W(wim).c_str());
            if (baseMedia) { c.error("Не удалось извлечь WinPE из установочного образа."); cleanBuildDir(); return; }
            c.warn("Перепаковка не удалась — копируем образ как есть (" + fmtBytes(wimSz) + ") ...");
            if (!CopyBig(baseWim, wim, c.cancel)) { c.error("Копирование не удалось/отменено."); cleanBuildDir(); return; }
        }
    }
    SetFileAttributesW(W(wim).c_str(), FILE_ATTRIBUTE_NORMAL);

    ProcResult m = c.run("DISM /Mount-Image", dism + " /Mount-Image /ImageFile:\"" + wim + "\" /Index:1 /MountDir:\"" + NoSlash(mnt) + "\" /ScratchDir:\"" + NoSlash(scr) + "\"");
    if (m.exitCode != 0) { c.error("Монтирование образа не удалось."); cleanBuildDir(); return; }

    // Проверка: отдаёт ли подключённый образ файлы. Если нет (0x780) — второй путь: распаковать образ в папку
    // (DISM /Apply-Image), править там как обычные файлы и упаковать заново (DISM /Capture-Image).
    bool applyMode = false;
    {
        std::string probe = wrk + "probe.hive";
        // Читаем с правами резервного копирования: обычный CopyFile на кусте реестра всегда даёт 0x5 (ACL) — это не поломка.
        DWORD pe = BackupCopyFile(mnt + "Windows\\System32\\config\\SYSTEM", probe);
        bool readable = pe == 0;
        SetFileAttributesW(W(probe).c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(W(probe).c_str());
        if (!readable) {
            c.warn("Подключённый образ не отдаёт файлы (" + ErrMsg(pe) + ") — правим его другим способом: распаковка в папку, правка, упаковка заново.");
            c.run("DISM /Unmount-Image /Discard", dism + " /Unmount-Image /MountDir:\"" + NoSlash(mnt) + "\" /Discard", 600);
            std::string ap = wrk + "apply\\";
            MakeDirs(ap);
            ProcResult a = c.run("DISM /Apply-Image", dism + " /Apply-Image /ImageFile:\"" + wim + "\" /Index:1 /ApplyDir:\"" + NoSlash(ap) + "\" /ScratchDir:\"" + NoSlash(scr) + "\"", 1800);
            if (a.exitCode != 0 || !IsDir(ap + "Windows\\System32")) { c.error("Не удалось распаковать образ в папку — этот образ использовать не получится. Возьмите другой boot.wim или базу ADK."); cleanBuildDir(); return; }
            mnt = ap;
            applyMode = true;
            c.info("Образ распакован в " + NoSlash(ap) + " — дальше правка идёт как с обычными файлами.");
            // Файлы распакованного образа принадлежат TrustedInstaller: даём администраторам права ровно на то,
            // что программа меняет (папки, куда добавляет файлы, startnet/winpeshl, кусты реестра и их журналы).
            std::string s32 = ap + "Windows\\System32\\";
            for (const std::string& f : {NoSlash(s32), s32 + "config", s32 + "ru-RU", s32 + "en-US", s32 + "startnet.cmd", s32 + "winpeshl.ini",
                                         s32 + "config\\SYSTEM", s32 + "config\\SYSTEM.LOG1", s32 + "config\\SYSTEM.LOG2",
                                         s32 + "config\\SOFTWARE", s32 + "config\\SOFTWARE.LOG1", s32 + "config\\SOFTWARE.LOG2",
                                         s32 + "config\\DEFAULT", s32 + "config\\DEFAULT.LOG1", s32 + "config\\DEFAULT.LOG2"})
                if (PathExists(f)) TakeOwnFull(c, f);
        }
    }
    if (!PathExists(mnt + "Windows\\System32\\winpeshl.exe"))
        c.warn("В образе нет Windows\\System32\\winpeshl.exe — это не похоже на WinPE; флешка может не загрузиться.");
    if ((baseMedia || baseAdk) && !NormalizeWinPeImage(c, mnt, wrk)) {   // путь X:\ и оболочка winpeshl.exe (чужой boot.wim, winpe.wim)
        if (!applyMode) c.run("DISM /Unmount-Image /Discard", dism + " /Unmount-Image /MountDir:\"" + NoSlash(mnt) + "\" /Discard", 600);
        c.error("Образ не собран: флешка с ним не загрузилась бы. Возьмите другой boot.wim (лучше оригинальный ISO Microsoft) или базу ADK.");
        cleanBuildDir();
        return;
    }
    // Загрузчики внутри образа: winload.exe — для режима BIOS (Legacy), winload.efi — для UEFI. «Авторские» сборки
    // иногда выбрасывают один из них; тогда флешка на такой прошивке падает с «\windows\system32\winload.exe 0xc0000225».
    // Недостающий берём из System32\boot самого образа, иначе — из этой Windows (ветка 19041 / 22621 и т.п. совместима по загрузчику).
    {
        std::string s32 = mnt + "Windows\\System32\\", hostS32 = WinDirLive() + "\\System32\\";
        if (!PathExists(s32 + "ntoskrnl.exe")) c.warn("В образе нет Windows\\System32\\ntoskrnl.exe — образ повреждён или это не WinPE.");
        for (const char* ld : {"winload.exe", "winload.efi"}) {
            bool bios = !strcmp(ld, "winload.exe");
            if (PathExists(s32 + ld)) { c.detail(std::string("Загрузчик ") + ld + (bios ? " (BIOS/Legacy)" : " (UEFI)") + ": есть."); continue; }
            std::string from = PathExists(s32 + "boot\\" + ld) ? s32 + "boot\\" + ld : (PathExists(hostS32 + ld) ? hostS32 + ld : std::string());
            if (from.empty()) {
                c.warn(std::string("В образе нет ") + ld + " и взять его негде — флешка не загрузится в режиме " + (bios ? "BIOS (Legacy)" : "UEFI") + ".");
                continue;
            }
            if (CopyFileW(W(from).c_str(), W(s32 + ld).c_str(), FALSE)) {
                SetFileAttributesW(W(s32 + ld).c_str(), FILE_ATTRIBUTE_NORMAL);
                c.warn(std::string("В образе не было ") + ld + " (загрузчик для " + (bios ? "BIOS/Legacy" : "UEFI") + ") — добавлен из " + from + ".");
            } else c.warn(std::string("Не удалось добавить ") + ld + " в образ (" + ErrMsg(GetLastError()) + ") — флешка не загрузится в режиме " + (bios ? "BIOS (Legacy)." : "UEFI."));
        }
    }

    bool injected = false;
    do {
        std::string sys = mnt + "Windows\\System32\\";
        if (!IsDir(sys)) { c.error("В образе нет Windows\\System32."); break; }
        if (!CopyOne(c, exeSrc, sys + "WinRepair.exe", true)) break;
        int nf = 0;
        if (IsDir(ExeDir() + "Playbooks")) { CopyTree(NoSlash(ExeDir() + "Playbooks"), NoSlash(sys + "Playbooks"), nf, c.cancel); c.detail(fmt("Playbooks добавлены в образ: %d файл(ов)", nf)); }
        if (IsDir(ExeDir() + "Lang")) { nf = 0; CopyTree(NoSlash(ExeDir() + "Lang"), NoSlash(sys + "Lang"), nf, c.cancel); }
        // Язык интерфейса в WinPE — как сейчас: WinPE обычно английский (en-US), и без этого файла программа открылась бы на английском.
        const char* langIni = CurrentLang() == Lang::EN ? "lang=en\r\n" : "lang=ru\r\n";
        WriteText(sys + "WinRepair.ini", langIni, false);
        c.detail(std::string("Язык программы в WinPE: ") + (CurrentLang() == Lang::EN ? "English" : "русский") + " (WinRepair.ini).");
        if (baseWinre) {
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
            WriteText(tools + "WinRepair.ini", CurrentLang() == Lang::EN ? "lang=en\r\n" : "lang=ru\r\n", false);
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
            c.warn("winpeshl.ini в этом образе защищён от перезаписи (0x780) — автозапуск первой не гарантирован. Для гарантированного автозапуска соберите флешку на базе ADK WinPE или установочного образа Windows.");
        injected = true;
    } while (false);

    ProcResult u;
    if (applyMode) {   // распакованный образ: упаковываем папку в новый WIM
        if (injected) {
            std::string wim2 = wrk + "boot_new.wim";
            u = c.run("DISM /Capture-Image", dism + " /Capture-Image /ImageFile:\"" + wim2 + "\" /CaptureDir:\"" + NoSlash(mnt) + "\" /Name:\"WinRepair WinPE\" /Compress:max /ScratchDir:\"" + NoSlash(scr) + "\"", 1800);
            if (u.exitCode == 0 && PathExists(wim2)) wim = wim2;
            else if (u.exitCode == 0) u.exitCode = 1;
        }
    } else
        u = c.run(injected ? "DISM /Unmount-Image /Commit" : "DISM /Unmount-Image /Discard",
                  dism + " /Unmount-Image /MountDir:\"" + NoSlash(mnt) + "\" " + (injected ? "/Commit" : "/Discard"));
    if (!injected || u.exitCode != 0) {
        if (!applyMode && u.exitCode != 0) c.run("DISM /Unmount-Image /Discard", dism + " /Unmount-Image /MountDir:\"" + NoSlash(mnt) + "\" /Discard");
        c.error("Образ не собран.");
        cleanBuildDir();
        return;
    }

    // Образ помечается загружаемым — как в оригинальных носителях Microsoft (пересжатия нет, это быстро).
    {
        std::string fin = wrk + "boot_final.wim";
        DeleteFileW(W(fin).c_str());
        ProcResult b = c.run("DISM /Export-Image /Bootable", dism + " /Export-Image /SourceImageFile:\"" + wim + "\" /SourceIndex:1 /DestinationImageFile:\"" + fin + "\" /Bootable /ScratchDir:\"" + NoSlash(scr) + "\"", 1200);
        if (b.exitCode == 0 && PathExists(fin)) wim = fin;
        else { DeleteFileW(W(fin).c_str()); c.warn("Пометить образ загружаемым не удалось — используется образ без пометки."); }
    }

    // --- загрузочные файлы носителя ---
    bool mediaOk = true;
    if (fromOwn) {
        mediaOk = CopyOne(c, ownRoot + "bootmgr", dest + "bootmgr", true);
        if (PathExists(ownRoot + "bootmgr.efi")) CopyOne(c, ownRoot + "bootmgr.efi", dest + "bootmgr.efi", false);
        for (const char* d : {"boot", "efi"}) {
            ProcResult x = c.run(std::string("Копирование загрузочных файлов носителя: \\") + d,
                                 "\"" + SysTool("cmd.exe") + "\" /c xcopy /e /i /y /q /h /r \"" + ownRoot + d + "\" \"" + dest + d + "\"", 300);
            if (x.exitCode != 0) { c.error(std::string("Не удалось скопировать папку ") + d + " установочного носителя."); mediaOk = false; }
        }
    } else if (fromAdk) {
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
        c.info("Размер: " + fmtBytes(FileSize(wim)) + ". Ход записи — в строке состояния внизу окна.");
        ULONGLONG wt0 = GetTickCount64();
        wimOk = CopyBig(wim, dest + "sources\\boot.wim", c.cancel, true);
        if (!wimOk) {
            DWORD e = GetLastError();
            DeleteFileW(W(dest + "sources\\boot.wim").c_str());   // недописанный образ не оставляем — флешка с ним не загрузится
            if (c.cancelled()) c.warn("Запись boot.wim отменена; недописанный файл на носителе удалён.");
            else c.error("Не удалось записать boot.wim на носитель: " + ErrMsg(e));
        } else {
            double sec = (double)(GetTickCount64() - wt0) / 1000.0;
            c.info(fmt("boot.wim записан: %s за %.0f с (%.1f МБ/с).", fmtBytes(FileSize(dest + "sources\\boot.wim")).c_str(), sec,
                       sec > 0 ? (double)FileSize(dest + "sources\\boot.wim") / 1048576.0 / sec : 0.0));
        }
    }
    cleanBuildDir();
    if (!mediaOk || !wimOk) return;

    // BIOS (Legacy): активный раздел + загрузочный код — автоматически, чтобы не делать это вручную.
    std::string srcRoot;
    if (baseMedia && !mediaWim.empty()) {
        std::string r = Dir(NoSlash(Dir(mediaWim)));   // ...\sources\boot.wim → корень носителя
        if (!r.empty() && IsDir(r + "boot")) srcRoot = r;
    }
    PrepareUsbForBios(c, dest, srcRoot);

    // Самопроверка: что реально лежит на носителе (для разбора, если флешка не загрузится).
    // На экран — одна строка итога; полный вывод bcdedit и DISM — в лог-файле.
    {
        ProcResult bb = c.run("Проверка: BCD носителя (BIOS: \\Boot\\BCD)", "\"" + SysTool("bcdedit.exe") + "\" /store \"" + dest + "Boot\\BCD\" /enum all", 30);
        ProcResult bw = c.run("Проверка: boot.wim на носителе", dism + " /Get-WimInfo /WimFile:\"" + dest + "sources\\boot.wim\" /Index:1", 120);
        std::string lo = toLowerUtf8(bb.output);
        bool bcdOk = bb.exitCode == 0 && lo.find("ramdisk") != std::string::npos && lo.find("winload") != std::string::npos;
        bool wimInfoOk = bw.exitCode == 0;
        std::string arch;
        for (const char* key : {"Architecture", "Архитектура"}) {
            size_t ap = bw.output.find(key);
            if (ap == std::string::npos) continue;
            size_t e = bw.output.find('\n', ap), cl = bw.output.find(':', ap);
            if (cl != std::string::npos && (e == std::string::npos || cl < e)) arch = trim(bw.output.substr(cl + 1, e == std::string::npos ? std::string::npos : e - cl - 1));
            break;
        }
        if (bcdOk && wimInfoOk) c.info("Проверка носителя: загрузочная запись есть, boot.wim читается" + (arch.empty() ? std::string(".") : " (" + arch + ")."));
        else c.warn(std::string("Проверка носителя: ") + (bcdOk ? "" : "загрузочная запись BCD не найдена или не читается; ") +
                    (wimInfoOk ? "" : "boot.wim на носителе не читается; ") + "подробности — в лог-файле.");
    }
    iso.Close();   // образ ISO больше не нужен — отключаем до итоговых сообщений

    AuditChange("Создан носитель WinPE + WinRepair: " + dest + (baseAdk ? " (база ADK)" : baseMedia ? " (база: установочный образ Windows)" : " (база winre.wim)"));
    c.ok("Готово: " + dest);
    c.info("Как загрузиться: вставьте флешку, при включении откройте Boot Menu (обычно F12/F11/Esc/F9) и выберите её — пункт UEFI или, на старых компьютерах, USB-HDD (BIOS/Legacy). Secure Boot выключать не нужно: загрузочные файлы подписаны Microsoft.");
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
