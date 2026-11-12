#include "triage.h"
#include "../core/bsod_catalog.h"
#include "../core/dumpheader.h"
#include "winre.h"
#include "autoruns.h"
#include "playbook.h"
#include "../ui_tools.h"   // UiConfirm

#include <cfgmgr32.h>
#include <setupapi.h>
#include <winioctl.h>

#include <cstring>

namespace wr {

namespace {

std::vector<Finding> g_find;

const char* kSymptoms[] = {
    "Windows не загружается (цикл, чёрный экран, «восстановление»)",
    "Синий экран (BSOD)",
    "Диск или Windows не виден",
    "Подозрение на вирус / блокировки / подмена настроек",
    "Нет сети / интернета",
    "Тормозит, зависает, перезагружается сам",
    "Обновления не ставятся / откат изменений",
};

TAct ACat(int no, const std::string& l) { TAct a; a.k = TAct::Catalog; a.no = no; a.label = l; return a; }
TAct AFix(std::vector<int> ids, const std::string& l) { TAct a; a.k = TAct::Fixes; a.ids = ids; a.label = l; return a; }
TAct AWin(const std::string& n, const std::string& l) { TAct a; a.k = TAct::Window; a.name = n; a.label = l; return a; }
TAct AMod(const std::string& n, const std::string& l) { TAct a; a.k = TAct::Module; a.name = n; a.label = l; return a; }

void AddF(Ctx& c, Sev s, const std::string& title, const std::string& detail, const TAct& a = TAct()) {
    Finding f;
    f.sev = s; f.title = title; f.detail = detail; f.act = a;
    g_find.push_back(f);
    std::string line = std::string(s == Sev::Bad ? "ПРОБЛЕМА" : s == Sev::Warn ? "ВНИМАНИЕ" : "ок") + ": " + title;
    if (s == Sev::Bad) c.error(line); else if (s == Sev::Warn) c.warn(line); else c.info(line);
    if (!detail.empty()) c.info("    " + truncUtf8(detail, 400));
}

TAct ActionForCategory(const std::string& cat) {
    if (cat == "memory" || cat == "memory_or_driver") return AMod("memory", "Проверить память (SMBIOS/диагностика), затем драйверы");
    if (cat == "hardware") return AMod("disk", "Проверить диски (SMART), затем память и температуры");
    if (cat == "disk" || cat == "filesystem" || cat == "driver_or_disk") return ACat(64, "chkdsk /scan на томе ОС и проверка SMART");
    if (cat == "boot") return AWin("bcd", "Проверить BCD и режим контроллера диска (SATA/VMD)");
    if (cat == "os_integrity" || cat == "registry" || cat == "os_or_driver") return AMod("integrity", "Проверка целостности (DISM/SFC без изменений)");
    if (cat == "gpu") return ACat(42, "Список драйверов; обновить/откатить драйвер GPU");
    return AMod("dump", "Разобрать дамп (заголовок + cdb !analyze) и найти модуль");
}

std::string Lower(const std::string& s) { return toLowerUtf8(s); }

// ---- probes -----------------------------------------------------------------------------------------------------
void PTarget(Ctx& c) {
    if (!c.target.valid) { AddF(c, Sev::Bad, "Целевая Windows не выбрана/не найдена", "Выберите ОС в списке. Если диска или системы нет в списке — см. находки про BitLocker и драйверы хранилища.", AWin("storage", "BitLocker и драйверы хранилища")); return; }
    if (!PathExists(c.target.winDir + "\\System32\\config\\SYSTEM")) { AddF(c, Sev::Bad, "На выбранном томе нет System32\\config\\SYSTEM", c.target.winDir, AWin("storage", "Проверить, тот ли это том")); return; }
    AddF(c, Sev::Info, "Целевая ОС: " + c.target.label, "");
}

void PBitlocker(Ctx& c) {
    if (!BlToolPresent()) { AddF(c, Sev::Info, "manage-bde отсутствует в этой среде", "Проверка BitLocker невозможна (в WinRE он есть). Если тома не открываются — загрузитесь в WinRE.", AWin("winre", "Войти в WinRE")); return; }
    int locked = 0;
    for (auto& v : BlScan()) if (v.locked) { ++locked; AddF(c, Sev::Bad, fmt("Том %c: заблокирован BitLocker", v.letter), "Без разблокировки Windows на нём не видна и не ремонтируется.", AWin("storage", "Разблокировать том ключом восстановления")); }
    if (!locked) AddF(c, Sev::Info, "Заблокированных BitLocker-томов нет", "");
}

void PDisks(Ctx& c) {
    auto d = CollectDisks();
    if (d.empty()) { AddF(c, Sev::Bad, "Физические диски не видны", "Нет драйвера контроллера (Intel VMD/RST, NVMe, RAID) или контроллер отключён в BIOS.", AWin("storage", "Устройства без драйвера → drvload")); return; }
    int bad = 0;
    for (auto& x : d) {
        std::string n = fmt("Disk %d %s", x.number, x.model.c_str());
        if (x.health != "Healthy") { ++bad; AddF(c, Sev::Bad, n + ": SMART прогнозирует отказ", x.note, AMod("disk", "Скопировать данные и заменить диск; проверить SMART")); }
        if (x.readErr > 0) { ++bad; AddF(c, Sev::Bad, n + fmt(": ошибки носителя (%lld)", x.readErr), "Pending/uncorrectable сектора или MediaErrors NVMe. Не запускайте chkdsk /r до копии данных.", AMod("disk", "Диски и SMART подробно")); }
        if (x.wear >= 80) { ++bad; AddF(c, Sev::Warn, n + fmt(": износ SSD %d%%", x.wear), "", AMod("disk", "Диски и SMART подробно")); }
        if (x.temp >= 70) { ++bad; AddF(c, Sev::Warn, n + fmt(": высокая температура %d°C", x.temp), "Проверьте охлаждение/радиатор NVMe.", AMod("disk", "Диски и SMART подробно")); }
    }
    if (!bad) AddF(c, Sev::Info, fmt("Диски: %zu, состояние SMART в норме", d.size()), "");
}

void PNoDriver(Ctx& c) {
    std::vector<std::string> lines;
    bool vmd = false;
    int n = NoDriverDevices(lines, vmd);
    if (n == 0) { AddF(c, Sev::Info, "Устройств без драйвера (код 28) нет", ""); return; }
    std::string d;
    for (size_t i = 0; i < lines.size() && i < 6; ++i) d += lines[i] + "; ";
    AddF(c, vmd ? Sev::Bad : Sev::Warn, vmd ? "Обнаружен Intel VMD/RST без драйвера — NVMe-диски не видны" : fmt("Устройств без драйвера: %d", n), d,
         AWin("storage", "Найти INF и загрузить драйвер (drvload)"));
}

void PDevices(Ctx& c) {
    HDEVINFO h = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (h == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA dd{};
    dd.cbSize = sizeof dd;
    int n = 0;
    std::string names;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &dd); ++i) {
        ULONG st = 0, pr = 0;
        if (CM_Get_DevNode_Status(&st, &pr, dd.DevInst, 0) != CR_SUCCESS || !(st & DN_HAS_PROBLEM) || pr == 0 || pr == 22 || pr == 28) continue;
        wchar_t b[512] = {0};
        DWORD sz = 0;
        SetupDiGetDeviceRegistryPropertyW(h, &dd, SPDRP_DEVICEDESC, nullptr, (PBYTE)b, sizeof b - 2, &sz);
        if (n < 6) names += U(b) + fmt(" (код %lu); ", (unsigned long)pr);
        ++n;
    }
    SetupDiDestroyDeviceInfoList(h);
    if (n) AddF(c, Sev::Warn, fmt("Устройства с ошибкой драйвера: %d", n), names, AMod("drivers", "Драйверы и устройства с ошибками"));
    else AddF(c, Sev::Info, "Устройств с ошибками драйверов нет", "");
}

void PBoot(Ctx& c) {
    if (!c.target.valid) return;
    int dn = VolumeDiskNumber(c.target.letter());
    std::vector<uint8_t> lay;
    int fw = 0;
    {
        typedef BOOL(WINAPI * PGetFirmwareType)(DWORD*);
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        PGetFirmwareType f = k ? (PGetFirmwareType)GetProcAddress(k, "GetFirmwareType") : nullptr;
        DWORD t = 0;
        if (f && f(&t)) fw = (int)t;
    }
    if (dn >= 0 && GetDiskLayout(dn, lay)) {
        auto* L = (DRIVE_LAYOUT_INFORMATION_EX*)lay.data();
        bool gpt = L->PartitionStyle == PARTITION_STYLE_GPT;
        if (gpt) {
            static const GUID kEsp = {0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};
            bool found = false;
            for (DWORD i = 0; i < L->PartitionCount; ++i)
                if (L->PartitionEntry[i].PartitionStyle == PARTITION_STYLE_GPT && memcmp(&L->PartitionEntry[i].Gpt.PartitionType, &kEsp, sizeof(GUID)) == 0) found = true;
            if (!found) AddF(c, Sev::Bad, "GPT-диск без раздела ESP", "Загрузчику UEFI негде находиться.", AWin("bcd", "Создать ESP и выполнить bcdboot"));
            else AddF(c, Sev::Info, "GPT + ESP присутствует", "");
            if (fw == 1) AddF(c, Sev::Warn, "GPT-диск, но компьютер загружен в Legacy BIOS", "Для GPT нужен режим UEFI.", AMod("boot", "Проверка загрузки"));
        } else {
            bool active = false;
            for (DWORD i = 0; i < L->PartitionCount; ++i) if (L->PartitionEntry[i].PartitionStyle == PARTITION_STYLE_MBR && L->PartitionEntry[i].Mbr.BootIndicator) active = true;
            if (fw == 1 && !active) AddF(c, Sev::Bad, "MBR + BIOS: нет активного раздела", "Нужен активный системный раздел.", AWin("bcd", "Пересоздать загрузчик (bcdboot, тип BIOS)"));
            if (fw == 2) AddF(c, Sev::Warn, "MBR-диск при загрузке в UEFI", "Загрузка UEFI с MBR возможна только в режиме CSM.", AMod("boot", "Проверка загрузки"));
        }
    }
    std::string store, raw;
    bool have = c.target.online;
    if (!c.target.online) {
        int tdisk = dn;
        std::string any;
        for (auto& s : FindBcdStores()) {
            if (s.path.empty()) continue;
            if (any.empty()) any = s.path;
            if (VolumeDiskNumber(s.path[0]) == tdisk) { store = s.path; break; }
        }
        if (store.empty()) store = any;
        have = !store.empty();
    }
    if (!have) { AddF(c, Sev::Bad, "Хранилище BCD не найдено (ни на ESP, ни на системном разделе)", "Без BCD Windows не загрузится.", AWin("bcd", "Пересоздать загрузчик (bcdboot)")); return; }
    auto entries = BcdEnum(store, raw);
    int loaders = 0;
    for (auto& e : entries) {
        if (Lower(e.get("path")).find("winload") != std::string::npos) ++loaders;
        if (!e.get("safeboot").empty()) AddF(c, Sev::Warn, "В BCD включён безопасный режим (safeboot " + e.get("safeboot") + ")", "Windows будет загружаться в безопасном режиме, пока параметр не сброшен.", AWin("bcd", "Сбросить режимы загрузки"));
    }
    if (loaders == 0) AddF(c, Sev::Bad, "В BCD нет записи загрузчика Windows (winload)", "Хранилище: " + (store.empty() ? std::string("текущей системы") : store), AWin("bcd", "Пересоздать загрузчик (bcdboot)"));
    else AddF(c, Sev::Info, fmt("BCD: найдено записей Windows — %d", loaders), store);
}

void PHives(Ctx& c) {
    if (!c.target.valid) return;
    RegCtx rc;
    std::string err;
    if (!rc.open(c.target, err)) {
        std::string cfg = c.target.winDir + "\\System32\\config\\RegBack\\SYSTEM";
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        bool rb = GetFileAttributesExW(W(cfg).c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow);
        AddF(c, Sev::Bad, "Не загружаются кусты реестра SYSTEM/SOFTWARE", err + (rb ? "  RegBack содержит копии — можно восстановить." : "  RegBack пуст."), AFix({17}, "Восстановить кусты из RegBack"));
    } else AddF(c, Sev::Info, "Кусты реестра SYSTEM/SOFTWARE загружаются", "");
}

void PPending(Ctx& c) {
    if (!c.target.valid) return;
    bool px = PathExists(c.target.winDir + "\\WinSxS\\pending.xml");
    RegCtx rc;
    std::string err, flags;
    if (rc.open(c.target, err)) {
        const std::string b = "Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\";
        for (const char* k : {"RebootPending", "PackagesPending", "SessionsPending"}) {
            HKEY h = nullptr;
            if (RegOpenKeyExW(rc.software, W(b + k).c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) { RegCloseKey(h); flags += std::string(k) + " "; }
        }
    }
    if (px || !flags.empty()) AddF(c, Sev::Warn, "Есть отложенные операции обновления", std::string(px ? "pending.xml; " : "") + flags + "— частая причина цикла «Отмена изменений» и зависаний при загрузке.", AFix({16}, "Отменить отложенные операции (DISM /RevertPendingActions)"));
    else AddF(c, Sev::Info, "Отложенных операций обновления нет", "");
}

void PCbs(Ctx& c) {
    if (!c.target.valid) return;
    std::string p = c.target.winDir + "\\Logs\\CBS\\CBS.log";
    if (!PathExists(p)) return;
    HANDLE h = CreateFileW(W(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    LONGLONG start = sz.QuadPart > (2 << 20) ? sz.QuadPart - (2 << 20) : 0;
    LARGE_INTEGER pos;
    pos.QuadPart = start;
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    std::string data((size_t)(sz.QuadPart - start), '\0');
    DWORD got = 0;
    ReadFile(h, &data[0], (DWORD)data.size(), &got, nullptr);
    CloseHandle(h);
    data.resize(got);
    std::string l = Lower(data);
    int n = 0;
    for (size_t q = 0; (q = l.find("cannot repair member file", q)) != std::string::npos; ++q) ++n;
    int cor = 0;
    for (size_t q = 0; (q = l.find("corrupt", q)) != std::string::npos; ++q) ++cor;
    if (n > 0) AddF(c, Sev::Bad, fmt("CBS.log: SFC не смог восстановить файлов — %d", n), "Системные файлы повреждены; нужен источник восстановления.", ACat(14, "DISM /RestoreHealth с источником (install.wim) + sfc"));
    else if (cor > 20) AddF(c, Sev::Warn, fmt("CBS.log: упоминаний повреждений — %d", cor), "", AMod("integrity", "Проверка целостности (DISM/SFC без изменений)"));
    else AddF(c, Sev::Info, "CBS.log: явных повреждений не видно", "");
}

void PSpace(Ctx& c) {
    if (!c.target.valid) return;
    ULARGE_INTEGER fb{}, tb{}, tf{};
    if (!GetDiskFreeSpaceExW(W(c.target.root).c_str(), &fb, &tb, &tf)) return;
    double gb = (double)fb.QuadPart / (1024.0 * 1024 * 1024);
    if (gb < 2) AddF(c, Sev::Bad, fmt("На %s почти нет места: %.1f ГБ", c.target.root.c_str(), gb), "Обновления и загрузка могут падать.", AFix({11, 10}, "Очистить временные файлы и кэш обновлений"));
    else if (gb < 8) AddF(c, Sev::Warn, fmt("Мало места на %s: %.1f ГБ", c.target.root.c_str(), gb), "", AFix({11}, "Очистить временные файлы"));
    else AddF(c, Sev::Info, fmt("Свободного места на %s достаточно: %.1f ГБ", c.target.root.c_str(), gb), "");
}

void PWinRE(Ctx& c) {
    if (!c.target.valid) return;
    WinReInfo i = ReadWinReInfo(c.target);
    if (i.wimPath.empty()) AddF(c, Sev::Warn, "Образ WinRE (Winre.wim) не найден", "Штатное восстановление и автовосстановление работать не будут.", AWin("winre", "Создать/восстановить запись WinRE"));
    else if (i.installState == "0") AddF(c, Sev::Warn, "WinRE выключен в ОС", i.wimPath, AWin("winre", "Создать/восстановить запись WinRE"));
    else AddF(c, Sev::Info, "WinRE настроен", i.wimPath);
}

void PDumps(Ctx& c) {
    if (!c.target.valid) return;
    struct D { std::string path; unsigned long long ft; };
    std::vector<D> ds;
    auto add = [&](const std::string& p) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(W(p).c_str(), GetFileExInfoStandard, &fa)) ds.push_back({p, ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime});
    };
    for (const auto& f : ListFiles(c.target.winDir + "\\Minidump\\", "*.dmp")) add(c.target.winDir + "\\Minidump\\" + f);
    if (PathExists(c.target.winDir + "\\MEMORY.DMP")) add(c.target.winDir + "\\MEMORY.DMP");
    if (ds.empty()) { AddF(c, Sev::Info, "Дампов падений нет", "Если синие экраны были, а дампов нет — запись дампа выключена или не хватало места/файла подкачки."); return; }
    std::sort(ds.begin(), ds.end(), [](const D& a, const D& b) { return a.ft > b.ft; });
    std::vector<uint8_t> head;
    ReadBytes(ds[0].path, head, 0x1000);
    DumpInfo di = parseDumpHeader(head.data(), head.size());
    if (!di.ok) { AddF(c, Sev::Warn, fmt("Дампов: %zu, заголовок последнего не распознан", ds.size()), di.error, AMod("dump", "Разобрать дампы")); return; }
    const BsodEntry* ce = bsodByCode(di.bugcheck);
    std::string d = fmt("Дампов: %zu; последний от %s. P1=0x%llX P2=0x%llX.", ds.size(), FileTimeStr(ds[0].ft).c_str(), (unsigned long long)di.param[0], (unsigned long long)di.param[1]);
    if (ce) d += std::string(" Категория: ") + ce->category + "; с чего начать: " + ce->firstStep + "; " + ce->note;
    AddF(c, Sev::Bad, fmt("Последний BugCheck: 0x%X %s", di.bugcheck, ce ? ce->name : "(нет в каталоге)"), d, ce ? ActionForCategory(ce->category) : AMod("dump", "Разобрать дамп (cdb)"));
}

void PEvents(Ctx& c) {
    if (!c.target.valid) return;
    int days = std::max(3, c.opt.days);
    std::vector<EvtRow> rows;
    std::string err;
    if (!QueryEvents(c, "System", XpathRecent(days, "(Level=1 or Level=2)"), 1500, false, false, rows, err)) { AddF(c, Sev::Info, "Журнал System недоступен", err); return; }
    std::map<std::string, int> prov;
    int p41 = 0, scm = 0;
    for (auto& r : rows) {
        prov[r.provider]++;
        if (r.provider == "Microsoft-Windows-Kernel-Power" && r.id == 41) ++p41;
        if (r.provider == "Service Control Manager" && (r.id == 7000 || r.id == 7001 || r.id == 7023 || r.id == 7026 || r.id == 7034)) ++scm;
    }
    auto cnt = [&](std::initializer_list<const char*> names) { int n = 0; for (auto x : names) if (prov.count(x)) n += prov[x]; return n; };
    int disk = cnt({"disk", "Ntfs", "Microsoft-Windows-Ntfs", "stornvme", "storahci", "volmgr", "iaStorAVC", "iaStorV"});
    int whea = cnt({"Microsoft-Windows-WHEA-Logger"});
    int bug = cnt({"Microsoft-Windows-WER-SystemErrorReporting", "BugCheck"});
    if (disk) AddF(c, Sev::Bad, fmt("Ошибки диска/файловой системы за %d дн.: %d", days, disk), "Источники: disk/Ntfs/stornvme/storahci. Возможны проблемы накопителя, кабеля или питания.", AMod("disk", "Диски: SMART, тома, события"));
    if (whea) AddF(c, Sev::Bad, fmt("Аппаратные ошибки WHEA: %d", whea), "Память, процессор, шина PCIe или перегрев.", AMod("memory", "Память и аппаратные проверки"));
    if (p41) AddF(c, Sev::Warn, fmt("Неожиданные выключения (Kernel-Power 41): %d", p41), "Питание/БП, перегрев, аварийные BSOD или кнопка питания.", AMod("events", "События подробно"));
    if (bug) AddF(c, Sev::Warn, fmt("Событий BugCheck за %d дн.: %d", days, bug), "", AMod("bsod", "BSOD: события и дампы"));
    if (scm) AddF(c, Sev::Warn, fmt("Сбои запуска служб/драйверов (SCM): %d", scm), "", AMod("drivers", "Драйверы и службы"));
    if (!disk && !whea && !p41 && !bug && !scm) {
        std::vector<std::pair<int, std::string>> top;
        for (auto& p : prov) top.push_back({p.second, p.first});
        std::sort(top.begin(), top.end(), [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) { return a.first > b.first; });
        std::string d;
        for (size_t i = 0; i < top.size() && i < 4; ++i) d += fmt("%s: %d; ", top[i].second.c_str(), top[i].first);
        AddF(c, Sev::Info, fmt("Критичных системных событий (%d дн.): %zu, характерных признаков нет", days, rows.size()), d, AMod("events", "События подробно"));
    }
}

void PPersistence(Ctx& c) {
    if (!c.target.valid) return;
    if (c.target.online) { AddF(c, Sev::Info, "Автозагрузка работающей ОС", "Текущую Windows можно просмотреть в окне «Автозагрузка и службы» (выберите её в списке «Целевая ОС»).", AWin("autoruns", "Открыть автозагрузку")); return; }
    std::string err;
    auto items = ScanAutoruns(c.target, err);
    if (!err.empty()) { AddF(c, Sev::Info, "Автозагрузка не прочитана", err); return; }
    int susp = 0, miss = 0, tp = 0;
    std::string sd, md;
    for (auto& i : items) {
        bool bad = i.flag.rfind("ПОДОЗРИТЕЛЬНО", 0) == 0 || i.kind == "Image File Execution Options" || i.kind == "AppInit_DLLs";
        if (bad) { ++susp; if (susp <= 6) sd += i.kind + ": " + i.name + " -> " + truncUtf8(i.command, 90) + "; "; }
        else if (i.flag == "ФАЙЛ НЕ НАЙДЕН" && i.state != "Отключено") { ++miss; if (miss <= 4) md += i.name + "; "; }
        if (i.op == AOp::Service && i.thirdParty && i.state != "Отключено") ++tp;
    }
    if (susp) AddF(c, Sev::Bad, fmt("Подозрительных записей автозапуска: %d", susp), sd, AFix({1, 2, 3}, "Сбросить Winlogon, IFEO, AppInit (и просмотреть автозагрузку)"));
    if (miss) AddF(c, Sev::Warn, fmt("Записей автозапуска со ссылкой на отсутствующий файл: %d", miss), md + "(остатки удалённых программ/вредоносов)", AWin("autoruns", "Просмотреть и отключить записи"));
    if (!susp && !miss) AddF(c, Sev::Info, fmt("Автозагрузка: записей %zu, подозрительных нет", items.size()), "");
    if (tp > 25) AddF(c, Sev::Warn, fmt("Много сторонних служб/драйверов с автозапуском: %d", tp), "Может замедлять систему.", AWin("autoruns", "«Чистая загрузка» для проверки"));
}

void PPolicies(Ctx& c) {
    if (!c.target.valid) return;
    RegCtx rc;
    std::string err;
    if (!rc.open(c.target, err)) { AddF(c, Sev::Info, "Реестр недоступен для проверки политик", err); return; }
    int found = 0;
    std::string d;
    auto chk = [&](HKEY root, const std::string& sub, std::initializer_list<const char*> names) {
        for (auto n : names) {
            RegVal v;
            if (QueryVal(root, sub, n, v) && v.data.size() >= 4 && *(DWORD*)v.data.data() != 0) { ++found; if (found <= 6) d += std::string(n) + "; "; }
        }
    };
    chk(rc.software, "Microsoft\\Windows\\CurrentVersion\\Policies\\System", {"DisableTaskMgr", "DisableRegistryTools", "DisableCMD", "DisableLockWorkstation"});
    chk(rc.software, "Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", {"NoRun", "NoControlPanel", "NoFolderOptions", "NoDesktop", "NoViewContextMenu"});
    chk(rc.software, "Policies\\Microsoft\\Windows Defender", {"DisableAntiSpyware", "DisableRealtimeMonitoring"});
    for (auto& u : rc.users) {
        chk(u.root, "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", {"DisableTaskMgr", "DisableRegistryTools", "DisableCMD"});
        chk(u.root, "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", {"NoRun", "NoControlPanel", "NoFolderOptions", "NoDesktop"});
    }
    if (found) AddF(c, Sev::Bad, fmt("Включены блокирующие политики: %d", found), d + "(часто оставляет вредоносное ПО: нет диспетчера задач/regedit/cmd)", AFix({4}, "Удалить политики, оставленные заражением"));
    else AddF(c, Sev::Info, "Блокирующих политик нет", "");
    // associations
    RegVal a;
    if (QueryVal(rc.software, "Classes\\exefile\\shell\\open\\command", "", a) && RegText(a) != "\"%1\" %*") AddF(c, Sev::Bad, "Подменена ассоциация запуска .exe", RegText(a), AFix({8}, "Исправить ассоциации .exe/.bat/.cmd/.com/.reg/.lnk"));
    // proxy
    int px = 0;
    auto proxy = [&](HKEY root, const std::string& sub) {
        RegVal v;
        if (QueryVal(root, sub, "ProxyEnable", v) && v.data.size() >= 4 && *(DWORD*)v.data.data() != 0) ++px;
        if (QueryVal(root, sub, "AutoConfigURL", v)) ++px;
    };
    proxy(rc.software, "Microsoft\\Windows\\CurrentVersion\\Internet Settings");
    for (auto& u : rc.users) proxy(u.root, "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings");
    if (px) AddF(c, Sev::Warn, "Включён прокси/скрипт автонастройки", "Если интернет «не работает» — проверьте, не подменён ли прокси.", AFix({5}, "Сбросить прокси"));
    // hosts
    std::string hosts = c.target.winDir + "\\System32\\drivers\\etc\\hosts";
    std::vector<uint8_t> raw;
    if (ReadBytes(hosts, raw, 1 << 20)) {
        std::string t((const char*)raw.data(), raw.size());
        int lines = 0;
        std::string sample;
        size_t p = 0;
        while (p < t.size()) {
            size_t e = t.find('\n', p);
            if (e == std::string::npos) e = t.size();
            std::string l = trim(t.substr(p, e - p));
            p = e + 1;
            if (!l.empty() && l[0] != '#') { ++lines; if (lines <= 3) sample += l + "; "; }
        }
        if (lines) AddF(c, Sev::Warn, fmt("В hosts есть записи: %d", lines), sample, AFix({6}, "Восстановить hosts по умолчанию"));
    }
    // SafeBoot keys
    int miss = 0;
    for (const char* k : {"Minimal", "Network"}) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(rc.system, W(rc.cs + "\\Control\\SafeBoot\\" + k).c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) RegCloseKey(h); else ++miss;
    }
    if (miss) AddF(c, Sev::Warn, "Отсутствуют ключи SafeBoot — безопасный режим не заработает", "", AFix({9}, "Создать ключи SafeBoot"));
}

void CheckSvc(Ctx& c, std::initializer_list<const char*> names, const std::string& what) {
    if (!c.target.valid) return;
    RegCtx rc;
    std::string err;
    if (!rc.open(c.target, err)) return;
    std::string off;
    int n = 0;
    for (auto s : names) {
        RegVal v;
        if (QueryVal(rc.system, rc.cs + "\\Services\\" + s, "Start", v) && v.data.size() >= 4 && *(DWORD*)v.data.data() == 4) { ++n; off += std::string(s) + " "; }
    }
    if (n) AddF(c, Sev::Bad, fmt("Отключены службы (%s): %s", what.c_str(), off.c_str()), "Часто последствие «оптимизаторов» или вредоносов.", AFix({7}, "Включить обратно ключевые службы"));
    else AddF(c, Sev::Info, "Ключевые службы (" + what + ") не отключены", "");
}
void PSvcNet(Ctx& c) { CheckSvc(c, {"Dnscache", "Dhcp", "NlaSvc", "nsi", "netprofm", "Netman", "LanmanWorkstation"}, "сеть"); }
void PSvcUpd(Ctx& c) { CheckSvc(c, {"wuauserv", "BITS", "CryptSvc", "TrustedInstaller", "msiserver", "Winmgmt", "RpcSs"}, "обновления"); }

void PNetwork(Ctx& c) {
    if (!c.target.online) AddF(c, Sev::Info, "Сетевые проверки относятся к текущей среде (WinPE/WinRE)", "Они показывают, есть ли сеть у самой среды, а не у целевой Windows.");
    Ctx sc = c;
    sc.outDir = c.outDir + "network\\";
    MakeDirs(sc.outDir);
    ModNetwork(sc);
    if (!c.res) return;
    for (auto& s : c.res->sections) {
        if (s.title != "Сеть") continue;
        int n = 0;
        for (auto& r : s.rows) if (r.first == "Замечание") { ++n; AddF(c, Sev::Warn, "Сеть: " + r.second, "", ACat(69, "Сброс сетевого стека (Winsock/TCP-IP) или проверка драйвера адаптера")); }
        if (!n) AddF(c, Sev::Info, "Сеть: замечаний нет", "");
    }
}

struct PEntry { const char* name; void (*fn)(Ctx&); };
const PEntry kProbeTable[] = {{"target", PTarget}, {"bitlocker", PBitlocker}, {"disks", PDisks}, {"nodriver", PNoDriver}, {"devices", PDevices}, {"boot", PBoot},
                              {"hives", PHives}, {"pending", PPending}, {"cbs", PCbs}, {"space", PSpace}, {"winre", PWinRE}, {"dumps", PDumps}, {"events", PEvents},
                              {"persistence", PPersistence}, {"policies", PPolicies}, {"svcnet", PSvcNet}, {"svcupd", PSvcUpd}, {"network", PNetwork}};

std::map<std::string, std::string> TriageVars(const Ctx& c) {
    std::map<std::string, std::string> v;
    v["TARGET"] = c.target.valid ? c.target.root : "";
    v["VOL"] = c.target.valid ? c.target.root.substr(0, 2) : "";
    v["WIN"] = c.target.valid ? c.target.winDir : "";
    v["SYS32"] = SysDir();
    v["DATA"] = DataDir();
    return v;
}

Sev SevOf(const std::string& s) { return s == "bad" ? Sev::Bad : s == "info" ? Sev::Info : Sev::Warn; }

bool NewestBugcheck(Ctx& c, uint32_t& code) {
    if (!c.target.valid) return false;
    std::string best;
    unsigned long long bt = 0;
    auto consider = [&](const std::string& p) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(W(p).c_str(), GetFileExInfoStandard, &fa)) return;
        unsigned long long t = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
        if (t >= bt) { bt = t; best = p; }
    };
    for (const auto& f : ListFiles(c.target.winDir + "\\Minidump\\", "*.dmp")) consider(c.target.winDir + "\\Minidump\\" + f);
    if (PathExists(c.target.winDir + "\\MEMORY.DMP")) consider(c.target.winDir + "\\MEMORY.DMP");
    if (best.empty()) return false;
    std::vector<uint8_t> head;
    ReadBytes(best, head, 0x1000);
    DumpInfo di = parseDumpHeader(head.data(), head.size());
    if (!di.ok) return false;
    code = di.bugcheck;
    return true;
}

