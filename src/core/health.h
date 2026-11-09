// Health score 0..100 (logic ported 1:1 from Get-WRHealthScore.ps1).
#pragma once
#include <algorithm>
#include <string>
#include <vector>
#include "textutil.h"

namespace wr {

struct DiskHealthIn {
    std::string name, health;
    int wear = -1;
    long long readErr = -1, writeErr = -1;
};

struct HealthInput {
    bool haveDisk = false;
    std::vector<DiskHealthIn> disks;
    bool haveIntegrity = false, integrityBad = false;
    bool haveDrivers = false;
    int errDevices = 0;
    bool haveBsod = false;
    int bsodEvents = 0;
    bool haveBoot = false;
    int bootIssues = 0;
};

struct HealthResult {
    int score = 100;
    std::string band;
    std::vector<std::string> reasons;
};

inline HealthResult computeHealth(const HealthInput& in) {
    HealthResult r;
    int s = 100;
    if (in.haveDisk) {
        for (const auto& d : in.disks) {
            if (d.health != "Healthy") { s -= 25; r.reasons.push_back(fmt("Диск %s: %s", d.name.c_str(), d.health.c_str())); }
            if (d.wear >= 80) { s -= 15; r.reasons.push_back(fmt("Диск %s: износ %d%%", d.name.c_str(), d.wear)); }
            if (d.readErr > 0 || d.writeErr > 0) {
                s -= 20;
                r.reasons.push_back(fmt("Диск %s: ошибки R=%lld/W=%lld", d.name.c_str(), d.readErr < 0 ? 0 : d.readErr, d.writeErr < 0 ? 0 : d.writeErr));
            }
        }
    }
    if (in.haveIntegrity && in.integrityBad) { s -= 25; r.reasons.push_back("Целостность: обнаружены нарушения"); }
    if (in.haveDrivers && in.errDevices > 0) {
        s -= std::min(20, in.errDevices * 5);
        r.reasons.push_back(fmt("Устройства с ошибкой: %d", in.errDevices));
    }
    if (in.haveBsod && in.bsodEvents > 0) {
        s -= std::min(30, in.bsodEvents * 10);
        r.reasons.push_back(fmt("BSOD: %d", in.bsodEvents));
    }
    if (in.haveBoot && in.bootIssues > 0) {
        s -= std::min(15, in.bootIssues * 5);
        r.reasons.push_back(fmt("Загрузка: %d замечаний", in.bootIssues));
    }
    if (s < 0) s = 0;
    r.score = s;
    r.band = s >= 90 ? "Excellent" : s >= 75 ? "Good" : s >= 50 ? "Fair" : s >= 25 ? "Poor" : "Critical";
    return r;
}

}  // namespace wr
