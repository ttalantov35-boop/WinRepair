// Offline file explorer: browse the files of a non-running Windows, copy / move / rename, delete (to a reversible quarantine by default),
// take ownership when access is denied, search. Works in WinPE/WinRE without Explorer.
#include "ui_tools_common.h"
#include <shellapi.h>
#include <shlobj.h>
#include <cctype>

namespace wr {
namespace ui {

namespace {

enum { ID_TREE = 8001, ID_LIST, ID_ADDR, ID_GO, ID_UP, ID_BACK, ID_FWD, ID_QF, ID_FIND, ID_STATUS, ID_NOTE, ID_B0 = 8100, NB = 12 };

struct FItem {
    std::string name;
    bool dir = false, reparse = false;
    unsigned long long size = 0, mtime = 0;
    DWORD attr = 0;
    std::string full;   // only in search mode
    int icon = 0;
};

struct State {
    HWND wnd = nullptr, tree, list, addr, qf, status, note, btn[NB], bBack, bFwd, bUp, bGo, bFind;
    std::vector<std::unique_ptr<std::string>> paths;
    std::vector<FItem> items;
    std::string cur;                 // current folder, always with trailing backslash ("" = list of drives)
    std::vector<std::string> back, fwd;
    std::vector<std::string> clip;   // internal clipboard
    bool clipCut = false, searchMode = false, syncing = false;
    int sortCol = 0;
    bool sortDesc = false;
    std::string quarantineStamp;
    std::map<std::string, int> iconCache;
};
State E;

// ---- helpers ---------------------------------------------------------------------------------------------
std::string Slash(std::string p) { if (!p.empty() && p.back() != '\\') p += '\\'; return p; }
std::string NoSlash(std::string p) { while (p.size() > 3 && p.back() == '\\') p.pop_back(); return p; }
std::string Lower(const std::string& s) { return toLowerUtf8(s); }
std::string Parent(const std::string& p) {
    std::string n = NoSlash(p);
    size_t k = n.find_last_of('\\');
    if (n.size() <= 3 || k == std::string::npos) return "";
    return k <= 2 ? n.substr(0, 3) : n.substr(0, k + 1);
}
bool IsDriveRoot(const std::string& p) { std::string n = NoSlash(p); return n.size() == 3 && n[1] == ':' && n[2] == '\\'; }
bool InWindows(const std::string& p) {
    std::string l = Lower(p);
    if (l.size() < 10 || l[1] != ':') return false;
    return l.compare(2, 9, "\\windows\\") == 0 || l.compare(2, std::string::npos, "\\windows") == 0;
}

bool IsProtectedFolder(const std::string& p, std::string& why) {
    std::string l = Lower(NoSlash(p));
    if (IsDriveRoot(p)) { why = "Корень диска удалять нельзя."; return true; }
    if (l.size() > 2) {
        std::string rest = l.substr(2);
        const char* banned[] = {"\\windows", "\\windows\\system32", "\\windows\\syswow64", "\\windows\\winsxs", "\\windows\\system32\\config", "\\windows\\system32\\drivers", "\\users", "\\program files",
                                "\\program files (x86)", "\\programdata", "\\boot", "\\efi", "\\recovery", "\\system volume information", "\\winrepair_removed"};
        for (const char* b : banned) if (rest == b) { why = "Это важная системная папка целиком — удалять её нельзя (содержимое можно, с подтверждением)."; return true; }
    }
    return false;
}

std::string SizeStr(unsigned long long b) {
    if (b < 1024) return fmt("%llu Б", b);
    if (b < 1024ull * 1024) return fmt("%.1f КБ", b / 1024.0);
    if (b < 1024ull * 1024 * 1024) return fmt("%.1f МБ", b / 1048576.0);
    return fmt("%.2f ГБ", b / 1073741824.0);
}
std::string AttrStr(DWORD a) {
    std::string s;
    if (a & FILE_ATTRIBUTE_READONLY) s += "R";
    if (a & FILE_ATTRIBUTE_HIDDEN) s += "H";
    if (a & FILE_ATTRIBUTE_SYSTEM) s += "S";
    if (a & FILE_ATTRIBUTE_ARCHIVE) s += "A";
    if (a & FILE_ATTRIBUTE_COMPRESSED) s += "C";
    if (a & FILE_ATTRIBUTE_ENCRYPTED) s += "E";
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) s += "L";
    return s;
}
std::string TypeStr(const FItem& f) {
    if (f.dir) return f.reparse ? "Папка (ссылка)" : "Папка";
    size_t k = f.name.find_last_of('.');
    if (k == std::string::npos || k + 1 >= f.name.size()) return "Файл";
    std::string e = f.name.substr(k + 1);
    for (char& c : e) c = (char)toupper((unsigned char)c);
    return e + "-файл";
}

int IconFor(const std::string& name, bool dir) {
    std::string key = dir ? "<dir>" : Lower(name.find_last_of('.') == std::string::npos ? "" : name.substr(name.find_last_of('.')));
    auto it = E.iconCache.find(key);
    if (it != E.iconCache.end()) return it->second;
    SHFILEINFOW sfi{};
    std::wstring fake = dir ? L"folder" : W("x" + (key.empty() ? std::string(".") : key));
    SHGetFileInfoW(fake.c_str(), dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    E.iconCache[key] = sfi.iIcon;
    return sfi.iIcon;
}

bool LoadDir(const std::string& dir, std::vector<FItem>& out, std::string& err) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(W(Slash(dir) + "*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) { err = ErrMsg(GetLastError()); return false; }
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        FItem f;
        f.name = U(n);
        f.attr = fd.dwFileAttributes;
        f.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        f.reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        f.size = f.dir ? 0 : (((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        f.mtime = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        f.icon = IconFor(f.name, f.dir);
        out.push_back(std::move(f));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

std::string FullOf(const FItem& f) { return !f.full.empty() ? f.full : Slash(E.cur) + f.name; }

void SortItems() {
    int c = E.sortCol;
    bool d = E.sortDesc;
    std::stable_sort(E.items.begin(), E.items.end(), [&](const FItem& a, const FItem& b) {
        if (a.dir != b.dir) return a.dir;   // folders first, always
        int r = 0;
        switch (c) {
            case 1: r = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0); break;
            case 2: { std::string x = TypeStr(a), y = TypeStr(b); r = x < y ? -1 : (x > y ? 1 : 0); break; }
            case 3: r = a.mtime < b.mtime ? -1 : (a.mtime > b.mtime ? 1 : 0); break;
            case 4: { std::string x = AttrStr(a.attr), y = AttrStr(b.attr); r = x < y ? -1 : (x > y ? 1 : 0); break; }
            default: { std::string x = Lower(a.name), y = Lower(b.name); r = x < y ? -1 : (x > y ? 1 : 0); }
        }
        return d ? r > 0 : r < 0;
    });
}

void FillList() {
    SortItems();
    std::string q = Lower(trim(U(GetText(E.qf))));
    SendMessageW(E.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(E.list);
    int row = 0;
    for (auto& f : E.items) {
        if (!E.searchMode && !q.empty() && Lower(f.name).find(q) == std::string::npos) { continue; }
        std::wstring n = W(E.searchMode ? f.full : f.name);
        LVITEMW li{};
        li.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        li.iItem = row;
        li.pszText = (LPWSTR)n.c_str();
        li.iImage = f.icon;
        li.lParam = (LPARAM)(&f - &E.items[0]);   // index into E.items (rows may be filtered)
        SendMessageW(E.list, LVM_INSERTITEMW, 0, (LPARAM)&li);
        auto set = [&](int sub, const std::string& t) { std::wstring w = W(t); LVITEMW x{}; x.iSubItem = sub; x.pszText = (LPWSTR)w.c_str(); SendMessageW(E.list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&x); };
        set(1, f.dir ? "" : SizeStr(f.size));
        set(2, U(TrW(W(TypeStr(f)))));
        set(3, f.mtime ? FileTimeStr(f.mtime) : "");
        set(4, AttrStr(f.attr));
        ++row;
    }
    SendMessageW(E.list, WM_SETREDRAW, TRUE, 0);
    unsigned long long total = 0;
    int nd = 0, nf = 0;
    for (auto& f : E.items) { if (f.dir) ++nd; else { ++nf; total += f.size; } }
    std::string st = fmt("  Папок: %d, файлов: %d (%s)", nd, nf, SizeStr(total).c_str());
    if (E.searchMode) st = fmt("  Найдено: %zu", E.items.size());
    SetWindowTextW(E.status, (W(E.searchMode ? "" : E.cur) + TrW(W(st))).c_str());
}

// ---- tree ------------------------------------------------------------------------------------------------------
HTREEITEM AddNode(HTREEITEM parent, const std::string& text, const std::string& path, int icon) {
    E.paths.push_back(std::make_unique<std::string>(path));
    std::wstring wt = W(text);
    TVINSERTSTRUCTW t{};
    t.hParent = parent;
    t.hInsertAfter = TVI_LAST;
    t.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    t.item.pszText = (LPWSTR)wt.c_str();
    t.item.lParam = (LPARAM)E.paths.back().get();
    t.item.cChildren = 1;
    t.item.iImage = t.item.iSelectedImage = icon;
    return TreeView_InsertItem(E.tree, &t);
}
std::string ItemPath(HTREEITEM it) {
    TVITEMW tv{};
    tv.mask = TVIF_PARAM;
    tv.hItem = it;
    TreeView_GetItem(E.tree, &tv);
    return tv.lParam ? *(std::string*)tv.lParam : "";
}
void ExpandNode(HTREEITEM it) {
    std::string p = ItemPath(it);
    std::vector<FItem> v;
    std::string err;
    LoadDir(p, v, err);
    std::sort(v.begin(), v.end(), [](const FItem& a, const FItem& b) { return Lower(a.name) < Lower(b.name); });
    int n = 0;
    for (const auto& f : v) if (f.dir) { AddNode(it, f.name, Slash(p) + f.name + "\\", IconFor("", true)); ++n; }
    if (!n) { TVITEMW tv{}; tv.mask = TVIF_CHILDREN; tv.hItem = it; tv.cChildren = 0; TreeView_SetItem(E.tree, &tv); }
}
void FillDrives() {
    TreeView_DeleteAllItems(E.tree);
    E.paths.clear();
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::string root = std::string(1, (char)('A' + i)) + ":\\";
        wchar_t lab[64] = L"";
        UINT type = GetDriveTypeW(W(root).c_str());
        if (type != DRIVE_REMOVABLE) GetVolumeInformationW(W(root).c_str(), lab, 64, nullptr, nullptr, nullptr, nullptr, 0);
        std::string text = root + (lab[0] ? "  [" + U(lab) + "]" : "");
        if (PathExists(root + "Windows\\System32\\config\\SYSTEM")) text += "  — Windows";
        AddNode(TVI_ROOT, text, root, IconFor("", true));
    }
}
void TreeSelect(const std::string& path) {
    std::string n = NoSlash(path);
    if (n.size() < 3) return;
    HTREEITEM it = TreeView_GetRoot(E.tree);
    while (it && Lower(ItemPath(it)).compare(0, 1, Lower(n.substr(0, 1))) != 0) it = TreeView_GetNextSibling(E.tree, it);
    if (!it) return;
    size_t pos = 3;
    while (pos < n.size()) {
        size_t e = n.find('\\', pos);
        std::string part = n.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        TreeView_Expand(E.tree, it, TVE_EXPAND);
        HTREEITEM c = TreeView_GetChild(E.tree, it);
        while (c && Lower(NoSlash(ItemPath(c)).substr(NoSlash(ItemPath(c)).find_last_of('\\') + 1)) != Lower(part)) c = TreeView_GetNextSibling(E.tree, c);
        if (!c) break;
        it = c;
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    E.syncing = true;
    TreeView_SelectItem(E.tree, it);
    TreeView_EnsureVisible(E.tree, it);
    E.syncing = false;
}

// ---- navigation ------------------------------------------------------------------------------------------------
void Navigate(std::string path, bool record = true, bool syncTree = true) {
    path = trim(path);
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
    std::replace(path.begin(), path.end(), '/', '\\');
    if (path.size() == 2 && path[1] == ':') path += "\\";
    if (path.size() < 3 || path[1] != ':') { SetWindowTextW(E.status, TrW(L"  Введите путь вида D:\\Windows").c_str()); return; }
    path = Slash(path);
    DWORD a = GetFileAttributesW(W(path).c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetWindowTextW(E.status, (TrW(L"  Папка не найдена: ") + W(path)).c_str());
        return;
    }
    std::vector<FItem> v;
    std::string err;
    if (!LoadDir(path, v, err)) {
        SetWindowTextW(E.status, (TrW(L"  Нет доступа: ") + W(path) + L"  —  " + W(err) + TrW(L"  (кнопка «Владелец и права…» может помочь)")).c_str());
        return;
    }
    if (record && !E.cur.empty() && E.cur != path) { E.back.push_back(E.cur); E.fwd.clear(); }
    E.cur = path;
    E.searchMode = false;
    E.items = std::move(v);
    SetWindowTextW(E.addr, W(path).c_str());
    FillList();
    if (syncTree) TreeSelect(path);
    EnableWindow(E.bBack, !E.back.empty());
    EnableWindow(E.bFwd, !E.fwd.empty());
    EnableWindow(E.bUp, !Parent(path).empty());
}
void Refresh() { std::string c = E.cur; E.cur.clear(); if (!c.empty()) { std::vector<std::string> b = E.back, f = E.fwd; Navigate(c, false); E.back = b; E.fwd = f; if (E.cur.empty()) E.cur = c; } }
void GoUp() { std::string p = Parent(E.cur); if (!p.empty()) Navigate(p); }
void GoBack() { if (E.back.empty()) return; std::string p = E.back.back(); E.back.pop_back(); E.fwd.push_back(E.cur); Navigate(p, false); }
void GoFwd() { if (E.fwd.empty()) return; std::string p = E.fwd.back(); E.fwd.pop_back(); E.back.push_back(E.cur); Navigate(p, false); }

std::vector<int> SelRows() {
    std::vector<int> r;
    for (int i = -1; (i = ListView_GetNextItem(E.list, i, LVNI_SELECTED)) >= 0;) r.push_back(i);
    return r;
}
const FItem* ItemAt(int row) {
    LVITEMW li{};
    li.mask = LVIF_PARAM;
    li.iItem = row;
    if (!ListView_GetItem(E.list, &li)) return nullptr;
    return (size_t)li.lParam < E.items.size() ? &E.items[(size_t)li.lParam] : nullptr;
}
std::vector<std::string> SelPaths() {
    std::vector<std::string> v;
    for (int r : SelRows()) if (const FItem* f = ItemAt(r)) v.push_back(FullOf(*f));
    return v;
}

// ---- operations ------------------------------------------------------------------------------------------------
bool TakeOwnership(const std::string& path, bool dir) {
    std::atomic<bool> nc{false};
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    RunProcess("\"" + SysTool("takeown.exe") + "\" /f \"" + NoSlash(path) + "\"" + (dir ? " /r /d y" : ""), "", nullptr, &nc, 900);
    ProcResult r = RunProcess("\"" + SysTool("icacls.exe") + "\" \"" + NoSlash(path) + "\" /grant *S-1-5-32-544:(OI)(CI)F /t /c /q", "", nullptr, &nc, 900);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    AuditChange("EXPLORER: ownership + admin rights taken: " + path);
    return r.started;
}

std::wstring DoubleNull(const std::vector<std::string>& v) {
    std::wstring s;
    for (const auto& p : v) { s += W(NoSlash(p)); s += L'\0'; }
    s += L'\0';
    return s;
}

std::string Describe(const std::vector<std::string>& v) {
    std::string t;
    for (size_t i = 0; i < v.size() && i < 8; ++i) t += "  " + v[i] + "\n";
    if (v.size() > 8) t += fmt("  … и ещё %zu\n", v.size() - 8);
    return t;
}

bool ToQuarantine(const std::string& path, std::string& dest) {
    std::string src = NoSlash(path);
    if (E.quarantineStamp.empty()) E.quarantineStamp = Stamp();
    std::string rel = src.substr(3);
    dest = src.substr(0, 3) + "WinRepair_Removed\\" + E.quarantineStamp + "\\" + rel;
    size_t k = dest.find_last_of('\\');
    MakeDirs(dest.substr(0, k));
    std::string d = dest;
    for (int i = 1; PathExists(d) && i < 1000; ++i) d = dest + "_" + std::to_string(i);
    dest = d;
    if (MoveFileExW(W(src).c_str(), W(dest).c_str(), MOVEFILE_COPY_ALLOWED)) return true;
    DWORD a = GetFileAttributesW(W(src).c_str());
    TakeOwnership(src, a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY));
    return MoveFileExW(W(src).c_str(), W(dest).c_str(), MOVEFILE_COPY_ALLOWED) != 0;
}

void DeleteSelected(bool permanent) {
    auto v = SelPaths();
    if (v.empty()) return;
    bool sys = false, inQ = false;
    for (const auto& p : v) {
        std::string why;
        if (IsProtectedFolder(p, why)) { MessageBoxW(E.wnd, (W(p) + L"\n\n" + TrW(W(why))).c_str(), TrW(L"Удаление запрещено").c_str(), MB_OK | MB_ICONSTOP); return; }
        if (InWindows(p)) sys = true;
        if (Lower(p).find("\\winrepair_removed\\") != std::string::npos) inQ = true;
    }
    if (inQ) permanent = true;   // quarantine contents: deleting there means purging
    std::wstring msg = (permanent ? TrW(L"УДАЛИТЬ НАВСЕГДА:\n\n") : TrW(L"Переместить в карантин WinRepair_Removed (на этом же диске, обратимо):\n\n")) + W(Describe(v));
    if (sys) msg += L"\n" + TrW(L"ВНИМАНИЕ: это файлы каталога Windows. Удаление нужных системных файлов сломает загрузку.\n");
    msg += L"\n" + TrW(L"Продолжить?");
    if (MessageBoxW(E.wnd, msg.c_str(), TrW(L"Подтверждение удаления").c_str(), MB_YESNO | ((permanent || sys) ? MB_ICONSTOP : MB_ICONWARNING) | MB_DEFBUTTON2) != IDYES) return;
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    int ok = 0, bad = 0;
    for (const auto& p : v) {
        if (!permanent) {
            std::string dest;
            if (ToQuarantine(p, dest)) { ++ok; AuditChange("EXPLORER: quarantined " + p + " -> " + dest); } else { ++bad; Logger::I().log(Lvl::Warn, "Не удалось переместить: " + p + " — " + ErrMsg(GetLastError())); }
        } else {
            DWORD a = GetFileAttributesW(W(NoSlash(p)).c_str());
            bool isDir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
            SHFILEOPSTRUCTW op{};
            std::wstring from = DoubleNull({p});
            op.hwnd = E.wnd; op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
            int r = SHFileOperationW(&op);
            if (r != 0 || PathExists(NoSlash(p))) {
                TakeOwnership(p, isDir);
                SetFileAttributesW(W(NoSlash(p)).c_str(), FILE_ATTRIBUTE_NORMAL);
                std::atomic<bool> nc{false};
                RunProcess(isDir ? "\"" + SysTool("cmd.exe") + "\" /c rd /s /q \"" + NoSlash(p) + "\"" : "\"" + SysTool("cmd.exe") + "\" /c del /f /q \"" + NoSlash(p) + "\"", "", nullptr, &nc, 900);
            }
            if (PathExists(NoSlash(p))) { ++bad; Logger::I().log(Lvl::Warn, "Не удалось удалить: " + p); } else { ++ok; AuditChange("EXPLORER: deleted " + p); }
        }
    }
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    Logger::I().log(bad ? Lvl::Warn : Lvl::Ok, fmt("Проводник: %s — успешно %d, с ошибками %d", permanent ? "удаление" : "в карантин", ok, bad));
    Refresh();
}

void CopyToClip(bool cut) {
    E.clip = SelPaths();
    E.clipCut = cut;
    SetWindowTextW(E.status, W(fmt("  В буфере: %zu (%s)", E.clip.size(), cut ? "переместить" : "копировать")).c_str());
}
void Paste() {
    if (E.clip.empty() || E.cur.empty() || E.searchMode) return;
    std::wstring from = DoubleNull(E.clip), to = W(NoSlash(E.cur)) + L'\0';
    to += L'\0';
    SHFILEOPSTRUCTW op{};
    op.hwnd = E.wnd; op.wFunc = E.clipCut ? FO_MOVE : FO_COPY; op.pFrom = from.c_str(); op.pTo = to.c_str(); op.fFlags = FOF_NOCONFIRMMKDIR;
    int r = SHFileOperationW(&op);
    AuditChange(std::string("EXPLORER: ") + (E.clipCut ? "moved" : "copied") + " " + std::to_string(E.clip.size()) + " item(s) to " + E.cur + (r ? " (error " + std::to_string(r) + ")" : ""));
    if (E.clipCut && !r) E.clip.clear();
    Refresh();
}
void NewFolder() {
    if (E.cur.empty() || E.searchMode) return;
    std::vector<FormField> f(1);
    f[0].label = L"Имя новой папки";
    f[0].value = L"Новая папка";
    if (!FormDialog(E.wnd, L"Новая папка", f)) return;
    std::string n = trim(U(f[0].value));
    if (n.empty() || n.find_first_of("\\/:*?\"<>|") != std::string::npos) return;
    if (!CreateDirectoryW(W(Slash(E.cur) + n).c_str(), nullptr)) MessageBoxW(E.wnd, W(ErrMsg(GetLastError())).c_str(), TrW(L"Ошибка").c_str(), MB_OK | MB_ICONERROR);
    else AuditChange("EXPLORER: mkdir " + Slash(E.cur) + n);
    Refresh();
}
void Rename() {
    auto v = SelPaths();
    if (v.size() != 1) return;
    std::string old = NoSlash(v[0]);
    std::string leaf = old.substr(old.find_last_of('\\') + 1);
    std::vector<FormField> f(1);
    f[0].label = L"Новое имя";
    f[0].value = W(leaf);
    if (!FormDialog(E.wnd, L"Переименовать", f)) return;
    std::string n = trim(U(f[0].value));
    if (n.empty() || n == leaf || n.find_first_of("\\/:*?\"<>|") != std::string::npos) return;
    std::string dest = old.substr(0, old.find_last_of('\\') + 1) + n;
    if (!MoveFileExW(W(old).c_str(), W(dest).c_str(), 0)) { MessageBoxW(E.wnd, W(ErrMsg(GetLastError())).c_str(), TrW(L"Ошибка").c_str(), MB_OK | MB_ICONERROR); return; }
    AuditChange("EXPLORER: rename " + old + " -> " + dest);
    Refresh();
}
void OpenItem(int row) {
    const FItem* f = ItemAt(row);
    if (!f) return;
    std::string p = FullOf(*f);
    if (f->dir) { Navigate(p); return; }
    if (E.searchMode) { std::string par = Parent(p); std::string nm = f->name; Navigate(par); return; }
    HINSTANCE r = ShellExecuteW(E.wnd, L"open", W(p).c_str(), nullptr, W(E.cur).c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) ShellExecuteW(E.wnd, nullptr, L"notepad.exe", (L"\"" + W(p) + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
void OpenInNotepad() {
    auto v = SelPaths();
    if (v.size() == 1) ShellExecuteW(E.wnd, nullptr, L"notepad.exe", (L"\"" + W(v[0]) + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
void ClearAttrs() {
    auto v = SelPaths();
    if (v.empty()) return;
    if (MessageBoxW(E.wnd, (TrW(L"Снять атрибуты «только чтение», «скрытый», «системный» у:\n\n") + W(Describe(v))).c_str(), TrW(L"Атрибуты").c_str(), MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    for (const auto& p : v) {
        DWORD a = GetFileAttributesW(W(NoSlash(p)).c_str());
        if (a != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(W(NoSlash(p)).c_str(), a & ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
        AuditChange("EXPLORER: attributes cleared " + p);
    }
    Refresh();
}
void OwnerRights() {
    auto v = SelPaths();
    if (v.empty()) v.push_back(E.cur);
    if (MessageBoxW(E.wnd, (TrW(L"Стать владельцем и выдать группе «Администраторы» полный доступ (takeown + icacls):\n\n") + W(Describe(v)) + L"\n" + TrW(L"Это нужно, если Windows отказывает в доступе. Продолжить?")).c_str(),
                    TrW(L"Владелец и права").c_str(), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    for (const auto& p : v) { DWORD a = GetFileAttributesW(W(NoSlash(p)).c_str()); TakeOwnership(p, a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)); }
    Refresh();
}

void CalcSize(const std::string& dir, unsigned long long& bytes, long& files, long& dirs, ULONGLONG deadline) {
    if (GetTickCount64() > deadline) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(W(Slash(dir) + "*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ++dirs;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) CalcSize(Slash(dir) + U(n), bytes, files, dirs, deadline);
        } else { ++files; bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; }
    } while (GetTickCount64() < deadline && FindNextFileW(h, &fd));
    FindClose(h);
}
void Properties() {
    auto v = SelPaths();
    if (v.empty()) v.push_back(E.cur);
    std::wstring t;
    for (const auto& p : v) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!GetFileAttributesExW(W(NoSlash(p)).c_str(), GetFileExInfoStandard, &d)) continue;
        bool dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        auto ft = [](const FILETIME& f) { return FileTimeStr(((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime); };
        t += W(NoSlash(p)) + L"\n";
        if (dir) {
            unsigned long long b = 0; long nf = 0, nd = 0;
            SetCursor(LoadCursor(nullptr, IDC_WAIT));
            CalcSize(p, b, nf, nd, GetTickCount64() + 8000);
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            t += W(fmt("  Размер: %s (файлов %ld, папок %ld; подсчёт ограничен 8 секундами)\n", SizeStr(b).c_str(), nf, nd));
        } else t += W(fmt("  Размер: %s (%llu байт)\n", SizeStr(((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow).c_str(), ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow));
        t += W("  Создан: " + ft(d.ftCreationTime) + "\n  Изменён: " + ft(d.ftLastWriteTime) + "\n  Атрибуты: " + AttrStr(d.dwFileAttributes) + "\n\n");
    }
    MessageBoxW(E.wnd, t.empty() ? TrW(L"Нет данных.").c_str() : t.c_str(), TrW(L"Свойства").c_str(), MB_OK | MB_ICONINFORMATION);
}

bool Match(const std::string& nameLower, const std::string& pat) {   // substring or * ? wildcard
    if (pat.find_first_of("*?") == std::string::npos) return nameLower.find(pat) != std::string::npos;
    std::function<bool(size_t, size_t)> m = [&](size_t i, size_t j) -> bool {
        if (j == pat.size()) return i == nameLower.size();
        if (pat[j] == '*') { for (size_t k = i; k <= nameLower.size(); ++k) if (m(k, j + 1)) return true; return false; }
        if (i < nameLower.size() && (pat[j] == '?' || pat[j] == nameLower[i])) return m(i + 1, j + 1);
        return false;
    };
    return m(0, 0);
}
void SearchRec(const std::string& dir, const std::string& pat, std::vector<FItem>& out, ULONGLONG deadline, int depth) {
    if (depth > 40 || GetTickCount64() > deadline || out.size() >= 3000) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(W(Slash(dir) + "*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> subs;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::string nm = U(n);
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (Match(Lower(nm), pat)) {
            FItem f;
            f.name = nm; f.dir = isDir; f.attr = fd.dwFileAttributes; f.reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            f.size = isDir ? 0 : (((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
            f.mtime = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            f.full = Slash(dir) + nm;
            f.icon = IconFor(nm, isDir);
            out.push_back(std::move(f));
        }
        if (isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) subs.push_back(nm);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const auto& s : subs) SearchRec(Slash(dir) + s, pat, out, deadline, depth + 1);
}
void DoSearch() {
    std::string q = Lower(trim(U(GetText(E.qf))));
    if (E.cur.empty() || q.empty()) { MessageBoxW(E.wnd, TrW(L"Введите имя или маску (например *.log) в поле поиска.").c_str(), TrW(L"Поиск").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::vector<FItem> res;
    SearchRec(E.cur, q, res, GetTickCount64() + 20000, 0);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    E.items = std::move(res);
    E.searchMode = true;
    FillList();
}

// ---- context menus ---------------------------------------------------------------------------------------------
enum { M_OPEN = 1, M_NOTE, M_COPY, M_CUT, M_PASTE, M_REN, M_DELQ, M_DELP, M_OWN, M_ATTR, M_PROPS, M_CPATH, M_NEWF, M_REFRESH, M_UP, M_SELALL };

void ListMenu(POINT pt, bool onItem) {
    auto sel = SelRows();
    bool one = sel.size() == 1;
    HMENU m = CreatePopupMenu();
    if (onItem) {
        AppendMenuW(m, MF_STRING | (one ? 0 : MF_GRAYED), M_OPEN, TrW(L"Открыть\tEnter").c_str());
        AppendMenuW(m, MF_STRING | (one ? 0 : MF_GRAYED), M_NOTE, TrW(L"Открыть в Блокноте").c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, M_COPY, TrW(L"Копировать\tCtrl+C").c_str());
        AppendMenuW(m, MF_STRING, M_CUT, TrW(L"Вырезать\tCtrl+X").c_str());
        AppendMenuW(m, MF_STRING | (E.clip.empty() || E.searchMode ? MF_GRAYED : 0), M_PASTE, TrW(L"Вставить\tCtrl+V").c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (one ? 0 : MF_GRAYED), M_REN, TrW(L"Переименовать\tF2").c_str());
        AppendMenuW(m, MF_STRING, M_DELQ, TrW(L"Удалить в карантин\tDel").c_str());
        AppendMenuW(m, MF_STRING, M_DELP, TrW(L"Удалить навсегда\tShift+Del").c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, M_OWN, TrW(L"Владелец и права…").c_str());
        AppendMenuW(m, MF_STRING, M_ATTR, TrW(L"Снять атрибуты R/H/S…").c_str());
        AppendMenuW(m, MF_STRING, M_CPATH, TrW(L"Копировать путь").c_str());
        AppendMenuW(m, MF_STRING, M_PROPS, TrW(L"Свойства").c_str());
    } else {   // empty area of the folder
        AppendMenuW(m, MF_STRING | (E.clip.empty() || E.searchMode ? MF_GRAYED : 0), M_PASTE, TrW(L"Вставить\tCtrl+V").c_str());
        AppendMenuW(m, MF_STRING | (E.searchMode ? MF_GRAYED : 0), M_NEWF, TrW(L"Создать папку").c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, M_SELALL, TrW(L"Выделить всё\tCtrl+A").c_str());
        AppendMenuW(m, MF_STRING, M_REFRESH, TrW(L"Обновить\tF5").c_str());
        AppendMenuW(m, MF_STRING | (Parent(E.cur).empty() ? MF_GRAYED : 0), M_UP, TrW(L"Вверх\tBackspace").c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, M_CPATH, TrW(L"Копировать путь папки").c_str());
        AppendMenuW(m, MF_STRING, M_OWN, TrW(L"Владелец и права папки…").c_str());
        AppendMenuW(m, MF_STRING, M_PROPS, TrW(L"Свойства папки").c_str());
    }
    SetForegroundWindow(E.wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, E.wnd, nullptr);
    DestroyMenu(m);
    switch (c) {
        case M_OPEN: if (!sel.empty()) OpenItem(sel[0]); break;
        case M_NOTE: OpenInNotepad(); break;
        case M_COPY: CopyToClip(false); break;
        case M_CUT: CopyToClip(true); break;
        case M_PASTE: Paste(); break;
        case M_REN: Rename(); break;
        case M_DELQ: DeleteSelected(false); break;
        case M_DELP: DeleteSelected(true); break;
        case M_OWN: OwnerRights(); break;
        case M_ATTR: ClearAttrs(); break;
        case M_CPATH: { auto v = SelPaths(); if (!onItem || v.empty()) ClipText(E.wnd, W(E.cur)); else { std::wstring a; for (auto& p : v) a += W(NoSlash(p)) + L"\r\n"; ClipText(E.wnd, a); } break; }
        case M_PROPS: Properties(); break;
        case M_NEWF: NewFolder(); break;
        case M_REFRESH: Refresh(); break;
        case M_UP: GoUp(); break;
        case M_SELALL: ListView_SetItemState(E.list, -1, LVIS_SELECTED, LVIS_SELECTED); break;
    }
}

void TreeMenu(POINT pt) {
    HTREEITEM it = TreeView_GetSelection(E.tree);
    if (!it) return;
    std::string p = ItemPath(it);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, TrW(L"Открыть").c_str());
    AppendMenuW(m, MF_STRING, 2, TrW(L"Развернуть / свернуть").c_str());
    AppendMenuW(m, MF_STRING, 3, TrW(L"Обновить ветку").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 4, TrW(L"Копировать путь").c_str());
    SetForegroundWindow(E.wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, E.wnd, nullptr);
    DestroyMenu(m);
    if (c == 1) Navigate(p, true, false);
    else if (c == 2) TreeView_Expand(E.tree, it, TVE_TOGGLE);
    else if (c == 3) { HTREEITEM ch; while ((ch = TreeView_GetChild(E.tree, it)) != nullptr) TreeView_DeleteItem(E.tree, ch); ExpandNode(it); TreeView_Expand(E.tree, it, TVE_EXPAND); }
    else if (c == 4) ClipText(E.wnd, W(NoSlash(p)));
}

bool ContextMenu(HWND src, LPARAM lp) {
    if (src != E.list && src != E.tree && src != ListView_GetHeader(E.list)) return false;
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    if (lp == (LPARAM)-1) { RECT r; GetWindowRect(src, &r); pt = {r.left + S(20), r.top + S(20)}; }
    if (src == E.tree) {
        if (lp != (LPARAM)-1) { POINT c = pt; ScreenToClient(E.tree, &c); TVHITTESTINFO hi{}; hi.pt = c; HTREEITEM it = TreeView_HitTest(E.tree, &hi); if (it) TreeView_SelectItem(E.tree, it); }
        TreeMenu(pt);
        return true;
    }
    if (src == ListView_GetHeader(E.list)) return true;   // header: nothing but sorting by click
    bool onItem = false;
    if (lp != (LPARAM)-1) {
        POINT c = pt;
        ScreenToClient(E.list, &c);
        LVHITTESTINFO hi{};
        hi.pt = c;
        int row = ListView_HitTest(E.list, &hi);
        if (row >= 0) {
            onItem = true;
            if (!(ListView_GetItemState(E.list, row, LVIS_SELECTED) & LVIS_SELECTED)) {
                ListView_SetItemState(E.list, -1, 0, LVIS_SELECTED);
                ListView_SetItemState(E.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
        } else ListView_SetItemState(E.list, -1, 0, LVIS_SELECTED);
    } else onItem = !SelRows().empty();
    ListMenu(pt, onItem);
    return true;
}

// ---- window ----------------------------------------------------------------------------------------------------
void Layout() {
    RECT rc;
    GetClientRect(E.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8), y = m;
    int bt = S(30);
    Place(E.bBack, m, y, bt, S(28));
    Place(E.bFwd, m + bt + S(2), y, bt, S(28));
    Place(E.bUp, m + 2 * (bt + S(2)), y, bt, S(28));
    int ax = m + 3 * (bt + S(2)) + S(6);
    int xFind = W_ - m - S(80), xQf = xFind - S(4) - S(260), xGo = xQf - S(6) - S(84);
    Place(E.addr, ax, y + S(1), std::max(S(100), xGo - S(6) - ax), S(26));
    Place(E.bGo, xGo, y, S(84), S(28));
    Place(E.qf, xQf, y + S(1), S(260), S(26));
    Place(E.bFind, xFind, y, S(80), S(28));
    y += S(32);
    Place(E.note, m, y, W_ - 2 * m, S(18));
    y += S(22);
    int rows = 2, bh = S(30);
    int bodyH = H - y - m - rows * (bh + S(6)) - S(26);
    int treeW = std::max(S(220), W_ * 22 / 100);
    Place(E.tree, m, y, treeW, bodyH);
    Place(E.list, m + treeW + m, y, W_ - treeW - 3 * m, bodyH);
    y += bodyH + S(4);
    Place(E.status, m, y, W_ - 2 * m, S(20));
    y += S(24);
    int per = NB / rows;
    int bw = (W_ - 2 * m - (per - 1) * S(6)) / per;
    for (int i = 0; i < NB; ++i) Place(E.btn[i], m + (i % per) * (bw + S(6)), y + (i / per) * (bh + S(6)), bw, bh);
}

void Command(int i) {
    switch (i) {
        case 0: { auto s = SelRows(); if (!s.empty()) OpenItem(s[0]); break; }
        case 1: NewFolder(); break;
        case 2: Rename(); break;
        case 3: CopyToClip(false); break;
        case 4: CopyToClip(true); break;
        case 5: Paste(); break;
        case 6: DeleteSelected(false); break;
        case 7: DeleteSelected(true); break;
        case 8: OwnerRights(); break;
        case 9: Properties(); break;
        case 10: Refresh(); break;
        case 11: DestroyWindow(E.wnd); break;
    }
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CONTEXTMENU: return ContextMenu((HWND)wp, lp) ? 1 : 0;
        case WM_SIZE: if (E.btn[NB - 1]) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(940); mm->ptMinTrackSize.y = S(560); return 0; }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == ID_GO) Navigate(U(GetText(E.addr)));
            else if (id == ID_UP) GoUp();
            else if (id == ID_BACK) GoBack();
            else if (id == ID_FWD) GoFwd();
            else if (id == ID_FIND) DoSearch();
            else if (id == ID_QF && HIWORD(wp) == EN_CHANGE && !E.searchMode) FillList();
            else if (id >= ID_B0 && id < ID_B0 + NB) Command(id - ID_B0);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->hwndFrom == E.tree) {
                if (nh->code == TVN_ITEMEXPANDINGW) {
                    auto* tv = (NMTREEVIEWW*)lp;
                    if (tv->action == TVE_EXPAND && !(tv->itemNew.state & TVIS_EXPANDEDONCE)) ExpandNode(tv->itemNew.hItem);
                } else if (nh->code == TVN_SELCHANGEDW && !E.syncing) {
                    auto* tv = (NMTREEVIEWW*)lp;
                    if (tv->itemNew.hItem) Navigate(ItemPath(tv->itemNew.hItem), true, false);
                }
            } else if (nh->hwndFrom == E.list) {
                if (nh->code == NM_DBLCLK || nh->code == LVN_ITEMACTIVATE) { int r = ListView_GetNextItem(E.list, -1, LVNI_SELECTED); if (r >= 0) OpenItem(r); }
                else if (nh->code == LVN_COLUMNCLICK) {
                    int c = ((NMLISTVIEW*)lp)->iSubItem;
                    if (c == E.sortCol) E.sortDesc = !E.sortDesc; else { E.sortCol = c; E.sortDesc = false; }
                    FillList();
                } else if (nh->code == LVN_KEYDOWN) {
                    WORD k = ((NMLVKEYDOWN*)lp)->wVKey;
                    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                    if (k == VK_RETURN) { int r = ListView_GetNextItem(E.list, -1, LVNI_SELECTED); if (r >= 0) OpenItem(r); }
                    else if (k == VK_BACK) GoUp();
                    else if (k == VK_F5) Refresh();
                    else if (k == VK_F2) Rename();
                    else if (k == VK_DELETE) DeleteSelected(shift);
                    else if (alt && k == VK_LEFT) GoBack();
                    else if (alt && k == VK_RIGHT) GoFwd();
                    else if (ctrl && k == 'C') CopyToClip(false);
                    else if (ctrl && k == 'X') CopyToClip(true);
                    else if (ctrl && k == 'V') Paste();
                    else if (ctrl && k == 'A') ListView_SetItemState(E.list, -1, LVIS_SELECTED, LVIS_SELECTED);
                    else if (ctrl && k == 'F') SetFocus(E.qf);
                } else if (nh->code == NM_CUSTOMDRAW) {   // hidden/system items are greyed like in Explorer, quarantine stands out
                    auto* cd = (NMLVCUSTOMDRAW*)lp;
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        const FItem* f = ItemAt((int)cd->nmcd.dwItemSpec);
                        if (f && (f->attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) cd->clrText = RGB(125, 125, 125);
                        return CDRF_NEWFONT;
                    }
                    return CDRF_DODEFAULT;
                }
            }
            return 0;
        }
        case WM_DESTROY: E.wnd = nullptr; E.paths.clear(); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT CALLBACK AddrProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && wp == VK_RETURN) { Navigate(U(GetText(h))); SetFocus(E.list); return 0; }
    if (m == WM_CHAR && wp == VK_RETURN) return 0;
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, AddrProc, 3);
    return DefSubclassProc(h, m, wp, lp);
}
LRESULT CALLBACK QfProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && wp == VK_RETURN) { DoSearch(); return 0; }   // Enter = search in subfolders; typing alone filters the current folder
    if (m == WM_CHAR && wp == VK_RETURN) return 0;
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, QfProc, 4);
    return DefSubclassProc(h, m, wp, lp);
}

}  // namespace

void OpenOfflineExplorerAt(const std::string& path) {
    if (!E.wnd) {
        RegisterOnce(L"WRExplorer", Proc);
        E = State();
        E.wnd = CreateWindowExW(0, L"WRExplorer", L"Офлайн-проводник — файлы выбранной Windows: просмотр, копирование, удаление", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1240), S(740),
                                g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND h = E.wnd;
        E.bBack = Mk(h, L"BUTTON", L"←", BS_PUSHBUTTON | WS_TABSTOP, ID_BACK);
        E.bFwd = Mk(h, L"BUTTON", L"→", BS_PUSHBUTTON | WS_TABSTOP, ID_FWD);
        E.bUp = Mk(h, L"BUTTON", L"↑", BS_PUSHBUTTON | WS_TABSTOP, ID_UP);
        E.addr = Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_ADDR, WS_EX_CLIENTEDGE);
        SetWindowSubclass(E.addr, AddrProc, 3, 0);
        E.bGo = Mk(h, L"BUTTON", L"Перейти", BS_PUSHBUTTON | WS_TABSTOP, ID_GO);
        E.qf = Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_QF, WS_EX_CLIENTEDGE);
        SetWindowSubclass(E.qf, QfProc, 4, 0);
        SendMessageW(E.qf, EM_SETCUEBANNER, TRUE, (LPARAM)TrW(L"Фильтр / поиск (Enter — в подпапках, * ?)").c_str());
        E.bFind = Mk(h, L"BUTTON", L"Найти", BS_PUSHBUTTON | WS_TABSTOP, ID_FIND);
        E.note = Mk(h, L"STATIC", L"Удаление по умолчанию — в карантин X:\\WinRepair_Removed (обратимо). «Удалить навсегда» и файлы Windows требуют повторного подтверждения. Все изменения пишутся в журнал.", SS_LEFT, ID_NOTE);
        E.tree = Mk(h, WC_TREEVIEWW, L"", TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | WS_TABSTOP, ID_TREE, WS_EX_CLIENTEDGE);
        E.list = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(E.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
        SHFILEINFOW sfi{};
        HIMAGELIST il = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (il) { ListView_SetImageList(E.list, il, LVSIL_SMALL); TreeView_SetImageList(E.tree, il, TVSIL_NORMAL); }
        const wchar_t* cols[] = {L"Имя", L"Размер", L"Тип", L"Изменён", L"Атрибуты"};
        int cw[] = {S(380), S(100), S(110), S(150), S(80)};
        for (int i = 0; i < 5; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = cw[i]; ListView_InsertColumn(E.list, i, &c); }
        E.status = Mk(h, L"STATIC", L"", SS_LEFT | SS_SUNKEN, ID_STATUS);
        const wchar_t* bt[] = {L"Открыть", L"Новая папка…", L"Переименовать…", L"Копировать", L"Вырезать", L"Вставить", L"Удалить в карантин", L"Удалить навсегда…", L"Владелец и права…", L"Свойства", L"Обновить", L"Закрыть"};
        for (int i = 0; i < NB; ++i) E.btn[i] = Mk(h, L"BUTTON", bt[i], BS_PUSHBUTTON | WS_TABSTOP, ID_B0 + i);
        Layout();
        ShowWindow(h, SW_SHOW);
        FillDrives();
    } else { ShowWindow(E.wnd, SW_RESTORE); SetForegroundWindow(E.wnd); }
    std::string start = path;
    if (start.empty()) { Target t = UiCurrentTarget(); start = t.valid ? t.root : "C:\\"; }
    DWORD a = GetFileAttributesW(W(start).c_str());
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) start = Parent(start);
    Navigate(start);
}

void OpenOfflineExplorerImpl() { OpenOfflineExplorerAt(""); }

}  // namespace ui
}  // namespace wr
