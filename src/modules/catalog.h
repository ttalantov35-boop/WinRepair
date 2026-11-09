// Command catalogue (55 items): console tools, GUI tools, native PowerShell replacements, manual procedures.
#pragma once
#include "../win/app.h"

namespace wr {

enum class CKind { Exec, Launch, Native, Manual, ViewFile };
enum class Risk { Safe, Changing, Danger };
enum : unsigned { P_VOL = 1, P_SRC = 2, P_HOST = 4, P_WIN = 8, P_EFI = 16, P_BCD = 32 };

struct OutBuf {  // collects text of a native command (log + saved file)
    std::string text;
    void add(const std::string& l) {
        Logger::I().log(Lvl::Out, "  | " + l);
        text += l + "\r\n";
    }
};
using NativeFn = void (*)(Ctx&, OutBuf&);

struct CmdItem {
    int no;
    const char* cat;
    const char* title;     // text shown to the user
    CKind kind;
    const char* tmpl;      // Exec: command template; Launch: "exe|args"; ViewFile: path under %WinDir%; Manual: instruction text
    unsigned params;       // P_* bitmask
    Risk risk;
    int timeoutSec;
    const char* input;     // text fed to stdin (answers to prompts)
    const char* script;    // content of a temporary script file ({SCRIPT} token)
    const char* note;
    NativeFn native;
};

const std::vector<CmdItem>& Catalog();
const CmdItem* FindCmd(int no);
bool CmdValidate(const CmdItem& it, const CmdParams& p, std::string& err);
// Exact command line / description for the confirmation dialog and journal.
std::string CmdBuild(const CmdItem& it, const CmdParams& p, const Target& t, const std::string& scratchDir, const std::string& scriptPath);
const char* RiskText(Risk r);
const char* KindText(CKind k);

// native replacements for PowerShell cmdlets (native_ps.cpp)
void NatGetProcess(Ctx&, OutBuf&);
void NatGetProcessCpu(Ctx&, OutBuf&);
void NatGetService(Ctx&, OutBuf&);
void NatOperatingSystem(Ctx&, OutBuf&);
void NatEventLogSystem(Ctx&, OutBuf&);
void NatNetAdapter(Ctx&, OutBuf&);
void NatNetIPConfiguration(Ctx&, OutBuf&);
void NatGetDisk(Ctx&, OutBuf&);
void NatPhysicalDisk(Ctx&, OutBuf&);
void NatGetVolume(Ctx&, OutBuf&);
void NatPnpDevice(Ctx&, OutBuf&);
void NatRestorePoints(Ctx&, OutBuf&);
void NatSecureBoot(Ctx&, OutBuf&);
void NatNoDriverDevices(Ctx&, OutBuf&);
void NatWinReInfo(Ctx&, OutBuf&);

// warning when a command describes the running environment (e.g. WinPE) instead of the selected target OS
std::string ScopeWarning(const CmdItem& it, const Target& t);

}  // namespace wr
