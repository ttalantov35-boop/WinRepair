// "Что случилось?" window: choose a symptom -> automatic probes -> ranked findings -> run the recommended action.
#include "ui_tools_common.h"
#include "modules/triage.h"

namespace wr {
namespace ui {

namespace {

enum { ID_SYMPTOM = 10001, ID_TARGET, ID_RUN, ID_LIST, ID_DETAIL, ID_ACT, ID_FULL, ID_CLOSE };

struct T {
    HWND wnd = nullptr, symptom, target, list, detail, bRun, bAct, bFull, bClose, hint;
    std::vector<Target> targets;
    Target tgt;
    std::vector<Finding> shown;
};
T X;

const wchar_t* SevText(Sev s) { return s == Sev::Bad ? L"ПРОБЛЕМА" : s == Sev::Warn ? L"Внимание" : L"ок"; }

void Fill() {
    ListView_DeleteAllItems(X.list);
    X.shown = TriageFindings();
    for (size_t i = 0; i < X.shown.size(); ++i) {
        const auto& f = X.shown[i];
        LVITEMW li{};
        li.mask = LVIF_TEXT;
        li.iItem = (int)i;
        li.pszText = (LPWSTR)SevText(f.sev);
        ListView_InsertItem(X.list, &li);
        ListView_SetItemText(X.list, (int)i, 1, (LPWSTR)W(f.title).c_str());
        ListView_SetItemText(X.list, (int)i, 2, (LPWSTR)W(f.act.label).c_str());
    }
    if (!X.shown.empty()) ListView_SetItemState(X.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    int bad = 0, warn = 0;
    for (auto& f : X.shown) { if (f.sev == Sev::Bad) ++bad; else if (f.sev == Sev::Warn) ++warn; }
    SetWindowTextW(X.hint, X.shown.empty() ? L"Выберите симптом и целевую ОС, затем «Диагностировать». Проверки только читают данные и ничего не меняют."
                                           : W(fmt("Проблем: %d, замечаний: %d. Сначала разберите находки сверху (отсортированы по важности).", bad, warn)).c_str());
}

const Finding* Sel() {
    int s = ListView_GetNextItem(X.list, -1, LVNI_SELECTED);
    return (s >= 0 && s < (int)X.shown.size()) ? &X.shown[(size_t)s] : nullptr;
}

void ShowDetail() {
    const Finding* f = Sel();
    if (!f) { SetWindowTextW(X.detail, L""); EnableWindow(X.bAct, FALSE); return; }
    std::string t = f->title + "\r\n\r\n" + f->detail + "\r\n\r\n";
    t += f->act.label.empty() ? "Рекомендованного действия нет (информационная находка)." : "Рекомендуемое действие: " + f->act.label;
    SetWindowTextW(X.detail, W(t).c_str());
    EnableWindow(X.bAct, f->act.k != TAct::None && !UiRunning());
}

void Diagnose() {
    if (UiRunning()) { MessageBoxW(X.wnd, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
    int t = (int)SendMessageW(X.target, CB_GETCURSEL, 0, 0);
    int s = (int)SendMessageW(X.symptom, CB_GETCURSEL, 0, 0);
    if (s < 0) return;
    Target tg;
    if (t >= 0 && t < (int)X.targets.size()) tg = X.targets[(size_t)t];
    X.tgt = tg;
    UiSelectTarget(tg);
    UiStartJob("triage", std::string("Что случилось? — ") + SymptomName(s), [s](Ctx& c) { c.opt.symptom = s; ModTriage(c); }, tg.valid ? &tg : nullptr);
}

void RunAction() {
    const Finding* f = Sel();
    if (!f || f->act.k == TAct::None) return;
    TAct a = f->act;
    UiSelectTarget(X.tgt);
    switch (a.k) {
        case TAct::Catalog: UiRunCatalog(a.no); break;
        case TAct::Module: UiRunModule(a.name); break;
        case TAct::Playbook: UiRunPlaybook(a.name); break;
        case TAct::Window:
            if (a.name == "storage") OpenStorageAccess();
            else if (a.name == "bcd") OpenBcdManager();
            else if (a.name == "autoruns") OpenAutoruns();
            else if (a.name == "registry") OpenRegistryEditor();
            else if (a.name == "winre") OpenWinReManager();
            else OpenFixWizard();
            break;
        case TAct::Fixes: {
            if (UiRunning()) { MessageBoxW(X.wnd, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
            std::wstring list;
            bool danger = false;
            for (int id : a.ids)
                for (const auto& d : FixList())
                    if (d.id == id) { list += L"  • " + W(d.title) + L"\n"; if (d.risk == Risk::Danger) danger = true; }
            std::wstring msg = L"Целевая ОС: " + W(X.tgt.label) + L"\n\nБудут выполнены исправления:\n" + list + L"\nПеред изменениями сохраняются копии кустов и веток реестра. Продолжить?";
            if (MessageBoxW(X.wnd, msg.c_str(), L"Подтверждение исправлений", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
            if (danger && MessageBoxW(X.wnd, L"Среди исправлений есть ОПАСНЫЕ. Убедитесь, что есть копия диска.\n\nПодтвердите повторно.", L"Повторное подтверждение", MB_YESNO | MB_ICONSTOP | MB_DEFBUTTON2) != IDYES) return;
            Target tg = X.tgt;
            std::vector<int> ids = a.ids;
            UiStartJob("fixes", "Исправления из мастера «Что случилось?»", [ids](Ctx& c) { c.opt.fixIds = ids; ModFixes(c); }, &tg);
            break;
        }
        default: break;
    }
}

void Layout() {
    RECT rc;
    GetClientRect(X.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8), y = m;
    Place(GetDlgItem(X.wnd, 10900), m, y + S(3), S(110), S(20));
    Place(X.symptom, m + S(115), y, W_ - 2 * m - S(115), S(300));
    y += S(32);
    Place(GetDlgItem(X.wnd, 10901), m, y + S(3), S(110), S(20));
    Place(X.target, m + S(115), y, W_ - 2 * m - S(115) - S(190), S(300));
    Place(X.bRun, W_ - m - S(180), y - 1, S(180), S(28));
    y += S(34);
    Place(X.hint, m, y, W_ - 2 * m, S(20));
    y += S(24);
    int bh = S(32), detH = S(130);
    int listH = H - y - detH - bh - 4 * m;
    Place(X.list, m, y, W_ - 2 * m, std::max(S(80), listH));
    y += std::max(S(80), listH) + m;
    Place(X.detail, m, y, W_ - 2 * m, detH);
    y += detH + m;
    Place(X.bAct, m, y, S(300), bh);
    Place(X.bFull, m + S(308), y, S(220), bh);
    Place(X.bClose, W_ - m - S(100), y, S(100), bh);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (X.bClose) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(900); mm->ptMinTrackSize.y = S(620); return 0; }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_RUN: Diagnose(); break;
                case ID_ACT: RunAction(); break;
                case ID_FULL: UiRunModule("full"); break;
                case ID_CLOSE: DestroyWindow(h); break;
            }
            return 0;
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->hwndFrom == X.list) {
                if (nh->code == LVN_ITEMCHANGED) ShowDetail();
                else if (nh->code == NM_DBLCLK) RunAction();
            }
            return 0;
        }
        case WM_DESTROY: X.wnd = nullptr; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void OpenTriageImpl() {
    if (X.wnd) { ShowWindow(X.wnd, SW_RESTORE); SetForegroundWindow(X.wnd); return; }
    RegisterOnce(L"WRTriage", Proc);
    X = T();
    X.wnd = CreateWindowExW(0, L"WRTriage", L"Что случилось? — диагностика по симптому", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1080), S(740),
                            g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = X.wnd;
    Mk(h, L"STATIC", L"Симптом:", SS_LEFT, 10900);
    X.symptom = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_SYMPTOM);
    for (int i = 0; i < SymptomCount(); ++i) SendMessageW(X.symptom, CB_ADDSTRING, 0, (LPARAM)W(SymptomName(i)).c_str());
    SendMessageW(X.symptom, CB_SETCURSEL, 0, 0);
    Mk(h, L"STATIC", L"Целевая ОС:", SS_LEFT, 10901);
    X.target = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_TARGET);
    X.targets = FillTargetCombo(X.target, false);
    X.bRun = Mk(h, L"BUTTON", L"Диагностировать", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_RUN);
    X.hint = Mk(h, L"STATIC", L"", SS_LEFT, 10902);
    X.list = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(X.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const wchar_t* cols[] = {L"Важность", L"Находка", L"Рекомендуемое действие"};
    int cw[] = {S(100), S(560), S(340)};
    for (int i = 0; i < 3; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = cw[i]; ListView_InsertColumn(X.list, i, &c); }
    X.detail = Mk(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, ID_DETAIL, WS_EX_CLIENTEDGE);
    X.bAct = Mk(h, L"BUTTON", L"Выполнить рекомендованное действие…", BS_PUSHBUTTON | WS_TABSTOP, ID_ACT);
    X.bFull = Mk(h, L"BUTTON", L"Полная диагностика + отчёт", BS_PUSHBUTTON | WS_TABSTOP, ID_FULL);
    X.bClose = Mk(h, L"BUTTON", L"Закрыть", BS_PUSHBUTTON | WS_TABSTOP, ID_CLOSE);
    EnableWindow(X.bAct, FALSE);
    Layout();
    Fill();
    ShowWindow(h, SW_SHOW);
}

void TriageJobDone() { if (X.wnd) { Fill(); ShowDetail(); } }

}  // namespace ui
}  // namespace wr
