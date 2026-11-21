// Встроенный справочник кодов BSOD (bugcheck). Тексты what/steps/important (ru) и whatEn/stepsEn/importantEn (en)
// — из справочника автора «Типовые коды BSOD» (Tynychsoft). Выбор языка — на стороне вызывающего (CurrentLang()).
// Код BSOD — категория bugcheck, а не диагноз: причину показывает дамп (!analyze -v), журнал событий и проверки.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace wr {

struct BsodEntry {
    uint32_t code;
    const char* name;
    const char* category;    // служебный идентификатор группы (driver, memory, disk…)
    const char* what;        // типичная категория причины (ru)
    const char* whatEn;
    const char* steps;       // первый шаг — проверки (ru)
    const char* stepsEn;
    const char* important;   // важно знать (ru)
    const char* importantEn;
    bool frequent;           // встречается часто
};

// Общая оговорка к справочнику (показывается в окне справочника и в отчёте).
inline const char* bsodCaveat(bool en = false) {
    return en
        ? "A BSOD code is a bugcheck category, not a diagnosis. The actual cause is found from the dump (WinDbg -> !analyze -v), "
          "the event log, recent actions (an update, a driver, a new device) and checks of disks, memory and drivers. "
          "One code is never a reason to replace hardware or reinstall the OS."
        : "Код BSOD — это категория bugcheck, а не диагноз. Конкретная причина устанавливается по дампу (WinDbg → !analyze -v), "
          "журналу событий, связи с недавними действиями (обновление, драйвер, новое устройство) и проверкам дисков, памяти и драйверов. "
          "Нельзя по одному коду назначать замену железа или переустановку ОС.";
}
inline const char* bsodDumpsNote(bool en = false) {
    return en
        ? "Dumps: %SystemRoot%\\Minidump\\ (small) and %SystemRoot%\\MEMORY.DMP (full, if enabled). "
          "Open them in WinDbg with the Microsoft symbol server or with the \"Dump analysis\" button."
        : "Дампы: %SystemRoot%\\Minidump\\ (малые) и %SystemRoot%\\MEMORY.DMP (полный, если включён). "
          "Открывать в WinDbg с символьным сервером Microsoft или кнопкой «Разбор дампов».";
}

