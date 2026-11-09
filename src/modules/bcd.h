// BCD store discovery (incl. temporary mounting of unlettered ESP), editing helpers, boot modes, step runner.
#pragma once
#include "../win/offreg.h"
#include "../core/bcdparse.h"

namespace wr {

struct BcdStore { std::string path, label; };  // path empty = default store of the running system

struct Step {
    std::string title, cmd, input;
    int timeoutSec = 120;
};

std::vector<BcdStore> FindBcdStores();   // scans letters; temporarily mounts ESP/System-Reserved volumes without letters
void UnmountTempBcdVolumes();
char TempMountVolume(const std::wstring& volumeGuidPath);  // assigns a free letter (0 on failure); undone by UnmountTempBcdVolumes
void TempUnmountVolume(char letter);
std::string BcdExe();
std::string BcdPrefix(const std::string& store);                      // "\"bcdedit.exe\" /store \"X\""
bool BcdRunSync(const std::string& store, const std::string& args, std::string& out, DWORD* code = nullptr);
std::vector<BcdEntry> BcdEnum(const std::string& store, std::string& raw);
std::string BcdBackup(const std::string& store, std::string& err);    // file copy of the BCD hive into Data\Backups
bool BcdRestore(const std::string& store, const std::string& backup, std::string& err);

struct BootMode {
    const char* title;
    std::vector<std::pair<const char*, const char*>> sets;  // bcdedit /set {id} <element> <value>
    std::vector<const char*> deletes;                         // bcdedit /deletevalue {id} <element>
};
const std::vector<BootMode>& BootModes();
std::vector<Step> BcdModeSteps(const std::string& store, const std::string& id, int modeIndex);
std::vector<Step> BcdResetModesSteps(const std::string& store, const std::string& id);

void RunSteps(Ctx& c, const std::vector<Step>& steps);  // sequentially, journaled, audited

}  // namespace wr
