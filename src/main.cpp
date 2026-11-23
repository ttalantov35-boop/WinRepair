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
#include "modules/undo.h"
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
    IDC_TAB, IDC_LOG, IDC_CANCEL, IDC_OPENREP, IDC_OPENLOG, IDC_SAVELOG, IDC_CLEAR, IDC_COPYLOG, IDC_PROG, IDC_STATUS,
    IDC_L1, IDC_L2, IDC_L3, IDC_L4, IDC_L5, IDC_L6, IDC_DESC, IDC_UNDO, IDC_ABOUT, IDC_ACTION0 = 1000,
    IDC_CAT_LIST = 2001, IDC_CAT_FILTER, IDC_CAT_RUN, IDC_CAT_CLOSE, IDC_CAT_DESC, IDC_CAT_E0 = 2010, IDC_CAT_L0 = 2020, IDC_CAT_FIND = 2030, IDC_LANG = 2040, IDC_EXIT = 2050, IDC_HELPBTN = 2060
};

struct Action {
    const wchar_t* text;
    int tab;
    ModuleFn fn;
    const char* name;
    const wchar_t* confirm;  // non-null => mutating, ask first
};

void ShowAbout();
void OpenUndoSession();
void OpenLastReport();
void OpenCatalog();
void ChangeLanguage();
void PutClipboard(const std::wstring& txt);
LRESULT CALLBACK LogSubProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
void QuickCmd(int no);
Target CurrentTarget();

const wchar_t* kTabs[] = {L"1 Старт", L"2 Система", L"3 Диски", L"4 Загрузка", L"5 Сбои", L"6 Целостность", L"7 Доступ", L"8 Защита", L"9 Сервис"};

const wchar_t* kStageNote[] = {
    L"Начните здесь: мастер и сценарии ведут по шагам, полная диагностика ничего не меняет.",
    L"Только чтение: сведения о системе, памяти и сети. Ничего не изменяется.",
    L"Только чтение: состояние дисков и файловой системы. Разделы и BitLocker — отдельные окна, читайте подсказки.",
    L"Проверка загрузки только читает; остальные окна могут менять загрузчик и WinRE — читайте подсказки.",
    L"Только чтение: драйверы, синие экраны, дампы и ошибки журналов. Ничего не изменяется.",
    L"Сначала проверка (без изменений), потом ремонт: он меняет систему и просит подтверждение.",
    L"Доступ и запуск: пароль и реестр могут менять систему — сначала точка восстановления.",
    L"Защита: точки восстановления, резервные копии и откат. Делайте перед ремонтом.",
    L"Сервис: каталог команд и командная строка. «О программе» и «Отменить изменения сеанса» — справа, на любой вкладке.",
};

Action kActions[] = {
    // ---- 1 Старт ----
    {L"Мастер «Что случилось?» (диагностика по симптому)…", 0, nullptr, "triage", nullptr},
    {L"Сценарии ремонта (playbooks)…", 0, nullptr, "playbooks", nullptr},
    {L"Полная диагностика + HTML-отчёт", 0, ModFull, "full", nullptr},
    {L"Открыть последний HTML-отчёт", 0, nullptr, "open-report", nullptr},
    // ---- 2 Система ----
    {L"Снимок системы", 1, ModSystem, "system", nullptr},
    {L"Память (SMBIOS, диагностика)", 1, ModMemory, "memory", nullptr},
    {L"Сетевая диагностика", 1, ModNetwork, "network", nullptr},
    // ---- 3 Диски ----
    {L"Диски, SMART, ошибки ФС", 2, ModDisk, "disk", nullptr},
    {L"Разделы диска (создать, удалить, форматировать)…", 2, nullptr, "partition", nullptr},
    {L"BitLocker и драйверы хранилища (диск не виден)…", 2, nullptr, "storagefix", nullptr},
    // ---- 4 Загрузка ----
    {L"Загрузка (BCD / GPT / WinRE)", 3, ModBoot, "boot", nullptr},
    {L"BCD / Загрузка: хранилище, записи, режимы…", 3, nullptr, "bcd", nullptr},
    {L"WinRE: вход в среду восстановления, WinRE + WinRepair…", 3, nullptr, "winre", nullptr},
    {L"Выйти в штатное меню WinRE (RecEnv)…", 3, nullptr, "winre-menu", nullptr},
    {L"Создать загрузочную флешку WinPE с WinRepair…", 3, nullptr, "winpeusb", nullptr},
    {L"Secure Boot: сертификаты 2023", 3, nullptr, "secureboot", nullptr},
    // ---- 5 Сбои ----
    {L"Драйверы и устройства с ошибками", 4, ModDrivers, "drivers", nullptr},
    {L"BSOD: события и дампы", 4, ModBsod, "bsod", nullptr},
    {L"Разбор дампов (заголовок + cdb)", 4, ModDump, "dump", nullptr},
    {L"Справочник кодов BSOD…", 4, nullptr, "bsodref", nullptr},
    {L"Ошибки System / Application", 4, ModEvents, "events", nullptr},
    // ---- 6 Целостность ----
    {L"Проверка целостности (без изменений)", 5, ModIntegrity, "integrity", nullptr},
    {L"ИСПРАВИТЬ целостность (DISM + SFC)", 5, ModRepair, "repair",
     L"Будет выполнено ИЗМЕНЯЮЩЕЕ действие:\nDISM /RestoreHealth, затем sfc /scannow на выбранной ОС.\n\n"
     L"Операция может занять десятки минут. Перед ремонтом рекомендуется точка восстановления и резервная копия данных.\n\nПродолжить?"},
    {L"Мастер исправлений после заражений…", 5, nullptr, "fixes", nullptr},
    // ---- 7 Доступ ----
    {L"Снятие пароля и разблокировка учётных записей…", 6, nullptr, "password", nullptr},
    {L"Реестр офлайн-Windows (просмотр и правка)…", 6, nullptr, "registry", nullptr},
    {L"Автозагрузка и службы: текущая и офлайн-Windows (Autoruns + msconfig)…", 6, nullptr, "autoruns", nullptr},
    {L"Установленные приложения: текущая и офлайн-Windows (удалить программу)…", 6, nullptr, "apps", nullptr},
    {L"Офлайн-проводник: файлы выбранной Windows…", 6, nullptr, "explorer", nullptr},
    // ---- 8 Защита ----
    {L"Создать точку восстановления", 7, ModRestorePoint, "restorepoint",
     L"Будет создана точка восстановления Windows (изменяющее действие).\n\nПродолжить?"},
    {L"Точки восстановления: состояние", 7, nullptr, "restore-info", nullptr},
    {L"Восстановление системы: rstrui…", 7, nullptr, "rstrui", nullptr},
    {L"Резервные копии: просмотр и очистка…", 7, nullptr, "backups", nullptr},
    {L"Сохранить журналы и отчёты на диск…", 7, nullptr, "savedata", nullptr},
    // ---- 9 Сервис ----
    {L"Каталог команд…", 8, nullptr, "catalog", nullptr},
    {L"Командная строка (администратор)…", 8, nullptr, "cmd", nullptr},
};
const int kActionCount = (int)(sizeof kActions / sizeof kActions[0]);

HWND g_wnd, g_tab, g_log, g_target, g_days, g_ext, g_force, g_host, g_port, g_source, g_prog, g_status, g_banner;
HWND g_btn[40], g_cancelBtn, g_refreshBtn, g_lbl[6], g_hint, g_stage, g_undoBtn, g_aboutBtn;
HWND g_langBtn = nullptr, g_exitBtn = nullptr, g_helpBtn = nullptr;
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

