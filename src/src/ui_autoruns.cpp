// Offline Autoruns + msconfig (services/startup) window.
#include "ui_tools_common.h"
#include "modules/autoruns.h"
#include <shellapi.h>
#include <cctype>

namespace wr {
namespace ui {

namespace {

enum { ID_TARGET = 6001, ID_SCAN, ID_HIDESYS, ID_ONLYFLAG, ID_LIST, ID_DISABLE, ID_ENABLE, ID_DELETE, ID_EDITCMD, ID_CLEAN, ID_REVERT, ID_CSV, ID_BCD, ID_REG, ID_CLOSE, ID_INFO, ID_CATTAB, ID_QF, ID_QFLBL };

struct State {
    HWND wnd = nullptr, combo, list, hideSys, onlyFlag, info, btn[10], cat = nullptr, qf = nullptr;
    std::vector<Target> targets;
    std::vector<AutoItem> items;
    std::vector<int> view;  // indices into items
    bool pinned = false;
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

// Categories of the tab strip, as in Sysinternals Autoruns: Everything, Logon, Services, Drivers, Scheduled Tasks, Other (hijack points).
const wchar_t* kCats[] = {L"Всё", L"Вход и автозапуск", L"Службы", L"Драйверы", L"Планировщик", L"Прочее (перехват)"};
int CatOf(const AutoItem& it) {
    if (it.kind == "Служба") return 2;
    if (it.kind == "Драйвер") return 3;
    if (it.kind.rfind("Планировщик", 0) == 0) return 4;
    if (it.op == AOp::Ifeo || it.op == AOp::MoveValue || it.op == AOp::EditOnly) return 5;
    return 1;
}

bool PassQuick(const std::string c[kNCols]) {   // quick filter: the text may be in any column (case-insensitive), like the Filter box of Autoruns
    std::string q = trim(U(GetText(A.qf)));
    if (q.empty()) return true;
    q = toLowerUtf8(q);
    for (int i = 0; i < kNCols; ++i) if (toLowerUtf8(c[i]).find(q) != std::string::npos) return true;
    return false;
}

void Fill() {
    ListView_DeleteAllItems(A.list);
    A.view.clear();
    for (size_t i = 0; i < A.items.size(); ++i) {
        const auto& it = A.items[i];
        if (!PassBase(it)) continue;
        std::string c[kNCols];
        Cells(it, c);
        int cs = A.cat ? TabCtrl_GetCurSel(A.cat) : 0;
        if (cs > 0 && CatOf(it) != cs) continue;
        if (!PassQuick(c)) continue;
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
    int nflag = 0;
    for (auto& i : A.items) if (!i.flag.empty()) ++nflag;
    SetWindowTextW(A.info, W(fmt("Записей: %zu, показано: %zu, с пометками: %d.  Розовый — подозрительная запись, жёлтый — файл не найден, серый — отключено.  Отключение обратимо.", A.items.size(), A.view.size(), nflag)).c_str());
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

std::wstring UrlEncode(const std::string& u8) {
    std::wstring o;
    for (unsigned char ch : u8) {
        if (isalnum(ch) || ch == '-' || ch == '.' || ch == '_') o += (wchar_t)ch;
        else o += W(fmt("%%%02X", ch));
    }
    return o;
}

std::string BaseName(const std::string& p) {
    size_t k = p.find_last_of("\\/");
    return k == std::string::npos ? p : p.substr(k + 1);
}

// Entry commands (the same set the Autoruns entry menu has): jump to, search online, properties, copy, delete.
void JumpTo(const AutoItem& it) {
    if (!it.file.empty() && it.fileExists) {
        std::wstring a = L"/select,\"" + W(it.file) + L"\"";
        ShellExecuteW(A.wnd, L"open", L"explorer.exe", a.c_str(), nullptr, SW_SHOWNORMAL);
    } else {   // a registry entry: put its path on the clipboard and open the offline registry editor
        ClipText(A.wnd, W(it.location));
        MessageBoxW(A.wnd, (TrW(L"Путь записи скопирован в буфер обмена:\n") + W(it.location) + TrW(L"\n\nВ окне реестра загрузите кусты и найдите запись через поиск.")).c_str(),
                    TrW(L"Перейти к записи").c_str(), MB_OK | MB_ICONINFORMATION);
        OpenRegistryEditor();
    }
}

void SearchOnline(const AutoItem& it) {
    std::string q = !it.file.empty() ? BaseName(it.file) : it.name;
    std::wstring url = L"https://www.google.com/search?q=" + UrlEncode(q);
    ShellExecuteW(A.wnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void FileProps(const AutoItem& it) {
    if (it.file.empty() || !it.fileExists) { MessageBoxW(A.wnd, TrW(L"У записи нет существующего файла.").c_str(), TrW(L"Свойства").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    std::wstring f = W(it.file);
    SHELLEXECUTEINFOW e{sizeof e};
    e.fMask = SEE_MASK_INVOKEIDLIST;
    e.hwnd = A.wnd;
    e.lpVerb = L"properties";
    e.lpFile = f.c_str();
    e.nShow = SW_SHOW;
    ShellExecuteExW(&e);
}

void CopyRow() { int si = ListView_GetNextItem(A.list, -1, LVNI_SELECTED); if (si >= 0) ClipText(A.wnd, ListRowText(A.list, si)); }

// Right-click on an entry: Autoruns-style menu. Outside the table: nothing.
bool ListContextMenu(HWND src, LPARAM lp) {
    if (src != A.list) return false;
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    if (lp == (LPARAM)-1) { RECT r; GetWindowRect(A.list, &r); pt = {r.left + S(20), r.top + S(20)}; }
    else {
        POINT c = pt;
        ScreenToClient(A.list, &c);
        LVHITTESTINFO hi{};
        hi.pt = c;
        int row = ListView_HitTest(A.list, &hi);
        if (row >= 0) ListView_SetItemState(A.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    const AutoItem* it = Sel();
    if (!it) return true;   // empty area of the table: nothing to act on
    bool off = it->state == "Отключено";
    enum { M_TOGGLE = 1, M_EDIT, M_DEL, M_JUMP, M_WEB, M_PROPS, M_COPY, M_ALL };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, M_TOGGLE, TrW(off ? L"Включить запись…" : L"Отключить запись…").c_str());
    AppendMenuW(m, MF_STRING, M_EDIT, TrW(L"Изменить команду…").c_str());
    AppendMenuW(m, MF_STRING, M_DEL, TrW(L"Удалить\tDel").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, M_JUMP, TrW(L"Перейти к записи…\tCtrl+J").c_str());
    AppendMenuW(m, MF_STRING, M_WEB, TrW(L"Поиск в интернете…").c_str());
    AppendMenuW(m, MF_STRING, M_PROPS, TrW(L"Свойства файла…").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, M_COPY, TrW(L"Копировать\tCtrl+C").c_str());
    AppendMenuW(m, MF_STRING, M_ALL, TrW(L"Копировать всю таблицу").c_str());
    SetForegroundWindow(A.wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, A.wnd, nullptr);
    DestroyMenu(m);
    AutoItem cp = *it;
    switch (c) {
        case M_TOGGLE: DoOne(off ? 1 : 0); break;
        case M_EDIT: DoOne(3); break;
        case M_DEL: DoOne(2); break;
        case M_JUMP: JumpTo(cp); break;
        case M_WEB: SearchOnline(cp); break;
        case M_PROPS: FileProps(cp); break;
        case M_COPY: CopyRow(); break;
        case M_ALL: { std::wstring all; for (int i = 0, n = ListView_GetItemCount(A.list); i < n; ++i) all += ListRowText(A.list, i) + L"\r\n"; ClipText(A.wnd, all); break; }
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
    Place(A.cat, m, y, W_ - 2 * m, S(30));
    y += S(34);
    Place(GetDlgItem(A.wnd, ID_QFLBL), m, y + S(4), S(70), S(20));
    Place(A.qf, m + S(75), y, std::min(S(420), W_ - 2 * m - S(75)), S(26));
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
                case ID_TARGET: if (HIWORD(wp) == CBN_SELCHANGE) Scan(); break;   // another Windows chosen -> its own data at once
                case ID_HIDESYS: case ID_ONLYFLAG: Fill(); break;
                case ID_QF: if (HIWORD(wp) == EN_CHANGE) Fill(); break;
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
            else if (nh->hwndFrom == A.cat && nh->code == TCN_SELCHANGE) Fill();
            else if (nh->hwndFrom == A.list && nh->code == LVN_KEYDOWN) {
                WORD k = ((NMLVKEYDOWN*)lp)->wVKey;
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (k == VK_DELETE) DoOne(2);
                else if (k == VK_F5) Scan();
                else if (ctrl && k == 'C') CopyRow();
                else if (ctrl && k == 'J') { const AutoItem* it = Sel(); if (it) { AutoItem cp = *it; JumpTo(cp); } }
                else if (ctrl && k == 'F') SetFocus(A.qf);
            } else if (nh->hwndFrom == A.list && nh->code == NM_CUSTOMDRAW) {   // row colours as in Autoruns
                auto* cd = (NMLVCUSTOMDRAW*)lp;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    size_t r = (size_t)cd->nmcd.dwItemSpec;
                    if (r < A.view.size() && !(ListView_GetItemState(A.list, (int)r, LVIS_SELECTED) & LVIS_SELECTED)) {
                        const AutoItem& it = A.items[(size_t)A.view[r]];
                        if (!it.file.empty() && !it.fileExists) cd->clrTextBk = RGB(255, 255, 170);   // file not found: yellow
                        else if (!it.flag.empty()) cd->clrTextBk = RGB(255, 205, 205);               // suspicious: pink
                        if (it.state == "Отключено") cd->clrText = RGB(125, 125, 125);                // disabled: grey
                    }
                    return CDRF_NEWFONT;
                }
                return CDRF_DODEFAULT;
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
    A.wnd = CreateWindowExW(0, L"WRAutoruns", L"Автозагрузка и службы текущей и офлайн-Windows (Autoruns + msconfig)", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1200), S(720),
                            g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = A.wnd;
    Mk(h, L"STATIC", L"Целевая ОС:", SS_LEFT, 6900);
    A.combo = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_TARGET);
    A.targets = FillTargetCombo(A.combo, false);
    Mk(h, L"BUTTON", L"Сканировать", BS_PUSHBUTTON | WS_TABSTOP, ID_SCAN);
    A.cat = Mk(h, WC_TABCONTROLW, L"", WS_TABSTOP, ID_CATTAB);
    for (int i = 0; i < 6; ++i) { TCITEMW ti{}; ti.mask = TCIF_TEXT; ti.pszText = (LPWSTR)kCats[i]; TabCtrl_InsertItem(A.cat, i, &ti); }
    Mk(h, L"STATIC", L"Фильтр:", SS_LEFT, ID_QFLBL);
    A.qf = Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_QF, WS_EX_CLIENTEDGE);
    SendMessageW(A.qf, EM_SETCUEBANNER, TRUE, (LPARAM)TrW(L"Текст в любом столбце (Ctrl+F)").c_str());
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
