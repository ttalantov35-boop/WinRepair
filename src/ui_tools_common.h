#pragma once
#include "ui_tools.h"
#include <commctrl.h>
#include <commdlg.h>
#include "ui_theme.h"
#include <cwchar>
#include <map>
#include <algorithm>
#include <functional>

namespace wr {

// text of list items, columns and tabs is translated like everything else on screen
inline int LvInsert(HWND h, LVITEMW* p) {
    if ((p->mask & LVIF_TEXT) && p->pszText) {
        std::wstring t = TrW(p->pszText);
        LVITEMW c = *p;
        c.pszText = (LPWSTR)t.c_str();
        return (int)SendMessageW(h, LVM_INSERTITEMW, 0, (LPARAM)&c);
    }
    return (int)SendMessageW(h, LVM_INSERTITEMW, 0, (LPARAM)p);
}
inline void LvSetText(HWND h, int i, int sub, const wchar_t* text) {
    std::wstring t = TrW(text ? text : L"");
    LVITEMW it{};
    it.iSubItem = sub;
    it.pszText = (LPWSTR)t.c_str();
    SendMessageW(h, LVM_SETITEMTEXTW, (WPARAM)i, (LPARAM)&it);
}
inline int LvInsertColumn(HWND h, int i, LVCOLUMNW* c) {
    std::wstring t;
    LVCOLUMNW cc = *c;
    if ((c->mask & LVCF_TEXT) && c->pszText) { t = TrW(c->pszText); cc.pszText = (LPWSTR)t.c_str(); }
    return (int)SendMessageW(h, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&cc);
}
inline int TabInsert(HWND h, int i, TCITEMW* p) {
    std::wstring t;
    TCITEMW c = *p;
    if ((p->mask & TCIF_TEXT) && p->pszText) { t = TrW(p->pszText); c.pszText = (LPWSTR)t.c_str(); }
    return (int)SendMessageW(h, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&c);
}

}  // namespace wr
#undef ListView_InsertItem
#undef ListView_SetItemText
#undef ListView_InsertColumn
#undef TabCtrl_InsertItem
#define ListView_InsertItem(h, p) ::wr::LvInsert(h, p)
#define ListView_SetItemText(h, i, s, t) ::wr::LvSetText(h, i, s, t)
#define ListView_InsertColumn(h, i, c) ::wr::LvInsertColumn(h, i, c)
#define TabCtrl_InsertItem(h, i, p) ::wr::TabInsert(h, i, p)

