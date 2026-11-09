// "BitLocker и драйверы хранилища": unlock encrypted volumes, load / inject storage drivers (Intel VMD/RST, NVMe, RAID).
#include "ui_tools_common.h"
#include "modules/access.h"

namespace wr {
namespace ui {

namespace {

enum { ID_LIST = 9001, ID_B0 = 9100, ID_INFO = 9200 };

struct St {
    HWND wnd = nullptr, list, info, btn[13];
    std::vector<BlVolume> vols;
};
St Q;
std::vector<std::string> g_infs;  // last search result

char SelLetter() {
    int s = ListView_GetNextItem(Q.list, -1, LVNI_SELECTED);
    return (s >= 0 && s < (int)Q.vols.size()) ? Q.vols[(size_t)s].letter : 0;
}

void Scan() {
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    Q.vols = BlScan();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    ListView_DeleteAllItems(Q.list);
    for (size_t i = 0; i < Q.vols.size(); ++i) {
        const auto& v = Q.vols[i];
        std::wstring l = W(std::string(1, v.letter) + ":");
        LVITEMW li{};
        li.mask = LVIF_TEXT;
        li.iItem = (int)i;
        li.pszText = (LPWSTR)l.c_str();
        ListView_InsertItem(Q.list, &li);
        std::string lock = v.known ? (v.locked ? "ЗАБЛОКИРОВАН" : "разблокирован / нет шифрования") : (v.locked ? "ЗАБЛОКИРОВАН (по доступу к ФС)" : "нет данных");
        ListView_SetItemText(Q.list, (int)i, 1, (LPWSTR)W(lock).c_str());
        ListView_SetItemText(Q.list, (int)i, 2, (LPWSTR)W(v.protection).c_str());
        ListView_SetItemText(Q.list, (int)i, 3, (LPWSTR)W(v.conversion).c_str());
    }
    int locked = 0;
    for (auto& v : Q.vols) if (v.locked) ++locked;
    std::wstring t = BlToolPresent() ? W(fmt("Томов: %zu, заблокировано: %d. Разблокировка нужна, если Windows на диске не находится в списке «Целевая ОС».", Q.vols.size(), locked))
                                     : std::wstring(L"manage-bde.exe не найден в этой среде (нужен компонент BitLocker в WinPE/WinRE). Используйте WinRE или образ WinPE с поддержкой BitLocker.");
    SetWindowTextW(Q.info, t.c_str());
}

bool Busy() {
    if (!UiRunning()) return false;
    MessageBoxW(Q.wnd, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION);
    return true;
}

bool Ask(const std::wstring& t, bool warn) { return MessageBoxW(Q.wnd, (t + L"\n\nПродолжить?").c_str(), L"Подтверждение", MB_YESNO | (warn ? MB_ICONWARNING : MB_ICONQUESTION) | MB_DEFBUTTON2) == IDYES; }

void Unlock() {
    if (Busy()) return;
    std::vector<FormField> f(4);
    f[0].label = L"Том";
    f[0].kind = 2;
    for (auto& v : Q.vols) f[0].options.push_back(W(std::string(1, v.letter) + ":"));
    char sel = SelLetter();
    for (auto& v : Q.vols) if (v.locked && (!sel || !Q.vols.empty())) { if (!sel) sel = v.letter; }
    f[0].value = sel ? W(std::string(1, sel) + ":") : L"";
    f[1].label = L"Способ";
    f[1].kind = 2;
    f[1].options = {L"Пароль восстановления (48 цифр)", L"Пароль", L"Файл ключа (.bek)"};
    f[1].value = f[1].options[0];
    f[2].label = L"Пароль восстановления / пароль (вводится скрыто, в журнал не пишется)";
    f[2].kind = 4;
    f[3].label = L"Путь к файлу ключа .bek (только для способа «Файл ключа»)";
    if (!FormDialog(Q.wnd, L"Разблокировать том BitLocker", f)) return;
    std::string vol = U(f[0].value);
    if (vol.empty()) return;
    int kind = f[1].value == f[1].options[0] ? 0 : f[1].value == f[1].options[1] ? 1 : 2;
    std::string secret = kind == 2 ? trim(U(f[3].value)) : U(f[2].value);
    char L = vol[0];
    if (!Ask(L"Разблокировать том " + W(vol) + L" (способ: " + f[1].value + L").", false)) return;
    UiStartJob("bl-unlock", "BitLocker: разблокировка " + vol, [L, kind, secret](Ctx& c) { BlUnlock(c, L, kind, secret); });
}

void Bde(const char* title, const char* args, bool needConfirm, bool warn, const wchar_t* why) {
    if (Busy()) return;
    char L = SelLetter();
    if (!L) { MessageBoxW(Q.wnd, L"Выберите том в списке.", L"BitLocker", MB_OK | MB_ICONINFORMATION); return; }
    std::string vol = std::string(1, L) + ":";
    std::string a = args;
    size_t p = a.find("%V");
    if (p != std::string::npos) a.replace(p, 2, vol);
    if (needConfirm && !Ask(std::wstring(why) + L"\nТом: " + W(vol) + L"\nКоманда: manage-bde " + W(a), warn)) return;
    std::string t = std::string(title) + " " + vol;
    if (needConfirm) AuditChange("BitLocker: manage-bde " + a);
    UiStartJob("bl-cmd", t, [t, a](Ctx& c) { BlRunSafe(c, t, a); });
}

void NoDriver() {
    if (Busy()) return;
    UiStartJob("nodriver", "Устройства без драйвера", [](Ctx& c) { OutBuf ob; NatNoDriverDevices(c, ob); c.saveText("nodriver.txt", ob.text); });
}

void FindInf() {
    if (Busy()) return;
    std::vector<FormField> f(1);
    f[0].label = L"Папка для поиска INF драйверов хранилища (SCSIAdapter/HDC: Intel RST/VMD, AHCI, NVMe, RAID)";
    f[0].kind = 3;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) if (mask & (1u << i)) f[0].options.push_back(std::wstring(1, (wchar_t)('A' + i)) + L":\\");
    f[0].value = f[0].options.empty() ? L"" : f[0].options.back();
    if (!FormDialog(Q.wnd, L"Найти драйверы хранилища", f)) return;
    std::string folder = trim(U(f[0].value));
    if (folder.empty()) return;
    UiStartJob("findinf", "Поиск INF драйверов хранилища", [folder](Ctx& c) {
        c.info("Поиск в " + folder + " (до 60 результатов)…");
        auto r = FindStorageInfs(folder, c.cancel);
        g_infs = r;
        for (auto& p : r) Logger::I().log(Lvl::Out, "  | " + p);
        c.ok(fmt("Найдено INF: %zu. Используйте «Загрузить драйвер в WinPE (drvload)».", r.size()));
    });
}

void DrvLoad() {
    if (Busy()) return;
    std::vector<FormField> f(1);
    f[0].label = L"Путь к INF драйвера для загрузки в ТЕКУЩУЮ WinPE/WinRE (drvload)";
    f[0].kind = 3;
    for (auto& p : g_infs) f[0].options.push_back(W(p));
    f[0].value = g_infs.empty() ? L"" : W(g_infs[0]);
    if (!FormDialog(Q.wnd, L"drvload", f)) return;
    std::string inf = trim(U(f[0].value));
    if (inf.empty() || !PathExists(inf)) { MessageBoxW(Q.wnd, L"INF не найден.", L"drvload", MB_OK | MB_ICONWARNING); return; }
    if (!Ask(L"Загрузить драйвер в работающую среду:\n" + W(inf) + L"\n\nДиск, скрытый за контроллером, должен появиться (затем обновится список целевых ОС). Система целевого диска не изменяется.", false)) return;
    UiStartJob("drvload", "drvload", [inf](Ctx& c) {
        AuditChange("drvload " + inf);
        c.run("drvload", "\"" + SysTool("drvload.exe") + "\" \"" + inf + "\"", 90);
    });
}

void Inject() {
    if (Busy()) return;
    std::vector<Target> offs;
    for (auto& t : UiTargets()) if (!t.online) offs.push_back(t);
    if (offs.empty()) { MessageBoxW(Q.wnd, L"Нет офлайн-систем. Сначала разблокируйте том/загрузите драйвер и обновите список.", L"Драйверы", MB_OK | MB_ICONINFORMATION); return; }
    std::vector<FormField> f(4);
    f[0].label = L"Целевая ОС (офлайн)";
    f[0].kind = 2;
    for (auto& t : offs) f[0].options.push_back(W(t.label));
    f[0].value = f[0].options[0];
    f[1].label = L"Путь к INF или папке с драйверами";
    f[1].kind = 3;
    for (auto& p : g_infs) f[1].options.push_back(W(p));
    f[1].value = g_infs.empty() ? L"" : W(g_infs[0]);
    f[2].label = L"Рекурсивно (для папки)";
    f[2].kind = 2;
    f[2].options = {L"Да", L"Нет"};
    f[2].value = L"Да";
    f[3].label = L"Разрешить неподписанные (/ForceUnsigned)";
    f[3].kind = 2;
    f[3].options = {L"Нет", L"Да"};
    f[3].value = L"Нет";
    if (!FormDialog(Q.wnd, L"Установить драйвер в офлайн-Windows (DISM /Add-Driver)", f)) return;
    Target tg;
    for (auto& t : offs) if (W(t.label) == f[0].value) tg = t;
    std::string path = trim(U(f[1].value));
    if (!tg.valid || path.empty() || path.find('"') != std::string::npos || !PathExists(path)) { MessageBoxW(Q.wnd, L"Проверьте цель и путь к драйверу.", L"Драйверы", MB_OK | MB_ICONWARNING); return; }
    bool rec = f[2].value == L"Да", uns = f[3].value == L"Да";
    if (!Ask(L"В образ ОС " + W(tg.label) + L" будет добавлен драйвер:\n" + W(path) + L"\n\nЭто ИЗМЕНЕНИЕ целевой Windows (нужно, например, для 0x7B после смены режима SATA/VMD).", true)) return;
    UiStartJob("addriver", "DISM /Add-Driver", [tg, path, rec, uns](Ctx& c) {
        std::string scr = c.sub("scratch");
        scr.pop_back();
        std::string cmd = "\"" + SysTool("dism.exe") + "\" /Image:" + tg.root + " /Add-Driver /Driver:\"" + path + "\"" + (rec ? " /Recurse" : "") + (uns ? " /ForceUnsigned" : "") + " /ScratchDir:\"" + scr + "\"";
        AuditChange("DISM Add-Driver в " + tg.root + ": " + path);
        c.run("DISM /Add-Driver", cmd);
    }, &tg);
}

void ListDrivers() {
    if (Busy()) return;
    Target t = UiCurrentTarget();
    if (!t.valid) { MessageBoxW(Q.wnd, L"Выберите целевую ОС в главном окне.", L"Драйверы", MB_OK | MB_ICONINFORMATION); return; }
    UiStartJob("getdrivers", "DISM /Get-Drivers", [](Ctx& c) {
        std::string scr = c.sub("scratch");
        scr.pop_back();
        std::string cmd = "\"" + SysTool("dism.exe") + "\"" + (c.target.online ? std::string(" /Online") : " /Image:" + c.target.root + " /ScratchDir:\"" + scr + "\"") + " /Get-Drivers /all";
        ProcResult r = c.run("DISM /Get-Drivers", cmd);
        c.saveText("drivers-dism.txt", r.output);
    });
}

void Layout() {
    RECT rc;
    GetClientRect(Q.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8), bh = S(30);
    int bw = (W_ - 2 * m - 2 * S(6)) / 3;
    int blRows = 3, drvRows = 3;
    int listH = H - 2 * m - S(24) - S(22) - (blRows + drvRows) * (bh + S(6)) - S(24);
    Place(Q.list, m, m, W_ - 2 * m, std::max(S(100), listH));
    int y = m + std::max(S(100), listH) + m;
    Place(Q.info, m, y, W_ - 2 * m, S(36));
    y += S(40);
    for (int i = 0; i < 7; ++i) Place(Q.btn[i], m + (i % 3) * (bw + S(6)), y + (i / 3) * (bh + S(6)), bw, bh);
    y += blRows * (bh + S(6)) + S(8);
    for (int i = 7; i < 13; ++i) Place(Q.btn[i], m + ((i - 7) % 3) * (bw + S(6)), y + ((i - 7) / 3) * (bh + S(6)), bw, bh);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (Q.btn[12]) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(860); mm->ptMinTrackSize.y = S(560); return 0; }
        case WM_COMMAND:
            if (LOWORD(wp) >= ID_B0 && LOWORD(wp) < ID_B0 + 13) {
                switch (LOWORD(wp) - ID_B0) {
                    case 0: Scan(); break;
                    case 1: Unlock(); break;
                    case 2: Bde("manage-bde -status", "-status %V", false, false, L""); break;
                    case 3: Bde("manage-bde -protectors -get", "-protectors -get %V", false, false, L""); break;
                    case 4: Bde("Приостановить защиту BitLocker", "-protectors -disable %V -RebootCount 1", true, true, L"Защита будет приостановлена на 1 перезагрузку (удобно перед правкой BIOS/BCD/загрузчика)."); break;
                    case 5: Bde("Возобновить защиту BitLocker", "-protectors -enable %V", true, false, L"Защита BitLocker будет включена снова."); break;
                    case 6: Bde("Заблокировать том", "-lock %V -ForceDismount", true, true, L"Том будет заблокирован и отключён от системы."); break;
                    case 7: NoDriver(); break;
                    case 8: FindInf(); break;
                    case 9: DrvLoad(); break;
                    case 10: Inject(); break;
                    case 11: ListDrivers(); break;
                    case 12: DestroyWindow(h); break;
                }
            }
            return 0;
        case WM_DESTROY: Q.wnd = nullptr; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void OpenStorageAccessImpl() {
    if (Q.wnd) { ShowWindow(Q.wnd, SW_RESTORE); SetForegroundWindow(Q.wnd); return; }
    RegisterOnce(L"WRStorage", Proc);
    Q = St();
    Q.wnd = CreateWindowExW(0, L"WRStorage", L"Доступ к диску: BitLocker и драйверы хранилища", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1000), S(720),
                            g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = Q.wnd;
    Q.list = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(Q.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const wchar_t* cols[] = {L"Том", L"Блокировка", L"Защита", L"Преобразование"};
    int cw[] = {S(70), S(300), S(200), S(300)};
    for (int i = 0; i < 4; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = cw[i]; ListView_InsertColumn(Q.list, i, &c); }
    Q.info = Mk(h, L"STATIC", L"", SS_LEFT, ID_INFO);
    const wchar_t* bt[] = {L"Сканировать тома", L"Разблокировать…", L"Статус тома (в журнал)", L"Защитники тома (ключи скрыты)", L"Приостановить защиту…", L"Возобновить защиту…", L"Заблокировать том…",
                           L"Устройства без драйвера (Hardware ID)", L"Найти INF драйверов хранилища…", L"Загрузить драйвер в WinPE (drvload)…", L"Установить драйвер в офлайн-ОС (DISM)…", L"Драйверы целевой ОС (DISM)", L"Закрыть"};
    for (int i = 0; i < 13; ++i) Q.btn[i] = Mk(h, L"BUTTON", bt[i], BS_PUSHBUTTON | WS_TABSTOP, ID_B0 + i);
    Layout();
    ShowWindow(h, SW_SHOW);
    Scan();
}

void StorageJobDone() { if (Q.wnd) Scan(); }

}  // namespace ui
}  // namespace wr
