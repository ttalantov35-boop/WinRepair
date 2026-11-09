// Dictionary-based translation at the output boundary. Keys are the original (Russian) texts; "{}" is a wildcard that captures run-time values.
// File format (UTF-8): `русский текст ==> English text`, # comments; escapes \n \r \t \\ . Pure C++ — unit-tested on any OS.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace wr {

class Dict {
public:
    static std::string unescape(const std::string& s) {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char n = s[++i];
                o += n == 'n' ? '\n' : n == 'r' ? '\r' : n == 't' ? '\t' : n;
            } else o += s[i];
        }
        return o;
    }

    size_t load(const std::string& text) {
        size_t added = 0, p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(p, e - p);
            p = e + 1;
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line = line.substr(3);
            if (line.empty() || line[0] == '#') continue;
            size_t sep = line.find(" ==> ");
            if (sep == std::string::npos) continue;
            std::string ru = unescape(line.substr(0, sep)), en = unescape(line.substr(sep + 5));
            if (en.empty()) continue;
            if (ru.find("{}") == std::string::npos) exact_[ru] = en;
            else {
                Tmpl t;
                t.ru = split(ru);
                t.en = split(en);
                for (auto& x : t.ru) t.lit += x.size();
                tmpl_.push_back(t);
            }
            ++added;
        }
        std::stable_sort(tmpl_.begin(), tmpl_.end(), [](const Tmpl& a, const Tmpl& b) { return a.lit > b.lit; });
        return added;
    }

    size_t size() const { return exact_.size() + tmpl_.size(); }

    static bool hasCyrillic(const std::string& s) {
        for (size_t i = 0; i + 1 < s.size(); ++i) { unsigned char c = (unsigned char)s[i]; if (c == 0xD0 || c == 0xD1) return true; }
        return false;
    }

    std::string tr(const std::string& s, int depth = 0) const {
        if (s.empty() || !hasCyrillic(s) || depth > 3) return s;
        auto it = exact_.find(s);
        if (it != exact_.end()) return it->second;
        for (const auto& t : tmpl_) {
            std::vector<std::string> caps;
            if (!match(t.ru, s, caps)) continue;
            if (t.lit < 5) {  // very short templates may only capture numbers / ASCII values, otherwise they would mangle unrelated text
                bool cyr = false;
                for (auto& c : caps) if (hasCyrillic(c)) cyr = true;
                if (cyr) continue;
            }
            std::string o;
            size_t ci = 0;
            for (size_t i = 0; i < t.en.size(); ++i) {
                o += t.en[i];
                if (i + 1 < t.en.size()) o += ci < caps.size() ? tr(caps[ci++], depth + 1) : "";
            }
            return o;
        }
        return s;
    }

private:
    struct Tmpl { std::vector<std::string> ru, en; size_t lit = 0; };
    std::unordered_map<std::string, std::string> exact_;
    std::vector<Tmpl> tmpl_;

    static std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> v;
        size_t p = 0;
        while (true) {
            size_t f = s.find("{}", p);
            if (f == std::string::npos) { v.push_back(s.substr(p)); break; }
            v.push_back(s.substr(p, f - p));
            p = f + 2;
        }
        return v;
    }

    static bool match(const std::vector<std::string>& parts, const std::string& s, std::vector<std::string>& caps) {
        size_t n = parts.size();
        if (n < 2) return false;
        if (s.compare(0, parts[0].size(), parts[0]) != 0) return false;
        size_t pos = parts[0].size();
        for (size_t i = 1; i + 1 < n; ++i) {
            size_t f = s.find(parts[i], pos);
            if (f == std::string::npos) return false;
            caps.push_back(s.substr(pos, f - pos));
            pos = f + parts[i].size();
        }
        const std::string& suf = parts[n - 1];
        if (s.size() < pos + suf.size() || s.compare(s.size() - suf.size(), suf.size(), suf) != 0) return false;
        caps.push_back(s.substr(pos, s.size() - suf.size() - pos));
        return true;
    }
};

}  // namespace wr
