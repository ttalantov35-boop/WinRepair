// Installed programs of an OFFLINE Windows (registry "Uninstall" keys) and forced removal of a program that blocks the boot.
// An offline system cannot run uninstallers, so "removal" = disable what starts (services, drivers, Run keys),
// move the program folder to quarantine (reversible) and drop the entry from the programs list.
#pragma once
#include "../win/offreg.h"
#include "autoruns.h"

namespace wr {

struct AppItem {
    std::string name, publisher, version, date, location, uninstall, source;
    unsigned long long sizeKB = 0;
    bool msi = false, system = false, update = false, dirExists = false;
    std::string user;   // "" = whole machine, else the user whose hive holds the entry
    std::string sub;    // key path under the hive root (SOFTWARE or the user's hive)
};

struct RemovePlan {
    bool disableStartup = true;
    int files = 1;          // 0 leave, 1 move to quarantine (reversible), 2 delete for good
    bool deleteEntry = true;
};

std::vector<AppItem> ScanApps(const Target& t, std::string& err);
std::vector<AutoItem> RelatedStartup(const AppItem& a, const std::vector<AutoItem>& all);
bool AppFolderSafe(const Target& t, const std::string& dir, std::string& why);   // refuses system folders and shallow paths
bool AppRemove(const Target& t, const AppItem& a, const RemovePlan& plan, const std::vector<AutoItem>& related, std::vector<std::string>& log);
bool AppDeleteEntry(const Target& t, const AppItem& a, std::string& err);
std::string AppsCsv(const std::vector<AppItem>& v);

}  // namespace wr
