// Offline Autoruns / msconfig engine: scan, disable/enable (reversible), delete, edit command.
#pragma once
#include "../win/offreg.h"

namespace wr {

enum class AOp { ReadOnly, RunValue, Service, StartupFile, Ifeo, MoveValue, EditOnly };

struct AutoItem {
    std::string kind, name, command, state, file, location, flag;
    bool fileExists = false;
    bool thirdParty = false;
    AOp op = AOp::ReadOnly;
    std::string mount, sub, value, approvedSub, path;  // where it lives
    bool once = false;
    DWORD origStart = 0;
};

std::string ResolveOfflinePath(const std::string& cmd, const Target& t, const std::string& profile);
std::vector<AutoItem> ScanAutoruns(const Target& t, std::string& err);
bool AutorunSetEnabled(const AutoItem& it, bool enable, std::string& err);
bool AutorunDelete(const AutoItem& it, std::string& err);
bool AutorunSetCommand(const AutoItem& it, const std::string& cmd, std::string& err);
std::string AutorunsCsv(const std::vector<AutoItem>& v);

}  // namespace wr