void RunCheck(Ctx& c, const PbStep& st) {
    auto vars = TriageVars(c);
    std::string kind = Lower(st.get("check"));
    Sev sev = SevOf(Lower(st.get("severity", "warn")));
    std::string title = expandVars(st.get("title", kind), vars), detail = expandVars(st.get("detail"), vars);
    TAct act = ParseActionSpec(st.get("action"), expandVars(st.get("action_label"), vars));
    bool hit = false;
    std::string extra;
    if (kind == "file_exists") {
        std::string path = expandVars(st.get("path"), vars);
        bool ex = PathExists(path);
        hit = Lower(st.get("if", "exists")) == "missing" ? !ex : ex;
        extra = path;
    } else if (kind == "reg_value") {
        if (!c.target.valid) return;
        RegCtx rc;
        std::string err;
        if (!rc.open(c.target, err)) { AddF(c, Sev::Info, title + ": реестр недоступен", err); return; }
        std::string hive = Lower(st.get("hive", "software")), key = expandVars(st.get("key"), vars), val = st.get("value"), cond = Lower(st.get("if", "exists")), data = expandVars(st.get("data"), vars);
        if (hive == "system" && key.rfind("CurrentControlSet", 0) == 0) key = rc.cs + key.substr(17);
        std::vector<HKEY> roots;
        if (hive == "software") roots.push_back(rc.software);
        else if (hive == "system") roots.push_back(rc.system);
        else for (auto& u : rc.users) roots.push_back(u.root);
        for (HKEY r : roots) {
            RegVal v;
            bool f = QueryVal(r, key, val, v);
            bool m = false;
            if (cond == "exists") m = f;
            else if (cond == "missing") m = !f;
            else if (f && v.type == REG_DWORD && v.data.size() >= 4) {
                DWORD d = *(DWORD*)v.data.data();
                if (cond == "dword_ne0") m = d != 0;
                else if (cond == "dword_eq0") m = d == 0;
                else if (cond == "equals") m = d == (DWORD)strtoul(data.c_str(), nullptr, 0);
                else if (cond == "notequals") m = d != (DWORD)strtoul(data.c_str(), nullptr, 0);
            } else if (f) {
                std::string t = Lower(RegValueText(v, 2000)), dl = Lower(data);
                if (cond == "equals") m = t == dl;
                else if (cond == "notequals") m = t != dl;
                else if (cond == "contains") m = t.find(dl) != std::string::npos;
            } else if (cond == "notequals") m = true;
            if (m) { hit = true; extra = key + "\\" + val + (f ? " = " + RegValueText(v, 120) : std::string(" (нет)")); break; }
        }
    } else if (kind == "bugcheck") {
        uint32_t code = 0;
        hit = NewestBugcheck(c, code) && code == (uint32_t)strtoul(st.get("code").c_str(), nullptr, 0);
        extra = fmt("последний BugCheck 0x%X", code);
    } else if (kind == "event") {
        int days = std::max(1, atoi(st.get("days", "7").c_str())), min = std::max(1, atoi(st.get("min", "1").c_str()));
        std::vector<EvtRow> rows;
        std::string err;
        std::string log = st.get("log", "System");
        if (!c.target.valid || !QueryEvents(c, log, XpathRecent(days, st.get("levels", "") .empty() ? "" : "(Level=" + st.get("levels") + ")"), 3000, false, false, rows, err)) return;
        int n = 0;
        std::string pv = Lower(st.get("provider")), id = st.get("id");
        for (auto& r : rows) if ((pv.empty() || Lower(r.provider) == pv) && (id.empty() || r.id == (unsigned)atoi(id.c_str()))) ++n;
        hit = n >= min;
        extra = fmt("событий: %d за %d дн.", n, days);
    } else if (kind == "service_disabled") {
        if (!c.target.valid) return;
        RegCtx rc;
        std::string err;
        if (!rc.open(c.target, err)) return;
        std::string names = st.get("names"), found;
        size_t p = 0;
        while (p < names.size()) {
            size_t e = names.find_first_of(", ;", p);
            if (e == std::string::npos) e = names.size();
            std::string n = trim(names.substr(p, e - p));
            p = e + 1;
            RegVal v;
            if (!n.empty() && QueryVal(rc.system, rc.cs + "\\Services\\" + n, "Start", v) && v.data.size() >= 4 && *(DWORD*)v.data.data() == 4) found += n + " ";
        }
        hit = !found.empty();
        extra = found;
    } else if (kind == "space_below") {
        ULARGE_INTEGER fb{}, tb{}, tf{};
        if (!c.target.valid || !GetDiskFreeSpaceExW(W(c.target.root).c_str(), &fb, &tb, &tf)) return;
        double gb = (double)fb.QuadPart / (1024.0 * 1024 * 1024);
        hit = gb < atof(st.get("gb", "10").c_str());
        extra = fmt("свободно %.1f ГБ", gb);
    } else {
        AddF(c, Sev::Info, "Сценарий: неизвестный тип check «" + kind + "»", "");
        return;
    }
    if (hit) AddF(c, sev, title, detail.empty() ? extra : detail + (extra.empty() ? "" : " [" + extra + "]"), act);
    else if (!st.get("okmsg").empty()) AddF(c, Sev::Info, expandVars(st.get("okmsg"), vars), "");
}

