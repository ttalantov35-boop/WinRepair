// Offline registry editor: choose a Windows folder, load its hives, browse and edit.
#include "ui_tools_common.h"
#include "win/offreg.h"
#include <shellapi.h>
#include <algorithm>
#include <map>
#include <set>
#include <cstring>
#include <commdlg.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace wr {
namespace ui {

namespace {

enum { ID_COMBO = 4001, ID_LOAD, ID_UNLOAD, ID_REGEDIT, ID_TREE, ID_LIST, ID_PATH, ID_NEWVAL, ID_EDIT, ID_DELVAL, ID_NEWKEY, ID_DELKEY, ID_EXPORT, ID_COPY, ID_NOTE };
enum { ID_SEARCH = 4200, ID_FIND, ID_FKEYS, ID_FNAMES, ID_FDATA, ID_FSEL, ID_SLBL, ID_FLIST, ID_FSTOP, ID_FCLOSE, ID_FSTATUS, ID_ADDR, ID_STATUSBAR };
enum { IDM_EXIT = ID_COPY + 50, IDM_FINDBOX = 4300, IDM_REFRESH, IDM_RENVAL, IDM_RENKEY, IDM_FAVADD, IDM_FAVDEL, IDM_STATUS, IDM_CPYNAME, IDM_LOADFILE, IDM_UNLOADSEL, IDM_UNLOADALL, IDM_FAV0 = 4400, IDM_LOADT0 = 4500, IDM_UNLOADT0 = 4520 };

struct State {
    HWND wnd = nullptr, combo, tree, list, path, note, btn[8], bLoad, bUnload, bRegedit, search = nullptr, bFind = nullptr, lblSearch = nullptr, chk[4] = {}, status = nullptr;
    bool showStatus = true;
    std::vector<std::string> favs;
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
    std::string p = tv.lParam ? *(std::string*)tv.lParam : "";
    return p.rfind("@@", 0) == 0 ? "" : p;   // "@@D" = the node of a whole Windows installation, not a registry key
}

// ---- how a path is shown to the user: D:\SYSTEM\ControlSet001 instead of HKLM\WRO_D_SYSTEM\ControlSet001 -------------------
std::string MountOf(const std::string& full) { size_t k = full.find('\\'); return k == std::string::npos ? full : full.substr(0, k); }
std::string Display(const std::string& full) {
    HiveInfo hi;
    std::string m = MountOf(full);
    if (!Hives().info(m, hi)) return "HKLM\\" + full;
    std::string rest = full.size() > m.size() ? full.substr(m.size()) : "";
    return (hi.volume == "*" ? "[файл] " : hi.volume + ":\\") + hi.label + rest;
}
// address typed by the user -> real path under HKLM ("" if it cannot be understood)
std::string ParseAddress(std::string a) {
    a = trim(a);
    auto starts = [&](const std::string& x, const std::string& pre) { return x.size() >= pre.size() && toLowerUtf8(x.substr(0, pre.size())) == toLowerUtf8(pre); };
    for (const char* pre : {"Компьютер\\", "Computer\\"}) if (starts(a, pre)) a = a.substr(strlen(pre));
    for (const char* pre : {"HKEY_LOCAL_MACHINE\\", "HKLM\\"}) if (starts(a, pre)) { a = a.substr(strlen(pre)); break; }
    while (!a.empty() && a.back() == '\\') a.pop_back();
    if (a.rfind("WRO_", 0) == 0) return a;
    if (a.size() >= 3 && a[1] == ':' && a[2] == '\\') {
        std::string vol(1, (char)toupper((unsigned char)a[0])), rest = a.substr(3);
        for (const auto& hi : Hives().loadedInfo()) {
            if (hi.volume != vol) continue;
            if (starts(rest, hi.label) && (rest.size() == hi.label.size() || rest[hi.label.size()] == '\\')) return hi.mount + rest.substr(hi.label.size());
        }
    }
    return "";
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
    if (full.rfind("@@", 0) == 0) return;   // group node: children are the hives, already added
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
    if (GetFocus() != R.path) SetWindowTextW(R.path, p.empty() ? L"" : W(Display(p)).c_str());
    if (R.status) SetWindowTextW(R.status, p.empty() ? L"" : (TrW(L"  Компьютер\\") + W(Display(p))).c_str());
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
    std::string f;
    HiveInfo hi;
    std::string root = UiCurrentTarget().root;
    if (Hives().info(MountOf(p), hi) && hi.file.size() >= 3) root = hi.file.substr(0, 3);   // the backup goes to the volume the key really belongs to
    if (BackupRegText(root, tag, reg, f)) Logger::I().log(Lvl::Info, "Резервная копия ветки реестра: " + f);
    else Logger::I().log(Lvl::Info, "Копия ветки уже сохранена в этом сеансе (до первого изменения): " + f);
}

std::string TargetLabel(const std::string& vol) {
    for (const auto& t : R.targets) if (!t.root.empty() && (char)toupper((unsigned char)t.root[0]) == vol[0]) return t.label;
    return "";
}

void RefreshTree() {
    TreeView_DeleteAllItems(R.tree);
    R.paths.clear();
    std::map<std::string, std::vector<HiveInfo>> by;
    for (const auto& hi : Hives().loadedInfo()) by[hi.volume].push_back(hi);
    for (auto& g : by) {
        std::sort(g.second.begin(), g.second.end(), [](const HiveInfo& x, const HiveInfo& y) { return x.user != y.user ? !x.user : x.label < y.label; });
        std::string text = g.first == "*" ? "Кусты из файлов" : g.first + ":\\  " + TargetLabel(g.first) + "  [офлайн]";
        HTREEITEM grp = AddNode(TVI_ROOT, text, "@@" + g.first, true);
        for (const auto& hi : g.second) AddNode(grp, hi.label, hi.mount, HasSubKeys(hi.mount));
        TreeView_Expand(R.tree, grp, TVE_EXPAND);
    }
    FillValues();
    size_t vols = by.size();
    SetWindowTextW(R.note, vols ? (TrW(L"Загружено систем: ") + std::to_wstring(vols) + TrW(L".  Файл → «Загрузить офлайн-реестр диска» добавляет ещё одну систему; «Выгрузить диск» убирает.  Копия каждого куста сохраняется в Data\\Backups перед загрузкой.")).c_str()
                               : TrW(L"Ни одна система не загружена. Откройте меню Файл → «Загрузить офлайн-реестр диска» и выберите диск (например D:).").c_str());
}

bool LoadTarget(const Target& t) {
    if (t.online) { MessageBoxW(R.wnd, TrW(L"Это работающая система: её кусты открыты ядром. Для неё используйте regedit (меню Файл).").c_str(), TrW(L"Реестр").c_str(), MB_OK | MB_ICONINFORMATION); return false; }
    if (!PathExists(t.winDir + "\\System32\\config\\SYSTEM")) { MessageBoxW(R.wnd, TrW(L"В папке Windows этого диска нет System32\\config\\SYSTEM.").c_str(), TrW(L"Реестр").c_str(), MB_OK | MB_ICONWARNING); return false; }
    if (!R.pinned) { Hives().pin(); R.pinned = true; }
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    int ok = 0, bad = 0;
    for (const auto& h : ListHives(t, true)) {
        std::string err;
        if (Hives().load(h, err)) { R.labels[h.mount] = h.label; ++ok; }
        else { ++bad; Logger::I().log(Lvl::Warn, "Куст не загружен: " + err); }
    }
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    RefreshTree();
    Logger::I().log(bad ? Lvl::Warn : Lvl::Ok, fmt("Реестр %s: загружено кустов %d, не загружено %d", t.root.c_str(), ok, bad));
    return ok > 0;
}

void LoadFromFile() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o; o.hwndOwner = R.wnd; o.lpstrFilter = L"Куст реестра (SYSTEM, SOFTWARE, NTUSER.DAT, *.hiv, *.dat)\0*\0"; o.lpstrFile = file; o.nMaxFile = MAX_PATH;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&o)) return;
    if (!R.pinned) { Hives().pin(); R.pinned = true; }
    HiveInfo hi;
    std::string err;
    if (!LoadHiveFile(U(file), hi, err)) { MessageBoxW(R.wnd, W(err).c_str(), TrW(L"Реестр").c_str(), MB_OK | MB_ICONWARNING); return; }
    RefreshTree();
}

