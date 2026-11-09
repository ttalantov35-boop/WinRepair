// Minimal ordered JSON builder (write-only).
#pragma once
#include <cstdio>
#include <string>
#include <vector>

namespace wr {

class Json {
public:
    enum Type { Null, Bool, Int, Real, Str, Arr, Obj };
    Json() {}
    Json(bool v) : t(Bool), i(v ? 1 : 0) {}
    Json(int v) : t(Int), i(v) {}
    Json(unsigned v) : t(Int), i((long long)v) {}
    Json(long v) : t(Int), i(v) {}
    Json(unsigned long v) : t(Int), i((long long)v) {}
    Json(long long v) : t(Int), i(v) {}
    Json(unsigned long long v) : t(Int), i((long long)v) {}
    Json(double v) : t(Real), d(v) {}
    Json(const char* v) : t(Str), s(v ? v : "") {}
    Json(const std::string& v) : t(Str), s(v) {}

    static Json array() { Json j; j.t = Arr; return j; }
    static Json object() { Json j; j.t = Obj; return j; }

    Json& add(const Json& v) { if (t != Arr) t = Arr; a.push_back(v); return *this; }
    Json& set(const std::string& k, const Json& v) {
        if (t != Obj) t = Obj;
        for (size_t n = 0; n < keys.size(); ++n)
            if (keys[n] == k) { a[n] = v; return *this; }
        keys.push_back(k);
        a.push_back(v);
        return *this;
    }
    size_t size() const { return a.size(); }
    Type type() const { return t; }

    std::string dump(int indent = 2) const {
        std::string o;
        write(o, indent, 0);
        o += "\n";
        return o;
    }

private:
    Type t = Null;
    long long i = 0;
    double d = 0;
    std::string s;
    std::vector<std::string> keys;  // for objects: parallel to `a`
    std::vector<Json> a;

    static void esc(std::string& o, const std::string& v) {
        o += '"';
        for (unsigned char c : v) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                    else o += (char)c;
            }
        }
        o += '"';
    }
    static void pad(std::string& o, int indent, int level) {
        if (indent > 0) { o += '\n'; o.append((size_t)(indent * level), ' '); }
    }
    void write(std::string& o, int indent, int level) const {
        switch (t) {
            case Null: o += "null"; break;
            case Bool: o += i ? "true" : "false"; break;
            case Int: { char b[32]; snprintf(b, sizeof b, "%lld", i); o += b; break; }
            case Real: { char b[40]; snprintf(b, sizeof b, "%g", d); o += b; break; }
            case Str: esc(o, s); break;
            case Arr:
                if (a.empty()) { o += "[]"; break; }
                o += '[';
                for (size_t n = 0; n < a.size(); ++n) {
                    if (n) o += ',';
                    pad(o, indent, level + 1);
                    a[n].write(o, indent, level + 1);
                }
                pad(o, indent, level);
                o += ']';
                break;
            case Obj:
                if (a.empty()) { o += "{}"; break; }
                o += '{';
                for (size_t n = 0; n < a.size(); ++n) {
                    if (n) o += ',';
                    pad(o, indent, level + 1);
                    esc(o, keys[n]);
                    o += indent > 0 ? ": " : ":";
                    a[n].write(o, indent, level + 1);
                }
                pad(o, indent, level);
                o += '}';
                break;
        }
    }
};

}  // namespace wr