inline const BsodEntry* bsodTable(size_t& n) {
    static const BsodEntry t[] = {
        {0xEF, "CRITICAL_PROCESS_DIED", "os_integrity",
         "Системный процесс завершился; часто — повреждение файлов, реже — диск или память",
         "A system process ended; often file corruption, less often disk or memory",
         "sfc /verifyonly, DISM /ScanHealth, SMART, chkdsk /scan",
         "sfc /verifyonly, DISM /ScanHealth, SMART, chkdsk /scan",
         "Не спешить с переустановкой: сначала целостность и диск",
         "Do not rush to reinstall: check integrity and the disk first", true},
        {0x7E, "SYSTEM_THREAD_EXCEPTION_NOT_HANDLED", "driver",
         "Необработанное исключение в системном потоке (драйвер или ПО)",
         "Unhandled exception in a system thread (driver or software)",
         "Дамп → !analyze -v, имя модуля",
         "Dump -> !analyze -v, the module name",
         "Модуль в дампе — ключ, а не код",
         "The module in the dump is the key, not the code", false},
        {0x0A, "IRQL_NOT_LESS_OR_EQUAL", "driver_or_memory",
         "Драйвер обратился к памяти на повышенном IRQL",
         "A driver accessed memory at a raised IRQL",
         "Дамп, драйверы, память",
         "Dump, drivers, memory",
         "Не путать с DRIVER_IRQL_NOT_LESS_OR_EQUAL: здесь виновник — часто не «явный» драйвер",
         "Do not confuse with DRIVER_IRQL_NOT_LESS_OR_EQUAL: here the culprit is often not the obvious driver", true},
        {0xD1, "DRIVER_IRQL_NOT_LESS_OR_EQUAL", "driver",
         "Драйвер работает на неверном IRQL",
         "A driver runs at the wrong IRQL",
         "Дамп → имя драйвера → обновить или откатить драйвер",
         "Dump -> the driver name -> update or roll back the driver",
         "Driver Verifier — только точечно, режим volatile",
         "Driver Verifier — targeted only, volatile mode", false},
        {0x50, "PAGE_FAULT_IN_NONPAGED_AREA", "memory_or_driver",
         "Обращение к невыгружаемой памяти; память, драйвер или диск",
         "Access to non-paged memory; memory, driver or disk",
         "Тест памяти JEDEC, SMART, дамп",
         "JEDEC memory test, SMART, dump",
         "Часто — память или повреждённый драйвер",
         "Often memory or a corrupted driver", true},
        {0x1A, "MEMORY_MANAGEMENT", "memory",
         "Ошибка управления памятью",
         "A memory management error",
         "Тест памяти JEDEC, дамп",
         "JEDEC memory test, dump",
         "Один прогон теста ничего не доказывает",
         "A single test pass proves nothing", true},
        {0x1E, "KMODE_EXCEPTION_NOT_HANDLED", "driver",
         "Исключение в режиме ядра (драйвер или ПО)",
         "An exception in kernel mode (driver or software)",
         "Дамп, драйверы; если в дампе указан модуль — имя модуля → драйвер/ПО",
         "Dump, drivers; if the dump names a module, the module name -> driver/software",
         "Аналогично SYSTEM_THREAD_EXCEPTION_NOT_HANDLED: имя модуля важнее кода",
         "Like SYSTEM_THREAD_EXCEPTION_NOT_HANDLED: the module name matters more than the code", false},
        {0x3B, "SYSTEM_SERVICE_EXCEPTION", "driver",
         "Исключение при выполнении системного сервиса",
         "An exception while running a system service",
         "Дамп, драйверы, ПО",
         "Dump, drivers, software",
         "Часто — драйвер или фильтр (антивирус, VPN, шифрование)",
         "Often a driver or filter (antivirus, VPN, encryption)", false},
        {0x139, "KERNEL_SECURITY_CHECK_FAILURE", "os_or_driver",
         "Нарушение защитной проверки ядра (повреждение структур, драйвер)",
         "A kernel security check failed (corrupted structures, a driver)",
         "Дамп, DISM /ScanHealth, sfc /verifyonly, память",
         "Dump, DISM /ScanHealth, sfc /verifyonly, memory",
         "Иногда — признак повреждения или атаки",
         "Sometimes a sign of corruption or an attack", false},
        {0x7F, "UNEXPECTED_KERNEL_MODE_TRAP", "hardware",
         "Ловушка ядра (например, двойная ошибка)",
         "A kernel trap (for example, a double fault)",
         "Дамп, память, питание, температуры",
         "Dump, memory, power, temperatures",
         "Аппаратные гипотезы приоритетнее",
         "Hardware hypotheses come first", false},
        {0x133, "DPC_WATCHDOG_VIOLATION", "driver_or_disk",
         "DPC/ISR превысил лимит времени; драйвер, диск или прошивка",
         "A DPC/ISR exceeded its time limit; driver, disk or firmware",
         "Дамп, журнал disk/stornvme, драйверы, прошивка накопителя",
         "Dump, disk/stornvme log, drivers, drive firmware",
         "Часто — SSD, драйвер хранилища или BIOS",
         "Often an SSD, a storage driver or the BIOS", false},
        {0x124, "WHEA_UNCORRECTABLE_ERROR", "hardware",
         "Аппаратная ошибка (процессор, память, шина, питание)",
         "A hardware error (processor, memory, bus, power)",
         "Журнал WHEA, температуры, память, питание",
         "WHEA log, temperatures, memory, power",
         "Программно не «лечится»; нужна диагностика оборудования",
         "Cannot be fixed in software; hardware diagnostics are needed", false},
        {0x24, "NTFS_FILE_SYSTEM", "filesystem",
         "Ошибка драйвера NTFS / файловой системы",
         "An NTFS / file system driver error",
         "chkdsk /scan, SMART, журнал Ntfs",
         "chkdsk /scan, SMART, the Ntfs log",
         "chkdsk /f — только после копии данных",
         "chkdsk /f — only after backing up the data", false},
        {0x7B, "INACCESSIBLE_BOOT_DEVICE", "boot",
         "ОС потеряла доступ к системному диску",
         "The OS lost access to the system disk",
         "Режим SATA (AHCI/RAID), драйвер хранилища, GPT/UEFI, bcdedit",
         "SATA mode (AHCI/RAID), the storage driver, GPT/UEFI, bcdedit",
         "Частая причина — смена режима SATA после установки Windows",
         "A common cause is changing the SATA mode after Windows was installed", true},
        {0x74, "BAD_SYSTEM_CONFIG_INFO", "registry",
         "Повреждение реестра или конфигурации",
         "Corruption of the registry or configuration",
         "WinRE → sfc /verifyonly, DISM /ScanHealth, точка восстановления",
         "WinRE -> sfc /verifyonly, DISM /ScanHealth, a restore point",
         "Копия перед правкой реестра",
         "Back up before editing the registry", false},
        {0x19, "BAD_POOL_HEADER", "driver",
         "Повреждение пула памяти",
         "Corruption of the memory pool",
         "Дамп, драйверы (Driver Verifier — точечно), память",
         "Dump, drivers (Driver Verifier — targeted), memory",
         "Часто — драйвер с ошибкой работы с пулом",
         "Often a driver that mishandles the pool", false},
        {0x5C, "HAL_INITIALIZATION_FAILED", "hardware",
         "Сбой инициализации HAL (слоя аппаратной абстракции)",
         "Failure to initialize the HAL (hardware abstraction layer)",
         "Прошивка, память, дамп",
         "Firmware, memory, dump",
         "Ранняя стадия загрузки; часто — оборудование",
         "An early boot stage; often hardware", false},
        {0x7A, "KERNEL_DATA_INPAGE_ERROR", "disk",
         "Ошибка чтения страницы с диска",
         "An error reading a page from the disk",
         "SMART, журнал disk/Ntfs, кабель и слот, память",
         "SMART, the disk/Ntfs log, the cable and slot, memory",
         "Признак проблем накопителя или кабеля",
         "A sign of a failing drive or cable", false},
        {0x109, "CRITICAL_STRUCTURE_CORRUPTION", "driver_or_attack",
         "Повреждение критической структуры ядра",
         "Corruption of a critical kernel structure",
         "Дамп, драйверы, целостность, память",
         "Dump, drivers, integrity, memory",
         "Иногда — признак вредоносного или неподписанного драйвера",
         "Sometimes a sign of a malicious or unsigned driver", false},
        {0x116, "VIDEO_TDR_FAILURE", "gpu",
         "Драйвер видеокарты не ответил вовремя (TDR)",
         "The video driver did not respond in time (TDR)",
         "Драйвер GPU, температуры, память и питание видеокарты",
         "The GPU driver, temperatures, the video card memory and power",
         "Смена или откат драйвера — по вендору; прошивка — осторожно",
         "Change or roll back the driver per the vendor; firmware — with care", false},
        {0xF7, "DRIVER_OVERRAN_STACK_BUFFER", "driver",
         "Переполнение стека драйвером",
         "A driver overran the stack",
         "Дамп, драйверы, обновить или откатить",
         "Dump, drivers, update or roll back",
         "Часто — уязвимый или устаревший драйвер",
         "Often a vulnerable or outdated driver", false},
        {0x18, "REFERENCE_BY_POINTER", "driver",
         "Некорректное управление ссылками на объекты (драйвер)",
         "Incorrect object reference handling (a driver)",
         "Дамп, драйверы, Driver Verifier — точечно",
         "Dump, drivers, Driver Verifier — targeted",
         "Проверить недавно установленные драйверы и фильтры",
         "Check recently installed drivers and filters", false},
        {0x4E, "PFN_LIST_CORRUPT", "memory_or_driver",
         "Повреждён список PFN (память или драйвер)",
         "The PFN list is corrupted (memory or a driver)",
         "Тест памяти JEDEC, дамп, драйверы",
         "JEDEC memory test, dump, drivers",
         "Не спешить с заменой ОЗУ без теста",
         "Do not replace RAM without a test first", false},
    };
    n = sizeof(t) / sizeof(t[0]);
    return t;
}

inline const BsodEntry* bsodByCode(uint32_t code) {
    size_t n;
    const BsodEntry* t = bsodTable(n);
    for (size_t i = 0; i < n; ++i)
        if (t[i].code == code) return &t[i];
    if (code & 0x10000000) return bsodByCode(code & ~0x10000000u);   // 0x1000007E и т. п. — тот же код
    return nullptr;
}

inline const BsodEntry* bsodByName(const std::string& name) {
    size_t n;
    const BsodEntry* t = bsodTable(n);
    for (size_t i = 0; i < n; ++i)
        if (name == t[i].name) return &t[i];
    return nullptr;
}

}  // namespace wr
