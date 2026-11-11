// Offline registry editor: choose a Windows folder, load its hives, browse and edit.
#include "ui_tools_common.h"
#include "win/offreg.h"
#include <shellapi.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace wr {
namespace ui {

namespace {

enum { ID_COMBO = 4001, ID_LOAD, ID_UNLOAD, ID_REGEDIT, ID_TREE, ID_LIST, ID_PATH, ID_NEWVAL, ID_EDIT, ID_DELVAL, ID_NEWKEY, ID_DELKEY, ID_EXPORT, ID_COPY, ID_NOTE };
enum { ID_SEARCH = 4200, ID_FIND, ID_FKEYS, ID_FNAMES, ID_FDATA, ID_FSEL, ID_SLBL, ID_FLIST, ID_FSTOP, ID_FCLOSE, ID_FSTATUS };

struct State {
    HWND wnd = nullptr, combo, tree, list, path, note, btn[8], bLoad, bUnload, bRegedit, search = nullptr, bFind = nullptr, lblSearch = nullptr, chk[4] = {};
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
    std::string f;
    if (BackupRegText(UiCurrentTarget().root, tag, reg, f)) Logger::I().log(Lvl::Info, "Резервная копия ветки реестра: " + f);
    else Logger::I().log(Lvl::Info, "Копия ветки уже сохранена в этом сеансе (до первого изменения): " + f);
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
    int btnH = S(30);
    int bodyH = H - top - btnH - 2 * m - S(24);
    Place(R.tree, m, top, treeW, bodyH);
    Place(R.path, m + treeW + m, top, W_ - treeW - 3 * m, S(20));
    Place(R.list, m + treeW + m, top + S(24), W_ - treeW - 3 * m, bodyH - S(24));
    int y = H - btnH - m, x = m, bw = std::max(S(120), (W_ - 2 * m - 7 * S(6)) / 8);
    for (int i = 0; i < 8; ++i) { Place(R.btn[i], x, y, bw, btnH); x += bw + S(6); }
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
    HTREEITEM it = TreeView_GetRoot(R.tree);
    while (it && ItemPath(it) != acc) it = TreeView_GetNextSibling(R.tree, it);
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
        std::wstring a = TrW(kWhat[h.what]), b = W("HKLM\\" + h.path), c = h.name.empty() && h.what ? TrW(L"(По умолчанию)") : W(h.name), d = W(h.data);
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
    if (Hives().mounts().empty()) { MessageBoxW(R.wnd, TrW(L"Сначала загрузите кусты офлайн-Windows (кнопка «Загрузить кусты»).").c_str(), TrW(L"Поиск в реестре").c_str(), MB_OK | MB_ICONINFORMATION); return; }
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


LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CONTEXTMENU: return RegContextMenu((HWND)wp, lp) ? 1 : 0;
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
                case ID_FIND: DoFind(); break;
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
    R.note = Mk(h, L"STATIC", L"Введите путь вида D:\\Windows или выберите из списка, затем «Загрузить кусты». Перед загрузкой копия каждого куста сохраняется в Data\\Backups.", SS_LEFT, ID_NOTE);
    R.lblSearch = Mk(h, L"STATIC", L"Поиск:", SS_LEFT, ID_SLBL);
    R.search = Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_SEARCH, WS_EX_CLIENTEDGE);
    SetWindowSubclass(R.search, SearchEditProc, 1, 0);
    SendMessageW(R.search, EM_SETCUEBANNER, TRUE, (LPARAM)TrW(L"Текст для поиска (Enter — найти)").c_str());
    R.bFind = Mk(h, L"BUTTON", L"Найти", BS_PUSHBUTTON | WS_TABSTOP, ID_FIND);
    const wchar_t* cl[] = {L"Имена разделов", L"Имена значений", L"Данные значений", L"Только в выбранном разделе"};
    for (int i = 0; i < 4; ++i) { R.chk[i] = Mk(h, L"BUTTON", cl[i], BS_AUTOCHECKBOX | WS_TABSTOP, ID_FKEYS + i); if (i < 3) SendMessageW(R.chk[i], BM_SETCHECK, BST_CHECKED, 0); }
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
