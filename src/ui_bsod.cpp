// Справочник кодов BSOD (вкладка «Драйверы и BSOD»): поиск по коду, имени и тексту, цвет — по группе причин,
// подробности выбранного кода и переход к проверкам (события BSOD, разбор дампов). Данные — core/bsod_catalog.h.
#include "ui_tools_common.h"
#include "core/bsod_catalog.h"

namespace wr {
namespace ui {

namespace {

enum { ID_FIND = 9501, ID_FREQ, ID_LIST, ID_DETAIL, ID_CAVEAT, ID_LEGEND, ID_DUMPS, ID_B_EVENTS, ID_B_DUMP, ID_B_COPY, ID_B_CLOSE };

struct State {
    HWND wnd = nullptr, find, freq, list, detail, caveat, legend, dumps, bEvents, bDump, bCopy, bClose;
    std::vector<const BsodEntry*> shown;
};
State B;

// Группа причин → цвет (легенда под списком).
Tone ToneOf(const BsodEntry& e) {
    std::string c = e.category;
    if (c == "hardware" || c == "gpu") return Tone::Red;                                       // оборудование
    if (c == "memory" || c == "memory_or_driver" || c == "driver_or_memory") return Tone::Orange; // память
    if (c == "disk" || c == "filesystem" || c == "driver_or_disk" || c == "boot") return Tone::Teal;  // диск, загрузка
    if (c == "os_integrity" || c == "registry") return Tone::Green;                           // целостность Windows, реестр
    return Tone::Blue;                                                                         // драйвер / ПО
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

std::string CodeText(uint32_t code) { return fmt("0x%X", code); }
inline bool EN() { return CurrentLang() == Lang::EN; }
inline const char* bWhat(const BsodEntry& e) { return EN() ? e.whatEn : e.what; }
inline const char* bSteps(const BsodEntry& e) { return EN() ? e.stepsEn : e.steps; }
inline const char* bImp(const BsodEntry& e) { return EN() ? e.importantEn : e.important; }

// Поиск: «7b», «0x7B», «0x0000007B», часть имени или текста.
bool Match(const BsodEntry& e, const std::wstring& q) {
    if (q.empty()) return true;
    std::wstring hay = Lower(WRaw(CodeText(e.code)) + L" " + WRaw(fmt("0x%08X", e.code)) + L" " + WRaw(e.name) + L" " + WRaw(bWhat(e)) + L" " + WRaw(bSteps(e)) + L" " + WRaw(bImp(e)));
    std::wstring qq = Lower(q);
    if (hay.find(qq) != std::wstring::npos) return true;
    // число без 0x и ведущих нулей
    std::wstring hex = qq;
    if (hex.rfind(L"0x", 0) == 0) hex = hex.substr(2);
    while (hex.size() > 1 && hex[0] == L'0') hex.erase(0, 1);
    return !hex.empty() && Lower(W(fmt("%X", e.code))) == hex;
}

void ShowDetail() {
    int s = ListView_GetNextItem(B.list, -1, LVNI_SELECTED);
    if (s < 0 || s >= (int)B.shown.size()) {
        SetWindowTextW(B.detail, EN() ? L"Pick a code in the list on the left — its explanation and first steps appear here."
                                      : L"Выберите код в списке слева — здесь появится расшифровка и первые шаги.");
        return;
    }
    const BsodEntry& e = *B.shown[(size_t)s];
    std::string t;
    if (EN())
        t = std::string(e.name) + "  (" + CodeText(e.code) + ")" + (e.frequent ? "  — common" : "") + "\r\n\r\n" +
            "What it is:\r\n" + e.whatEn + ".\r\n\r\n" +
            "First step:\r\n" + e.stepsEn + ".\r\n\r\n" +
            "Important:\r\n" + e.importantEn + ".\r\n\r\n" +
            "In WinRepair:\r\n"
            "  - \"BSOD: events and dumps\" — when and with which code blue screens happened;\r\n"
            "  - \"Dump analysis\" — which module (driver) is named in the dump;\r\n"
            "  - \"Drivers and devices with errors\", \"Disks, SMART\", \"Memory (SMBIOS)\", \"Integrity check\" — the checks from the first step.";
    else
        t = std::string(e.name) + "  (" + CodeText(e.code) + ")" + (e.frequent ? "  — встречается часто" : "") + "\r\n\r\n" +
            "Что это:\r\n" + e.what + ".\r\n\r\n" +
            "Первый шаг:\r\n" + e.steps + ".\r\n\r\n" +
            "Важно:\r\n" + e.important + ".\r\n\r\n" +
            "В WinRepair:\r\n"
            "  • «BSOD: события и дампы» — когда и с каким кодом были синие экраны;\r\n"
            "  • «Разбор дампов» — какой модуль (драйвер) указан в дампе;\r\n"
            "  • «Драйверы и устройства с ошибками», «Диски, SMART», «Память (SMBIOS)», «Проверка целостности» — проверки из первого шага.";
    SetWindowTextW(B.detail, WRaw(t).c_str());
}

void Fill() {
    std::wstring q = GetText(B.find);
    bool onlyFreq = SendMessageW(B.freq, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ListView_DeleteAllItems(B.list);
    B.shown.clear();
    size_t n = 0;
    const BsodEntry* t = bsodTable(n);
    for (size_t i = 0; i < n; ++i) {
        if (onlyFreq && !t[i].frequent) continue;
        if (!Match(t[i], q)) continue;
        B.shown.push_back(&t[i]);
    }
    for (size_t i = 0; i < B.shown.size(); ++i) {
        const BsodEntry& e = *B.shown[i];
        std::wstring code = W(CodeText(e.code));
        LVITEMW li{};
        li.mask = LVIF_TEXT;
        li.iItem = (int)i;
        li.pszText = (LPWSTR)code.c_str();
        ListView_InsertItem(B.list, &li);
        std::wstring nm = WRaw(e.name), what = WRaw(bWhat(e));
        ListView_SetItemText(B.list, (int)i, 1, (LPWSTR)nm.c_str());
        ListView_SetItemText(B.list, (int)i, 2, (LPWSTR)what.c_str());
        ListView_SetItemText(B.list, (int)i, 3, (LPWSTR)(e.frequent ? (EN() ? L"common" : L"часто") : L""));
    }
    if (!B.shown.empty()) ListView_SetItemState(B.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ShowDetail();
}

void Layout() {
    RECT rc;
    GetClientRect(B.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8), y = m, bh = S(32);
    Place(B.caveat, m, y, W_ - 2 * m, S(54));
    y += S(60);
    Place(GetDlgItem(B.wnd, 9590), m, y + S(4), S(60), S(20));
    Place(B.find, m + S(64), y, S(320), S(26));
    Place(B.freq, m + S(400), y + S(3), S(260), S(22));
    y += S(34);
    int listW = (W_ - 3 * m) * 58 / 100, bottom = H - m - bh - S(8) - S(24) - S(24);
    Place(B.list, m, y, listW, bottom - y);
    Place(B.detail, 2 * m + listW, y, W_ - 3 * m - listW, bottom - y);
    y = bottom + S(4);
    Place(B.legend, m, y, W_ - 2 * m, S(20));
    y += S(24);
    Place(B.dumps, m, y, W_ - 2 * m, S(20));
    y = H - m - bh;
    Place(B.bEvents, m, y, S(220), bh);
    Place(B.bDump, m + S(228), y, S(200), bh);
    Place(B.bCopy, m + S(436), y, S(170), bh);
    Place(B.bClose, W_ - m - S(110), y, S(110), bh);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (B.bClose) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(900); mm->ptMinTrackSize.y = S(560); return 0; }
        case WM_CTLCOLORSTATIC:
            if ((HWND)lp == B.caveat) {   // оговорка — на светлом жёлтом фоне, чтобы её прочли первой
                static HBRUSH yb = CreateSolidBrush(RGB(255, 247, 214));
                SetBkColor((HDC)wp, RGB(255, 247, 214));
                SetTextColor((HDC)wp, RGB(110, 70, 0));
                return (LRESULT)yb;
            }
            break;
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == ID_FIND && code == EN_CHANGE) Fill();
            else if (id == ID_FREQ) Fill();
            else if (id == ID_B_EVENTS) UiRunModule("bsod");
            else if (id == ID_B_DUMP) UiRunModule("dump");
            else if (id == ID_B_COPY) ClipText(h, GetText(B.detail));
            else if (id == ID_B_CLOSE) DestroyWindow(h);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->hwndFrom == B.list) {
                if (nh->code == LVN_ITEMCHANGED) ShowDetail();
                else if (nh->code == NM_CUSTOMDRAW)
                    return ListToneDraw(lp, [](int i) { return i >= 0 && i < (int)B.shown.size() ? ToneOf(*B.shown[(size_t)i]) : Tone::None; }, 1, true);
            }
            return 0;
        }
        case WM_DESTROY: B.wnd = nullptr; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void OpenBsodReferenceImpl() {
    if (B.wnd) { ShowWindow(B.wnd, SW_RESTORE); SetForegroundWindow(B.wnd); return; }
    RegisterOnce(L"WRBsod", Proc);
    B = State();
    B.wnd = CreateWindowExW(0, L"WRBsod", L"Справочник кодов BSOD — что значит код и с чего начать (F1 — справка)", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, S(1180), S(720), g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = B.wnd;
    B.caveat = Mk(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, ID_CAVEAT);
    SetWindowTextW(B.caveat, WRaw(std::string("  ") + bsodCaveat(EN())).c_str());
    Mk(h, L"STATIC", L"Поиск:", SS_LEFT, 9590);
    B.find = Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_FIND, WS_EX_CLIENTEDGE);
    SendMessageW(B.find, EM_SETCUEBANNER, TRUE, (LPARAM)TrW(L"Код (7B, 0x7B) или название, например IRQL").c_str());
    B.freq = Mk(h, L"BUTTON", L"Только частые коды", BS_AUTOCHECKBOX | WS_TABSTOP, ID_FREQ);
    B.list = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(B.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[] = {L"Код", L"Название", L"Что это", L"Частый"};
    int cw[] = {S(70), S(290), S(260), S(60)};
    for (int i = 0; i < 4; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = cw[i]; ListView_InsertColumn(B.list, i, &c); }
    B.detail = Mk(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, ID_DETAIL, WS_EX_CLIENTEDGE);
    B.legend = MkLegend(h, ID_LEGEND, {{Tone::Blue, L"драйвер / ПО"}, {Tone::Orange, L"память"}, {Tone::Teal, L"диск / загрузка"},
                                       {Tone::Green, L"целостность Windows / реестр"}, {Tone::Red, L"оборудование"}});
    B.dumps = Mk(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, ID_DUMPS);
    SetWindowTextW(B.dumps, WRaw(bsodDumpsNote(EN())).c_str());
    B.bEvents = MkBtn(h, L"BSOD: события и дампы", ID_B_EVENTS, Tone::Blue);
    B.bDump = MkBtn(h, L"Разбор дампов", ID_B_DUMP, Tone::Blue);
    B.bCopy = MkBtn(h, L"Копировать описание", ID_B_COPY, Tone::Gray);
    B.bClose = MkBtn(h, L"Закрыть", ID_B_CLOSE, Tone::Gray);
    Layout();
    Fill();
    ShowWindow(h, SW_SHOW);
    SetFocus(B.find);
}

}  // namespace ui

void OpenBsodReference() { ui::OpenBsodReferenceImpl(); }

}  // namespace wr