typedef void (*Probe)(Ctx&);

std::vector<Probe> ProbesFor(int s) {
    switch (s) {
        case 0: return {PTarget, PBitlocker, PDisks, PNoDriver, PBoot, PHives, PPending, PDumps, PCbs, PSpace, PWinRE};
        case 1: return {PTarget, PDumps, PEvents, PDevices, PDisks, PPending, PCbs};
        case 2: return {PBitlocker, PDisks, PNoDriver, PTarget};
        case 3: return {PTarget, PPersistence, PPolicies, PEvents};
        case 4: return {PTarget, PNetwork, PPolicies, PSvcNet, PNoDriver, PDevices};
        case 5: return {PTarget, PDisks, PEvents, PPersistence, PSpace, PDevices};
        default: return {PTarget, PPending, PCbs, PSvcUpd, PSpace, PWinRE, PEvents};
    }
}

}  // namespace

static int Builtin() { return (int)(sizeof kSymptoms / sizeof kSymptoms[0]); }
int SymptomCount() { return Builtin() + (int)PlaybooksOfKind("triage").size(); }
const char* SymptomName(int i) {
    if (i >= 0 && i < Builtin()) return kSymptoms[i];
    auto v = PlaybooksOfKind("triage");
    int k = i - Builtin();
    return (k >= 0 && k < (int)v.size()) ? v[(size_t)k]->title.c_str() : "";
}

