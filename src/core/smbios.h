// SMBIOS parser (input: GetSystemFirmwareTable('RSMB') blob). No WMI required -> works in WinPE.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include "textutil.h"

namespace wr {

struct SmbMem {
    std::string locator, bank, manufacturer, part, serial;
    uint64_t sizeMB = 0;
    unsigned speed = 0, configSpeed = 0, type = 0;
    bool present = false;
};

struct SmbInfo {
    bool ok = false;
    unsigned verMajor = 0, verMinor = 0;
    std::string biosVendor, biosVersion, biosDate;
    std::string boardMfr, boardProduct, sysMfr, sysProduct;
    std::vector<SmbMem> mem;
    unsigned memSlots = 0;
    uint64_t maxCapKB = 0;
};

inline const char* smbMemTypeName(unsigned t) {
    switch (t) {
        case 0x12: return "DDR";
        case 0x13: return "DDR2";
        case 0x18: return "DDR3";
        case 0x1A: return "DDR4";
        case 0x1B: return "LPDDR";
        case 0x1C: return "LPDDR2";
        case 0x1D: return "LPDDR3";
        case 0x1E: return "LPDDR4";
        case 0x22: return "DDR5";
        case 0x23: return "LPDDR5";
        default: return "?";
    }
}

inline SmbInfo parseSmbios(const uint8_t* raw, size_t len) {
    SmbInfo r;
    if (!raw || len < 8) return r;
    r.verMajor = raw[1];
    r.verMinor = raw[2];
    uint32_t tlen = raw[4] | ((uint32_t)raw[5] << 8) | ((uint32_t)raw[6] << 16) | ((uint32_t)raw[7] << 24);
    const uint8_t* t = raw + 8;
    size_t avail = len - 8;
    if (tlen < avail) avail = tlen;
    size_t pos = 0;
    while (pos + 4 <= avail) {
        uint8_t type = t[pos];
        size_t hl = t[pos + 1];
        if (hl < 4 || pos + hl > avail) break;
        const uint8_t* h = t + pos;
        // string set
        std::vector<std::string> strs;
        size_t s = pos + hl;
        if (s < avail && t[s] == 0) {
            s += 2;
        } else {
            while (s < avail && t[s] != 0) {
                size_t e = s;
                while (e < avail && t[e] != 0) ++e;
                strs.emplace_back((const char*)t + s, e - s);
                s = e + 1;
            }
            ++s;  // closing NUL
        }
        auto str = [&](size_t off) -> std::string {
            if (off >= hl) return "";
            unsigned idx = h[off];
            if (idx == 0 || idx > strs.size()) return "";
            return trim(strs[idx - 1]);
        };
        auto u16 = [&](size_t off) -> unsigned { return off + 2 <= hl ? (unsigned)(h[off] | (h[off + 1] << 8)) : 0u; };
        auto u32 = [&](size_t off) -> uint32_t {
            return off + 4 <= hl ? (uint32_t)(h[off] | (h[off + 1] << 8) | ((uint32_t)h[off + 2] << 16) | ((uint32_t)h[off + 3] << 24)) : 0u;
        };
        auto u64 = [&](size_t off) -> uint64_t { return (uint64_t)u32(off) | ((uint64_t)u32(off + 4) << 32); };

        if (type == 127) break;
        if (type == 0 && hl >= 0x09) {
            r.biosVendor = str(4); r.biosVersion = str(5); r.biosDate = str(8);
        } else if (type == 1 && hl >= 0x06) {
            r.sysMfr = str(4); r.sysProduct = str(5);
        } else if (type == 2 && hl >= 0x06) {
            r.boardMfr = str(4); r.boardProduct = str(5);
        } else if (type == 16 && hl >= 0x0F) {
            uint32_t cap = u32(7);
            r.memSlots += u16(0x0D);
            if (cap == 0x80000000u && hl >= 0x17) r.maxCapKB += u64(0x0F) / 1024;
            else r.maxCapKB += cap;
        } else if (type == 17 && hl >= 0x15) {
            SmbMem m;
            unsigned sz = u16(0x0C);
            m.present = sz != 0 && sz != 0xFFFF;
            if (sz == 0x7FFF && hl >= 0x20) m.sizeMB = u32(0x1C) & 0x7FFFFFFF;
            else if (sz & 0x8000) m.sizeMB = (sz & 0x7FFF) / 1024;
            else m.sizeMB = sz;
            m.locator = str(0x10);
            m.bank = str(0x11);
            m.type = hl > 0x12 ? h[0x12] : 0;
            m.speed = u16(0x15);
            if (hl >= 0x18) m.manufacturer = str(0x17);
            if (hl >= 0x19) m.serial = str(0x18);
            if (hl >= 0x1B) m.part = str(0x1A);
            if (hl >= 0x22) m.configSpeed = u16(0x20);
            r.mem.push_back(m);
        }
        pos = s;
    }
    r.ok = true;
    return r;
}

}  // namespace wr
