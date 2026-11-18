// WinRE manager: enter WinRE on next boot (online and from WinPE), build "WinRE + WinRepair", save logs to disk.
#include "ui_tools_common.h"
#include "modules/winre.h"
#include <shlobj.h>
#include <cwctype>
#include <memory>

namespace wr {
namespace ui {

namespace {

enum { ID_TARGET = 8001, ID_STORE, ID_FIND, ID_STATUS, ID_B0 = 8100 };

struct State {
    HWND wnd = nullptr, target, store, find, status, btn[11];
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

// ===== Свой загрузочный WinPE: отдельное окно с тремя пронумерованными панелями =====
// 1. Куда записать  2. База образа  3. Установочный образ Windows (две рамки: «Файл ISO» и «Диск или флешка»).
// Активная панель — цветная, неактивная — серая с отключёнными полями; под рамками образа — живая проверка (зелёный текст — подходит, красный — нет).
namespace pe {

enum { ID_DEST = 8601, ID_DEST_BR, ID_B_MEDIA, ID_B_ADK, ID_B_WINRE, ID_SRC_ISO, ID_ISO, ID_ISO_BR, ID_SRC_DRV, ID_DRV, ID_DRV_BR, ID_CHECK, ID_DEST_INFO, ID_BIOS };
const UINT WM_RECHECK = WM_APP + 41;
const UINT WM_ISO_DONE = WM_APP + 42;   // фоновое подключение ISO завершено (lParam — IsoDone*)

COLORREF Mix(COLORREF a, COLORREF b, int pct) {   // pct % of b
    auto ch = [&](int sh) { int x = (a >> sh) & 255, y = (b >> sh) & 255; return (COLORREF)(x + (y - x) * pct / 100) << sh; };
    return ch(0) | ch(8) | ch(16);
}

struct Dlg {
    HWND h = nullptr, dest = nullptr, destBr = nullptr, bMedia = nullptr, bAdk = nullptr, bWinre = nullptr;
    HWND srcIso = nullptr, iso = nullptr, isoBr = nullptr, srcDrv = nullptr, drv = nullptr, drvBr = nullptr, check = nullptr;
    HWND destInfo = nullptr, biosBtn = nullptr;   // подсказка «что выбрано» в панели 1; кнопка «Сделать флешку загрузочной для BIOS»
    int destKind = 0;    // цвет подсказки панели 1: 0 нейтрально, 1 хорошо, 2 плохо, 3 внимание
    int action = 0;      // чем закрыто окно: 0 — отмена, 1 — собрать, 2 — только подготовить флешку для BIOS
    RECT card1{}, card2{}, card3{}, subIso{}, subDrv{};
    HBRUSH fill1 = nullptr, fill2 = nullptr, fill3 = nullptr, fill3off = nullptr, white = nullptr;
    bool adk = false, done = false, accepted = false;
    std::wstring adkWim;   // winpe.wim, указанный вручную (если ADK не найден автоматически)
    int base = 2;        // 2 — установочный образ, 1 — ADK, 0 — winre.wim
    bool useIso = true;  // в панели 3: true — файл ISO, false — диск/флешка/папка
    int checkKind = 0;   // 0 нейтрально, 1 хорошо, 2 плохо, 3 внимание (идёт подключение)
    // Образ ISO подключается сразу после выбора (в фоне), его буква подставляется в рамку «Папка или диск».
    HANDLE mountThread = nullptr;
    bool mounting = false, mountOk = false, mountByUs = false, okAfterMount = false;
    std::wstring mountIso, mountErr;   // какой ISO подключали; причина неудачи
    std::wstring pendingIso;           // выбран другой ISO, пока подключался предыдущий
    char mountLetter = 0;
    HWND tip = nullptr;
};
Dlg D;
std::wstring g_lastIso;   // последний ISO в этом сеансе программы

struct IsoDone { std::string iso; IsoMountResult r; };
struct MountReq { HWND h; std::string iso; };
DWORD WINAPI MountThread(LPVOID p) {
    MountReq* rq = (MountReq*)p;
    IsoDone* d = new IsoDone{rq->iso, MountIsoImage(rq->iso, 30)};
    if (!PostMessageW(rq->h, WM_ISO_DONE, 0, (LPARAM)d)) {   // окно уже закрыто: подключённый нами образ не оставляем
        if (d->r.attachedByUs) DismountIsoImage(d->iso);
        delete d;
    }
    delete rq;
    return 0;
}
DWORD WINAPI DismountThread(LPVOID p) {
    std::string* iso = (std::string*)p;
    DismountIsoImage(*iso);
    delete iso;
    return 0;
}
void DismountAsync(const std::wstring& iso) {
    if (iso.empty()) return;
    HANDLE t = CreateThread(nullptr, 0, DismountThread, new std::string(U(iso)), 0, nullptr);
    if (t) CloseHandle(t);
}

std::wstring Unquote(std::wstring s) {
    while (!s.empty() && (s.front() == L'"' || s.front() == L' ')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L'"' || s.back() == L' ')) s.pop_back();
    return s;
}
bool EndsWithI(const std::wstring& s, const wchar_t* tail) {
    size_t n = wcslen(tail);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}
bool IsFile(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
bool IsFolder(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
uint64_t SizeOf(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}
std::wstring WithSlash(std::wstring p) {
    if (p.size() == 2 && p[1] == L':') p += L"\\";
    if (!p.empty() && p.back() != L'\\') p += L"\\";
    return p;
}

// Выбор папки (корень флешки, подключённый образ, папка с sources\boot.wim). Старый вариант окна —
// без COM, работает и в WinPE; есть поле для ввода пути.
int CALLBACK FolderCb(HWND h, UINT m, LPARAM, LPARAM data) {
    if (m == BFFM_INITIALIZED && data) SendMessageW(h, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}
bool BrowseFolder(HWND owner, const std::wstring& title, std::wstring& path) {
    std::wstring start = Unquote(path);
    while (start.size() > 3 && start.back() == L'\\') start.pop_back();
    if (!IsFolder(start)) start.clear();
    std::wstring t = TrW(title);
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = t.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_EDITBOX;
    bi.lpfn = FolderCb;
    bi.lParam = start.empty() ? 0 : (LPARAM)start.c_str();
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t buf[MAX_PATH] = L"";
    bool ok = SHGetPathFromIDListW(pidl, buf) != FALSE;
    ILFree(pidl);
    if (ok) path = buf;
    return ok && buf[0];
}
bool BrowseIso(HWND owner, std::wstring& path) {
    std::wstring cur = Unquote(path), dir;
    wchar_t file[MAX_PATH * 2] = L"";
    if (IsFile(cur) && cur.size() < MAX_PATH) lstrcpynW(file, cur.c_str(), (int)(sizeof file / sizeof file[0]));
    else {
        size_t k = cur.find_last_of(L'\\');
        if (k != std::wstring::npos && IsFolder(cur.substr(0, k + 1))) dir = cur.substr(0, k + 1);
    }
    std::wstring filt = TrW(L"Образ Windows (*.iso, *.wim)") + std::wstring(1, L'\0') + L"*.iso;*.wim" + std::wstring(1, L'\0') +
                        TrW(L"Образ ISO (*.iso)") + std::wstring(1, L'\0') + L"*.iso" + std::wstring(1, L'\0') +
                        TrW(L"Образ WIM (*.wim)") + std::wstring(1, L'\0') + L"*.wim" + std::wstring(1, L'\0') +
                        TrW(L"Все файлы (*.*)") + std::wstring(1, L'\0') + L"*.*" + std::wstring(2, L'\0');
    std::wstring title = TrW(L"Выберите образ Windows (.iso или boot.wim)");
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = owner;
    o.lpstrFilter = filt.c_str();
    o.lpstrFile = file;
    o.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
    o.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    o.lpstrTitle = title.c_str();
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&o)) return false;
    path = file;
    return true;
}

bool BrowseWinPeWim(HWND owner, std::wstring& path) {
    wchar_t file[MAX_PATH * 2] = L"winpe.wim";
    std::wstring filt = TrW(L"Образ ADK WinPE (winpe.wim)") + std::wstring(1, L'\0') + L"winpe.wim;*.wim" + std::wstring(1, L'\0') +
                        TrW(L"Все файлы (*.*)") + std::wstring(1, L'\0') + L"*.*" + std::wstring(2, L'\0');
    std::wstring title = TrW(L"Укажите winpe.wim из Windows ADK");
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = owner;
    o.lpstrFilter = filt.c_str();
    o.lpstrFile = file;
    o.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
    o.lpstrTitle = title.c_str();
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&o)) return false;
    path = file;
    return true;
}

// Что сейчас выбрано в панели 3: путь для сборки ("" — не готово) и текст проверки.
std::wstring Source(std::wstring& msg, int& kind) {
    kind = 0;
    if (D.useIso) {
        std::wstring p = Unquote(GetText(D.iso));
        if (p.empty()) { msg = L"Укажите файл .iso или boot.wim: кнопка «Обзор…» или вставьте путь (можно в кавычках, как из «Копировать как путь»)."; return L""; }
        bool isWim = EndsWithI(p, L".wim"), isIso = EndsWithI(p, L".iso");
        if (!isWim && !isIso) { kind = 2; msg = L"Нужен файл .iso или .wim (boot.wim). Для папки или диска выберите вариант ниже."; return L""; }
        if (!IsFile(p)) { kind = 2; msg = L"Файл не найден: " + p; return L""; }
        kind = 1;
        if (isWim) msg = L"Файл boot.wim найден (" + W(fmtBytes(SizeOf(p))) + L"). Из него будет взят образ №1 — WinPE.";
        else if (D.mounting && _wcsicmp(D.mountIso.c_str(), p.c_str()) == 0) { kind = 3; msg = L"Подключаю образ ISO… подождите, обычно это 5–30 секунд."; }
        else if (!D.mountErr.empty() && _wcsicmp(D.mountIso.c_str(), p.c_str()) == 0) { kind = 2; msg = L"Не удалось подключить образ: " + D.mountErr + L" Подключите его вручную (правый клик → «Подключить») и выберите букву ниже."; }
        else msg = L"Файл ISO найден (" + W(fmtBytes(SizeOf(p))) + L"). Он подключится сразу, буква диска появится ниже; после записи образ отключится.";
        return p;
    }
    std::wstring p = Unquote(GetText(D.drv));
    if (p.empty()) { msg = L"Выберите папку или диск с файлами установки Windows (внутри должен быть sources\\boot.wim)."; return L""; }
    if (EndsWithI(p, L"boot.wim") && IsFile(p)) { kind = 1; msg = L"Найден boot.wim: " + p; return p; }
    std::wstring r = WithSlash(p);
    if (!IsFolder(r)) { kind = 2; msg = L"Диск или папка не найдены: " + p; return L""; }
    if (!IsFile(r + L"sources\\boot.wim")) { kind = 2; msg = L"В «" + r + L"» нет sources\\boot.wim — это не установочный носитель Windows."; return L""; }
    kind = 1;
    msg = L"Найден " + r + L"sources\\boot.wim (" + W(fmtBytes(SizeOf(r + L"sources\\boot.wim"))) + L").";
    if (D.mountOk && D.mountLetter && r.size() == 3 && towupper(r[0]) == (wchar_t)D.mountLetter)
        msg = L"Образ ISO подключён как " + r + L" — найден sources\\boot.wim (" + W(fmtBytes(SizeOf(r + L"sources\\boot.wim"))) + L"). После записи образ отключится.";
    return r;
}

// Подсказка панели 1: что за место выбрано и где с него можно будет загрузиться.
std::wstring DestInfo(int& kind, bool& usableForBios) {
    kind = 0;
    usableForBios = false;
    std::wstring d = Unquote(GetText(D.dest));
    if (d.size() < 2) { kind = 2; return L"Выберите флешку из списка или укажите папку."; }
    BootMediaInfo b = QueryBootMedia(U(d));
    if (!b.isRoot) {
        kind = 3;
        return std::wstring(b.exists ? L"Это папка" : L"Это папка (будет создана)") +
               L": с неё не загрузиться. Скопируете на флешку — для UEFI хватит, для BIOS потом нажмите «Сделать флешку загрузочной для BIOS». Проще сразу выбрать флешку.";
    }
    if (!b.exists) { kind = 2; return L"Диска " + d.substr(0, 2) + L" нет — вставьте флешку и выберите её в списке."; }
    if (b.systemDisk) { kind = 2; return L"Это диск, на котором стоит Windows, — выберите флешку."; }
    std::wstring fs = W(b.fs.empty() ? std::string("?") : b.fs), sz = W(fmtBytes(b.size));
    if (!b.usb && !b.removable) { kind = 3; return L"Это не флешка (внутренний диск, " + fs + L", " + sz + L"). Загрузочный код для BIOS на него автоматически не пишется."; }
    usableForBios = !b.gpt;
    bool fat = _wcsicmp(fs.c_str(), L"FAT32") == 0;
    if (b.gpt) { kind = 3; return L"Флешка " + fs + L", " + sz + L", разметка GPT: загрузится только в UEFI. Для старых компьютеров (BIOS) нужна разметка MBR."; }
    if (!fat) { kind = 3; return L"Флешка " + fs + L", " + sz + L": в BIOS загрузится, но многие UEFI-компьютеры с " + fs + L" не грузятся — лучше отформатировать в FAT32."; }
    kind = 1;
    return L"Флешка FAT32, " + sz + L": загрузится и в UEFI, и в BIOS (Legacy) — всё настроится автоматически." + (b.hasWinPe ? L" На ней уже есть WinPE — файлы обновятся." : L"");
}

void Recheck() {
    bool media = D.base == 2;
    EnableWindow(D.srcIso, media);
    EnableWindow(D.srcDrv, media);
    EnableWindow(D.iso, media && D.useIso);
    EnableWindow(D.isoBr, media && D.useIso && !D.mounting);
    EnableWindow(GetDlgItem(D.h, IDOK), !D.mounting);
    EnableWindow(D.drv, media && !D.useIso);
    EnableWindow(D.drvBr, media && !D.useIso);
    SendMessageW(D.bMedia, BM_SETCHECK, D.base == 2 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(D.bAdk, BM_SETCHECK, D.base == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(D.bWinre, BM_SETCHECK, D.base == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(D.srcIso, BM_SETCHECK, D.useIso ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(D.srcDrv, BM_SETCHECK, D.useIso ? BST_UNCHECKED : BST_CHECKED, 0);
    std::wstring msg;
    int kind = 0;
    if (media) Source(msg, kind);
    else if (D.base == 1) msg = D.adk ? L"Для базы ADK установочный образ не нужен." : (D.adkWim.empty() ? std::wstring(L"ADK не найден автоматически — после нажатия «Собрать флешку» укажите файл winpe.wim.") : L"ADK WinPE: " + D.adkWim);
    else msg = L"Для базы winre.wim установочный образ не нужен.";
    D.checkKind = kind;
    SetWindowTextW(D.check, msg.c_str());
    bool biosOk = false;
    std::wstring di = DestInfo(D.destKind, biosOk);
    SetWindowTextW(D.destInfo, di.c_str());
    RedrawWindow(D.h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);   // фон рамок и переключателей меняется
}

// Подключить выбранный ISO в фоне. Буква диска появится в рамке «Папка или диск».
void StartMount(const std::wstring& isoIn) {
    std::wstring iso = Unquote(isoIn);
    if (!EndsWithI(iso, L".iso") || !IsFile(iso)) return;
    if (D.mounting) { if (_wcsicmp(D.mountIso.c_str(), iso.c_str()) != 0) D.pendingIso = iso; return; }
    if (D.mountOk && _wcsicmp(D.mountIso.c_str(), iso.c_str()) == 0) return;   // уже подключён
    if (D.mountOk && D.mountByUs) DismountAsync(D.mountIso);                    // выбран другой ISO — прежний отключаем
    D.mountOk = D.mountByUs = false;
    D.mountLetter = 0;
    D.mountErr.clear();
    D.mountIso = iso;
    MountReq* rq = new MountReq{D.h, U(iso)};
    D.mountThread = CreateThread(nullptr, 0, MountThread, rq, 0, nullptr);
    if (!D.mountThread) { delete rq; D.mountErr = L"не удалось запустить подключение."; Recheck(); return; }
    D.mounting = true;
    Recheck();
}

void OnMountDone(IsoDone* d) {
    std::unique_ptr<IsoDone> keep(d);
    if (D.mountThread) { CloseHandle(D.mountThread); D.mountThread = nullptr; }
    D.mounting = false;
    std::wstring pending;
    pending.swap(D.pendingIso);
    if (!pending.empty() && _wcsicmp(pending.c_str(), W(d->iso).c_str()) != 0) {   // пока подключался этот, выбран другой ISO
        if (d->r.attachedByUs) DismountAsync(W(d->iso));
        StartMount(pending);
        return;
    }
    if (!d->r.ok) {
        D.mountErr = W(d->r.error);
        if (!D.mountErr.empty() && D.mountErr.back() != L'.') D.mountErr += L".";
        if (d->r.attachedByUs) DismountAsync(W(d->iso));   // подключился без буквы — не оставляем
        bool wanted = D.okAfterMount;
        D.okAfterMount = false;
        Recheck();
        if (wanted)
            MessageBoxW(D.h, (L"Не удалось подключить образ ISO: " + D.mountErr + L"\n\nПодключите его вручную (правый клик по файлу → «Подключить») и выберите его букву в рамке «Папка или диск».").c_str(),
                        L"Образ ISO", MB_OK | MB_ICONWARNING);
        return;
    }
    D.mountOk = true;
    D.mountByUs = d->r.attachedByUs;
    D.mountLetter = d->r.letter;
    std::wstring r = std::wstring(1, (wchar_t)d->r.letter) + L":\\";
    if (SendMessageW(D.drv, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)r.c_str()) == CB_ERR) SendMessageW(D.drv, CB_ADDSTRING, 0, (LPARAM)r.c_str());
    SetWindowTextW(D.drv, r.c_str());
    D.useIso = false;   // дальше работаем с буквой подключённого образа
    Recheck();
    if (D.okAfterMount) { D.okAfterMount = false; PostMessageW(D.h, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0); }
}

void AddTip(HWND ctl, const wchar_t* ru) {
    if (!D.tip || !ctl) return;
    std::wstring t = TrW(ru);
    auto add = [&](HWND w) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = D.h;
        ti.uId = (UINT_PTR)w;
        ti.lpszText = (LPWSTR)t.c_str();   // элемент подсказок хранит свою копию текста
        SendMessageW(D.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    };
    add(ctl);
    HWND ed = FindWindowExW(ctl, nullptr, L"Edit", nullptr);   // у выпадающего списка с вводом мышь над вложенным полем
    if (ed) add(ed);
}

void CreateTips() {
    D.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, D.h, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!D.tip) return;
    SendMessageW(D.tip, TTM_SETMAXTIPWIDTH, 0, S(380));
    SendMessageW(D.tip, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
    SendMessageW(D.tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    AddTip(D.dest, L"Куда записать WinPE. Лучше всего — корень флешки в FAT32 (например, E:\\): она загрузится и в UEFI, и в BIOS. Можно указать папку и потом скопировать её на флешку.");
    AddTip(D.destBr, L"Выбрать флешку или папку в проводнике.");
    AddTip(D.bMedia, L"WinPE берётся из установочного образа Windows 10/11: ISO, boot.wim или установочная флешка. Ничего устанавливать не нужно.");
    AddTip(D.bAdk, L"Чистый WinPE из Windows ADK (нужны ADK и дополнение WinPE с сайта Microsoft). Если ADK не найден, укажете winpe.wim вручную.");
    AddTip(D.bWinre, L"Среда восстановления этой Windows (winre.wim) со встроенным «Восстановлением системы» (rstrui). Не подходит, если WinRE отключена или повреждена.");
    AddTip(D.srcIso, L"Файл .iso — программа сама подключит его и после записи отключит. Или boot.wim из папки sources распакованного образа.");
    AddTip(D.iso, L"Путь к файлу .iso или boot.wim. Можно вставить путь из «Копировать как путь» (в кавычках). ISO подключается сразу после выбора.");
    AddTip(D.isoBr, L"Выбрать файл .iso или boot.wim в проводнике.");
    AddTip(D.srcDrv, L"Папка или диск, где есть sources\\boot.wim: распакованный ISO, подключённый образ (буква диска) или установочная флешка Windows.");
    AddTip(D.drv, L"Буква подключённого образа или установочной флешки (например, F:\\) либо папка. В списке — найденные диски с sources\\boot.wim.");
    AddTip(D.drvBr, L"Выбрать папку или диск в проводнике.");
    AddTip(D.biosBtn, L"Если WinPE уже скопирован на флешку вручную: сделать раздел активным и записать загрузочный код для старых компьютеров (BIOS/Legacy). Файлы не удаляются.");
    AddTip(GetDlgItem(D.h, IDOK), L"Собрать WinPE с WinRepair и записать на флешку. Флешка не форматируется: заменяются только Boot, EFI, bootmgr и sources\\boot.wim.");
    AddTip(GetDlgItem(D.h, IDCANCEL), L"Закрыть окно без сборки. Образ ISO, подключённый программой, будет отключён.");
}

void Layout() {
    RECT cr;
    GetClientRect(D.h, &cr);
    int pad = S(12), x = pad, w = cr.right - 2 * pad, ix = x + S(16), iw = w - S(30), bw = S(104), y = pad;
    D.card1 = {x, y, x + w, y + S(98)};
    Place(D.dest, ix, y + S(32), iw - bw - S(8), S(260));
    Place(D.destBr, ix + iw - bw, y + S(31), bw, S(26));
    Place(D.destInfo, ix, y + S(62), iw, S(32));
    y += S(98) + S(10);
    D.card2 = {x, y, x + w, y + S(112)};
    Place(D.bMedia, ix, y + S(32), iw, S(22));
    Place(D.bAdk, ix, y + S(56), iw, S(22));
    Place(D.bWinre, ix, y + S(80), iw, S(22));
    y += S(112) + S(10);
    D.card3 = {x, y, x + w, y + S(198)};
    int sx = ix - S(4), sw = iw + S(8);
    D.subIso = {sx, y + S(30), sx + sw, y + S(30) + S(66)};
    Place(D.srcIso, ix + S(6), y + S(36), iw - S(12), S(22));
    Place(D.iso, ix + S(26), y + S(62), iw - bw - S(40), S(24));
    Place(D.isoBr, ix + iw - bw - S(8), y + S(61), bw, S(26));
    D.subDrv = {sx, y + S(102), sx + sw, y + S(102) + S(66)};
    Place(D.srcDrv, ix + S(6), y + S(108), iw - S(12), S(22));
    Place(D.drv, ix + S(26), y + S(134), iw - bw - S(40), S(260));
    Place(D.drvBr, ix + iw - bw - S(8), y + S(133), bw, S(26));
    Place(D.check, ix, y + S(174), iw, S(20));
    y += S(198) + S(14);
    Place(D.biosBtn, x, y, S(400), S(32));
    Place(GetDlgItem(D.h, IDOK), x + w - S(286), y, S(180), S(32));
    Place(GetDlgItem(D.h, IDCANCEL), x + w - S(98), y, S(98), S(32));
}

HBRUSH BrushFor(HWND ctl) {
    bool media = D.base == 2;
    if (ctl == GetDlgItem(D.h, IDOK) || ctl == GetDlgItem(D.h, IDCANCEL) || ctl == D.biosBtn) return GetSysColorBrush(COLOR_BTNFACE);
    if (ctl == D.isoBr) return media && D.useIso ? D.white : media ? D.fill3 : D.fill3off;
    if (ctl == D.drvBr) return media && !D.useIso ? D.white : media ? D.fill3 : D.fill3off;
    if (ctl == D.srcIso || ctl == D.iso) return media && D.useIso ? D.white : media ? D.fill3 : D.fill3off;
    if (ctl == D.srcDrv || ctl == D.drv) return media && !D.useIso ? D.white : media ? D.fill3 : D.fill3off;
    if (ctl == D.check) return media ? D.fill3 : D.fill3off;
    if (ctl == D.bMedia || ctl == D.bAdk || ctl == D.bWinre) return D.fill2;
    return D.fill1;
}

void PaintSub(HDC dc, const RECT& r, bool on, bool enabled) {
    COLORREF acc = theme::Accent(theme::Cat::Diag);
    HPEN pen = CreatePen(PS_SOLID, on && enabled ? S(2) : 1, on && enabled ? acc : Mix(RGB(255, 255, 255), RGB(120, 130, 150), 35));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, on && enabled ? D.white : (enabled ? D.fill3 : D.fill3off));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(8), S(8));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            bool media = D.base == 2;
            theme::PaintCard(dc, D.card1, theme::Cat::Repair, TrW(L"1. Куда записать"), TrW(L"корень флешки (FAT32) или обычная папка"));
            theme::PaintCard(dc, D.card2, theme::Cat::Guide, TrW(L"2. База образа"), TrW(L"откуда взять WinPE"));
            theme::PaintCard(dc, D.card3, media ? theme::Cat::Diag : theme::Cat::Info, TrW(L"3. Установочный образ Windows"),
                             TrW(media ? L"выберите ОДИН вариант — файл образа или папку/диск" : L"не нужен для выбранной базы"));
            PaintSub(dc, D.subIso, D.useIso, media);
            PaintSub(dc, D.subDrv, !D.useIso, media);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wp;
            HWND ctl = (HWND)lp;
            SetBkMode(dc, TRANSPARENT);
            if (ctl == D.check) SetTextColor(dc, D.checkKind == 1 ? RGB(30, 130, 60) : D.checkKind == 2 ? RGB(200, 40, 40) : D.checkKind == 3 ? RGB(170, 95, 0) : RGB(90, 100, 120));
            if (ctl == D.destInfo) SetTextColor(dc, D.destKind == 1 ? RGB(30, 130, 60) : D.destKind == 2 ? RGB(200, 40, 40) : D.destKind == 3 ? RGB(170, 95, 0) : RGB(90, 100, 120));
            return (LRESULT)BrushFor(ctl);
        }
        case WM_RECHECK: Recheck(); return 0;
        case WM_ISO_DONE: OnMountDone((IsoDone*)lp); return 0;
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            switch (id) {
                case ID_B_MEDIA: D.base = 2; Recheck(); return 0;
                case ID_B_ADK: D.base = 1; Recheck(); return 0;
                case ID_B_WINRE: D.base = 0; Recheck(); return 0;
                case ID_SRC_ISO: D.useIso = true; Recheck(); SetFocus(D.iso); return 0;
                case ID_SRC_DRV: D.useIso = false; Recheck(); SetFocus(D.drv); return 0;
                case ID_DEST: if (code == CBN_EDITCHANGE || code == CBN_SELCHANGE) PostMessageW(h, WM_RECHECK, 0, 0); return 0;
                case ID_ISO:
                    if (code == EN_CHANGE) PostMessageW(h, WM_RECHECK, 0, 0);
                    else if (code == EN_KILLFOCUS && D.useIso && D.base == 2 && !D.done) {   // путь вставлен вручную
                        HWND f = GetFocus();
                        if (f != GetDlgItem(h, IDCANCEL) && f != D.isoBr && f != GetDlgItem(h, IDOK)) StartMount(GetText(D.iso));
                    }
                    return 0;
                case ID_DRV: if (code == CBN_EDITCHANGE || code == CBN_SELCHANGE) PostMessageW(h, WM_RECHECK, 0, 0); return 0;
                case ID_ISO_BR: { std::wstring p = GetText(D.iso); if (BrowseIso(h, p)) { SetWindowTextW(D.iso, p.c_str()); StartMount(p); } return 0; }
                case ID_DRV_BR: { std::wstring p = GetText(D.drv); if (BrowseFolder(h, L"Папка или диск с файлами Windows (внутри должен быть sources\\boot.wim)", p)) { SetWindowTextW(D.drv, p.c_str()); PostMessageW(h, WM_RECHECK, 0, 0); } return 0; }
                case ID_DEST_BR: { std::wstring p = GetText(D.dest); if (BrowseFolder(h, L"Куда записать WinPE: корень флешки (FAT32) или папка", p)) { SetWindowTextW(D.dest, p.c_str()); PostMessageW(h, WM_RECHECK, 0, 0); } return 0; }
                case ID_BIOS: {   // флешка, на которую WinPE скопирован вручную
                    BootMediaInfo b = QueryBootMedia(U(Unquote(GetText(D.dest))));
                    const wchar_t* t = L"Сделать флешку загрузочной для BIOS";
                    if (!b.isRoot || !b.exists) { MessageBoxW(h, L"В панели 1 выберите флешку (букву диска), на которую уже скопирован WinPE.", t, MB_OK | MB_ICONINFORMATION); SetFocus(D.dest); return 0; }
                    if (b.systemDisk) { MessageBoxW(h, L"Это диск, на котором стоит Windows. Выберите флешку.", t, MB_OK | MB_ICONWARNING); return 0; }
                    if (!b.hasWinPe) { MessageBoxW(h, L"На этой флешке нет WinPE (bootmgr и sources\\boot.wim). Сначала нажмите «Собрать флешку» — тогда всё, в том числе загрузка в BIOS, настроится само.", t, MB_OK | MB_ICONINFORMATION); return 0; }
                    if (b.gpt) { MessageBoxW(h, L"Флешка размечена в GPT — в режиме BIOS (Legacy) с неё не загрузиться. Нужна разметка MBR (данные на флешке при этом удаляются).", t, MB_OK | MB_ICONWARNING); return 0; }
                    if (MessageBoxW(h, L"На флешке будет сделан активным раздел и записан загрузочный код BOOTMGR (bootsect /nt60). Файлы на флешке не удаляются.\n\nПродолжить?", t, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
                    D.action = 2;
                    D.accepted = true;
                    D.done = true;
                    return 0;
                }
                case IDOK: {
                    std::wstring dst = Unquote(GetText(D.dest));
                    if (dst.size() < 2) { MessageBoxW(h, L"Укажите, куда записать WinPE (панель 1).", L"Свой WinPE с WinRepair", MB_OK | MB_ICONINFORMATION); SetFocus(D.dest); return 0; }
                    if (D.mounting) { D.okAfterMount = true; return 0; }   // соберём, как только образ подключится
                    if (D.base == 2 && D.useIso) {   // выбран ISO, но ещё не подключён: подключаем и потом продолжаем сами
                        std::wstring p = Unquote(GetText(D.iso));
                        if (EndsWithI(p, L".iso") && IsFile(p)) {
                            if (!D.mountErr.empty() && _wcsicmp(D.mountIso.c_str(), p.c_str()) == 0) D.mountErr.clear();   // повторная попытка
                            D.okAfterMount = true;
                            StartMount(p);
                            if (D.mounting) return 0;
                            D.okAfterMount = false;
                        }
                    }
                    if (D.base == 2) {
                        std::wstring msg;
                        int kind = 0;
                        if (Source(msg, kind).empty()) {
                            MessageBoxW(h, (TrW(L"Панель 3 — установочный образ Windows:") + L"\n" + TrW(msg) + L"\n\n" + TrW(L"Скачать образ можно на сайте Microsoft («Скачать Windows 10/11»).")).c_str(),
                                        L"Нужен образ Windows", MB_OK | MB_ICONINFORMATION);
                            SetFocus(D.useIso ? D.iso : D.drv);
                            return 0;
                        }
                    }
                    if (D.base == 1 && !D.adk && D.adkWim.empty()) {
                        MessageBoxW(h, L"ADK WinPE не найден автоматически.\nУкажите файл winpe.wim — обычно он лежит в папке Windows Kits\\10\\Assessment and Deployment Kit\\Windows Preinstallation Environment\\amd64\\en-us.",
                                    L"Свой WinPE с WinRepair", MB_OK | MB_ICONINFORMATION);
                        std::wstring p;
                        if (!BrowseWinPeWim(h, p)) return 0;
                        D.adkWim = p;
                        Recheck();
                    }
                    D.action = 1;
                    D.accepted = true;
                    D.done = true;
                    return 0;
                }
                case IDCANCEL: D.done = true; return 0;
            }
            return 0;
        }
        case WM_CLOSE: D.done = true; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// Показать окно; 0 — отмена, 1 — «Собрать флешку», 2 — «Сделать флешку загрузочной для BIOS».
// isoDismount — ISO, который окно подключило и который сборка должна отключить в конце.
int Run(HWND owner, std::wstring& dest, int& base, std::wstring& src, std::wstring& baseText, std::wstring& isoDismount) {
    RegisterOnce(L"WRWinPeBuild", Proc);
    D = Dlg();
    D.adk = WinPeAdkInstalled();
    COLORREF wh = RGB(255, 255, 255);
    D.fill1 = CreateSolidBrush(Mix(wh, theme::Accent(theme::Cat::Repair), 12));
    D.fill2 = CreateSolidBrush(Mix(wh, theme::Accent(theme::Cat::Guide), 12));
    D.fill3 = CreateSolidBrush(Mix(wh, theme::Accent(theme::Cat::Diag), 12));
    D.fill3off = CreateSolidBrush(Mix(wh, theme::Accent(theme::Cat::Info), 12));
    D.white = CreateSolidBrush(wh);

    int cw = S(720), ch = S(12) + S(98) + S(10) + S(112) + S(10) + S(198) + S(14) + S(32) + S(14);
    RECT wr_{0, 0, cw, ch};
    AdjustWindowRectEx(&wr_, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int ww = wr_.right - wr_.left, wh_ = wr_.bottom - wr_.top;
    RECT orc{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (owner) GetWindowRect(owner, &orc);
    int x0 = orc.left + ((orc.right - orc.left) - ww) / 2, y0 = std::max(0, (int)(orc.top + ((orc.bottom - orc.top) - wh_) / 2));
    D.h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"WRWinPeBuild", L"Свой WinPE с WinRepair — загрузочная флешка (F1 — справка)", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                          x0, y0, ww, wh_, owner, nullptr, GetModuleHandleW(nullptr), nullptr);

    // 1. Куда записать
    D.dest = Mk(D.h, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP | WS_GROUP, ID_DEST);
    DWORD mask = GetLogicalDrives();
    std::wstring defDest, defDrv;
    for (char L = 'D'; L <= 'Z'; ++L) {
        if (!(mask & (1u << (L - 'A')))) continue;
        std::wstring r = std::wstring(1, (wchar_t)L) + L":\\";
        if (GetDriveTypeW(r.c_str()) == DRIVE_REMOVABLE) { SendMessageW(D.dest, CB_ADDSTRING, 0, (LPARAM)r.c_str()); if (defDest.empty()) defDest = r; }
    }
    SendMessageW(D.dest, CB_ADDSTRING, 0, (LPARAM)L"D:\\WinPE_WR");
    SetWindowTextW(D.dest, (defDest.empty() ? std::wstring(L"D:\\WinPE_WR") : defDest).c_str());
    D.destBr = Mk(D.h, L"BUTTON", L"Обзор…", BS_PUSHBUTTON | WS_TABSTOP, ID_DEST_BR);
    D.destInfo = Mk(D.h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, ID_DEST_INFO);

    // 2. База образа
    D.bMedia = Mk(D.h, L"BUTTON", L"Установочный образ Windows (ISO или флешка) — ADK не нужен", BS_RADIOBUTTON | WS_TABSTOP | WS_GROUP, ID_B_MEDIA);
    D.bAdk = Mk(D.h, L"BUTTON", D.adk ? L"ADK WinPE — чистый минимальный образ" : L"ADK WinPE — чистый минимальный образ (ADK не найден автоматически — укажете winpe.wim)", BS_RADIOBUTTON | WS_TABSTOP, ID_B_ADK);
    D.bWinre = Mk(D.h, L"BUTTON", L"winre.wim этой системы — со встроенным «Восстановлением системы» (rstrui)", BS_RADIOBUTTON | WS_TABSTOP, ID_B_WINRE);
    D.base = D.adk ? 1 : 2;

    // 3. Установочный образ: две рамки
    D.srcIso = Mk(D.h, L"BUTTON", L"Файл образа — .iso (подключится сам) или boot.wim (например, sources\\boot.wim распакованного ISO)", BS_RADIOBUTTON | WS_TABSTOP | WS_GROUP, ID_SRC_ISO);
    D.iso = Mk(D.h, L"EDIT", g_lastIso.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, ID_ISO, WS_EX_CLIENTEDGE);
    D.isoBr = Mk(D.h, L"BUTTON", L"Обзор…", BS_PUSHBUTTON | WS_TABSTOP, ID_ISO_BR);
    D.srcDrv = Mk(D.h, L"BUTTON", L"Папка или диск с файлами Windows — распакованный ISO, подключённый образ или установочная флешка (внутри sources\\boot.wim)", BS_RADIOBUTTON | WS_TABSTOP, ID_SRC_DRV);
    D.drv = Mk(D.h, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, ID_DRV);
    for (char L = 'C'; L <= 'Z'; ++L) {
        if (!(mask & (1u << (L - 'A')))) continue;
        std::wstring r = std::wstring(1, (wchar_t)L) + L":\\";
        if (IsFile(r + L"sources\\boot.wim")) { SendMessageW(D.drv, CB_ADDSTRING, 0, (LPARAM)r.c_str()); if (defDrv.empty()) defDrv = r; }
    }
    SetWindowTextW(D.drv, defDrv.c_str());
    D.drvBr = Mk(D.h, L"BUTTON", L"Обзор…", BS_PUSHBUTTON | WS_TABSTOP, ID_DRV_BR);
    D.useIso = !g_lastIso.empty() || defDrv.empty();
    D.check = Mk(D.h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, ID_CHECK);

    D.biosBtn = Mk(D.h, L"BUTTON", L"Флешка уже с WinPE? Сделать загрузочной для BIOS…", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, ID_BIOS);
    Mk(D.h, L"BUTTON", L"Собрать флешку", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, IDOK);
    Mk(D.h, L"BUTTON", L"Отмена", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);

    Layout();
    CreateTips();
    Recheck();
    EnableWindow(owner, FALSE);
    ShowWindow(D.h, SW_SHOW);
    SetFocus(D.base == 2 ? (D.useIso ? D.iso : D.drv) : D.dest);
    MSG msg;
    bool quit = false;
    while (!D.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F1) { ShowHelp("winpe"); continue; }
        if (!IsDialogMessageW(D.h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (!D.done) quit = true;
    // Подключение ISO ещё идёт — дожидаемся (обычно секунды), чтобы не оставить образ подключённым.
    if (D.mountThread) {
        WaitForSingleObject(D.mountThread, 120000);
        MSG mm;
        while (PeekMessageW(&mm, D.h, WM_ISO_DONE, WM_ISO_DONE, PM_REMOVE)) { D.okAfterMount = false; D.pendingIso.clear(); OnMountDone((IsoDone*)mm.lParam); }
        if (D.mountThread) { CloseHandle(D.mountThread); D.mountThread = nullptr; }
    }
    bool ok = D.accepted;
    if (ok && D.action == 2) dest = Unquote(GetText(D.dest));
    if (ok && D.action == 1) {
        dest = Unquote(GetText(D.dest));
        base = D.base;
        std::wstring m;
        int k = 0;
        src = D.base == 2 ? Source(m, k) : D.base == 1 ? D.adkWim : L"";
        baseText = GetText(D.base == 2 ? D.bMedia : D.base == 1 ? D.bAdk : D.bWinre);
        if (D.base == 2 && D.useIso) g_lastIso = src;
        if (D.base == 2 && D.mountOk) g_lastIso = D.mountIso;
    }
    // Подключённый окном ISO: сборка с его буквой отключит его сама; иначе отключаем сейчас (если подключали мы).
    isoDismount.clear();
    if (D.mountOk) {
        bool usedByBuild = ok && D.action == 1 && D.base == 2 &&
                           ((src.size() >= 2 && towupper(src[0]) == (wchar_t)D.mountLetter && src[1] == L':') ||
                            _wcsicmp(src.c_str(), D.mountIso.c_str()) == 0);
        if (usedByBuild) { if (src.size() >= 2 && src[1] == L':') isoDismount = D.mountIso; }   // путь .iso: сборка подключит/отключит сама
        else if (D.mountByUs) DismountAsync(D.mountIso);
    }
    EnableWindow(owner, TRUE);
    DestroyWindow(D.h);
    D.h = nullptr;
    for (HBRUSH b : {D.fill1, D.fill2, D.fill3, D.fill3off, D.white}) DeleteObject(b);
    if (quit) PostQuitMessage(0);
    if (owner) SetForegroundWindow(owner);
    return ok ? D.action : 0;
}

}  // namespace pe

// Свой загрузочный WinPE: база — установочный образ Windows (ISO/флешка), ADK или winre.wim; результат — папка/флешка с автозапуском WinRepair.
void BuildOwnPE() {
    HWND own = V.wnd ? V.wnd : g_ui.main;
    if (UiRunning()) { MessageBoxW(own, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
    std::wstring wdest, wsrc, baseText, wiso;
    int baseMode = 2;
    int act = pe::Run(own, wdest, baseMode, wsrc, baseText, wiso);
    if (act == 0) return;
    std::string dest = trim(U(wdest));
    if (dest.size() < 2) { pe::DismountAsync(wiso); return; }
    if (act == 2) {   // флешка с вручную скопированным WinPE: только активный раздел и загрузочный код
        UiStartJob("winpe-bios", "Флешка WinPE: загрузка в BIOS", [dest](Ctx& c) { JobPrepareBiosBoot(c, dest); });
        return;
    }
    bool isFolder = !QueryBootMedia(dest).isRoot;
    std::string mediaSrc = trim(U(wsrc));
    std::wstring msg = L"В «" + W(dest) + L"» будет собран загрузочный WinPE с автозапуском WinRepair:\n"
                       L" • база: " + baseText + (!mediaSrc.empty() ? L" — " + W(mediaSrc) : std::wstring()) + L"\n"
                       L" • файлы Boot, EFI, bootmgr и sources\\boot.wim в указанной папке будут ПЕРЕЗАПИСАНЫ\n"
                       L" • флешка НЕ форматируется, остальные файлы на ней не трогаются\n" +
                       (isFolder ? std::wstring(L" • это ПАПКА: загружаться с неё нельзя — скопируйте её содержимое в корень флешки; для BIOS-компьютеров затем нажмите «Сделать флешку загрузочной для BIOS» в этом окне\n")
                                 : std::wstring(L" • для BIOS (Legacy) флешка будет сделана активной и в неё запишется загрузочный код — автоматически\n")) +
                       L"\n"
                       L"Распространять собранный образ нельзя (лицензия Microsoft), использовать для своей работы — можно.";
    if (MessageBoxW(own, (msg + L"\n\nПродолжить?").c_str(), L"Подтверждение", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        pe::DismountAsync(wiso);   // сборку не начали — подключённый окном образ ISO не оставляем
        return;
    }
    std::string isoDismount = U(wiso);
    UiStartJob("winpe-build", "Свой WinPE с WinRepair", [dest, baseMode, mediaSrc, isoDismount](Ctx& c) { JobBuildWinPE(c, dest, baseMode, mediaSrc, isoDismount); });
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
    int bh = S(32), rows = 6;
    int stH = H - y - m - rows * (bh + S(6));
    Place(V.status, m, y, W_ - 2 * m, stH);
    y += stH + m;
    int bw = (W_ - 2 * m - S(8)) / 2;
    for (int i = 0; i < 11; ++i) Place(V.btn[i], m + (i % 2) * (bw + S(8)), y + (i / 2) * (bh + S(6)), bw, bh);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (V.btn[10]) Layout(); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(860); mm->ptMinTrackSize.y = S(640); return 0; }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == ID_TARGET && code == CBN_SELCHANGE) { Refresh(); }
            else if (id == ID_STORE && code == CBN_SELCHANGE) Refresh();
            else if (id == ID_FIND) { Rescan(); Refresh(); }
            else if (id >= ID_B0 && id < ID_B0 + 11) {
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
                    case 9: BuildOwnPE(); break;
                    case 10: DestroyWindow(h); break;
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
                           L"Проверить winre.wim (DISM)", L"Свой WinPE с WinRepair (флешка)…", L"Закрыть"};
    for (int i = 0; i < 11; ++i) V.btn[i] = Mk(h, L"BUTTON", bt[i], BS_PUSHBUTTON | WS_TABSTOP, ID_B0 + i);
    Layout();
    ShowWindow(h, SW_SHOW);
    if (V.targets.empty()) { SetWindowTextW(V.status, L"Целевые ОС не найдены."); return; }
    Refresh();
    // offline target: find stores right away so the status can show the WinRE entry
    if (!V.targets[0].online) { Rescan(); Refresh(); }
}

void WinReJobDone() { if (V.wnd) Refresh(); }

void OpenWinPeBuilderImpl() { BuildOwnPE(); }

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