namespace wr {
namespace ui {

inline int S(int v) { return MulDiv(v, g_ui.dpi, 96); }
inline std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}
inline HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    bool push = !wcscmp(cls, L"BUTTON") && (style & 0xF) <= BS_DEFPUSHBUTTON;   // command buttons are bold in every tool window
    SendMessageW(h, WM_SETFONT, (WPARAM)(push ? wr::theme::Semi() : g_ui.font), TRUE);
    return h;
}
inline void Place(HWND h, int x, int y, int w, int hgt) { MoveWindow(h, x, y, w, hgt, TRUE); }
inline void ClipText(HWND owner, const std::wstring& txt) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (txt.size() + 1) * sizeof(wchar_t));
    if (g) {
        memcpy(GlobalLock(g), txt.c_str(), (txt.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

inline std::wstring ListRowText(HWND lv, int row) {
    HWND hdr = ListView_GetHeader(lv);
    int cols = hdr ? Header_GetItemCount(hdr) : 1;
    std::wstring out;
    for (int c = 0; c < std::max(1, cols); ++c) {
        wchar_t buf[2048] = L"";
        ListView_GetItemText(lv, row, c, buf, 2048);
        if (c) out += L'\t';
        out += buf;
    }
    return out;
}

// Right-click menu shared by every tool window: ONLY for a table under the pointer (copy cell / row / whole table).
// Anywhere else (buttons, labels, empty area) nothing is shown: a context menu belongs to the object it is opened on.
// `clicked` is the control under the pointer (WM_CONTEXTMENU wParam).
inline void ShowCommandsMenu(HWND wnd, HWND clicked, LPARAM lp) {
    wchar_t cn[64] = L"";
    if (clicked) GetClassNameW(clicked, cn, 64);
    if (wcscmp(cn, WC_LISTVIEWW) != 0) return;
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    if (lp == (LPARAM)-1) { RECT r; GetWindowRect(clicked, &r); pt = {r.left + 12, r.top + 12}; }
    else {   // right-click selects the row under the pointer, like in Explorer
        POINT c = pt;
        ScreenToClient(clicked, &c);
        LVHITTESTINFO hi{};
        hi.pt = c;
        int i = ListView_HitTest(clicked, &hi);
        if (i >= 0) ListView_SetItemState(clicked, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    int sel = ListView_GetNextItem(clicked, -1, LVNI_SELECTED);
    enum { C_ROW = 1, C_ALL };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (sel < 0 ? MF_GRAYED : 0), C_ROW, TrW(L"Копировать выбранную строку").c_str());
    AppendMenuW(m, MF_STRING | (ListView_GetItemCount(clicked) == 0 ? MF_GRAYED : 0), C_ALL, TrW(L"Копировать всю таблицу").c_str());
    SetForegroundWindow(wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (c == C_ROW && sel >= 0) ClipText(wnd, ListRowText(clicked, sel));
    else if (c == C_ALL) {
        std::wstring all;
        for (int i = 0, n = ListView_GetItemCount(clicked); i < n; ++i) all += ListRowText(clicked, i) + L"\r\n";
        ClipText(wnd, all);
    }
}

// ---- цвета по действиям: одинаковые во всех окнах -----------------------------------------------------
// Синий — только чтение/диагностика, бирюзовый — открывает инструмент или окно, оранжевый — меняет систему,
// красный — опасно (нужна копия), зелёный — защита/хорошо, серый — инструкция/служебное/системное.
enum class Tone { None, Blue, Teal, Orange, Red, Green, Gray };
inline COLORREF ToneColor(Tone t) {
    switch (t) {
        case Tone::Blue: return RGB(26, 103, 210);
        case Tone::Teal: return RGB(0, 137, 123);
        case Tone::Orange: return RGB(214, 110, 0);
        case Tone::Red: return RGB(200, 32, 32);
        case Tone::Green: return RGB(30, 140, 70);
        case Tone::Gray: return RGB(110, 118, 130);
        default: return CLR_NONE;
    }
}
inline COLORREF ToneBk(Tone t) { return t == Tone::None ? CLR_NONE : theme::Mix(RGB(255, 255, 255), ToneColor(t), t == Tone::Gray ? 8 : 11); }
inline theme::Cat ToneCat(Tone t) {
    switch (t) {
        case Tone::Blue: return theme::Cat::Diag;
        case Tone::Teal: return theme::Cat::Guide;
        case Tone::Orange: return theme::Cat::Repair;
        case Tone::Red: return theme::Cat::Exit;
        case Tone::Green: return theme::Cat::Protect;
        default: return theme::Cat::Info;
    }
}

// Цветная кнопка в окне инструмента (рисует ToolProc / окно-владелец через DrawToolButton).
inline HWND MkBtn(HWND parent, const wchar_t* text, int id, Tone tone) {
    HWND b = Mk(parent, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, id);
    SetPropW(b, L"WRTone", (HANDLE)(INT_PTR)((int)tone + 1));
    theme::AttachHover(b);
    return b;
}
inline bool DrawToolButton(LPARAM lp, HBRUSH under = nullptr) {
    auto* d = (DRAWITEMSTRUCT*)lp;
    if (!d || d->CtlType != ODT_BUTTON) return false;
    INT_PTR v = (INT_PTR)GetPropW(d->hwndItem, L"WRTone");
    if (!v) return false;
    Tone t = (Tone)(v - 1);
    theme::DrawButton(d, ToneCat(t), theme::IsHover(d->hwndItem), under ? under : GetSysColorBrush(COLOR_BTNFACE));
    return true;
}

// Раскраска строк ListView: светлый фон строки по цвету, цветная полоса слева, выделенная цветом колонка.
// toneOf(i) — цвет строки i; accentCol — колонка, текст которой пишется цветом и жирно (-1 — нет);
// stripe — рисовать полосу слева (не для списков с галочками).
inline LRESULT ListToneDraw(LPARAM lp, const std::function<Tone(int)>& toneOf, int accentCol, bool stripe) {
    auto* cd = (NMLVCUSTOMDRAW*)lp;
    static HFONT bold = nullptr;
    if (!bold) {
        LOGFONTW lf{};
        GetObjectW(g_ui.font, sizeof lf, &lf);
        lf.lfWeight = FW_BOLD;
        bold = CreateFontIndirectW(&lf);
    }
    HWND lv = cd->nmcd.hdr.hwndFrom;
    int item = (int)cd->nmcd.dwItemSpec;
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            Tone t = toneOf(item);
            bool sel = (ListView_GetItemState(lv, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            if (t != Tone::None && !sel) cd->clrTextBk = ToneBk(t);
            return CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT;
        }
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            Tone t = toneOf(item);
            bool sel = (ListView_GetItemState(lv, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            if (t != Tone::None && !sel) cd->clrTextBk = ToneBk(t);
            if (cd->iSubItem == accentCol && t != Tone::None) {
                if (!sel) cd->clrText = ToneColor(t);
                SelectObject(cd->nmcd.hdc, bold);
            } else {
                cd->clrText = sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);
                SelectObject(cd->nmcd.hdc, g_ui.font);
            }
            return CDRF_NEWFONT;
        }
        case CDDS_ITEMPOSTPAINT: {
            Tone t = toneOf(item);
            if (stripe && t != Tone::None) {
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

// Строка-легенда под списком: «■ синий — … ■ оранжевый — …». Рисуется сама (WM_PAINT статика не нужна).
struct LegendItem { Tone tone; const wchar_t* text; };
inline LRESULT CALLBACK LegendProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
        auto* items = (std::vector<LegendItem>*)ref;
        HGDIOBJ of = SelectObject(dc, g_ui.font);
        SetBkMode(dc, TRANSPARENT);
        int x = 0, sq = S(11), cy = (rc.bottom - rc.top);
        for (auto& it : *items) {
            RECT b{x, (cy - sq) / 2, x + sq, (cy - sq) / 2 + sq};
            HBRUSH br = CreateSolidBrush(ToneColor(it.tone));
            FillRect(dc, &b, br);
            DeleteObject(br);
            x += sq + S(5);
            std::wstring t = TrW(it.text);
            SIZE sz{};
            GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
            SetTextColor(dc, RGB(50, 58, 70));
            TextOutW(dc, x, (cy - sz.cy) / 2, t.c_str(), (int)t.size());
            x += sz.cx + S(16);
        }
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_NCDESTROY) { delete (std::vector<LegendItem>*)ref; RemoveWindowSubclass(h, LegendProc, 1); }
    return DefSubclassProc(h, m, wp, lp);
}
inline HWND MkLegend(HWND parent, int id, std::initializer_list<LegendItem> items) {
    HWND h = Mk(parent, L"STATIC", L"", SS_LEFT, id);
    SetWindowSubclass(h, LegendProc, 1, (DWORD_PTR) new std::vector<LegendItem>(items));
    return h;
}

namespace detail {
inline std::map<std::wstring, WNDPROC>& ToolProcs() { static std::map<std::wstring, WNDPROC> m; return m; }
// The window's own procedure may handle WM_CONTEXTMENU (return 1); otherwise a table gets the shared copy menu.
inline LRESULT CALLBACK ToolProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    wchar_t cn[128] = L"";
    GetClassNameW(h, cn, 128);
    auto it = ToolProcs().find(cn);
    WNDPROC orig = it != ToolProcs().end() ? it->second : DefWindowProcW;
    if (m == WM_CONTEXTMENU) {
        if (orig(h, m, wp, lp) != 1) ShowCommandsMenu(h, (HWND)wp, lp);
        return 0;
    }
    if (m == WM_DRAWITEM && DrawToolButton(lp)) return TRUE;   // цветные кнопки MkBtn во всех окнах инструментов
    return orig(h, m, wp, lp);
}
}  // namespace detail

inline void RegisterOnce(const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW wc{sizeof wc};
    if (GetClassInfoExW(GetModuleHandleW(nullptr), name, &wc)) return;
    detail::ToolProcs()[name] = proc;
    wc = WNDCLASSEXW{sizeof wc};
    wc.lpfnWndProc = detail::ToolProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

struct FormField {
    std::wstring label;
    int kind = 0;  // 0 edit, 1 multiline, 2 dropdown list, 3 dropdown with editing, 4 password
    std::vector<std::wstring> options;
    std::wstring value;
    bool readonly = false;
    // Кнопка «Обзор…» справа от поля (kind 0 или 3): выбор файла в стандартном окне проводника.
    // Набор пути вручную по-прежнему работает. Фильтр: "Описание|маска|Описание|маска|", например
    // L"Образ ISO (*.iso)|*.iso|Все файлы (*.*)|*.*|"; пусто — все файлы.
    bool browseFile = false;
    std::wstring browseFilter;
};
bool FormDialog(HWND owner, const std::wstring& title, std::vector<FormField>& fields);

// fill a combo with targets; returns them
std::vector<Target> FillTargetCombo(HWND combo, bool offlineOnly);
bool RefreshTargetCombo(HWND combo, std::vector<Target>& list, bool offlineOnly);   // true if the list changed

void OpenRegistryEditorImpl();
void OpenAppsImpl();
void AppsJobDone();
void OpenOfflineExplorerImpl();
void OpenOfflineExplorerAt(const std::string& path);
void OpenBcdManagerImpl();
void OpenAutorunsImpl();
void OpenFixWizardImpl();
void OpenStorageAccessImpl();
void OpenTriageImpl();
void OpenPlaybooksImpl();
void PlaybooksJobDone();
void TriageJobDone();
void OpenWinReImpl();
void OpenWinPeBuilderImpl();   // диалог «Свой WinPE с WinRepair» (владелец — окно WinRE или главное окно)
void OpenSaveDataImpl();
void OpenPasswordToolImpl();   // снятие пароля / разблокировка (ui_password.cpp)
void PasswordJobDone();
void OpenPartitionToolImpl();  // разделы диска / diskpart (ui_partition.cpp)
void PartitionJobDone();
void WinReJobDone();
void StorageJobDone();
void BcdJobDone();
void AutorunsJobDone();

}  // namespace ui
}  // namespace wr
