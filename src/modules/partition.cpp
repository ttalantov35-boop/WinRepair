// Менеджер разделов: листинг через Windows API, изменения — скриптами diskpart. Все операции необратимы.
#include "partition.h"
#include "../win/offreg.h"   // AuditChange

#include <winioctl.h>
#include <algorithm>
#include <cstring>

namespace wr {

namespace {

// ---- типы разделов (для показа) -------------------------------------------------------------
// Сравнение GUID типа раздела GPT с известными (memcmp, без ole32/StringFromGUID2).
std::string GptTypeName(const GUID& g) {
    static const GUID kEfi  = {0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};
    static const GUID kMsr  = {0xE3C9E316, 0x0B5C, 0x4DB8, {0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE}};
    static const GUID kData = {0xEBD0A0A2, 0xB9E5, 0x4433, {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};
    static const GUID kWinRE= {0xDE94BBA4, 0x06D1, 0x4D40, {0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC}};
    static const GUID kSpace= {0xE75CAF8F, 0xF680, 0x4CEE, {0xAF, 0xA3, 0xB0, 0x01, 0xE5, 0x6E, 0xFC, 0x2D}};
    if (!memcmp(&g, &kEfi, sizeof(GUID))) return "EFI (ESP)";
    if (!memcmp(&g, &kMsr, sizeof(GUID))) return "MSR";
    if (!memcmp(&g, &kData, sizeof(GUID))) return "Основной (данные)";
    if (!memcmp(&g, &kWinRE, sizeof(GUID))) return "Восстановление (WinRE)";
    if (!memcmp(&g, &kSpace, sizeof(GUID))) return "Storage Spaces";
    return "GPT-раздел";
}
std::string MbrTypeName(unsigned char t, bool active) {
    std::string n;
    switch (t) {
        case 0x07: n = "NTFS/exFAT"; break;
        case 0x0B: case 0x0C: n = "FAT32"; break;
        case 0x06: case 0x0E: n = "FAT16"; break;
        case 0x01: n = "FAT12"; break;
        case 0x27: n = "Восстановление"; break;
        case 0xEE: n = "GPT (защитный)"; break;
        case 0xEF: n = "EFI (ESP)"; break;
        case 0x83: n = "Linux"; break;
        case 0x82: n = "Linux swap"; break;
        case 0x17: case 0x1B: case 0x1C: n = "скрытый"; break;
        default: n = fmt("тип 0x%02X", t); break;
    }
    if (active) n += ", активный";
    return n;
}

// ---- тома (буква/ФС/метка/свободно) и их смещение на диске -----------------------------------
struct VolInfo { char letter; int disk; unsigned long long offset; std::string fs, label; unsigned long long freeBytes, total; };

bool VolExtent(char letter, int& disk, unsigned long long& offset) {
    std::wstring p = std::wstring(L"\\\\.\\") + (wchar_t)letter + L":";
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> b(1024, 0);
    DWORD ret = 0;
    bool ok = false;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, b.data(), (DWORD)b.size(), &ret, nullptr)) {
        auto* e = (VOLUME_DISK_EXTENTS*)b.data();
        if (e->NumberOfDiskExtents >= 1) { disk = (int)e->Extents[0].DiskNumber; offset = (unsigned long long)e->Extents[0].StartingOffset.QuadPart; ok = true; }
    }
    CloseHandle(h);
    return ok;
}

std::vector<VolInfo> CollectVolumes() {
    std::vector<VolInfo> v;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char L = (char)('A' + i);
        std::wstring root = std::wstring(1, (wchar_t)L) + L":\\";
        UINT dt = GetDriveTypeW(root.c_str());
        if (dt != DRIVE_FIXED && dt != DRIVE_REMOVABLE) continue;
        int disk = -1;
        unsigned long long off = 0;
        if (!VolExtent(L, disk, off)) continue;
        wchar_t label[128] = {0}, fsn[64] = {0};
        DWORD ser = 0, maxc = 0, fl = 0;
        GetVolumeInformationW(root.c_str(), label, 128, &ser, &maxc, &fl, fsn, 64);
        ULARGE_INTEGER fb{}, tb{}, tf{};
        GetDiskFreeSpaceExW(root.c_str(), &fb, &tb, &tf);
        v.push_back({L, disk, off, U(fsn), U(label), (unsigned long long)fb.QuadPart, (unsigned long long)tb.QuadPart});
    }
    return v;
}

