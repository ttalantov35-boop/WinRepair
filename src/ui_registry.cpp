// Offline registry editor: choose a Windows folder, load its hives, browse and edit.
#include "ui_tools_common.h"
#include "win/offreg.h"
#include <shellapi.h>

namespace wr {
namespace ui {

namespace {

enum { ID_COMBO = 4001, ID_LOAD, ID_UNLOAD, ID_REGEDIT, ID_TREE, ID_LIST, ID_PATH, ID_NEWVAL, ID_EDIT, ID_DELVAL, ID_NEWKEY, ID_DELKEY, ID_EXPORT, ID_COPY, ID_NOTE };

struct State {
    HWND wnd = nullptr, combo, tree, list, path, note, btn[8], bLoad, bUnload, bRegedit;
    std::vector<std::unique_ptr<std::string>> paths;
    std::map<std::string, std::string> labels;  // mount -> label
    std::vector<RegVal> vals;
    std::vector<Target> targets;
    bool pinned = false;
};
State R;

std::string SelPath() {
    HTREEITEM it = TreeView_GetSelection(R.tree);
    if (!it) return "";
    TVITEMW tv{};
    tv.mask = TVIF_PARAM;
    tv.hItem = it;
    TreeView_GetItem(R.tree, &tv);
    return tv.lParam ? *(std::string*)tv.lParam : "";
}

HTREEITEM AddNode(HTREEITEM parent, const std::string& text, const std::string& full, bool kids) {
    R.paths.push_back(std::make_unique<std::string>(full));
    std::wstring wt = W(text);
    TVINSERTSTRUCTW t{};
    t.hParent = parent;
    t.hInsertAfter = TVI_LAST;
    t.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    t.item.pszText = (LPWSTR)wt.c_str();
    t.item.lParam = (LPARAM)R.paths.back().get();
    t.item.cChildren = kids ? 1 : 0;
    return TreeView_InsertItem(R.tree, &t);
}

bool HasSubKeys(const std::string& full) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, W(full).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD n = 0;
    RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &n, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    return n > 0;
}

void Expand(HTREEITEM it, const std::string& full) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, W(full).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    std::vector<std::string> subs;
    EnumSubKeys(k, subs);
    RegCloseKey(k);
    std::sort(subs.begin(), subs.end(), [](const std::string& a, const std::string& b) { return toLowerUtf8(a) < toLowerUtf8(b); });
    for (const auto& s : subs) AddNode(it, s, full + "\\" + s, HasSubKeys(full + "\\" + s));
}

std::string EditText(const RegVal& v) {
    switch (v.type) {
        case REG_SZ: case REG_EXPAND_SZ: return RegText(v);
        case REG_DWORD: return v.data.size() >= 4 ? std::to_string(*(const DWORD*)v.data.data()) : "";
        case REG_QWORD: return v.data.size() >= 8 ? std::to_string(*(const unsigned long long*)v.data.data()) : "";
        case REG_MULTI_SZ: { std::string s = RegValueText(v, 100000); for (size_t p; (p = s.find(" | ")) != std::string::npos;) s.replace(p, 3, "\r\n"); return s; }
        default: { std::string s; for (uint8_t b : v.data) s += fmt("%02X ", b); return s; }
    }
}

void FillValues() {
    ListView_DeleteAllItems(R.list);
    R.vals.clear();
    std::string p = SelPath();
    SetWindowTextW(R.path, p.empty() ? L"" : W("HKLM\\" + p).c_str());
    if (p.empty()) return;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, W(p).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    EnumValues(k, R.vals);
    RegCloseKey(k);
    for (size_t i = 0; i < R.vals.size(); ++i) {
        std::wstring n = R.vals[i].name.empty() ? L"(По умолчанию)" : W(R.vals[i].name);
        LVITEMW li{};
        li.mask = LVIF_TEXT;
        li.iItem = (int)i;
        li.pszText = (LPWSTR)n.c_str();
        ListView_InsertItem(R.list, &li);
        ListView_SetItemText(R.list, (int)i, 1, (LPWSTR)W(RegTypeName(R.vals[i].type)).c_str());
        ListView_SetItemText(R.list, (int)i, 2, (LPWSTR)W(RegValueText(R.vals[i])).c_str());
    }
}

int SelVal() { return ListView_GetNextItem(R.list, -1, LVNI_SELECTED); }

void BackupPath(const std::string& p, const char* tag) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, W(p).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    std::string reg;
    ExportRegKey(k, "HKEY_LOCAL_MACHINE\\" + p, reg);
    RegCloseKey(k);
    std::string f = Hives().backupDir() + tag + "_" + Stamp() + ".reg";
    if (WriteText(f, reg)) Logger::I().log(Lvl::Info, "Резервная копия ветки реестра: " + f);
}

