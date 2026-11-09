// "Что случилось?" wizard: symptom -> automatic probes -> ranked findings -> recommended actions.
#pragma once
#include "fixes.h"
#include "access.h"

namespace wr {

enum class Sev { Info = 0, Warn = 1, Bad = 2 };

struct TAct {
    enum K { None, Catalog, Fixes, Window, Module, Playbook } k = None;
    int no = 0;                // catalog command number
    std::vector<int> ids;      // repair wizard fixes
    std::string name;          // window / module name
    std::string label;         // text of the recommended action
};

struct Finding {
    Sev sev = Sev::Info;
    std::string title, detail;
    TAct act;
};

bool RunProbeByName(Ctx& c, const std::string& name);   // built-in probes usable from playbooks
TAct ParseActionSpec(const std::string& spec, const std::string& label);
int SymptomCount();
const char* SymptomName(int i);
const std::vector<Finding>& TriageFindings();

}  // namespace wr
