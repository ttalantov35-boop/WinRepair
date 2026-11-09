#include "playbook.h"
#include "../ui_tools.h"

namespace wr {

namespace {

std::vector<Playbook> g_pbs;
std::vector<std::string> g_errs;

struct ModEntry { const char* name; ModuleFn fn; };
const ModEntry kMods[] = {{"system", ModSystem}, {"memory", ModMemory}, {"boot", ModBoot}, {"disk", ModDisk}, {"drivers", ModDrivers}, {"events", ModEvents},
                          {"integrity", ModIntegrity}, {"bsod", ModBsod}, {"dump", ModDump}, {"network", ModNetwork}};

void LoadText(const std::vector<uint8_t>& raw, const std::string& name, const std::string& file) {
    std::string text = (raw.size() > 2 && raw[0] == 0xFF && raw[1] == 0xFE) ? utf16leToUtf8(raw.data(), raw.size()) : std::string((const char*)raw.data(), raw.size());
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) text.erase(0, 3);
    Playbook pb;
    std::vector<std::string> errs;
    parsePlaybook(text, pb, errs);
    pb.file = file;
    for (auto& e : errs) g_errs.push_back(name + ": " + e);
    if (!pb.valid) return;
    bool replaced = false;
    for (auto& x : g_pbs) if (x.id == pb.id) { x = pb; replaced = true; }  // a later source (disk) overrides an earlier one (built in)
    if (!replaced) g_pbs.push_back(pb);
}

// Returns the number of files read.
int LoadDir(const std::string& dir) {
    int n = 0;
    for (const auto& f : ListFiles(dir, "*.wrp")) {
        std::vector<uint8_t> raw;
        if (!ReadBytes(dir + f, raw, 1 << 20) || raw.empty()) continue;
        LoadText(raw, f, dir + f);
        n++;
    }
    return n;
}

// Playbooks compiled into the exe (RCDATA ids 100..199) - the program works as a single file even without a Playbooks folder.
int LoadBuiltIn() {
    int n = 0;
    for (int id = 100; id < 200; id++) {
        HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!r) continue;
        HGLOBAL g = LoadResource(nullptr, r);
        const void* d = g ? LockResource(g) : nullptr;
        DWORD sz = SizeofResource(nullptr, r);
        if (!d || !sz) continue;
        std::vector<uint8_t> raw((const uint8_t*)d, (const uint8_t*)d + sz);
        LoadText(raw, fmt("[встроенный #%d]", id), "[встроен в программу]");
        n++;
    }
    return n;
}

std::string Resolve(const std::string& name) {
    std::string n = name;
    if (n.find('.') == std::string::npos) n += ".exe";
    std::string p = SysDir() + "\\" + n;
    if (PathExists(p)) return p;
    std::string f = FindInPath(n.c_str());
    return f.empty() ? name : f;
}

std::string BuildCmd(const std::string& line) {
    std::string s = trim(line);
    if (s.empty()) return s;
    std::string exe, rest;
    if (s[0] == '"') { size_t e = s.find('"', 1); exe = s.substr(1, e == std::string::npos ? std::string::npos : e - 1); rest = e == std::string::npos ? "" : s.substr(e + 1); }
    else { size_t sp = s.find(' '); exe = s.substr(0, sp); rest = sp == std::string::npos ? "" : s.substr(sp); }
    if (exe.find('\\') == std::string::npos) exe = Resolve(exe);
    return "\"" + exe + "\"" + rest;
}

std::vector<int> Ids(const std::string& s) {
    std::vector<int> v;
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find_first_of(", ;", p);
        if (e == std::string::npos) e = s.size();
        std::string t = trim(s.substr(p, e - p));
        if (!t.empty()) v.push_back(atoi(t.c_str()));
        p = e + 1;
    }
    return v;
}

std::vector<std::string> Lines(const std::string& s) {
    std::vector<std::string> v;
    size_t p = 0;
    while (p <= s.size()) {
        size_t e = s.find('\n', p);
        if (e == std::string::npos) e = s.size();
        v.push_back(s.substr(p, e - p));
        p = e + 1;
    }
    return v;
}

std::string DefaultRisk(const std::string& type) { return (type == "cmd" || type == "catalog" || type == "fix") ? "changing" : "safe"; }

}  // namespace

std::string PlaybookDir() { return DataDir() + "Playbooks\\"; }
std::string PlaybookBuildCmd(const std::string& line) { return BuildCmd(line); }

void ReloadPlaybooks() {
    g_pbs.clear();
    g_errs.clear();
    int b = LoadBuiltIn();
    // Folders are searched in order; later ones override earlier ones by playbook id.
    // "..\\Playbooks" covers running build\\WinRepair.exe straight from the source tree.
    int e1 = LoadDir(ExeDir() + "..\\Playbooks\\");
    int e2 = LoadDir(ExeDir() + "Playbooks\\");
    MakeDirs(PlaybookDir());
    int e3 = LoadDir(PlaybookDir());
    Logger::I().log(Lvl::Info, fmt("Сценарии (playbooks): всего %zu (встроенных файлов %d, с диска: %d; ошибок разбора %zu)", g_pbs.size(), b, e1 + e2 + e3, g_errs.size()));
    for (auto& e : g_errs) Logger::I().log(Lvl::Warn, "Сценарий: " + e);
}

