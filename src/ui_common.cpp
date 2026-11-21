// Shared UI plumbing: modal form dialog, target combo, public entry points.
#include "ui_tools_common.h"
#include "modules/bcd.h"
#include "win/offreg.h"

namespace wr {

UiHost g_ui;

namespace ui {

namespace {
struct FormState {
    std::vector<FormField>* f = nullptr;
    std::vector<HWND> ctl;
    bool done = false, ok = false;
};

// «Обзор…»: стандартное окно выбора файла. Стартует с того, что уже набрано в поле (файл — выделен,
// папка — открыта); кавычки из «Копировать как путь» снимаются. Выбранный путь вписывается в поле.
void BrowseForField(HWND owner, const FormField& f, HWND ctl) {
    std::wstring cur = GetText(ctl);
    while (!cur.empty() && (cur.front() == L'"' || cur.front() == L' ')) cur.erase(cur.begin());
    while (!cur.empty() && (cur.back() == L'"' || cur.back() == L' ')) cur.pop_back();
    wchar_t file[MAX_PATH * 2] = L"";
    std::wstring dir;
    DWORD a = cur.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(cur.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) dir = cur;
    else if (a != INVALID_FILE_ATTRIBUTES && cur.size() < MAX_PATH) lstrcpynW(file, cur.c_str(), (int)(sizeof file / sizeof file[0]));
    else if (!cur.empty()) {   // набран несуществующий файл — открываем его папку, если она есть
        size_t k = cur.find_last_of(L'\\');
        if (k != std::wstring::npos) {
            std::wstring d = cur.substr(0, k + 1);
            DWORD da = GetFileAttributesW(d.c_str());
            if (da != INVALID_FILE_ATTRIBUTES && (da & FILE_ATTRIBUTE_DIRECTORY)) dir = d;
        }
    }
    // Фильтр "Описание|маска|…": описания переводятся, разделители '|' → '\0', в конце двойной '\0'.
    std::wstring src = f.browseFilter.empty() ? std::wstring(L"Все файлы (*.*)|*.*|") : f.browseFilter, filt, part;
    int n = 0;
    for (wchar_t ch : src) {
        if (ch != L'|') { part += ch; continue; }
        filt += (n % 2 == 0 ? TrW(part) : part);
        filt.push_back(L'\0');
        part.clear();
        ++n;
    }
    filt.push_back(L'\0');
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = owner;
    o.lpstrFilter = filt.c_str();
    o.lpstrFile = file;
    o.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
    o.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&o)) return;
    SetWindowTextW(ctl, file);
    SetFocus(ctl);
}

LRESULT CALLBACK FormProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* st = (FormState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_COMMAND:
            if (!st) return 0;
            if (LOWORD(wp) >= 3200 && LOWORD(wp) < 3200 + st->f->size() && HIWORD(wp) == BN_CLICKED) {
                size_t i = LOWORD(wp) - 3200;
                BrowseForField(h, (*st->f)[i], st->ctl[i]);
                return 0;
            }
            if (LOWORD(wp) == IDOK) {
                for (size_t i = 0; i < st->f->size(); ++i) {
                    FormField& f = (*st->f)[i];
                    if (f.kind == 2) {
                        int sel = (int)SendMessageW(st->ctl[i], CB_GETCURSEL, 0, 0);
                        if (sel >= 0 && sel < (int)f.options.size()) f.value = f.options[(size_t)sel];  // original (untranslated) text
                    } else f.value = GetText(st->ctl[i]);
                }
                st->ok = true;
                st->done = true;
            } else if (LOWORD(wp) == IDCANCEL) st->done = true;
            return 0;
        case WM_CLOSE: if (st) st->done = true; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
}  // namespace

bool FormDialog(HWND owner, const std::wstring& title, std::vector<FormField>& fields) {
    RegisterOnce(L"WRForm", FormProc);
    int w = S(620), y = S(10), pad = S(12);
    int total = S(10);
    for (auto& f : fields) total += S(20) + (f.kind == 1 ? S(110) : S(28)) + S(8);
    total += S(56);
    RECT orc{0, 0, 800, 600};
    if (owner) GetWindowRect(owner, &orc);
    int x0 = orc.left + ((orc.right - orc.left) - w) / 2, y0 = orc.top + ((orc.bottom - orc.top) - total) / 2;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"WRForm", title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x0, y0, w, total + S(30), owner, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    FormState st;
    st.f = &fields;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);
    RECT cr;
    GetClientRect(h, &cr);
    int cw = cr.right - 2 * pad;
    for (size_t i = 0; i < fields.size(); ++i) {
        auto& f = fields[i];
        HWND lb = Mk(h, L"STATIC", f.label.c_str(), SS_LEFT, 3000 + (int)i);
        Place(lb, pad, y, cw, S(18));
        y += S(20);
        HWND c = nullptr;
        if (f.kind == 0 || f.kind == 4) c = Mk(h, L"EDIT", f.value.c_str(), ES_AUTOHSCROLL | WS_TABSTOP | (f.readonly ? ES_READONLY : 0) | (f.kind == 4 ? ES_PASSWORD : 0), 3100 + (int)i, WS_EX_CLIENTEDGE);
        else if (f.kind == 1) c = Mk(h, L"EDIT", f.value.c_str(), ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP | ES_WANTRETURN, 3100 + (int)i, WS_EX_CLIENTEDGE);
        else {
            c = Mk(h, L"COMBOBOX", L"", (f.kind == 2 ? CBS_DROPDOWNLIST : CBS_DROPDOWN) | WS_VSCROLL | WS_TABSTOP, 3100 + (int)i);
            for (auto& o : f.options) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)TrW(o).c_str());
            if (f.kind == 2) {
                int idx = -1;
                for (size_t k = 0; k < f.options.size(); ++k) if (f.options[k] == f.value) { idx = (int)k; break; }
                SendMessageW(c, CB_SETCURSEL, idx >= 0 ? idx : 0, 0);
            } else SetWindowTextW(c, f.value.c_str());
        }
        int hh = f.kind == 1 ? S(110) : S(24);
        bool browse = f.browseFile && (f.kind == 0 || f.kind == 3);
        int fw = browse ? cw - S(108) : cw;
        Place(c, pad, y, fw, (f.kind == 2 || f.kind == 3) ? S(240) : hh);
        if (browse) {
            HWND b = Mk(h, L"BUTTON", TrW(L"Обзор…").c_str(), BS_PUSHBUTTON | WS_TABSTOP, 3200 + (int)i);
            Place(b, pad + cw - S(102), y - S(1), S(102), S(26));
        }
        st.ctl.push_back(c);
        y += (f.kind == 1 ? S(110) : S(28)) + S(8);
    }
    HWND ok = Mk(h, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
    HWND cn = Mk(h, L"BUTTON", L"Отмена", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
    Place(ok, cw + pad - S(190), y + S(4), S(90), S(28));
    Place(cn, cw + pad - S(92), y + S(4), S(90), S(28));
    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    if (!st.ctl.empty()) SetFocus(st.ctl[0]);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F1) { ShowHelp(HelpTopicForWindow(owner)); continue; }
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (!st.done) PostQuitMessage(0);
    EnableWindow(owner, TRUE);
    DestroyWindow(h);
    if (owner) SetForegroundWindow(owner);
    return st.ok;
}