void RefreshTree() {
    TreeView_DeleteAllItems(R.tree);
    R.paths.clear();
    for (const auto& m : Hives().mounts()) AddNode(TVI_ROOT, R.labels.count(m) ? R.labels[m] : m, m, HasSubKeys(m));
    FillValues();
}

void LoadHives() {
    std::wstring wp = GetText(R.combo);
    std::string p = trim(U(wp));
    int sel = (int)SendMessageW(R.combo, CB_GETCURSEL, 0, 0);
    Target t;
    if (sel >= 0 && sel < (int)R.targets.size() && W(R.targets[(size_t)sel].label) == wp) t = R.targets[(size_t)sel];
    else {
        while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
        t.winDir = p;
        t.root = p.size() >= 2 ? p.substr(0, 2) + "\\" : "";
    }
    if (t.online) { MessageBoxW(R.wnd, L"Это работающая система: её кусты открыты ядром. Для неё используйте regedit (кнопка справа).", L"Реестр", MB_OK | MB_ICONINFORMATION); return; }
    if (!PathExists(t.winDir + "\\System32\\config\\SYSTEM")) { MessageBoxW(R.wnd, L"В указанной папке нет System32\\config\\SYSTEM. Укажите папку Windows офлайн-системы (например D:\\Windows).", L"Реестр", MB_OK | MB_ICONWARNING); return; }
    if (!R.pinned) { Hives().pin(); R.pinned = true; }
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    int ok = 0;
    for (const auto& h : ListHives(t, true)) {
        std::string err;
        if (Hives().load(h, err)) { R.labels[h.mount] = h.label; ++ok; }
        else Logger::I().log(Lvl::Warn, "Куст не загружен: " + err);
    }
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    RefreshTree();
    std::wstring note = L"Загружено кустов: " + std::to_wstring(ok) + L". Резервные копии кустов: " + W(Hives().backupDir());
    RegVal v;
    HKEY sys = Hives().open("WRO_SYSTEM", KEY_READ);
    if (sys) {
        if (QueryVal(sys, "Select", "Current", v) && v.data.size() >= 4) note += L".  CurrentControlSet = ControlSet" + W(fmt("%03lu", (unsigned long)*(DWORD*)v.data.data()));
        RegCloseKey(sys);
    }
    SetWindowTextW(R.note, note.c_str());
}

