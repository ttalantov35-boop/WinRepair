#pragma once
#include "ui_tools.h"
#include <commctrl.h>
#include <commdlg.h>
#include "ui_theme.h"
#include <cwchar>
#include <map>
#include <algorithm>

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

// Right-click menu shared by every tool window: copy from a table under the pointer + every push button of the window as a command.
// `clicked` is the control under the pointer (WM_CONTEXTMENU wParam).
inline void ShowCommandsMenu(HWND wnd, HWND clicked, LPARAM lp) {
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    if (lp == (LPARAM)-1) { RECT r; GetWindowRect(clicked ? clicked : wnd, &r); pt = {r.left + 12, r.top + 12}; }
    HMENU m = CreatePopupMenu();
    wchar_t cn[64] = L"";
    if (clicked) GetClassNameW(clicked, cn, 64);
    bool isList = !wcscmp(cn, WC_LISTVIEWW);
    enum { C_ROW = 1, C_ALL, C_CMD0 = 100 };
    if (isList) {
        int sel = ListView_GetNextItem(clicked, -1, LVNI_SELECTED);
        AppendMenuW(m, MF_STRING | (sel < 0 ? MF_GRAYED : 0), C_ROW, TrW(L"Копировать выбранную строку").c_str());
        AppendMenuW(m, MF_STRING | (ListView_GetItemCount(clicked) == 0 ? MF_GRAYED : 0), C_ALL, TrW(L"Копировать всю таблицу").c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    std::vector<HWND> btns;
    std::vector<std::wstring> seen;
    for (HWND c = GetWindow(wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t k[32] = L"";
        GetClassNameW(c, k, 32);
        if (_wcsicmp(k, L"Button") != 0 || !IsWindowVisible(c)) continue;
        if ((GetWindowLongW(c, GWL_STYLE) & 0xF) > BS_DEFPUSHBUTTON) continue;   // only push buttons, not check boxes or groups
        std::wstring t = GetText(c);
        t.erase(std::remove(t.begin(), t.end(), L'&'), t.end());
        if (t.empty() || std::find(seen.begin(), seen.end(), t) != seen.end()) continue;
        seen.push_back(t);
        AppendMenuW(m, MF_STRING | (IsWindowEnabled(c) ? 0 : MF_GRAYED), (UINT_PTR)(C_CMD0 + btns.size()), t.c_str());
        btns.push_back(c);
    }
    SetForegroundWindow(wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (c == C_ROW) { int sel = ListView_GetNextItem(clicked, -1, LVNI_SELECTED); if (sel >= 0) ClipText(wnd, ListRowText(clicked, sel)); }
    else if (c == C_ALL) {
        std::wstring all;
        for (int i = 0, n = ListView_GetItemCount(clicked); i < n; ++i) all += ListRowText(clicked, i) + L"\r\n";
        ClipText(wnd, all);
    } else if (c >= C_CMD0 && c < C_CMD0 + (int)btns.size()) SendMessageW(btns[(size_t)(c - C_CMD0)], BM_CLICK, 0, 0);
}

namespace detail {
inline std::map<std::wstring, WNDPROC>& ToolProcs() { static std::map<std::wstring, WNDPROC> m; return m; }
// Every tool window gets the right-click menu: the window's own procedure may handle WM_CONTEXTMENU and return 1,
// otherwise the shared menu (commands of the window + copy from tables) is shown.
inline LRESULT CALLBACK ToolProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    wchar_t cn[128] = L"";
    GetClassNameW(h, cn, 128);
    auto it = ToolProcs().find(cn);
    WNDPROC orig = it != ToolProcs().end() ? it->second : DefWindowProcW;
    if (m == WM_CONTEXTMENU) {
        if (orig(h, m, wp, lp) != 1) ShowCommandsMenu(h, (HWND)wp, lp);
        return 0;
    }
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
};
bool FormDialog(HWND owner, const std::wstring& title, std::vector<FormField>& fields);

// fill a combo with targets; returns them
std::vector<Target> FillTargetCombo(HWND combo, bool offlineOnly);

void OpenRegistryEditorImpl();
void OpenBcdManagerImpl();
void OpenAutorunsImpl();
void OpenFixWizardImpl();
void OpenStorageAccessImpl();
void OpenTriageImpl();
void OpenPlaybooksImpl();
void PlaybooksJobDone();
void TriageJobDone();
void OpenWinReImpl();
void OpenSaveDataImpl();
void WinReJobDone();
void StorageJobDone();
void BcdJobDone();
void AutorunsJobDone();

}  // namespace ui
}  // namespace wr
