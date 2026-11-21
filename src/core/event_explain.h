// Пояснения к частым событиям журналов System/Application (источник + код) и к кодам завершения DISM/SFC. ru + en.
// Используется в HTML-отчёте: не «ошибок N штук», а что это за событие и что проверить.
#pragma once
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>

namespace wr {

struct EventExplain {
    const char* provider;   // часть имени источника (без учёта регистра); "" — любой
    unsigned id;            // код события; 0 — любой код этого источника
    const char* meaning;
    const char* meaningEn;
    const char* action;
    const char* actionEn;
};

inline const EventExplain* eventExplainTable(size_t& n) {
    static const EventExplain t[] = {
        {"Kernel-Power", 41, "Компьютер перезагрузился без корректного завершения работы (зависание, синий экран, пропало питание, удержание кнопки)",
         "The computer restarted without shutting down cleanly (a hang, a blue screen, lost power, holding the button)",
         "Если одновременно есть BugCheck 1001 — это синий экран (см. раздел BSOD). Иначе проверить блок питания, температуры, кнопку питания",
         "If there is also BugCheck 1001, it was a blue screen (see the BSOD section). Otherwise check the power supply, temperatures and the power button"},
        {"EventLog", 6008, "Предыдущее завершение работы было неожиданным", "The previous shutdown was unexpected",
         "Сопоставить по времени с Kernel-Power 41, BugCheck и событиями дисков", "Match by time with Kernel-Power 41, BugCheck and disk events"},
        {"WER-SystemErrorReporting", 1001, "Был синий экран (BugCheck); код остановки — в разделе BSOD", "There was a blue screen (BugCheck); the stop code is in the BSOD section",
         "Справочник кодов BSOD и «Разбор дампов»", "The BSOD code reference and \"Dump analysis\""},
        {"BugCheck", 1001, "Был синий экран (BugCheck); код остановки — в разделе BSOD", "There was a blue screen (BugCheck); the stop code is in the BSOD section",
         "Справочник кодов BSOD и «Разбор дампов»", "The BSOD code reference and \"Dump analysis\""},
        {"disk", 7, "На диске обнаружен повреждённый блок (сбой чтения поверхности)", "A bad block was found on the disk (a surface read failure)",
         "Срочно сделать копию данных; проверить SMART; chkdsk /scan", "Back up the data urgently; check SMART; chkdsk /scan"},
        {"disk", 11, "Ошибка контроллера при обращении к диску", "A controller error accessing the disk",
         "Проверить кабель, порт, питание диска, драйвер контроллера", "Check the cable, port, disk power and the controller driver"},
        {"disk", 51, "Ошибка при страничной операции (чтение/запись подкачки)", "An error during a paging operation (reading/writing the page file)",
         "Проверить кабель и SMART накопителя; обновить драйвер хранилища", "Check the cable and the drive's SMART; update the storage driver"},
        {"disk", 153, "Операция ввода-вывода повторена (накопитель отвечает с задержками)", "An I/O operation was retried (the drive responds with delays)",
         "Проверить SMART, кабель, режим питания накопителя, прошивку SSD", "Check SMART, the cable, the drive power mode, the SSD firmware"},
        {"disk", 154, "Операция ввода-вывода не выполнена из-за аппаратной ошибки", "An I/O operation failed because of a hardware error",
         "Копия данных; проверить SMART и кабель; накопитель может отказывать", "Back up the data; check SMART and the cable; the drive may be failing"},
        {"Ntfs", 55, "Повреждена структура файловой системы на томе", "The file system structure on the volume is corrupted",
         "chkdsk /scan, затем chkdsk /f (только после копии данных)", "chkdsk /scan, then chkdsk /f (only after backing up the data)"},
        {"Ntfs", 98, "Том требует проверки (обнаружены ошибки файловой системы)", "The volume needs checking (file system errors found)",
         "chkdsk /scan; при ошибках — chkdsk /f после копии данных", "chkdsk /scan; on errors, chkdsk /f after backing up the data"},
        {"Ntfs", 137, "Ошибка журнала транзакций NTFS", "An NTFS transaction log error",
         "chkdsk /scan; проверить накопитель (SMART)", "chkdsk /scan; check the drive (SMART)"},
        {"Ntfs", 140, "Не удалось сбросить данные журнала транзакций на диск", "Failed to flush transaction log data to the disk",
         "Проверить накопитель, кабель, питание", "Check the drive, cable and power"},
        {"stornvme", 0, "Ошибка драйвера NVMe-накопителя", "An NVMe drive driver error",
         "Обновить прошивку SSD и драйвер хранилища; проверить SMART", "Update the SSD firmware and the storage driver; check SMART"},
        {"storahci", 0, "Ошибка драйвера SATA/AHCI", "A SATA/AHCI driver error",
         "Проверить кабель SATA, порт, режим SATA в BIOS, драйвер контроллера", "Check the SATA cable, port, the SATA mode in BIOS, the controller driver"},
        {"volmgr", 161, "Не удалось создать файл дампа памяти при синем экране", "Failed to create the memory dump file on a blue screen",
         "Проверить размер файла подкачки и свободное место на системном диске", "Check the page file size and free space on the system disk"},
        {"volsnap", 0, "Ошибка теневых копий тома (точек восстановления)", "A volume shadow copy error (restore points)",
         "Проверить место для теневых копий (vssadmin list shadowstorage) и состояние диска", "Check shadow copy space (vssadmin list shadowstorage) and the disk state"},
        {"WHEA-Logger", 17, "Исправленная аппаратная ошибка шины PCI Express", "A corrected PCI Express bus hardware error",
         "Обычно не критично; при частых повторах — обновить BIOS, драйвер чипсета, проверить устройство в слоте", "Usually not critical; if frequent, update the BIOS and chipset driver, check the device in its slot"},
        {"WHEA-Logger", 18, "Неисправимая аппаратная ошибка процессора (Machine Check)", "An uncorrectable processor hardware error (Machine Check)",
         "Проверить температуры, питание, разгон; тест процессора и памяти", "Check temperatures, power, overclocking; test the processor and memory"},
        {"WHEA-Logger", 19, "Исправленная аппаратная ошибка процессора или памяти", "A corrected processor or memory hardware error",
         "При повторах — тест памяти, проверка температуры и разгона", "If it repeats, test memory and check temperature and overclocking"},
        {"WHEA-Logger", 47, "Исправленная ошибка памяти", "A corrected memory error",
         "Тест памяти JEDEC; проверить модули и слоты", "A JEDEC memory test; check the modules and slots"},
        {"Service Control Manager", 7000, "Служба не запустилась", "A service failed to start",
         "Посмотреть имя службы в сообщении; проверить, установлена ли программа, которой она принадлежит", "Look at the service name in the message; check whether the program it belongs to is installed"},
        {"Service Control Manager", 7001, "Служба не запустилась, потому что не запустилась служба, от которой она зависит", "A service failed to start because a service it depends on failed to start",
         "Найти и исправить зависимую службу (первая в цепочке)", "Find and fix the service it depends on (the first in the chain)"},
        {"Service Control Manager", 7009, "Служба не ответила вовремя при запуске (таймаут)", "A service did not respond in time on start (timeout)",
         "Часто — перегрузка при старте или медленный диск; проверить автозагрузку и диск", "Often a startup overload or a slow disk; check startup items and the disk"},
        {"Service Control Manager", 7011, "Служба не ответила на команду вовремя (таймаут)", "A service did not respond to a command in time (timeout)",
         "Проверить нагрузку и диск; обновить программу, которой принадлежит служба", "Check the load and the disk; update the program the service belongs to"},
        {"Service Control Manager", 7023, "Служба завершилась с ошибкой", "A service terminated with an error",
         "Смотреть текст ошибки в сообщении и журнал программы службы", "See the error text in the message and the log of the service's program"},
        {"Service Control Manager", 7024, "Служба завершилась со своей (служебной) ошибкой", "A service terminated with a service-specific error",
         "Смотреть код в сообщении; переустановить программу службы", "See the code in the message; reinstall the service's program"},
        {"Service Control Manager", 7026, "Не загрузились драйверы, которые должны запускаться при загрузке", "Boot-start drivers failed to load",
         "Имена драйверов — в сообщении; удалить остатки старых программ (окно «Автозагрузка и службы»)", "The driver names are in the message; remove leftovers of old programs (\"Startup and services\" window)"},
        {"Service Control Manager", 7031, "Служба неожиданно завершилась и была перезапущена", "A service terminated unexpectedly and was restarted",
         "При повторах — обновить или переустановить программу службы", "If it repeats, update or reinstall the service's program"},
        {"Service Control Manager", 7034, "Служба неожиданно завершилась", "A service terminated unexpectedly",
         "При повторах — обновить или переустановить программу службы", "If it repeats, update or reinstall the service's program"},
        {"Service Control Manager", 7043, "Служба не завершилась корректно при выключении", "A service did not shut down cleanly",
         "Обычно не критично; при долгом выключении — обновить программу службы", "Usually not critical; if shutdown is slow, update the service's program"},
        {"Kernel-PnP", 219, "Не загрузился драйвер устройства", "A device driver failed to load",
         "Найти устройство в разделе «Драйверы»; установить или обновить драйвер", "Find the device in the \"Drivers\" section; install or update the driver"},
        {"Kernel-Boot", 0, "Событие загрузки ядра", "A kernel boot event",
         "Сопоставить по времени с проблемами загрузки", "Match by time with boot problems"},
        {"Kernel-General", 0, "Событие ядра Windows", "A Windows kernel event",
         "Смотреть текст сообщения", "See the message text"},
        {"DistributedCOM", 10016, "Нет прав запуска компонента DCOM (обычно безвредно)", "No launch permission for a DCOM component (usually harmless)",
         "Как правило, не требует действий", "Usually needs no action"},
        {"DistributedCOM", 10010, "Компонент DCOM не зарегистрировался вовремя", "A DCOM component did not register in time",
         "Обычно безвредно; при проблемах с программой — переустановить её", "Usually harmless; if a program misbehaves, reinstall it"},
        {"Application Error", 1000, "Программа аварийно завершилась (имя программы и модуля — в сообщении)", "A program crashed (the program and module names are in the message)",
         "Обновить или переустановить программу; если модуль системный — проверить целостность Windows", "Update or reinstall the program; if the module is a system one, check Windows integrity"},
        {"Application Hang", 1002, "Программа зависла и была закрыта", "A program hung and was closed",
         "Обновить программу; проверить диск и нагрузку", "Update the program; check the disk and load"},
        {".NET Runtime", 1026, "Ошибка программы на .NET", "An error in a .NET program",
         "Обновить программу и .NET; смотреть текст исключения", "Update the program and .NET; see the exception text"},
        {"Windows Error Reporting", 1001, "Отчёт об ошибке программы или системы", "An error report for a program or the system",
         "Смотреть имя программы в сообщении", "See the program name in the message"},
        {"TPM", 0, "Событие модуля TPM", "A TPM module event",
         "Проверить TPM в BIOS/UEFI и обновить прошивку", "Check the TPM in BIOS/UEFI and update the firmware"},
        {"Netwtw", 0, "Ошибка драйвера Wi-Fi Intel", "An Intel Wi-Fi driver error",
         "Обновить драйвер Wi-Fi с сайта Intel или производителя ноутбука", "Update the Wi-Fi driver from Intel or the laptop maker"},
        {"e1d", 0, "Ошибка драйвера сетевой карты Intel", "An Intel network card driver error",
         "Обновить драйвер сетевой карты", "Update the network card driver"},
        {"Tcpip", 4199, "Обнаружен конфликт IP-адресов", "An IP address conflict was detected",
         "Проверить статические IP-адреса в сети и DHCP", "Check static IP addresses on the network and DHCP"},
        {"Time-Service", 0, "Ошибка синхронизации времени", "A time synchronization error",
         "Проверить доступ к интернету и службу времени; обычно не критично", "Check internet access and the time service; usually not critical"},
        {"WindowsUpdateClient", 20, "Обновление Windows не установилось", "A Windows update failed to install",
         "Код ошибки — в сообщении; при повторах — сброс Windows Update (Мастер исправлений)", "The error code is in the message; if it repeats, reset Windows Update (the fix wizard)"},
        {"Servicing", 0, "Ошибка обслуживания компонентов Windows", "A Windows component servicing error",
         "Проверка целостности (DISM /ScanHealth)", "An integrity check (DISM /ScanHealth)"},
        {"Wininit", 0, "Событие процесса запуска Windows", "A Windows startup process event",
         "Смотреть текст сообщения", "See the message text"},
        {"User32", 1074, "Компьютер выключен или перезагружен программой/пользователем", "The computer was shut down or restarted by a program/user",
         "Обычно не ошибка — показывает, кто перезагрузил", "Usually not an error — it shows who restarted it"},
    };
    n = sizeof(t) / sizeof(t[0]);
    return t;
}

inline bool eeContainsI(const std::string& hay, const char* needle) {
    size_t nl = std::strlen(needle);
    if (!nl) return true;
    if (hay.size() < nl) return false;
    for (size_t i = 0; i + nl <= hay.size(); ++i) {
        size_t k = 0;
        while (k < nl) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            ++k;
        }
        if (k == nl) return true;
    }
    return false;
}