std::vector<Target> FillTargetCombo(HWND combo, bool offlineOnly) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    std::vector<Target> all = UiTargets(), out;
    for (auto& t : all) if (!offlineOnly || !t.online) out.push_back(t);
    for (auto& t : out) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)W(t.label).c_str());
    // preselect the system chosen in the main window (otherwise the first one)
    size_t pick = 0;
    Target cur = UiCurrentTarget();
    if (cur.valid)
        for (size_t i = 0; i < out.size(); ++i)
            if (out[i].online == cur.online && toLowerUtf8(out[i].winDir) == toLowerUtf8(cur.winDir)) { pick = i; break; }
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)pick, 0);
    return out;
}

// The main window may have re-scanned the disks since this tool window was opened: refill the list, keep the user's choice.
bool RefreshTargetCombo(HWND combo, std::vector<Target>& list, bool offlineOnly) {
    size_t n = 0;
    for (auto& t : UiTargets()) if (!offlineOnly || !t.online) ++n;
    if (n == list.size()) return false;
    int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    std::string keep = (sel >= 0 && sel < (int)list.size()) ? list[(size_t)sel].winDir : "";
    list = FillTargetCombo(combo, offlineOnly);
    for (size_t i = 0; i < list.size(); ++i)
        if (!keep.empty() && list[i].winDir == keep) { SendMessageW(combo, CB_SETCURSEL, (WPARAM)i, 0); break; }
    return true;
}

}  // namespace ui

void OpenRegistryEditor() { ui::OpenRegistryEditorImpl(); }
void OpenBcdManager() { ui::OpenBcdManagerImpl(); }
void OpenAutoruns() { ui::OpenAutorunsImpl(); }
void OpenInstalledApps() { ui::OpenAppsImpl(); }
void OpenOfflineExplorer() { ui::OpenOfflineExplorerImpl(); }
void OpenFixWizard() { ui::OpenFixWizardImpl(); }
void OpenWinReManager() { ui::OpenWinReImpl(); }
void OpenWinPeBuilder() { ui::OpenWinPeBuilderImpl(); }
void OpenTriage() { ui::OpenTriageImpl(); }
void OpenPlaybooks() { ui::OpenPlaybooksImpl(); }
void OpenStorageAccess() { ui::OpenStorageAccessImpl(); }
void OpenSaveData() { ui::OpenSaveDataImpl(); }
void UiJobDone() { ui::BcdJobDone(); ui::AutorunsJobDone(); ui::WinReJobDone(); ui::StorageJobDone(); ui::TriageJobDone(); ui::PlaybooksJobDone(); ui::AppsJobDone(); ui::PasswordJobDone(); ui::PartitionJobDone(); }
void UiShutdown() {
    Hives().unloadAll();
    UnmountTempBcdVolumes();
}

}  // namespace wr
