// «Разделы диска»: обёртка над diskpart для работы с разделами из WinPE/WinRE. ВСЕ операции необратимы.
#include "ui_tools_common.h"
#include "modules/partition.h"

namespace wr {
namespace ui {

namespace {

enum { ID_DISK = 7501, ID_LIST, ID_WARN, ID_HEAD, ID_MAP, ID_STATUS, ID_B0 = 7520, ID_LEGEND = 7560 };
enum { B_CREATE, B_DELETE, B_FORMAT, B_LETTER, B_ACTIVE, B_SHRINK, B_EXTEND, B_CONVERT, B_REFRESH, B_CLOSE, B_COUNT };

struct St {
    HWND wnd = nullptr, combo, list, warn, head, map, status, legend, btn[B_COUNT];
    std::vector<PartDisk> disks;
};
St Q;
std::wstring g_pending;
inline bool EN() { return CurrentLang() == Lang::EN; }
const unsigned long long MiB = 1024ull * 1024;

// размер с единицами на языке интерфейса
std::string fb(unsigned long long x) { return fmtBytes(x, EN()); }
// двуязычная строка
std::wstring L(const char* ru, const char* en) { return WRaw(EN() ? en : ru); }

HFONT MkFont(double scale, int weight) {
    LOGFONTW lf{};
    GetObjectW(g_ui.font, sizeof lf, &lf);
    lf.lfHeight = (LONG)(lf.lfHeight * scale);
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}
HFONT MedFont() { static HFONT f = nullptr; if (!f) f = MkFont(1.20, FW_NORMAL); return f; }
HFONT MedBold() { static HFONT f = nullptr; if (!f) f = MkFont(1.20, FW_BOLD); return f; }
HFONT HeadFont() { static HFONT f = nullptr; if (!f) f = MkFont(1.40, FW_BOLD); return f; }

int CurDiskIdx() {
    int t = (int)SendMessageW(Q.combo, CB_GETCURSEL, 0, 0);
    return (t >= 0 && t < (int)Q.disks.size()) ? t : -1;
}
PartDisk* CurDisk() { int i = CurDiskIdx(); return i < 0 ? nullptr : &Q.disks[(size_t)i]; }
int CurIdx() { return ListView_GetNextItem(Q.list, -1, LVNI_SELECTED); }
PartItem* CurItem() {
    PartDisk* d = CurDisk();
    if (!d) return nullptr;
    int s = CurIdx();
    return (s >= 0 && s < (int)d->items.size()) ? &d->items[(size_t)s] : nullptr;
}

Tone ToneOf(int i) {
    PartDisk* d = CurDisk();
    if (!d || i < 0 || i >= (int)d->items.size()) return Tone::None;
    const PartItem& it = d->items[(size_t)i];
    if (it.free) return Tone::Gray;
    if (it.type.find("EFI") != std::string::npos || it.type.find("MSR") != std::string::npos || it.type.find("осстанов") != std::string::npos) return Tone::Blue;
    if (it.active) return Tone::Orange;
    return Tone::None;
}

bool Busy() {
    if (!UiRunning()) return false;
    MessageBoxW(Q.wnd, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION);
    return true;
}
bool Confirm(const std::wstring& body, bool danger) {
    return MessageBoxW(Q.wnd, body.c_str(), L("Подтверждение — операция необратима", "Confirm — irreversible").c_str(),
                       MB_YESNO | (danger ? MB_ICONSTOP : MB_ICONWARNING) | MB_DEFBUTTON2) == IDYES;
}
void MsgSel(const char* ru, const char* en, const char* capRu, const char* capEn) {
    MessageBoxW(Q.wnd, L(ru, en).c_str(), L(capRu, capEn).c_str(), MB_OK | MB_ICONINFORMATION);
}

std::string DiskStr(const PartDisk& d) {
    return fmt("%s %d: %s — %s, %s%s%s", EN() ? "Disk" : "Диск", d.number, d.model.c_str(), fb(d.size).c_str(), d.style.c_str(),
               d.serial.empty() ? "" : (", SN " + d.serial).c_str(), d.bus.empty() ? "" : (", " + d.bus).c_str());
}

std::vector<char> FreeLetters() {
    std::vector<char> v;
    DWORD mask = GetLogicalDrives();
    for (int i = 2; i < 26; ++i) if (!(mask & (1u << i))) v.push_back((char)('A' + i));
    return v;
}

void SetStatus(const std::wstring& s) { SetWindowTextW(Q.status, s.c_str()); }

// ---- локальная отрисовка списка (крупный шрифт + цвета) ----
LRESULT PartListDraw(LPARAM lp) {
    auto* cd = (NMLVCUSTOMDRAW*)lp;
    HWND lv = cd->nmcd.hdr.hwndFrom;
    int item = (int)cd->nmcd.dwItemSpec;
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            Tone t = ToneOf(item);
            bool sel = (ListView_GetItemState(lv, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            if (t != Tone::None && !sel) cd->clrTextBk = ToneBk(t);
            return CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT;
        }
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            Tone t = ToneOf(item);
            bool sel = (ListView_GetItemState(lv, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            if (t != Tone::None && !sel) cd->clrTextBk = ToneBk(t);
            bool accent = (cd->iSubItem == 3 && t != Tone::None);
            if (accent) { if (!sel) cd->clrText = ToneColor(t); SelectObject(cd->nmcd.hdc, MedBold()); }
            else { cd->clrText = sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT); SelectObject(cd->nmcd.hdc, MedFont()); }
            return CDRF_NEWFONT;
        }
        case CDDS_ITEMPOSTPAINT: {
            Tone t = ToneOf(item);
            if (t != Tone::None) {
                RECT r{};
                ListView_GetItemRect(lv, item, &r, LVIR_BOUNDS);
                r.right = r.left + S(5);
                HBRUSH b = CreateSolidBrush(ToneColor(t));
                FillRect(cd->nmcd.hdc, &r, b);
                DeleteObject(b);
            }
            return CDRF_DODEFAULT;
        }
    }
    return CDRF_DODEFAULT;
}

// ---- визуальная полоса-шкала разделов диска ----
int MapHitIndex(int x) {
    PartDisk* d = CurDisk();
    if (!d || d->items.empty() || d->size == 0) return -1;
    RECT rc;
    GetClientRect(Q.map, &rc);
    int W_ = rc.right - rc.left;
    double cx = 0;
    for (size_t i = 0; i < d->items.size(); ++i) {
        double w = (double)W_ * (double)d->items[i].size / (double)d->size;
        if (x >= (int)cx && x < (int)(cx + w)) return (int)i;
        cx += w;
    }
    return (int)d->items.size() - 1;
}

LRESULT CALLBACK MapProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
        PartDisk* d = CurDisk();
        if (d && d->size && !d->items.empty()) {
            int W_ = rc.right - rc.left, H = rc.bottom - rc.top;
            int sel = CurIdx();
            SelectObject(dc, g_ui.font);
            SetBkMode(dc, TRANSPARENT);
            double cx = 0;
            for (size_t i = 0; i < d->items.size(); ++i) {
                const PartItem& it = d->items[i];
                double w = (double)W_ * (double)it.size / (double)d->size;
                RECT seg{(int)cx + 1, 1, (int)(cx + w) - 1, H - 1};
                cx += w;
                if (seg.right <= seg.left) seg.right = seg.left + 1;
                Tone t = ToneOf((int)i);
                HBRUSH fill = CreateSolidBrush(it.free ? RGB(238, 240, 242) : (t == Tone::None ? RGB(214, 230, 250) : ToneBk(t)));
                FillRect(dc, &seg, fill);
                DeleteObject(fill);
                HPEN pen = CreatePen(PS_SOLID, (int)i == sel ? S(2) : 1, (int)i == sel ? RGB(20, 90, 200) : (it.free ? RGB(180, 185, 190) : ToneColor(t == Tone::None ? Tone::Blue : t)));
                HGDIOBJ op = SelectObject(dc, pen);
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, seg.left, seg.top, seg.right, seg.bottom);
                SelectObject(dc, op);
                SelectObject(dc, ob);
                DeleteObject(pen);
                if (seg.right - seg.left > S(44)) {
                    std::string s = it.free ? (EN() ? "free " : "свободно ") + fb(it.size) : ((it.letter ? std::string(1, it.letter) + ": " : "") + fb(it.size));
                    std::wstring lab = WRaw(s);
                    SetTextColor(dc, RGB(40, 50, 60));
                    RECT tr = seg;
                    DrawTextW(dc, lab.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        int idx = MapHitIndex((short)LOWORD(lp));
        if (idx >= 0) {
            ListView_SetItemState(Q.list, idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(Q.list, idx, FALSE);
            InvalidateRect(h, nullptr, TRUE);
        }
        return 0;
    }
    if (m == WM_NCHITTEST) return HTCLIENT;
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, MapProc, 2);
    return DefSubclassProc(h, m, wp, lp);
}

void FillList() {
    ListView_DeleteAllItems(Q.list);
    PartDisk* d = CurDisk();
    SetWindowTextW(Q.head, d ? WRaw(DiskStr(*d) + (d->freeMax ? (EN() ? "   •   free: " : "   •   свободно: ") + fb(d->freeMax) : std::string())).c_str() : L"");
    if (d) {
        for (size_t i = 0; i < d->items.size(); ++i) {
            const PartItem& it = d->items[i];
            std::wstring c0 = WRaw(it.free ? "—" : fmt("%u", it.number));
            LVITEMW li{};
            li.mask = LVIF_TEXT;
            li.iItem = (int)i;
            li.pszText = (LPWSTR)c0.c_str();
            ListView_InsertItem(Q.list, &li);
            std::wstring off = WRaw(fb(it.offset)), sz = WRaw(fb(it.size)), ty = WRaw(it.type);
            std::wstring lt = WRaw(it.letter ? std::string(1, it.letter) + ":" : "—");
            std::wstring fs = WRaw(it.fs.empty() ? "—" : it.fs);
            std::wstring lb = WRaw(it.label.empty() ? "" : it.label);
            std::wstring fr = WRaw(it.hasVol ? fb(it.freeBytes) : "—");
            ListView_SetItemText(Q.list, (int)i, 1, (LPWSTR)off.c_str());
            ListView_SetItemText(Q.list, (int)i, 2, (LPWSTR)sz.c_str());
            ListView_SetItemText(Q.list, (int)i, 3, (LPWSTR)ty.c_str());
            ListView_SetItemText(Q.list, (int)i, 4, (LPWSTR)lt.c_str());
            ListView_SetItemText(Q.list, (int)i, 5, (LPWSTR)fs.c_str());
            ListView_SetItemText(Q.list, (int)i, 6, (LPWSTR)lb.c_str());
            ListView_SetItemText(Q.list, (int)i, 7, (LPWSTR)fr.c_str());
        }
        if (!d->items.empty()) ListView_SetItemState(Q.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (Q.map) InvalidateRect(Q.map, nullptr, TRUE);
}

void Scan() {
    int keep = CurDiskIdx();
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::string err;
    bool ok = PartScan(Q.disks, err);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    SendMessageW(Q.combo, CB_RESETCONTENT, 0, 0);
    if (!ok) {
        SetWindowTextW(Q.head, WRaw(err).c_str());
        ListView_DeleteAllItems(Q.list);
        if (Q.map) InvalidateRect(Q.map, nullptr, TRUE);
        return;
    }
    for (const auto& d : Q.disks) SendMessageW(Q.combo, CB_ADDSTRING, 0, (LPARAM)WRaw(DiskStr(d)).c_str());
    int sel = (keep >= 0 && keep < (int)Q.disks.size()) ? keep : 0;
    SendMessageW(Q.combo, CB_SETCURSEL, sel, 0);
    FillList();
}

void Start(const std::wstring& opName, std::function<void(Ctx&)> fn) {
    g_pending = opName;
    SetStatus(L("Выполняется: ", "Running: ") + opName + L"…");
    UiStartJob("partition", U(opName), fn);
}

// ------- операции -------
void DoCreate() {
    if (Busy()) return;
    PartDisk* d = CurDisk();
    if (!d) return;
    if (!d->freeMax) { MsgSel("На диске нет свободного места для нового раздела.", "The disk has no free space for a new partition.", "Создать раздел", "Create partition"); return; }
    std::vector<FormField> f(4);
    f[0].label = WRaw(fmt(EN() ? "Partition size in MB (free %s = %llu MB; 1 GB = 1024 MB). Empty or 0 = all free space"
                              : "Размер раздела в МБ (свободно %s = %llu МБ; 1 ГБ = 1024 МБ). Пусто или 0 = всё свободное место",
                          fb(d->freeMax).c_str(), d->freeMax / MiB));
    f[0].kind = 0;
    f[0].value = W(fmt("%llu", d->freeMax / MiB));
    f[1].label = L("Файловая система", "File system");
    f[1].kind = 2;
    f[1].options = {L"NTFS", L"FAT32", L"exFAT", L(EN() ? "No format" : "Без форматирования", "No format")};
    f[1].value = f[1].options[0];
    f[2].label = L("Метка тома (название, необязательно)", "Volume label (name, optional)");
    f[2].kind = 0;
    f[3].label = L("Буква диска", "Drive letter");
    f[3].kind = 2;
    f[3].options = {L(EN() ? "Do not assign" : "Не назначать", "Do not assign")};
    for (char l : FreeLetters()) f[3].options.push_back(std::wstring(1, (wchar_t)l) + L":");
    f[3].value = f[3].options.size() > 1 ? f[3].options[1] : f[3].options[0];
    if (!FormDialog(Q.wnd, L("Создать раздел", "Create partition"), f)) return;
    unsigned long long mb = (unsigned long long)_wtoi64(f[0].value.c_str());
    std::wstring fsOpt = f[1].value;
    std::string fs = fsOpt == L"NTFS" ? "ntfs" : fsOpt == L"FAT32" ? "fat32" : fsOpt == L"exFAT" ? "exfat" : "";
    std::string label = U(f[2].value);
    char letter = (f[3].value.size() >= 2 && f[3].value[1] == L':') ? (char)f[3].value[0] : 0;
    std::string human = mb ? fmt(EN() ? "%llu MB (%.1f GB)" : "%llu МБ (%.1f ГБ)", mb, (double)mb / 1024.0) : (EN() ? "all free space" : "всё свободное место");
    std::string body = DiskStr(*d) + (EN() ? "\n\nCreate a primary partition: " : "\n\nСоздать основной раздел: ") + human +
                       (fs.empty() ? (EN() ? ", no format" : ", без форматирования") : ", " + fs) + (letter ? fmt(EN() ? ", letter %c:" : ", буква %c:", letter) : "") + ".";
    if (!Confirm(WRaw(body), false)) return;
    int disk = d->number;
    Start(L("Создание раздела", "Create partition"), [disk, mb, fs, label, letter](Ctx& c) { PartCreate(c, disk, mb, fs, label, letter); });
}

void DoDelete() {
    if (Busy()) return;
    PartDisk* d = CurDisk();
    PartItem* it = CurItem();
    if (!d || !it || it->free || it->number == 0) { MsgSel("Выберите раздел (не свободную область).", "Select a partition (not free space).", "Удаление", "Delete"); return; }
    unsigned part = it->number;
    std::string body = DiskStr(*d) + fmt(EN() ? "\n\nDELETE partition %u: %s, %s%s.\nAll data on it will be lost permanently."
                                             : "\n\nУДАЛИТЬ раздел %u: %s, %s%s.\nВсе данные на нём будут потеряны безвозвратно.",
                                         part, it->type.c_str(), fb(it->size).c_str(), it->letter ? fmt(EN() ? ", letter %c:" : ", буква %c:", it->letter).c_str() : "");
    if (!Confirm(WRaw(body), true)) return;
    int disk = d->number;
    Start(L("Удаление раздела", "Delete partition"), [disk, part](Ctx& c) { PartDelete(c, disk, part); });
}

void DoFormat() {
    if (Busy()) return;
    PartDisk* d = CurDisk();
    PartItem* it = CurItem();
    if (!d || !it || it->free || it->number == 0) { MsgSel("Выберите раздел.", "Select a partition.", "Форматирование", "Format"); return; }
    unsigned part = it->number;
    std::vector<FormField> f(3);
    f[0].label = L("Файловая система", "File system");
    f[0].kind = 2;
    f[0].options = {L"NTFS", L"FAT32", L"exFAT"};
    f[0].value = f[0].options[0];
    f[1].label = L("Метка тома (название)", "Volume label (name)");
    f[1].kind = 0;
    f[1].value = W(it->label);
    f[2].label = L("Режим", "Mode");
    f[2].kind = 2;
    f[2].options = {L(EN() ? "Quick format" : "Быстрое форматирование", "Quick format"), L(EN() ? "Full format" : "Полное форматирование", "Full format")};
    f[2].value = f[2].options[0];
    if (!FormDialog(Q.wnd, L("Форматировать раздел", "Format partition"), f)) return;
    std::wstring fsOpt = f[0].value;
    std::string fs = fsOpt == L"FAT32" ? "fat32" : fsOpt == L"exFAT" ? "exfat" : "ntfs";
    std::string label = U(f[1].value);
    bool quick = f[2].value == f[2].options[0];
    std::string body = DiskStr(*d) + fmt(EN() ? "\n\nFORMAT partition %u%s (%s, %s) as %s.\nAll data on this partition will be erased."
                                             : "\n\nФОРМАТИРОВАТЬ раздел %u%s (%s, %s) в %s.\nВсе данные на этом разделе будут удалены.",
                                         part, it->letter ? fmt(" (%c:)", it->letter).c_str() : "", it->type.c_str(), fb(it->size).c_str(), fs.c_str());
    if (!Confirm(WRaw(body), true)) return;
    int disk = d->number;
    Start(L("Форматирование", "Format"), [disk, part, fs, label, quick](Ctx& c) { PartFormat(c, disk, part, fs, label, quick); });
}

void DoLetter() {
    if (Busy()) return;
    PartDisk* d = CurDisk();
    PartItem* it = CurItem();
    if (!d || !it || it->free || it->number == 0) { MsgSel("Выберите раздел.", "Select a partition.", "Буква", "Letter"); return; }
    unsigned part = it->number;
    std::vector<FormField> f(1);
    f[0].label = it->letter ? WRaw(fmt(EN() ? "Current letter %c:. Pick a new one or «Remove letter»" : "Сейчас буква %c:. Выберите новую или «Убрать букву»", it->letter))
                            : L("Буква диска для раздела (сейчас буквы нет)", "Drive letter for the partition (none now)");
    f[0].kind = 2;
    f[0].options = {L(EN() ? "Remove letter" : "Убрать букву", "Remove letter")};
    for (char l : FreeLetters()) f[0].options.push_back(std::wstring(1, (wchar_t)l) + L":");
    f[0].value = it->letter ? f[0].options[0] : (f[0].options.size() > 1 ? f[0].options[1] : f[0].options[0]);
    if (!FormDialog(Q.wnd, L("Буква раздела", "Partition letter"), f)) return;
    char letter = (f[0].value.size() >= 2 && f[0].value[1] == L':') ? (char)f[0].value[0] : 0;
    int disk = d->number;
    Start(L("Буква раздела", "Drive letter"), [disk, part, letter](Ctx& c) { PartSetLetter(c, disk, part, letter); });
}

void DoActive() {
    if (Busy()) return;
    PartDisk* d = CurDisk();
    PartItem* it = CurItem();
    if (!d || !it || it->free || it->number == 0) { MsgSel("Выберите раздел.", "Select a partition.", "Активный", "Active"); return; }
    if (d->style != "MBR") { MsgSel("«Активный» раздел есть только на дисках MBR (на GPT загрузкой управляет ESP).", "An «active» partition exists only on MBR disks (on GPT the ESP controls booting).", "Активный", "Active"); return; }
    unsigned part = it->number;
    bool active = !it->active;
    std::string body = DiskStr(*d) + fmt(EN() ? "\n\nPartition %u: %s." : "\n\nРаздел %u: %s.", part,
                                         active ? (EN() ? "make active (bootable)" : "сделать активным (загрузочным)") : (EN() ? "clear active flag" : "снять активность"));
    if (!Confirm(WRaw(body), false)) return;
    int disk = d->number;
    Start(L("Активный раздел", "Active flag"), [disk, part, active](Ctx& c) { PartActive(c, disk, part, active); });
}

void DoShrink() {
    if (Busy()) return;
    PartDisk* d = CurDisk();
    PartItem* it = CurItem();
    if (!d || !it || it->free || it->number == 0) { MsgSel("Выберите раздел.", "Select a partition.", "Сжать / Разделить", "Shrink / Split"); return; }
    unsigned part = it->number;
    std::vector<FormField> f(1);
    f[0].label = WRaw(fmt(EN() ? "Partition is %s now. By how many MB to shrink it? The freed space at the end becomes a new partition (split). 1 GB = 1024 MB."
                              : "Сейчас раздел %s. Насколько МБ его сжать? Освободится место в конце — из него можно создать новый раздел (разделить). 1 ГБ = 1024 МБ.",
                          fb(it->size).c_str()));
    f[0].kind = 0;
    if (!FormDialog(Q.wnd, L("Сжать / разделить раздел", "Shrink / split partition"), f)) return;
    unsigned long long mb = (unsigned long long)_wtoi64(f[0].value.c_str());
    if (!mb) { MsgSel("Укажите размер в МБ.", "Enter the size in MB.", "Сжать / Разделить", "Shrink / Split"); return; }
    std::string body = DiskStr(*d) + fmt(EN() ? "\n\nShrink partition %u by %llu MB (%.1f GB). Now: %s.\nThe freed space appears as «free» — create a new partition in it."
                                             : "\n\nСжать раздел %u на %llu МБ (%.1f ГБ). Сейчас: %s.\nОсвободившееся место появится как «свободно» — создайте в нём новый раздел.",
                                         part, mb, (double)mb / 1024.0, fb(it->size).c_str());
    if (!Confirm(WRaw(body), false)) return;
    int disk = d->number;
    Start(L("Сжатие раздела", "Shrink partition"), [disk, part, mb](Ctx& c) { PartShrink(c, disk, part, mb); });
}

void DoExtend() {
    if (Busy()) return;
    PartDisk* d = CurDisk();
    PartItem* it = CurItem();
    int idx = CurIdx();
    if (!d || !it || it->free || it->number == 0) { MsgSel("Выберите раздел.", "Select a partition.", "Расширить", "Extend"); return; }
    unsigned long long freeAfter = 0;
    if (idx >= 0 && idx + 1 < (int)d->items.size() && d->items[(size_t)idx + 1].free) freeAfter = d->items[(size_t)idx + 1].size;
    if (!freeAfter) { MsgSel("Сразу за этим разделом нет свободного места — расширять некуда. Расширение возможно только в непрерывное свободное место после раздела.",
                             "No free space right after this partition — nothing to extend into. Extending works only into contiguous free space after the partition.", "Расширить", "Extend"); return; }
    unsigned part = it->number;
    std::vector<FormField> f(1);
    f[0].label = WRaw(fmt(EN() ? "Free space right after the partition: %s = %llu MB. By how many MB to extend? Empty = use all free space. 1 GB = 1024 MB."
                              : "Сразу за разделом свободно %s = %llu МБ. На сколько МБ расширить? Пусто = занять всё. 1 ГБ = 1024 МБ.",
                          fb(freeAfter).c_str(), freeAfter / MiB));
    f[0].kind = 0;
    f[0].value = W(fmt("%llu", freeAfter / MiB));
    if (!FormDialog(Q.wnd, L("Расширить раздел", "Extend partition"), f)) return;
    unsigned long long mb = (unsigned long long)_wtoi64(f[0].value.c_str());
    std::string amount = mb ? fmt(EN() ? "%llu MB (%.1f GB)" : "%llu МБ (%.1f ГБ)", mb, (double)mb / 1024.0) : (EN() ? "all free space right after it" : "всё свободное место сразу за ним");
    std::string body = DiskStr(*d) + fmt(EN() ? "\n\nExtend partition %u by %s." : "\n\nРасширить раздел %u на %s.", part, amount.c_str());
    if (!Confirm(WRaw(body), false)) return;
    int disk = d->number;
    Start(L("Расширение раздела", "Extend partition"), [disk, part, mb](Ctx& c) { PartExtend(c, disk, part, mb); });
}

void DoConvert() {
    if (Busy()) return;
    PartDisk* d = CurDisk();
    if (!d) return;
    std::vector<FormField> f(1);
    f[0].label = L("Стиль разделов (диск должен быть ПУСТ — сначала удалите все разделы)", "Partition style (disk must be EMPTY — delete all partitions first)");
    f[0].kind = 2;
    f[0].options = {L"GPT", L"MBR"};
    f[0].value = d->style == "GPT" ? L"MBR" : L"GPT";
    if (!FormDialog(Q.wnd, L("Конвертировать диск", "Convert disk"), f)) return;
    bool toGpt = f[0].value == L"GPT";
    std::string body = DiskStr(*d) + fmt(EN() ? "\n\nConvert the disk to %s. The disk must be empty (no partitions), otherwise diskpart will refuse."
                                             : "\n\nКонвертировать диск в %s. Диск должен быть пустым (без разделов), иначе diskpart откажет.",
                                         toGpt ? "GPT" : "MBR");
    if (!Confirm(WRaw(body), false)) return;
    int disk = d->number;
    Start(L("Конвертация диска", "Convert disk"), [disk, toGpt](Ctx& c) { PartConvert(c, disk, toGpt); });
}

void ShowOpsMenu(LPARAM lp) {
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    if (lp == (LPARAM)-1) { RECT r; GetWindowRect(Q.list, &r); pt = {r.left + 24, r.top + 48}; }
    HMENU m = CreatePopupMenu();
    auto add = [&](int b, const wchar_t* t) { AppendMenuW(m, MF_STRING, (UINT)(ID_B0 + b), TrW(t).c_str()); };
    add(B_CREATE, L"Создать раздел…");
    add(B_DELETE, L"Удалить раздел…");
    add(B_FORMAT, L"Форматировать…");
    add(B_LETTER, L"Буква…");
    add(B_ACTIVE, L"Активный (MBR)…");
    add(B_SHRINK, L"Сжать / Разделить…");
    add(B_EXTEND, L"Расширить…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(B_CONVERT, L"Конвертировать GPT/MBR…");
    add(B_REFRESH, L"Обновить");
    SetForegroundWindow(Q.wnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, Q.wnd, nullptr);
    DestroyMenu(m);
    if (cmd >= ID_B0 && cmd < ID_B0 + B_COUNT) SendMessageW(Q.wnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

void Layout() {
    RECT rc;
    GetClientRect(Q.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8), y = m, bh = S(30);
    Place(Q.warn, m, y, W_ - 2 * m, S(40));
    y += S(46);
    Place(Q.combo, m, y, W_ - 2 * m, S(320));
    y += S(38);
    Place(Q.head, m, y, W_ - 2 * m, S(28));
    y += S(32);
    Place(Q.map, m, y, W_ - 2 * m, S(34));
    y += S(40);
    int rows = 2, gridH = rows * (bh + S(6));
    int listH = H - y - m - S(22) - S(22) - gridH - S(10);
    Place(Q.list, m, y, W_ - 2 * m, std::max(S(120), listH));
    y += std::max(S(120), listH) + S(4);
    Place(Q.legend, m, y, W_ - 2 * m, S(18));
    y += S(22);
    Place(Q.status, m, y, W_ - 2 * m, S(20));
    y += S(24);
    int cols = 6, bw = (W_ - 2 * m - (cols - 1) * S(6)) / cols;
    for (int i = 0; i < B_COUNT; ++i) Place(Q.btn[i], m + (i % cols) * (bw + S(6)), y + (i / cols) * (bh + S(6)), bw, bh);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (Q.btn[B_CLOSE]) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(1000); mm->ptMinTrackSize.y = S(640); return 0; }
        case WM_CONTEXTMENU:
            if ((HWND)wp == Q.list) { ShowOpsMenu(lp); return 1; }
            return 0;
        case WM_CTLCOLORSTATIC:
            if ((HWND)lp == Q.warn) {
                static HBRUSH rb = CreateSolidBrush(RGB(255, 235, 235));
                SetBkColor((HDC)wp, RGB(255, 235, 235));
                SetTextColor((HDC)wp, RGB(170, 30, 30));
                return (LRESULT)rb;
            }
            if ((HWND)lp == Q.head) { SetBkMode((HDC)wp, TRANSPARENT); SetTextColor((HDC)wp, RGB(26, 103, 210)); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
            if ((HWND)lp == Q.status) { SetBkMode((HDC)wp, TRANSPARENT); SetTextColor((HDC)wp, RGB(30, 120, 60)); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
            break;
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == ID_DISK && code == CBN_SELCHANGE) { FillList(); return 0; }
            if (id >= ID_B0 && id < ID_B0 + B_COUNT) {
                switch (id - ID_B0) {
                    case B_CREATE: DoCreate(); break;
                    case B_DELETE: DoDelete(); break;
                    case B_FORMAT: DoFormat(); break;
                    case B_LETTER: DoLetter(); break;
                    case B_ACTIVE: DoActive(); break;
                    case B_SHRINK: DoShrink(); break;
                    case B_EXTEND: DoExtend(); break;
                    case B_CONVERT: DoConvert(); break;
                    case B_REFRESH: Scan(); break;
                    case B_CLOSE: DestroyWindow(h); break;
                }
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->hwndFrom == Q.list) {
                if (nh->code == NM_CUSTOMDRAW) return PartListDraw(lp);
                if (nh->code == LVN_ITEMCHANGED && Q.map) InvalidateRect(Q.map, nullptr, TRUE);
            }
            return 0;
        }
        case WM_DESTROY: Q.wnd = nullptr; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void OpenPartitionToolImpl() {
    if (Q.wnd) { ShowWindow(Q.wnd, SW_RESTORE); SetForegroundWindow(Q.wnd); return; }
    RegisterOnce(L"WRPart", Proc);
    Q = St();
    Q.wnd = CreateWindowExW(0, L"WRPart", L"Разделы диска (diskpart) — все операции необратимы (F1 — справка)", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, S(1160), S(800), g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = Q.wnd;
    Q.warn = Mk(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, ID_WARN);
    SetWindowTextW(Q.warn, WRaw(EN()
        ? "  For advanced users. Operations are IRREVERSIBLE — there is NO undo and data cannot be recovered. Pick the correct disk (check model, size, serial). Best run from WinPE/WinRE."
        : "  Для продвинутых пользователей. Операции НЕОБРАТИМЫ — отмены нет, данные не восстановить. Выбирайте правильный диск (сверяйте модель, размер, серийный номер). Лучше запускать из WinPE/WinRE.").c_str());
    Q.combo = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_DISK);
    SendMessageW(Q.combo, WM_SETFONT, (WPARAM)MedBold(), TRUE);
    Q.head = Mk(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, ID_HEAD);
    SendMessageW(Q.head, WM_SETFONT, (WPARAM)HeadFont(), TRUE);
    Q.map = Mk(h, L"STATIC", L"", SS_LEFT | SS_NOTIFY, ID_MAP);
    SetWindowSubclass(Q.map, MapProc, 2, 0);
    Q.list = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
    SendMessageW(Q.list, WM_SETFONT, (WPARAM)MedFont(), TRUE);
    ListView_SetExtendedListViewStyle(Q.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[] = {L"№", L"Смещение", L"Размер", L"Тип", L"Буква", L"ФС", L"Метка", L"Свободно"};
    int cw[] = {S(50), S(140), S(140), S(240), S(80), S(100), S(200), S(140)};
    for (int i = 0; i < 8; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = cw[i]; ListView_InsertColumn(Q.list, i, &c); }
    Q.legend = MkLegend(h, ID_LEGEND, {{Tone::Blue, L"системный (EFI/MSR/восстановление)"}, {Tone::Orange, L"активный"}, {Tone::Gray, L"свободно"}});
    Q.status = Mk(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, ID_STATUS);
    SetWindowTextW(Q.status, WRaw(EN() ? "Right-click a row for the operations menu. Click the bar to select. All operations are irreversible."
                                       : "Правый клик по строке — меню операций. Клик по полосе — выбрать раздел. Все операции необратимы.").c_str());
    const wchar_t* bt[] = {L"Создать раздел…", L"Удалить раздел…", L"Форматировать…", L"Буква…", L"Активный (MBR)…", L"Сжать / Разделить…",
                           L"Расширить…", L"Конвертировать GPT/MBR…", L"Обновить", L"Закрыть"};
    Tone tn[] = {Tone::Orange, Tone::Red, Tone::Orange, Tone::Orange, Tone::Orange, Tone::Orange, Tone::Orange, Tone::Red, Tone::Gray, Tone::Gray};
    for (int i = 0; i < B_COUNT; ++i) Q.btn[i] = MkBtn(h, bt[i], ID_B0 + i, tn[i]);
    Layout();
    ShowWindow(h, SW_SHOW);
    Scan();
}

void PartitionJobDone() {
    if (!Q.wnd) return;
    if (!g_pending.empty()) {
        SetStatus(L("Готово: ", "Done: ") + g_pending + L(" — список обновлён (подробности в журнале).", " — list updated (details in the log)."));
        g_pending.clear();
    }
    Scan();
}

}  // namespace ui

void OpenPartitionTool() { ui::OpenPartitionToolImpl(); }

}  // namespace wr
