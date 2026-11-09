// Crash dump analysis: built-in header parser (works anywhere) + optional cdb.exe !analyze -v.
#include "../win/app.h"
#include "../core/bsod_catalog.h"
#include "../core/dumpheader.h"

namespace wr {

std::string DumpDirOf(const Target& t) { return t.winDir; }

std::string FindCdb() {
    std::vector<std::string> cand;
    std::string ed = ExeDir();
    cand.push_back(ed + "Tools\\cdb.exe");
    cand.push_back(ed + "Tools\\x64\\cdb.exe");
    cand.push_back(ed + "Tools\\x86\\cdb.exe");
    for (const char* pf : {"ProgramFiles(x86)", "ProgramFiles"}) {
        std::string p = EnvVar(pf);
        if (p.empty()) continue;
        cand.push_back(p + "\\Windows Kits\\10\\Debuggers\\x64\\cdb.exe");
        cand.push_back(p + "\\Windows Kits\\10\\Debuggers\\x86\\cdb.exe");
    }
    cand.push_back(EnvVar("LOCALAPPDATA") + "\\Microsoft\\WindowsApps\\cdb.exe");
    for (const auto& p : cand) if (PathExists(p)) return p;
    return FindInPath("cdb.exe");
}

void ModDump(Ctx& c) {
    c.info("=== Разбор дампов ===");
    if (!c.needTarget()) return;
    std::string wd = DumpDirOf(c.target);
    std::vector<std::string> dumps;
    for (const auto& f : ListFiles(wd + "\\Minidump\\", "*.dmp")) dumps.push_back(wd + "\\Minidump\\" + f);
    if (PathExists(wd + "\\MEMORY.DMP")) dumps.push_back(wd + "\\MEMORY.DMP");
    if (dumps.empty()) { c.info("Дампы не найдены."); return; }

    std::string cdb = FindCdb();
    std::string sym = DataDir() + "Symbols";
    if (!cdb.empty()) {
        MakeDirs(sym + "\\");
        SetEnvironmentVariableW(L"_NT_SYMBOL_PATH", W("srv*" + sym + "*https://msdl.microsoft.com/download/symbols").c_str());
        c.info("cdb: " + cdb);
    } else {
        c.warn("cdb.exe не найден (положите Debugging Tools в Tools\\ рядом с программой или установите WinDbg/Windows SDK). "
               "Выполняется только быстрый разбор заголовка дампа.");
    }

    Json res = Json::array();
    Section sec;
    sec.title = "Дампы";
    for (const auto& d : dumps) {
        if (c.cancelled()) break;
        Json o = Json::object();
        o.set("DumpPath", d);
        std::vector<uint8_t> head;
        ReadBytes(d, head, 0x1000);
        DumpInfo di = parseDumpHeader(head.data(), head.size());
        std::string name = d.substr(d.find_last_of('\\') + 1);
        if (di.ok) {
            const BsodEntry* ce = bsodByCode(di.bugcheck);
            c.info(fmt("%s: %s, BugCheck 0x%X %s | P1=0x%llX P2=0x%llX P3=0x%llX P4=0x%llX", name.c_str(), di.kind.c_str(), di.bugcheck,
                       ce ? ce->name : "", (unsigned long long)di.param[0], (unsigned long long)di.param[1], (unsigned long long)di.param[2], (unsigned long long)di.param[3]));
            if (ce) c.info(fmt("   категория: %s; с чего начать: %s; %s", ce->category, ce->firstStep, ce->note));
            o.set("HeaderOk", true).set("Kind", di.kind).set("BugCheckCode", fmt("0x%X", di.bugcheck)).set("BugCheckName", ce ? ce->name : "");
            sec.rows.push_back({name, fmt("0x%X %s", di.bugcheck, ce ? ce->name : "")});
        } else {
            c.warn(name + ": заголовок не распознан — " + di.error);
            o.set("HeaderOk", false).set("Error", di.error);
        }
        if (!cdb.empty()) {
            std::string out = c.sub("dump") + name + ".analyze.txt";
            std::string cmd = "\"" + cdb + "\" -z \"" + d + "\" -c \"!analyze -v;q\"";
            ProcResult r = c.run("cdb !analyze -v: " + name, cmd, 180);
            WriteText(out, r.output);
            std::string code = fieldValue(r.output, "BUGCHECK_CODE:");
            std::string bname = fieldValue(r.output, "BUGCHECK_STR:");
            std::string mod = fieldValue(r.output, "MODULE_NAME:");
            if (mod.empty()) mod = fieldValue(r.output, "IMAGE_NAME:");
            std::string bucket = fieldValue(r.output, "FAILURE_BUCKET_ID:");
            o.set("AnalyzeLog", out).set("Module", mod).set("FailureBucket", bucket).set("Success", !code.empty());
            if (!code.empty()) {
                c.ok(fmt("  %s: код %s, модуль: %s, bucket: %s", name.c_str(), code.c_str(), mod.empty() ? "?" : mod.c_str(), bucket.c_str()));
                sec.rows.push_back({name + " — модуль", mod.empty() ? "?" : mod});
            } else c.warn("  cdb не вернул BUGCHECK_CODE (нет символов/сети или повреждённый дамп). Лог: " + out);
        }
        res.add(o);
    }
    c.saveText("dump-analysis.json", res.dump());
    if (c.res) c.res->setSection(sec);
}

}  // namespace wr