// Сначала точное совпадение «источник + код», потом «источник, любой код».
inline const EventExplain* explainEvent(const std::string& provider, unsigned id) {
    size_t n;
    const EventExplain* t = eventExplainTable(n);
    for (size_t i = 0; i < n; ++i)
        if (t[i].id == id && eeContainsI(provider, t[i].provider)) return &t[i];
    for (size_t i = 0; i < n; ++i)
        if (t[i].id == 0 && eeContainsI(provider, t[i].provider)) return &t[i];
    return nullptr;
}

// Коды завершения DISM / SFC.
inline std::string explainExitCode(const std::string& tool, uint32_t code, bool en = false) {
    if (code == 0) return en ? "0 — success" : "0 — успешно";
    bool dism = eeContainsI(tool, "dism"), sfc = eeContainsI(tool, "sfc");
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08X", code);
    std::string h = std::string(hex) + " (" + std::to_string(code) + ") — ";
    if (dism) {
        if (en) {
            switch (code) {
                case 87: return h + "invalid parameter. On Windows 7 the /CheckHealth, /ScanHealth and /RestoreHealth parameters are not supported — use SFC in offline mode";
                case 2: return h + "file or path not found (check the volume letter and the /Image path)";
                case 5: return h + "access denied (administrator rights are required)";
                case 50: return h + "the operation is not supported for this image or environment (for example, /Online in WinPE)";
                case 1726: return h + "remote procedure call failed (the service was interrupted; retry after a restart)";
                case 1910: return h + "the specified object source was not found";
                case 0x800F081F: return h + "no source for the repair files: specify /Source with an image of the same version and edition";
                case 0x800F0906: return h + "could not download repair files (no internet or Windows Update unavailable) — specify /Source";
                case 0x800F0907: return h + "download from Windows Update is blocked by policy — specify /Source";
                case 0x800F0950: return h + "the component is unavailable or the source was not found";
                case 0x800F082F: return h + "pending operations in the component store; restart and retry";
                case 0x800F0954: return h + "the update server (WSUS) is unavailable — specify /Source";
                case 0x80070002: return h + "file not found";
                case 0x80070003: return h + "path not found";
                case 0x80070005: return h + "access denied";
                case 0x8007000D: return h + "the data is corrupted";
                case 0x80070070: return h + "not enough disk space (several GB are needed for temporary files)";
                case 0x800736CC: return h + "a file in the component store is corrupted (needs /Source)";
                case 0x80073712: return h + "the component store is corrupted (needs /RestoreHealth with /Source)";
            }
            return h + "a DISM error; details are in the command output and in %WinDir%\\Logs\\DISM\\dism.log";
        }
        switch (code) {
            case 87: return h + "неверный параметр. Для Windows 7 параметры /CheckHealth, /ScanHealth, /RestoreHealth не поддерживаются — используйте sfc в офлайн-режиме";
            case 2: return h + "файл или путь не найден (проверьте букву тома и путь /Image)";
            case 5: return h + "отказано в доступе (нужны права администратора)";
            case 50: return h + "операция не поддерживается для этого образа или среды (например, /Online в WinPE)";
            case 1726: return h + "сбой удалённого вызова процедуры (служба прервана; повторите после перезагрузки)";
            case 1910: return h + "указанный источник объекта не найден";
            case 0x800F081F: return h + "не найден источник файлов для восстановления: укажите /Source с образом той же версии и редакции";
            case 0x800F0906: return h + "не удалось скачать файлы восстановления (нет интернета или Windows Update недоступен) — укажите /Source";
            case 0x800F0907: return h + "загрузка из Windows Update запрещена политикой — укажите /Source";
            case 0x800F0950: return h + "компонент недоступен или не найден источник";
            case 0x800F082F: return h + "отложенные операции в хранилище компонентов; перезагрузите и повторите";
            case 0x800F0954: return h + "сервер обновлений (WSUS) недоступен — укажите /Source";
            case 0x80070002: return h + "файл не найден";
            case 0x80070003: return h + "путь не найден";
            case 0x80070005: return h + "отказано в доступе";
            case 0x8007000D: return h + "данные повреждены";
            case 0x80070070: return h + "недостаточно места на диске (нужно несколько ГБ для временных файлов)";
            case 0x800736CC: return h + "файл в хранилище компонентов повреждён (нужен /Source)";
            case 0x80073712: return h + "хранилище компонентов повреждено (нужен /RestoreHealth с /Source)";
        }
        return h + "ошибка DISM; подробности — в выводе команды и в журнале %WinDir%\\Logs\\DISM\\dism.log";
    }
    if (sfc) {
        if (en) return code == 1 ? h + "SFC found violations (corrupted files); details in CBS.log"
                                 : h + "SFC finished with an error; details in %WinDir%\\Logs\\CBS\\CBS.log";
        if (code == 1) return h + "SFC нашёл нарушения (повреждённые файлы); подробности — CBS.log";
        return h + "SFC завершился с ошибкой; подробности — %WinDir%\\Logs\\CBS\\CBS.log";
    }
    return h + (en ? "a program exit code" : "код завершения программы");
}

}  // namespace wr
