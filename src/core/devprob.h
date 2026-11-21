// Коды проблем устройств (Диспетчер устройств, CM_PROB_*): что значит код и что делать. ru + en.
// Используется в журнале модуля «Драйверы» и в HTML-отчёте полной диагностики.
#pragma once

namespace wr {

struct DevProblem {
    unsigned code;
    const char* meaning;     // что значит (ru)
    const char* meaningEn;
    const char* action;      // что делать (ru)
    const char* actionEn;
};

inline const DevProblem* devProblemTable(unsigned& n) {
    static const DevProblem t[] = {
        {1, "Устройство настроено неправильно (нет подходящего драйвера или настроек)", "The device is not configured correctly (no suitable driver or settings)",
         "Установить или обновить драйвер от производителя устройства", "Install or update the driver from the device maker"},
        {3, "Драйвер повреждён, или системе не хватает памяти/ресурсов для него", "The driver is corrupted, or the system lacks memory/resources for it",
         "Переустановить драйвер; закрыть лишние программы; проверить целостность Windows", "Reinstall the driver; close extra programs; check Windows integrity"},
        {9, "Windows не может определить устройство (неверные идентификаторы)", "Windows cannot identify the device (invalid identifiers)",
         "Обновить драйвер и прошивку (BIOS/UEFI); проверить подключение устройства", "Update the driver and firmware (BIOS/UEFI); check the device connection"},
        {10, "Устройство не может запуститься", "The device cannot start",
         "Обновить или откатить драйвер; посмотреть события System в момент подключения; проверить питание и кабель", "Update or roll back the driver; look at System events at the time; check power and cable"},
        {12, "Недостаточно свободных ресурсов (конфликт с другим устройством)", "Not enough free resources (a conflict with another device)",
         "Отключить лишние устройства в BIOS/UEFI; обновить BIOS; переустановить драйвер", "Disable extra devices in BIOS/UEFI; update the BIOS; reinstall the driver"},
        {14, "Устройству нужна перезагрузка компьютера", "The device needs the computer to restart",
         "Перезагрузить компьютер", "Restart the computer"},
        {16, "Windows не может определить все ресурсы устройства", "Windows cannot identify all the device's resources",
         "Обновить драйвер; при необходимости назначить ресурсы вручную в свойствах устройства", "Update the driver; if needed, assign resources manually in the device properties"},
        {18, "Драйверы устройства нужно переустановить", "The device drivers must be reinstalled",
         "Удалить устройство в Диспетчере устройств и обновить конфигурацию оборудования или установить драйвер заново", "Remove the device in Device Manager and scan for hardware changes, or install the driver again"},
        {19, "Повреждены или неполны сведения о настройке устройства в реестре", "The device configuration information in the registry is corrupted or incomplete",
         "Удалить устройство и переустановить драйвер; проверить фильтры UpperFilters/LowerFilters класса устройства", "Remove the device and reinstall the driver; check the UpperFilters/LowerFilters of the device class"},
        {21, "Windows удаляет это устройство", "Windows is removing this device",
         "Подождать несколько секунд и обновить список; при необходимости перезагрузить компьютер", "Wait a few seconds and refresh; restart the computer if needed"},
        {22, "Устройство отключено (выключено вручную в Диспетчере устройств или программой)", "The device is disabled (turned off manually in Device Manager or by a program)",
         "Если устройство нужно — включить в Диспетчере устройств («Включить устройство»). Если отключено специально — это не ошибка", "If the device is needed, enable it in Device Manager (\"Enable device\"). If it was disabled on purpose, this is not an error"},
        {24, "Устройство отсутствует, работает неправильно или установлены не все драйверы", "The device is absent, is working improperly, or not all drivers are installed",
         "Проверить подключение и питание; переустановить драйвер; для отсутствующих устройств — удалить запись", "Check the connection and power; reinstall the driver; for absent devices, remove the entry"},
        {28, "Драйверы для устройства не установлены", "The drivers for the device are not installed",
         "Установить драйвер с сайта производителя устройства или ноутбука; в WinPE — загрузить драйвер (drvload)", "Install the driver from the device or laptop maker; in WinPE load the driver (drvload)"},
        {29, "Устройство отключено в прошивке (BIOS/UEFI)", "The device is disabled in the firmware (BIOS/UEFI)",
         "Включить устройство в настройках BIOS/UEFI", "Enable the device in the BIOS/UEFI settings"},
        {31, "Устройство работает неправильно: Windows не может загрузить нужные драйверы", "The device works improperly: Windows cannot load the required drivers",
         "Переустановить или обновить драйвер; проверить подпись драйвера и фильтры класса", "Reinstall or update the driver; check the driver signature and the class filters"},
        {32, "Служба (драйвер) этого устройства отключена (тип запуска «Отключена»)", "The service (driver) of this device is disabled (start type \"Disabled\")",
         "Вернуть службе драйвера тип запуска по умолчанию (окно «Автозагрузка и службы») или переустановить драйвер", "Set the driver service back to its default start type (\"Startup and services\" window) or reinstall the driver"},
        {33, "Windows не может определить, какие ресурсы нужны устройству", "Windows cannot determine which resources the device needs",
         "Обновить BIOS/UEFI; проверить или заменить устройство", "Update the BIOS/UEFI; check or replace the device"},
        {34, "Устройство требует ручной настройки ресурсов", "The device requires manual resource configuration",
         "Настроить ресурсы по документации устройства или переставить его в другой слот", "Configure the resources per the device documentation or move it to another slot"},
        {35, "В прошивке компьютера нет нужных сведений для устройства", "The computer firmware lacks the information the device needs",
         "Обновить BIOS/UEFI у производителя материнской платы или ноутбука", "Update the BIOS/UEFI from the motherboard or laptop maker"},
        {36, "Конфликт настроек прерываний (запрос PCI, а настроено ISA)", "An interrupt setting conflict (a PCI request configured as ISA)",
         "Изменить резервирование IRQ в BIOS/UEFI; обновить BIOS", "Change the IRQ reservation in BIOS/UEFI; update the BIOS"},
        {37, "Драйвер вернул ошибку при запуске", "The driver returned an error on start",
         "Переустановить или откатить драйвер; посмотреть события System", "Reinstall or roll back the driver; look at System events"},
        {38, "Предыдущий экземпляр драйвера ещё в памяти", "A previous instance of the driver is still in memory",
         "Перезагрузить компьютер", "Restart the computer"},
        {39, "Драйвер повреждён или отсутствует", "The driver is corrupted or missing",
         "Переустановить драйвер; проверить целостность Windows (sfc, DISM)", "Reinstall the driver; check Windows integrity (sfc, DISM)"},
        {40, "Повреждена или отсутствует запись службы драйвера в реестре", "The driver service entry in the registry is corrupted or missing",
         "Переустановить драйвер", "Reinstall the driver"},
        {41, "Драйвер загружен, но устройство не найдено", "The driver loaded, but the device was not found",
         "Удалить драйвер (если устройство не используется) или проверить подключение устройства", "Remove the driver (if the device is unused) or check the device connection"},
        {42, "В системе уже есть такое же устройство (дубликат)", "An identical device already exists in the system (a duplicate)",
         "Перезагрузить компьютер; удалить дубликат в Диспетчере устройств", "Restart the computer; remove the duplicate in Device Manager"},
        {43, "Устройство сообщило о проблеме, и Windows его остановила", "The device reported a problem and Windows stopped it",
         "Часто — сбой оборудования, кабеля или порта. Переподключить, обновить драйвер и прошивку устройства, проверить на другом компьютере", "Often a failure of the hardware, cable or port. Reconnect, update the device driver and firmware, test on another computer"},
        {44, "Приложение или служба остановили это устройство", "An application or service stopped this device",
         "Перезагрузить компьютер", "Restart the computer"},
        {45, "Устройство сейчас не подключено (ранее было)", "The device is not currently connected (it was before)",
         "Подключить устройство; если оно больше не используется — это не ошибка", "Connect the device; if it is no longer used, this is not an error"},
        {46, "Устройство недоступно: Windows завершает работу", "The device is unavailable: Windows is shutting down",
         "Перезагрузить компьютер", "Restart the computer"},
        {47, "Устройство подготовлено к безопасному извлечению", "The device was prepared for safe removal",
         "Отключить и снова подключить устройство", "Unplug and reconnect the device"},
        {48, "Драйвер заблокирован из-за известных проблем совместимости", "The driver is blocked due to known compatibility problems",
         "Установить более новый совместимый драйвер от производителя", "Install a newer compatible driver from the maker"},
        {49, "Куст реестра SYSTEM слишком большой — новые устройства не запускаются", "The SYSTEM registry hive is too large — new devices do not start",
         "Удалить неиспользуемые (скрытые) устройства; проверить реестр", "Remove unused (hidden) devices; check the registry"},
        {50, "Windows не может применить все свойства устройства", "Windows cannot apply all the device's properties",
         "Обновить драйвер", "Update the driver"},
        {51, "Устройство ожидает запуска другого устройства", "The device is waiting on another device to start",
         "Найти и исправить устройство, от которого зависит это", "Find and fix the device this one depends on"},
        {52, "Не удаётся проверить цифровую подпись драйвера", "The driver's digital signature cannot be verified",
         "Установить подписанный драйвер от производителя; проверить Secure Boot", "Install a signed driver from the maker; check Secure Boot"},
        {53, "Устройство зарезервировано отладчиком ядра", "The device is reserved by the kernel debugger",
         "Отключить отладку ядра (bcdedit /debug off) и перезагрузить компьютер", "Turn off kernel debugging (bcdedit /debug off) and restart the computer"},
        {54, "Сбой устройства, выполняется сброс", "The device failed and is being reset",
         "Подождать; если повторяется — обновить драйвер и прошивку устройства", "Wait; if it repeats, update the device driver and firmware"},
        {56, "Windows настраивает сетевой адаптер (настройка класса)", "Windows is configuring the network adapter (class setup)",
         "Подождать и перезагрузить компьютер", "Wait and restart the computer"},
    };
    n = sizeof(t) / sizeof(t[0]);
    return t;
}

inline const DevProblem* devProblem(unsigned code) {
    unsigned n;
    const DevProblem* t = devProblemTable(n);
    for (unsigned i = 0; i < n; ++i)
        if (t[i].code == code) return &t[i];
    return nullptr;
}

}  // namespace wr