void UnloadVolume(const std::string& vol) {
    Hives().unloadVolume(vol);
    RefreshTree();
    for (const auto& hi : Hives().loadedInfo())
        if (hi.volume == vol) { MessageBoxW(R.wnd, TrW(L"Часть кустов не выгрузилась: их удерживает другая программа или окно. Подробности — в журнале.").c_str(), TrW(L"Реестр").c_str(), MB_OK | MB_ICONWARNING); return; }
}

void UnloadSelectedHive() {
    std::string p = SelPath();
    if (p.empty() || p.find('\\') != std::string::npos) { MessageBoxW(R.wnd, TrW(L"Выберите корень куста (например SYSTEM) в дереве.").c_str(), TrW(L"Реестр").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    Hives().unload(p);
    RefreshTree();
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
    std::wstring q = L"Раздел: " + W(Display(p)) + L"\nЗначение: " + (name.empty() ? L"(По умолчанию)" : W(name)) + L"\nБыло: " + (isNew ? L"(нет)" : W(RegValueText(cur, 200))) + L"\nСтанет: " + W(RegValueText(nv, 200));
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
    if (!ConfirmChange(L"Удалить значение «" + (v.name.empty() ? std::wstring(L"(По умолчанию)") : W(v.name)) + L"»\nв " + W(Display(p)) + L"\nТекущее: " + W(RegValueText(v, 200)))) return;
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
    if (!ConfirmChange(L"Создать раздел «" + W(n) + L"» в " + W(Display(p)))) return;
    LONG r = RegMakeKey(HKEY_LOCAL_MACHINE, p + "\\" + n);
    if (r != ERROR_SUCCESS) { MessageBoxW(R.wnd, W(ErrMsg((DWORD)r)).c_str(), L"Ошибка", MB_OK | MB_ICONERROR); return; }
    AuditChange("REG CREATE KEY HKLM\\" + p + "\\" + n);
    RefreshTree();
}

void DelKey() {
    std::string p = SelPath();
    if (p.empty()) return;
    if (p.find('\\') == std::string::npos) { MessageBoxW(R.wnd, L"Корень куста удалить нельзя.", L"Реестр", MB_OK | MB_ICONWARNING); return; }
    if (!ConfirmChange(L"УДАЛИТЬ раздел со всеми вложенными разделами и значениями:\n" + W(Display(p)))) return;
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
    // system selection lives in the File menu (as in other registry tools); the old combo/buttons stay hidden
    for (HWND hw : {GetDlgItem(R.wnd, ID_NOTE + 100), R.combo, R.bLoad, R.bUnload, R.bRegedit}) ShowWindow(hw, SW_HIDE);
    Place(R.note, m, top, W_ - 2 * m, S(18));
    top += S(24);
    {   // search row: text + options (regedit-style Ctrl+F, but with results list)
        int bx = W_ - m - S(120);
        Place(R.lblSearch, m, top + S(4), S(70), S(20));
        Place(R.search, m + S(75), top, std::max(S(120), bx - (m + S(75)) - S(8)), S(26));
        Place(R.bFind, bx, top - 1, S(120), S(28));
        top += S(32);
        int cw = std::max(S(120), (W_ - 2 * m - S(75)) / 4);
        for (int i = 0; i < 4; ++i) Place(R.chk[i], m + S(75) + i * cw, top, cw - S(6), S(22));
        top += S(28);
    }
    int treeW = std::max(S(260), W_ * 36 / 100);
    int statusH = R.showStatus ? S(22) : 0;
    int bodyH = H - top - statusH - m;
    Place(R.tree, m, top, treeW, bodyH);
    Place(R.path, m + treeW + m, top, W_ - treeW - 3 * m, S(24));
    Place(R.list, m + treeW + m, top + S(28), W_ - treeW - 3 * m, bodyH - S(28));
    ShowWindow(R.status, R.showStatus ? SW_SHOW : SW_HIDE);
    Place(R.status, 0, H - statusH, W_, statusH);
    for (int i = 0; i < 8; ++i) ShowWindow(R.btn[i], SW_HIDE);   // commands live in the menu bar, the context menus and the keyboard, as in regedit
}

// ---- search --------------------------------------------------------------------------------------------------
std::string ItemPath(HTREEITEM it) {
    TVITEMW tv{};
    tv.mask = TVIF_PARAM;
    tv.hItem = it;
    TreeView_GetItem(R.tree, &tv);
    return tv.lParam ? *(std::string*)tv.lParam : "";
}

// Open the key in the tree (expanding the branch on the way) and select a value in the right-hand list.
void NavigateTo(const std::string& full, const std::string& valueName) {
    if (!R.wnd || full.empty()) return;
    std::vector<std::string> parts;
    for (size_t a = 0;;) {
        size_t b = full.find('\\', a);
        parts.push_back(full.substr(a, b == std::string::npos ? b : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    std::string acc = parts[0];
    HTREEITEM it = nullptr;
    for (HTREEITEM g = TreeView_GetRoot(R.tree); g && !it; g = TreeView_GetNextSibling(R.tree, g)) {
        TreeView_Expand(R.tree, g, TVE_EXPAND);
        for (HTREEITEM c = TreeView_GetChild(R.tree, g); c; c = TreeView_GetNextSibling(R.tree, c))
            if (ItemPath(c) == acc) { it = c; break; }
    }
    if (!it) return;
    for (size_t i = 1; i < parts.size(); ++i) {
        TreeView_Expand(R.tree, it, TVE_EXPAND);   // children are loaded lazily by TVN_ITEMEXPANDING
        acc += "\\" + parts[i];
        HTREEITEM c = TreeView_GetChild(R.tree, it);
        while (c && ItemPath(c) != acc) c = TreeView_GetNextSibling(R.tree, c);
        if (!c) break;
        it = c;
    }
    TreeView_SelectItem(R.tree, it);
    TreeView_EnsureVisible(R.tree, it);
    if (!valueName.empty()) {
        for (size_t i = 0; i < R.vals.size(); ++i)
            if (R.vals[i].name == valueName) {
                ListView_SetItemState(R.list, (int)i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(R.list, (int)i, FALSE);
                break;
            }
    }
    SetForegroundWindow(R.wnd);
}

struct Hit { std::string path, name, data; int what; };   // what: 0 key name, 1 value name, 2 value data
struct FindState {
    HWND wnd = nullptr, list = nullptr, status = nullptr, bStop = nullptr;
    std::vector<Hit> hits;
    std::mutex mu;
    size_t shown = 0;
    std::atomic<bool> cancel{false}, done{false};
    std::atomic<unsigned> scanned{0};
    std::thread th;
    std::string q;   // lower-case
    bool inKeys = true, inNames = true, inData = true;
    std::vector<std::string> roots;
};
FindState* F = nullptr;
constexpr size_t kMaxHits = 5000;

bool Has(const std::string& hay, const std::string& needleLower) { return toLowerUtf8(hay).find(needleLower) != std::string::npos; }

void SearchKey(FindState* f, const std::string& path, int depth) {
    if (f->cancel || depth > 80) return;
    {
        std::lock_guard<std::mutex> lk(f->mu);
        if (f->hits.size() >= kMaxHits) return;
    }
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, W(path).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    std::vector<std::string> subs;
    std::vector<RegVal> vals;
    EnumSubKeys(k, subs);
    if (f->inNames || f->inData) EnumValues(k, vals);
    RegCloseKey(k);
    ++f->scanned;
    auto add = [&](Hit h) { std::lock_guard<std::mutex> lk(f->mu); f->hits.push_back(std::move(h)); };
    if (f->inKeys && depth > 0) {
        size_t sl = path.rfind('\\');
        if (Has(sl == std::string::npos ? path : path.substr(sl + 1), f->q)) add({path, "", "", 0});
    }
    for (const auto& v : vals) {
        if (f->inNames && Has(v.name, f->q)) { add({path, v.name, RegValueText(v, 300), 1}); continue; }
        if (f->inData) {
            std::string d = RegValueText(v, 4096);
            if (Has(d, f->q)) add({path, v.name, d.size() > 300 ? d.substr(0, 300) : d, 2});
        }
    }
    for (const auto& sname : subs) {
        if (f->cancel) return;
        SearchKey(f, path + "\\" + sname, depth + 1);
    }
}

void FindPump() {
    if (!F) return;
    std::vector<Hit> fresh;
    {
        std::lock_guard<std::mutex> lk(F->mu);
        fresh.assign(F->hits.begin() + (long)F->shown, F->hits.end());
        F->shown = F->hits.size();
    }
    static const wchar_t* kWhat[] = {L"Раздел", L"Имя значения", L"Данные"};
    int row = ListView_GetItemCount(F->list);
    SendMessageW(F->list, WM_SETREDRAW, FALSE, 0);
    for (const auto& h : fresh) {
        std::wstring a = TrW(kWhat[h.what]), b = W(Display(h.path)), c = h.name.empty() && h.what ? TrW(L"(По умолчанию)") : W(h.name), d = W(h.data);
        LVITEMW li{};
        li.mask = LVIF_TEXT;
        li.iItem = row;
        li.pszText = (LPWSTR)a.c_str();
        SendMessageW(F->list, LVM_INSERTITEMW, 0, (LPARAM)&li);
        li.iSubItem = 1; li.pszText = (LPWSTR)b.c_str(); SendMessageW(F->list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&li);
        li.iSubItem = 2; li.pszText = (LPWSTR)c.c_str(); SendMessageW(F->list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&li);
        li.iSubItem = 3; li.pszText = (LPWSTR)d.c_str(); SendMessageW(F->list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&li);
        ++row;
    }
    SendMessageW(F->list, WM_SETREDRAW, TRUE, 0);
    bool fin = F->done;
    std::wstring st = (fin ? (F->cancel ? TrW(L"Остановлено. ") : TrW(L"Готово. ")) : TrW(L"Идёт поиск… ")) + TrW(L"Найдено: ") + std::to_wstring(row) +
                      TrW(L".  Просмотрено разделов: ") + std::to_wstring(F->scanned.load());
    if (row >= (int)kMaxHits) st += TrW(L".  Достигнут предел списка — уточните запрос.");
    SetWindowTextW(F->status, st.c_str());
    if (fin) { KillTimer(F->wnd, 1); EnableWindow(F->bStop, FALSE); }
}

void FindLayout() {
    RECT rc;
    GetClientRect(F->wnd, &rc);
    int m = S(8), W_ = rc.right, H = rc.bottom;
    Place(F->status, m, m, W_ - 2 * m - S(240), S(22));
    Place(F->bStop, W_ - m - S(230), m - S(2), S(110), S(28));
    Place(GetDlgItem(F->wnd, ID_FCLOSE), W_ - m - S(114), m - S(2), S(114), S(28));
    Place(F->list, m, m + S(32), W_ - 2 * m, H - 2 * m - S(32));
}

LRESULT CALLBACK FindProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (F && F->list) FindLayout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(640); mm->ptMinTrackSize.y = S(320); return 0; }
        case WM_TIMER: FindPump(); return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == ID_FSTOP && F) F->cancel = true;
            else if (LOWORD(wp) == ID_FCLOSE) DestroyWindow(h);
            return 0;
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (F && nh->hwndFrom == F->list && (nh->code == NM_DBLCLK || nh->code == LVN_ITEMACTIVATE)) {
                int sel = ListView_GetNextItem(F->list, -1, LVNI_SELECTED);
                Hit hit;
                {
                    std::lock_guard<std::mutex> lk(F->mu);
                    if (sel < 0 || sel >= (int)F->hits.size()) return 0;
                    hit = F->hits[(size_t)sel];
                }
                NavigateTo(hit.path, hit.what ? hit.name : "");
            }
            return 0;
        }
        case WM_DESTROY:
            if (F) {
                F->cancel = true;
                if (F->th.joinable()) F->th.join();
                KillTimer(h, 1);
                delete F;
                F = nullptr;
            }
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void DoFind() {
    std::string q = trim(U(GetText(R.search)));
    if (q.empty()) { MessageBoxW(R.wnd, TrW(L"Введите, что искать.").c_str(), TrW(L"Поиск в реестре").c_str(), MB_OK | MB_ICONINFORMATION); SetFocus(R.search); return; }
    if (Hives().mounts().empty()) { MessageBoxW(R.wnd, TrW(L"Сначала загрузите кусты офлайн-Windows (меню Файл → «Загрузить офлайн-реестр диска»).").c_str(), TrW(L"Поиск в реестре").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    bool k = SendMessageW(R.chk[0], BM_GETCHECK, 0, 0) == BST_CHECKED, n = SendMessageW(R.chk[1], BM_GETCHECK, 0, 0) == BST_CHECKED,
         d = SendMessageW(R.chk[2], BM_GETCHECK, 0, 0) == BST_CHECKED, onlySel = SendMessageW(R.chk[3], BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!k && !n && !d) { MessageBoxW(R.wnd, TrW(L"Отметьте, где искать: имена разделов, имена значений или данные.").c_str(), TrW(L"Поиск в реестре").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    if (F) DestroyWindow(F->wnd);   // a new search replaces the previous results window
    RegisterOnce(L"WRRegFind", FindProc);
    F = new FindState();
    F->q = toLowerUtf8(q);
    F->inKeys = k; F->inNames = n; F->inData = d;
    std::string sel = SelPath();
    if (onlySel && !sel.empty()) F->roots = {sel};
    else for (const auto& mt : Hives().mounts()) F->roots.push_back(mt);
    std::wstring title = TrW(L"Результаты поиска в реестре: ") + W(q);
    F->wnd = CreateWindowExW(0, L"WRRegFind", title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(980), S(520), R.wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = F->wnd;
    F->status = Mk(h, L"STATIC", L"", SS_LEFT, ID_FSTATUS);
    F->bStop = Mk(h, L"BUTTON", L"Остановить", BS_PUSHBUTTON | WS_TABSTOP, ID_FSTOP);
    Mk(h, L"BUTTON", L"Закрыть", BS_PUSHBUTTON | WS_TABSTOP, ID_FCLOSE);
    F->list = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_FLIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(F->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[] = {L"Найдено в", L"Раздел", L"Имя значения", L"Данные"};
    int cw[] = {S(110), S(380), S(160), S(280)};
    for (int i = 0; i < 4; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = cw[i]; ListView_InsertColumn(F->list, i, &c); }
    FindLayout();
    ShowWindow(h, SW_SHOW);
    SetTimer(h, 1, 250, nullptr);
    FindState* f = F;
    f->th = std::thread([f]() {
        for (const auto& r : f->roots) { if (f->cancel) break; SearchKey(f, r, 0); }
        f->done = true;
    });
}

LRESULT CALLBACK SearchEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && wp == VK_RETURN) { PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(ID_FIND, BN_CLICKED), (LPARAM)GetDlgItem(GetParent(h), ID_FIND)); return 0; }
    if (m == WM_CHAR && wp == VK_RETURN) return 0;   // no beep
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, SearchEditProc, 1);
    return DefSubclassProc(h, m, wp, lp);
}

// ---- right-click menus ---------------------------------------------------------------------------------------
// returns true when handled (the generic window menu is then skipped)
bool RegContextMenu(HWND src, LPARAM lp) {
    if (src != R.tree && src != R.list) return false;
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    bool kbd = lp == (LPARAM)-1;
    if (kbd) { RECT r; GetWindowRect(src, &r); pt = {r.left + S(20), r.top + S(20)}; }
    else {
        POINT c = pt;
        ScreenToClient(src, &c);
        if (src == R.tree) {
            TVHITTESTINFO hi{};
            hi.pt = c;
            HTREEITEM it = TreeView_HitTest(R.tree, &hi);
            if (it) TreeView_SelectItem(R.tree, it);
        } else {
            LVHITTESTINFO hi{};
            hi.pt = c;
            int i = ListView_HitTest(R.list, &hi);
            if (i >= 0) ListView_SetItemState(R.list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
    enum { M_NEWV = 1, M_EDIT, M_DELV, M_COPYN, M_COPYD, M_COPYP, M_NEWK, M_DELK, M_EXP, M_EXPAND, M_FINDHERE, M_REFRESH };
    bool hasKey = !SelPath().empty();
    HMENU m = CreatePopupMenu();
    if (src == R.list) {
        bool hasVal = SelVal() >= 0;
        AppendMenuW(m, MF_STRING | (hasVal ? 0 : MF_GRAYED), M_EDIT, TrW(L"Изменить…").c_str());
        AppendMenuW(m, MF_STRING | (hasKey ? 0 : MF_GRAYED), M_NEWV, TrW(L"Новое значение").c_str());
        AppendMenuW(m, MF_STRING | (hasVal ? 0 : MF_GRAYED), M_DELV, TrW(L"Удалить значение").c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (hasVal ? 0 : MF_GRAYED), M_COPYN, TrW(L"Копировать имя значения").c_str());
        AppendMenuW(m, MF_STRING | (hasVal ? 0 : MF_GRAYED), M_COPYD, TrW(L"Копировать данные").c_str());
        AppendMenuW(m, MF_STRING | (hasKey ? 0 : MF_GRAYED), M_COPYP, TrW(L"Копировать путь раздела").c_str());
    } else {
        AppendMenuW(m, MF_STRING | (hasKey ? 0 : MF_GRAYED), M_EXPAND, TrW(L"Развернуть / свернуть").c_str());
        AppendMenuW(m, MF_STRING | (hasKey ? 0 : MF_GRAYED), M_FINDHERE, TrW(L"Найти в этом разделе…").c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (hasKey ? 0 : MF_GRAYED), M_NEWK, TrW(L"Новый раздел").c_str());
        AppendMenuW(m, MF_STRING | (hasKey ? 0 : MF_GRAYED), M_DELK, TrW(L"Удалить раздел").c_str());
        AppendMenuW(m, MF_STRING | (hasKey ? 0 : MF_GRAYED), M_EXP, TrW(L"Экспорт .reg…").c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (hasKey ? 0 : MF_GRAYED), M_COPYP, TrW(L"Копировать путь").c_str());
        AppendMenuW(m, MF_STRING, M_REFRESH, TrW(L"Обновить дерево").c_str());
    }
    SetForegroundWindow(R.wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, R.wnd, nullptr);
    DestroyMenu(m);
    int si = SelVal();
    switch (c) {
        case M_NEWV: SetValue(true); break;
        case M_EDIT: SetValue(false); break;
        case M_DELV: DelValue(); break;
        case M_COPYN: if (si >= 0 && si < (int)R.vals.size()) ClipText(R.wnd, W(R.vals[(size_t)si].name)); break;
        case M_COPYD: if (si >= 0 && si < (int)R.vals.size()) ClipText(R.wnd, W(RegValueText(R.vals[(size_t)si], 100000))); break;
        case M_COPYP: CopyPath(); break;
        case M_NEWK: NewKey(); break;
        case M_DELK: DelKey(); break;
        case M_EXP: ExportKey(); break;
        case M_EXPAND: { HTREEITEM it = TreeView_GetSelection(R.tree); if (it) TreeView_Expand(R.tree, it, TVE_TOGGLE); break; }
        case M_FINDHERE: SendMessageW(R.chk[3], BM_SETCHECK, BST_CHECKED, 0); SetFocus(R.search); SendMessageW(R.search, EM_SETSEL, 0, -1); break;
        case M_REFRESH: RefreshTree(); break;
    }
    return true;
}


// ---- regedit-style extras: menu bar, address bar, rename, favourites, keys ----------------------------------
std::string FavFile() { return DataDir() + "registry_favorites.txt"; }
void LoadFavs() {
    R.favs.clear();
    std::vector<uint8_t> b;
    if (!ReadBytes(FavFile(), b, 1 << 20)) return;
    std::string t(b.begin(), b.end());
    if (t.size() >= 3 && (uint8_t)t[0] == 0xEF) t.erase(0, 3);
    size_t a = 0;
    while (a < t.size()) {
        size_t e = t.find('\n', a);
        std::string l = trim(t.substr(a, e == std::string::npos ? std::string::npos : e - a));
        if (!l.empty()) R.favs.push_back(l);
        if (e == std::string::npos) break;
        a = e + 1;
    }
}
void SaveFavs() {
    std::string t;
    for (const auto& f : R.favs) t += f + "\r\n";
    WriteText(FavFile(), t);
}

HMENU BuildMenuBar() {
    HMENU bar = CreateMenu();
    auto add = [](HMENU m, UINT id, const wchar_t* t, UINT fl = 0) { AppendMenuW(m, MF_STRING | fl, id, TrW(t).c_str()); };
    HMENU f = CreatePopupMenu();
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)f, TrW(L"Файл").c_str());   // filled in WM_INITMENUPOPUP
    HMENU e = CreatePopupMenu();
    add(e, ID_EDIT, L"Изменить…\tEnter");
    HMENU nw = CreatePopupMenu();
    add(nw, ID_NEWKEY, L"Раздел");
    add(nw, ID_NEWVAL, L"Значение…");
    AppendMenuW(e, MF_POPUP, (UINT_PTR)nw, TrW(L"Создать").c_str());
    add(e, ID_DELKEY, L"Удалить раздел");
    add(e, ID_DELVAL, L"Удалить значение\tDel");
    add(e, IDM_RENVAL, L"Переименовать значение\tF2");
    add(e, IDM_RENKEY, L"Переименовать раздел");
    AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
    add(e, IDM_CPYNAME, L"Копировать имя раздела");
    AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
    add(e, IDM_FINDBOX, L"Найти…\tCtrl+F");
    add(e, ID_FIND, L"Найти далее\tF3");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)e, TrW(L"Правка").c_str());
    HMENU v = CreatePopupMenu();
    add(v, IDM_STATUS, L"Строка состояния", MF_CHECKED);
    add(v, IDM_REFRESH, L"Обновить\tF5");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)v, TrW(L"Вид").c_str());
    HMENU fv = CreatePopupMenu();   // rebuilt in WM_INITMENUPOPUP
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)fv, TrW(L"Избранное").c_str());
    return bar;
}

