// Offline registry engine: load hives of a (non-running) Windows with RegLoadKey, edit values/keys, backups, audit.
#pragma once
#include "app.h"

namespace wr {

struct HiveInfo {
    std::string label;  // "SYSTEM", "NTUSER (user)"
    std::string file;   // full path to hive file
    std::string mount;  // key name under HKLM while loaded
    std::string owner;  // user name for profile hives
    std::string profileDir;
    bool user = false;
};

std::vector<HiveInfo> ListHives(const Target& t, bool includeUsers = true);
std::vector<std::string> ListDirs(const std::string& dir);  // names of sub-directories
void EnablePrivilegeByName(const wchar_t* name);

class HiveManager {
public:
    bool load(const HiveInfo& h, std::string& err);
    bool isLoaded(const std::string& mount);
    HKEY open(const std::string& mount, REGSAM access = KEY_READ | KEY_WRITE);
    void unload(const std::string& mount);
    void unloadAll();
    std::vector<std::string> mounts();
    std::string backupDir();
    void pin();    // somebody uses the hives
    void unpin();  // last user gone -> unloadAll()

private:
    std::recursive_mutex mu_;
    std::map<std::string, std::string> loaded_;
    std::string backupDir_;
    int pins_ = 0;
};
HiveManager& Hives();

// Registry access to one Windows installation: live HKLM (online) or loaded hives (offline).
struct RegCtx {
    struct UserRoot { std::string name, profile; HKEY root = nullptr; };
    bool online = false;
    HKEY software = nullptr, system = nullptr;
    std::string cs = "ControlSet001";  // control set name usable under `system`
    std::vector<UserRoot> users;
    bool open(const Target& t, std::string& err);
    ~RegCtx();

private:
    bool pinned_ = false;
    std::vector<HKEY> opened_;
};

struct RegVal {
    std::string name;
    DWORD type = REG_NONE;
    std::vector<uint8_t> data;
};

bool EnumSubKeys(HKEY k, std::vector<std::string>& out);
bool EnumValues(HKEY k, std::vector<RegVal>& out);
bool QueryVal(HKEY base, const std::string& sub, const std::string& name, RegVal& out);
std::string RegTypeName(DWORD t);
DWORD RegTypeFromName(const std::string& n);
std::string RegValueText(const RegVal& v, size_t maxLen = 400);
bool RegParse(DWORD type, const std::string& text, std::vector<uint8_t>& out, std::string& err);
LONG RegSetRaw(HKEY k, const std::string& name, DWORD type, const std::vector<uint8_t>& data);
LONG RegSetString(HKEY base, const std::string& sub, const std::string& name, const std::string& value, DWORD type = REG_SZ);
LONG RegSetDword(HKEY base, const std::string& sub, const std::string& name, DWORD value);
LONG RegDelValue(HKEY base, const std::string& sub, const std::string& name);
LONG RegDelTree(HKEY base, const std::string& sub);
LONG RegMakeKey(HKEY base, const std::string& sub);
std::string RegText(const RegVal& v);  // string value as UTF-8 (first string for MULTI_SZ)
void ExportRegKey(HKEY k, const std::string& label, std::string& out, int depth = 0);

void AuditChange(const std::string& what);  // Data\Logs\changes_<date>.log + main journal
std::string FileBackup(const std::string& path, const std::string& tag);  // copy into Data\Backups\<stamp>_files\, returns backup path or ""

}  // namespace wr
