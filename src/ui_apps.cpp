// Installed programs of an offline Windows: find a program that breaks the boot (antivirus, driver tools) and remove it by force.
#include "ui_tools_common.h"
#include "modules/apps.h"
#include <shellapi.h>
#include <cctype>

namespace wr {
namespace ui {

namespace {

enum { ID_TARGET = 7001, ID_SCAN, ID_QF, ID_QFLBL, ID_HIDEUPD, ID_ONLYAV, ID_LIST, ID_INFO, ID_B0 = 7100, NB = 8 };

struct State {
    HWND wnd = nullptr, combo, list, qf, hideUpd, onlyAv, info, btn[NB];
    std::vector<Target> targets;
    std::vector<AppItem> items;
    std::vector<AutoItem> startup;   // all autoruns/services/drivers of the scanned system (to find what belongs to a program)
    std::vector<int> view;
    int sortCol = 0;
    bool sortDesc = false, pinned = false;
};
State A;
bool g_jobPending = false;

const wchar_t* kCols[] = {L"Название", L"Категория", L"Издатель", L"Версия", L"Дата", L"Размер", L"Папка установки", L"Источник"};
constexpr int kN = 8;

const char* kAvWords[] = {"kaspersky", "avast", "avg ", "norton", "mcafee", "eset", "nod32", "bitdefender", "sophos", "dr.web", "drweb", "comodo", "avira", "malwarebytes", "symantec",
                          "trend micro", "f-secure", "panda", "360 total", "360 security", "webroot", "crowdstrike", "cylance", "carbon black", "sentinel", "cybereason", "endpoint protection",
                          "antivirus", "anti-virus", "антивирус", "firewall", "брандмауэр", "kes ", "защит", "security suite", "total security", "internet security", "vipnet", "secret net", "kaspersky"};

bool IsAv(const AppItem& a) {
    std::string l = toLowerUtf8(a.name + " " + a.publisher);
    for (const char* w : kAvWords) if (l.find(w) != std::string::npos) return true;
    return false;
}

std::string CategoryOf(const AppItem& a) {
    if (IsAv(a)) return "Антивирус / защита";
    if (a.update) return "Обновление";
    if (a.system) return "Системный компонент";
    std::string l = toLowerUtf8(a.name + " " + a.publisher);
    if (l.find("driver") != std::string::npos || l.find("драйвер") != std::string::npos || l.find("nvidia") != std::string::npos || l.find("intel(r)") != std::string::npos || l.find("realtek") != std::string::npos || l.find("amd ") != std::string::npos) return "Драйвер / утилита";
    if (l.find("microsoft") != std::string::npos) return "Microsoft";
    return "Программа";
}

std::string SizeText(const AppItem& a) { return a.sizeKB ? fmt("%.1f МБ", a.sizeKB / 1024.0) : ""; }

void Cells(const AppItem& a, std::string c[kN]) {
    c[0] = a.name; c[1] = CategoryOf(a); c[2] = a.publisher; c[3] = a.version; c[4] = a.date; c[5] = SizeText(a);
    c[6] = a.location.empty() ? "" : (a.dirExists ? a.location : a.location + "  (нет на диске)"); c[7] = a.source;
}

bool PassBase(const AppItem& a) {
    bool hide = SendMessageW(A.hideUpd, BM_GETCHECK, 0, 0) == BST_CHECKED, onlyAv = SendMessageW(A.onlyAv, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (hide && (a.update || a.system)) return false;
    if (onlyAv && !IsAv(a)) return false;
    return true;
}

void Fill() {
    ListView_DeleteAllItems(A.list);
    A.view.clear();
    std::string q = toLowerUtf8(trim(U(GetText(A.qf))));
    for (size_t i = 0; i < A.items.size(); ++i) {
        if (!PassBase(A.items[i])) continue;
        std::string c[kN];
        Cells(A.items[i], c);
        if (!q.empty()) {
            bool hit = false;
            for (int k = 0; k < kN && !hit; ++k) hit = toLowerUtf8(c[k]).find(q) != std::string::npos;
            if (!hit) continue;
        }
        A.view.push_back((int)i);
    }
    int col = A.sortCol;
    bool desc = A.sortDesc;
    std::stable_sort(A.view.begin(), A.view.end(), [&](int x, int y) {
        const AppItem &a = A.items[(size_t)x], &b = A.items[(size_t)y];
        if (col == 5) return desc ? a.sizeKB > b.sizeKB : a.sizeKB < b.sizeKB;
        std::string ca[kN], cb[kN];
        Cells(a, ca); Cells(b, cb);
        std::string la = toLowerUtf8(ca[col]), lb = toLowerUtf8(cb[col]);
        return desc ? la > lb : la < lb;
    });
    SendMessageW(A.list, WM_SETREDRAW, FALSE, 0);
    for (size_t r = 0; r < A.view.size(); ++r) {
        std::string c[kN];
        Cells(A.items[(size_t)A.view[r]], c);
        std::wstring first = W(c[0]);
        LVITEMW li{};
        li.mask = LVIF_TEXT;
        li.iItem = (int)r;
        li.pszText = (LPWSTR)first.c_str();
        ListView_InsertItem(A.list, &li);
        for (int k = 1; k < kN; ++k) ListView_SetItemText(A.list, (int)r, k, (LPWSTR)W(c[k]).c_str());
    }
    SendMessageW(A.list, WM_SETREDRAW, TRUE, 0);
    int av = 0;
    for (const auto& a : A.items) if (IsAv(a)) ++av;
    SetWindowTextW(A.info, W(fmt("Программ в реестре: %zu, показано: %zu, антивирусов/защиты: %d.  Оранжевым выделены антивирусы и средства защиты — чаще всего именно они мешают загрузке.", A.items.size(), A.view.size(), av)).c_str());
}

int TargetIdx() { return (int)SendMessageW(A.combo, CB_GETCURSEL, 0, 0); }

void Scan() {
    int t = TargetIdx();
    if (t < 0 || t >= (int)A.targets.size()) { MessageBoxW(A.wnd, TrW(L"Нет офлайн-целей. Подключите диск с Windows и нажмите «Обновить» в главном окне.").c_str(), TrW(L"Установленные приложения").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    if (!A.pinned) { Hives().pin(); A.pinned = true; }
    std::string err;
    A.items = ScanApps(A.targets[(size_t)t], err);
    std::string e2;
    A.startup = ScanAutoruns(A.targets[(size_t)t], e2);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!err.empty()) MessageBoxW(A.wnd, W(err).c_str(), TrW(L"Установленные приложения").c_str(), MB_OK | MB_ICONWARNING);
    Fill();
}

const AppItem* Sel() {
    int s = ListView_GetNextItem(A.list, -1, LVNI_SELECTED);
    return (s >= 0 && s < (int)A.view.size()) ? &A.items[(size_t)A.view[(size_t)s]] : nullptr;
}

std::wstring RelatedText(const AppItem& a, const std::vector<AutoItem>& rel) {
    std::wstring t = TrW(L"Программа: ") + W(a.name) + L"\n";
    t += TrW(L"Папка: ") + (a.location.empty() ? TrW(L"(не определена)") : W(a.location) + (a.dirExists ? L"" : TrW(L"  (нет на диске)"))) + L"\n";
    t += TrW(L"Команда удаления (на живой системе): ") + (a.uninstall.empty() ? TrW(L"(нет)") : W(a.uninstall)) + L"\n\n";
    t += TrW(L"Связанные службы, драйверы и автозапуск: ") + std::to_wstring(rel.size()) + L"\n";
    for (const auto& it : rel) t += L"  • [" + W(it.kind) + L"] " + W(it.name) + L"  —  " + W(it.state) + L"\n";
    return t;
}

void ShowRelated() {
    const AppItem* a = Sel();
    if (!a) { MessageBoxW(A.wnd, TrW(L"Выберите программу.").c_str(), TrW(L"Установленные приложения").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    auto rel = RelatedStartup(*a, A.startup);
    MessageBoxW(A.wnd, RelatedText(*a, rel).c_str(), TrW(L"Что связано с программой").c_str(), MB_OK | MB_ICONINFORMATION);
}

// mode 0: disable startup only, 1: force removal (dialog), 2: only the entry of the programs list
void RemoveApp(int mode) {
    const AppItem* sel = Sel();
    int ti = TargetIdx();
    if (!sel || ti < 0 || ti >= (int)A.targets.size()) { MessageBoxW(A.wnd, TrW(L"Выберите программу.").c_str(), TrW(L"Установленные приложения").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    if (UiRunning()) { MessageBoxW(A.wnd, TrW(L"Дождитесь завершения текущей операции.").c_str(), TrW(L"Занято").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    AppItem app = *sel;
    Target tg = A.targets[(size_t)ti];
    auto rel = RelatedStartup(app, A.startup);
    RemovePlan plan;
    if (mode == 0) { plan.files = 0; plan.deleteEntry = false; }
    else if (mode == 2) { plan.disableStartup = false; plan.files = 0; }
    else {
        std::vector<FormField> f(4);
        f[0].label = L"План (принудительное удаление «офлайн»: деинсталлятор без работающей Windows не запустить)";
        f[0].kind = 1;
        f[0].value = RelatedText(app, rel);
        f[1].label = L"Службы, драйверы, автозапуск программы";
        f[1].kind = 2;
        f[1].options = {L"Отключить (обратимо)", L"Не трогать"};
        f[1].value = f[1].options[0];
        f[2].label = L"Папка программы";
        f[2].kind = 2;
        f[2].options = {L"Переместить в карантин WinRepair_Removed (обратимо)", L"Удалить навсегда", L"Не трогать"};
        f[2].value = f[2].options[0];
        f[3].label = L"Запись в списке программ";
        f[3].kind = 2;
        f[3].options = {L"Удалить (копия .reg сохраняется)", L"Оставить"};
        f[3].value = f[3].options[0];
        if (!FormDialog(A.wnd, L"Принудительное удаление программы", f)) return;
        plan.disableStartup = f[1].value == f[1].options[0];
        plan.files = f[2].value == f[2].options[0] ? 1 : (f[2].value == f[2].options[1] ? 2 : 0);
        plan.deleteEntry = f[3].value == f[3].options[0];
    }
    std::wstring what;
    if (plan.disableStartup) what += L"\n • " + TrW(L"отключить службы/драйверы/автозапуск: ") + std::to_wstring(rel.size());
    if (plan.files == 1) what += L"\n • " + TrW(L"папку программы переместить в карантин: ") + W(app.location);
    if (plan.files == 2) what += L"\n • " + TrW(L"папку программы УДАЛИТЬ НАВСЕГДА: ") + W(app.location);
    if (plan.deleteEntry) what += L"\n • " + TrW(L"удалить запись из списка программ");
    if (what.empty()) { MessageBoxW(A.wnd, TrW(L"Нечего выполнять.").c_str(), TrW(L"Установленные приложения").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    std::wstring msg = TrW(L"Программа: ") + W(app.name) + L"\n" + TrW(L"Целевая ОС: ") + W(tg.label) + L"\n\n" + TrW(L"Будет выполнено:") + what + L"\n\n" +
                       TrW(L"Все изменения записываются в журнал, ветки реестра сохраняются в Backups. Продолжить?");
    if (MessageBoxW(A.wnd, msg.c_str(), TrW(L"Подтверждение").c_str(), MB_YESNO | (plan.files == 2 ? MB_ICONSTOP : MB_ICONWARNING) | MB_DEFBUTTON2) != IDYES) return;
    if (plan.files == 2 && MessageBoxW(A.wnd, TrW(L"Файлы программы будут удалены БЕЗ возможности вернуть.\n\nУверены?").c_str(), TrW(L"Повторное подтверждение").c_str(), MB_YESNO | MB_ICONSTOP | MB_DEFBUTTON2) != IDYES) return;
    g_jobPending = true;
    UiStartJob("apps", "Удаление программы офлайн: " + app.name, [tg, app, plan, rel](Ctx& c) {
        std::vector<std::string> log;
        bool ok = AppRemove(tg, app, plan, rel, log);
        for (const auto& l : log) (l.find("НЕ УДАЛОСЬ") != std::string::npos || l.find("не тронуты") != std::string::npos ? c.warn(l) : c.info(l));
        if (ok) c.ok("Готово: " + app.name); else c.warn("Выполнено с замечаниями: " + app.name);
    }, &tg);
}

void OpenInExplorer() {
    const AppItem* a = Sel();
    if (!a || a->location.empty()) { MessageBoxW(A.wnd, TrW(L"У программы не определена папка установки.").c_str(), TrW(L"Установленные приложения").c_str(), MB_OK | MB_ICONINFORMATION); return; }
    OpenOfflineExplorerAt(a->location);
}

void SaveCsv() {
    wchar_t file[MAX_PATH] = L"installed_apps.csv";
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o; o.hwndOwner = A.wnd; o.lpstrFilter = L"CSV (*.csv)\0*.csv\0"; o.lpstrFile = file; o.nMaxFile = MAX_PATH; o.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&o)) return;
    if (WriteText(U(file), AppsCsv(A.items))) Logger::I().log(Lvl::Ok, "Список программ сохранён: " + U(file));
}

void Layout() {
    RECT rc;
    GetClientRect(A.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8), y = m;
    Place(GetDlgItem(A.wnd, 7900), m, y + S(3), S(100), S(20));
    Place(A.combo, m + S(105), y, W_ - 2 * m - S(105) - S(130), S(300));
    Place(GetDlgItem(A.wnd, ID_SCAN), W_ - m - S(120), y - 1, S(120), S(26));
    y += S(34);
    Place(GetDlgItem(A.wnd, ID_QFLBL), m, y + S(4), S(70), S(20));
    Place(A.qf, m + S(75), y, std::min(S(380), W_ / 3), S(26));
    Place(A.hideUpd, m + S(75) + std::min(S(380), W_ / 3) + S(14), y + S(3), S(360), S(20));
    Place(A.onlyAv, m + S(75) + std::min(S(380), W_ / 3) + S(380), y + S(3), S(280), S(20));
    y += S(32);
    int bh = S(32), rows = 2;
    int listH = H - y - m - rows * (bh + S(6)) - S(30);
    Place(A.list, m, y, W_ - 2 * m, listH);
    y += listH + S(4);
    Place(A.info, m, y, W_ - 2 * m, S(22));
    y += S(26);
    int bw = (W_ - 2 * m - (NB / 2 - 1) * S(6)) / (NB / 2);
    for (int i = 0; i < NB; ++i) Place(A.btn[i], m + (i % (NB / 2)) * (bw + S(6)), y + (i / (NB / 2)) * (bh + S(6)), bw, bh);
}

bool ContextMenu(HWND src, LPARAM lp) {
    if (src != A.list) return false;
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    if (lp == (LPARAM)-1) { RECT r; GetWindowRect(A.list, &r); pt = {r.left + S(20), r.top + S(20)}; }
    else {
        POINT c = pt;
        ScreenToClient(A.list, &c);
        LVHITTESTINFO hi{};
        hi.pt = c;
        int row = ListView_HitTest(A.list, &hi);
        if (row >= 0) ListView_SetItemState(A.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    const AppItem* a = Sel();
    if (!a) return true;
    enum { M_FORCE = 1, M_DIS, M_REL, M_DIR, M_ENTRY, M_WEB, M_COPY, M_ALL };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, M_FORCE, TrW(L"Принудительное удаление…").c_str());
    AppendMenuW(m, MF_STRING, M_DIS, TrW(L"Отключить только запуск (службы, драйверы, автозапуск)…").c_str());
    AppendMenuW(m, MF_STRING, M_ENTRY, TrW(L"Удалить только запись из списка…").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, M_REL, TrW(L"Что связано с программой…").c_str());
    AppendMenuW(m, MF_STRING | (a->location.empty() ? MF_GRAYED : 0), M_DIR, TrW(L"Открыть папку в офлайн-проводнике").c_str());
    AppendMenuW(m, MF_STRING, M_WEB, TrW(L"Поиск в интернете…").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, M_COPY, TrW(L"Копировать\tCtrl+C").c_str());
    AppendMenuW(m, MF_STRING, M_ALL, TrW(L"Копировать всю таблицу").c_str());
    SetForegroundWindow(A.wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, A.wnd, nullptr);
    DestroyMenu(m);
    int si = ListView_GetNextItem(A.list, -1, LVNI_SELECTED);
    switch (c) {
        case M_FORCE: RemoveApp(1); break;
        case M_DIS: RemoveApp(0); break;
        case M_ENTRY: RemoveApp(2); break;
        case M_REL: ShowRelated(); break;
        case M_DIR: OpenInExplorer(); break;
        case M_WEB: {
            std::string q = a->name;
            std::wstring url = L"https://www.google.com/search?q=";
            for (unsigned char ch : q) url += (isalnum(ch) || ch == '.' || ch == '-') ? std::wstring(1, (wchar_t)ch) : W(fmt("%%%02X", ch));
            ShellExecuteW(A.wnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case M_COPY: if (si >= 0) ClipText(A.wnd, ListRowText(A.list, si)); break;
        case M_ALL: { std::wstring all; for (int i = 0, n = ListView_GetItemCount(A.list); i < n; ++i) all += ListRowText(A.list, i) + L"\r\n"; ClipText(A.wnd, all); break; }
    }
    return true;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CONTEXTMENU: return ContextMenu((HWND)wp, lp) ? 1 : 0;
        case WM_SIZE: if (A.btn[NB - 1]) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(980); mm->ptMinTrackSize.y = S(540); return 0; }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == ID_SCAN || (id == ID_TARGET && HIWORD(wp) == CBN_SELCHANGE)) Scan();
            else if (id == ID_QF) { if (HIWORD(wp) == EN_CHANGE) Fill(); }
            else if (id == ID_HIDEUPD || id == ID_ONLYAV) Fill();
            else if (id >= ID_B0 && id < ID_B0 + NB) {
                switch (id - ID_B0) {
                    case 0: ShowRelated(); break;
                    case 1: RemoveApp(1); break;
                    case 2: RemoveApp(0); break;
                    case 3: RemoveApp(2); break;
                    case 4: OpenInExplorer(); break;
                    case 5: OpenAutoruns(); break;
                    case 6: SaveCsv(); break;
                    case 7: DestroyWindow(h); break;
                }
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->hwndFrom == A.list && nh->code == LVN_COLUMNCLICK) {
                int c = ((NMLISTVIEW*)lp)->iSubItem;
                if (c == A.sortCol) A.sortDesc = !A.sortDesc; else { A.sortCol = c; A.sortDesc = false; }
                Fill();
            } else if (nh->hwndFrom == A.list && nh->code == NM_DBLCLK) ShowRelated();
            else if (nh->hwndFrom == A.list && nh->code == LVN_KEYDOWN) {
                WORD k = ((NMLVKEYDOWN*)lp)->wVKey;
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (k == VK_F5) Scan();
                else if (ctrl && k == 'F') SetFocus(A.qf);
                else if (ctrl && k == 'C') { int si = ListView_GetNextItem(A.list, -1, LVNI_SELECTED); if (si >= 0) ClipText(A.wnd, ListRowText(A.list, si)); }
            } else if (nh->hwndFrom == A.list && nh->code == NM_CUSTOMDRAW) {   // security software stands out
                auto* cd = (NMLVCUSTOMDRAW*)lp;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    size_t r = (size_t)cd->nmcd.dwItemSpec;
                    if (r < A.view.size() && !(ListView_GetItemState(A.list, (int)r, LVIS_SELECTED) & LVIS_SELECTED)) {
                        const AppItem& a = A.items[(size_t)A.view[r]];
                        if (IsAv(a)) cd->clrTextBk = RGB(255, 226, 196);
                        else if (a.update || a.system) cd->clrText = RGB(125, 125, 125);
                    }
                    return CDRF_NEWFONT;
                }
                return CDRF_DODEFAULT;
            }
            return 0;
        }
        case WM_DESTROY:
            if (A.pinned) { Hives().unpin(); A.pinned = false; }
            A.wnd = nullptr;
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void OpenAppsImpl() {
    if (A.wnd) { ShowWindow(A.wnd, SW_RESTORE); SetForegroundWindow(A.wnd); return; }
    RegisterOnce(L"WRApps", Proc);
    A = State();
    A.wnd = CreateWindowExW(0, L"WRApps", L"Установленные приложения офлайн-Windows — найти и принудительно удалить программу, мешающую загрузке", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1240), S(720),
                            g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = A.wnd;
    Mk(h, L"STATIC", L"Целевая ОС:", SS_LEFT, 7900);
    A.combo = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_TARGET);
    A.targets = FillTargetCombo(A.combo, true);
    Mk(h, L"BUTTON", L"Сканировать", BS_PUSHBUTTON | WS_TABSTOP, ID_SCAN);
    Mk(h, L"STATIC", L"Фильтр:", SS_LEFT, ID_QFLBL);
    A.qf = Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_QF, WS_EX_CLIENTEDGE);
    SendMessageW(A.qf, EM_SETCUEBANNER, TRUE, (LPARAM)TrW(L"Название, издатель, папка… (Ctrl+F)").c_str());
    A.hideUpd = Mk(h, L"BUTTON", L"Скрыть обновления Windows и системные компоненты", BS_AUTOCHECKBOX | WS_TABSTOP, ID_HIDEUPD);
    SendMessageW(A.hideUpd, BM_SETCHECK, BST_CHECKED, 0);
    A.onlyAv = Mk(h, L"BUTTON", L"Только антивирусы и защита", BS_AUTOCHECKBOX | WS_TABSTOP, ID_ONLYAV);
    A.list = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(A.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    int cw[] = {S(280), S(140), S(170), S(90), S(90), S(80), S(300), S(120)};
    for (int i = 0; i < kN; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)kCols[i]; c.cx = cw[i]; ListView_InsertColumn(A.list, i, &c); }
    A.info = Mk(h, L"STATIC", L"Выберите целевую ОС и нажмите «Сканировать».", SS_LEFT, ID_INFO);
    const wchar_t* bt[] = {L"Что связано…", L"Принудительное удаление…", L"Отключить только запуск…", L"Удалить только запись…", L"Папка в проводнике…", L"Службы и автозагрузка…", L"Экспорт CSV…", L"Закрыть"};
    for (int i = 0; i < NB; ++i) A.btn[i] = Mk(h, L"BUTTON", bt[i], BS_PUSHBUTTON | WS_TABSTOP, ID_B0 + i);
    Layout();
    ShowWindow(h, SW_SHOW);
    if (!A.targets.empty()) PostMessageW(h, WM_COMMAND, MAKEWPARAM(ID_SCAN, BN_CLICKED), 0);
}

void AppsJobDone() { if (g_jobPending && A.wnd) { g_jobPending = false; Scan(); } }

}  // namespace ui
}  // namespace wr
