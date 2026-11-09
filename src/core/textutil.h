// WinRepair GUI - platform independent text helpers (UTF-8 everywhere).
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wr {

inline std::string fmt(const char* f, ...) {
    va_list ap, ap2;
    va_start(ap, f);
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, f, ap);
    va_end(ap);
    std::string s;
    if (n > 0) {
        s.resize((size_t)n + 1);
        vsnprintf(&s[0], s.size(), f, ap2);
        s.resize((size_t)n);
    }
    va_end(ap2);
    return s;
}

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

inline std::string htmlEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += c;
        }
    }
    return o;
}

inline std::string csvQuote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += "\"\"";
        else if (c == '\r' || c == '\n') o += ' ';
        else o += c;
    }
    o += '"';
    return o;
}

inline void appendUtf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

// UTF-16LE bytes -> UTF-8 (sfc.exe prints UTF-16 when stdout is a pipe).
inline std::string utf16leToUtf8(const uint8_t* p, size_t n) {
    std::string o;
    o.reserve(n / 2 + 8);
    size_t units = n / 2;
    for (size_t i = 0; i < units; ++i) {
        uint32_t u = p[2 * i] | ((uint32_t)p[2 * i + 1] << 8);
        if (u == 0xFEFF && i == 0) continue;
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < units) {
            uint32_t lo = p[2 * i + 2] | ((uint32_t)p[2 * i + 3] << 8);
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        appendUtf8(o, u);
    }
    return o;
}

// Heuristic: text in UTF-16LE contains NUL bytes at odd positions and none at even ones.
inline bool looksLikeUtf16le(const uint8_t* p, size_t n) {
    if (n < 4) return false;
    size_t m = (n < 256 ? n : 256) & ~(size_t)1;
    size_t oddZero = 0, evenZero = 0;
    for (size_t i = 0; i < m; i += 2) {
        if (p[i] == 0) ++evenZero;
        if (p[i + 1] == 0) ++oddZero;
    }
    return oddZero >= 2 && evenZero * 4 <= oddZero;
}

// ---- console output code page detection -------------------------------------------------
// Child tools write in different encodings when piped: cmd tools in the OEM page (866), pnputil/others in ANSI (1251), some in UTF-8.
// Number of high-byte characters in a buffer.
inline size_t countHighBytes(const char* p, size_t n) { size_t c = 0; for (size_t i = 0; i < n; ++i) if ((unsigned char)p[i] >= 0x80) ++c; return c; }

// Strict UTF-8 check; *multi receives the number of multi-byte sequences. A truncated sequence at the very end is tolerated (chunk boundary).
inline bool utf8Valid(const char* p, size_t n, size_t* multi = nullptr) {
    size_t m = 0, i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)p[i];
        size_t len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        if (!len) return false;
        if (i + len > n) { if (multi) *multi = m; return true; }   // truncated tail
        for (size_t k = 1; k < len; ++k) if (((unsigned char)p[i + k] & 0xC0) != 0x80) return false;
        if (len > 1) ++m;
        i += len;
    }
    if (multi) *multi = m;
    return true;
}

// Length of the complete UTF-8 prefix (so a sequence split across two reads is carried over to the next one).
inline size_t utf8CompletePrefix(const char* p, size_t n) {
    for (size_t back = 1; back <= 3 && back <= n; ++back) {
        unsigned char c = (unsigned char)p[n - back];
        if ((c & 0xC0) == 0x80) continue;                       // continuation byte, keep looking for the lead
        size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return len > back ? n - back : n;
    }
    return n;
}