// ---- запуск скрипта diskpart ----------------------------------------------------------------
void RunDiskpart(Ctx& c, const std::string& title, const std::vector<std::string>& lines) {
    std::string dir = c.sub("diskpart");  // с завершающим "\"
    std::string path = dir + "dp_script.txt";
    std::string script;
    for (const auto& l : lines) script += l + "\r\n";
    if (!WriteText(path, script, false)) { c.error("Не удалось записать скрипт diskpart."); return; }
    std::string cmd = "\"" + SysTool("diskpart.exe") + "\" /s \"" + path + "\"";
    ProcResult r = c.run(title, cmd, 600);
    DeleteFileW(W(path).c_str());
    std::string low = toLowerUtf8(r.output);
    bool bad = !r.started || r.exitCode != 0 || low.find("error") != std::string::npos || low.find("ошибк") != std::string::npos ||
               low.find("fail") != std::string::npos || low.find("не удал") != std::string::npos || low.find("отказ") != std::string::npos;
    if (bad) c.error(title + ": diskpart сообщил об ошибке (подробности в лог-файле, код " + std::to_string(r.exitCode) + ").");
    else c.ok(title + ": готово.");
}

std::string QuoteLabel(const std::string& s) {
    std::string t;
    for (char ch : s) if (ch != '"') t += ch;  // кавычки убираем
    return t;
}

}  // namespace

bool PartScan(std::vector<PartDisk>& out, std::string& err) {
    out.clear();
    std::vector<DiskInfo> disks = CollectDisks();
    if (disks.empty()) { err = "Физические диски не обнаружены (нет драйвера хранилища?)."; return false; }
    std::vector<VolInfo> vols = CollectVolumes();

    for (const auto& d : disks) {
        PartDisk pd;
        pd.number = d.number;
        pd.model = d.model;
        pd.serial = d.serial;
        pd.bus = d.bus;
        pd.media = d.media;
        pd.style = d.partStyle;
        pd.size = d.size;

        std::vector<uint8_t> lay;
        std::vector<PartItem> parts;
        if (GetDiskLayout(d.number, lay) && lay.size() >= sizeof(DRIVE_LAYOUT_INFORMATION_EX)) {
            auto* L = (DRIVE_LAYOUT_INFORMATION_EX*)lay.data();
            DWORD cnt = L->PartitionCount;
            size_t maxCnt = (lay.size() - offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) / sizeof(PARTITION_INFORMATION_EX);
            if (cnt > maxCnt) cnt = (DWORD)maxCnt;
            for (DWORD i = 0; i < cnt; ++i) {
                const PARTITION_INFORMATION_EX& p = L->PartitionEntry[i];
                if (p.PartitionLength.QuadPart <= 0) continue;
                if (p.PartitionStyle == PARTITION_STYLE_MBR && p.PartitionNumber == 0) continue;
                PartItem it;
                it.number = p.PartitionNumber;
                it.offset = (unsigned long long)p.StartingOffset.QuadPart;
                it.size = (unsigned long long)p.PartitionLength.QuadPart;
                if (p.PartitionStyle == PARTITION_STYLE_GPT) it.type = GptTypeName(p.Gpt.PartitionType);
                else if (p.PartitionStyle == PARTITION_STYLE_MBR) { it.active = p.Mbr.BootIndicator != 0; it.type = MbrTypeName(p.Mbr.PartitionType, it.active); }
                else it.type = "RAW";
                parts.push_back(it);
            }
        }

        // привязать буквы/ФС/метки/свободно по совпадению смещения
        for (auto& it : parts)
            for (const auto& v : vols)
                if (v.disk == d.number && v.offset == it.offset) { it.letter = v.letter; it.fs = v.fs; it.label = v.label; it.freeBytes = v.freeBytes; it.hasVol = true; break; }

        // вычислить свободные области (пробелы между разделами и в конце), порог 1 МиБ
        std::sort(parts.begin(), parts.end(), [](const PartItem& a, const PartItem& b) { return a.offset < b.offset; });
        const unsigned long long MiB = 1024ull * 1024;
        unsigned long long cursor = MiB;  // первый мегабайт — служебный
        std::vector<PartItem> items;
        for (const auto& it : parts) {
            if (it.offset > cursor && it.offset - cursor >= MiB) {
                PartItem f;
                f.free = true;
                f.offset = cursor;
                f.size = it.offset - cursor;
                f.type = "— свободно —";
                items.push_back(f);
                pd.freeMax = std::max(pd.freeMax, f.size);
            }
            items.push_back(it);
            cursor = std::max(cursor, it.offset + it.size);
        }
        if (pd.size > cursor && pd.size - cursor >= MiB) {
            PartItem f;
            f.free = true;
            f.offset = cursor;
            f.size = pd.size - cursor;
            f.type = "— свободно —";
            items.push_back(f);
            pd.freeMax = std::max(pd.freeMax, f.size);
        }
        pd.items = items;
        out.push_back(pd);
    }
    return true;
}

