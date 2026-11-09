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
};
State A;

void Fill() {
    ListView_DeleteAllItems(A.list);
    A.view.clear();
    bool hide = SendMessageW(A.hideSys, BM_GETCHECK, 0, 0) == BST_CHECKED, only = SendMessageW(A.onlyFlag, BM_GETCHECK, 0, 0) == BST_CHECKED;
    for (size_t i = 0; i < A.items.size(); ++i) {
        const auto& it = A.items[i];
        if (hide && (it.op == AOp::Service) && !it.thirdParty && it.flag.empty()) continue;
        if (only && it.flag.empty() && !(it.op == AOp::Service && it.thirdParty)) continue;
        int row = (int)A.view.size();
        A.view.push_back((int)i);
        std::wstring st = W(it.state);
        LVITEMW li{};
        li.mask = LVIF_TEXT;
        li.iItem = row;
        li.pszText = (LPWSTR)st.c_str();
        ListView_InsertItem(A.list, &li);
        const std::string cols[] = {it.kind, it.name, it.command, it.fileExists ? "да" : (it.file.empty() ? "" : "НЕТ"), it.flag, it.location};
        for (int c = 0; c < 6; ++c) ListView_SetItemText(A.list, row, c + 1, (LPWSTR)W(cols[c]).c_str());
    }
    int nflag = 0;
    for (auto& i : A.items) if (!i.flag.empty()) ++nflag;
    SetWindowTextW(A.info, W(fmt("Записей: %zu, показано: %zu, с пометками: %d.  Отключение — обратимо (StartupApproved / Start=4 с резервным значением).", A.items.size(), A.view.size(), nflag)).c_str());
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
    const wchar_t* cols[] = {L"Состояние", L"Тип", L"Имя", L"Команда / образ", L"Файл", L"Пометка", L"Расположение"};
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