bool RunProbeByName(Ctx& c, const std::string& name) {
    for (auto& e : kProbeTable) if (name == e.name) { e.fn(c); return true; }
    return false;
}

TAct ParseActionSpec(const std::string& spec, const std::string& label) {
    TAct a;
    size_t col = spec.find(':');
    if (spec.empty() || col == std::string::npos) return a;
    std::string k = toLowerUtf8(trim(spec.substr(0, col))), v = trim(spec.substr(col + 1));
    a.label = label.empty() ? spec : label;
    if (k == "catalog") { a.k = TAct::Catalog; a.no = atoi(v.c_str()); }
    else if (k == "fix") {
        a.k = TAct::Fixes;
        size_t p = 0;
        while (p < v.size()) { size_t e = v.find_first_of(", ;", p); if (e == std::string::npos) e = v.size(); std::string t = trim(v.substr(p, e - p)); if (!t.empty()) a.ids.push_back(atoi(t.c_str())); p = e + 1; }
    } else if (k == "window") { a.k = TAct::Window; a.name = v; }
    else if (k == "module") { a.k = TAct::Module; a.name = v; }
    else if (k == "playbook") { a.k = TAct::Playbook; a.name = v; }
    else a = TAct();
    return a;
}

static void RunTriagePlaybook(Ctx& c, const Playbook& pb) {
    c.info("Сценарий-симптом: " + pb.title + " (" + pb.file + ")");
    int idx = 0;
    for (const auto& st : pb.steps) {
        ++idx;
        if (c.cancelled()) { c.warn("Прервано пользователем."); return; }
        std::string ty = st.get("type");
        try {
            if (ty == "probe") { if (!RunProbeByName(c, toLowerUtf8(st.get("name")))) c.warn("Неизвестная проверка в сценарии: " + st.get("name")); }
            else if (ty == "check") RunCheck(c, st);
            else if (ty == "message") c.info(st.get("text"));
            else if (ty == "cmdcheck") {
                auto vars = TriageVars(c);
                if (st.get("risk", "safe") != "safe" && !UiConfirm(W("Проверка сценария «" + pb.title + "» запускает:\n" + expandVars(st.get("run"), vars) + "\n\nВыполнить?"), 1)) continue;
                ProcResult r = c.run(st.get("title", "cmdcheck"), PlaybookBuildCmd(expandVars(st.get("run"), vars)), atoi(st.get("timeout", "60").c_str()));
                std::string needle = toLowerUtf8(expandVars(st.get("contains"), vars)), out = toLowerUtf8(r.output);
                bool found = !needle.empty() && out.find(needle) != std::string::npos;
                bool hit = toLowerUtf8(st.get("finding_if", "found")) == "missing" ? !found : found;
                if (hit) AddF(c, SevOf(toLowerUtf8(st.get("severity", "warn"))), expandVars(st.get("title", "Результат команды"), vars), expandVars(st.get("detail"), vars), ParseActionSpec(st.get("action"), expandVars(st.get("action_label"), vars)));
            }
        } catch (const std::exception& e) { c.warn(fmt("Шаг %d сценария прерван: %s", idx, e.what())); }
    }
}
const std::vector<Finding>& TriageFindings() { return g_find; }