// What each command does (shown in the card under the tabs when the pointer is on a button)
struct ActionDesc { const char* name; const wchar_t* text; };
const ActionDesc kDescs[] = {
    {"triage", L"Выберите, что случилось (не загружается, синий экран, медленно работает, нет сети и т. д.). Программа выполнит набор проверок только на чтение, покажет найденные проблемы по важности и предложит действие для каждой."},
    {"playbooks", L"Готовые пошаговые сценарии ремонта: ошибка 0x7B, зацикленное обновление, ремонт загрузки UEFI и другие. Перед запуском показывается весь план, каждый шаг, меняющий систему, подтверждается отдельно."},
    {"full", L"Запускает 9 диагностических модулей подряд, выставляет оценку состояния системы и сохраняет HTML-отчёт в папку отчётов. Ничего не изменяет."},
    {"open-report", L"Открывает в браузере последний сохранённый HTML-отчёт полной диагностики."},
    {"catalog", L"Каталог из 83 команд Windows (winver, systeminfo, sfc, DISM, bcdedit, diskpart, netsh и др.) с описанием, уровнем риска и точной командной строкой. Каждый запуск подтверждается отдельно."},
    {"about", L"Версия программы, разработчик (Tynychsoft), среда запуска, права, расположение журналов и отчётов, результат проверки целостности."},
    {"cmd", L"Консоль cmd с правами администратора — для своих команд (diskpart, bcdedit, dism, sfc и др.). В WinPE и WinRE открывается сразу, в обычной Windows — через запрос UAC. Открывается в корне выбранной целевой ОС. Команды из консоли не попадают в журнал программы."},
    {"system", L"Снимок системы: версия и сборка Windows, имя и состояние компьютера, процессор, память, BIOS и плата, TPM, Secure Boot, диски. Только чтение."},
    {"memory", L"Модули памяти по данным SMBIOS (объём, тип, скорость, слоты) и результаты диагностики памяти Windows, если она запускалась. Даёт рекомендации при подозрении на неисправное ОЗУ. Только чтение."},
    {"boot", L"Состояние загрузки выбранной ОС: хранилище BCD, записи загрузки, схема дисков (GPT/MBR), системный раздел ESP и среда WinRE. Показывает замечания, например отсутствующую запись. Только чтение."},
    {"disk", L"Состояние дисков: SMART и здоровье, тома и свободное место, ошибки файловой системы из журнала событий за выбранный период. В расширенном режиме добавляется chkdsk /scan. Ничего не меняет."},
    {"integrity", L"Проверяет целостность системных файлов и хранилища компонентов Windows (DISM, SFC) только в режиме проверки: ничего не исправляет, показывает, есть ли нарушения."},
    {"repair", L"ИСПРАВЛЯЕТ целостность: DISM /RestoreHealth, затем sfc /scannow на выбранной ОС. Может занять десятки минут. Перед запуском создайте точку восстановления и сохраните данные. Потребуется подтверждение."},
    {"restorepoint", L"Создаёт точку восстановления Windows, чтобы можно было откатить изменения. Обычно система разрешает не чаще одной точки в 24 часа: галка рядом снимает это ограничение."},
    {"drivers", L"Устройства с ошибками драйверов (код проблемы, ИД оборудования) и список установленных пакетов драйверов. Помогает найти устройство без драйвера. Только чтение."},
    {"bsod", L"События BugCheck за выбранный период и найденные файлы дампов (Minidump, MEMORY.DMP). Код синего экрана — категория, а не диагноз. Только чтение."},
    {"dump", L"Разбор файлов дампов: заголовок и коды остановки; если в папке Tools есть cdb.exe, выполняется подробный анализ. Только чтение."},
    {"bsodref", L"Справочник типовых кодов синего экрана: что означает код, с чего начать проверку и что важно знать. Поиск по коду (7B, 0x7B) и названию. Только чтение."},
    {"events", L"Ошибки и критические события из журналов System и Application за выбранный период. Только чтение."},
    {"network", L"Сетевая диагностика: адаптеры, настройки IP, шлюз, DNS, проверка связи с указанным хостом и портом. Только чтение."},
    {"registry", L"Офлайн-редактор реестра выбранной Windows: просмотр и правка кустов (SYSTEM, SOFTWARE, профили пользователей). Перед изменением сохраняется копия ветки; правки подтверждаются и пишутся в журнал."},
    {"bcd", L"Менеджер загрузки: просмотр и правка записей BCD, режимы безопасной загрузки при следующем запуске, создание загрузочных файлов (bcdboot). Перед изменением делается копия хранилища."},
    {"apps", L"Список программ офлайн-Windows из реестра (машина и пользователи) с выделением антивирусов и средств защиты. Для программы, из-за которой Windows не запускается: отключить её службы, драйверы и автозапуск, переместить папку в карантин (обратимо) и убрать запись из списка. Деинсталлятор без работающей Windows не запустить — это принудительное удаление."},
    {"explorer", L"Автономный проводник для файлов выбранной (в том числе неработающей) Windows: дерево дисков, копирование, перемещение, переименование, поиск. Удаление по умолчанию — в карантин WinRepair_Removed (обратимо), есть окончательное удаление и получение прав владельца, если доступ запрещён."},
    {"autoruns", L"Автозагрузка и службы выбранной (в том числе офлайн) Windows: ключи Run, задачи, службы, драйверы. Подозрительные записи можно отключить, режим запуска служб менять (аналог msconfig). Отключение обратимо."},
    {"fixes", L"Обратимые исправления после заражения: Winlogon Shell/Userinit, IFEO, AppInit_DLLs, политики, прокси, файл hosts и др. Каждое исправление описано, подтверждается и записывается в журнал."},
    {"password", L"Снятие пароля и разблокировка локальных учётных записей выбранной офлайн-Windows (из WinPE/WinRE). Снятие пароля = вход с пустым паролем; можно включить отключённую запись и снять блокировку. Учётные записи Microsoft офлайн не сбрасываются. Перед изменением сохраняется копия куста SAM; новый пароль не задаётся и в журнал не пишется."},
    {"partition", L"Разделы диска (обёртка над diskpart) для работы из WinPE/WinRE: список дисков и их разделов/томов, создание, удаление, форматирование, назначение буквы, активный раздел (MBR), сжатие/расширение, очистка диска (clean) и конвертация GPT/MBR. ВСЕ операции необратимы — данные не восстановить; для продвинутых пользователей."},
    {"rstrui", L"Запускает мастер восстановления системы (rstrui.exe) для отката к точке восстановления, если она есть. Меняет систему, потребуется подтверждение."},
    {"winpeusb", L"Собирает независимый загрузочный WinPE с автозапуском WinRepair: укажите корень флешки (FAT32) — и с неё можно загрузить любой компьютер. База — Windows ADK (если установлен) или winre.wim этой системы."},
    {"restore-info", L"Показывает, какие точки восстановления есть в системе, их дату и описание. Только чтение."},
    {"winre", L"Среда восстановления Windows (WinRE): состояние, вход в неё при следующей загрузке или сразу, создание образа WinRE с этой утилитой и его удаление."},
    {"winre-menu", L"Из WinRE выходит в штатное меню среды восстановления (Поиск и устранение неисправностей, Командная строка и др.). Работает только внутри WinRE."},
    {"savedata", L"Копирует журналы и отчёты на выбранный диск: оперативный диск WinPE/WinRE очищается при перезагрузке, и результаты иначе потеряются."},
    {"backups", L"Показывает резервные копии кустов реестра, BCD и файлов, сделанные перед изменениями: размер и расположение. Можно удалить все копии или открыть папку. Хранится одна копия на систему."},
    {"undo", L"Показывает всё, что программа изменила в этом сеансе (по журналу резервных копий), и возвращает состояние на его начало: кусты реестра — копией целиком, BCD — импортом, файлы и карантин — обратно на место. Каждый шаг подтверждается и записывается в журнал."},
    {"secureboot", L"Проверка Secure Boot: включён ли он и есть ли в KEK и db сертификаты 2023 года взамен истекающих в 2026. Только чтение."},
    {"storagefix", L"Если диск не виден или синий экран 0x7B: поиск томов, заблокированных BitLocker (разблокировка ключом), устройств без драйвера хранилища (Intel VMD/RST, NVMe, RAID) и загрузка драйвера."},
};

const wchar_t* DescOf(const char* name) {
    for (const auto& d : kDescs) if (!strcmp(d.name, name)) return d.text;
    return L"";
}

