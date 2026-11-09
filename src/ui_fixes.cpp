// Repair wizard window (after-infection fixes).
#include "ui_tools_common.h"
#include "modules/fixes.h"

namespace wr {
namespace ui {

namespace {

enum { ID_TARGET = 7001, ID_LIST, ID_SAFE, ID_NONE, ID_RUN, ID_CLOSE, ID_DESC };

struct State {
    HWND wnd = nullptr, combo, list, desc, bSafe, bNone, bRun, bClose;
    std::vector<Target> targets;
};
State F;

void ShowDesc() {
    int s = ListView_GetNextItem(F.list, -1, LVNI_SELECTED);
    const auto& all = FixList();
    if (s < 0 || s >= (int)all.size()) { SetWindowTextW(F.desc, L"Отметьте нужные исправления галочками. Перед каждым изменением реестра создаётся копия ветки/куста (Data\\Backups), все действия пишутся в журнал."); return; }
    SetWindowTextW(F.desc, W(fmt("%s\r\n\r\n%s\r\n\r\nРиск: %s. Применимо: %s%s.", all[(size_t)s].title, all[(size_t)s].desc, RiskText(all[(size_t)s].risk),
                                 all[(size_t)s].offline ? "офлайн-ОС (WinPE)" : "", all[(size_t)s].online ? (all[(size_t)s].offline ? " и работающая Windows" : "работающая Windows") : "")).c_str());
}

void Run() {
    if (UiRunning()) { MessageBoxW(F.wnd, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
    int t = (int)SendMessageW(F.combo, CB_GETCURSEL, 0, 0);
    if (t < 0 || t >= (int)F.targets.size()) { MessageBoxW(F.wnd, L"Выберите целевую ОС.", L"Мастер", MB_OK | MB_ICONINFORMATION); return; }
    const auto& all = FixList();
    std::vector<int> ids;
    std::wstring list;
    bool danger = false;
    for (size_t i = 0; i < all.size(); ++i)
        if (ListView_GetCheckState(F.list, (int)i)) { ids.push_back(all[i].id); list += L"  • " + W(all[i].title) + L"\n"; if (all[i].risk == Risk::Danger) danger = true; }
    if (ids.empty()) { MessageBoxW(F.wnd, L"Не отмечено ни одного исправления.", L"Мастер", MB_OK | MB_ICONINFORMATION); return; }
    Target tg = F.targets[(size_t)t];
    std::wstring msg = L"Целевая ОС: " + W(tg.label) + L"\n\nБудут выполнены исправления:\n" + list + L"\nПеред изменениями сохраняются копии кустов и веток реестра. Продолжить?";
    if (MessageBoxW(F.wnd, msg.c_str(), L"Подтверждение исправлений", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    if (danger && MessageBoxW(F.wnd, L"Среди выбранных есть ОПАСНЫЕ исправления (отложенные операции / замена кустов реестра).\nУбедитесь, что есть копия диска или образ.\n\nПодтвердите повторно.",
                              L"Повторное подтверждение", MB_YESNO | MB_ICONSTOP | MB_DEFBUTTON2) != IDYES) return;
    UiStartJob("fixes", "Мастер исправлений", [ids](Ctx& c) { c.opt.fixIds = ids; ModFixes(c); }, &tg);
}

void Layout() {
    RECT rc;
    GetClientRect(F.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8), y = m;
    Place(GetDlgItem(F.wnd, 7900), m, y + S(3), S(100), S(20));
    Place(F.combo, m + S(105), y, W_ - 2 * m - S(105), S(300));
    y += S(34);
    int descH = S(120), btnH = S(32);
    Place(F.list, m, y, W_ - 2 * m, H - y - descH - btnH - 3 * m);
    y = H - descH - btnH - 2 * m;
    Place(F.desc, m, y, W_ - 2 * m, descH);
    y += descH + m;
    Place(F.bSafe, m, y, S(190), btnH);
    Place(F.bNone, m + S(198), y, S(150), btnH);
    Place(F.bRun, W_ - m - S(330), y, S(240), btnH);
    Place(F.bClose, W_ - m - S(82), y, S(82), btnH);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (F.bClose) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(820); mm->ptMinTrackSize.y = S(560); return 0; }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_SAFE: { const auto& all = FixList(); for (size_t i = 0; i < all.size(); ++i) ListView_SetCheckState(F.list, (int)i, all[i].risk == Risk::Safe); break; }
                case ID_NONE: { const auto& all = FixList(); for (size_t i = 0; i < all.size(); ++i) ListView_SetCheckState(F.list, (int)i, FALSE); break; }
                case ID_RUN: Run(); break;
                case ID_CLOSE: DestroyWindow(h); break;
            }
            return 0;
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->hwndFrom == F.list && (nh->code == LVN_ITEMCHANGED)) ShowDesc();
            return 0;
        }
        case WM_DESTROY: F.wnd = nullptr; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void OpenFixWizardImpl() {
    if (F.wnd) { ShowWindow(F.wnd, SW_RESTORE); SetForegroundWindow(F.wnd); return; }
    RegisterOnce(L"WRFixes", Proc);
    F = State();
    F.wnd = CreateWindowExW(0, L"WRFixes", L"Мастер исправлений после заражений — онлайн и офлайн", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1000), S(700),
                            g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = F.wnd;
    Mk(h, L"STATIC", L"Целевая ОС:", SS_LEFT, 7900);
    F.combo = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_TARGET);
    F.targets = FillTargetCombo(F.combo, false);
    F.list = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(F.list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_GRIDLINES);
    const wchar_t* cols[] = {L"Исправление", L"Риск", L"Применимо к"};
    int cw[] = {S(560), S(110), S(230)};
    for (int i = 0; i < 3; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = cw[i]; ListView_InsertColumn(F.list, i, &c); }
    const auto& all = FixList();
    for (size_t i = 0; i < all.size(); ++i) {
        std::wstring t = W(all[i].title);
        LVITEMW li{};
        li.mask = LVIF_TEXT;
        li.iItem = (int)i;
        li.pszText = (LPWSTR)t.c_str();
        ListView_InsertItem(F.list, &li);
        ListView_SetItemText(F.list, (int)i, 1, (LPWSTR)(all[i].risk == Risk::Safe ? L"Безопасно" : all[i].risk == Risk::Changing ? L"Изменяет" : L"ОПАСНО"));
        ListView_SetItemText(F.list, (int)i, 2, (LPWSTR)(all[i].online && all[i].offline ? L"онлайн и офлайн" : all[i].offline ? L"офлайн (WinPE)" : L"работающая Windows"));
    }
    F.desc = Mk(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, ID_DESC, WS_EX_CLIENTEDGE);
    F.bSafe = Mk(h, L"BUTTON", L"Отметить безопасные", BS_PUSHBUTTON | WS_TABSTOP, ID_SAFE);
    F.bNone = Mk(h, L"BUTTON", L"Снять все", BS_PUSHBUTTON | WS_TABSTOP, ID_NONE);
    F.bRun = Mk(h, L"BUTTON", L"Выполнить отмеченное…", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_RUN);
    F.bClose = Mk(h, L"BUTTON", L"Закрыть", BS_PUSHBUTTON | WS_TABSTOP, ID_CLOSE);
    Layout();
    ShowDesc();
    ShowWindow(h, SW_SHOW);
}

}  // namespace ui
}  // namespace wr
