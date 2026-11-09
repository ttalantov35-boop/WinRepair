// WinRE integration: discovery, one-time boot into WinRE, custom "WinRE + WinRepair" image (copy of winre.wim + our tool).
#pragma once
#include "bcd.h"

namespace wr {

struct WinReInfo {
    bool xmlFound = false;
    std::string xmlPath, installState, location, bcdId;
    std::string wimPath;                    // Winre.wim actually found (drive letter path)
    std::vector<std::string> wimCandidates;
    unsigned long long wimSize = 0;
};

WinReInfo ReadWinReInfo(const Target& t);
std::string FindWinReEntry(const std::vector<BcdEntry>& entries, const std::string& preferId, bool custom);
std::string WinReSdiFor(const std::string& wimPath, const Target& t);  // path of boot.sdi next to wim (copied from Windows\Boot if missing)
bool CreateRamdiskEntry(Ctx& c, const std::string& store, const std::string& desc, const std::string& wimPath, const std::string& sdiPath, std::string& outId);
void JobBuildWinRepairRE(Ctx& c, const std::string& store, const std::string& srcWim);
void JobRemoveWinRepairRE(Ctx& c, const std::string& store);
void JobBootOnce(Ctx& c, const std::string& store, const std::string& id, bool reboot);
std::string RebootCommand();
bool CopyTree(const std::string& src, const std::string& dst, int& files, const std::atomic<bool>* cancel);

}  // namespace wr