const std::vector<Playbook>& Playbooks() { return g_pbs; }
const std::vector<std::string>& PlaybookErrors() { return g_errs; }

const Playbook* FindPlaybook(const std::string& id) {
    for (auto& p : g_pbs) if (p.id == id) return &p;
    return nullptr;
}

std::vector<const Playbook*> PlaybooksOfKind(const std::string& kind) {
    std::vector<const Playbook*> v;
    for (auto& p : g_pbs) if (p.kind == kind) v.push_back(&p);
    return v;
}

std::string PlaybookApplicable(const Playbook& pb, const Target& t) {
    const Env& e = GetEnv();
    if (pb.needs == "any") return "";
    if (pb.needs == "winpe") return (e.isWinPE) ? "" : "Сценарий работает только внутри WinPE/WinRE.";
    if (!t.valid) return "Выберите целевую ОС.";
    if (pb.needs == "offline" && t.online) return "Сценарий для офлайн-системы (WinPE/WinRE или другой диск), а выбрана работающая ОС.";
    if (pb.needs == "online" && !t.online) return "Сценарий для работающей Windows, а выбрана офлайн-система.";
    return "";
}

std::string PlaybookPlan(const Playbook& pb) {
    std::string s;
    int n = 0;
    for (const auto& st : pb.steps) {
        ++n;
        std::string ty = st.get("type"), risk = st.get("risk", DefaultRisk(ty));
        std::string what = st.get("title");
        if (what.empty()) what = ty == "cmd" || ty == "cmdcheck" ? st.get("run") : ty == "catalog" ? "команда каталога №" + st.get("no") : ty == "fix" ? "исправления " + st.get("ids") : ty == "message" ? truncUtf8(st.get("text"), 70) : st.get("name", st.get("check", st.get("prompt", st.get("text"))));
        s += fmt("%2d. [%s] %s: %s\r\n", n, Tr(risk == "danger" ? "ОПАСНО" : risk == "changing" ? "изменяет" : "чтение").c_str(), ty.c_str(), truncUtf8(what, 110).c_str());
    }
    return s;
}

