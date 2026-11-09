#include "catalog.h"
#include "winre.h"

#include <cctype>
#include <shellapi.h>

namespace wr {

namespace {

const char* kManual51 =
    "Driver Verifier — ТОЛЬКО РУЧНАЯ ПРОЦЕДУРА (программа ничего не запускает).\r\n"
    "1. Создайте точку восстановления и резервную копию данных. Убедитесь, что есть доступ к безопасному режиму/WinRE.\r\n"
    "2. Включите дампы: Параметры системы → Загрузка и восстановление → «Малый дамп памяти».\r\n"
    "3. Запустите verifier.exe от администратора → «Создать нестандартные параметры» → «Выбрать отдельные параметры» (Special pool, Force IRQL checking,\r\n"
    "   Pool tracking, Deadlock detection) → «Выбрать имена драйверов из списка» → отметьте ТОЛЬКО подозреваемые сторонние драйверы.\r\n"
    "   Никогда не включайте проверку для всех драйверов на рабочей машине.\r\n"
    "4. Перезагрузитесь и работайте как обычно до сбоя. Типичный код: DRIVER_VERIFIER_DETECTED_VIOLATION (0xC4) / 0xC9.\r\n"
    "5. Разберите дамп кнопкой «Разбор дампов» (модуль в стеке — виновник).\r\n"
    "6. ОБЯЗАТЕЛЬНО отключите: verifier /reset, затем перезагрузка. Если система не грузится — безопасный режим → verifier /reset.\r\n";
const char* kManual52 =
    "Удаление обновления — ТОЛЬКО РУЧНАЯ ПРОЦЕДУРА.\r\n"
    "1. Сначала точка восстановления и резервная копия.\r\n"
    "2. Работающая ОС: Параметры → Центр обновления Windows → Журнал обновлений → «Удалить обновления» (или wusa /uninstall /kb:XXXXXXX).\r\n"
    "3. ОС не загружается: WinRE → Дополнительные параметры → «Удалить обновления» (качественное/компонентное).\r\n"
    "4. Из WinPE (офлайн): DISM /Image:<том>:\\ /Get-Packages — найдите пакет; DISM /Image:<том>:\\ /Remove-Package /PackageName:<имя>.\r\n"
    "   Пакеты servicing stack (SSU) удалять нельзя; удаляйте только LCU/конкретный KB.\r\n"
    "5. После удаления приостановите установку обновления (Параметры → Приостановить) и проверьте целостность.\r\n";
const char* kManual53 =
    "Recovery — РУЧНОЙ ПОРЯДОК (от менее разрушительного к более разрушительному):\r\n"
    "1. Резервное копирование данных (из WinPE скопировать профили на внешний носитель).\r\n"
    "2. System Restore (rstrui.exe / WinRE → Восстановление системы).\r\n"
    "3. Восстановление запуска (Startup Repair), удаление проблемного обновления/драйвера.\r\n"
    "4. DISM /RestoreHealth + sfc /scannow, проверка диска и BCD.\r\n"
    "5. «Вернуть компьютер в исходное состояние» (Reset this PC) с сохранением файлов.\r\n"
    "6. Восстановление установкой поверх (Repair Install): setup.exe с ISO той же редакции/языка, «Сохранить файлы и приложения».\r\n"
    "7. Чистая установка (Clean Install) — последний вариант: диск будет очищен, лицензии/ПО/драйверы подготовьте заранее.\r\n"
    "Программа ни одного из этих действий автоматически не выполняет.\r\n";
const char* kManual54 =
    "icacls /save — РУЧНОЕ сохранение ACL (программа ничего не выполняет).\r\n"
    "Сохранение (от администратора, в консоли, файл — на ДРУГОЙ диск):\r\n"
    "    icacls \"C:\\Users\" /save \"D:\\acl_Users.txt\" /t /c\r\n"
    "Восстановление (укажите РОДИТЕЛЬСКУЮ папку):\r\n"
    "    icacls \"C:\\\" /restore \"D:\\acl_Users.txt\"\r\n"
    "Сохраняйте ACL перед любым takeown/icacls /grant /reset на системных и пользовательских папках.\r\n"
    "Параметры: /t — рекурсивно, /c — продолжать при ошибках.\r\n";

std::vector<CmdItem> Build() {
    using K = CKind; using R = Risk;
    const char* DISM = "{DISM}";
    (void)DISM;
    return {
        {1, "Первичная диагностика", "winver", K::Launch, "winver.exe", 0, R::Safe, 0, nullptr, nullptr, "Окно «О Windows».", nullptr},
        {2, "Первичная диагностика", "systeminfo", K::Exec, "systeminfo.exe", 0, R::Safe, 180, nullptr, nullptr, "Сводка об ОС и оборудовании (может занять до минуты).", nullptr},
        {3, "Первичная диагностика", "msinfo32", K::Launch, "msinfo32.exe", 0, R::Safe, 0, nullptr, nullptr, "Сведения о системе (GUI).", nullptr},
        {4, "Первичная диагностика", "dxdiag", K::Launch, "dxdiag.exe", 0, R::Safe, 0, nullptr, nullptr, "Диагностика DirectX (GUI).", nullptr},
        {5, "Инструменты Windows", "taskmgr", K::Launch, "taskmgr.exe", 0, R::Safe, 0, nullptr, nullptr, "Диспетчер задач (GUI).", nullptr},
        {6, "Инструменты Windows", "perfmon /rel", K::Launch, "perfmon.exe|/rel", 0, R::Safe, 0, nullptr, nullptr, "Монитор стабильности системы.", nullptr},
        {7, "Инструменты Windows", "eventvwr.msc", K::Launch, "eventvwr.msc", 0, R::Safe, 0, nullptr, nullptr, "Просмотр событий (GUI).", nullptr},
        {8, "Инструменты Windows", "resmon", K::Launch, "resmon.exe", 0, R::Safe, 0, nullptr, nullptr, "Монитор ресурсов (GUI).", nullptr},
        {9, "Инструменты Windows", "perfmon", K::Launch, "perfmon.exe", 0, R::Safe, 0, nullptr, nullptr, "Системный монитор (GUI).", nullptr},
        {10, "DISM/SFC", "DISM /Online /Cleanup-Image /CheckHealth", K::Exec, "{DISM} /Cleanup-Image /CheckHealth", 0, R::Safe, 0, nullptr, nullptr, "Быстрая проверка флага повреждения хранилища. Для офлайн-цели автоматически /Image:<том>.", nullptr},
        {11, "DISM/SFC", "DISM /Online /Cleanup-Image /ScanHealth", K::Exec, "{DISM} /Cleanup-Image /ScanHealth", 0, R::Safe, 0, nullptr, nullptr, "Полное сканирование хранилища компонентов (долго), без изменений.", nullptr},
        {12, "DISM/SFC", "DISM /Online /Cleanup-Image /RestoreHealth", K::Exec, "{DISM} /Cleanup-Image /RestoreHealth", 0, R::Changing, 0, nullptr, nullptr, "ИЗМЕНЯЕТ систему: восстанавливает компоненты (нужен доступ к WU или /Source).", nullptr},
        {13, "DISM/SFC", "sfc /scannow", K::Exec, "{SFC} /scannow", 0, R::Changing, 0, nullptr, nullptr, "ИЗМЕНЯЕТ систему: проверяет и заменяет повреждённые системные файлы.", nullptr},
        {14, "DISM/SFC", "DISM /Image:<том> /Cleanup-Image /RestoreHealth /Source:<источник>", K::Exec,
         "dism.exe /Image:{VOLR} /Cleanup-Image /RestoreHealth /Source:{SRC} /ScratchDir:{SCR}", P_VOL | P_SRC, R::Changing, 0, nullptr, nullptr,
         "ИЗМЕНЯЕТ офлайн-образ. Источник: путь к папке с файлами или wim:D:\\sources\\install.wim:1 (редакция и сборка должны совпадать).", nullptr},
        {15, "Журналы", "C:\\Windows\\Logs\\CBS\\CBS.log", K::ViewFile, "Logs\\CBS\\CBS.log", 0, R::Safe, 0, nullptr, nullptr, "Показывает хвост журнала CBS и строки с ошибками.", nullptr},
        {16, "Журналы", "C:\\Windows\\Logs\\DISM\\dism.log", K::ViewFile, "Logs\\DISM\\dism.log", 0, R::Safe, 0, nullptr, nullptr, "Показывает хвост журнала DISM и строки с ошибками.", nullptr},
        {17, "Загрузка/BCD", "bcdedit /store <BCD> /enum all /v", K::Exec, "bcdedit.exe {BCDSTORE} /enum all /v", P_BCD, R::Safe, 60, nullptr, nullptr, "Только чтение. Выберите хранилище BCD целевой системы (кнопка «Найти BCD»); без выбора — хранилище текущей среды (в WinPE это НЕ целевая ОС).", nullptr},
        {18, "Загрузка/BCD", "bcdboot <Windows> /s <EFI> /f UEFI", K::Exec, "bcdboot.exe {WIN} /s {EFI} /f UEFI", P_WIN | P_EFI, R::Danger, 120, nullptr, nullptr,
         "ОПАСНО: пересоздаёт файлы загрузки на EFI-разделе. EFI-раздел должен иметь букву (mountvol S: /S или diskpart assign).", nullptr},
        {19, "Загрузка/BCD", "bootrec /fixmbr", K::Exec, "bootrec.exe /fixmbr", 0, R::Danger, 60, nullptr, nullptr, "ОПАСНО: записывает основную загрузочную запись MBR. Только для MBR/BIOS. Доступно в WinRE/WinPE.", nullptr},
        {20, "Загрузка/BCD", "bootrec /fixboot", K::Exec, "bootrec.exe /fixboot", 0, R::Danger, 60, nullptr, nullptr, "ОПАСНО: записывает загрузочный сектор системного раздела. Доступно в WinRE/WinPE.", nullptr},
        {21, "Загрузка/BCD", "bootrec /scanos", K::Exec, "bootrec.exe /scanos", 0, R::Safe, 300, nullptr, nullptr, "Только поиск установленных Windows. Доступно в WinRE/WinPE.", nullptr},
        {22, "Загрузка/BCD", "bootrec /rebuildbcd", K::Exec, "bootrec.exe /rebuildbcd", 0, R::Danger, 300, "Y\r\nY\r\nY\r\nY\r\n", nullptr,
         "ОПАСНО: перестраивает BCD; на запрос «добавить в список загрузки» программа отвечает Y. Доступно в WinRE/WinPE.", nullptr},
        {23, "Хранилище", "chkdsk <том>", K::Exec, "chkdsk.exe {VOL}", P_VOL, R::Safe, 0, nullptr, nullptr, "Только чтение (без /f).", nullptr},
        {24, "Хранилище", "chkdsk <том> /f", K::Exec, "chkdsk.exe {VOL} /f", P_VOL, R::Changing, 0, "Y\r\n", nullptr,
         "ИЗМЕНЯЕТ файловую систему. На занятом томе ответ Y: проверка при перезагрузке/размонтирование тома.", nullptr},
        {25, "Хранилище", "chkdsk <том> /f /r", K::Exec, "chkdsk.exe {VOL} /f /r", P_VOL, R::Danger, 0, "Y\r\n", nullptr,
         "ОПАСНО/ДОЛГО: поиск сбойных секторов. На умирающем диске может ускорить отказ — сначала копия данных.", nullptr},
        {26, "Сеть", "ipconfig /all", K::Exec, "ipconfig.exe /all", 0, R::Safe, 60, nullptr, nullptr, "Конфигурация IP.", nullptr},
        {27, "Сеть", "nslookup <имя>", K::Exec, "nslookup.exe {HOST}", P_HOST, R::Safe, 60, nullptr, nullptr, "Разрешение имени через DNS.", nullptr},
        {28, "Сеть", "route print", K::Exec, "route.exe print", 0, R::Safe, 60, nullptr, nullptr, "Таблица маршрутизации.", nullptr},
        {29, "Сеть", "netstat -ano", K::Exec, "netstat.exe -ano", 0, R::Safe, 90, nullptr, nullptr, "Соединения и PID.", nullptr},
        {30, "Сеть", "tasklist", K::Exec, "tasklist.exe", 0, R::Safe, 60, nullptr, nullptr, "Список процессов.", nullptr},
        {31, "PowerShell-замена", "Get-Process", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Нативно: ToolHelp32 + GetProcessMemoryInfo.", NatGetProcess},
        {32, "PowerShell-замена", "Get-Process | Sort-Object CPU -Descending", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Нативно: сортировка по времени CPU.", NatGetProcessCpu},
        {33, "PowerShell-замена", "Get-Service", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Нативно: Service Control Manager.", NatGetService},
        {34, "PowerShell-замена", "Get-CimInstance Win32_OperatingSystem", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Нативно: реестр + Win32 API (без WMI).", NatOperatingSystem},
        {35, "PowerShell-замена", "Get-WinEvent -LogName System -MaxEvents 50", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Нативно: wevtapi (для офлайн-цели — файл System.evtx).", NatEventLogSystem},
        {36, "PowerShell-замена", "Get-NetAdapter", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Нативно: iphlpapi.", NatNetAdapter},
        {37, "PowerShell-замена", "Get-NetIPConfiguration", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Нативно: iphlpapi.", NatNetIPConfiguration},
        {38, "PowerShell-замена", "Get-Disk", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Нативно: IOCTL физических дисков.", NatGetDisk},
        {39, "PowerShell-замена", "Get-PhysicalDisk", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Нативно: IOCTL + SMART/NVMe.", NatPhysicalDisk},
        {40, "PowerShell-замена", "Get-Volume", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Нативно: Win32 API томов.", NatGetVolume},
        {41, "PowerShell-замена", "Get-PnpDevice", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Нативно: SetupAPI/CfgMgr32.", NatPnpDevice},
        {42, "PowerShell-замена", "Get-WindowsDriver -Online", K::Exec, "{DISM} /Get-Drivers /all", 0, R::Safe, 0, nullptr, nullptr, "Замена: DISM /Get-Drivers /all (для офлайн-цели /Image:).", nullptr},
        {43, "Питание", "powercfg /a", K::Exec, "powercfg.exe /a", 0, R::Safe, 60, nullptr, nullptr, "Доступные состояния сна.", nullptr},
        {44, "Питание", "powercfg /getactivescheme", K::Exec, "powercfg.exe /getactivescheme", 0, R::Safe, 30, nullptr, nullptr, "Активная схема питания.", nullptr},
        {45, "Система", "whoami", K::Exec, "whoami.exe", 0, R::Safe, 30, nullptr, nullptr, "Текущий пользователь.", nullptr},
        {46, "Система", "sc query", K::Exec, "sc.exe query", 0, R::Safe, 60, nullptr, nullptr, "Состояние служб.", nullptr},
        {47, "Драйверы", "driverquery", K::Exec, "driverquery.exe", 0, R::Safe, 120, nullptr, nullptr, "Список загруженных драйверов.", nullptr},
        {48, "Сеть", "ping <узел>", K::Exec, "ping.exe {HOST}", P_HOST, R::Safe, 90, nullptr, nullptr, "Проверка доступности узла.", nullptr},
        {49, "Сеть", "tracert <узел>", K::Exec, "tracert.exe {HOST}", P_HOST, R::Safe, 240, nullptr, nullptr, "Трассировка маршрута.", nullptr},
        {50, "Сеть", "pathping <узел>", K::Exec, "pathping.exe {HOST}", P_HOST, R::Safe, 900, nullptr, nullptr, "Трассировка + статистика потерь (до ~5–10 минут).", nullptr},
        {51, "Драйверы", "Driver Verifier — только ручная процедура", K::Manual, kManual51, 0, R::Safe, 0, nullptr, nullptr, "Показывает инструкцию. Ничего не запускается.", nullptr},
        {52, "Обновления", "Удаление обновления — только ручная процедура", K::Manual, kManual52, 0, R::Safe, 0, nullptr, nullptr, "Показывает инструкцию. Ничего не запускается.", nullptr},
        {53, "Recovery", "System Restore/Reset/Repair Install/Clean Install — ручной порядок", K::Manual, kManual53, 0, R::Safe, 0, nullptr, nullptr, "Показывает порядок действий. Ничего не запускается.", nullptr},
        {54, "Профили/ACL", "icacls /save — ручное сохранение ACL", K::Manual, kManual54, 0, R::Safe, 0, nullptr, nullptr, "Показывает инструкцию. Ничего не запускается.", nullptr},
        {55, "WinPE", "diskpart → list volume — сначала определить буквы", K::Exec, "diskpart.exe /s {SCRIPT}", 0, R::Safe, 60, nullptr, "list volume\r\nexit\r\n",
         "Только вывод списка томов (скрипт во временном файле). Используйте перед операциями с буквами томов.", nullptr},
        {56, "Recovery", "rstrui.exe — Восстановление системы из точек", K::Launch, "rstrui.exe", 0, R::Changing, 0, nullptr, nullptr, "GUI мастер отката из точек восстановления работающей Windows.", nullptr},
        {57, "Recovery", "rstrui.exe /offline:<Windows>=active", K::Launch, "rstrui.exe|/offline:{WINRAW}=active", P_WIN, R::Changing, 0, nullptr, nullptr, "Откат офлайн-системы из точек восстановления (работает в WinRE; в WinPE нужен rstrui и компоненты восстановления).", nullptr},
        {58, "Recovery", "vssadmin list shadows /for=<том>", K::Exec, "vssadmin.exe list shadows /for={VOL}", P_VOL, R::Safe, 60, nullptr, nullptr, "Точки восстановления Windows 7+ — это теневые копии: показывает, есть ли что восстанавливать.", nullptr},
        {59, "Recovery", "Точки восстановления: состояние защиты и список", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Читает настройки System Restore целевой ОС (в т.ч. офлайн) и список теневых копий.", NatRestorePoints},
        {60, "DISM/SFC", "DISM /Cleanup-Image /AnalyzeComponentStore", K::Exec, "{DISM} /Cleanup-Image /AnalyzeComponentStore", 0, R::Safe, 0, nullptr, nullptr, "Анализ хранилища компонентов, без изменений.", nullptr},
        {61, "DISM/SFC", "DISM /Cleanup-Image /StartComponentCleanup", K::Exec, "{DISM} /Cleanup-Image /StartComponentCleanup", 0, R::Changing, 0, nullptr, nullptr, "ИЗМЕНЯЕТ: очищает устаревшие компоненты WinSxS.", nullptr},
        {62, "DISM/SFC", "DISM /Cleanup-Image /RevertPendingActions (офлайн)", K::Exec, "{DISM} /Cleanup-Image /RevertPendingActions", 0, R::Danger, 0, nullptr, nullptr, "ОПАСНО: отменяет отложенные операции обновления (лечит «Отмена изменений» при загрузке). Только для офлайн-цели.", nullptr},
        {63, "DISM/SFC", "sfc /verifyonly", K::Exec, "{SFC} /verifyonly", 0, R::Safe, 0, nullptr, nullptr, "Только проверка системных файлов, без исправления.", nullptr},
        {64, "Хранилище", "chkdsk <том> /scan", K::Exec, "chkdsk.exe {VOL} /scan", P_VOL, R::Safe, 0, nullptr, nullptr, "Оперативное сканирование NTFS без остановки тома.", nullptr},
        {65, "Хранилище", "chkdsk <том> /spotfix", K::Exec, "chkdsk.exe {VOL} /spotfix", P_VOL, R::Changing, 0, "Y\r\n", nullptr, "ИЗМЕНЯЕТ: быстрое исправление найденных /scan проблем.", nullptr},
        {66, "Загрузка/BCD", "reagentc /info", K::Exec, "reagentc.exe /info", 0, R::Safe, 60, nullptr, nullptr, "Состояние среды восстановления WinRE текущей ОС.", nullptr},
        {67, "Загрузка/BCD", "reagentc /enable", K::Exec, "reagentc.exe /enable", 0, R::Changing, 120, nullptr, nullptr, "ИЗМЕНЯЕТ: включает WinRE.", nullptr},
        {68, "Загрузка/BCD", "reagentc /boottore", K::Exec, "reagentc.exe /boottore", 0, R::Changing, 60, nullptr, nullptr, "ИЗМЕНЯЕТ: следующая загрузка — в WinRE.", nullptr},
        {69, "Сеть", "netsh winsock reset", K::Exec, "netsh.exe winsock reset", 0, R::Changing, 60, nullptr, nullptr, "ИЗМЕНЯЕТ: сброс Winsock (нужна перезагрузка).", nullptr},
        {70, "Сеть", "netsh int ip reset", K::Exec, "netsh.exe int ip reset", 0, R::Changing, 60, nullptr, nullptr, "ИЗМЕНЯЕТ: сброс TCP/IP (нужна перезагрузка).", nullptr},
        {71, "Сеть", "ipconfig /flushdns", K::Exec, "ipconfig.exe /flushdns", 0, R::Safe, 30, nullptr, nullptr, "Очистка кэша DNS.", nullptr},
        {72, "Система", "winmgmt /verifyrepository", K::Exec, "wbem\\winmgmt.exe /verifyrepository", 0, R::Safe, 120, nullptr, nullptr, "Проверка согласованности репозитория WMI.", nullptr},
        {73, "Система", "lodctr /R", K::Exec, "lodctr.exe /R", 0, R::Changing, 120, nullptr, nullptr, "ИЗМЕНЯЕТ: пересборка счётчиков производительности.", nullptr},
        {74, "Система", "secedit /configure … defltbase.inf", K::Exec, "secedit.exe /configure /cfg {WINRAW}\\inf\\defltbase.inf /db defltbase.sdb /verbose", P_WIN, R::Danger, 600, nullptr, nullptr, "ОПАСНО: сброс политики безопасности/ACL к умолчанию. Работает на работающей ОС.", nullptr},
        {75, "Система", "mountvol", K::Exec, "mountvol.exe", 0, R::Safe, 30, nullptr, nullptr, "Список томов, их GUID и точек монтирования.", nullptr},
        {76, "Перезагрузка", "shutdown /r /o /t 0 — расширенные параметры (WinRE)", K::Exec, "shutdown.exe /r /o /t 0", 0, R::Changing, 30, nullptr, nullptr, "ИЗМЕНЯЕТ: немедленная перезагрузка в меню восстановления.", nullptr},
        {77, "Перезагрузка", "shutdown /r /t 0", K::Exec, "shutdown.exe /r /t 0", 0, R::Changing, 30, nullptr, nullptr, "ИЗМЕНЯЕТ: немедленная перезагрузка.", nullptr},
        {78, "WinPE", "wpeutil reboot", K::Exec, "wpeutil.exe reboot", 0, R::Changing, 30, nullptr, nullptr, "ИЗМЕНЯЕТ: перезагрузка из WinPE.", nullptr},
        {80, "Загрузка/BCD", "Secure Boot: сертификаты 2023 (KEK / db / dbx)", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Читает переменные UEFI и проверяет наличие сертификатов 2023 (срок старых истёк в 2026). Работает и в WinPE.", NatSecureBoot},
        {81, "Загрузка/BCD", "WinRE: статус, ReAgent.xml, расположение Winre.wim", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Определяет, настроен ли WinRE целевой ОС и где лежит образ (включая скрытый раздел восстановления).", NatWinReInfo},
        {82, "Хранилище", "Устройства без драйвера (код 28) и контроллеры хранилища (Intel VMD/RST)", K::Native, nullptr, 0, R::Safe, 0, nullptr, nullptr, "Показывает Hardware ID устройств без драйвера: так находят нужный драйвер, когда диск «не виден» в WinPE.", NatNoDriverDevices},
        {83, "Хранилище", "manage-bde -status (BitLocker)", K::Exec, "manage-bde.exe -status", 0, R::Safe, 60, nullptr, nullptr, "Состояние BitLocker всех томов. Разблокировка — в окне «BitLocker и драйверы хранилища».", nullptr},
        {79, "WinPE", "diskpart → list disk", K::Exec, "diskpart.exe /s {SCRIPT}", 0, R::Safe, 60, nullptr, "list disk\r\nexit\r\n", "Только список физических дисков.", nullptr},
    };
}

std::string Q(const std::string& s) { return s.find(' ') != std::string::npos ? "\"" + s + "\"" : s; }
std::string QQ(const std::string& s) { return "\"" + s + "\""; }

bool IsVol(const std::string& s) {
    return (s.size() == 2 || s.size() == 3) && isalpha((unsigned char)s[0]) && s[1] == ':' && (s.size() == 2 || s[2] == '\\');
}
std::string NormVol(const std::string& s) { return std::string(1, (char)toupper((unsigned char)s[0])) + ":"; }

std::string ResolveExe(const std::string& name) {
    std::string p = SysDir() + "\\" + name;
    if (PathExists(p)) return p;
    return FindInPath(name.c_str());
}

void Replace(std::string& s, const std::string& tok, const std::string& val) {
    size_t p = 0;
    while ((p = s.find(tok, p)) != std::string::npos) { s.replace(p, tok.size(), val); p += val.size(); }
}

std::string Expand(std::string s, const CmdParams& p, const std::string& scratch, const std::string& scriptPath) {
    Replace(s, "{VOLR}", IsVol(p.vol) ? NormVol(p.vol) + "\\" : p.vol);
    Replace(s, "{VOL}", IsVol(p.vol) ? NormVol(p.vol) : p.vol);
    Replace(s, "{EFI}", IsVol(p.efi) ? NormVol(p.efi) : p.efi);
    Replace(s, "{SRC}", Q(p.src));
    Replace(s, "{HOST}", p.host);
    Replace(s, "{WINRAW}", p.win);
    Replace(s, "{WIN}", Q(p.win));
    Replace(s, "{SCR}", QQ(scratch));
    Replace(s, "{SCRIPT}", QQ(scriptPath));
    Replace(s, "{BCDSTORE}", p.bcd.empty() ? std::string() : "/store \"" + p.bcd + "\"");
    return s;
}

void Audit(const std::string& line) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string path = DataDir() + "Logs\\" + fmt("commands_%04d%02d%02d.log", t.wYear, t.wMonth, t.wDay);
    MakeDirs(DataDir() + "Logs\\");
    HANDLE h = CreateFileW(W(path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    if (GetLastError() != ERROR_ALREADY_EXISTS) { const unsigned char b[3] = {0xEF, 0xBB, 0xBF}; WriteFile(h, b, 3, &w, nullptr); }
    std::string l = NiceNow() + " | " + line + "\r\n";
    WriteFile(h, l.data(), (DWORD)l.size(), &w, nullptr);
    CloseHandle(h);
}

std::string ReadTail(const std::string& path, size_t maxBytes) {
    HANDLE h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    LONGLONG start = sz.QuadPart > (LONGLONG)maxBytes ? sz.QuadPart - (LONGLONG)maxBytes : 0;
    LARGE_INTEGER pos;
    pos.QuadPart = start;
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    std::string data((size_t)(sz.QuadPart - start), '\0');
    size_t got = 0;
    while (got < data.size()) {
        DWORD r = 0;
        if (!ReadFile(h, &data[got], (DWORD)std::min<size_t>(data.size() - got, 1u << 20), &r, nullptr) || !r) break;
        got += r;
    }
    data.resize(got);
    CloseHandle(h);
    if (start > 0) { size_t nl = data.find('\n'); if (nl != std::string::npos) data.erase(0, nl + 1); }
    int wl = MultiByteToWideChar(CP_UTF8, 0, data.data(), (int)data.size(), nullptr, 0);
    if (wl > 0) { std::wstring w((size_t)wl, L'\0'); MultiByteToWideChar(CP_UTF8, 0, data.data(), (int)data.size(), &w[0], wl); return U(w); }
    return data;
}

}  // namespace

const std::vector<CmdItem>& Catalog() {
    static const std::vector<CmdItem> v = Build();
    return v;
}

const CmdItem* FindCmd(int no) {
    for (const auto& i : Catalog()) if (i.no == no) return &i;
    return nullptr;
}

std::string ScopeWarning(const CmdItem& it, const Target& t) {
    static const int envOnly[] = {2, 30, 31, 32, 43, 44, 45, 46, 47};
    if (t.valid && t.online) return "";
    for (int n : envOnly)
        if (it.no == n) return "ВНИМАНИЕ: команда описывает ТЕКУЩУЮ среду (например WinPE), а не выбранную целевую Windows.";
    if (it.no == 17 || it.no == 66) return "";
    return "";
}

const char* RiskText(Risk r) {
    return r == Risk::Safe ? "Безопасно (только чтение)" : r == Risk::Changing ? "ИЗМЕНЯЕТ систему" : "ОПАСНО (возможна потеря загрузки/данных)";
}
const char* KindText(CKind k) {
    switch (k) {
        case CKind::Exec: return "консольная команда (вывод — в журнал)";
        case CKind::Launch: return "GUI-программа Windows";
        case CKind::Native: return "нативная замена PowerShell";
        case CKind::Manual: return "ручная процедура (не выполняется)";
        case CKind::ViewFile: return "просмотр файла журнала";
    }
    return "";
}

bool CmdValidate(const CmdItem& it, const CmdParams& p, std::string& err) {
    if (it.params & P_VOL) { if (!IsVol(p.vol)) { err = "Параметр «Том»: ожидается буква диска, например C:"; return false; } }
    if (it.params & P_EFI) { if (!IsVol(p.efi)) { err = "Параметр «EFI-раздел»: ожидается буква, например S: (раздел должен быть смонтирован)"; return false; } }
    if (it.params & P_HOST) {
        if (p.host.empty() || p.host.size() > 253) { err = "Параметр «Узел / имя»: пусто или слишком длинно"; return false; }
        for (unsigned char ch : p.host)
            if (!(isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch == ':')) { err = "Параметр «Узел / имя»: допустимы только буквы, цифры, . - _ :"; return false; }
    }
    if ((it.params & P_BCD) && !p.bcd.empty() && !PathExists(p.bcd)) { err = "Параметр «BCD-хранилище»: файл не найден"; return false; }
    if (it.params & P_SRC) {
        if (trim(p.src).empty() || p.src.find('"') != std::string::npos) { err = "Параметр «Источник»: укажите путь или wim:<файл>:<индекс> (без кавычек)"; return false; }
    }
    if (it.params & P_WIN) {
        if (p.win.empty() || p.win.find_first_of("\"<>|") != std::string::npos || !IsDir(p.win)) { err = "Параметр «Путь Windows»: папка не найдена (например C:\\Windows)"; return false; }
    }
    return true;
}

std::string CmdBuild(const CmdItem& it, const CmdParams& p, const Target& t, const std::string& scratch, const std::string& scriptPath) {
    if (it.kind == CKind::Native) return std::string("[нативно] ") + it.title;
    if (it.kind == CKind::Manual) return "[инструкция] " + std::string(it.title);
    if (it.kind == CKind::ViewFile) return "[просмотр] " + (t.valid ? t.winDir : std::string("<Windows>")) + "\\" + it.tmpl;
    std::string s = it.tmpl;
    if (it.kind == CKind::Launch) {
        size_t bar = s.find('|');
        std::string exe = bar == std::string::npos ? s : s.substr(0, bar);
        return "[запуск GUI] " + exe + (bar == std::string::npos ? "" : " " + Expand(s.substr(bar + 1), p, scratch, scriptPath));
    }
    std::string head, rest;
    auto exeOf = [](const char* n) { std::string r = ResolveExe(n); return r.empty() ? std::string(n) : r; };
    if (s.rfind("{DISM}", 0) == 0) {
        head = QQ(exeOf("dism.exe"));
        head += t.online ? " /Online" : " /Image:" + t.root + (scratch.empty() ? "" : " /ScratchDir:\"" + scratch + "\"");
        rest = s.substr(6);
    } else if (s.rfind("{SFC}", 0) == 0) {
        head = QQ(exeOf("sfc.exe"));
        rest = s.substr(5);
        if (!t.online) rest += " /offbootdir=" + t.root + " /offwindir=" + t.winDir;
    } else {
        size_t sp = s.find(' ');
        std::string exe = sp == std::string::npos ? s : s.substr(0, sp);
        head = QQ(exeOf(exe.c_str()));
        rest = sp == std::string::npos ? "" : s.substr(sp);
    }
    s = head + rest;
    s = Expand(s, p, scratch, scriptPath);
    return s;
}

// ---------------------------------------------------------------------------------------------
void ModCommand(Ctx& c) {
    const CmdItem* it = FindCmd(c.opt.cmdNo);
    if (!it) { c.error("Команда не найдена в каталоге."); return; }
    std::string err;
    if (!CmdValidate(*it, c.opt.cp, err)) { c.error(err); return; }
    c.info(fmt("=== Команда №%d [%s] %s ===", it->no, it->cat, it->title));
    c.info(std::string("Тип: ") + KindText(it->kind) + "; уровень: " + RiskText(it->risk));
    std::string paramStr = fmt("vol=%s src=%s host=%s win=%s efi=%s", c.opt.cp.vol.c_str(), c.opt.cp.src.c_str(), c.opt.cp.host.c_str(), c.opt.cp.win.c_str(), c.opt.cp.efi.c_str());
    std::string tgt = c.target.valid ? c.target.root + (c.target.online ? " (онлайн)" : " (офлайн)") : "-";
    Audit(fmt("START #%d | %s | target=%s | %s | out=%s", it->no, it->title, tgt.c_str(), paramStr.c_str(), c.outDir.c_str()));
    std::string file = fmt("cmd%02d.txt", it->no);
    std::string result = "ok";

    switch (it->kind) {
        case CKind::Manual: {
            std::string t = it->tmpl;
            size_t p = 0;
            while (p < t.size()) {
                size_t e = t.find("\r\n", p);
                if (e == std::string::npos) e = t.size();
                c.info(t.substr(p, e - p));
                p = e + 2;
            }
            c.saveText(fmt("cmd%02d_manual.txt", it->no), t);
            result = "manual-shown";
            break;
        }
        case CKind::Native: {
            OutBuf ob;
            it->native(c, ob);
            c.saveText(file, ob.text);
            break;
        }
        case CKind::Launch: {
            std::string s = it->tmpl, exe = s, args;
            size_t bar = s.find('|');
            if (bar != std::string::npos) { exe = s.substr(0, bar); args = Expand(s.substr(bar + 1), c.opt.cp, "", ""); }
            bool msc = exe.size() > 4 && exe.substr(exe.size() - 4) == ".msc";
            std::string full = msc ? exe : ResolveExe(exe);
            if (full.empty()) { c.error(exe + " не найден в этой среде (в WinPE многие GUI-программы отсутствуют)."); result = "missing"; break; }
            HINSTANCE r = ShellExecuteW(nullptr, L"open", W(full).c_str(), args.empty() ? nullptr : W(args).c_str(), W(SysDir()).c_str(), SW_SHOWNORMAL);
            if ((INT_PTR)r > 32) c.ok("Запущено: " + full + (args.empty() ? "" : " " + args));
            else { c.error(fmt("Не удалось запустить %s (код %ld).", full.c_str(), (long)(INT_PTR)r)); result = "failed"; }
            break;
        }
        case CKind::ViewFile: {
            if (!c.needTarget()) { result = "no-target"; break; }
            std::string path = c.target.winDir + "\\" + it->tmpl;
            if (!PathExists(path)) { c.error("Файл не найден: " + path); result = "missing"; break; }
            std::string tail = ReadTail(path, 2u << 20);
            std::vector<std::string> lines;
            size_t p = 0;
            while (p < tail.size()) {
                size_t e = tail.find('\n', p);
                if (e == std::string::npos) e = tail.size();
                std::string l = tail.substr(p, e - p);
                while (!l.empty() && (l.back() == '\r')) l.pop_back();
                lines.push_back(l);
                p = e + 1;
            }
            std::vector<std::string> bad;
            for (const auto& l : lines) {
                std::string lo = toLowerUtf8(l);
                if (lo.find("error") != std::string::npos || lo.find("corrupt") != std::string::npos || lo.find("cannot repair") != std::string::npos || lo.find("failed") != std::string::npos) bad.push_back(l);
            }
            c.info(fmt("Файл: %s (прочитан хвост, строк: %zu, подозрительных: %zu)", path.c_str(), lines.size(), bad.size()));
            c.info("--- строки с ошибками (последние 60) ---");
            for (size_t i = bad.size() > 60 ? bad.size() - 60 : 0; i < bad.size(); ++i) Logger::I().log(Lvl::Out, "  | " + truncUtf8(bad[i], 300));
            c.info("--- последние 120 строк ---");
            for (size_t i = lines.size() > 120 ? lines.size() - 120 : 0; i < lines.size(); ++i) Logger::I().log(Lvl::Out, "  | " + truncUtf8(lines[i], 300));
            c.saveText(fmt("cmd%02d_tail.txt", it->no), tail);
            break;
        }
        case CKind::Exec: {
            std::string tm = it->tmpl;
            bool needTgt = tm.find("{DISM}") != std::string::npos || tm.find("{SFC}") != std::string::npos;
            if (needTgt && !c.needTarget()) { result = "no-target"; break; }
            std::string scratch;
            if (tm.find("{SCR}") != std::string::npos || (needTgt && !c.target.online)) { scratch = c.sub("scratch"); scratch.pop_back(); }
            std::string scriptPath;
            if (it->script) {
                scriptPath = c.outDir + "script.txt";
                WriteText(scriptPath, it->script, false);
            }
            std::string cmd = CmdBuild(*it, c.opt.cp, c.target, scratch, scriptPath);
            // fail early when the tool does not exist in this environment (e.g. bootrec outside WinRE)
            std::string first = cmd.size() && cmd[0] == '"' ? cmd.substr(1, cmd.find('"', 1) - 1) : cmd.substr(0, cmd.find(' '));
            if (first.find('\\') == std::string::npos && ResolveExe(first).empty()) {
                c.error(first + " не найден в этой среде. (bootrec доступен только в WinRE/WinPE; в обычной Windows используйте bcdboot/bcdedit.)");
                result = "missing";
                break;
            }
            ProcResult r = c.run(it->title, cmd, it->timeoutSec, it->input ? it->input : "");
            c.saveText(file, "> " + cmd + "\r\n\r\n" + r.output + fmt("\r\n[exit code: %lu]\r\n", (unsigned long)r.exitCode));
            result = r.cancelled ? "cancelled" : r.timedOut ? "timeout" : !r.started ? "not-started" : fmt("exit=%lu", (unsigned long)r.exitCode);
            if (r.started && !r.cancelled && !r.timedOut && r.exitCode == 0) c.ok(fmt("Команда №%d завершена успешно.", it->no));
            else if (r.started && !r.cancelled && !r.timedOut) c.warn(fmt("Команда №%d завершилась с кодом %lu — см. вывод выше.", it->no, (unsigned long)r.exitCode));
            break;
        }
    }
    Audit(fmt("END   #%d | %s | result=%s", it->no, it->title, result.c_str()));
}

}  // namespace wr
