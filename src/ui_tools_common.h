#pragma once
#include "ui_tools.h"
#include <commctrl.h>
#include <commdlg.h>

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
    SendMessageW(h, WM_SETFONT, (WPARAM)g_ui.font, TRUE);
    return h;
}
inline void Place(HWND h, int x, int y, int w, int hgt) { MoveWindow(h, x, y, w, hgt, TRUE); }
inline void RegisterOnce(const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW wc{sizeof wc};
    if (GetClassInfoExW(GetModuleHandleW(nullptr), name, &wc)) return;
    wc = WNDCLASSEXW{sizeof wc};
    wc.lpfnWndProc = proc;
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