void ModTriage(Ctx& c) {
    int s = c.opt.symptom < 0 || c.opt.symptom >= SymptomCount() ? 0 : c.opt.symptom;
    g_find.clear();
    c.info(std::string("=== Что случилось? — ") + SymptomName(s) + " ===");
    Hives().pin();
    struct Unpin { ~Unpin() { Hives().unpin(); } } unpin;
    if (s >= Builtin()) {
        auto pbs = PlaybooksOfKind("triage");
        size_t k = (size_t)(s - Builtin());
        if (k < pbs.size()) RunTriagePlaybook(c, *pbs[k]);
    } else {
        for (Probe p : ProbesFor(s)) {
            if (c.cancelled()) { c.warn("Прервано пользователем."); break; }
            try { p(c); }
            catch (const std::exception& e) { c.warn(std::string("Проверка прервана ошибкой: ") + e.what()); }
            catch (...) { c.warn("Проверка прервана неизвестной ошибкой."); }
        }
    }
    std::stable_sort(g_find.begin(), g_find.end(), [](const Finding& a, const Finding& b) { return (int)a.sev > (int)b.sev; });
    c.info("──────── ИТОГ ────────");
    int bad = 0, warn = 0;
    for (auto& f : g_find) { if (f.sev == Sev::Bad) ++bad; else if (f.sev == Sev::Warn) ++warn; }
    std::string txt = std::string("Симптом: ") + SymptomName(s) + "\r\n\r\n";
    Json arr = Json::array();
    int shown = 0;
    for (auto& f : g_find) {
        const char* tag = f.sev == Sev::Bad ? "[ПРОБЛЕМА]" : f.sev == Sev::Warn ? "[ВНИМАНИЕ]" : "[ок]";
        txt += std::string(tag) + " " + f.title + "\r\n";
        if (!f.detail.empty()) txt += "    " + f.detail + "\r\n";
        if (!f.act.label.empty()) txt += "    → " + f.act.label + "\r\n";
        arr.add(Json::object().set("severity", (int)f.sev).set("title", f.title).set("detail", f.detail).set("action", f.act.label));
        if (f.sev != Sev::Info && shown < 3) { ++shown; c.warn(fmt("%d) %s", shown, Tr(f.title).c_str())); if (!f.act.label.empty()) c.info("     Рекомендую: " + f.act.label); }
    }
    if (!bad && !warn) c.ok("Явных проблем для этого симптома не найдено. Дальше: «Полная диагностика», расширенный режим (DISM ScanHealth, chkdsk /scan), тест памяти, проверка кабелей/питания.");
    else c.info(fmt("Проблем: %d, замечаний: %d. Выберите находку в окне мастера и нажмите «Выполнить рекомендованное действие».", bad, warn));
    c.saveText("triage.txt", txt);
    c.saveText("triage.json", Json::object().set("symptom", SymptomName(s)).set("findings", arr).dump());
}

}  // namespace wr