void ModPlaybook(Ctx& c) {
    const Playbook* pb = FindPlaybook(c.opt.playbookId);
    if (!pb) { c.error("Сценарий не найден: " + c.opt.playbookId); return; }
    std::string why = PlaybookApplicable(*pb, c.target);
    if (!why.empty()) { c.error(why); return; }
    c.info("=== Сценарий: " + pb->title + " (" + pb->file + ") ===");
    AuditChange("PLAYBOOK START " + pb->id + " (target " + (c.target.valid ? c.target.root : std::string("-")) + ")");
    std::map<std::string, std::string> vars;
    std::string scratch = c.sub("scratch");
    scratch.pop_back();
    vars["TARGET"] = c.target.valid ? c.target.root : "";
    vars["VOL"] = c.target.valid ? c.target.root.substr(0, 2) : "";
    vars["WIN"] = c.target.valid ? c.target.winDir : "";
    vars["SYS32"] = SysDir();
    vars["DATA"] = DataDir();
    vars["REPORT"] = c.outDir;
    vars["SCRATCH"] = scratch;
    vars["STAMP"] = Stamp();
    vars["LAST"] = "0";
    Hives().pin();
    struct Unpin { ~Unpin() { Hives().unpin(); } } unpin;
    int idx = 0, okc = 0, failc = 0;
    bool lastOk = true, stop = false;
    for (const auto& st : pb->steps) {
        ++idx;
        if (c.cancelled()) { c.warn("Прервано пользователем."); break; }
        std::string when = st.get("when");
        if ((when == "ok" && !lastOk) || (when == "failed" && lastOk)) { c.info(fmt("Шаг %d пропущен (условие when=%s)", idx, when.c_str())); continue; }
        std::string type = st.get("type"), title = expandVars(st.get("title", type), vars);
        std::string risk = st.get("risk", DefaultRisk(type));
        c.info(fmt("--- Шаг %d/%zu: %s ---", idx, pb->steps.size(), title.c_str()));
        std::string cmdLine = (type == "cmd" || type == "cmdcheck") ? BuildCmd(expandVars(st.get("run"), vars)) : "";
        bool needConfirm = risk != "safe" && st.get("confirm") != "no";
        if (needConfirm) {
            std::string q = "Сценарий: " + pb->title + "\nШаг " + std::to_string(idx) + ": " + title;
            if (!cmdLine.empty()) q += "\n\n" + cmdLine;
            if (type == "fix") q += "\n\nИсправления: " + st.get("ids");
            if (type == "catalog") q += "\n\nКоманда каталога №" + st.get("no");
            q += "\n\nУровень: " + std::string(risk == "danger" ? "ОПАСНО" : "изменяет систему") + ". Выполнить шаг?";
            if (!UiConfirm(W(q), risk == "danger" ? 2 : 1)) { c.warn("Шаг отклонён — сценарий остановлен."); stop = true; break; }
        }
        bool ok = true;
        if (type == "message") {
            for (auto& l : Lines(expandVars(st.get("text"), vars))) c.info(l);
        } else if (type == "confirm") {
            if (!UiConfirm(W(expandVars(st.get("text"), vars)), risk == "danger" ? 2 : 0)) { c.warn("Подтверждение не получено — сценарий остановлен."); stop = true; break; }
        } else if (type == "ask") {
            std::string out;
            std::string var = pbLower(st.get("var"));
            for (auto& ch : var) ch = (char)toupper((unsigned char)ch);
            if (!UiAsk(W(pb->title), W(expandVars(st.get("prompt"), vars)), W(expandVars(st.get("default"), vars)), out)) { c.warn("Ввод отменён — сценарий остановлен."); stop = true; break; }
            vars[var] = trim(out);
        } else if (type == "set") {
            std::string var = st.get("var");
            for (auto& ch : var) ch = (char)toupper((unsigned char)ch);
            vars[var] = expandVars(st.get("value"), vars);
        } else if (type == "cmd") {
            std::string codes = "," + st.get("ok_codes", "0") + ",";
            ProcResult r = c.run(title, cmdLine, atoi(st.get("timeout", "0").c_str()), expandVars(st.get("input"), vars));
            vars["LAST"] = std::to_string((long long)(int)r.exitCode);
            ok = r.started && !r.cancelled && !r.timedOut && codes.find("," + std::to_string((long long)(int)r.exitCode) + ",") != std::string::npos;
            AuditChange(fmt("PLAYBOOK %s шаг %d cmd -> %s (exit %lu)", pb->id.c_str(), idx, ok ? "ok" : "сбой", (unsigned long)r.exitCode));
        } else if (type == "cmdcheck") {
            ProcResult r = c.run(title, cmdLine, atoi(st.get("timeout", "60").c_str()));
            std::string needle = toLowerUtf8(expandVars(st.get("contains"), vars)), out = toLowerUtf8(r.output);
            bool found = !needle.empty() && out.find(needle) != std::string::npos;
            ok = true;
            (found ? c.warn(title + ": признак найден") : c.info(title + ": признак не найден"));
        } else if (type == "catalog") {
            int no = atoi(st.get("no").c_str());
            Ctx sc = c;
            sc.opt.cmdNo = no;
            sc.opt.cp.vol = expandVars(st.get("vol", "{VOL}"), vars);
            sc.opt.cp.win = expandVars(st.get("win", "{WIN}"), vars);
            sc.opt.cp.src = expandVars(st.get("src"), vars);
            sc.opt.cp.host = expandVars(st.get("host", c.opt.host), vars);
            sc.opt.cp.efi = expandVars(st.get("efi"), vars);
            sc.opt.cp.bcd = expandVars(st.get("bcd"), vars);
            sc.outDir = c.outDir + fmt("step%02d\\", idx);
            MakeDirs(sc.outDir);
            if (!FindCmd(no)) { c.error(fmt("Команда каталога №%d не найдена.", no)); ok = false; }
            else ModCommand(sc);
        } else if (type == "fix") {
            Ctx sc = c;
            sc.opt.fixIds = Ids(expandVars(st.get("ids"), vars));
            ModFixes(sc);
        } else if (type == "module") {
            std::string name = pbLower(st.get("name"));
            bool found = false;
            for (auto& m : kMods) if (name == m.name) {
                Ctx sc = c;
                sc.outDir = c.outDir + fmt("step%02d_%s\\", idx, m.name);
                MakeDirs(sc.outDir);
                m.fn(sc);
                found = true;
            }
            if (!found) { c.error("Неизвестный модуль: " + name); ok = false; }
        } else if (type == "probe") {
            if (!RunProbeByName(c, pbLower(st.get("name")))) { c.error("Неизвестная проверка: " + st.get("name")); ok = false; }
        } else if (type == "window") {
            UiOpenWindow(pbLower(st.get("name")));
            c.info("Открыто окно «" + st.get("name") + "». Завершите работу в нём и подтвердите следующий шаг.");
        } else if (type == "check") {
            c.warn("Шаг check работает в сценариях kind=triage (мастер «Что случилось?»).");
        }
        lastOk = ok;
        if (ok) ++okc; else ++failc;
        if (!ok && st.get("onfail", "stop") == "stop") { c.error(fmt("Шаг %d не удался — сценарий остановлен.", idx)); stop = true; break; }
    }
    AuditChange(fmt("PLAYBOOK END %s: шагов ok=%d, сбоев=%d%s", pb->id.c_str(), okc, failc, stop ? ", остановлен" : ""));
    if (failc == 0 && !stop) c.ok(fmt("Сценарий завершён: выполнено шагов — %d.", okc));
    else c.warn(fmt("Сценарий завершён с остановкой/сбоями: ok=%d, сбоев=%d.", okc, failc));
}

}  // namespace wr
