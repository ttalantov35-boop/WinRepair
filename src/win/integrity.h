// Self-integrity check: SHA-256 of files against MANIFEST.sha256 + Authenticode signature state of the exe.
#pragma once
#include <string>
#include <vector>
#include "common.h"

namespace wr {

enum class SigState { Unsigned, Valid, Invalid, Unknown };

struct IntegrityReport {
    bool manifestFound = false;
    int checked = 0;
    std::vector<std::string> mismatched;   // executables (.exe/.dll/.sys) whose hash differs - real tampering
    std::vector<std::string> modifiedData; // editable data (Playbooks, Lang, docs) that differ - normal for user edits
    std::vector<std::string> missing;      // listed in the manifest but absent
    bool selfListed = false, selfOk = false;
    std::string selfHash;
    SigState sig = SigState::Unknown;
    // Tampering = a hash really differs, or the signature is present but broken.
    bool tampered() const { return !mismatched.empty() || sig == SigState::Invalid; }
};

IntegrityReport RunIntegrityCheck();
std::string IntegritySummary(const IntegrityReport& r);   // one line for the log
std::string FileSha256(const std::string& utf8Path);      // "" on read error

}  // namespace wr
