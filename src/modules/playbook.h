// Playbooks: text scenarios (*.wrp) next to the exe (Playbooks\) and in Data\Playbooks\ — new symptoms and repair procedures without recompiling.
#pragma once
#include "../core/playbook_parse.h"
#include "triage.h"

namespace wr {

void ReloadPlaybooks();
const std::vector<Playbook>& Playbooks();
const std::vector<std::string>& PlaybookErrors();
const Playbook* FindPlaybook(const std::string& id);
std::vector<const Playbook*> PlaybooksOfKind(const std::string& kind);
std::string PlaybookApplicable(const Playbook& pb, const Target& t);  // "" when it can run, otherwise the reason
std::string PlaybookPlan(const Playbook& pb);                          // human-readable list of steps
std::string PlaybookDir();
std::string PlaybookBuildCmd(const std::string& line);                 // resolves the executable (System32 / PATH) and quotes it                                             // Data\Playbooks\ (user playbooks)

}  // namespace wr