// Purpose of a command -> colour (see ui_theme.h)
theme::Cat CatOfAction(const char* n) {
    static const char* guide[] = {"triage", "playbooks", "catalog", "cmd", "bsodref"};
    static const char* repair[] = {"repair", "fixes", "password", "partition", "registry", "bcd", "autoruns", "winre", "winre-menu", "winpeusb", "storagefix", "rstrui", "apps", "explorer"};
    static const char* protect[] = {"restorepoint", "savedata", "backups", "undo"};
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
        cf.crTextColor = theme::LevelColor(LevelOf(utf8), &bold);
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

void Layout();
void ShowTabButtons() {
    int sel = TabCtrl_GetCurSel(g_tab);
    for (int i = 0; i < kActionCount; ++i) ShowWindow(g_btn[i], kActions[i].tab == sel ? SW_SHOW : SW_HIDE);
    if (g_stage && sel >= 0 && sel < (int)(sizeof kStageNote / sizeof kStageNote[0])) SetWindowTextW(g_stage, TrW(kStageNote[sel]).c_str());
    Layout();   // options (period, DISM source, host...) follow the selected tab
}

// Options live inside the tab they belong to, right under that tab's buttons (tab index = name: 1 = 2 Система, 2 = 3 Диски, 4 = 5 Сбои, 5 = 6 Целостность).
int OptionsStripH(int tab) {
    switch (tab) {
        case 2: case 4: case 1: case 7: return S(88);   // one row of options + a two-line hint
        case 5: return S(126);                          // two rows + hint
        default: return 0;
    }
}

void PlaceOptions(const RECT& tr, const int* cnt, int cols, int bh, int gap) {
    int sel = TabCtrl_GetCurSel(g_tab);
    HWND all[] = {g_lbl[1], g_days, g_ext, g_force, g_lbl[2], g_host, g_lbl[3], g_port, g_lbl[4], g_source, g_hint};
    for (HWND o : all) ShowWindow(o, SW_HIDE);
    if (sel < 0 || sel > 15 || !OptionsStripH(sel)) return;
    int rowsB = (cnt[sel] + cols - 1) / cols;
    int ox = tr.left + S(10), oy = tr.top + S(6) + rowsB * (bh + gap) + S(6), ow = (int)(tr.right - tr.left) - S(20);
    int rowH = S(30), step = S(38);
    auto put = [&](HWND o, int x, int yy, int w) { MoveWindow(o, x, yy, w, rowH, TRUE); ShowWindow(o, SW_SHOW); };
    auto label = [&](HWND o, int x, int yy, int w) { MoveWindow(o, x, yy + S(5), w, rowH - S(5), TRUE); ShowWindow(o, SW_SHOW); };
    const wchar_t* hint = L"";
    int rows = 1;
    if (sel == 2) {
        label(g_lbl[1], ox, oy, S(130));
        put(g_days, ox + S(134), oy, S(64));
        SetWindowTextW(g_ext, L"Расширенный режим: chkdsk /scan (дольше)");
        put(g_ext, ox + S(230), oy, ow - S(230));
        hint = L"Период — за сколько последних дней искать ошибки дисков и файловой системы в журнале событий. Расширенный режим добавляет проверку chkdsk /scan (занимает больше времени).";
    } else if (sel == 5) {
        SetWindowTextW(g_ext, L"Расширенный режим: DISM /ScanHealth (дольше)");
        put(g_ext, ox, oy, ow);
        label(g_lbl[4], ox, oy + step, S(210));
        put(g_source, ox + S(214), oy + step, ow - S(214));
        rows = 2;
        hint = L"Источник DISM (/Source) нужен, если Windows Update недоступен: укажите папку с файлами, например D:\\sources\\sxs, или WIM:D:\\sources\\install.wim:1. Пусто — обычный источник.";
    } else if (sel == 4) {
        label(g_lbl[1], ox, oy, S(130));
        put(g_days, ox + S(134), oy, S(64));
        hint = L"Период — за сколько последних дней показывать ошибки и предупреждения из журналов System и Application.";
    } else if (sel == 1) {
        label(g_lbl[2], ox, oy, S(170));
        put(g_host, ox + S(174), oy, S(260));
        label(g_lbl[3], ox + S(450), oy, S(60));
        put(g_port, ox + S(514), oy, S(90));
        hint = L"Хост и порт для проверки связи: пинг, разрешение имени (DNS) и подключение к TCP-порту (443 — HTTPS, 80 — HTTP). Подойдёт любой доступный сайт или адрес.";
    } else if (sel == 7) {
        put(g_force, ox, oy, ow);
        hint = L"Точка восстановления Windows обычно создаётся не чаще раза в 24 часа; галка отключает это ограничение (для «Создать точку восстановления» и точки перед ремонтом).";
    }
    SetWindowTextW(g_hint, hint);
    MoveWindow(g_hint, ox, oy + rows * step + S(2), ow, S(40), TRUE);
    ShowWindow(g_hint, SW_SHOW);
}

void Layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int W_ = rc.right, H = rc.bottom, m = S(8);
    int hdrH = S(72);
    MoveWindow(g_banner, 0, 0, 1, 1, TRUE);   // hidden: the header band is painted in WM_PAINT
    MoveWindow(g_exitBtn, W_ - m - S(104), S(20), S(104), S(32), TRUE);
    MoveWindow(g_langBtn, W_ - m - S(104) - S(8) - S(96), S(20), S(96), S(32), TRUE);
    MoveWindow(g_helpBtn, W_ - m - S(104) - S(8) - S(96) - S(8) - S(110), S(20), S(110), S(32), TRUE);
    int y = hdrH + S(32);
    int rh = S(24);
    y += S(2);
    g_targetFrame = {m, y, W_ - m, y + S(42)};
    MoveWindow(g_lbl[0], m + S(14), y + S(11), S(104), rh, TRUE);
    MoveWindow(g_target, m + S(122), y + S(7), W_ - 2 * m - S(122) - S(124), S(300), TRUE);
    MoveWindow(g_refreshBtn, W_ - m - S(112), y + S(6), S(100), S(30), TRUE);
    y += S(52);
    int gap = S(8), bh = S(44);
    int innerW = W_ - 2 * m - S(16);
    int cols = std::max(1, (innerW + gap) / (S(235) + gap));
    int cnt[16] = {0}, maxRows = 1;
    for (int i = 0; i < kActionCount; ++i) cnt[kActions[i].tab]++;
    for (int t = 0; t < 16; ++t) maxRows = std::max(maxRows, (cnt[t] + cols - 1) / cols);
    int need = 0;   // tallest tab: its button rows + its options strip
    for (int t = 0; t < 16; ++t) {
        int rowsT = (cnt[t] + cols - 1) / cols;
        need = std::max(need, rowsT * (bh + gap) + OptionsStripH(t));
    }
    (void)maxRows;
    need += S(36);   // the stage caption line (+ the always-visible undo button on its right) at the bottom of the panel
    int tabHdr = S(50);
    int tabH = tabHdr + need + S(8);
    MoveWindow(g_tab, m, y, W_ - 2 * m, tabH, TRUE);
    {   // the tab strip spans the whole width: every tab gets an equal share up to the right edge
        RECT tc;
        GetClientRect(g_tab, &tc);
        int n = (int)(sizeof kTabs / sizeof kTabs[0]);
        TabCtrl_SetItemSize(g_tab, std::max(S(60), (int)(tc.right - tc.left - S(6)) / n), S(44));   // a few pixels spare: no scroll arrows, every tab visible
    }
    RECT tr = {m, y, W_ - m, y + tabH};
    TabCtrl_AdjustRect(g_tab, FALSE, &tr);
    int bw = std::max(S(150), ((int)(tr.right - tr.left) - S(12) - gap * (cols - 1)) / cols);
    int idxInTab[16] = {0};
    for (int i = 0; i < kActionCount; ++i) {
        int t = kActions[i].tab, k = idxInTab[t]++;
        MoveWindow(g_btn[i], tr.left + S(6) + (k % cols) * (bw + gap), tr.top + S(6) + (k / cols) * (bh + gap), bw, bh, TRUE);
    }
    PlaceOptions(tr, cnt, cols, bh, gap);
    {   // bottom row of the panel: caption on the left, "undo session changes" always visible at the right end
        int ubw = S(270), abw = S(210), ubh = S(30), ry = tr.bottom - S(34);
        MoveWindow(g_undoBtn, tr.right - S(10) - ubw, ry, ubw, ubh, TRUE);
        ShowWindow(g_undoBtn, SW_SHOW);
        MoveWindow(g_aboutBtn, tr.right - S(10) - ubw - S(8) - abw, ry, abw, ubh, TRUE);
        ShowWindow(g_aboutBtn, SW_SHOW);
        MoveWindow(g_stage, tr.left + S(10), ry + S(4), std::max(S(100), (int)(tr.right - tr.left) - S(40) - ubw - abw), S(24), TRUE);
        ShowWindow(g_stage, SW_SHOW);
    }
    y += tabH + S(6);
    y += m;
    int barH = S(34);
    MoveWindow(g_log, m, y, W_ - 2 * m, std::max(S(60), H - y - barH - 2 * m - S(22)), TRUE);
    int by = H - barH - S(22) - m + S(0);
    by = y + std::max(S(60), H - y - barH - 2 * m - S(22)) + m;
    int x = m;
    int ws[] = {S(110), S(200), S(170), S(180), S(170), S(100)};
    HWND hs[] = {g_cancelBtn, GetDlgItem(g_wnd, IDC_OPENREP), GetDlgItem(g_wnd, IDC_OPENLOG), GetDlgItem(g_wnd, IDC_SAVELOG), GetDlgItem(g_wnd, IDC_COPYLOG), GetDlgItem(g_wnd, IDC_CLEAR)};
    for (int i = 0; i < 6; ++i) { MoveWindow(hs[i], x, by, ws[i], barH - S(4), TRUE); x += ws[i] + gap; }
    MoveWindow(g_prog, x + gap, by + S(6), std::max(S(60), W_ - x - m - gap), S(18), TRUE);
    MoveWindow(g_status, m, H - S(22) - m / 2, W_ - 2 * m, S(20), TRUE);
}

