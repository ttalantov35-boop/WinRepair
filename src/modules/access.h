// Access to the target disk: BitLocker (unlock/suspend/status) and storage drivers (drvload, offline DISM /Add-Driver).
#pragma once
#include "catalog.h"
#include "../win/offreg.h"   // AuditChange, ListDirs

namespace wr {

struct BlVolume {
    char letter = 0;
    std::string lockStatus, protection, conversion, raw;
    bool locked = false, encrypted = false, known = false;
};

bool BlToolPresent();
std::vector<BlVolume> BlScan();                      // manage-bde -status for every fixed/removable letter
std::string NormalizeRecoveryPassword(const std::string& in);  // 48 digits -> 8 groups of 6; "" when invalid
std::string MaskSecrets(const std::string& line);              // hides 6-digit recovery password groups
// kind: 0 recovery password, 1 password, 2 recovery key file (.bek). The secret is never written to the journal.
bool BlUnlock(Ctx& c, char letter, int kind, const std::string& secret);
void BlRunSafe(Ctx& c, const std::string& title, const std::string& args);  // manage-bde <args>, output masked

// devices without driver (problem code 28): returns count, fills "name | hardware id" lines, sets vmd for Intel VMD/RST controllers
int NoDriverDevices(std::vector<std::string>& lines, bool& vmd);

std::vector<std::string> FindStorageInfs(const std::string& folder, const std::atomic<bool>* cancel, int maxResults = 60);

}  // namespace wr