// Relative plausibility of decoded text as Russian (compare two decodings of the same bytes; absolute values mean little): lowercase/uppercase Cyrillic letters add, box-drawing and rare Cyrillic-block / Latin-1 symbols subtract.
// Text of 1251 decoded as 866 gives lots of box-drawing characters; 866 decoded as 1251 gives rare letters and symbols.
inline int cyrillicScore(const std::u16string& s) {
    int sc = 0;
    for (char16_t c : s) {
        if ((c >= 0x0430 && c <= 0x044F) || c == 0x0451) sc += 2;                       // а-я, ё
        else if ((c >= 0x0410 && c <= 0x042F) || c == 0x0401) sc += 1;                  // А-Я, Ё
        else if (c >= 0x2500 && c <= 0x259F) sc -= 4;                                   // box drawing / blocks
        else if ((c >= 0x0400 && c <= 0x040F) || (c >= 0x0452 && c <= 0x045F) || (c >= 0x0490 && c <= 0x04FF)) sc -= 3;   // Ђ Ѓ ђ ђ ...
        else if (c >= 0x00A0 && c <= 0x00FF) sc -= 2;                                   // Latin-1 symbols
        else if (c >= 0x2200 && c <= 0x22FF) sc -= 3;                                   // math symbols (866 0xF9/0xFB: bullet operator, root)
        else if (c >= 0x2018 && c <= 0x203A) sc -= 1;                                   // typographic punctuation (appears in mis-decoded 1251)
    }
    return sc;
}

// Lower-case for ASCII + basic Cyrillic; maps yo->ye so keywords compare reliably.
inline std::string toLowerUtf8(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') { o += (char)(c + 32); continue; }
        if ((c == 0xD0 || c == 0xD1) && i + 1 < s.size()) {
            unsigned char d = (unsigned char)s[i + 1];
            if (c == 0xD0 && d >= 0x90 && d <= 0x9F) { o += (char)0xD0; o += (char)(d + 0x20); ++i; continue; }
            if (c == 0xD0 && d >= 0xA0 && d <= 0xAF) { o += (char)0xD1; o += (char)(d - 0x20); ++i; continue; }
            if ((c == 0xD0 && d == 0x81) || (c == 0xD1 && d == 0x91)) { o += (char)0xD0; o += (char)0xB5; ++i; continue; }
        }
        o += (char)c;
    }
    return o;
}

inline bool containsAny(const std::string& hayLower, const std::vector<const char*>& needles) {
    for (const char* n : needles)
        if (hayLower.find(n) != std::string::npos) return true;
    return false;
}

// Sizes
inline std::string fmtBytes(uint64_t b) {
    const char* u[] = {"Б", "КБ", "МБ", "ГБ", "ТБ"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    return fmt(i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
}

// First whitespace-delimited token after `key` (used for cdb !analyze output).
inline std::string fieldValue(const std::string& text, const std::string& key) {
    size_t p = 0;
    while ((p = text.find(key, p)) != std::string::npos) {
        if (p == 0 || text[p - 1] == '\n') {
            size_t q = p + key.size();
            while (q < text.size() && (text[q] == ' ' || text[q] == '\t')) ++q;
            size_t e = q;
            while (e < text.size() && text[e] != '\r' && text[e] != '\n' && text[e] != ' ' && text[e] != '\t') ++e;
            return text.substr(q, e - q);
        }
        p += key.size();
    }
    return "";
}

// "0x00000133 (0x0000000000000001, ...)" -> 0x133
inline bool parseHexCode(const std::string& s, uint32_t& code) {
    size_t p = s.find("0x");
    if (p == std::string::npos) p = s.find("0X");
    if (p == std::string::npos) return false;
    p += 2;
    uint64_t v = 0;
    int digits = 0;
    while (p < s.size() && digits < 16) {
        char c = s[p];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (uint64_t)d;
        ++p; ++digits;
    }
    if (digits == 0) return false;
    code = (uint32_t)v;
    return true;
}

// Truncate to n bytes without cutting a UTF-8 sequence.
inline std::string truncUtf8(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t k = n;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) --k;
    return s.substr(0, k);
}

// sfc / DISM output analysis (EN + RU). Returns true when problems were reported.
inline bool integrityViolationFound(const std::string& output) {
    std::string l = toLowerUtf8(output);
    return containsAny(l, {"found integrity violations", "found corrupt", "repairable",
                           "нарушения целостности", "поврежденные файлы", "подлежит восстановлению",
                           "обнаружены повреждения"});
}

}  // namespace wr
