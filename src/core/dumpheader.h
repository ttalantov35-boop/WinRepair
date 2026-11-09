// Quick parser for Windows kernel dump headers (PAGEDU64 / PAGEDUMP).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace wr {

struct DumpInfo {
    bool ok = false;
    bool is64 = false;
    std::string kind, error;
    uint32_t major = 0, minor = 0, bugcheck = 0, dumpType = 0;
    uint64_t param[4] = {0, 0, 0, 0};
};

inline DumpInfo parseDumpHeader(const uint8_t* d, size_t n) {
    DumpInfo r;
    auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= n ? (uint32_t)(d[o] | (d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24)) : 0u; };
    auto u64 = [&](size_t o) -> uint64_t { return (uint64_t)u32(o) | ((uint64_t)u32(o + 4) << 32); };
    if (n < 0x60) { r.error = "Файл слишком мал"; return r; }
    if (memcmp(d, "MDMP", 4) == 0) { r.error = "Формат MDMP (пользовательский дамп), не дамп ядра"; return r; }
    if (memcmp(d, "PAGE", 4) != 0) { r.error = "Неизвестная сигнатура"; return r; }
    bool x64 = memcmp(d + 4, "DU64", 4) == 0;
    bool x86 = memcmp(d + 4, "DUMP", 4) == 0;
    if (!x64 && !x86) { r.error = "Неизвестная сигнатура PAGE"; return r; }
    r.is64 = x64;
    r.major = u32(8);
    r.minor = u32(12);
    if (x64) {
        r.bugcheck = u32(0x38);
        for (int i = 0; i < 4; ++i) r.param[i] = u64(0x40 + 8 * i);
        r.dumpType = u32(0xF98);
    } else {
        r.bugcheck = u32(0x28);
        for (int i = 0; i < 4; ++i) r.param[i] = u32(0x2C + 4 * i);
        r.dumpType = u32(0xF88);
    }
    switch (r.dumpType) {
        case 1: r.kind = "Полный дамп"; break;
        case 2: r.kind = "Дамп ядра"; break;
        case 5: r.kind = "Малый дамп (minidump)"; break;
        default: r.kind = "Дамп (тип не определён)";
    }
    r.ok = true;
    return r;
}

}  // namespace wr
