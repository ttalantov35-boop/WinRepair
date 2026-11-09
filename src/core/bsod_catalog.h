// Built-in BSOD catalogue (ported from Data/bsod-codes.json, bug check codes added).
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace wr {

struct BsodEntry {
    uint32_t code;
    const char* name;
    const char* category;
    const char* firstStep;
    const char* note;
};

inline const BsodEntry* bsodTable(size_t& n) {
    static const BsodEntry t[] = {
        {0xEF,  "CRITICAL_PROCESS_DIED", "os_integrity", "Проверка целостности + диск", "Сначала целостность и диск"},
        {0x7E,  "SYSTEM_THREAD_EXCEPTION_NOT_HANDLED", "driver", "Разбор дампа (!analyze -v)", "Модуль в дампе — ключ"},
        {0x0A,  "IRQL_NOT_LESS_OR_EQUAL", "driver_or_memory", "Дамп + отчёт по памяти", "Не путать с DRIVER_IRQL"},
        {0xD1,  "DRIVER_IRQL_NOT_LESS_OR_EQUAL", "driver", "Дамп + отчёт по драйверам", "Verifier volatile"},
        {0x50,  "PAGE_FAULT_IN_NONPAGED_AREA", "memory_or_driver", "Отчёт по памяти + отчёт по дискам", "JEDEC, несколько проходов"},
        {0x1A,  "MEMORY_MANAGEMENT", "memory", "Отчёт по памяти", "1 проход не доказывает"},
        {0x1E,  "KMODE_EXCEPTION_NOT_HANDLED", "driver", "Разбор дампа", "См. имя модуля"},
        {0x3B,  "SYSTEM_SERVICE_EXCEPTION", "driver", "Дамп + отчёт по драйверам", "Часто фильтр"},
        {0x139, "KERNEL_SECURITY_CHECK_FAILURE", "os_or_driver", "Проверка целостности + память", "Возможно повреждение"},
        {0x7F,  "UNEXPECTED_KERNEL_MODE_TRAP", "hardware", "Память + температуры", "Аппаратные гипотезы"},
        {0x133, "DPC_WATCHDOG_VIOLATION", "driver_or_disk", "Отчёт по дискам + прошивка", "SSD/драйвер хранилища"},
        {0x124, "WHEA_UNCORRECTABLE_ERROR", "hardware", "Журнал WHEA + температуры", "Аппаратная диагностика"},
        {0x24,  "NTFS_FILE_SYSTEM", "filesystem", "chkdsk /scan + SMART", "/f только с копией"},
        {0x7B,  "INACCESSIBLE_BOOT_DEVICE", "boot", "Режим SATA + BCD", "Часто смена SATA"},
        {0x74,  "BAD_SYSTEM_CONFIG_INFO", "registry", "WinRE + проверка целостности", "Копия перед правкой"},
        {0x19,  "BAD_POOL_HEADER", "driver", "Verifier точечно", "Ошибка пула"},
        {0x5C,  "HAL_INITIALIZATION_FAILED", "hardware", "Прошивка + память", "Ранняя стадия"},
        {0x7A,  "KERNEL_DATA_INPAGE_ERROR", "disk", "SMART + журнал disk", "Накопитель/кабель"},
        {0x109, "CRITICAL_STRUCTURE_CORRUPTION", "driver_or_attack", "Дамп + целостность", "Возможно неподписанный драйвер"},
        {0x116, "VIDEO_TDR_FAILURE", "gpu", "Драйвер GPU", "Прошивка GPU — осторожно"},
        {0xF7,  "DRIVER_OVERRAN_STACK_BUFFER", "driver", "Дамп + обновление драйвера", "Возможно уязвимость"},
        {0x18,  "REFERENCE_BY_POINTER", "driver", "Verifier точечно", "Недавние драйверы"},
        {0x4E,  "PFN_LIST_CORRUPT", "memory_or_driver", "Отчёт по памяти", "Не спешить с заменой ОЗУ"},
    };
    n = sizeof(t) / sizeof(t[0]);
    return t;
}

inline const BsodEntry* bsodByCode(uint32_t code) {
    size_t n;
    const BsodEntry* t = bsodTable(n);
    for (size_t i = 0; i < n; ++i)
        if (t[i].code == code) return &t[i];
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
