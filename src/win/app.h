// WinRepair GUI - application model shared by modules and UI.
#pragma once
#include "common.h"
#include "../../res/version.h"

namespace wr {

constexpr const char* kAppName = "WinRepair Toolkit";
constexpr const char* kAppVersion = WR_VERSION_STR;

// ---- environment --------------------------------------------------------
struct Env {
    bool isWinPE = false, isAdmin = false, isWow64 = false, isWinRE = false, winreShell = false;
    std::string arch;        // x64 / x86 / ARM64
    std::string osName;      // "Windows 11 Pro 23H2 (build 22631.2861)"
    std::string computer;
    std::string winDir;      // running system root, e.g. C:\Windows
    std::string sysDrive;    // "C:"
    unsigned major = 0, minor = 0, build = 0;
};
const Env& GetEnv();
void InitEnvironment();  // disables WoW64 redirection, fills Env

struct Target {
    std::string root;    // "C:\"
    std::string winDir;  // "C:\Windows"
    std::string label;
    bool online = false;
    bool valid = false;
    char letter() const { return root.empty() ? 'C' : root[0]; }
};
std::vector<Target> FindTargets();

// ---- results shared between modules (health score, HTML report) -----------
struct DiskInfo {
    int number = -1;
    std::string model, serial, bus, media, partStyle, health = "Healthy", note;
    unsigned long long size = 0;
    int temp = -1, wear = -1;
    long long readErr = -1, writeErr = -1, powerOnHours = -1;
};

struct Section {
    std::string title;
    std::vector<std::pair<std::string, std::string>> rows;
};

struct Results {
    std::vector<DiskInfo> disks;
    bool haveDisk = false, haveIntegrity = false, integrityBad = false, haveDrivers = false;
    int errDevices = 0;
    bool haveBsod = false;
    int bsodEvents = 0;
    bool haveBoot = false;
    int bootIssues = 0;
    std::vector<Section> sections;
    std::string lastReportHtml;
    void setSection(const Section& s) {
        for (auto& x : sections) if (x.title == s.title) { x = s; return; }
        sections.push_back(s);
    }
};

struct CmdParams {
    std::string vol, src, host, win, efi, bcd;
};

struct Options {
    int days = 3;
    bool extended = false;  // ScanHealth + chkdsk /scan
    bool force = false;     // restore point: ignore 24h limit
    std::string host = "www.microsoft.com";
    int port = 443;
    std::string source;     // DISM /Source
    int cmdNo = 0;          // command catalog item to run (ModCommand)
    CmdParams cp;
    std::vector<int> fixIds;  // repair wizard selection (ModFixes)
    std::string bcdStore;     // selected BCD store for catalog commands
    int symptom = 0;          // triage wizard symptom index (ModTriage)
    std::string playbookId;   // playbook to run (ModPlaybook)
};

class Ctx {
public:
    Target target;
    Options opt;
    std::string outDir;  // module output folder (trailing backslash)
    const std::atomic<bool>* cancel = nullptr;
    Results* res = nullptr;

    bool cancelled() const { return cancel && cancel->load(); }
    void info(const std::string& m) const { Logger::I().log(Lvl::Info, m); }
    void warn(const std::string& m) const { Logger::I().log(Lvl::Warn, m); }
    void error(const std::string& m) const { Logger::I().log(Lvl::Error, m); }
    void ok(const std::string& m) const { Logger::I().log(Lvl::Ok, m); }
    ProcResult run(const std::string& title, const std::string& cmd, int timeoutSec = 0, const std::string& input = "") const;
    std::string sub(const std::string& name) const;  // creates outDir\name\ and returns it
    bool needTarget() const;                          // logs an error if no target selected
    bool preflight(bool needAdmin, double minFreeGB) const;
    void saveText(const std::string& file, const std::string& text) const;
};

using ModuleFn = void (*)(Ctx&);

// ---- modules --------------------------------------------------------------
void ModSystem(Ctx&);
void ModMemory(Ctx&);
void ModBoot(Ctx&);
void ModDisk(Ctx&);
void ModDrivers(Ctx&);
void ModEvents(Ctx&);
void ModIntegrity(Ctx&);
void ModRepair(Ctx&);
void ModRestorePoint(Ctx&);
void ModNetwork(Ctx&);
void ModBsod(Ctx&);
void ModDump(Ctx&);
void ModFull(Ctx&);
void ModCommand(Ctx&);  // catalog.cpp
void ModFixes(Ctx&);    // fixes.cpp
void ModTriage(Ctx&);   // triage.cpp
void ModPlaybook(Ctx&); // playbook.cpp

// ---- shared helpers between module files ------------------------------------
std::vector<DiskInfo> CollectDisks();                       // storage.cpp
bool GetDiskLayout(int disk, std::vector<uint8_t>& buf);    // storage.cpp (DRIVE_LAYOUT_INFORMATION_EX)
int VolumeDiskNumber(char letter);                          // storage.cpp
bool GetSmbiosBlob(std::vector<uint8_t>& out);              // system.cpp

struct EvtRow {
    std::string log, time, level, provider, message, xml;
    unsigned id = 0;
};
bool QueryEvents(const Ctx& c, const std::string& logName, const std::string& xpath, size_t maxRows,
                 bool wantMsg, bool wantXml, std::vector<EvtRow>& out, std::string& err);
std::string XpathRecent(int days, const std::string& extraCond);  // events.cpp

std::string FindCdb();                                             // dump.cpp
std::string DumpDirOf(const Target& t);

std::string BuildHtmlReport(const Ctx& c, const Results& r, int score, const std::string& band, const std::vector<std::string>& reasons);

}  // namespace wr
