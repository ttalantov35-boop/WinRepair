// Playbook (.wrp) format: INI-like, UTF-8. Sections [playbook] and any number of [step]. Pure C++ (unit-tested on any OS).
#pragma once
#include <map>
#include <string>
#include <vector>
#include "textutil.h"

namespace wr {

struct PbStep {
    std::map<std::string, std::string> kv;
    int line = 0;
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    }
};

struct Playbook {
    std::string id, title, kind = "repair", risk = "changing", needs = "target", description, file;
    std::vector<PbStep> steps;
    bool valid = false;
};

inline std::string pbLower(const std::string& s) { return toLowerUtf8(s); }

inline bool pbKnownType(const std::string& t) {
    static const char* k[] = {"message", "ask", "confirm", "cmd", "catalog", "fix", "module", "probe", "window", "set", "check", "cmdcheck"};
    for (const char* x : k) if (t == x) return true;
    return false;
}

inline bool parsePlaybook(const std::string& text, Playbook& pb, std::vector<std::string>& errors) {
    size_t errStart = errors.size();
    std::map<std::string, std::string> head;
    enum { None, Head, Step } sec = None;
    PbStep cur;
    std::string lastKey;
    auto flush = [&]() { if (sec == Step) pb.steps.push_back(cur); cur = PbStep(); };
    size_t p = 0;
    int ln = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string raw = text.substr(p, e - p);
        p = e + 1;
        ++ln;
        while (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (ln == 1 && raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) raw = raw.substr(3);
        std::string t = trim(raw);
        if (t.empty()) { lastKey.clear(); continue; }
        if (t[0] == '#' || t[0] == ';') continue;
        if (t[0] == '[') {
            size_t r = t.find(']');
            std::string name = r == std::string::npos ? "" : pbLower(trim(t.substr(1, r - 1)));
            flush();
            lastKey.clear();
            if (name == "playbook") sec = Head;
            else if (name == "step") { sec = Step; cur.line = ln; }
            else { sec = None; errors.push_back(fmt("строка %d: неизвестная секция «%s»", ln, t.c_str())); }
            continue;
        }
        bool cont = (raw[0] == ' ' || raw[0] == '\t') && !lastKey.empty() && sec != None;
        if (cont) {
            std::string& v = sec == Head ? head[lastKey] : cur.kv[lastKey];
            v += "\n" + t;
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos || sec == None) { errors.push_back(fmt("строка %d: ожидается «ключ = значение» внутри секции", ln)); continue; }
        std::string key = pbLower(trim(t.substr(0, eq))), val = trim(t.substr(eq + 1));
        if (sec == Head) head[key] = val; else cur.kv[key] = val;
        lastKey = key;
    }
    flush();
    pb.id = pbLower(head["id"]);
    pb.title = head["title"];
    if (!head["kind"].empty()) pb.kind = pbLower(head["kind"]);
    if (!head["risk"].empty()) pb.risk = pbLower(head["risk"]);
    if (!head["needs"].empty()) pb.needs = pbLower(head["needs"]);
    pb.description = head["description"];
    if (pb.id.empty()) errors.push_back("[playbook]: не задан id");
    for (char c : pb.id) if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) { errors.push_back("[playbook]: id допускает буквы, цифры, - и _"); break; }
    if (pb.title.empty()) errors.push_back("[playbook]: не задан title");
    if (pb.kind != "repair" && pb.kind != "triage") errors.push_back("[playbook]: kind = repair или triage");
    if (pb.risk != "safe" && pb.risk != "changing" && pb.risk != "danger") errors.push_back("[playbook]: risk = safe | changing | danger");
    if (pb.needs != "target" && pb.needs != "offline" && pb.needs != "online" && pb.needs != "winpe" && pb.needs != "any") errors.push_back("[playbook]: needs = target | offline | online | winpe | any");
    if (pb.steps.empty()) errors.push_back("нет ни одного [step]");
    auto need = [&](const PbStep& s, const char* key) {
        if (s.get(key).empty()) errors.push_back(fmt("шаг со строки %d (type=%s): не задан «%s»", s.line, s.get("type").c_str(), key));
    };
    for (auto& s : pb.steps) {
        std::string ty = pbLower(s.get("type"));
        s.kv["type"] = ty;
        if (!pbKnownType(ty)) { errors.push_back(fmt("шаг со строки %d: неизвестный type «%s»", s.line, ty.c_str())); continue; }
        if (ty == "message" || ty == "confirm") need(s, "text");
        else if (ty == "ask") { need(s, "var"); need(s, "prompt"); }
        else if (ty == "cmd" || ty == "cmdcheck") need(s, "run");
        else if (ty == "catalog") need(s, "no");
        else if (ty == "fix") need(s, "ids");
        else if (ty == "module" || ty == "probe" || ty == "window") need(s, "name");
        else if (ty == "set") need(s, "var");
        else if (ty == "check") need(s, "check");
        if (pb.kind == "triage" && !(ty == "probe" || ty == "check" || ty == "cmdcheck" || ty == "message"))
            errors.push_back(fmt("шаг со строки %d: в сценарии kind=triage разрешены только probe, check, cmdcheck, message", s.line));
    }
    pb.valid = errors.size() == errStart;
    return pb.valid;
}

// {NAME} -> vars[NAME] (case-insensitive names); unknown tokens stay as they are.
inline std::string expandVars(const std::string& s, const std::map<std::string, std::string>& vars) {
    std::string o;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '{') {
            size_t e = s.find('}', i);
            if (e != std::string::npos && e - i <= 40) {
                std::string name = s.substr(i + 1, e - i - 1), up;
                for (char c : name) up += (char)toupper((unsigned char)c);
                auto it = vars.find(up);
                if (it != vars.end()) { o += it->second; i = e + 1; continue; }
            }
        }
        o += s[i++];
    }
    return o;
}

}  // namespace wr