void RebuildFileMenu(HMENU m) {
    while (GetMenuItemCount(m) > 0) DeleteMenu(m, 0, MF_BYPOSITION);
    auto add = [&](HMENU mm, UINT id, const std::wstring& t, UINT fl = 0) { AppendMenuW(mm, MF_STRING | fl, id, t.c_str()); };
    std::set<std::string> loaded;
    for (const auto& hi : Hives().loadedInfo()) loaded.insert(hi.volume);
    HMENU ld = CreatePopupMenu(), ul = CreatePopupMenu();
    for (size_t i = 0; i < R.targets.size() && i < 20; ++i) {
        const auto& t = R.targets[i];
        std::string v(1, (char)toupper((unsigned char)t.root[0]));
        bool on = loaded.count(v) > 0;
        add(ld, (UINT)(IDM_LOADT0 + i), W(t.label) + (on ? TrW(L"   (загружен — обновить)") : L""), on ? MF_CHECKED : 0);
        if (on) add(ul, (UINT)(IDM_UNLOADT0 + i), W(t.label));
    }
    if (R.targets.empty()) add(ld, 0, TrW(L"(офлайн-системы не найдены)"), MF_GRAYED);
    if (loaded.empty() || (loaded.size() == 1 && loaded.count("*"))) add(ul, 0, TrW(L"(ничего не загружено)"), MF_GRAYED);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)ld, TrW(L"Загрузить офлайн-реестр диска").c_str());
    AppendMenuW(m, MF_POPUP, (UINT_PTR)ul, TrW(L"Выгрузить диск").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(m, IDM_LOADFILE, TrW(L"Загрузить куст из файла…"));
    add(m, IDM_UNLOADSEL, TrW(L"Выгрузить выбранный куст"));
    add(m, IDM_UNLOADALL, TrW(L"Выгрузить всё"), Hives().loadedInfo().empty() ? MF_GRAYED : 0);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(m, ID_EXPORT, TrW(L"Экспорт ветки .reg…"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(m, ID_REGEDIT, TrW(L"regedit текущей ОС"));
    add(m, IDM_EXIT, TrW(L"Выход"));
}

void RebuildFavMenu(HMENU m) {
    while (GetMenuItemCount(m) > 0) DeleteMenu(m, 0, MF_BYPOSITION);
    std::string cur = SelPath();
    AppendMenuW(m, MF_STRING | (cur.empty() ? MF_GRAYED : 0), IDM_FAVADD, TrW(L"Добавить в избранное").c_str());
    AppendMenuW(m, MF_STRING | (R.favs.empty() ? MF_GRAYED : 0), IDM_FAVDEL, TrW(L"Удалить из избранного…").c_str());
    if (!R.favs.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < R.favs.size() && i < 40; ++i) {
        std::wstring t = W(R.favs[i]);
        if (t.size() > 90) t = L"…" + t.substr(t.size() - 88);
        for (size_t p = 0; (p = t.find(L'&', p)) != std::wstring::npos; p += 2) t.insert(p, 1, L'&');
        AppendMenuW(m, MF_STRING, (UINT_PTR)(IDM_FAV0 + i), t.c_str());
    }
}

void AddFavorite() {
    std::string p = SelPath();
    if (p.empty()) return;
    if (std::find(R.favs.begin(), R.favs.end(), p) == R.favs.end()) { R.favs.push_back(p); SaveFavs(); }
}
void DeleteFavorite() {
    std::vector<FormField> f(1);
    f[0].label = L"Какую запись удалить из избранного";
    f[0].kind = 2;
    for (const auto& x : R.favs) f[0].options.push_back(W(x));
    f[0].value = f[0].options.empty() ? L"" : f[0].options[0];
    if (!FormDialog(R.wnd, L"Избранное", f)) return;
    std::string v = U(f[0].value);
    R.favs.erase(std::remove(R.favs.begin(), R.favs.end(), v), R.favs.end());
    SaveFavs();
}

// address bar: HKLM\WRO_SYSTEM\... (also HKEY_LOCAL_MACHINE\, "Компьютер\" prefixes)
void GoToAddress() {
    std::string a = ParseAddress(U(GetText(R.path)));
    if (a.empty()) { MessageBoxW(R.wnd, TrW(L"Адрес не распознан. Пример: D:\\SYSTEM\\ControlSet001\\Services (диск должен быть загружен через меню Файл).").c_str(), TrW(L"Реестр").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    NavigateTo(a, "");
    SetFocus(R.tree);
}

LRESULT CALLBACK AddrProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && wp == VK_RETURN) { GoToAddress(); return 0; }
    if (m == WM_CHAR && wp == VK_RETURN) return 0;
    if (m == WM_KILLFOCUS) { std::string sp = SelPath(); SetWindowTextW(h, sp.empty() ? L"" : W(Display(sp)).c_str()); }   // unfinished text -> path of the selected key
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, AddrProc, 2);
    return DefSubclassProc(h, m, wp, lp);
}

void RenameValue() {
    std::string p = SelPath();
    int si = SelVal();
    if (p.empty() || si < 0 || si >= (int)R.vals.size()) return;
    RegVal cur = R.vals[(size_t)si];
    if (cur.name.empty()) { MessageBoxW(R.wnd, TrW(L"Значение «По умолчанию» переименовать нельзя.").c_str(), TrW(L"Реестр").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    std::vector<FormField> f(1);
    f[0].label = L"Новое имя значения";
    f[0].value = W(cur.name);
    if (!FormDialog(R.wnd, L"Переименовать значение", f)) return;
    std::string nn = U(f[0].value);
    if (nn.empty() || nn == cur.name) return;
    if (!ConfirmChange(L"Переименовать значение «" + W(cur.name) + L"» → «" + W(nn) + L"»\nв " + W(Display(p)))) return;
    BackupPath(p, "regedit");
    HKEY k = nullptr;
    LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, W(p).c_str(), 0, KEY_SET_VALUE, &k);
    if (r == ERROR_SUCCESS) { r = RegSetRaw(k, nn, cur.type, cur.data); if (r == ERROR_SUCCESS) RegDeleteValueW(k, W(cur.name).c_str()); RegFlushKey(k); RegCloseKey(k); }
    if (r != ERROR_SUCCESS) { MessageBoxW(R.wnd, W(ErrMsg((DWORD)r)).c_str(), L"Ошибка", MB_OK | MB_ICONERROR); return; }
    AuditChange("REG RENAME VALUE HKLM\\" + p + " \"" + cur.name + "\" -> \"" + nn + "\"");
    FillValues();
}

void RenameKey() {
    std::string p = SelPath();
    size_t sl = p.rfind('\\');
    if (p.empty() || sl == std::string::npos) { MessageBoxW(R.wnd, TrW(L"Корень куста переименовать нельзя.").c_str(), TrW(L"Реестр").c_str(), MB_OK | MB_ICONWARNING); return; }
    std::string parent = p.substr(0, sl), leaf = p.substr(sl + 1);
    std::vector<FormField> f(1);
    f[0].label = L"Новое имя раздела";
    f[0].value = W(leaf);
    if (!FormDialog(R.wnd, L"Переименовать раздел", f)) return;
    std::string nn = trim(U(f[0].value));
    if (nn.empty() || nn == leaf || nn.find('\\') != std::string::npos) return;
    if (!ConfirmChange(L"Переименовать раздел «" + W(leaf) + L"» → «" + W(nn) + L"»\nв " + W(Display(parent)))) return;
    BackupPath(p, "regedit_renkey");
    HKEY pk = nullptr;
    LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, W(parent).c_str(), 0, KEY_READ | KEY_WRITE, &pk);
    if (r == ERROR_SUCCESS) {
        typedef LONG(WINAPI * RenFn)(HKEY, LPCWSTR, LPCWSTR);
        RenFn fn = (RenFn)GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "RegRenameKey");
        r = fn ? fn(pk, W(leaf).c_str(), W(nn).c_str()) : ERROR_CALL_NOT_IMPLEMENTED;
        RegCloseKey(pk);
    }
    if (r != ERROR_SUCCESS) { MessageBoxW(R.wnd, W(ErrMsg((DWORD)r)).c_str(), L"Ошибка", MB_OK | MB_ICONERROR); return; }
    AuditChange("REG RENAME KEY HKLM\\" + p + " -> " + nn);
    RefreshTree();
    NavigateTo(parent + "\\" + nn, "");
}


LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_INITMENUPOPUP: {
            HMENU mp = (HMENU)wp;
            HMENU bar = GetMenu(h);
            if (bar && GetMenuItemCount(bar) == 4 && GetSubMenu(bar, 3) == mp) RebuildFavMenu(mp);
            else if (bar && GetSubMenu(bar, 0) == mp) RebuildFileMenu(mp);
            break;
        }
        case WM_CONTEXTMENU: return RegContextMenu((HWND)wp, lp) ? 1 : 0;
        case WM_SIZE: if (R.btn[7]) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(900); mm->ptMinTrackSize.y = S(520); return 0; }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_LOADFILE: LoadFromFile(); break;
                case IDM_UNLOADSEL: UnloadSelectedHive(); break;
                case IDM_UNLOADALL: Hives().unloadAll(); R.labels.clear(); RefreshTree(); break;
                case IDM_EXIT: DestroyWindow(h); break;
                case ID_REGEDIT: ShellExecuteW(h, L"open", L"regedit.exe", nullptr, nullptr, SW_SHOWNORMAL); break;
                case ID_NEWVAL: SetValue(true); break;
                case ID_EDIT: SetValue(false); break;
                case ID_DELVAL: DelValue(); break;
                case ID_NEWKEY: NewKey(); break;
                case ID_DELKEY: DelKey(); break;
                case ID_EXPORT: ExportKey(); break;
                case ID_COPY: CopyPath(); break;
                case ID_FIND: DoFind(); break;
                case IDM_FINDBOX: SetFocus(R.search); SendMessageW(R.search, EM_SETSEL, 0, -1); break;
                case IDM_REFRESH: RefreshTree(); break;
                case IDM_RENVAL: RenameValue(); break;
                case IDM_RENKEY: RenameKey(); break;
                case IDM_CPYNAME: CopyPath(); break;
                case IDM_FAVADD: AddFavorite(); break;
                case IDM_FAVDEL: DeleteFavorite(); break;
                case IDM_STATUS: R.showStatus = !R.showStatus; CheckMenuItem(GetMenu(h), IDM_STATUS, MF_BYCOMMAND | (R.showStatus ? MF_CHECKED : MF_UNCHECKED)); Layout(); break;
                default:
                    if (LOWORD(wp) >= IDM_LOADT0 && LOWORD(wp) < IDM_LOADT0 + 20 && (size_t)(LOWORD(wp) - IDM_LOADT0) < R.targets.size()) {
                        const Target& t = R.targets[(size_t)(LOWORD(wp) - IDM_LOADT0)];
                        std::string v(1, (char)toupper((unsigned char)t.root[0]));
                        Hives().unloadVolume(v);   // reload = fresh data
                        LoadTarget(t);
                        break;
                    }
                    if (LOWORD(wp) >= IDM_UNLOADT0 && LOWORD(wp) < IDM_UNLOADT0 + 20 && (size_t)(LOWORD(wp) - IDM_UNLOADT0) < R.targets.size()) {
                        UnloadVolume(std::string(1, (char)toupper((unsigned char)R.targets[(size_t)(LOWORD(wp) - IDM_UNLOADT0)].root[0])));
                        break;
                    }
                    if (LOWORD(wp) >= IDM_FAV0 && LOWORD(wp) < IDM_FAV0 + 40 && (size_t)(LOWORD(wp) - IDM_FAV0) < R.favs.size()) NavigateTo(R.favs[(size_t)(LOWORD(wp) - IDM_FAV0)], "");
                    break;
            }
            return 0;
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->hwndFrom == R.tree) {
                if (nh->code == TVN_ITEMEXPANDINGW) {
                    auto* tv = (NMTREEVIEWW*)lp;
                    if (tv->action == TVE_EXPAND && !(tv->itemNew.state & TVIS_EXPANDEDONCE)) Expand(tv->itemNew.hItem, *(std::string*)tv->itemNew.lParam);
                } else if (nh->code == TVN_SELCHANGEDW) FillValues();
                else if (nh->code == TVN_KEYDOWN) {
                    WORD k = ((NMTVKEYDOWN*)lp)->wVKey;
                    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (k == VK_F2) RenameKey();
                    else if (k == VK_DELETE) DelKey();
                    else if (k == VK_F5) RefreshTree();
                    else if (k == VK_F3) DoFind();
                    else if (ctrl && k == 'F') { SetFocus(R.search); SendMessageW(R.search, EM_SETSEL, 0, -1); }
                }
            } else if (nh->hwndFrom == R.list && nh->code == NM_DBLCLK) SetValue(false);
            else if (nh->hwndFrom == R.list && nh->code == LVN_KEYDOWN) {
                WORD k = ((NMLVKEYDOWN*)lp)->wVKey;
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (k == VK_F2) RenameValue();
                else if (k == VK_DELETE) DelValue();
                else if (k == VK_RETURN) SetValue(false);
                else if (k == VK_F5) RefreshTree();
                else if (k == VK_F3) DoFind();
                else if (ctrl && k == 'F') { SetFocus(R.search); SendMessageW(R.search, EM_SETSEL, 0, -1); }
            }
            return 0;
        }
        case WM_DESTROY:
            if (F) DestroyWindow(F->wnd);
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
    R.note = Mk(h, L"STATIC", L"", SS_LEFT, ID_NOTE);
    R.lblSearch = Mk(h, L"STATIC", L"Поиск:", SS_LEFT, ID_SLBL);
    R.search = Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_SEARCH, WS_EX_CLIENTEDGE);
    SetWindowSubclass(R.search, SearchEditProc, 1, 0);
    SendMessageW(R.search, EM_SETCUEBANNER, TRUE, (LPARAM)TrW(L"Текст для поиска (Enter — найти)").c_str());
    R.bFind = Mk(h, L"BUTTON", L"Найти", BS_PUSHBUTTON | WS_TABSTOP, ID_FIND);
    const wchar_t* cl[] = {L"Имена разделов", L"Имена значений", L"Данные значений", L"Только в выбранном разделе"};
    for (int i = 0; i < 4; ++i) { R.chk[i] = Mk(h, L"BUTTON", cl[i], BS_AUTOCHECKBOX | WS_TABSTOP, ID_FKEYS + i); if (i < 3) SendMessageW(R.chk[i], BM_SETCHECK, BST_CHECKED, 0); }
    R.tree = Mk(h, WC_TREEVIEWW, L"", TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | WS_TABSTOP, ID_TREE, WS_EX_CLIENTEDGE);
    R.path = Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_ADDR, WS_EX_CLIENTEDGE);
    SetWindowSubclass(R.path, AddrProc, 2, 0);
    SendMessageW(R.path, EM_SETCUEBANNER, TRUE, (LPARAM)TrW(L"Адрес раздела: D:\\SYSTEM\\ControlSet001\\… (Enter — перейти)").c_str());
    R.status = Mk(h, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_SUNKEN, ID_STATUSBAR);
    LoadFavs();
    SetMenu(h, BuildMenuBar());
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
    {   // the system chosen in the main window is loaded at once; others are added from File menu
        Target cur = UiCurrentTarget();
        bool done = false;
        if (!cur.online) done = LoadTarget(cur);
        if (!done) RefreshTree();
    }
}

}  // namespace ui
}  // namespace wr