bool ConfirmChange(const std::wstring& text) {
    return MessageBoxW(R.wnd, (text + L"\n\nИзменение будет записано в куст целевой Windows (копия куста уже сохранена). Продолжить?").c_str(), L"Подтверждение изменения реестра",
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

void SetValue(bool isNew) {
    std::string p = SelPath();
    if (p.empty()) return;
    int si = SelVal();
    RegVal cur;
    if (!isNew) { if (si < 0 || si >= (int)R.vals.size()) return; cur = R.vals[(size_t)si]; }
    std::vector<FormField> f(3);
    f[0].label = L"Имя значения (пусто = «По умолчанию»)";
    f[0].value = W(cur.name);
    f[0].readonly = !isNew;
    f[1].label = L"Тип";
    f[1].kind = 2;
    f[1].options = {L"REG_SZ", L"REG_EXPAND_SZ", L"REG_DWORD", L"REG_QWORD", L"REG_MULTI_SZ", L"REG_BINARY"};
    f[1].value = W(isNew ? "REG_SZ" : RegTypeName(cur.type));
    f[2].label = L"Данные (DWORD/QWORD — число или 0x…; MULTI_SZ — по строке; BINARY — hex)";
    f[2].kind = 1;
    f[2].value = W(isNew ? "" : EditText(cur));
    if (!FormDialog(R.wnd, isNew ? L"Новое значение" : L"Изменить значение", f)) return;
    std::string name = U(f[0].value);
    DWORD type = RegTypeFromName(U(f[1].value));
    std::vector<uint8_t> data;
    std::string err;
    if (!RegParse(type, U(f[2].value), data, err)) { MessageBoxW(R.wnd, W(err).c_str(), L"Ошибка данных", MB_OK | MB_ICONWARNING); return; }
    RegVal nv;
    nv.name = name; nv.type = type; nv.data = data;
    std::wstring q = L"Раздел: HKLM\\" + W(p) + L"\nЗначение: " + (name.empty() ? L"(По умолчанию)" : W(name)) + L"\nБыло: " + (isNew ? L"(нет)" : W(RegValueText(cur, 200))) + L"\nСтанет: " + W(RegValueText(nv, 200));
    if (!ConfirmChange(q)) return;
    BackupPath(p, "regedit");
    HKEY k = nullptr;
    LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, W(p).c_str(), 0, KEY_SET_VALUE, &k);
    if (r == ERROR_SUCCESS) { r = RegSetRaw(k, name, type, data); RegFlushKey(k); RegCloseKey(k); }
    if (r != ERROR_SUCCESS) { MessageBoxW(R.wnd, W("Не удалось записать: " + ErrMsg((DWORD)r)).c_str(), L"Ошибка", MB_OK | MB_ICONERROR); return; }
    AuditChange("REG SET HKLM\\" + p + " \"" + name + "\" [" + RegTypeName(type) + "]: " + (isNew ? "(нет)" : RegValueText(cur, 200)) + " -> " + RegValueText(nv, 200));
    FillValues();
}

void DelValue() {
    std::string p = SelPath();
    int si = SelVal();
    if (p.empty() || si < 0 || si >= (int)R.vals.size()) return;
    const RegVal& v = R.vals[(size_t)si];
    if (!ConfirmChange(L"Удалить значение «" + (v.name.empty() ? std::wstring(L"(По умолчанию)") : W(v.name)) + L"»\nв HKLM\\" + W(p) + L"\nТекущее: " + W(RegValueText(v, 200)))) return;
    BackupPath(p, "regedit");
    LONG r = RegDelValue(HKEY_LOCAL_MACHINE, p, v.name);
    if (r != ERROR_SUCCESS) { MessageBoxW(R.wnd, W(ErrMsg((DWORD)r)).c_str(), L"Ошибка", MB_OK | MB_ICONERROR); return; }
    AuditChange("REG DELETE VALUE HKLM\\" + p + " \"" + v.name + "\" = " + RegValueText(v, 200));
    FillValues();
}

void NewKey() {
    std::string p = SelPath();
    if (p.empty()) return;
    std::vector<FormField> f(1);
    f[0].label = L"Имя нового раздела";
    if (!FormDialog(R.wnd, L"Новый раздел", f)) return;
    std::string n = trim(U(f[0].value));
    if (n.empty() || n.find('\\') != std::string::npos) return;
    if (!ConfirmChange(L"Создать раздел «" + W(n) + L"» в HKLM\\" + W(p))) return;
    LONG r = RegMakeKey(HKEY_LOCAL_MACHINE, p + "\\" + n);
    if (r != ERROR_SUCCESS) { MessageBoxW(R.wnd, W(ErrMsg((DWORD)r)).c_str(), L"Ошибка", MB_OK | MB_ICONERROR); return; }
    AuditChange("REG CREATE KEY HKLM\\" + p + "\\" + n);
    RefreshTree();
}

void DelKey() {
    std::string p = SelPath();
    if (p.empty()) return;
    if (p.find('\\') == std::string::npos) { MessageBoxW(R.wnd, L"Корень куста удалить нельзя.", L"Реестр", MB_OK | MB_ICONWARNING); return; }
    if (!ConfirmChange(L"УДАЛИТЬ раздел со всеми вложенными разделами и значениями:\nHKLM\\" + W(p))) return;
    BackupPath(p, "regedit_delkey");
    LONG r = RegDelTree(HKEY_LOCAL_MACHINE, p);
    if (r != ERROR_SUCCESS) { MessageBoxW(R.wnd, W(ErrMsg((DWORD)r)).c_str(), L"Ошибка", MB_OK | MB_ICONERROR); return; }
    AuditChange("REG DELETE KEY HKLM\\" + p + " (копия .reg в папке Backups)");
    RefreshTree();
}

void ExportKey() {
    std::string p = SelPath();
    if (p.empty()) return;
    wchar_t file[MAX_PATH] = L"export.reg";
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o; o.hwndOwner = R.wnd; o.lpstrFilter = L"Файл реестра (*.reg)\0*.reg\0"; o.lpstrFile = file; o.nMaxFile = MAX_PATH; o.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&o)) return;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, W(p).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    std::string reg;
    ExportRegKey(k, "HKEY_LOCAL_MACHINE\\" + p, reg);
    RegCloseKey(k);
    WriteText(U(file), reg);
    Logger::I().log(Lvl::Ok, "Экспорт реестра: " + U(file));
}

