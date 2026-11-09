// Repair wizard ("SmartFix"-like): reversible, journaled fixes working online and on an offline Windows.
#pragma once
#include "../win/offreg.h"
#include "catalog.h"

namespace wr {

struct FixDef {
    int id;
    const char* title;
    const char* desc;
    Risk risk;
    bool online, offline;
    void (*fn)(Ctx&);
};
const std::vector<FixDef>& FixList();

}  // namespace wr
