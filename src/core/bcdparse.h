// Parser for `bcdedit /enum` output (locale independent: relies on layout, not on words).
#pragma once
#include <string>
#include <vector>
#include "textutil.h"

namespace wr {

struct BcdProp { std::string key, value; };
struct BcdEntry {
    std::string heading;
    std::vector<BcdProp> props;
    std::string id() const { return props.empty() ? "" : props[0].value; }
    std::string get(const std::string& k) const {
        std::string lk = toLowerUtf8(k);
        for (const auto& p : props) if (toLowerUtf8(p.key) == lk) return p.value;
        return "";
    }
    std::string desc() const {
        std::string d = get("description");
        return d.empty() ? get("описание") : d;
    }
    std::string label() const {
        std::string d = desc();
        return heading + (d.empty() ? "" : " — " + d) + "  " + id();
    }
};

inline bool isDashLine(const std::string& s) {
    std::string t = trim(s);
    if (t.size() < 3) return false;
    for (char c : t) if (c != '-') return false;
    return true;
}

inline std::vector<BcdEntry> parseBcdEnum(const std::string& text) {
    std::vector<std::string> lines;
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(p, e - p);
        while (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        p = e + 1;
    }
    std::vector<BcdEntry> out;
    std::string lastNonEmpty;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (isDashLine(l) && !lastNonEmpty.empty()) {
            BcdEntry en;
            en.heading = trim(lastNonEmpty);
            ++i;
            for (; i < lines.size() && !trim(lines[i]).empty(); ++i) {
                const std::string& pl = lines[i];
                if (pl[0] == ' ' || pl[0] == '\t') {  // continuation of a multi-line value
                    if (!en.props.empty()) en.props.back().value += (en.props.back().value.empty() ? "" : ", ") + trim(pl);
                    continue;
                }
                size_t k = 0;
                while (k < pl.size()) {
                    if (pl[k] == '\t') break;
                    if (pl[k] == ' ' && k + 1 < pl.size() && pl[k + 1] == ' ') break;
                    ++k;
                }
                BcdProp pr;
                pr.key = trim(pl.substr(0, k));
                pr.value = k < pl.size() ? trim(pl.substr(k)) : "";
                en.props.push_back(pr);
            }
            out.push_back(en);
            lastNonEmpty.clear();
            continue;
        }
        if (!trim(l).empty()) lastNonEmpty = l;
    }
    return out;
}

}  // namespace wr