void CopyPath() {
    std::wstring p = GetText(R.path);
    if (p.empty() || !OpenClipboard(R.wnd)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (p.size() + 1) * 2);
    if (g) { memcpy(GlobalLock(g), p.c_str(), (p.size() + 1) * 2); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
    CloseClipboard();
}

void Layout() {
    RECT rc;
    GetClientRect(R.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8);
    int top = m;
    Place(GetDlgItem(R.wnd, ID_NOTE + 100), m, top + S(3), S(100), S(20));
    Place(R.combo, m + S(105), top, W_ - 2 * m - S(105) - S(480), S(300));
    Place(R.bLoad, W_ - m - S(470), top - 1, S(150), S(26));
    Place(R.bUnload, W_ - m - S(315), top - 1, S(150), S(26));
    Place(R.bRegedit, W_ - m - S(160), top - 1, S(160), S(26));
    top += S(32);
    Place(R.note, m, top, W_ - 2 * m, S(18));
    top += S(24);
    int treeW = std::max(S(260), W_ * 36 / 100);
    int btnH = S(30);
    int bodyH = H - top - btnH - 2 * m - S(24);
    Place(R.tree, m, top, treeW, bodyH);
    Place(R.path, m + treeW + m, top, W_ - treeW - 3 * m, S(20));
    Place(R.list, m + treeW + m, top + S(24), W_ - treeW - 3 * m, bodyH - S(24));
    int y = H - btnH - m, x = m, bw = std::max(S(120), (W_ - 2 * m - 7 * S(6)) / 8);
    for (int i = 0; i < 8; ++i) { Place(R.btn[i], x, y, bw, btnH); x += bw + S(6); }
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (R.btn[7]) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(900); mm->ptMinTrackSize.y = S(520); return 0; }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_LOAD: LoadHives(); break;
                case ID_UNLOAD: TreeView_DeleteAllItems(R.tree); R.paths.clear(); ListView_DeleteAllItems(R.list); R.vals.clear(); Hives().unloadAll(); R.labels.clear(); break;
                case ID_COPY + 50: DestroyWindow(h); break;
                case ID_REGEDIT: ShellExecuteW(h, L"open", L"regedit.exe", nullptr, nullptr, SW_SHOWNORMAL); break;
                case ID_NEWVAL: SetValue(true); break;
                case ID_EDIT: SetValue(false); break;
                case ID_DELVAL: DelValue(); break;
                case ID_NEWKEY: NewKey(); break;
                case ID_DELKEY: DelKey(); break;
                case ID_EXPORT: ExportKey(); break;
                case ID_COPY: CopyPath(); break;
            }
            return 0;
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->hwndFrom == R.tree) {
                if (nh->code == TVN_ITEMEXPANDINGW) {
                    auto* tv = (NMTREEVIEWW*)lp;
                    if (tv->action == TVE_EXPAND && !(tv->itemNew.state & TVIS_EXPANDEDONCE)) Expand(tv->itemNew.hItem, *(std::string*)tv->itemNew.lParam);
                } else if (nh->code == TVN_SELCHANGEDW) FillValues();
            } else if (nh->hwndFrom == R.list && nh->code == NM_DBLCLK) SetValue(false);
            return 0;
        }
        case WM_DESTROY:
            if (R.pinned) { Hives().unpin(); R.pinned = false; }
            R.wnd = nullptr;
            R.paths.clear();
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void OpenRegistryEditorImpl() {
    if (R.wnd) { ShowWindow(R.wnd, SW_RESTORE); SetForegroundWindow(R.wnd); return; }
    RegisterOnce(L"WRRegistry", Proc);
    R = State();
    R.wnd = CreateWindowExW(0, L"WRRegistry", L"Реестр офлайн-Windows — выбор пути Windows, редактирование кустов", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1100), S(700),
                            g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = R.wnd;
    Mk(h, L"STATIC", L"Путь Windows:", SS_LEFT, ID_NOTE + 100);
    R.combo = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, ID_COMBO);
    R.targets = FillTargetCombo(R.combo, true);
    if (!R.targets.empty()) SetWindowTextW(R.combo, W(R.targets[0].label).c_str());
    R.bLoad = Mk(h, L"BUTTON", L"Загрузить кусты", BS_PUSHBUTTON | WS_TABSTOP, ID_LOAD);
    R.bUnload = Mk(h, L"BUTTON", L"Выгрузить кусты", BS_PUSHBUTTON | WS_TABSTOP, ID_UNLOAD);
    R.bRegedit = Mk(h, L"BUTTON", L"regedit (текущая ОС)", BS_PUSHBUTTON | WS_TABSTOP, ID_REGEDIT);
    R.note = Mk(h, L"STATIC", L"Введите путь вида D:\\Windows или выберите из списка, затем «Загрузить кусты». Перед загрузкой копия каждого куста сохраняется в Data\\Backups.", SS_LEFT, ID_NOTE);
    R.tree = Mk(h, WC_TREEVIEWW, L"", TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | WS_TABSTOP, ID_TREE, WS_EX_CLIENTEDGE);
    R.path = Mk(h, L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, ID_PATH, WS_EX_CLIENTEDGE);
    R.list = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(R.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const wchar_t* cols[] = {L"Имя", L"Тип", L"Данные"};
    int cw[] = {S(220), S(120), S(500)};
    for (int i = 0; i < 3; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = cw[i]; ListView_InsertColumn(R.list, i, &c); }
    const wchar_t* bt[] = {L"Новое значение", L"Изменить…", L"Удалить значение", L"Новый раздел", L"Удалить раздел", L"Экспорт .reg…", L"Копировать путь", L"Закрыть"};
    int ids[] = {ID_NEWVAL, ID_EDIT, ID_DELVAL, ID_NEWKEY, ID_DELKEY, ID_EXPORT, ID_COPY, IDCANCEL};
    for (int i = 0; i < 8; ++i) R.btn[i] = Mk(h, L"BUTTON", bt[i], BS_PUSHBUTTON | WS_TABSTOP, ids[i]);
    SetWindowLongPtrW(R.btn[7], GWLP_ID, ID_COPY + 50);
    Layout();
    ShowWindow(h, SW_SHOW);
}

}  // namespace ui
}  // namespace wr