// ---- операции -------------------------------------------------------------------------------
void PartCreate(Ctx& c, int disk, unsigned long long sizeMB, const std::string& fs, const std::string& label, char letter) {
    std::vector<std::string> s = {fmt("select disk %d", disk)};
    s.push_back(sizeMB > 0 ? fmt("create partition primary size=%llu", sizeMB) : "create partition primary");
    if (!fs.empty()) s.push_back(fmt("format fs=%s label=\"%s\" quick", fs.c_str(), QuoteLabel(label).c_str()));
    if (letter) s.push_back(fmt("assign letter=%c", letter));
    AuditChange(fmt("Диск %d: создание раздела (size=%llu МБ, fs=%s, letter=%s)", disk, sizeMB, fs.empty() ? "нет" : fs.c_str(), letter ? std::string(1, letter).c_str() : "нет"));
    RunDiskpart(c, fmt("Создание раздела на диске %d", disk), s);
}

void PartDelete(Ctx& c, int disk, unsigned part) {
    AuditChange(fmt("Диск %d: удаление раздела %u", disk, part));
    RunDiskpart(c, fmt("Удаление раздела %u (диск %d)", part, disk), {fmt("select disk %d", disk), fmt("select partition %u", part), "delete partition override"});
}

void PartFormat(Ctx& c, int disk, unsigned part, const std::string& fs, const std::string& label, bool quick) {
    AuditChange(fmt("Диск %d: форматирование раздела %u (fs=%s)", disk, part, fs.c_str()));
    RunDiskpart(c, fmt("Форматирование раздела %u (диск %d)", part, disk),
                {fmt("select disk %d", disk), fmt("select partition %u", part), fmt("format fs=%s label=\"%s\"%s", fs.c_str(), QuoteLabel(label).c_str(), quick ? " quick" : "")});
}

void PartSetLetter(Ctx& c, int disk, unsigned part, char letter) {
    std::vector<std::string> s = {fmt("select disk %d", disk), fmt("select partition %u", part)};
    if (letter) { s.push_back(fmt("assign letter=%c", letter)); AuditChange(fmt("Диск %d, раздел %u: назначить букву %c", disk, part, letter)); }
    else { s.push_back("remove"); AuditChange(fmt("Диск %d, раздел %u: убрать букву", disk, part)); }
    RunDiskpart(c, fmt("Буква раздела %u (диск %d)", part, disk), s);
}

void PartActive(Ctx& c, int disk, unsigned part, bool active) {
    AuditChange(fmt("Диск %d, раздел %u: %s", disk, part, active ? "сделать активным" : "снять активность"));
    RunDiskpart(c, fmt("%s раздела %u (диск %d)", active ? "Активный" : "Неактивный", part, disk),
                {fmt("select disk %d", disk), fmt("select partition %u", part), active ? "active" : "inactive"});
}

void PartShrink(Ctx& c, int disk, unsigned part, unsigned long long mb) {
    AuditChange(fmt("Диск %d, раздел %u: сжать на %llu МБ", disk, part, mb));
    RunDiskpart(c, fmt("Сжатие раздела %u (диск %d)", part, disk),
                {fmt("select disk %d", disk), fmt("select partition %u", part), fmt("shrink desired=%llu", mb)});
}

void PartExtend(Ctx& c, int disk, unsigned part, unsigned long long mb) {
    AuditChange(fmt("Диск %d, раздел %u: расширить на %s", disk, part, mb ? fmt("%llu МБ", mb).c_str() : "максимум"));
    RunDiskpart(c, fmt("Расширение раздела %u (диск %d)", part, disk),
                {fmt("select disk %d", disk), fmt("select partition %u", part), mb ? fmt("extend size=%llu", mb) : std::string("extend")});
}

void PartClean(Ctx& c, int disk) {
    AuditChange(fmt("Диск %d: ОЧИСТКА (clean) — удалена таблица разделов", disk));
    RunDiskpart(c, fmt("Очистка диска %d", disk), {fmt("select disk %d", disk), "clean"});
}

void PartConvert(Ctx& c, int disk, bool toGpt) {
    AuditChange(fmt("Диск %d: конвертация в %s", disk, toGpt ? "GPT" : "MBR"));
    RunDiskpart(c, fmt("Конвертация диска %d в %s", disk, toGpt ? "GPT" : "MBR"),
                {fmt("select disk %d", disk), toGpt ? "convert gpt" : "convert mbr"});
}

}  // namespace wr
