// Снятие пароля и разблокировка локальных учётных записей (вкладка «Инструменты ремонта»).
// Работает только для офлайн-Windows (из WinPE/WinRE): показывает учётные записи из SAM, снимает пароль
// (вход с пустым паролем), включает и разблокирует учётки. Новый пароль здесь не задаётся.
// Перед изменением куст SAM копируется автоматически; действия пишутся в журнал, пароль в журнал не попадает.
#include "ui_tools_common.h"
#include "modules/sampwd.h"

namespace wr {
namespace ui {

namespace {

enum { ID_TARGET = 7301, ID_LIST, ID_DESC, ID_CAVEAT, ID_LEGEND,
       ID_BLANKON, ID_BLANK, ID_ENABLE, ID_UNLOCK, ID_REFRESH, ID_CLOSE, ID_TLABEL };

struct State {
    HWND wnd = nullptr, combo, list, desc, caveat, legend, bBlankOn, bBlank, bEnable, bUnlock, bRefresh, bClose;
    std::vector<Target> targets;
    std::vector<SamAccount> accts;
    std::string err;
};
State P;

inline bool EN() { return CurrentLang() == Lang::EN; }

// Цвет строки: серый — Microsoft (офлайн-сброс недоступен), оранжевый — требует действия (отключена/заблокирована),
// синий — встроенный Администратор, иначе без цвета.
Tone ToneOf(const SamAccount& a) {
    if (a.microsoft) return Tone::Gray;
    if (a.disabled || a.locked) return Tone::Orange;
    if (a.builtinAdmin) return Tone::Blue;
    return Tone::None;
}

Target* CurTarget() {
    int t = (int)SendMessageW(P.combo, CB_GETCURSEL, 0, 0);
    if (t < 0 || t >= (int)P.targets.size()) return nullptr;
    return &P.targets[(size_t)t];
}
const SamAccount* CurAcct() {
    int s = ListView_GetNextItem(P.list, -1, LVNI_SELECTED);
    if (s < 0 || s >= (int)P.accts.size()) return nullptr;
    return &P.accts[(size_t)s];
}

std::string TypeText(const SamAccount& a) {
    std::string s = a.builtinAdmin ? (EN() ? "Administrator (built-in)" : "Администратор (встроенный)")
                   : a.guest       ? (EN() ? "Guest" : "Гость")
                                   : (EN() ? "User" : "Пользователь");
    if (a.microsoft) s += EN() ? " · Microsoft" : " · Microsoft";
    return s;
}
std::string StateText(const SamAccount& a) {
    std::string s = a.disabled ? (EN() ? "disabled" : "отключена") : (EN() ? "enabled" : "включена");
    if (a.locked) s += EN() ? ", locked" : ", заблокирована";
    return s;
}

void ShowDesc() {
    if (!P.err.empty()) { SetWindowTextW(P.desc, WRaw(P.err).c_str()); return; }
    if (!CurTarget()) {
        SetWindowTextW(P.desc, EN()
            ? L"Password tools work on an OFFLINE Windows. Boot this PC from a WinPE/WinRE flash drive with WinRepair, then pick the installed Windows above. The list of its local accounts will appear here."
            : L"Снятие пароля работает для ОФЛАЙН-Windows. Загрузите компьютер с флешки WinPE/WinRE с WinRepair, затем выберите установленную Windows сверху — здесь появится список её локальных учётных записей.");
        return;
    }
    const SamAccount* a = CurAcct();
    if (!a) {
        SetWindowTextW(P.desc, EN() ? L"Pick an account in the list. Then: \"Reset password\" lets you log in with an empty password; \"Enable\" / \"Unlock\" restore access to a disabled or locked account."
                                    : L"Выберите учётную запись в списке. Затем: «Снять пароль» — вход с пустым паролем; «Включить» / «Разблокировать» — вернуть доступ к отключённой или заблокированной записи.");
        return;
    }
    std::string t;
    if (EN()) {
        t = a->name + (a->fullName.empty() ? "" : " (" + a->fullName + ")") + "\r\n";
        t += "RID " + fmt("%u", a->rid) + " · " + TypeText(*a) + " · " + StateText(*a) + " · " + (a->hasPassword ? "has a password" : "no password") + "\r\n\r\n";
        if (a->microsoft)
            t += "This looks like a Microsoft account. Offline reset is unreliable for it: the real password is checked by Microsoft online, not stored in SAM. Recommended: reset it at account.live.com, or enable the built-in Administrator and use it instead.\r\n\r\n";
        t += "What happens:\r\n";
        t += "  - Reset password — the account logs in with an EMPTY password (leave the password box blank).\r\n";
        t += "  - Enable / Unlock — clears the \"disabled\" / \"locked\" flags.\r\n\r\n";
        t += "Important: resetting or changing a password makes these unreadable — EFS-encrypted files, saved passwords (Credential Manager, browsers, Wi-Fi). If the user remembers the old password, it is safer to log in and change it normally.";
    } else {
        t = a->name + (a->fullName.empty() ? "" : " (" + a->fullName + ")") + "\r\n";
        t += "RID " + fmt("%u", a->rid) + " · " + TypeText(*a) + " · " + StateText(*a) + " · " + (a->hasPassword ? "пароль есть" : "пароля нет") + "\r\n\r\n";
        if (a->microsoft)
            t += "Похоже на учётную запись Microsoft. Офлайн-сброс для неё ненадёжен: настоящий пароль проверяет Microsoft онлайн, в SAM его нет. Лучше: сбросить на account.live.com или включить встроенного Администратора и войти под ним.\r\n\r\n";
        t += "Что произойдёт:\r\n";
        t += "  • Снять пароль — вход в учётную запись с ПУСТЫМ паролем (поле пароля оставьте пустым).\r\n";
        t += "  • Включить / Разблокировать — снимает флаги «отключена» / «заблокирована».\r\n\r\n";
        t += "Важно: снятие или смена пароля делает недоступными зашифрованные EFS-файлы и сохранённые пароли (Диспетчер учётных данных, браузеры, Wi-Fi). Если пользователь помнит старый пароль — надёжнее войти и сменить его штатно.";
    }
    SetWindowTextW(P.desc, WRaw(t).c_str());
}

void Scan() {
    ListView_DeleteAllItems(P.list);
    P.accts.clear();
    P.err.clear();
    Target* t = CurTarget();
    if (t) {
        std::string err;
        if (!SamListAccounts(*t, P.accts, err)) P.err = err;
    }
    for (size_t i = 0; i < P.accts.size(); ++i) {
        const SamAccount& a = P.accts[i];
        std::wstring nm = WRaw(a.name);
        LVITEMW li{};
        li.mask = LVIF_TEXT;
        li.iItem = (int)i;
        li.pszText = (LPWSTR)nm.c_str();
        ListView_InsertItem(P.list, &li);
        std::wstring rid = WRaw(fmt("%u", a.rid)), ty = WRaw(TypeText(a)), st = WRaw(StateText(a));
        std::wstring pw = WRaw(a.hasPassword ? (EN() ? "yes" : "есть") : (EN() ? "no" : "нет"));
        ListView_SetItemText(P.list, (int)i, 1, (LPWSTR)rid.c_str());
        ListView_SetItemText(P.list, (int)i, 2, (LPWSTR)ty.c_str());
        ListView_SetItemText(P.list, (int)i, 3, (LPWSTR)st.c_str());
        ListView_SetItemText(P.list, (int)i, 4, (LPWSTR)pw.c_str());
    }
    if (!P.accts.empty()) ListView_SetItemState(P.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ShowDesc();
}

void Do(SamOp op) {
    if (UiRunning()) { MessageBoxW(P.wnd, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
    Target* t = CurTarget();
    if (!t) { MessageBoxW(P.wnd, L"Выберите целевую Windows (офлайн, из WinPE).", L"Снятие пароля", MB_OK | MB_ICONINFORMATION); return; }
    const SamAccount* a = CurAcct();
    if (!a) { MessageBoxW(P.wnd, L"Выберите учётную запись в списке.", L"Снятие пароля", MB_OK | MB_ICONINFORMATION); return; }

    bool blank = (op == SamOp::Blank || op == SamOp::BlankEnableUnlock);
    std::string what = op == SamOp::BlankEnableUnlock ? (EN() ? "reset the password, enable and unlock" : "снять пароль, включить и разблокировать")
                     : op == SamOp::Blank            ? (EN() ? "reset the password (login with empty password)" : "снять пароль (вход с пустым паролем)")
                     : op == SamOp::Enable           ? (EN() ? "enable the account" : "включить учётную запись")
                                                     : (EN() ? "unlock the account" : "разблокировать учётную запись");

    std::string msg = (EN() ? "Target: " : "Целевая Windows: ") + t->label + "\r\n"
                    + (EN() ? "Account: " : "Учётная запись: ") + a->name + "  (RID " + fmt("%u", a->rid) + ")\r\n\r\n"
                    + (EN() ? "Action: " : "Действие: ") + what + "\r\n\r\n";
    if (blank)
        msg += EN() ? "A copy of the SAM hive is saved first (undoable). The new password is empty — set a password in Windows after logging in. EFS files and saved passwords for this account will be lost.\r\n\r\n"
                    : "Сначала сохраняется копия куста SAM (обратимо). Новый пароль — пустой, задайте его в Windows после входа. Зашифрованные EFS-файлы и сохранённые пароли этой записи будут потеряны.\r\n\r\n";
    msg += EN() ? "Continue?" : "Продолжить?";

    if (MessageBoxW(P.wnd, WRaw(msg).c_str(), WRaw(EN() ? "Confirm" : "Подтверждение").c_str(), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;

    if (a->microsoft && blank) {
        std::string w = EN() ? "This looks like a Microsoft account — offline password reset is unreliable and may block login. Reset it online (account.live.com) or enable the built-in Administrator instead.\r\n\r\nReset anyway?"
                             : "Похоже на учётную запись Microsoft — офлайн-сброс ненадёжен и может заблокировать вход. Лучше сбросить онлайн (account.live.com) или включить встроенного Администратора.\r\n\r\nВсё равно снять пароль?";
        if (MessageBoxW(P.wnd, WRaw(w).c_str(), WRaw(EN() ? "Microsoft account" : "Учётная запись Microsoft").c_str(), MB_YESNO | MB_ICONSTOP | MB_DEFBUTTON2) != IDYES) return;
    }

    unsigned rid = a->rid;
    Target tg = *t;
    std::string title = (EN() ? "Password: " : "Пароль: ") + a->name;
    UiStartJob("sampwd", title, [rid, op](Ctx& c) {
        std::string e;
        if (!SamApply(c, rid, op, e)) c.error(e);
    }, &tg);
}

void Layout() {
    RECT rc;
    GetClientRect(P.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8), y = m, bh = S(32);
    Place(P.caveat, m, y, W_ - 2 * m, S(40));
    y += S(46);
    Place(GetDlgItem(P.wnd, ID_TLABEL), m, y + S(3), S(100), S(20));
    Place(P.combo, m + S(105), y, W_ - 2 * m - S(105), S(300));
    y += S(34);
    int descH = S(150);
    int listBottom = H - m - bh - S(8) - S(22) - descH - S(8);
    Place(P.list, m, y, W_ - 2 * m, listBottom - y);
    y = listBottom + S(4);
    Place(P.legend, m, y, W_ - 2 * m, S(18));
    y += S(22);
    Place(P.desc, m, y, W_ - 2 * m, descH);
    y = H - m - bh;
    Place(P.bBlankOn, m, y, S(260), bh);
    Place(P.bBlank, m + S(268), y, S(150), bh);
    Place(P.bEnable, m + S(424), y, S(150), bh);
    Place(P.bUnlock, m + S(580), y, S(160), bh);
    Place(P.bRefresh, W_ - m - S(200), y, S(96), bh);
    Place(P.bClose, W_ - m - S(98), y, S(98), bh);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (P.bClose) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(980); mm->ptMinTrackSize.y = S(620); return 0; }
        case WM_CTLCOLORSTATIC:
            if ((HWND)lp == P.caveat) {
                static HBRUSH yb = CreateSolidBrush(RGB(255, 247, 214));
                SetBkColor((HDC)wp, RGB(255, 247, 214));
                SetTextColor((HDC)wp, RGB(110, 70, 0));
                return (LRESULT)yb;
            }
            break;
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == ID_TARGET && code == CBN_SELCHANGE) Scan();
            else if (id == ID_BLANKON) Do(SamOp::BlankEnableUnlock);
            else if (id == ID_BLANK) Do(SamOp::Blank);
            else if (id == ID_ENABLE) Do(SamOp::Enable);
            else if (id == ID_UNLOCK) Do(SamOp::Unlock);
            else if (id == ID_REFRESH) { P.targets = FillTargetCombo(P.combo, true); Scan(); }
            else if (id == ID_CLOSE) DestroyWindow(h);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->hwndFrom == P.list) {
                if (nh->code == LVN_ITEMCHANGED) ShowDesc();
                else if (nh->code == NM_CUSTOMDRAW)
                    return ListToneDraw(lp, [](int i) { return i >= 0 && i < (int)P.accts.size() ? ToneOf(P.accts[(size_t)i]) : Tone::None; }, 3, true);
            }
            return 0;
        }
        case WM_DESTROY: P.wnd = nullptr; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void OpenPasswordToolImpl() {
    if (P.wnd) { ShowWindow(P.wnd, SW_RESTORE); SetForegroundWindow(P.wnd); return; }
    RegisterOnce(L"WRPwd", Proc);
    P = State();
    P.wnd = CreateWindowExW(0, L"WRPwd", L"Снятие пароля и разблокировка учётных записей (F1 — справка)", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, S(1120), S(760), g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = P.wnd;
    P.caveat = Mk(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, ID_CAVEAT);
    SetWindowTextW(P.caveat, WRaw(EN()
        ? "  Offline Windows only (from WinPE/WinRE). Reset password = log in with an empty password. Microsoft accounts cannot be reset offline."
        : "  Только для офлайн-Windows (из WinPE/WinRE). Снятие пароля = вход с пустым паролем. Учётные записи Microsoft офлайн не сбрасываются.").c_str());
    Mk(h, L"STATIC", L"Целевая ОС:", SS_LEFT, ID_TLABEL);
    P.combo = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_TARGET);
    P.targets = FillTargetCombo(P.combo, true);  // только офлайн-ОС
    P.list = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(P.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[] = {L"Имя", L"RID", L"Тип", L"Состояние", L"Пароль"};
    int cw[] = {S(220), S(70), S(280), S(220), S(90)};
    for (int i = 0; i < 5; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = cw[i]; ListView_InsertColumn(P.list, i, &c); }
    P.legend = MkLegend(h, ID_LEGEND, {{Tone::Orange, L"отключена / заблокирована"}, {Tone::Blue, L"встроенный администратор"}, {Tone::Gray, L"Microsoft — сброс недоступен"}});
    P.desc = Mk(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, ID_DESC, WS_EX_CLIENTEDGE);
    P.bBlankOn = MkBtn(h, L"Снять пароль и включить вход", ID_BLANKON, Tone::Orange);
    P.bBlank = MkBtn(h, L"Снять пароль", ID_BLANK, Tone::Orange);
    P.bEnable = MkBtn(h, L"Включить", ID_ENABLE, Tone::Orange);
    P.bUnlock = MkBtn(h, L"Разблокировать", ID_UNLOCK, Tone::Orange);
    P.bRefresh = MkBtn(h, L"Обновить", ID_REFRESH, Tone::Gray);
    P.bClose = MkBtn(h, L"Закрыть", ID_CLOSE, Tone::Gray);
    Layout();
    Scan();
    ShowWindow(h, SW_SHOW);
}

void PasswordJobDone() { if (P.wnd) Scan(); }

}  // namespace ui

void OpenPasswordTool() { ui::OpenPasswordToolImpl(); }

}  // namespace wr