void SetRunning(bool run) {
    g_running = run;
    for (int i = 0; i < kActionCount; ++i) EnableWindow(g_btn[i], !run);
    EnableWindow(g_refreshBtn, !run);
    EnableWindow(g_cancelBtn, run);
    if (g_undoBtn) EnableWindow(g_undoBtn, !run);
    if (g_aboutBtn) EnableWindow(g_aboutBtn, !run);
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

// Tab colour = how risky the tab is (by the new order): blue = read-only, orange = may change the system, green = protection
theme::Cat TabCat(int tab) {
    switch (tab) {
        case 0: return theme::Cat::Guide;     // 1 Старт
        case 3: case 5: case 6: return theme::Cat::Repair;   // 4 Загрузка, 6 Целостность, 7 Доступ
        case 7: return theme::Cat::Protect;   // 8 Защита
        case 8: return theme::Cat::Info;      // 9 Сервис
        default: return theme::Cat::Diag;     // 2 Система, 3 Диски, 5 Сбои
    }
}

const wchar_t* CatLabel(theme::Cat c) {
    switch (c) {
        case theme::Cat::Diag: return L"Диагностика — только чтение";
        case theme::Cat::Guide: return L"Мастера и сценарии";
        case theme::Cat::Repair: return L"Изменяет систему";
        case theme::Cat::Protect: return L"Защита и сохранение";
        default: return L"Служебное";
    }
}

// Hover tooltip for every command button: title + what the command does.
HWND g_tip = nullptr;
std::wstring g_tipText;

void CreateTooltips() {
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_tip) return;
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, S(420));
    SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_INITIAL, 350);
    SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_RESHOW, 100);
    for (int i = 0; i < kActionCount; ++i) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = g_wnd;
        ti.uId = (UINT_PTR)g_btn[i];
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    for (HWND hb : {g_helpBtn, g_undoBtn, g_aboutBtn}) {   // кнопка заголовка / нижней строки панели: текст — в TooltipNeedText
        TOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = g_wnd;
        ti.uId = (UINT_PTR)hb;
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

// TTN_GETDISPINFO: title and text for the button under the pointer
void TooltipNeedText(NMTTDISPINFOW* di) {
    int id = GetDlgCtrlID((HWND)di->hdr.idFrom);
    if (id == IDC_HELPBTN) {
        static std::wstring title;
        title = TrW(L"Справка");
        g_tipText = TrW(L"Справка по программе (F1). В окнах инструментов F1 открывает раздел про это окно.");
        SendMessageW(g_tip, TTM_SETTITLEW, TTI_INFO, (LPARAM)title.c_str());
        di->lpszText = const_cast<wchar_t*>(g_tipText.c_str());
        return;
    }
    if (id == IDC_UNDO || id == IDC_ABOUT) {   // buttons of the bottom row: text from the same descriptions as the old tab buttons
        const char* nm = id == IDC_UNDO ? "undo" : "about";
        static std::wstring ttl;
        ttl = TrW(id == IDC_UNDO ? L"Отменить изменения сеанса" : L"О программе WinRepair");
        g_tipText = W(U(std::wstring(DescOf(nm))));
        SendMessageW(g_tip, TTM_SETTITLEW, TTI_INFO, (LPARAM)ttl.c_str());
        di->lpszText = const_cast<wchar_t*>(g_tipText.c_str());
        return;
    }
    if (id < IDC_ACTION0 || id >= IDC_ACTION0 + kActionCount) return;
    const Action& a = kActions[id - IDC_ACTION0];
    std::wstring t = W(U(std::wstring(a.text)));
    while (!t.empty() && (t.back() == L'…' || t.back() == L'.')) t.pop_back();
    g_tipText = W(U(std::wstring(DescOf(a.name))));
    if (g_tipText.empty()) g_tipText = t;
    SendMessageW(g_tip, TTM_SETTITLEW, TTI_INFO, (LPARAM)t.c_str());
    di->lpszText = const_cast<wchar_t*>(g_tipText.c_str());
}

// Командная строка cmd с правами администратора. В WinPE/WinRE программа уже работает с полными правами —
// консоль наследует их; в обычной Windows без прав — запрос UAC (runas). Открывается в корне целевой ОС.
void OpenAdminCmd() {
    const Env& e = GetEnv();
    std::wstring cmd = W(SysTool("cmd.exe"));
    Target t = CurrentTarget();
    std::wstring dir = t.valid && IsDir(t.root) ? WRaw(t.root) : WRaw(ExeDir());
    std::wstring title = TrW(L"WinRepair — командная строка (администратор)");
    std::wstring l1 = t.valid ? W("Целевая Windows: " + t.winDir) : TrW(L"Целевая Windows не выбрана.");
    std::wstring l2 = TrW(L"Буквы дисков в WinPE могут отличаться: diskpart, затем list volume. Список команд: help");
    // Внутри /k "..." нельзя символы & | < > ^ в тексте сообщений — они служебные для cmd.
    auto safe = [](std::wstring s) { for (wchar_t& ch : s) if (ch == L'&' || ch == L'|' || ch == L'<' || ch == L'>' || ch == L'^' || ch == L'"') ch = L' '; return s; };
    std::wstring args = L"/k \"title " + safe(title) + L" & echo " + safe(l1) + L" & echo " + safe(l2) + L" & echo.\"";
    bool ok = false;
    if (e.isAdmin) {
        std::wstring cl = L"\"" + cmd + L"\" " + args;
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        ok = CreateProcessW(cmd.c_str(), &cl[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT, nullptr, dir.c_str(), &si, &pi) != FALSE;
        if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    } else {
        ok = (INT_PTR)ShellExecuteW(g_wnd, L"runas", cmd.c_str(), args.c_str(), dir.c_str(), SW_SHOWNORMAL) > 32;
    }
    if (ok) Logger::I().log(Lvl::Info, "Открыта командная строка (администратор) в " + U(dir) + ". Команды из неё в журнал программы не попадают.");
    else Logger::I().log(Lvl::Error, "Не удалось открыть командную строку: " + ErrMsg(GetLastError()));
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

// «Отменить изменения сеанса»: список изменений по журналу копий и возврат состояния на начало сеанса.
void OpenUndoSession() {
    std::vector<SessionChange> v = SessionChanges();
    bool onlyHiveSnapshots = !v.empty() && SessionAuditCount() == 0;
    for (const auto& c : v) if (c.kind != "hive") onlyHiveSnapshots = false;
    if (onlyHiveSnapshots) v.clear();   // кусты копировались при загрузке реестра, но правок не было
    bool previous = false;
    if (v.empty()) {
        std::string started;
        std::vector<SessionChange> prev = LoadPreviousSession(started);
        if (prev.empty()) {
            MessageBoxW(g_wnd, L"В этом сеансе программа ничего не меняла.\nЖурнала прошлого сеанса с сохранившимися копиями тоже нет.", L"Отменить изменения сеанса", MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::wstring q = W(fmt("В этом сеансе программа ничего не меняла.\n\nНайден журнал прошлого сеанса (начат: %s), записей с сохранившимися копиями: %zu.\nПоказать его и предложить откат?", started.c_str(), prev.size()));
        if (MessageBoxW(g_wnd, q.c_str(), L"Отменить изменения сеанса", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        v = prev;
        previous = true;
    }
    const char* head = previous ? "Изменения прошлого сеанса (%zu):\n\n%s\nОткат вернёт состояние на начало сеанса. Текущие файлы сохраняются рядом с расширением .before_undo.\n\nДа — откатить всё. Нет — открыть папку журналов. Отмена — закрыть."
                                : "Изменения этого сеанса (%zu):\n\n%s\nОткат вернёт состояние на начало сеанса. Текущие файлы сохраняются рядом с расширением .before_undo.\n\nДа — откатить всё. Нет — открыть папку журналов. Отмена — закрыть.";
    std::wstring msg = W(fmt(head, v.size(), UndoSummary(v).c_str()));
    int r = MessageBoxW(g_wnd, msg.c_str(), L"Отменить изменения сеанса", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
    if (r == IDNO) { OpenPath(DataDir() + "Logs"); return; }
    if (r != IDYES) return;
    if (MessageBoxW(g_wnd, L"Выполнить откат всех перечисленных изменений?\n\nПеред откатом закройте окна реестра и других инструментов. После отката рекомендуется перезагрузка.", L"Подтверждение отката", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    Logger::I().log(Lvl::Info, previous ? "Откат изменений прошлого сеанса…" : "Откат изменений сеанса…");
    UndoReport rep = UndoRestore(v);
    Logger::I().log(rep.failed ? Lvl::Warn : Lvl::Ok, fmt("Откат завершён: успешно %d, с ошибками %d, пропущено %d", rep.ok, rep.failed, rep.skipped));
    std::string tail = rep.text;
    if (tail.size() > 1600) tail = tail.substr(0, 1600) + "…\n";
    std::wstring done = W(fmt("Откат завершён.\nУспешно: %d, с ошибками: %d, пропущено: %d.\n\n%s\nПодробности — в журнале. После отката рекомендуется перезагрузка.", rep.ok, rep.failed, rep.skipped, tail.c_str()));
    MessageBoxW(g_wnd, done.c_str(), L"Отменить изменения сеанса", rep.failed ? MB_OK | MB_ICONWARNING : MB_OK | MB_ICONINFORMATION);
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
        else if (!strcmp(a.name, "cmd")) OpenAdminCmd();
        else if (!strcmp(a.name, "bsodref")) OpenBsodReference();
        else if (!strcmp(a.name, "triage")) OpenTriage();
        else if (!strcmp(a.name, "playbooks")) OpenPlaybooks();
        else if (!strcmp(a.name, "registry")) OpenRegistryEditor();
        else if (!strcmp(a.name, "bcd")) OpenBcdManager();
        else if (!strcmp(a.name, "autoruns")) OpenAutoruns();
        else if (!strcmp(a.name, "apps")) OpenInstalledApps();
        else if (!strcmp(a.name, "explorer")) OpenOfflineExplorer();
        else if (!strcmp(a.name, "fixes")) OpenFixWizard();
        else if (!strcmp(a.name, "password")) OpenPasswordTool();
        else if (!strcmp(a.name, "partition")) OpenPartitionTool();
        else if (!strcmp(a.name, "rstrui")) { Target t = CurrentTarget(); QuickCmd(t.valid && t.online ? 56 : 57); }
        else if (!strcmp(a.name, "restore-info")) QuickCmd(59);
        else if (!strcmp(a.name, "winre")) OpenWinReManager();
        else if (!strcmp(a.name, "winpeusb")) OpenWinPeBuilder();
        else if (!strcmp(a.name, "savedata")) OpenSaveData();
        else if (!strcmp(a.name, "backups")) ManageBackups();
        else if (!strcmp(a.name, "undo")) OpenUndoSession();
        else if (!strcmp(a.name, "secureboot")) QuickCmd(80);
        else if (!strcmp(a.name, "storagefix")) OpenStorageAccess();
        else if (!strcmp(a.name, "winre-menu")) {
            const Env& e = GetEnv();
            if (e.winreShell) {
                if (MessageBoxW(g_wnd, L"Закрыть WinRepair и открыть штатное меню WinRE (Выбор действия)?\n\nПосле выхода из меню можно вернуться через «Командная строка» → WinRepair.exe.", L"WinRE", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    // Если RecEnv.exe был подменён на WinRepair, штатное меню сохранено как RecEnvOrig.exe — запускаем его явно.
                    for (const std::string& orig : {e.sysDrive + "\\sources\\recovery\\RecEnvOrig.exe", e.winDir + "\\System32\\Recovery\\RecEnvOrig.exe"})
                        if (PathExists(orig)) { ShellExecuteW(nullptr, L"open", W(orig).c_str(), nullptr, nullptr, SW_SHOWNORMAL); break; }
                    DestroyWindow(g_wnd);
                }
            } else if (e.isWinRE) {
                std::string rec = PathExists(e.sysDrive + "\\sources\\recovery\\RecEnv.exe") ? e.sysDrive + "\\sources\\recovery\\RecEnv.exe" : e.winDir + "\\System32\\Recovery\\RecEnv.exe";
                ShellExecuteW(g_wnd, L"open", W(rec).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else MessageBoxW(g_wnd, L"Эта функция работает только внутри WinRE. Откройте «WinRE: вход в среду восстановления…», чтобы загрузиться в WinRE.", L"WinRE", MB_OK | MB_ICONINFORMATION);
        }
        else ShowAbout();
        return;
    }
    if (a.confirm && MessageBoxW(g_wnd, a.confirm, L"Подтверждение изменяющего действия", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    if (*DescOf(a.name)) Logger::I().log(Lvl::Info, "Назначение: " + U(std::wstring(DescOf(a.name))));
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
    std::string t = fmt("%s %s\nРазработчик: %s\n%s\n\nНативное приложение Win32 (C++17), без зависимостей от .NET, PowerShell и VC++ Runtime.\n\n"
                        "Среда: %s, %s%s\nАдминистратор: %s\nЖурналы и отчёты: %s",
                        kAppName, kAppVersion, WR_COMPANY, WR_COPYRIGHT, e.osName.c_str(), e.arch.c_str(), e.isWinPE ? " (WinPE)" : "", e.isAdmin ? "да" : "нет", DataDir().c_str());
    MessageBoxW(g_wnd, W(t).c_str(), L"О программе WinRepair", MB_OK | MB_ICONINFORMATION);
}

void CopyLog() {   // whole log to the clipboard (handy for pasting into a ticket or a message)
    std::wstring w = GetText(g_log), n;
    for (size_t i = 0; i < w.size(); ++i) {
        n += w[i];
        if (w[i] == L'\r' && (i + 1 >= w.size() || w[i + 1] != L'\n')) n += L'\n';
    }
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (n.size() + 1) * sizeof(wchar_t));
    if (g) {
        memcpy(GlobalLock(g), n.c_str(), (n.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
    SetWindowTextW(g_status, L"Журнал скопирован в буфер обмена.");
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
    bool push = !wcscmp(cls, L"BUTTON") && (style & 0xF) <= BS_DEFPUSHBUTTON;   // command buttons of tool windows are bold
    SendMessageW(h, WM_SETFONT, (WPARAM)(push ? theme::Semi() : g_font), TRUE);
    return h;
}

// Цвет и метка команды каталога по действию: синий — только чтение, бирюзовый — открывает окно Windows,
// оранжевый — меняет систему, красный — опасно, серый — только инструкция (ничего не запускается).
COLORREF CatToneColor(const CmdItem& it, const wchar_t** label) {
    const wchar_t* l;
    COLORREF c;
    if (it.kind == CKind::Manual) { l = L"инструкция"; c = RGB(110, 118, 130); }
    else if (it.risk == Risk::Danger) { l = L"опасно"; c = RGB(200, 32, 32); }
    else if (it.risk == Risk::Changing) { l = L"меняет систему"; c = RGB(214, 110, 0); }
    else if (it.kind == CKind::Launch) { l = L"окно Windows"; c = RGB(0, 137, 123); }
    else { l = L"только чтение"; c = RGB(26, 103, 210); }
    if (label) *label = l;
    return c;
}

void CatDrawItem(const DRAWITEMSTRUCT* d) {
    if (d->itemID == (UINT)-1) return;
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    bool sel = (d->itemState & ODS_SELECTED) != 0;
    const auto& all = Catalog();
    int idx = (d->itemID < g_catMap.size()) ? g_catMap[d->itemID] : -1;
    const wchar_t* lbl = L"";
    COLORREF acc = idx >= 0 ? CatToneColor(all[(size_t)idx], &lbl) : RGB(120, 120, 120);
    HBRUSH bg = CreateSolidBrush(sel ? theme::Mix(RGB(255, 255, 255), acc, 22) : (d->itemID % 2 ? RGB(250, 251, 253) : RGB(255, 255, 255)));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    RECT st{r.left, r.top + S(1), r.left + S(6), r.bottom - S(1)};
    HBRUSH sb = CreateSolidBrush(acc);
    FillRect(dc, &st, sb);
    // метка типа справа — цветная «плашка»
    std::wstring tag = TrW(lbl);
    HGDIOBJ of = SelectObject(dc, theme::Small());
    SIZE ts{};
    GetTextExtentPoint32W(dc, tag.c_str(), (int)tag.size(), &ts);
    RECT pill{r.right - ts.cx - S(18), r.top + S(4), r.right - S(6), r.bottom - S(4)};
    HBRUSH pb = CreateSolidBrush(theme::Mix(RGB(255, 255, 255), acc, sel ? 30 : 14));
    HPEN pp = CreatePen(PS_SOLID, 1, acc);
    HGDIOBJ ob = SelectObject(dc, pb), op = SelectObject(dc, pp);
    RoundRect(dc, pill.left, pill.top, pill.right, pill.bottom, S(10), S(10));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pb);
    DeleteObject(pp);
    DeleteObject(sb);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, acc);
    DrawTextW(dc, tag.c_str(), -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    // текст команды
    SelectObject(dc, sel ? theme::Semi() : g_font);
    SetTextColor(dc, theme::Text());
    wchar_t buf[600] = L"";
    SendMessageW(d->hwndItem, LB_GETTEXT, d->itemID, (LPARAM)buf);
    RECT tr{r.left + S(14), r.top, pill.left - S(8), r.bottom};
    DrawTextW(dc, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
    if (d->itemState & ODS_FOCUS) DrawFocusRect(dc, &r);
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

void CatContextMenu(HWND h, HWND src, LPARAM lp) {
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    if (lp == (LPARAM)-1) { RECT r; GetWindowRect(src, &r); pt = {r.left + S(20), r.top + S(20)}; }
    else if (src == g_catList) {   // right-click selects the item under the pointer
        POINT c = pt;
        ScreenToClient(src, &c);
        DWORD r = (DWORD)SendMessageW(src, LB_ITEMFROMPOINT, 0, MAKELPARAM(c.x, c.y));
        if (HIWORD(r) == 0 && LOWORD(r) != 0xFFFF) { SendMessageW(src, LB_SETCURSEL, LOWORD(r), 0); CatUpdate(); }
    }
    const CmdItem* it = CatSelected();
    enum { K_RUN = 1, K_NAME, K_CMD, K_CLOSE };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (it && !g_running ? 0 : MF_GRAYED), K_RUN, TrW(L"Выполнить выбранную команду").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (it ? 0 : MF_GRAYED), K_NAME, TrW(L"Копировать название").c_str());
    AppendMenuW(m, MF_STRING | (it ? 0 : MF_GRAYED), K_CMD, TrW(L"Копировать текст команды").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, K_CLOSE, TrW(L"Закрыть").c_str());
    SetForegroundWindow(h);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
    DestroyMenu(m);
    if (c == K_RUN) CatRun();
    else if (c == K_NAME && it) PutClipboard(W(fmt("%d  %s", it->no, it->title)));
    else if (c == K_CMD && it) PutClipboard(W(CmdBuild(*it, ReadParams(), CurrentTarget(), "<папка отчёта>\\scratch", "<временный файл>")));
    else if (c == K_CLOSE) DestroyWindow(h);
}

LRESULT CALLBACK CatProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CONTEXTMENU: CatContextMenu(h, (HWND)wp, lp); return 0;
        case WM_SIZE: if (g_catClose) CatLayout(h); return 0;
        case WM_MEASUREITEM: { auto* mi = (MEASUREITEMSTRUCT*)lp; if (mi->CtlID == IDC_CAT_LIST) { mi->itemHeight = S(28); return TRUE; } break; }
        case WM_DRAWITEM: {
            auto* d = (DRAWITEMSTRUCT*)lp;
            if (d->CtlID == IDC_CAT_LIST) { CatDrawItem(d); return TRUE; }
            if (d->CtlType == ODT_BUTTON) {
                theme::Cat c = d->CtlID == IDC_CAT_RUN ? theme::Cat::Repair : d->CtlID == IDC_CAT_FIND ? theme::Cat::Diag : theme::Cat::Info;
                theme::DrawButton(d, c, theme::IsHover(d->hwndItem));
                return TRUE;
            }
            break;
        }
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
    g_catList = MkC(h, L"LISTBOX", L"", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, IDC_CAT_LIST, WS_EX_CLIENTEDGE);
    g_catDesc = MkC(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, IDC_CAT_DESC, WS_EX_CLIENTEDGE);
    static const wchar_t* lbl[6] = {L"Том (например C:)", L"Источник (/Source: папка или wim:файл:индекс)", L"Узел / имя (host, IP)", L"Путь Windows (например C:\\Windows)", L"EFI-раздел (буква, например S:)", L"BCD-хранилище (файл; пусто = хранилище текущей среды)"};
    Target t = CurrentTarget();
    std::string def[6] = {t.valid ? std::string(1, t.letter()) + ":" : "C:", trim(U(GetText(g_source))), trim(U(GetText(g_host))), t.valid ? t.winDir : "C:\\Windows", "", ""};
    for (int i = 0; i < 6; ++i) {
        g_catLb[i] = MkC(h, L"STATIC", lbl[i], SS_LEFT, IDC_CAT_L0 + i);
        if (i == 5) g_catEd[i] = MkC(h, L"COMBOBOX", W(def[i]).c_str(), CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, IDC_CAT_E0 + i);
        else g_catEd[i] = MkC(h, L"EDIT", W(def[i]).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, IDC_CAT_E0 + i, WS_EX_CLIENTEDGE);
    }
    g_catFind = MkC(h, L"BUTTON", L"Найти BCD", BS_OWNERDRAW | WS_TABSTOP, IDC_CAT_FIND);
    g_catRun = MkC(h, L"BUTTON", L"Выполнить…", BS_OWNERDRAW | WS_TABSTOP, IDC_CAT_RUN);
    g_catClose = MkC(h, L"BUTTON", L"Закрыть", BS_OWNERDRAW | WS_TABSTOP, IDC_CAT_CLOSE);
    for (HWND b : {g_catFind, g_catRun, g_catClose}) theme::AttachHover(b);
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
    g_helpBtn = Mk(L"BUTTON", L"Справка", BS_OWNERDRAW | WS_TABSTOP, IDC_HELPBTN);
    g_lbl[0] = Mk(L"STATIC", L"Целевая ОС:", SS_LEFT, IDC_L1);
    g_target = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_TARGET);
    g_refreshBtn = Mk(L"BUTTON", L"Обновить", BS_OWNERDRAW | WS_TABSTOP, IDC_REFRESH);
    g_lbl[1] = Mk(L"STATIC", L"Период, дней:", SS_LEFT, IDC_L2);
    g_hint = Mk(L"STATIC", L"", SS_LEFT, IDC_L6);
    g_stage = Mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_DESC);
    g_undoBtn = Mk(L"BUTTON", L"Отменить изменения сеанса…", BS_OWNERDRAW | WS_TABSTOP, IDC_UNDO);
    g_aboutBtn = Mk(L"BUTTON", L"О программе WinRepair", BS_OWNERDRAW | WS_TABSTOP, IDC_ABOUT);
    g_days = Mk(L"EDIT", L"3", ES_NUMBER | WS_TABSTOP, IDC_DAYS, WS_EX_CLIENTEDGE);
    g_ext = Mk(L"BUTTON", L"Расширенный режим (ScanHealth, chkdsk /scan)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_EXT);
    g_force = Mk(L"BUTTON", L"Точка восстановления: игнорировать лимит 24 ч", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_FORCE);
    g_lbl[2] = Mk(L"STATIC", L"Хост для проверки:", SS_LEFT, IDC_L3);
    g_host = Mk(L"EDIT", L"www.microsoft.com", ES_AUTOHSCROLL | WS_TABSTOP, IDC_HOST, WS_EX_CLIENTEDGE);
    g_lbl[3] = Mk(L"STATIC", L"Порт:", SS_LEFT, IDC_L4);
    g_port = Mk(L"EDIT", L"443", ES_NUMBER | WS_TABSTOP, IDC_PORT, WS_EX_CLIENTEDGE);
    g_lbl[4] = Mk(L"STATIC", L"Источник DISM (/Source):", SS_LEFT, IDC_L5);
    g_source = Mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_SOURCE, WS_EX_CLIENTEDGE);
    g_tab = Mk(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS | TCS_OWNERDRAWFIXED | TCS_FIXEDWIDTH | TCS_MULTILINE, IDC_TAB);
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
    Mk(L"BUTTON", L"Копировать журнал", BS_OWNERDRAW | WS_TABSTOP, IDC_COPYLOG);
    Mk(L"BUTTON", L"Очистить", BS_OWNERDRAW | WS_TABSTOP, IDC_CLEAR);
    g_prog = Mk(PROGRESS_CLASSW, L"", PBS_MARQUEE, IDC_PROG);
    g_status = Mk(L"STATIC", L"Готово", SS_LEFT, IDC_STATUS);
    SendMessageW(g_tab, WM_SETFONT, (WPARAM)theme::Semi(), TRUE);
    SendMessageW(g_target, WM_SETFONT, (WPARAM)theme::Big(), TRUE);
    SendMessageW(g_target, CB_SETITEMHEIGHT, (WPARAM)-1, S(22));
    SendMessageW(g_lbl[0], WM_SETFONT, (WPARAM)theme::Big(), TRUE);
    SendMessageW(g_hint, WM_SETFONT, (WPARAM)theme::Body(), TRUE);
    for (HWND o : {g_lbl[1], g_lbl[2], g_lbl[3], g_lbl[4], g_ext, g_force}) SendMessageW(o, WM_SETFONT, (WPARAM)theme::Big(), TRUE);   // option captions: big and bold
    for (HWND o : {g_days, g_host, g_port, g_source}) SendMessageW(o, WM_SETFONT, (WPARAM)theme::Semi(), TRUE);
    SendMessageW(g_stage, WM_SETFONT, (WPARAM)theme::Semi(), TRUE);
    EnableWindow(g_cancelBtn, FALSE);
    ShowWindow(g_prog, SW_HIDE);
    for (HWND hb : {g_langBtn, g_exitBtn, g_helpBtn, g_refreshBtn, g_cancelBtn, GetDlgItem(g_wnd, IDC_OPENREP), GetDlgItem(g_wnd, IDC_OPENLOG), GetDlgItem(g_wnd, IDC_SAVELOG), GetDlgItem(g_wnd, IDC_COPYLOG), GetDlgItem(g_wnd, IDC_CLEAR), g_undoBtn, g_aboutBtn}) theme::AttachHover(hb);
    for (int i = 0; i < kActionCount; ++i) theme::AttachHover(g_btn[i]);
    FillTargets();
    ShowTabButtons();
    SetWindowSubclass(g_log, LogSubProc, 7, 0);
    CreateTooltips();
}

// ---- right-click menus -----------------------------------------------------------------------------------
std::wstring Tm(const wchar_t* ru) { return TrW(ru); }

std::wstring StripAmp(std::wstring t) {
    t.erase(std::remove(t.begin(), t.end(), L'&'), t.end());
    return t;
}

void PutClipboard(const std::wstring& txt) {
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (txt.size() + 1) * sizeof(wchar_t));
    if (g) {
        memcpy(GlobalLock(g), txt.c_str(), (txt.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

POINT MenuPoint(HWND src, LPARAM lp) {
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    if (lp == (LPARAM)-1) {   // keyboard (Shift+F10 / menu key): below the control
        RECT r;
        GetWindowRect(src, &r);
        pt = {r.left + S(12), r.top + S(12)};
    }
    return pt;
}

enum { CM_RUN = 1, CM_ABOUT, CM_COPYNAME, CM_COPYDESC, CM_COPYSEL, CM_SELALL, CM_COPYALL, CM_SAVE, CM_OPENLOG, CM_CLEAR, CM_OPENREP,
       CM_REFRESH, CM_LANG, CM_EXIT, CM_TAB0 = 100 };

// Menu for a command button: run it, read what it does, copy its name or description.
void CommandMenu(int idx, POINT pt) {
    const Action& a = kActions[idx];
    std::wstring name = W(U(std::wstring(a.text)));
    while (!name.empty() && (name.back() == L'\u2026' || name.back() == L'.')) name.pop_back();
    std::wstring desc = W(U(std::wstring(DescOf(a.name))));
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_running ? MF_GRAYED : 0), CM_RUN, (Tm(L"Запустить: ") + name).c_str());
    AppendMenuW(m, MF_STRING | (desc.empty() ? MF_GRAYED : 0), CM_ABOUT, Tm(L"Что делает эта команда…").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CM_COPYNAME, Tm(L"Копировать название").c_str());
    AppendMenuW(m, MF_STRING | (desc.empty() ? MF_GRAYED : 0), CM_COPYDESC, Tm(L"Копировать описание").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CM_OPENREP, Tm(L"Открыть папку отчётов").c_str());
    SetForegroundWindow(g_wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    switch (c) {
        case CM_RUN: if (!g_running) StartAction(idx); break;
        case CM_ABOUT: MessageBoxW(g_wnd, desc.c_str(), name.c_str(), MB_OK | MB_ICONINFORMATION); break;
        case CM_COPYNAME: PutClipboard(name); break;
        case CM_COPYDESC: PutClipboard(desc); break;
        case CM_OPENREP: SendMessageW(g_wnd, WM_COMMAND, MAKEWPARAM(IDC_OPENREP, 0), 0); break;
    }
}

// Menu for the log pane.
void LogMenu(POINT pt) {
    DWORD s0 = 0, s1 = 0;
    SendMessageW(g_log, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
    bool hasSel = s1 > s0;
    bool empty = GetWindowTextLengthW(g_log) == 0;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (hasSel ? 0 : MF_GRAYED), CM_COPYSEL, Tm(L"Копировать выделенное").c_str());
    AppendMenuW(m, MF_STRING | (empty ? MF_GRAYED : 0), CM_SELALL, Tm(L"Выделить всё").c_str());
    AppendMenuW(m, MF_STRING | (empty ? MF_GRAYED : 0), CM_COPYALL, Tm(L"Копировать весь журнал").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (empty ? MF_GRAYED : 0), CM_SAVE, Tm(L"Сохранить журнал…").c_str());
    AppendMenuW(m, MF_STRING, CM_OPENLOG, Tm(L"Открыть файл журнала").c_str());
    AppendMenuW(m, MF_STRING, CM_OPENREP, Tm(L"Открыть папку отчётов").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (empty ? MF_GRAYED : 0), CM_CLEAR, Tm(L"Очистить").c_str());
    SetForegroundWindow(g_wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    switch (c) {
        case CM_COPYSEL: SendMessageW(g_log, WM_COPY, 0, 0); break;
        case CM_SELALL: SendMessageW(g_log, EM_SETSEL, 0, -1); break;
        case CM_COPYALL: CopyLog(); break;
        case CM_SAVE: SaveLogAs(); break;
        case CM_OPENLOG: SendMessageW(g_wnd, WM_COMMAND, MAKEWPARAM(IDC_OPENLOG, 0), 0); break;
        case CM_OPENREP: SendMessageW(g_wnd, WM_COMMAND, MAKEWPARAM(IDC_OPENREP, 0), 0); break;
        case CM_CLEAR: SetWindowTextW(g_log, L""); break;
    }
}

// Tab strip menu: jump to a tab.
void GeneralMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    int cur = TabCtrl_GetCurSel(g_tab);
    for (int i = 0; i < (int)(sizeof kTabs / sizeof kTabs[0]); ++i)
        AppendMenuW(m, MF_STRING | (i == cur ? MF_CHECKED : 0), CM_TAB0 + i, TrW(kTabs[i]).c_str());
    SetForegroundWindow(g_wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    if (c >= CM_TAB0 && c < CM_TAB0 + 16) { TabCtrl_SetCurSel(g_tab, c - CM_TAB0); ShowTabButtons(); }
}

void OnContextMenu(HWND src, LPARAM lp) {
    POINT pt = MenuPoint(src, lp);
    if (src == g_log) { LogMenu(pt); return; }
    int id = GetDlgCtrlID(src);
    if (id >= IDC_ACTION0 && id < IDC_ACTION0 + kActionCount) { CommandMenu(id - IDC_ACTION0, pt); return; }
    if (src == g_tab) GeneralMenu(pt);   // tab strip: go to a tab; other places have no menu of their own
}

// the log (RichEdit) has its own built-in menu; route it to ours so Save / Clear / Open are there too
LRESULT CALLBACK LogSubProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_CONTEXTMENU) { OnContextMenu(h, lp); return 0; }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, LogSubProc, 7);
    return DefSubclassProc(h, m, wp, lp);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE: g_wnd = h; return 0;
        case WM_CONTEXTMENU: OnContextMenu((HWND)wp, lp); return 0;
        case WM_SIZE: if (g_tab) Layout(); InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            RECT hr = rc;
            hr.bottom = S(72);
            theme::PaintHeader(dc, hr, g_icon, W(kAppName), g_hdrSub, g_chips, S(8) + S(104) + S(8) + S(96) + S(8) + S(110) + S(14));
            theme::PaintLegend(dc, S(10), hr.bottom + S(7), rc.right - S(20));
            theme::PaintTargetPanel(dc, g_targetFrame);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* di = (DRAWITEMSTRUCT*)lp;
            if (di->CtlType == ODT_TAB) {
                wchar_t buf[128] = {0};
                TCITEMW ti{};
                ti.mask = TCIF_TEXT;
                ti.pszText = buf;
                ti.cchTextMax = 127;
                TabCtrl_GetItem(g_tab, di->itemID, &ti);
                theme::DrawTab(di, buf, TabCat((int)di->itemID), (di->itemState & ODS_SELECTED) != 0);
                return TRUE;
            }
            if (di->CtlType != ODT_BUTTON) break;
            int id = (int)di->CtlID;
            theme::Cat c = theme::Cat::Info;
            if (id >= IDC_ACTION0 && id < IDC_ACTION0 + kActionCount) c = CatOfAction(kActions[id - IDC_ACTION0].name);
            else if (id == IDC_EXIT) c = theme::Cat::Exit;
            else if (id == IDC_LANG) c = theme::Cat::Lang;
            else if (id == IDC_HELPBTN) c = theme::Cat::Help;
            else if (id == IDC_CANCEL) c = theme::Cat::Repair;
            else if (id == IDC_REFRESH) c = theme::Cat::Diag;
            else if (id == IDC_UNDO) c = CatOfAction("undo");
            else if (id == IDC_ABOUT) c = CatOfAction("about");
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
            if ((HWND)lp == g_stage || (HWND)lp == g_hint || (HWND)lp == g_lbl[1] || (HWND)lp == g_lbl[2] || (HWND)lp == g_lbl[3] || (HWND)lp == g_lbl[4] || (HWND)lp == g_ext || (HWND)lp == g_force) {   // they sit on the grey tab page, not on the white window background
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                SetTextColor(dc, (HWND)lp == g_stage ? theme::Mix(theme::Accent(TabCat(TabCtrl_GetCurSel(g_tab))), RGB(0, 0, 0), 30) : ((HWND)lp == g_hint ? RGB(52, 64, 88) : RGB(20, 30, 50)));
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            SetTextColor(dc, (HWND)lp == g_hint ? RGB(88, 100, 122) : theme::Text());
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
            if (nh->code == TTN_GETDISPINFOW && nh->hwndFrom == g_tip) { TooltipNeedText((NMTTDISPINFOW*)lp); return 0; }
            if (nh->hwndFrom == g_tab && nh->code == TCN_SELCHANGE) ShowTabButtons();
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id >= IDC_ACTION0 && id < IDC_ACTION0 + kActionCount) { if (!g_running) StartAction(id - IDC_ACTION0); }
            else if (id == IDC_UNDO) { if (!g_running) OpenUndoSession(); }
            else if (id == IDC_ABOUT) { if (!g_running) ShowAbout(); }
            else if (id == IDC_CANCEL) { g_cancel = true; Logger::I().log(Lvl::Warn, "Запрошена отмена…"); }
            else if (id == IDC_REFRESH) { FillTargets(); Logger::I().log(Lvl::Info, fmt("Найдено целевых ОС: %zu", g_targets.size())); }
            else if (id == IDC_CLEAR) SetWindowTextW(g_log, L"");
            else if (id == IDC_LANG) ChangeLanguage();
            else if (id == IDC_HELPBTN) ShowHelp("");
            else if (id == IDC_EXIT) SendMessageW(h, WM_CLOSE, 0, 0);
            else if (id == IDC_OPENLOG) OpenPath(Logger::I().path());
            else if (id == IDC_OPENREP) OpenPath(g_lastOut.empty() ? DataDir() + "Reports" : g_lastOut);
            else if (id == IDC_SAVELOG) SaveLogAs();
            else if (id == IDC_COPYLOG) CopyLog();
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
            else if (*n == "apps") wr::OpenInstalledApps();
            else if (*n == "explorer") wr::OpenOfflineExplorer();
            else if (*n == "registry") wr::OpenRegistryEditor();
            else if (*n == "winre") wr::OpenWinReManager();
            else if (*n == "fixes") wr::OpenFixWizard();
            else if (*n == "password") wr::OpenPasswordTool();
            else if (*n == "partition") wr::OpenPartitionTool();
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
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F1) { ShowHelp(HelpTopicForWindow(msg.hwnd)); continue; }   // F1 — справка (в окне инструмента — его раздел)
        if (g_cat && IsDialogMessageW(g_cat, &msg)) continue;
        if (!IsDialogMessageW(w, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return (int)msg.wParam;
}
