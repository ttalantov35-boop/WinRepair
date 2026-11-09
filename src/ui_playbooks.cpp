// Playbooks window: list repair scenarios (*.wrp), show the plan, run after confirmation.
#include "ui_tools_common.h"
#include "modules/playbook.h"

#include <shellapi.h>

namespace wr {
namespace ui {

namespace {

enum { ID_TARGET = 11001, ID_LIST, ID_DESC, ID_RUN, ID_RELOAD, ID_FOLDER, ID_CLOSE };

struct P {
    HWND wnd = nullptr, target, list, desc, bRun, bReload, bFolder, bClose;
    std::vector<Target> targets;
    std::vector<const Playbook*> shown;
};
P Z;

const wchar_t* RiskW(const std::string& r) { return r == "danger" ? L"ОПАСНО" : r == "safe" ? L"Безопасно" : L"Изменяет"; }

Target CurTarget() {
    int t = (int)SendMessageW(Z.target, CB_GETCURSEL, 0, 0);
    return (t >= 0 && t < (int)Z.targets.size()) ? Z.targets[(size_t)t] : Target();
}

const Playbook* Sel() {
    int s = ListView_GetNextItem(Z.list, -1, LVNI_SELECTED);
    return (s >= 0 && s < (int)Z.shown.size()) ? Z.shown[(size_t)s] : nullptr;
}

void ShowDesc() {
    const Playbook* pb = Sel();
    std::string t;
    if (!pb) {
        t = fmt("Сценариев ремонта: %zu. Сценарии встроены в программу; дополнительные *.wrp читаются из Playbooks рядом с программой (и на уровень выше) и из %s — они заменяют встроенные с тем же id.\r\nФормат описан в docs\\PLAYBOOKS.md.", Z.shown.size(), PlaybookDir().c_str());
        if (!PlaybookErrors().empty()) { t += "\r\n\r\nОШИБКИ РАЗБОРА:\r\n"; for (auto& e : PlaybookErrors()) t += "  • " + e + "\r\n"; }
    } else {
        std::string why = PlaybookApplicable(*pb, CurTarget());
        t = pb->title + "\r\n\r\n" + pb->description + "\r\n\r\n" + (why.empty() ? "Применим к выбранной цели." : "НЕДОСТУПЕН: " + why) + "\r\n\r\nПлан:\r\n" + PlaybookPlan(*pb) + "\r\nФайл: " + pb->file;
    }
    SetWindowTextW(Z.desc, W(t).c_str());
    EnableWindow(Z.bRun, pb != nullptr && !UiRunning());
}

void Fill() {
    ListView_DeleteAllItems(Z.list);
    Z.shown = PlaybooksOfKind("repair");
    for (size_t i = 0; i < Z.shown.size(); ++i) {
        std::wstring t = W(Z.shown[i]->title);
        LVITEMW li{};
        li.mask = LVIF_TEXT;
        li.iItem = (int)i;
        li.pszText = (LPWSTR)t.c_str();
        ListView_InsertItem(Z.list, &li);
        ListView_SetItemText(Z.list, (int)i, 1, (LPWSTR)RiskW(Z.shown[i]->risk));
        ListView_SetItemText(Z.list, (int)i, 2, (LPWSTR)W(Z.shown[i]->needs).c_str());
    }
    if (!Z.shown.empty()) ListView_SetItemState(Z.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ShowDesc();
}

void Start(HWND owner, const Playbook& pb, const Target& t) {
    if (UiRunning()) { MessageBoxW(owner, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
    std::string why = PlaybookApplicable(pb, t);
    if (!why.empty()) { MessageBoxW(owner, W(why).c_str(), L"Сценарий недоступен", MB_OK | MB_ICONWARNING); return; }
    std::wstring msg = L"Сценарий: " + W(pb.title) + L"\nЦелевая ОС: " + (t.valid ? W(t.label) : std::wstring(L"—")) + L"\n\nПлан:\n" + W(PlaybookPlan(pb)) +
                       L"\nКаждый шаг, изменяющий систему, будет подтверждаться отдельно. Запускайте только сценарии из доверенных источников. Начать?";
    if (MessageBoxW(owner, msg.c_str(), L"Подтверждение сценария", MB_YESNO | (pb.risk == "safe" ? MB_ICONQUESTION : MB_ICONWARNING) | MB_DEFBUTTON2) != IDYES) return;
    std::string id = pb.id;
    Target tg = t;
    UiStartJob("playbook", "Сценарий: " + pb.title, [id](Ctx& c) { c.opt.playbookId = id; ModPlaybook(c); }, tg.valid ? &tg : nullptr);
}

void Layout() {
    RECT rc;
    GetClientRect(Z.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8), y = m;
    Place(GetDlgItem(Z.wnd, 11900), m, y + S(3), S(100), S(20));
    Place(Z.target, m + S(105), y, W_ - 2 * m - S(105), S(300));
    y += S(34);
    int bh = S(32), listH = (H - y - bh - 3 * m) * 38 / 100;
    Place(Z.list, m, y, W_ - 2 * m, listH);
    y += listH + m;
    Place(Z.desc, m, y, W_ - 2 * m, H - y - bh - 2 * m);
    y = H - bh - m;
    Place(Z.bRun, m, y, S(210), bh);
    Place(Z.bReload, m + S(218), y, S(190), bh);
    Place(Z.bFolder, m + S(416), y, S(210), bh);
    Place(Z.bClose, W_ - m - S(100), y, S(100), bh);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (Z.bClose) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(860); mm->ptMinTrackSize.y = S(600); return 0; }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == ID_TARGET && code == CBN_SELCHANGE) ShowDesc();
            else if (id == ID_RUN) { const Playbook* pb = Sel(); if (pb) Start(h, *pb, CurTarget()); }
            else if (id == ID_RELOAD) { ReloadPlaybooks(); Fill(); }
            else if (id == ID_FOLDER) {
                MakeDirs(PlaybookDir());
                if ((INT_PTR)ShellExecuteW(h, L"open", W(PlaybookDir()).c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32) Logger::I().log(Lvl::Info, "Папка пользовательских сценариев: " + PlaybookDir());
            } else if (id == ID_CLOSE) DestroyWindow(h);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->hwndFrom == Z.list && nh->code == LVN_ITEMCHANGED) ShowDesc();
            return 0;
        }
        case WM_DESTROY: Z.wnd = nullptr; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void OpenPlaybooksImpl() {
    if (Z.wnd) { ShowWindow(Z.wnd, SW_RESTORE); SetForegroundWindow(Z.wnd); return; }
    RegisterOnce(L"WRPlaybooks", Proc);
    Z = P();
    Z.wnd = CreateWindowExW(0, L"WRPlaybooks", L"Сценарии ремонта (playbooks)", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1000), S(740), g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = Z.wnd;
    Mk(h, L"STATIC", L"Целевая ОС:", SS_LEFT, 11900);
    Z.target = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_TARGET);
    Z.targets = FillTargetCombo(Z.target, false);
    Z.list = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(Z.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const wchar_t* cols[] = {L"Сценарий", L"Риск", L"Условие"};
    int cw[] = {S(640), S(110), S(120)};
    for (int i = 0; i < 3; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = cw[i]; ListView_InsertColumn(Z.list, i, &c); }
    Z.desc = Mk(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, ID_DESC, WS_EX_CLIENTEDGE);
    SendMessageW(Z.desc, WM_SETFONT, (WPARAM)g_ui.mono, TRUE);
    Z.bRun = Mk(h, L"BUTTON", L"Выполнить сценарий…", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_RUN);
    Z.bReload = Mk(h, L"BUTTON", L"Перезагрузить список", BS_PUSHBUTTON | WS_TABSTOP, ID_RELOAD);
    Z.bFolder = Mk(h, L"BUTTON", L"Папка своих сценариев", BS_PUSHBUTTON | WS_TABSTOP, ID_FOLDER);
    Z.bClose = Mk(h, L"BUTTON", L"Закрыть", BS_PUSHBUTTON | WS_TABSTOP, ID_CLOSE);
    Layout();
    Fill();
    ShowWindow(h, SW_SHOW);
}

void PlaybooksJobDone() { if (Z.wnd) ShowDesc(); }

}  // namespace ui

void UiRunPlaybook(const std::string& id) {
    const Playbook* pb = FindPlaybook(id);
    if (!pb) { MessageBoxW(g_ui.main, W("Сценарий не найден: " + id).c_str(), L"Сценарии", MB_OK | MB_ICONWARNING); return; }
    ui::Start(g_ui.main, *pb, UiCurrentTarget());
}

}  // namespace wr
