// WinRE manager: enter WinRE on next boot (online and from WinPE), build "WinRE + WinRepair", save logs to disk.
#include "ui_tools_common.h"
#include "modules/winre.h"

namespace wr {
namespace ui {

namespace {

enum { ID_TARGET = 8001, ID_STORE, ID_FIND, ID_STATUS, ID_B0 = 8100 };

struct State {
    HWND wnd = nullptr, target, store, find, status, btn[10];
    std::vector<Target> targets;
    std::vector<BcdStore> stores;
    Target tgt;
    WinReInfo info;
    std::string storePath, entryId, customId;
    std::vector<BcdEntry> entries;
};
State V;

std::string CurStore() {
    if (V.tgt.valid && V.tgt.online) return "";
    int s = (int)SendMessageW(V.store, CB_GETCURSEL, 0, 0);
    return (s >= 0 && s < (int)V.stores.size()) ? V.stores[(size_t)s].path : "";
}

void Rescan() {
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    V.stores.clear();
    for (auto& s : FindBcdStores()) if (!s.path.empty()) V.stores.push_back(s);
    SendMessageW(V.store, CB_RESETCONTENT, 0, 0);
    int pick = 0, tdisk = V.tgt.valid ? VolumeDiskNumber(V.tgt.letter()) : -1;
    for (size_t i = 0; i < V.stores.size(); ++i) {
        SendMessageW(V.store, CB_ADDSTRING, 0, (LPARAM)W(V.stores[i].label).c_str());
        if (tdisk >= 0 && VolumeDiskNumber(V.stores[i].path[0]) == tdisk) { pick = (int)i; tdisk = -1; }
    }
    SendMessageW(V.store, CB_SETCURSEL, V.stores.empty() ? -1 : pick, 0);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

void Refresh() {
    int t = (int)SendMessageW(V.target, CB_GETCURSEL, 0, 0);
    if (t < 0 || t >= (int)V.targets.size()) { SetWindowTextW(V.status, L"Целевая ОС не выбрана."); return; }
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    V.tgt = V.targets[(size_t)t];
    EnableWindow(V.store, !V.tgt.online);
    EnableWindow(V.find, !V.tgt.online);
    V.info = ReadWinReInfo(V.tgt);
    V.storePath = CurStore();
    V.entries.clear();
    V.entryId.clear();
    V.customId.clear();
    std::string raw;
    if (V.tgt.online || !V.storePath.empty()) {
        V.entries = BcdEnum(V.storePath, raw);
        V.entryId = FindWinReEntry(V.entries, V.info.bcdId, false);
        V.customId = FindWinReEntry(V.entries, "", true);
    }
    ULARGE_INTEGER fb{}, tb{}, tf{};
    GetDiskFreeSpaceExW(W(V.tgt.root).c_str(), &fb, &tb, &tf);
    unsigned long long need = V.info.wimSize * 2 + (300ULL << 20);
    std::string s = "Целевая ОС: " + V.tgt.label + "\r\n\r\n";
    s += "ReAgent.xml: " + (V.info.xmlFound ? V.info.xmlPath : std::string("не найден — WinRE этой ОС не настроен")) + "\r\n";
    s += std::string("Состояние WinRE по данным ОС: ") + (V.info.installState == "1" ? "включён" : V.info.installState == "0" ? "ВЫКЛЮЧЕН" : "не определено") + "\r\n";
    s += "BCD-идентификатор из ReAgent.xml: " + (V.info.bcdId.empty() ? std::string("—") : V.info.bcdId) + "\r\n";
    s += "Winre.wim: " + (V.info.wimPath.empty() ? std::string("НЕ НАЙДЕН (ни на томе ОС, ни на разделе восстановления)") : V.info.wimPath + "  (" + fmtBytes(V.info.wimSize) + ")") + "\r\n";
    s += "Хранилище BCD: " + (V.tgt.online ? std::string("текущей системы (по умолчанию)") : V.storePath.empty() ? std::string("НЕ ВЫБРАНО — нажмите «Найти BCD»") : V.storePath) + "\r\n";
    s += "Запись WinRE в BCD: " + (V.entryId.empty() ? std::string("отсутствует") : V.entryId) + "\r\n";
    s += "Запись «Windows RE + WinRepair»: " + (V.customId.empty() ? std::string("не создана") : V.customId) + "\r\n";
    s += "Свободно на " + V.tgt.root + ": " + fmtBytes(fb.QuadPart) + (V.info.wimSize ? ", для WinRE + WinRepair нужно около " + fmtBytes(need) : std::string()) + "\r\n\r\n";
    if (V.entryId.empty() && !V.info.wimPath.empty()) s += "→ Запись WinRE отсутствует, но образ найден: кнопка «Создать/восстановить запись WinRE» или вход в WinRE создадут её сами.\r\n";
    if (V.info.wimPath.empty()) s += "→ Образа нет: WinRE не удалось бы запустить даже штатно. Образ можно извлечь из install.wim установочного носителя (Windows\\System32\\Recovery\\Winre.wim).\r\n";
    s += "→ Вход в WinRE «при следующей загрузке» выполняется ОДИН раз (BCD bootsequence), затем система загружается как обычно.\r\n";
    s += "→ «Windows RE + WinRepair» — копия вашего winre.wim с добавленной программой: она запускается первой, а после выхода открывается штатное меню WinRE. Оригинальный WinRE не изменяется.\r\n";
    SetWindowTextW(V.status, W(s).c_str());
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

bool Confirm(const std::wstring& t, bool danger) {
    return MessageBoxW(V.wnd, (t + L"\n\nПродолжить?").c_str(), L"Подтверждение", MB_YESNO | (danger ? MB_ICONWARNING : MB_ICONQUESTION) | MB_DEFBUTTON2) == IDYES;
}

bool BackupFirst() {
    std::string err, b = BcdBackup(V.storePath, err);
    if (b.empty()) return MessageBoxW(V.wnd, W("Не удалось сделать копию BCD: " + err + "\n\nПродолжить БЕЗ копии?").c_str(), L"Копия не создана", MB_YESNO | MB_ICONSTOP | MB_DEFBUTTON2) == IDYES;
    Logger::I().log(Lvl::Ok, "Копия BCD: " + b);
    return true;
}

bool NeedStore() {
    if (V.tgt.online || !V.storePath.empty()) return true;
    MessageBoxW(V.wnd, L"Выберите хранилище BCD (кнопка «Найти BCD»).", L"WinRE", MB_OK | MB_ICONINFORMATION);
    return false;
}

void Enter(bool reboot, bool custom) {
    if (UiRunning()) { MessageBoxW(V.wnd, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
    if (!V.tgt.valid || !NeedStore()) return;
    Target tg = V.tgt;
    std::string store = V.storePath;
    std::wstring what = custom ? L"Войти в «Windows RE + WinRepair»" : L"Войти в WinRE";
    what += reboot ? L" и ПЕРЕЗАГРУЗИТЬ СЕЙЧАС." : L" при СЛЕДУЮЩЕЙ загрузке (один раз).";
    if (tg.online && !custom) {
        if (!Confirm(what + L"\n\nБудет выполнено: reagentc /boottore" + (reboot ? L" и перезагрузка." : L"."), reboot)) return;
        UiStartJob("winre-once", "WinRE: вход при следующей загрузке", [reboot](Ctx& c) {
            AuditChange("WinRE: reagentc /boottore");
            ProcResult r = c.run("reagentc /boottore", "\"" + SysTool("reagentc.exe") + "\" /boottore", 60);
            if (r.exitCode != 0) { c.error("reagentc /boottore не удался — вероятно WinRE выключен. Используйте «Создать/восстановить запись WinRE» или reagentc /enable."); return; }
            c.ok("Следующая загрузка — в WinRE.");
            if (reboot) c.run("Перезагрузка", RebootCommand(), 30);
        }, &tg);
        return;
    }
    std::string id = custom ? V.customId : V.entryId;
    if (custom && id.empty()) { MessageBoxW(V.wnd, L"Запись «Windows RE + WinRepair» не создана. Сначала нажмите «WinRE + WinRepair: создать».", L"WinRE", MB_OK | MB_ICONINFORMATION); return; }
    if (id.empty() && V.info.wimPath.empty()) { MessageBoxW(V.wnd, L"Нет ни записи WinRE в BCD, ни файла Winre.wim — войти в WinRE невозможно.", L"WinRE", MB_OK | MB_ICONWARNING); return; }
    std::wstring msg = what + L"\n\nХранилище BCD: " + (store.empty() ? std::wstring(L"текущей системы") : W(store));
    msg += id.empty() ? L"\nЗапись WinRE отсутствует и будет создана из:\n" + W(V.info.wimPath) : L"\nЗапись: " + W(id);
    if (!Confirm(msg, reboot)) return;
    if (!BackupFirst()) return;
    WinReInfo info = V.info;
    UiStartJob("winre-once", "WinRE: вход при следующей загрузке", [=](Ctx& c) {
        std::string eid = id;
        if (eid.empty()) {
            std::string sdi = WinReSdiFor(info.wimPath, c.target);
            if (sdi.empty()) { c.error("Не найден boot.sdi для RAM-диска WinRE."); return; }
            if (!CreateRamdiskEntry(c, store, "Windows Recovery Environment", info.wimPath, sdi, eid)) return;
        }
        JobBootOnce(c, store, eid, reboot);
    }, &tg);
}

void CreateEntry() {
    if (UiRunning() || !V.tgt.valid || !NeedStore()) return;
    if (!V.entryId.empty()) { MessageBoxW(V.wnd, W("Запись WinRE уже существует: " + V.entryId).c_str(), L"WinRE", MB_OK | MB_ICONINFORMATION); return; }
    if (V.info.wimPath.empty()) { MessageBoxW(V.wnd, L"Winre.wim не найден.", L"WinRE", MB_OK | MB_ICONWARNING); return; }
    if (!Confirm(L"Создать запись загрузки WinRE (RAM-диск) из:\n" + W(V.info.wimPath), true) || !BackupFirst()) return;
    Target tg = V.tgt;
    std::string store = V.storePath;
    WinReInfo info = V.info;
    UiStartJob("winre-entry", "WinRE: создать запись BCD", [=](Ctx& c) {
        std::string sdi = WinReSdiFor(info.wimPath, c.target), id;
        if (sdi.empty()) { c.error("Не найден boot.sdi."); return; }
        if (CreateRamdiskEntry(c, store, "Windows Recovery Environment", info.wimPath, sdi, id)) c.ok("Запись WinRE создана: " + id);
    }, &tg);
}

void BuildCustom() {
    if (UiRunning() || !V.tgt.valid || !NeedStore()) return;
    if (V.info.wimPath.empty()) { MessageBoxW(V.wnd, L"Winre.wim не найден — не из чего собрать образ.", L"WinRE", MB_OK | MB_ICONWARNING); return; }
    if (!V.customId.empty()) { MessageBoxW(V.wnd, L"«Windows RE + WinRepair» уже создан. Удалите его, чтобы пересоздать.", L"WinRE", MB_OK | MB_ICONINFORMATION); return; }
    std::wstring msg = L"Будет создана копия WinRE с программой WinRepair:\n • копия " + W(V.info.wimPath) + L" → " + W(V.tgt.root) + L"Recovery\\WinRepairRE\\WinRE_WR.wim\n • образ монтируется DISM, добавляется WinRepair.exe и winpeshl.ini (WinRepair → штатное меню WinRE), коммит\n • создаётся отдельная запись BCD (оригинальный WinRE не меняется)\n\nНужно около " + W(fmtBytes(V.info.wimSize * 2 + (300ULL << 20))) + L" свободного места. Операция займёт несколько минут.";
    if (!Confirm(msg, true) || !BackupFirst()) return;
    Target tg = V.tgt;
    std::string store = V.storePath, wim = V.info.wimPath;
    UiStartJob("winre-build", "WinRE + WinRepair: создание", [=](Ctx& c) { JobBuildWinRepairRE(c, store, wim); }, &tg);
}

void RemoveCustom() {
    if (UiRunning() || !V.tgt.valid || !NeedStore()) return;
    if (!Confirm(L"Удалить запись «Windows RE + WinRepair» и папку Recovery\\WinRepairRE на " + W(V.tgt.root) + L"?\nОригинальный WinRE не затрагивается.", true) || !BackupFirst()) return;
    Target tg = V.tgt;
    std::string store = V.storePath;
    UiStartJob("winre-remove", "WinRE + WinRepair: удаление", [=](Ctx& c) { JobRemoveWinRepairRE(c, store); }, &tg);
}

void CancelOnce() {
    if (UiRunning() || !NeedStore()) return;
    if (!Confirm(L"Отменить запланированный разовый вход (удалить bootsequence)?", false)) return;
    Target tg = V.tgt;
    std::string store = V.storePath;
    UiStartJob("winre-cancel", "BCD: отмена разового входа", [=](Ctx& c) {
        AuditChange("BCD: удалён bootsequence");
        c.run("bcdedit /deletevalue {bootmgr} bootsequence", BcdPrefix(store) + " /deletevalue {bootmgr} bootsequence", 30);
    }, &tg);
}

void CheckWim() {
    if (UiRunning()) return;
    if (V.info.wimPath.empty()) { MessageBoxW(V.wnd, L"Winre.wim не найден.", L"WinRE", MB_OK | MB_ICONWARNING); return; }
    std::string wim = V.info.wimPath;
    Target tg = V.tgt;
    UiStartJob("winre-check", "Проверка winre.wim", [wim](Ctx& c) { c.run("DISM /Get-WimInfo", "\"" + SysTool("dism.exe") + "\" /Get-WimInfo /WimFile:\"" + wim + "\"", 120); }, &tg);
}

void Layout() {
    RECT rc;
    GetClientRect(V.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8), y = m;
    Place(GetDlgItem(V.wnd, 8900), m, y + S(3), S(100), S(20));
    Place(V.target, m + S(105), y, W_ - 2 * m - S(105), S(300));
    y += S(32);
    Place(GetDlgItem(V.wnd, 8901), m, y + S(3), S(100), S(20));
    Place(V.store, m + S(105), y, W_ - 2 * m - S(105) - S(120), S(300));
    Place(V.find, W_ - m - S(112), y - 1, S(112), S(26));
    y += S(34);
    int bh = S(32), rows = 5;
    int stH = H - y - m - rows * (bh + S(6));
    Place(V.status, m, y, W_ - 2 * m, stH);
    y += stH + m;
    int bw = (W_ - 2 * m - S(8)) / 2;
    for (int i = 0; i < 10; ++i) Place(V.btn[i], m + (i % 2) * (bw + S(8)), y + (i / 2) * (bh + S(6)), bw, bh);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (V.btn[9]) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(860); mm->ptMinTrackSize.y = S(600); return 0; }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == ID_TARGET && code == CBN_SELCHANGE) { Refresh(); }
            else if (id == ID_STORE && code == CBN_SELCHANGE) Refresh();
            else if (id == ID_FIND) { Rescan(); Refresh(); }
            else if (id >= ID_B0 && id < ID_B0 + 10) {
                switch (id - ID_B0) {
                    case 0: Refresh(); break;
                    case 1: Enter(false, false); break;
                    case 2: Enter(true, false); break;
                    case 3: CreateEntry(); break;
                    case 4: BuildCustom(); break;
                    case 5: Enter(false, true); break;
                    case 6: RemoveCustom(); break;
                    case 7: CancelOnce(); break;
                    case 8: CheckWim(); break;
                    case 9: DestroyWindow(h); break;
                }
            }
            return 0;
        }
        case WM_DESTROY: V.wnd = nullptr; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void OpenWinReImpl() {
    if (V.wnd) { ShowWindow(V.wnd, SW_RESTORE); SetForegroundWindow(V.wnd); return; }
    RegisterOnce(L"WRWinRE", Proc);
    V = State();
    V.wnd = CreateWindowExW(0, L"WRWinRE", L"WinRE: вход в среду восстановления и интеграция с WinRepair", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1000), S(760),
                            g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = V.wnd;
    Mk(h, L"STATIC", L"Целевая ОС:", SS_LEFT, 8900);
    V.target = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_TARGET);
    V.targets = FillTargetCombo(V.target, false);
    Mk(h, L"STATIC", L"Хранилище BCD:", SS_LEFT, 8901);
    V.store = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_STORE);
    V.find = Mk(h, L"BUTTON", L"Найти BCD", BS_PUSHBUTTON | WS_TABSTOP, ID_FIND);
    V.status = Mk(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, ID_STATUS, WS_EX_CLIENTEDGE);
    SendMessageW(V.status, WM_SETFONT, (WPARAM)g_ui.mono, TRUE);
    const wchar_t* bt[] = {L"Обновить статус", L"Войти в WinRE при следующей загрузке…", L"Войти в WinRE и перезагрузить сейчас…", L"Создать/восстановить запись WinRE в BCD…",
                           L"WinRE + WinRepair: создать…", L"Войти в WinRE + WinRepair (следующая загрузка)…", L"WinRE + WinRepair: удалить…", L"Отменить разовый вход",
                           L"Проверить winre.wim (DISM)", L"Закрыть"};
    for (int i = 0; i < 10; ++i) V.btn[i] = Mk(h, L"BUTTON", bt[i], BS_PUSHBUTTON | WS_TABSTOP, ID_B0 + i);
    Layout();
    ShowWindow(h, SW_SHOW);
    if (V.targets.empty()) { SetWindowTextW(V.status, L"Целевые ОС не найдены."); return; }
    Refresh();
    // offline target: find stores right away so the status can show the WinRE entry
    if (!V.targets[0].online) { Rescan(); Refresh(); }
}

void WinReJobDone() { if (V.wnd) Refresh(); }

void OpenSaveDataImpl() {
    if (UiRunning()) { MessageBoxW(g_ui.main, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
    Target t = UiCurrentTarget();
    std::vector<FormField> f(1);
    f[0].label = L"Куда сохранить журналы, отчёты и копии (папка Data программы). В WinPE/WinRE ОЗУ-диск пропадёт при перезагрузке!";
    f[0].kind = 3;
    for (auto& x : UiTargets()) f[0].options.push_back(W(x.root + "WinRepair_Data\\" + Stamp()));
    f[0].value = t.valid ? W(t.root + "WinRepair_Data\\" + Stamp()) : L"D:\\WinRepair_Data";
    if (!FormDialog(g_ui.main, L"Сохранить Data на диск", f)) return;
    std::string dest = trim(U(f[0].value));
    while (!dest.empty() && dest.back() == '\\') dest.pop_back();
    if (dest.size() < 3) return;
    std::string src = DataDir();
    while (!src.empty() && src.back() == '\\') src.pop_back();
    UiStartJob("savedata", "Сохранение Data на диск", [src, dest](Ctx& c) {
        int files = 0;
        bool ok = CopyTree(src, dest, files, c.cancel);
        AuditChange(fmt("Data сохранена: %s -> %s (%d файлов)", src.c_str(), dest.c_str(), files));
        if (ok) c.ok(fmt("Сохранено файлов: %d в %s", files, dest.c_str()));
        else c.error("Копирование не удалось.");
    });
}

}  // namespace ui
}  // namespace wr
