// WinRepair Toolkit - native Win32 GUI (no .NET / PowerShell / VC++ runtime required).
#include "win/integrity.h"
#include "win/backups.h"
#include "ui_theme.h"
#include <richedit.h>
#include <set>
#include "win/app.h"
#include "modules/catalog.h"
#include "modules/bcd.h"
#include "modules/winre.h"
#include "modules/playbook.h"
#include "ui_tools_common.h"
#include "ui_tools.h"

#include <cctype>
#include <cstring>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

using namespace wr;

namespace {

enum {
    WM_APP_LOG = WM_APP + 1, WM_APP_STATUS, WM_APP_DONE, WM_APP_ASK = WM_APP + 10, WM_APP_CONFIRM, WM_APP_OPENWIN,
    IDC_BANNER = 100, IDC_TARGET, IDC_REFRESH, IDC_DAYS, IDC_EXT, IDC_FORCE, IDC_HOST, IDC_PORT, IDC_SOURCE,
    IDC_TAB, IDC_LOG, IDC_CANCEL, IDC_OPENREP, IDC_OPENLOG, IDC_SAVELOG, IDC_CLEAR, IDC_PROG, IDC_STATUS,
    IDC_L1, IDC_L2, IDC_L3, IDC_L4, IDC_L5, IDC_ACTION0 = 1000,
    IDC_CAT_LIST = 2001, IDC_CAT_FILTER, IDC_CAT_RUN, IDC_CAT_CLOSE, IDC_CAT_DESC, IDC_CAT_E0 = 2010, IDC_CAT_L0 = 2020, IDC_CAT_FIND = 2030, IDC_LANG = 2040, IDC_EXIT = 2050
};

struct Action {
    const wchar_t* text;
    int tab;
    ModuleFn fn;
    const char* name;
    const wchar_t* confirm;  // non-null => mutating, ask first
};

void ShowAbout();
void OpenLastReport();
void OpenCatalog();
void ChangeLanguage();
void QuickCmd(int no);
Target CurrentTarget();

const wchar_t* kTabs[] = {L"Обзор", L"Система", L"Диски", L"Целостность", L"Драйверы и BSOD", L"События", L"Сеть", L"Инструменты ремонта"};

Action kActions[] = {
    {L"Мастер «Что случилось?» (диагностика по симптому)…", 0, nullptr, "triage", nullptr},
    {L"Сценарии ремонта (playbooks)…", 0, nullptr, "playbooks", nullptr},
    {L"Полная диагностика + HTML-отчёт", 0, ModFull, "full", nullptr},
    {L"Открыть последний HTML-отчёт", 0, nullptr, "open-report", nullptr},
    {L"Каталог команд (55)…", 0, nullptr, "catalog", nullptr},
    {L"О программе", 0, nullptr, "about", nullptr},
    {L"Снимок системы", 1, ModSystem, "system", nullptr},
    {L"Память (SMBIOS, диагностика)", 1, ModMemory, "memory", nullptr},
    {L"Загрузка (BCD / GPT / WinRE)", 1, ModBoot, "boot", nullptr},
    {L"Диски, SMART, ошибки ФС", 2, ModDisk, "disk", nullptr},
    {L"Проверка целостности (без изменений)", 3, ModIntegrity, "integrity", nullptr},
    {L"ИСПРАВИТЬ целостность (DISM + SFC)", 3, ModRepair, "repair",
     L"Будет выполнено ИЗМЕНЯЮЩЕЕ действие:\nDISM /RestoreHealth, затем sfc /scannow на выбранной ОС.\n\n"
     L"Операция может занять десятки минут. Перед ремонтом рекомендуется точка восстановления и резервная копия данных.\n\nПродолжить?"},
    {L"Создать точку восстановления", 3, ModRestorePoint, "restorepoint",
     L"Будет создана точка восстановления Windows (изменяющее действие).\n\nПродолжить?"},
    {L"Драйверы и устройства с ошибками", 4, ModDrivers, "drivers", nullptr},
    {L"BSOD: события и дампы", 4, ModBsod, "bsod", nullptr},
    {L"Разбор дампов (заголовок + cdb)", 4, ModDump, "dump", nullptr},
    {L"Ошибки System / Application", 5, ModEvents, "events", nullptr},
    {L"Сетевая диагностика", 6, ModNetwork, "network", nullptr},
    {L"Реестр офлайн-Windows (просмотр и правка)…", 7, nullptr, "registry", nullptr},
    {L"BCD / Загрузка: хранилище, записи, режимы…", 7, nullptr, "bcd", nullptr},
    {L"Автозагрузка и службы офлайн (Autoruns + msconfig)…", 7, nullptr, "autoruns", nullptr},
    {L"Мастер исправлений (SmartFix / Windows Repair)…", 7, nullptr, "fixes", nullptr},
    {L"Восстановление системы: rstrui…", 7, nullptr, "rstrui", nullptr},
    {L"Точки восстановления: состояние", 7, nullptr, "restore-info", nullptr},
    {L"WinRE: вход в среду восстановления, WinRE + WinRepair…", 7, nullptr, "winre", nullptr},
    {L"Выйти в штатное меню WinRE (RecEnv)…", 7, nullptr, "winre-menu", nullptr},
    {L"Сохранить журналы и отчёты на диск…", 7, nullptr, "savedata", nullptr},
    {L"Резервные копии: просмотр и очистка…", 7, nullptr, "backups", nullptr},
    {L"Secure Boot: сертификаты 2023", 7, nullptr, "secureboot", nullptr},
    {L"BitLocker и драйверы хранилища (диск не виден)…", 7, nullptr, "storagefix", nullptr},
};
const int kActionCount = (int)(sizeof kActions / sizeof kActions[0]);

HWND g_wnd, g_tab, g_log, g_target, g_days, g_ext, g_force, g_host, g_port, g_source, g_prog, g_status, g_banner;
HWND g_btn[32], g_cancelBtn, g_refreshBtn, g_lbl[6];
HWND g_langBtn = nullptr, g_exitBtn = nullptr;
HICON g_icon = nullptr;
bool g_logRich = false;
RECT g_targetFrame{};
std::vector<theme::Chip> g_chips;
std::wstring g_hdrSub;
HWND g_cat = nullptr, g_catList, g_catFilter, g_catDesc, g_catRun, g_catClose, g_catEd[6], g_catLb[6], g_catFind;
std::vector<std::string> g_bcdPaths;
std::vector<int> g_catMap;
std::vector<std::string> g_catCats;
HFONT g_font, g_mono;
int g_dpi = 96;
std::vector<Target> g_targets;
Results g_results;
std::atomic<bool> g_cancel(false);
bool g_running = false;
std::string g_lastOut, g_jobName;
ULONGLONG g_jobStart = 0;

int S(int v) { return MulDiv(v, g_dpi, 96); }

// Purpose of a command -> colour (see ui_theme.h)
theme::Cat CatOfAction(const char* n) {
    static const char* guide[] = {"triage", "playbooks", "catalog"};
    static const char* repair[] = {"repair", "fixes", "registry", "bcd", "autoruns", "winre", "winre-menu", "storagefix", "rstrui"};
    static const char* protect[] = {"restorepoint", "savedata", "backups"};
    for (auto g : guide) if (!strcmp(n, g)) return theme::Cat::Guide;
    for (auto g : repair) if (!strcmp(n, g)) return theme::Cat::Repair;
    for (auto g : protect) if (!strcmp(n, g)) return theme::Cat::Protect;
    if (!strcmp(n, "about")) return theme::Cat::Info;
    return theme::Cat::Diag;
}

std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

// Log line colour by level tag: "2026-... [WARN ] text"
int LevelOf(const std::string& line) {
    if (line.find("──────") != std::string::npos) return 5;
    size_t p = line.find('[');
    if (p != std::string::npos && p < 30 && line.size() > p + 6) {
        if (!line.compare(p, 5, "[WARN")) return 1;
        if (!line.compare(p, 6, "[ERROR")) return 2;
        if (!line.compare(p, 4, "[OUT")) return 3;
        if (!line.compare(p, 3, "[OK")) return 4;
    }
    return 0;
}

void AppendLog(const std::string& utf8) {
    std::wstring w = W(utf8) + L"\r\n";
    int len = GetWindowTextLengthW(g_log);
    if (len > 3000000) {  // keep the control responsive: drop the oldest ~1/3
        SendMessageW(g_log, EM_SETSEL, 0, 1000000);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }
    SendMessageW(g_log, EM_SETSEL, (WPARAM)-1, -1);   // end of text (works for EDIT and RichEdit)
    if (g_logRich) {
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof cf;
        cf.dwMask = CFM_COLOR | CFM_BOLD;
        bool bold = false;
        cf.crColor = theme::LevelColor(LevelOf(utf8), &bold);
        cf.dwEffects = bold ? CFE_BOLD : 0;
        SendMessageW(g_log, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
    SendMessageW(g_log, WM_VSCROLL, SB_BOTTOM, 0);
}

void OpenPath(const std::string& p) {
    HINSTANCE r = ShellExecuteW(g_wnd, L"open", W(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        r = ShellExecuteW(g_wnd, nullptr, L"notepad.exe", W(p).c_str(), nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) Logger::I().log(Lvl::Warn, "Не удалось открыть автоматически (нет проводника/браузера). Путь: " + p);
    }
}

void FillTargets() {
    SendMessageW(g_target, CB_RESETCONTENT, 0, 0);
    g_targets = FindTargets();
    for (auto& t : g_targets) SendMessageW(g_target, CB_ADDSTRING, 0, (LPARAM)W(t.label).c_str());
    if (g_targets.empty()) SendMessageW(g_target, CB_ADDSTRING, 0, (LPARAM)TrP(L"(установленная Windows не найдена)"));
    SendMessageW(g_target, CB_SETCURSEL, 0, 0);
}

void ShowTabButtons() {
    int sel = TabCtrl_GetCurSel(g_tab);
    for (int i = 0; i < kActionCount; ++i) ShowWindow(g_btn[i], kActions[i].tab == sel ? SW_SHOW : SW_HIDE);
}

void Layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8);
    int hdrH = S(72);
    MoveWindow(g_banner, 0, 0, 1, 1, TRUE);   // hidden: the header band is painted in WM_PAINT
    MoveWindow(g_exitBtn, W_ - m - S(104), S(20), S(104), S(32), TRUE);
    MoveWindow(g_langBtn, W_ - m - S(104) - S(8) - S(96), S(20), S(96), S(32), TRUE);
    int y = hdrH + S(32);
    int rh = S(24);
    y += S(2);
    g_targetFrame = {m, y, W_ - m, y + S(42)};
    MoveWindow(g_lbl[0], m + S(14), y + S(11), S(104), rh, TRUE);
    MoveWindow(g_target, m + S(122), y + S(7), W_ - 2 * m - S(122) - S(124), S(300), TRUE);
    MoveWindow(g_refreshBtn, W_ - m - S(112), y + S(6), S(100), S(30), TRUE);
    y += S(52);
    MoveWindow(g_lbl[1], m, y + S(3), S(90), rh, TRUE);
    MoveWindow(g_days, m + S(95), y, S(50), rh, TRUE);
    MoveWindow(g_ext, m + S(160), y, S(380), rh, TRUE);
    MoveWindow(g_force, m + S(550), y, W_ - 2 * m - S(550), rh, TRUE);
    y += S(30);
    MoveWindow(g_lbl[2], m, y + S(3), S(90), rh, TRUE);
    MoveWindow(g_host, m + S(95), y, S(190), rh, TRUE);
    MoveWindow(g_lbl[3], m + S(295), y + S(3), S(50), rh, TRUE);
    MoveWindow(g_port, m + S(345), y, S(60), rh, TRUE);
    MoveWindow(g_lbl[4], m + S(420), y + S(3), S(120), rh, TRUE);
    MoveWindow(g_source, m + S(545), y, W_ - 2 * m - S(545), rh, TRUE);
    y += S(32);
    int gap = S(8), bh = S(44);
    int innerW = W_ - 2 * m - S(16);
    int cols = std::max(1, (innerW + gap) / (S(235) + gap));
    int cnt[16] = {0}, maxRows = 1;
    for (int i = 0; i < kActionCount; ++i) cnt[kActions[i].tab]++;
    for (int t = 0; t < 16; ++t) maxRows = std::max(maxRows, (cnt[t] + cols - 1) / cols);
    int tabH = S(34) + maxRows * (bh + gap) + S(6);
    MoveWindow(g_tab, m, y, W_ - 2 * m, tabH, TRUE);
    RECT tr = {m, y, W_ - m, y + tabH};
    TabCtrl_AdjustRect(g_tab, FALSE, &tr);
    int bw = std::max(S(150), (tr.right - tr.left - S(12) - gap * (cols - 1)) / cols);
    int idxInTab[16] = {0};
    for (int i = 0; i < kActionCount; ++i) {
        int t = kActions[i].tab, k = idxInTab[t]++;
        MoveWindow(g_btn[i], tr.left + S(6) + (k % cols) * (bw + gap), tr.top + S(6) + (k / cols) * (bh + gap), bw, bh, TRUE);
    }
    y += tabH + m;
    int barH = S(34);
    MoveWindow(g_log, m, y, W_ - 2 * m, std::max(S(60), H - y - barH - 2 * m - S(22)), TRUE);
    int by = H - barH - S(22) - m + S(0);
    by = y + std::max(S(60), H - y - barH - 2 * m - S(22)) + m;
    int x = m;
    int ws[] = {S(110), S(200), S(170), S(180), S(100)};
    HWND hs[] = {g_cancelBtn, GetDlgItem(g_wnd, IDC_OPENREP), GetDlgItem(g_wnd, IDC_OPENLOG), GetDlgItem(g_wnd, IDC_SAVELOG), GetDlgItem(g_wnd, IDC_CLEAR)};
    for (int i = 0; i < 5; ++i) { MoveWindow(hs[i], x, by, ws[i], barH - S(4), TRUE); x += ws[i] + gap; }
    MoveWindow(g_prog, x + gap, by + S(6), std::max(S(60), W_ - x - m - gap), S(18), TRUE);
    MoveWindow(g_status, m, H - S(22) - m / 2, W_ - 2 * m, S(20), TRUE);
}

void SetRunning(bool run) {
    g_running = run;
    for (int i = 0; i < kActionCount; ++i) EnableWindow(g_btn[i], !run);
    EnableWindow(g_refreshBtn, !run);
    EnableWindow(g_cancelBtn, run);
    if (g_cat) EnableWindow(g_catRun, !run);
    SendMessageW(g_prog, PBM_SETMARQUEE, run, 30);
    ShowWindow(g_prog, run ? SW_SHOW : SW_HIDE);
}

struct JobData { Ctx ctx; std::function<void(Ctx&)> fn; std::string name; };

DWORD WINAPI JobThread(LPVOID p) {
    std::unique_ptr<JobData> d((JobData*)p);
    try { d->fn(d->ctx); }
    catch (const std::exception& e) { Logger::I().log(Lvl::Error, std::string("Необработанное исключение: ") + e.what()); }
    catch (...) { Logger::I().log(Lvl::Error, "Необработанное исключение"); }
    PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
    return 0;
}

JobData* MakeJob(const std::string& name, std::function<void(Ctx&)> fn) {
    auto* d = new JobData();
    int ti = (int)SendMessageW(g_target, CB_GETCURSEL, 0, 0);
    if (ti >= 0 && ti < (int)g_targets.size()) d->ctx.target = g_targets[ti];
    d->ctx.opt.days = std::max(1, std::min(90, _wtoi(GetText(g_days).c_str())));
    d->ctx.opt.extended = SendMessageW(g_ext, BM_GETCHECK, 0, 0) == BST_CHECKED;
    d->ctx.opt.force = SendMessageW(g_force, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::string host = trim(U(GetText(g_host)));
    if (!host.empty()) d->ctx.opt.host = host;
    int port = _wtoi(GetText(g_port).c_str());
    d->ctx.opt.port = (port > 0 && port < 65536) ? port : 443;
    d->ctx.opt.source = trim(U(GetText(g_source)));
    d->ctx.outDir = DataDir() + "Reports\\" + Stamp() + "_" + name + "\\";
    MakeDirs(d->ctx.outDir);
    g_lastOut = d->ctx.outDir;
    d->ctx.cancel = &g_cancel;
    d->ctx.res = &g_results;
    d->fn = fn;
    d->name = name;
    g_jobName = name;
    return d;
}

void RunJob(JobData* d, const std::string& title) {
    g_cancel = false;
    g_jobStart = GetTickCount64();
    SetRunning(true);
    Logger::I().log(Lvl::Info, "────── ЗАПУСК: " + title + " ──────");
    Logger::I().log(Lvl::Info, "Целевая ОС: " + (d->ctx.target.valid ? d->ctx.target.label : std::string("не выбрана")) + "; папка отчёта: " + d->ctx.outDir);
    SetWindowTextW(g_status, L"Выполняется…");
    HANDLE h = CreateThread(nullptr, 0, JobThread, d, 0, nullptr);
    if (h) CloseHandle(h);
    else { delete d; SetRunning(false); }
}

void ManageBackups() {
    std::vector<BackupItem> bk = ListBackups();
    if (bk.empty()) { MessageBoxW(g_wnd, L"Резервных копий нет: папка Data\\Backups пуста.", L"Резервные копии", MB_OK | MB_ICONINFORMATION); return; }
    std::wstring msg = W(fmt("Резервные копии хранятся по одной на систему и не накапливаются.\n\n%s\nДа — удалить все копии.\nНет — открыть папку с копиями.\nОтмена — закрыть окно.", BackupsSummary(bk).c_str()));
    int r = MessageBoxW(g_wnd, msg.c_str(), L"Резервные копии", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
    if (r == IDYES) {
        if (MessageBoxW(g_wnd, L"Удалить все резервные копии? Вернуть изменённые системные файлы из них после этого будет нельзя.", L"Подтверждение", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            int n = DeleteBackups(bk);
            Logger::I().log(Lvl::Ok, fmt("Удалено резервных копий: %d", n));
        }
    } else if (r == IDNO) OpenPath(DataDir() + "Backups");
}

// Copies left by other computers (the Data folder travels on the USB drive) and old-format folders are offered for deletion at start.
void CheckForeignBackups() {
    std::vector<BackupItem> bk = ListBackups();
    if (bk.empty()) return;
    std::set<std::string> mine;
    mine.insert(VolumeId(""));
    for (auto& t : g_targets) mine.insert(VolumeId(t.root));
    std::vector<BackupItem> foreign;
    for (auto& i : bk) if (i.legacy || !mine.count(i.name)) foreign.push_back(i);
    if (foreign.empty()) return;
    std::wstring msg = W(fmt("В папке Data\\Backups найдены резервные копии от других систем или старого формата.\nСейчас здесь должны быть только копии проверяемых вами систем.\n\n%s\nУдалить эти копии?", BackupsSummary(foreign).c_str()));
    if (MessageBoxW(g_wnd, msg.c_str(), L"Резервные копии других систем", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        int n = DeleteBackups(foreign);
        Logger::I().log(Lvl::Ok, fmt("Удалено резервных копий других систем: %d", n));
    } else Logger::I().log(Lvl::Info, "Резервные копии других систем оставлены по выбору пользователя.");
}

void StartAction(int idx) {
    const Action& a = kActions[idx];
    if (!a.fn) {
        if (!strcmp(a.name, "open-report")) OpenLastReport();
        else if (!strcmp(a.name, "catalog")) OpenCatalog();
        else if (!strcmp(a.name, "triage")) OpenTriage();
        else if (!strcmp(a.name, "playbooks")) OpenPlaybooks();
        else if (!strcmp(a.name, "registry")) OpenRegistryEditor();
        else if (!strcmp(a.name, "bcd")) OpenBcdManager();
        else if (!strcmp(a.name, "autoruns")) OpenAutoruns();
        else if (!strcmp(a.name, "fixes")) OpenFixWizard();
        else if (!strcmp(a.name, "rstrui")) { Target t = CurrentTarget(); QuickCmd(t.valid && t.online ? 56 : 57); }
        else if (!strcmp(a.name, "restore-info")) QuickCmd(59);
        else if (!strcmp(a.name, "winre")) OpenWinReManager();
        else if (!strcmp(a.name, "savedata")) OpenSaveData();
        else if (!strcmp(a.name, "backups")) ManageBackups();
        else if (!strcmp(a.name, "secureboot")) QuickCmd(80);
        else if (!strcmp(a.name, "storagefix")) OpenStorageAccess();
        else if (!strcmp(a.name, "winre-menu")) {
            const Env& e = GetEnv();
            if (e.winreShell) {
                if (MessageBoxW(g_wnd, L"Закрыть WinRepair и открыть штатное меню WinRE (Выбор действия)?\n\nПосле выхода из меню можно вернуться через «Командная строка» → WinRepair.exe.", L"WinRE", MB_YESNO | MB_ICONQUESTION) == IDYES) DestroyWindow(g_wnd);
            } else if (e.isWinRE) {
                std::string rec = PathExists(e.sysDrive + "\\sources\\recovery\\RecEnv.exe") ? e.sysDrive + "\\sources\\recovery\\RecEnv.exe" : e.winDir + "\\System32\\Recovery\\RecEnv.exe";
                ShellExecuteW(g_wnd, L"open", W(rec).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else MessageBoxW(g_wnd, L"Эта функция работает только внутри WinRE. Откройте «WinRE: вход в среду восстановления…», чтобы загрузиться в WinRE.", L"WinRE", MB_OK | MB_ICONINFORMATION);
        }
        else ShowAbout();
        return;
    }
    if (a.confirm && MessageBoxW(g_wnd, a.confirm, L"Подтверждение изменяющего действия", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    RunJob(MakeJob(a.name, a.fn), U(a.text));
}

void StartCommand(const CmdItem& it, const CmdParams& p) {
    JobData* d = MakeJob(fmt("cmd%02d", it.no), ModCommand);
    d->ctx.opt.cmdNo = it.no;
    d->ctx.opt.cp = p;
    RunJob(d, fmt("Команда №%d: %s", it.no, it.title));
}

void ChangeLanguage() {
    if (g_running) { MessageBoxW(g_wnd, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
    bool toEn = CurrentLang() == Lang::RU;
    const wchar_t* q = toEn ? L"Switch the interface to English? The program will restart." : L"Сменить язык интерфейса на русский? Программа перезапустится.";
    if (MessageBoxW(g_wnd, q, L"Language / Язык", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    SetLanguage(toEn ? Lang::EN : Lang::RU);
    wchar_t exe[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    ShellExecuteW(nullptr, L"open", exe, nullptr, nullptr, SW_SHOWNORMAL);
    DestroyWindow(g_wnd);
}

void OpenLastReport() {
    if (!g_results.lastReportHtml.empty() && PathExists(g_results.lastReportHtml)) OpenPath(g_results.lastReportHtml);
    else Logger::I().log(Lvl::Warn, "HTML-отчёта ещё нет — сначала запустите «Полную диагностику».");
}

void ShowAbout() {
    const Env& e = GetEnv();
    std::string t = fmt("%s %s\n\nНативное приложение Win32 (C++17), без зависимостей от .NET, PowerShell и VC++ Runtime.\n\n"
                        "Среда: %s, %s%s\nАдминистратор: %s\nЖурналы и отчёты: %s",
                        kAppName, kAppVersion, e.osName.c_str(), e.arch.c_str(), e.isWinPE ? " (WinPE)" : "", e.isAdmin ? "да" : "нет", DataDir().c_str());
    MessageBoxW(g_wnd, W(t).c_str(), L"О программе", MB_OK | MB_ICONINFORMATION);
}

void SaveLogAs() {
    wchar_t file[MAX_PATH] = L"WinRepair_log.txt";
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = g_wnd;
    o.lpstrFilter = L"Текст (*.txt)\0*.txt\0Все файлы\0*.*\0";
    o.lpstrFile = file;
    o.nMaxFile = MAX_PATH;
    o.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&o)) return;
    std::wstring w = GetText(g_log), n;
    for (size_t i = 0; i < w.size(); ++i) {   // RichEdit stores line breaks as a lone CR
        n += w[i];
        if (w[i] == L'\r' && (i + 1 >= w.size() || w[i + 1] != L'\n')) n += L'\n';
    }
    if (WriteText(U(file), U(n))) Logger::I().log(Lvl::Ok, "Журнал сохранён: " + U(file));
}

// ---- command catalogue window ---------------------------------------------------------------------------
Target CurrentTarget() {
    int ti = (int)SendMessageW(g_target, CB_GETCURSEL, 0, 0);
    return (ti >= 0 && ti < (int)g_targets.size()) ? g_targets[ti] : Target();
}

CmdParams ReadParams() {
    CmdParams p;
    p.vol = trim(U(GetText(g_catEd[0])));
    p.src = trim(U(GetText(g_catEd[1])));
    p.host = trim(U(GetText(g_catEd[2])));
    p.win = trim(U(GetText(g_catEd[3])));
    p.efi = trim(U(GetText(g_catEd[4])));
    p.bcd = trim(U(GetText(g_catEd[5])));
    return p;
}

const CmdItem* CatSelected() {
    int s = (int)SendMessageW(g_catList, LB_GETCURSEL, 0, 0);
    if (s < 0 || s >= (int)g_catMap.size()) return nullptr;
    return &Catalog()[(size_t)g_catMap[(size_t)s]];
}

void CatUpdate() {
    const CmdItem* it = CatSelected();
    unsigned mask = it ? it->params : 0;
    for (int i = 0; i < 6; ++i) { EnableWindow(g_catEd[i], (mask & (1u << i)) != 0); EnableWindow(g_catLb[i], (mask & (1u << i)) != 0); }
    EnableWindow(g_catFind, (mask & P_BCD) != 0);
    EnableWindow(g_catRun, it != nullptr && !g_running);
    if (!it) { SetWindowTextW(g_catDesc, L"Выберите команду в списке."); return; }
    std::string sw = ScopeWarning(*it, CurrentTarget());
    std::string t = fmt("Команда №%d\r\nКатегория: %s\r\nТип: %s\r\nУровень: %s\r\n\r\n%s\r\n%s\r\nБудет выполнено:\r\n%s", it->no, it->cat, KindText(it->kind),
                        RiskText(it->risk), it->note, sw.empty() ? "" : ("\r\n" + sw + "\r\n").c_str(), CmdBuild(*it, ReadParams(), CurrentTarget(), "<папка отчёта>\\scratch", "<временный файл>").c_str());
    SetWindowTextW(g_catDesc, W(t).c_str());
}

void CatFillList() {
    int fi = (int)SendMessageW(g_catFilter, CB_GETCURSEL, 0, 0);
    std::string cat = (fi > 0 && fi <= (int)g_catCats.size()) ? g_catCats[(size_t)fi - 1] : "";
    SendMessageW(g_catList, LB_RESETCONTENT, 0, 0);
    g_catMap.clear();
    const auto& all = Catalog();
    for (size_t i = 0; i < all.size(); ++i) {
        if (!cat.empty() && cat != all[i].cat) continue;
        std::string line = fmt("%2d. [%s] %s", all[i].no, Tr(all[i].cat).c_str(), Tr(all[i].title).c_str());
        SendMessageW(g_catList, LB_ADDSTRING, 0, (LPARAM)W(line).c_str());
        g_catMap.push_back((int)i);
    }
    SendMessageW(g_catList, LB_SETCURSEL, 0, 0);
}

bool ConfirmStart(HWND owner, const CmdItem& it, const CmdParams& p) {
    std::string err;
    if (!CmdValidate(it, p, err)) { MessageBoxW(owner, W(err).c_str(), L"Проверьте параметры", MB_OK | MB_ICONWARNING); return false; }
    Target t = CurrentTarget();
    std::string sw = ScopeWarning(it, t);
    std::string msg = fmt("Команда №%d [%s]\n\n%s\n\nЦелевая ОС: %s\nТип: %s\nУровень: %s\n\n", it.no, it.cat,
                          CmdBuild(it, p, t, "<папка отчёта>\\scratch", "<временный файл>").c_str(), t.valid ? t.label.c_str() : "не выбрана", KindText(it.kind), RiskText(it.risk));
    if (!sw.empty()) msg += sw + "\n\n";
    msg += it.kind == CKind::Manual ? "Будет показана инструкция. Ничего не выполняется.\n\nПоказать?" : "Выполнить эту команду?";
    UINT flags = MB_YESNO | (it.risk == Risk::Safe ? MB_ICONQUESTION : (MB_ICONWARNING | MB_DEFBUTTON2));
    if (MessageBoxW(owner, W(msg).c_str(), L"Подтверждение команды", flags) != IDYES) return false;
    if (it.risk == Risk::Danger &&
        MessageBoxW(owner, L"ОПАСНАЯ операция: возможна потеря загрузки или данных.\n\nУ вас есть резервная копия, и вы понимаете последствия?\nПодтвердите повторно.",
                    L"Повторное подтверждение", MB_YESNO | MB_ICONSTOP | MB_DEFBUTTON2) != IDYES)
        return false;
    return true;
}

void CatRun() {
    if (g_running) { MessageBoxW(g_cat, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
    const CmdItem* it = CatSelected();
    if (!it) { MessageBoxW(g_cat, L"Выберите команду.", L"Каталог команд", MB_OK | MB_ICONINFORMATION); return; }
    CmdParams p = ReadParams();
    if (ConfirmStart(g_cat, *it, p)) StartCommand(*it, p);
}

void QuickCmd(int no) {
    if (g_running) { MessageBoxW(g_wnd, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
    const CmdItem* it = FindCmd(no);
    if (!it) return;
    Target t = CurrentTarget();
    CmdParams p;
    p.vol = t.valid ? std::string(1, t.letter()) + ":" : "C:";
    p.win = t.valid ? t.winDir : "C:\\Windows";
    p.host = trim(U(GetText(g_host)));
    p.src = trim(U(GetText(g_source)));
    if (ConfirmStart(g_wnd, *it, p)) StartCommand(*it, p);
}

void CatFindBcd() {
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    auto st = FindBcdStores();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    SendMessageW(g_catEd[5], CB_RESETCONTENT, 0, 0);
    g_bcdPaths.clear();
    for (auto& s : st) { if (s.path.empty()) continue; SendMessageW(g_catEd[5], CB_ADDSTRING, 0, (LPARAM)W(s.label).c_str()); g_bcdPaths.push_back(s.path); }
    if (g_bcdPaths.empty()) MessageBoxW(g_cat, L"Хранилища BCD не найдены.", L"BCD", MB_OK | MB_ICONINFORMATION);
    else { SendMessageW(g_catEd[5], CB_SHOWDROPDOWN, TRUE, 0); }
}

HWND MkC(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

void CatLayout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8);
    int leftW = std::max(S(380), W_ * 56 / 100);
    MoveWindow(g_catFilter, m, m, leftW, S(300), TRUE);
    MoveWindow(g_catList, m, m + S(32), leftW, H - 2 * m - S(32), TRUE);
    int x = leftW + 2 * m, rw = W_ - x - m, y = m;
    int descH = std::max(S(120), H - S(6 * 50) - S(60) - 2 * m);
    MoveWindow(g_catDesc, x, y, rw, descH, TRUE);
    y += descH + m;
    for (int i = 0; i < 6; ++i) {
        MoveWindow(g_catLb[i], x, y, i == 5 ? rw - S(110) : rw, S(18), TRUE);
        MoveWindow(g_catEd[i], x, y + S(18), rw, i == 5 ? S(300) : S(24), TRUE);
        if (i == 5) MoveWindow(g_catFind, x + rw - S(106), y - S(2), S(106), S(20), TRUE);
        y += S(50);
    }
    MoveWindow(g_catRun, x, H - m - S(32), S(150), S(32), TRUE);
    MoveWindow(g_catClose, x + S(160), H - m - S(32), S(110), S(32), TRUE);
}

LRESULT CALLBACK CatProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE: if (g_catClose) CatLayout(h); return 0;
        case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(820); mm->ptMinTrackSize.y = S(560); return 0; }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_CAT_LIST && code == LBN_SELCHANGE) CatUpdate();
            else if (id == IDC_CAT_LIST && code == LBN_DBLCLK) CatRun();
            else if (id == IDC_CAT_FILTER && code == CBN_SELCHANGE) { CatFillList(); CatUpdate(); }
            else if (id >= IDC_CAT_E0 && id < IDC_CAT_E0 + 5 && code == EN_CHANGE) CatUpdate();
            else if (id == IDC_CAT_E0 + 5 && code == CBN_SELCHANGE) { int si = (int)SendMessageW(g_catEd[5], CB_GETCURSEL, 0, 0); if (si >= 0 && si < (int)g_bcdPaths.size()) { SetWindowTextW(g_catEd[5], W(g_bcdPaths[(size_t)si]).c_str()); CatUpdate(); } }
            else if (id == IDC_CAT_E0 + 5 && code == CBN_EDITCHANGE) CatUpdate();
            else if (id == IDC_CAT_FIND) CatFindBcd();
            else if (id == IDC_CAT_RUN) CatRun();
            else if (id == IDC_CAT_CLOSE) DestroyWindow(h);
            return 0;
        }
        case WM_DESTROY: g_cat = nullptr; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void OpenCatalog() {
    if (g_cat) { ShowWindow(g_cat, SW_RESTORE); SetForegroundWindow(g_cat); return; }
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = CatProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hbrBackground = theme::BgBrush();
        wc.lpszClassName = L"WinRepairCatalog";
        RegisterClassExW(&wc);
        reg = true;
    }
    g_cat = CreateWindowExW(0, L"WinRepairCatalog", L"Каталог команд — каждое действие требует подтверждения", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, S(1060), S(680), g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND h = g_cat;
    g_catFilter = MkC(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_CAT_FILTER);
    SendMessageW(g_catFilter, CB_ADDSTRING, 0, (LPARAM)TrP(L"Все категории"));
    g_catCats.clear();
    for (const auto& it : Catalog())
        if (std::find(g_catCats.begin(), g_catCats.end(), std::string(it.cat)) == g_catCats.end()) g_catCats.push_back(it.cat);
    for (const auto& c : g_catCats) SendMessageW(g_catFilter, CB_ADDSTRING, 0, (LPARAM)W(c).c_str());
    SendMessageW(g_catFilter, CB_SETCURSEL, 0, 0);
    g_catList = MkC(h, L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, IDC_CAT_LIST, WS_EX_CLIENTEDGE);
    g_catDesc = MkC(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, IDC_CAT_DESC, WS_EX_CLIENTEDGE);
    static const wchar_t* lbl[6] = {L"Том (например C:)", L"Источник (/Source: папка или wim:файл:индекс)", L"Узел / имя (host, IP)", L"Путь Windows (например C:\\Windows)", L"EFI-раздел (буква, например S:)", L"BCD-хранилище (файл; пусто = хранилище текущей среды)"};
    Target t = CurrentTarget();
    std::string def[6] = {t.valid ? std::string(1, t.letter()) + ":" : "C:", trim(U(GetText(g_source))), trim(U(GetText(g_host))), t.valid ? t.winDir : "C:\\Windows", "", ""};
    for (int i = 0; i < 6; ++i) {
        g_catLb[i] = MkC(h, L"STATIC", lbl[i], SS_LEFT, IDC_CAT_L0 + i);
        if (i == 5) g_catEd[i] = MkC(h, L"COMBOBOX", W(def[i]).c_str(), CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, IDC_CAT_E0 + i);
        else g_catEd[i] = MkC(h, L"EDIT", W(def[i]).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, IDC_CAT_E0 + i, WS_EX_CLIENTEDGE);
    }
    g_catFind = MkC(h, L"BUTTON", L"Найти BCD", BS_PUSHBUTTON | WS_TABSTOP, IDC_CAT_FIND);
    g_catRun = MkC(h, L"BUTTON", L"Выполнить…", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_CAT_RUN);
    g_catClose = MkC(h, L"BUTTON", L"Закрыть", BS_PUSHBUTTON | WS_TABSTOP, IDC_CAT_CLOSE);
    SendMessageW(g_catDesc, WM_SETFONT, (WPARAM)g_mono, TRUE);
    CatLayout(h);
    CatFillList();
    CatUpdate();
    ShowWindow(h, SW_SHOW);
}

HWND Mk(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

void CreateControls() {
    const Env& e = GetEnv();
    g_banner = Mk(L"STATIC", L"", SS_LEFT, IDC_BANNER);
    std::string b = fmt("Среда: %s, %s%s  |  Администратор: %s  |  Версия %s", e.osName.c_str(), e.arch.c_str(), e.isWinRE ? "  [WinRE]" : (e.isWinPE ? "  [WinPE]" : ""), e.isAdmin ? "да" : "НЕТ", kAppVersion);
    SetWindowTextW(g_banner, W(b).c_str());
    ShowWindow(g_banner, SW_HIDE);
    g_hdrSub = W(e.osName);
    g_chips.push_back({W(e.arch), 0});
    if (e.isWinRE) g_chips.push_back({L"WinRE", 0}); else if (e.isWinPE) g_chips.push_back({L"WinPE", 0});
    g_chips.push_back({e.isAdmin ? W("Администратор: да") : W("БЕЗ прав администратора"), e.isAdmin ? 1 : 2});
    g_chips.push_back({W(std::string("v") + kAppVersion), 0});
    g_langBtn = Mk(L"BUTTON", CurrentLang() == Lang::RU ? L"English" : L"Русский", BS_OWNERDRAW | WS_TABSTOP, IDC_LANG);
    g_exitBtn = Mk(L"BUTTON", L"Выход", BS_OWNERDRAW | WS_TABSTOP, IDC_EXIT);
    g_lbl[0] = Mk(L"STATIC", L"Целевая ОС:", SS_LEFT, IDC_L1);
    g_target = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_TARGET);
    g_refreshBtn = Mk(L"BUTTON", L"Обновить", BS_OWNERDRAW | WS_TABSTOP, IDC_REFRESH);
    g_lbl[1] = Mk(L"STATIC", L"Период, дней:", SS_LEFT, IDC_L2);
    g_days = Mk(L"EDIT", L"3", ES_NUMBER | WS_TABSTOP, IDC_DAYS, WS_EX_CLIENTEDGE);
    g_ext = Mk(L"BUTTON", L"Расширенный режим (ScanHealth, chkdsk /scan)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_EXT);
    g_force = Mk(L"BUTTON", L"Точка восстановления: игнорировать лимит 24 ч", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_FORCE);
    g_lbl[2] = Mk(L"STATIC", L"Сеть, хост:", SS_LEFT, IDC_L3);
    g_host = Mk(L"EDIT", L"www.microsoft.com", ES_AUTOHSCROLL | WS_TABSTOP, IDC_HOST, WS_EX_CLIENTEDGE);
    g_lbl[3] = Mk(L"STATIC", L"Порт:", SS_LEFT, IDC_L4);
    g_port = Mk(L"EDIT", L"443", ES_NUMBER | WS_TABSTOP, IDC_PORT, WS_EX_CLIENTEDGE);
    g_lbl[4] = Mk(L"STATIC", L"DISM /Source:", SS_LEFT, IDC_L5);
    g_source = Mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_SOURCE, WS_EX_CLIENTEDGE);
    g_tab = Mk(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, IDC_TAB);
    for (int i = 0; i < (int)(sizeof kTabs / sizeof kTabs[0]); ++i) {
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = (LPWSTR)kTabs[i];
        TabCtrl_InsertItem(g_tab, i, &it);
    }
    for (int i = 0; i < kActionCount; ++i) {
        g_btn[i] = Mk(L"BUTTON", kActions[i].text, BS_OWNERDRAW | WS_TABSTOP, IDC_ACTION0 + i);
        SetParent(g_btn[i], g_wnd);
        SetWindowPos(g_btn[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    {   // RichEdit (msftedit.dll) gives per-level colours; fall back to a plain EDIT where it is missing (some WinPE images)
        g_logRich = LoadLibraryW(L"msftedit.dll") != nullptr;
        g_log = Mk(g_logRich ? L"RICHEDIT50W" : L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL | ES_AUTOHSCROLL, IDC_LOG, WS_EX_CLIENTEDGE);
        if (!g_log && g_logRich) {
            g_logRich = false;
            g_log = Mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL | ES_AUTOHSCROLL, IDC_LOG, WS_EX_CLIENTEDGE);
        }
        if (g_logRich) SendMessageW(g_log, EM_SETBKGNDCOLOR, 0, (LPARAM)theme::LogBg());
    }
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_mono, TRUE);
    SendMessageW(g_log, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
    g_cancelBtn = Mk(L"BUTTON", L"Отмена", BS_OWNERDRAW | WS_TABSTOP, IDC_CANCEL);
    Mk(L"BUTTON", L"Открыть папку отчётов", BS_OWNERDRAW | WS_TABSTOP, IDC_OPENREP);
    Mk(L"BUTTON", L"Открыть лог-файл", BS_OWNERDRAW | WS_TABSTOP, IDC_OPENLOG);
    Mk(L"BUTTON", L"Сохранить журнал…", BS_OWNERDRAW | WS_TABSTOP, IDC_SAVELOG);
    Mk(L"BUTTON", L"Очистить", BS_OWNERDRAW | WS_TABSTOP, IDC_CLEAR);
    g_prog = Mk(PROGRESS_CLASSW, L"", PBS_MARQUEE, IDC_PROG);
    g_status = Mk(L"STATIC", L"Готово", SS_LEFT, IDC_STATUS);
    SendMessageW(g_tab, WM_SETFONT, (WPARAM)theme::Semi(), TRUE);
    SendMessageW(g_target, WM_SETFONT, (WPARAM)theme::Big(), TRUE);
    SendMessageW(g_target, CB_SETITEMHEIGHT, (WPARAM)-1, S(22));
    SendMessageW(g_lbl[0], WM_SETFONT, (WPARAM)theme::Big(), TRUE);
    EnableWindow(g_cancelBtn, FALSE);
    ShowWindow(g_prog, SW_HIDE);
    for (HWND hb : {g_langBtn, g_exitBtn, g_refreshBtn, g_cancelBtn, GetDlgItem(g_wnd, IDC_OPENREP), GetDlgItem(g_wnd, IDC_OPENLOG), GetDlgItem(g_wnd, IDC_SAVELOG), GetDlgItem(g_wnd, IDC_CLEAR)}) theme::AttachHover(hb);
    for (int i = 0; i < kActionCount; ++i) theme::AttachHover(g_btn[i]);
    FillTargets();
    ShowTabButtons();
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE: g_wnd = h; return 0;
        case WM_SIZE: if (g_tab) Layout(); InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            RECT hr = rc;
            hr.bottom = S(72);
            theme::PaintHeader(dc, hr, g_icon, W(kAppName), g_hdrSub, g_chips);
            theme::PaintLegend(dc, S(10), hr.bottom + S(7), rc.right - S(20));
            theme::PaintTargetPanel(dc, g_targetFrame);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* di = (DRAWITEMSTRUCT*)lp;
            if (di->CtlType != ODT_BUTTON) break;
            int id = (int)di->CtlID;
            theme::Cat c = theme::Cat::Info;
            if (id >= IDC_ACTION0 && id < IDC_ACTION0 + kActionCount) c = CatOfAction(kActions[id - IDC_ACTION0].name);
            else if (id == IDC_EXIT) c = theme::Cat::Exit;
            else if (id == IDC_LANG) c = theme::Cat::Header;
            else if (id == IDC_CANCEL) c = theme::Cat::Repair;
            else if (id == IDC_REFRESH) c = theme::Cat::Diag;
            theme::DrawButton(di, c, theme::IsHover(di->hwndItem));
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            if ((HWND)lp == g_log) {
                SetTextColor(dc, theme::LogText());
                SetBkColor(dc, theme::LogBg());
                return (LRESULT)theme::LogBrush();
            }
            if ((HWND)lp == g_lbl[0]) {   // "Target OS" label sits on the highlighted panel
                SetBkColor(dc, theme::TintColor());
                SetTextColor(dc, RGB(16, 78, 168));
                return (LRESULT)theme::TintBrush();
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, theme::Text());
            return (LRESULT)theme::BgBrush();
        }
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = S(920);
            mm->ptMinTrackSize.y = S(720);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)lp;
            if (nh->hwndFrom == g_tab && nh->code == TCN_SELCHANGE) ShowTabButtons();
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id >= IDC_ACTION0 && id < IDC_ACTION0 + kActionCount) { if (!g_running) StartAction(id - IDC_ACTION0); }
            else if (id == IDC_CANCEL) { g_cancel = true; Logger::I().log(Lvl::Warn, "Запрошена отмена…"); }
            else if (id == IDC_REFRESH) { FillTargets(); Logger::I().log(Lvl::Info, fmt("Найдено целевых ОС: %zu", g_targets.size())); }
            else if (id == IDC_CLEAR) SetWindowTextW(g_log, L"");
            else if (id == IDC_LANG) ChangeLanguage();
            else if (id == IDC_EXIT) SendMessageW(h, WM_CLOSE, 0, 0);
            else if (id == IDC_OPENLOG) OpenPath(Logger::I().path());
            else if (id == IDC_OPENREP) OpenPath(g_lastOut.empty() ? DataDir() + "Reports" : g_lastOut);
            else if (id == IDC_SAVELOG) SaveLogAs();
            return 0;
        }
        case WM_APP_ASK: {
            struct AskData { std::wstring title, prompt, def; std::string out; bool ok; };
            auto* d = (AskData*)lp;
            std::vector<wr::ui::FormField> f(1);
            f[0].label = d->prompt;
            f[0].value = d->def;
            d->ok = wr::ui::FormDialog(h, d->title, f);
            if (d->ok) d->out = U(f[0].value);
            return 0;
        }
        case WM_APP_CONFIRM: {
            struct ConfData { std::wstring text; int level; bool yes; };
            auto* d = (ConfData*)lp;
            UINT fl = MB_YESNO | (d->level >= 2 ? MB_ICONSTOP : d->level == 1 ? MB_ICONWARNING : MB_ICONQUESTION) | (d->level > 0 ? MB_DEFBUTTON2 : 0);
            d->yes = MessageBoxW(h, d->text.c_str(), L"Подтверждение шага сценария", fl) == IDYES;
            return 0;
        }
        case WM_APP_OPENWIN: {
            std::unique_ptr<std::string> n((std::string*)lp);
            if (*n == "storage") wr::OpenStorageAccess();
            else if (*n == "bcd") wr::OpenBcdManager();
            else if (*n == "autoruns") wr::OpenAutoruns();
            else if (*n == "registry") wr::OpenRegistryEditor();
            else if (*n == "winre") wr::OpenWinReManager();
            else if (*n == "fixes") wr::OpenFixWizard();
            else if (*n == "triage") wr::OpenTriage();
            else if (*n == "playbooks") wr::OpenPlaybooks();
            else if (*n == "catalog") OpenCatalog();
            return 0;
        }
        case WM_APP_LOG: {
            std::unique_ptr<std::string> s((std::string*)lp);
            AppendLog(*s);
            return 0;
        }
        case WM_APP_STATUS: {
            std::unique_ptr<std::string> s((std::string*)lp);
            SetWindowTextW(g_status, s->empty() ? L"Выполняется…" : W(*s).c_str());
            return 0;
        }
        case WM_APP_DONE: {
            double sec = (double)(GetTickCount64() - g_jobStart) / 1000.0;
            SetRunning(false);
            Logger::I().log(g_cancel ? Lvl::Warn : Lvl::Ok, fmt("────── ЗАВЕРШЕНО за %.1f с%s. Отчёт: %s", sec, g_cancel ? " (отменено)" : "", g_lastOut.c_str()));
            SetWindowTextW(g_status, W(fmt("Готово (%.1f с). Отчёт: %s", sec, g_lastOut.c_str())).c_str());
            if (!g_results.lastReportHtml.empty() && g_lastOut.find("_full") != std::string::npos && !g_cancel) OpenLastReport();
            if (g_jobName == "drvload" || g_jobName == "bl-unlock") FillTargets();
            UiJobDone();
            return 0;
        }
        case WM_CLOSE:
            if (GetEnv().winreShell && !g_running) {
                int r = MessageBoxW(h, L"Вы в WinRE с WinRepair.\n\nДа — выйти из WinRepair и открыть штатное меню WinRE.\nНет — перезагрузить компьютер.\nОтмена — остаться в WinRepair.", L"Выход из WinRepair", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (r == IDCANCEL) return 0;
                if (r == IDNO) { RunProcess(RebootCommand(), "", nullptr, nullptr, 20); return 0; }
            }
            if (g_running && MessageBoxW(h, L"Операция выполняется. Прервать и выйти?", L"Выход", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            g_cancel = true;
            {
                std::vector<BackupItem> bk = ListBackups();
                if (!bk.empty()) {
                    std::wstring msg = W(fmt("На диске остались резервные копии состояния проверенных систем (кусты реестра, BCD).\n\n%s\nДа — удалить копии сейчас.\nНет — оставить (удалить можно позже кнопкой «Резервные копии…»; при запуске на другом ПК копии чужих систем будут предложены к удалению).\nОтмена — не выходить.", BackupsSummary(bk).c_str()));
                    int r = MessageBoxW(h, msg.c_str(), L"Резервные копии", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
                    if (r == IDCANCEL) { g_cancel = false; return 0; }
                    if (r == IDYES) Logger::I().log(Lvl::Ok, fmt("Удалено резервных копий: %d", DeleteBackups(bk)));
                }
            }
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

namespace wr {
Target UiCurrentTarget() { return CurrentTarget(); }
std::vector<Target> UiTargets() { return g_targets; }
bool UiRunning() { return g_running; }
void UiRefreshTargets() { FillTargets(); }
void UiSelectTarget(const Target& t) {
    for (size_t i = 0; i < g_targets.size(); ++i)
        if (g_targets[i].winDir == t.winDir) { SendMessageW(g_target, CB_SETCURSEL, (WPARAM)i, 0); return; }
}
void UiRunCatalog(int no) { QuickCmd(no); }
bool UiConfirm(const std::wstring& text, int level) {
    if (!g_wnd) return false;  // headless: risky steps are never confirmed automatically
    struct ConfData { std::wstring text; int level; bool yes; } d{text, level, false};
    SendMessageW(g_wnd, WM_APP_CONFIRM, 0, (LPARAM)&d);
    return d.yes;
}
bool UiAsk(const std::wstring& title, const std::wstring& prompt, const std::wstring& def, std::string& out) {
    if (!g_wnd) { out = U(def); return true; }
    struct AskData { std::wstring title, prompt, def; std::string out; bool ok; } d{title, prompt, def, "", false};
    SendMessageW(g_wnd, WM_APP_ASK, 0, (LPARAM)&d);
    out = d.out;
    return d.ok;
}
void UiOpenWindow(const std::string& name) {
    if (g_wnd) PostMessageW(g_wnd, WM_APP_OPENWIN, 0, (LPARAM) new std::string(name));
}
void UiRunModule(const std::string& name) {
    for (int i = 0; i < kActionCount; ++i)
        if (!strcmp(kActions[i].name, name.c_str())) { StartAction(i); return; }
}
void UiStartJob(const std::string& name, const std::string& title, std::function<void(Ctx&)> fn, const Target* target) {
    if (g_running) { MessageBoxW(g_wnd, L"Дождитесь завершения текущей операции.", L"Занято", MB_OK | MB_ICONINFORMATION); return; }
    JobData* d = MakeJob(name, fn);
    if (target) d->ctx.target = *target;
    RunJob(d, title);
}
}  // namespace wr

namespace {

int RunHeadless() {
    // wrapper: WinRepair.exe /full [/out:<dir>]  -> full diagnostic without UI; exit code 0 = ok, 2 = poor health
    std::string out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.rfind(L"/out:", 0) == 0) out = U(a.substr(5));
    }
    if (argv) LocalFree(argv);
    if (out.empty()) out = DataDir() + "Reports\\" + Stamp() + "_full\\";
    if (out.back() != '\\') out += '\\';
    MakeDirs(out);
    Results res;
    Ctx c;
    auto t = FindTargets();
    if (!t.empty()) c.target = t[0];
    c.outDir = out;
    c.res = &res;
    ModFull(c);
    return res.lastReportHtml.empty() ? 1 : 0;
}

IntegrityReport g_integrity;

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    InitEnvironment();
    InitLanguage();
    Logger::I().init(DataDir() + "Logs\\");
    ReloadPlaybooks();
    Logger::I().log(Lvl::Info, fmt("%s %s запущен. ОС: %s, %s%s, админ=%s", kAppName, kAppVersion, GetEnv().osName.c_str(), GetEnv().arch.c_str(),
                                   GetEnv().isWinPE ? " (WinPE)" : "", GetEnv().isAdmin ? "да" : "нет"));
    g_integrity = RunIntegrityCheck();
    Logger::I().log(g_integrity.tampered() ? Lvl::Error : Lvl::Info, IntegritySummary(g_integrity));
    for (auto& f : g_integrity.mismatched) Logger::I().log(Lvl::Error, "Файл изменён относительно MANIFEST.sha256: " + f);
    std::wstring cl = GetCommandLineW();
    if (g_integrity.tampered() && cl.find(L"/full") == std::wstring::npos)
        MessageBoxW(nullptr, W("Обнаружено нарушение целостности программы (см. журнал).\nФайлы могли быть изменены или повреждены. Используйте программу только если доверяете источнику.").c_str(),
                    W("WinRepair — целостность").c_str(), MB_ICONSTOP | MB_OK);
    if (cl.find(L"/full") != std::wstring::npos) return RunHeadless();

    INITCOMMONCONTROLSEX ic{sizeof ic, ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);

    theme::Init(g_dpi);
    g_font = theme::Body();
    g_mono = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WinRepairMain";
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_icon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, S(48), S(48), 0);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, (W(kAppName) + L" — диагностика и ремонт Windows").c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(1120), S(840), nullptr, nullptr, hi, nullptr);
    g_wnd = w;
    g_ui.main = w; g_ui.font = g_font; g_ui.mono = g_mono; g_ui.dpi = g_dpi;
    CreateControls();
    Logger::I().attachUi(w, WM_APP_LOG, WM_APP_STATUS);
    Layout();
    ShowWindow(w, show);
    UpdateWindow(w);
    CheckForeignBackups();
    AppendLog("Файл журнала: " + Logger::I().path());
    if (!GetEnv().isAdmin) Logger::I().log(Lvl::Error, "Программа запущена БЕЗ прав администратора — большинство функций будет недоступно.");
    if (GetEnv().isWinPE) Logger::I().log(Lvl::Info, "Обнаружена WinPE: выберите установленную Windows в списке «Целевая ОС» (режим офлайн).");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_cat && IsDialogMessageW(g_cat, &msg)) continue;
        if (!IsDialogMessageW(w, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return (int)msg.wParam;
}
