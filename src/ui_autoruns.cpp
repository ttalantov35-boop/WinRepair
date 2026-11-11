// Offline Autoruns + msconfig (services/startup) window.
#include "ui_tools_common.h"
#include "modules/autoruns.h"

namespace wr {
namespace ui {

namespace {

enum { ID_TARGET = 6001, ID_SCAN, ID_HIDESYS, ID_ONLYFLAG, ID_LIST, ID_DISABLE, ID_ENABLE, ID_DELETE, ID_EDITCMD, ID_CLEAN, ID_REVERT, ID_CSV, ID_BCD, ID_REG, ID_CLOSE, ID_INFO };

struct State {
    HWND wnd = nullptr, combo, list, hideSys, onlyFlag, info, btn[10];
    std::vector<Target> targets;
    std::vector<AutoItem> items;
    std::vector<int> view;  // indices into items
    bool pinned = false;
    std::map<int, std::string> flt;   // column -> exact value shown only (like an Excel column filter)
};
State A;

const wchar_t* kCols[] = {L"Состояние", L"Тип", L"Имя", L"Команда / образ", L"Файл", L"Пометка", L"Расположение"};
constexpr int kNCols = 7;

void Cells(const AutoItem& it, std::string c[kNCols]) {
    c[0] = it.state; c[1] = it.kind; c[2] = it.name; c[3] = it.command;
    c[4] = it.fileExists ? "да" : (it.file.empty() ? "" : "НЕТ");
    c[5] = it.flag; c[6] = it.location;
}

bool PassBase(const AutoItem& it) {
    bool hide = SendMessageW(A.hideSys, BM_GETCHECK, 0, 0) == BST_CHECKED, only = SendMessageW(A.onlyFlag, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (hide && (it.op == AOp::Service) && !it.thirdParty && it.flag.empty()) return false;
    if (only && it.flag.empty() && !(it.op == AOp::Service && it.thirdParty)) return false;
    return true;
}

bool PassFilters(const std::string c[kNCols], int skipCol) {
    for (const auto& f : A.flt) if (f.first != skipCol && c[f.first] != f.second) return false;
    return true;
}

void MarkHeaders() {   // a filtered column carries a marker in its caption
    for (int i = 0; i < kNCols; ++i) {
        std::wstring t = TrW(kCols[i]);
        if (A.flt.count(i)) t += L"  [\u25BC]";
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT;
        c.pszText = (LPWSTR)t.c_str();
        SendMessageW(A.list, LVM_SETCOLUMNW, (WPARAM)i, (LPARAM)&c);
    }
}

void Fill() {
    ListView_DeleteAllItems(A.list);
    A.view.clear();
    for (size_t i = 0; i < A.items.size(); ++i) {
        const auto& it = A.items[i];
        if (!PassBase(it)) continue;
        std::string c[kNCols];
        Cells(it, c);
        if (!PassFilters(c, -1)) continue;
        int row = (int)A.view.size();
        A.view.push_back((int)i);
        std::wstring st = W(c[0]);
        LVITEMW li{};
        li.mask = LVIF_TEXT;
        li.iItem = row;
        li.pszText = (LPWSTR)st.c_str();
        ListView_InsertItem(A.list, &li);
        for (int k = 1; k < kNCols; ++k) ListView_SetItemText(A.list, row, k, (LPWSTR)W(c[k]).c_str());
    }
    MarkHeaders();
    int nflag = 0;
    for (auto& i : A.items) if (!i.flag.empty()) ++nflag;
    std::string extra = A.flt.empty() ? "" : fmt("  Фильтров по столбцам: %zu (правая кнопка на таблице — сбросить).", A.flt.size());
    SetWindowTextW(A.info, W(fmt("Записей: %zu, показано: %zu, с пометками: %d.  Отключение — обратимо (StartupApproved / Start=4 с резервным значением).", A.items.size(), A.view.size(), nflag) + extra).c_str());
}

int Target_() { return (int)SendMessageW(A.combo, CB_GETCURSEL, 0, 0); }

void Scan() {
    int t = Target_();
    if (t < 0 || t >= (int)A.targets.size()) { MessageBoxW(A.wnd, L"Нет офлайн-целей. Подключите диск с Windows и нажмите «Обновить» в главном окне.", L"Автозагрузка", MB_OK | MB_ICONINFORMATION); return; }
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    if (!A.pinned) { Hives().pin(); A.pinned = true; }
    std::string err;
    A.items = ScanAutoruns(A.targets[(size_t)t], err);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!err.empty()) MessageBoxW(A.wnd, W(err).c_str(), L"Автозагрузка", MB_OK | MB_ICONWARNING);
    Fill();
}

const AutoItem* Sel() {
    int s = ListView_GetNextItem(A.list, -1, LVNI_SELECTED);
    if (s < 0 || s >= (int)A.view.size()) return nullptr;
    return &A.items[(size_t)A.view[(size_t)s]];
}

bool Confirm(const std::wstring& t) { return MessageBoxW(A.wnd, (t + L"\n\nПродолжить?").c_str(), L"Подтверждение", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES; }

void DoOne(int what) {
    const AutoItem* it = Sel();
    if (!it) { MessageBoxW(A.wnd, L"Выберите запись.", L"Автозагрузка", MB_OK | MB_ICONINFORMATION); return; }
    AutoItem cp = *it;
    std::string err;
    bool ok = false;
    std::wstring head = L"Запись: " + W(cp.name) + L"\nТип: " + W(cp.kind) + L"\nКоманда: " + W(cp.command) + L"\nРасположение: " + W(cp.location);
    if (what == 0 || what == 1) {
        if (!Confirm(head + (what == 0 ? L"\n\nОТКЛЮЧИТЬ запись (обратимо)." : L"\n\nВКЛЮЧИТЬ запись."))) return;
        ok = AutorunSetEnabled(cp, what == 1, err);
    } else if (what == 2) {
        if (!Confirm(head + L"\n\nУДАЛИТЬ запись (значение/файл будет удалён; значение записывается в журнал изменений).")) return;
        ok = AutorunDelete(cp, err);
    } else {
        std::vector<FormField> f(1);
        f[0].label = L"Новая команда / значение";
        f[0].kind = 1;
        f[0].value = W(cp.command);
        if (!FormDialog(A.wnd, L"Изменить команду", f)) return;
        if (!Confirm(head + L"\n\nНовое значение:\n" + f[0].value)) return;
        ok = AutorunSetCommand(cp, U(f[0].value), err);
    }
    if (!ok) MessageBoxW(A.wnd, W(err.empty() ? "Операция не выполнена." : err).c_str(), L"Ошибка", MB_OK | MB_ICONERROR);
    Scan();
}

void CleanBoot(bool disable) {
    if (A.items.empty()) { MessageBoxW(A.wnd, L"Сначала выполните сканирование.", L"Чистая загрузка", MB_OK | MB_ICONINFORMATION); return; }
    std::wstring t = disable ? L"ЧИСТАЯ ЗАГРУЗКА: будут отключены все сторонние службы/драйверы (вне Windows\\System32) с автозапуском и все записи автозагрузки (Run, папка «Автозагрузка»).\nОтключение обратимо кнопкой «Вернуть отключённое»."
                             : L"Будут ВКЛЮЧЕНЫ все отключённые записи автозагрузки и службы (включая отключённые ранее вручную).";
    if (!Confirm(t)) return;
    int n = 0, bad = 0;
    for (const auto& it : A.items) {
        std::string err;
        bool target = false;
        if (disable) {
            if (it.state == "Отключено") continue;
            target = (it.op == AOp::Service && it.thirdParty && it.origStart == 2) || ((it.op == AOp::RunValue && !it.approvedSub.empty()) || it.op == AOp::StartupFile);
        } else {
            if (it.state != "Отключено") continue;
            target = it.op == AOp::Service || it.op == AOp::StartupFile || (it.op == AOp::RunValue && !it.approvedSub.empty());
        }
        if (!target) continue;
        if (AutorunSetEnabled(it, !disable, err)) ++n; else ++bad;
    }
    Logger::I().log(Lvl::Ok, fmt("%s: изменено записей %d, ошибок %d", disable ? "Чистая загрузка" : "Возврат отключённого", n, bad));
    Scan();
}

void SaveCsv() {
    wchar_t file[MAX_PATH] = L"autoruns.csv";
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o; o.hwndOwner = A.wnd; o.lpstrFilter = L"CSV (*.csv)\0*.csv\0"; o.lpstrFile = file; o.nMaxFile = MAX_PATH; o.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&o)) return;
    if (WriteText(U(file), AutorunsCsv(A.items))) Logger::I().log(Lvl::Ok, "Автозагрузка сохранена: " + U(file));
}

// Excel-style column filter: lists the values of the column (with counts) and keeps only the rows with the chosen one.
void FilterMenu(int col, POINT pt) {
    std::map<std::string, int> cnt;
    for (const auto& it : A.items) {
        if (!PassBase(it)) continue;
        std::string c[kNCols];
        Cells(it, c);
        if (PassFilters(c, col)) ++cnt[c[col]];
    }
    std::vector<std::string> vals;
    for (const auto& kv : cnt) vals.push_back(kv.first);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (A.flt.count(col) ? 0 : MF_GRAYED), 1, (TrW(L"Показать все значения столбца «") + TrW(kCols[col]) + L"»").c_str());
    AppendMenuW(m, MF_STRING | (A.flt.empty() ? MF_GRAYED : 0), 2, TrW(L"Сбросить все фильтры").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const size_t kMax = 60;
    for (size_t i = 0; i < vals.size() && i < kMax; ++i) {
        std::wstring t = vals[i].empty() ? TrW(L"(пусто)") : TrW(W(vals[i]));
        if (t.size() > 70) t = t.substr(0, 67) + L"…";
        for (size_t p = 0; (p = t.find(L'&', p)) != std::wstring::npos; p += 2) t.insert(p, 1, L'&');   // literal ampersand
        t += L"   (" + std::to_wstring(cnt[vals[i]]) + L")";
        UINT fl = MF_STRING | ((A.flt.count(col) && A.flt[col] == vals[i]) ? MF_CHECKED : 0);
        AppendMenuW(m, fl, (UINT_PTR)(100 + i), t.c_str());
    }
    if (vals.size() > kMax) AppendMenuW(m, MF_STRING | MF_GRAYED, 9999, (TrW(L"… показаны первые ") + std::to_wstring(kMax) + TrW(L" значений")).c_str());
    SetForegroundWindow(A.wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, A.wnd, nullptr);
    DestroyMenu(m);
    if (c == 1) A.flt.erase(col);
    else if (c == 2) A.flt.clear();
    else if (c >= 100 && c < 100 + (int)std::min(vals.size(), kMax)) A.flt[col] = vals[(size_t)(c - 100)];
    else return;
    Fill();
}

int ColumnAt(POINT screen) {   // column under a screen point inside the table (header or cell); -1 if none
    HWND hdr = ListView_GetHeader(A.list);
    POINT hp = screen;
    ScreenToClient(hdr, &hp);
    HDHITTESTINFO hh{};
    hh.pt = hp;
    SendMessageW(hdr, HDM_HITTEST, 0, (LPARAM)&hh);
    return hh.iItem >= 0 && hh.iItem < kNCols ? hh.iItem : -1;
}

// Right-click on the table: filter by the column under the pointer, copy, and the row commands. Outside the table: nothing.
bool ListContextMenu(HWND src, LPARAM lp) {
    HWND hdr = ListView_GetHeader(A.list);
    if (src != A.list && src != hdr) return false;
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    if (lp == (LPARAM)-1) { RECT r; GetWindowRect(A.list, &r); pt = {r.left + S(20), r.top + S(20)}; }
    int col = lp == (LPARAM)-1 ? 0 : ColumnAt(pt);
    bool onHeader = src == hdr;
    int row = -1;
    if (!onHeader && lp != (LPARAM)-1) {
        POINT c = pt;
        ScreenToClient(A.list, &c);
        LVHITTESTINFO hi{};
        hi.pt = c;
        ListView_SubItemHitTest(A.list, &hi);
        row = hi.iItem;
        if (hi.iSubItem >= 0 && hi.iSubItem < kNCols) col = hi.iSubItem;
        if (row >= 0) ListView_SetItemState(A.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (col < 0) col = 0;
    if (onHeader) { FilterMenu(col, pt); return true; }
    const AutoItem* it = Sel();
    enum { M_FILTER = 1, M_RESET, M_CELL, M_ROW, M_ALL, M_OFF, M_ON, M_DEL, M_EDIT };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, M_FILTER, (TrW(L"Фильтр по столбцу «") + TrW(kCols[col]) + L"»…").c_str());
    AppendMenuW(m, MF_STRING | (A.flt.empty() ? MF_GRAYED : 0), M_RESET, TrW(L"Сбросить все фильтры").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (it ? 0 : MF_GRAYED), M_CELL, TrW(L"Копировать ячейку").c_str());
    AppendMenuW(m, MF_STRING | (it ? 0 : MF_GRAYED), M_ROW, TrW(L"Копировать строку").c_str());
    AppendMenuW(m, MF_STRING | (A.view.empty() ? MF_GRAYED : 0), M_ALL, TrW(L"Копировать всю таблицу").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (it ? 0 : MF_GRAYED), M_OFF, TrW(L"Отключить запись…").c_str());
    AppendMenuW(m, MF_STRING | (it ? 0 : MF_GRAYED), M_ON, TrW(L"Включить запись…").c_str());
    AppendMenuW(m, MF_STRING | (it ? 0 : MF_GRAYED), M_EDIT, TrW(L"Изменить команду…").c_str());
    AppendMenuW(m, MF_STRING | (it ? 0 : MF_GRAYED), M_DEL, TrW(L"Удалить запись…").c_str());
    SetForegroundWindow(A.wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, A.wnd, nullptr);
    DestroyMenu(m);
    int si = ListView_GetNextItem(A.list, -1, LVNI_SELECTED);
    switch (c) {
        case M_FILTER: FilterMenu(col, pt); break;
        case M_RESET: A.flt.clear(); Fill(); break;
        case M_CELL: if (si >= 0) { wchar_t b[2048] = L""; ListView_GetItemText(A.list, si, col, b, 2048); ClipText(A.wnd, b); } break;
        case M_ROW: if (si >= 0) ClipText(A.wnd, ListRowText(A.list, si)); break;
        case M_ALL: { std::wstring all; for (int i = 0, n = ListView_GetItemCount(A.list); i < n; ++i) all += ListRowText(A.list, i) + L"\r\n"; ClipText(A.wnd, all); break; }
        case M_OFF: DoOne(0); break;
        case M_ON: DoOne(1); break;
        case M_EDIT: DoOne(3); break;
        case M_DEL: DoOne(2); break;
    }
    return true;
}

void Layout() {
    RECT rc;
    GetClientRect(A.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8), y = m;
    Place(GetDlgItem(A.wnd, 6900), m, y + S(3), S(100), S(20));
    Place(A.combo, m + S(105), y, W_ - 2 * m - S(105) - S(130), S(300));
    Place(GetDlgItem(A.wnd, ID_SCAN), W_ - m - S(120), y - 1, S(120), S(26));
    y += S(32);
    Place(A.hideSys, m, y, S(330), S(20));
    Place(A.onlyFlag, m + S(340), y, S(380), S(20));
    y += S(26);
    int bh = S(30), rows = 2;
    int listH = H - y - m - rows * (bh + S(6)) - S(26);
    Place(A.list, m, y, W_ - 2 * m, listH);
    y += listH + m;
    Place(A.info, m, y, W_ - 2 * m, S(18));
    y += S(22);
    int bw = (W_ - 2 * m - 4 * S(6)) / 5;
    for (int i = 0; i < 10; ++i) Place(A.btn[i], m + (i % 5) * (bw + S(6)), y + (i / 5) * (bh + S(6)), bw, bh);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CONTEXTMENU: return ListContextMenu((HWND)wp, lp) ? 1 : 0;
        case WM_SIZE: if (A.btn[9]) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(980); mm->ptMinTrackSize.y = S(560); return 0; }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_SCAN: Scan(); break;
                case ID_HIDESYS: case ID_ONLYFLAG: Fill(); break;
                case ID_DISABLE: DoOne(0); break;
                case ID_ENABLE: DoOne(1); break;
                case ID_DELETE: DoOne(2); break;
                case ID_EDITCMD: DoOne(3); break;
                case ID_CLEAN: CleanBoot(true); break;
                case ID_REVERT: CleanBoot(false); break;
                case ID_CSV: SaveCsv(); break;
                case ID_BCD: OpenBcdManager(); break;
                case ID_REG: OpenRegistryEditor(); break;
                case ID_CLOSE: DestroyWindow(h); break;
            }
            return 0;
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->hwndFrom == A.list && nh->code == NM_DBLCLK) DoOne(3);
            else if (nh->hwndFrom == A.list && nh->code == LVN_COLUMNCLICK) {   // click on a column caption = value filter of that column
                POINT pt;
                GetCursorPos(&pt);
                FilterMenu(((NMLISTVIEW*)lp)->iSubItem, pt);
            }
            return 0;
        }
        case WM_DESTROY:
            if (A.pinned) { Hives().unpin(); A.pinned = false; }
            A.wnd = nullptr;
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void OpenAutorunsImpl() {
    if (A.wnd) { ShowWindow(A.wnd, SW_RESTORE); SetForegroundWindow(A.wnd); return; }
    RegisterOnce(L"WRAutoruns", Proc);
    A = State();
    A.wnd = CreateWindowExW(0, L"WRAutoruns", L"Автозагрузка и службы офлайн-Windows (Autoruns + msconfig)", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1200), S(720),
                            g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = A.wnd;
    Mk(h, L"STATIC", L"Целевая ОС:", SS_LEFT, 6900);
    A.combo = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_TARGET);
    A.targets = FillTargetCombo(A.combo, true);
    Mk(h, L"BUTTON", L"Сканировать", BS_PUSHBUTTON | WS_TABSTOP, ID_SCAN);
    A.hideSys = Mk(h, L"BUTTON", L"Скрыть системные службы и драйверы (как «Не показывать службы Microsoft»)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_HIDESYS);
    SendMessageW(A.hideSys, BM_SETCHECK, BST_CHECKED, 0);
    A.onlyFlag = Mk(h, L"BUTTON", L"Только записи с пометками (подозрительные / файл не найден)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_ONLYFLAG);
    A.list = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(A.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const wchar_t** cols = kCols;
    int cw[] = {S(110), S(150), S(220), S(380), S(50), S(150), S(380)};
    for (int i = 0; i < 7; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = cw[i]; ListView_InsertColumn(A.list, i, &c); }
    A.info = Mk(h, L"STATIC", L"", SS_LEFT, ID_INFO);
    const wchar_t* bt[] = {L"Отключить…", L"Включить…", L"Удалить…", L"Изменить команду…", L"Экспорт CSV…", L"Чистая загрузка…", L"Вернуть отключённое…", L"Загрузка (BCD)…", L"Реестр офлайн…", L"Закрыть"};
    int ids[] = {ID_DISABLE, ID_ENABLE, ID_DELETE, ID_EDITCMD, ID_CSV, ID_CLEAN, ID_REVERT, ID_BCD, ID_REG, ID_CLOSE};
    for (int i = 0; i < 10; ++i) A.btn[i] = Mk(h, L"BUTTON", bt[i], BS_PUSHBUTTON | WS_TABSTOP, ids[i]);
    Layout();
    ShowWindow(h, SW_SHOW);
    if (!A.targets.empty()) Scan();
    else SetWindowTextW(A.info, L"Офлайн-системы не найдены. В WinPE подключите диск с Windows и обновите список в главном окне.");
}

void AutorunsJobDone() {}

}  // namespace ui
}  // namespace wr
