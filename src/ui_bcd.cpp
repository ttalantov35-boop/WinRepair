// BCD manager: choose a store (ESP is mounted temporarily when needed), edit entries, boot modes, bcdboot.
#include "ui_tools_common.h"
#include "modules/bcd.h"

#include <shellapi.h>

namespace wr {
namespace ui {

namespace {

enum { ID_STORE = 5001, ID_REFRESH, ID_LIST, ID_DETAIL, ID_MODE, ID_APPLYMODE, ID_RESETMODES, ID_B0 = 5100 };

struct State {
    HWND wnd = nullptr, store, list, detail, mode, bApply, bReset, btn[12];
    std::vector<BcdStore> stores;
    std::vector<BcdEntry> entries;
    std::string raw;
};
State B;

std::string CurStore() {
    int s = (int)SendMessageW(B.store, CB_GETCURSEL, 0, 0);
    return (s >= 0 && s < (int)B.stores.size()) ? B.stores[(size_t)s].path : "";
}
const BcdEntry* CurEntry() {
    int s = (int)SendMessageW(B.list, LB_GETCURSEL, 0, 0);
    return (s >= 0 && s < (int)B.entries.size()) ? &B.entries[(size_t)s] : nullptr;
}

void ShowDetail() {
    const BcdEntry* e = CurEntry();
    std::string t;
    if (e) {
        t = e->heading + "\r\n";
        for (const auto& p : e->props) t += p.key + std::string(p.key.size() < 24 ? 24 - p.key.size() : 1, ' ') + p.value + "\r\n";
    } else t = B.raw.empty() ? "Записи не найдены. Проверьте выбранное хранилище." : "";
    SetWindowTextW(B.detail, W(t).c_str());
}

void LoadEntries() {
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    B.entries = BcdEnum(CurStore(), B.raw);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    SendMessageW(B.list, LB_RESETCONTENT, 0, 0);
    for (const auto& e : B.entries) SendMessageW(B.list, LB_ADDSTRING, 0, (LPARAM)W(e.label()).c_str());
    SendMessageW(B.list, LB_SETCURSEL, B.entries.size() > 1 ? 1 : 0, 0);
    ShowDetail();
    if (B.entries.empty()) Logger::I().log(Lvl::Warn, "bcdedit не вернул записей: " + truncUtf8(B.raw, 300));
}

void Rescan() {
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    B.stores = FindBcdStores();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    SendMessageW(B.store, CB_RESETCONTENT, 0, 0);
    for (const auto& s : B.stores) SendMessageW(B.store, CB_ADDSTRING, 0, (LPARAM)W(s.label).c_str());
    SendMessageW(B.store, CB_SETCURSEL, 0, 0);
    if (B.stores.empty()) { MessageBoxW(B.wnd, L"Хранилища BCD не найдены (ни на буквах дисков, ни на скрытых служебных томах).", L"BCD", MB_OK | MB_ICONWARNING); return; }
    LoadEntries();
}

// confirm -> auto backup -> run steps as a journaled job
void Execute(const std::wstring& what, std::vector<Step> steps, bool danger) {
    if (UiRunning()) { MessageBoxW(B.wnd, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
    if (steps.empty()) return;
    std::string cmds;
    for (const auto& s : steps) cmds += "  " + s.cmd + "\n";
    std::wstring msg = what + L"\n\nХранилище: " + (CurStore().empty() ? std::wstring(L"текущей системы") : W(CurStore())) + L"\n\nБудут выполнены команды:\n" + W(cmds) +
                       L"\nПеред изменением автоматически создаётся копия BCD. Продолжить?";
    if (MessageBoxW(B.wnd, msg.c_str(), L"Подтверждение изменения BCD", MB_YESNO | (danger ? MB_ICONSTOP : MB_ICONWARNING) | MB_DEFBUTTON2) != IDYES) return;
    std::string err, store = CurStore();
    std::string bk = BcdBackup(store, err);
    if (bk.empty()) {
        if (MessageBoxW(B.wnd, W("Не удалось сделать копию BCD: " + err + "\n\nПродолжить БЕЗ копии?").c_str(), L"Копия не создана", MB_YESNO | MB_ICONSTOP | MB_DEFBUTTON2) != IDYES) return;
    } else Logger::I().log(Lvl::Ok, "Копия BCD: " + bk);
    UiStartJob("bcd", "BCD: " + U(what), [steps](Ctx& c) { RunSteps(c, steps); });
}

std::string Id() { const BcdEntry* e = CurEntry(); return e ? e->id() : ""; }

void Action(int i) {
    const BcdEntry* e = CurEntry();
    std::string store = CurStore(), pre = BcdPrefix(store), id = Id();
    auto needEntry = [&]() { if (!e || id.empty()) { MessageBoxW(B.wnd, L"Выберите запись в списке.", L"BCD", MB_OK | MB_ICONINFORMATION); return false; } return true; };
    switch (i) {
        case 0: {  // description
            if (!needEntry()) return;
            std::vector<FormField> f(1);
            f[0].label = L"Новое описание записи";
            f[0].value = W(e->desc());
            if (!FormDialog(B.wnd, L"Описание", f)) return;
            Execute(L"Изменить описание записи " + W(id), {{"description", pre + " /set " + id + " description \"" + U(f[0].value) + "\"", "", 30}}, false);
            break;
        }
        case 1: if (needEntry()) Execute(L"Сделать запись " + W(id) + L" загружаемой по умолчанию", {{"default", pre + " /default " + id, "", 30}}, false); break;
        case 2: {
            std::vector<FormField> f(1);
            f[0].label = L"Таймаут меню загрузки, секунд";
            f[0].value = L"5";
            if (!FormDialog(B.wnd, L"Таймаут", f)) return;
            int n = _wtoi(f[0].value.c_str());
            Execute(L"Таймаут меню загрузки = " + std::to_wstring(n), {{"timeout", pre + " /timeout " + std::to_string(n), "", 30}}, false);
            break;
        }
        case 3: {  // device / osdevice
            if (!needEntry()) return;
            std::vector<FormField> f(1);
            f[0].label = L"Раздел с Windows (device и osdevice), например partition=D:";
            f[0].value = W(e->get("osdevice").empty() ? e->get("device") : e->get("osdevice"));
            if (!FormDialog(B.wnd, L"device / osdevice", f)) return;
            std::string v = U(f[0].value);
            Execute(L"Изменить device/osdevice записи " + W(id), {{"device", pre + " /set " + id + " device " + v, "", 30}, {"osdevice", pre + " /set " + id + " osdevice " + v, "", 30}}, true);
            break;
        }
        case 4: {  // copy
            if (!needEntry()) return;
            std::vector<FormField> f(1);
            f[0].label = L"Описание новой записи";
            f[0].value = W(e->desc() + " (копия)");
            if (!FormDialog(B.wnd, L"Копировать запись", f)) return;
            Execute(L"Скопировать запись " + W(id), {{"copy", pre + " /copy " + id + " /d \"" + U(f[0].value) + "\"", "", 30}}, false);
            break;
        }
        case 5: if (needEntry()) Execute(L"УДАЛИТЬ запись " + W(id) + L" (" + W(e->desc()) + L")", {{"delete", pre + " /delete " + id, "", 30}}, true); break;
        case 6: {  // backup
            std::string err, b = BcdBackup(store, err);
            if (b.empty()) MessageBoxW(B.wnd, W("Ошибка: " + err).c_str(), L"Копия BCD", MB_OK | MB_ICONERROR);
            else { Logger::I().log(Lvl::Ok, "Копия BCD сохранена: " + b); MessageBoxW(B.wnd, W("Копия сохранена:\n" + b).c_str(), L"Копия BCD", MB_OK | MB_ICONINFORMATION); }
            break;
        }
        case 7: {  // restore
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW o{};
            o.lStructSize = sizeof o; o.hwndOwner = B.wnd; o.lpstrFile = file; o.nMaxFile = MAX_PATH; o.lpstrFilter = L"Копии BCD\0*\0"; o.Flags = OFN_FILEMUSTEXIST;
            std::string init = DataDir() + "Backups\\";
            o.lpstrInitialDir = nullptr;
            if (!GetOpenFileNameW(&o)) return;
            if (MessageBoxW(B.wnd, W("ЗАМЕНИТЬ хранилище BCD содержимым файла?\n\n" + U(file) + "\n\nТекущее хранилище будет сохранено в копию.").c_str(), L"Восстановление BCD", MB_YESNO | MB_ICONSTOP | MB_DEFBUTTON2) != IDYES) return;
            std::string err, cur = BcdBackup(store, err);
            if (!BcdRestore(store, U(file), err)) MessageBoxW(B.wnd, W("Ошибка: " + err).c_str(), L"Восстановление BCD", MB_OK | MB_ICONERROR);
            else { AuditChange("BCD восстановлен из " + U(file) + " (прежнее состояние: " + cur + ")"); LoadEntries(); }
            break;
        }
        case 8: {  // bcdboot
            std::vector<FormField> f(4);
            auto t = UiCurrentTarget();
            f[0].label = L"Папка Windows (источник файлов загрузки), например D:\\Windows";
            f[0].kind = 3;
            for (auto& x : UiTargets()) if (!x.online) f[0].options.push_back(W(x.winDir));
            f[0].value = t.valid && !t.online ? W(t.winDir) : (f[0].options.empty() ? L"" : f[0].options[0]);
            f[1].label = L"Системный раздел / ESP (буква, должна быть назначена), например S:";
            std::string cs = store.size() > 2 ? store.substr(0, 2) : "";
            f[1].value = W(cs);
            f[2].label = L"Тип прошивки (/f)";
            f[2].kind = 2;
            f[2].options = {L"UEFI", L"BIOS", L"ALL"};
            f[2].value = L"UEFI";
            f[3].label = L"Язык (/l), необязательно, например ru-ru";
            if (!FormDialog(B.wnd, L"bcdboot — пересоздать файлы загрузки", f)) return;
            std::string cmd = "\"" + SysTool("bcdboot.exe") + "\" \"" + U(f[0].value) + "\" /s " + U(f[1].value) + " /f " + U(f[2].value);
            if (!trim(U(f[3].value)).empty()) cmd += " /l " + trim(U(f[3].value));
            cmd += " /v";
            Execute(L"bcdboot: пересоздать загрузчик из " + f[0].value + L" на " + f[1].value, {{"bcdboot", cmd, "", 180}}, true);
            break;
        }
        case 9: {  // show all in journal
            std::string s = store;
            UiStartJob("bcd-show", "BCD: показать все записи", [s](Ctx& c) {
                ProcResult r = c.run("bcdedit /enum all /v", BcdPrefix(s) + " /enum all /v", 60);
                c.saveText("bcd.txt", r.output);
            });
            break;
        }
        case 10: {  // reboot
            const Env& en = GetEnv();
            std::string cmd = en.isWinPE ? "\"" + SysTool("wpeutil.exe") + "\" reboot" : "\"" + SysTool("shutdown.exe") + "\" /r /t 0";
            if (MessageBoxW(B.wnd, L"Перезагрузить компьютер СЕЙЧАС?\nНесохранённые данные будут потеряны.", L"Перезагрузка", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
            AuditChange("Перезагрузка по запросу пользователя: " + cmd);
            UiStartJob("reboot", "Перезагрузка", [cmd](Ctx& c) { c.run("Перезагрузка", cmd, 30); });
            break;
        }
        case 11: {  // reagentc /boottore (online) / shutdown /r /o
            const Env& en = GetEnv();
            if (en.isWinPE) { MessageBoxW(B.wnd, L"Эта функция доступна только в работающей Windows.", L"WinRE", MB_OK | MB_ICONINFORMATION); return; }
            Execute(L"Перезагрузить в среду восстановления Windows (WinRE)", {{"reagentc /boottore", "\"" + SysTool("reagentc.exe") + "\" /boottore", "", 60}}, false);
            break;
        }
    }
}

void Layout() {
    RECT rc;
    GetClientRect(B.wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8), y = m;
    Place(GetDlgItem(B.wnd, 5900), m, y + S(3), S(110), S(20));
    Place(B.store, m + S(115), y, W_ - 2 * m - S(115) - S(120), S(300));
    Place(GetDlgItem(B.wnd, ID_REFRESH), W_ - m - S(110), y - 1, S(110), S(26));
    y += S(34);
    int leftW = std::max(S(380), W_ * 45 / 100);
    int btnAreaH = S(36) * 3 + S(40);
    int listH = (H - y - m - btnAreaH) * 38 / 100;
    Place(B.list, m, y, leftW, listH);
    Place(B.detail, m, y + listH + m, leftW, H - y - listH - 2 * m - btnAreaH);
    int rx = leftW + 2 * m, rw = W_ - rx - m;
    Place(GetDlgItem(B.wnd, 5901), rx, y, rw, S(20));
    Place(B.mode, rx, y + S(22), rw, S(300));
    Place(B.bApply, rx, y + S(54), rw / 2 - S(4), S(30));
    Place(B.bReset, rx + rw / 2 + S(4), y + S(54), rw / 2 - S(4), S(30));
    int by = y + S(100), bw = (rw - S(8)) / 2, bh = S(30);
    for (int i = 0; i < 12; ++i) Place(B.btn[i], rx + (i % 2) * (bw + S(8)), by + (i / 2) * (bh + S(6)), bw, bh);
    Place(GetDlgItem(B.wnd, 5902), rx, by + 6 * (bh + S(6)) + S(4), rw, S(90));
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (B.btn[11]) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(940); mm->ptMinTrackSize.y = S(620); return 0; }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == ID_STORE && code == CBN_SELCHANGE) LoadEntries();
            else if (id == ID_REFRESH) Rescan();
            else if (id == ID_LIST && code == LBN_SELCHANGE) ShowDetail();
            else if (id == ID_APPLYMODE || id == ID_RESETMODES) {
                if (UiRunning()) return 0;
                std::string idv = Id();
                const BcdEntry* e = CurEntry();
                if (!e || idv.empty() || idv == "{bootmgr}") { MessageBoxW(h, L"Выберите запись Windows Boot Loader (не диспетчер загрузки).", L"BCD", MB_OK | MB_ICONINFORMATION); return 0; }
                if (id == ID_APPLYMODE) {
                    int mi = (int)SendMessageW(B.mode, CB_GETCURSEL, 0, 0);
                    Execute(L"Режим загрузки «" + W(BootModes()[(size_t)mi].title) + L"» для записи " + W(idv), BcdModeSteps(CurStore(), idv, mi), false);
                } else Execute(L"Сбросить ВСЕ особые режимы загрузки записи " + W(idv), BcdResetModesSteps(CurStore(), idv), false);
            } else if (id >= ID_B0 && id < ID_B0 + 12) Action(id - ID_B0);
            return 0;
        }
        case WM_DESTROY: B.wnd = nullptr; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void OpenBcdManagerImpl() {
    if (B.wnd) { ShowWindow(B.wnd, SW_RESTORE); SetForegroundWindow(B.wnd); return; }
    RegisterOnce(L"WRBcd", Proc);
    B = State();
    B.wnd = CreateWindowExW(0, L"WRBcd", L"BCD / Загрузка — выбор хранилища, записи, режимы загрузки, bcdboot", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1120), S(720),
                            g_ui.main, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = B.wnd;
    Mk(h, L"STATIC", L"Хранилище BCD:", SS_LEFT, 5900);
    B.store = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_STORE);
    Mk(h, L"BUTTON", L"Найти заново", BS_PUSHBUTTON | WS_TABSTOP, ID_REFRESH);
    B.list = Mk(h, L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
    B.detail = Mk(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL | ES_AUTOHSCROLL, ID_DETAIL, WS_EX_CLIENTEDGE);
    SendMessageW(B.detail, WM_SETFONT, (WPARAM)g_ui.mono, TRUE);
    Mk(h, L"STATIC", L"Режим следующей загрузки (как «Загрузка» в msconfig и F8-меню) для выбранной записи:", SS_LEFT, 5901);
    B.mode = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_MODE);
    for (const auto& m : BootModes()) SendMessageW(B.mode, CB_ADDSTRING, 0, (LPARAM)W(m.title).c_str());
    SendMessageW(B.mode, CB_SETCURSEL, 0, 0);
    B.bApply = Mk(h, L"BUTTON", L"Применить режим…", BS_PUSHBUTTON | WS_TABSTOP, ID_APPLYMODE);
    B.bReset = Mk(h, L"BUTTON", L"Сбросить все режимы…", BS_PUSHBUTTON | WS_TABSTOP, ID_RESETMODES);
    const wchar_t* bt[] = {L"Описание…", L"Загружать по умолчанию…", L"Таймаут меню…", L"device/osdevice…", L"Копировать запись…", L"Удалить запись…",
                           L"Резервная копия BCD", L"Восстановить из копии…", L"bcdboot: пересоздать…", L"Все записи — в журнал", L"Перезагрузить…", L"Перезагрузить в WinRE"};
    for (int i = 0; i < 12; ++i) B.btn[i] = Mk(h, L"BUTTON", bt[i], BS_PUSHBUTTON | WS_TABSTOP, ID_B0 + i);
    Mk(h, L"STATIC", L"Каждое изменение: подтверждение → автоматическая копия BCD → выполнение с записью в журнал и Data\\Logs\\changes_*.log. "
                      "ESP без буквы диска монтируется временно и отмонтируется при выходе. Для UEFI-Windows 11 безопасные режимы работают при включённой «Безопасной загрузке» — "
                      "параметр nointegritychecks/testsigning может блокироваться Secure Boot.", SS_LEFT, 5902);
    Layout();
    ShowWindow(h, SW_SHOW);
    Rescan();
}

void BcdJobDone() { if (B.wnd) LoadEntries(); }

}  // namespace ui
}  // namespace wr
