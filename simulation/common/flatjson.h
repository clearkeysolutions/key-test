// A strict reader for the flat JSON objects used as case plans
// (schemas/case.json): string, number, bool and null values, and arrays of
// numbers. Nested objects are rejected so a plan cannot carry hidden state.
#ifndef CKT_FLATJSON_H
#define CKT_FLATJSON_H

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace ckt {

struct JVal {
    enum Kind { Null, Bool, Num, Str, Arr } kind = Null;
    double num = 0;
    std::string str;
    std::vector<double> arr;
};

using JObj = std::map<std::string, JVal>;

inline JObj parse_flat_json(const std::string &s) {
    size_t i = 0;
    const auto ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i; };
    const auto fail = [&](const char *why) { throw std::runtime_error(std::string("plan json: ") + why + " at " + std::to_string(i)); };
    const auto str = [&]() -> std::string {
        if (s[i] != '"') fail("expected string");
        ++i; std::string out;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') {
                ++i; if (i >= s.size()) fail("bad escape");
                const char e = s[i];
                if (e == 'n') out += '\n'; else if (e == 't') out += '\t'; else if (e == '"' || e == '\\' || e == '/') out += e;
                else if (e == 'u') { if (i + 4 >= s.size()) fail("bad \\u"); out += static_cast<char>(std::strtol(s.substr(i + 1, 4).c_str(), nullptr, 16) & 0x7f); i += 4; }
                else fail("unsupported escape");
                ++i;
            } else out += s[i++];
        }
        if (i >= s.size()) fail("unterminated string");
        ++i; return out;
    };
    const auto number = [&]() -> double {
        const char *b = s.c_str() + i; char *e = nullptr; const double v = std::strtod(b, &e);
        if (e == b) fail("expected number");
        i += static_cast<size_t>(e - b); return v;
    };
    JObj obj; ws();
    if (i >= s.size() || s[i] != '{') fail("expected object");
    ++i; ws();
    if (s[i] == '}') return obj;
    while (true) {
        ws(); const std::string key = str(); ws();
        if (s[i] != ':') fail("expected colon");
        ++i; ws();
        JVal v;
        if (s[i] == '"') { v.kind = JVal::Str; v.str = str(); }
        else if (s.compare(i, 4, "true") == 0) { v.kind = JVal::Bool; v.num = 1; i += 4; }
        else if (s.compare(i, 5, "false") == 0) { v.kind = JVal::Bool; v.num = 0; i += 5; }
        else if (s.compare(i, 4, "null") == 0) { v.kind = JVal::Null; i += 4; }
        else if (s[i] == '[') {
            v.kind = JVal::Arr; ++i; ws();
            if (s[i] != ']') while (true) { ws(); v.arr.push_back(number()); ws(); if (s[i] == ',') { ++i; continue; } if (s[i] == ']') break; fail("bad array"); }
            ++i;
        } else if (s[i] == '{') fail("nested objects are not allowed in plans");
        else { v.kind = JVal::Num; v.num = number(); }
        if (obj.count(key)) fail("duplicate key");
        obj[key] = v; ws();
        if (s[i] == ',') { ++i; continue; }
        if (s[i] == '}') break;
        fail("expected , or }");
    }
    return obj;
}

// JSON string escaping for the result writer.
inline std::string jstr(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\"; else if (c == '\n') o += "\\n";
        else if (c < 0x20 || c >= 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o + "\"";
}

inline std::string jnum(double v) {
    if (!std::isfinite(v)) return "null";
    char b[40]; std::snprintf(b, sizeof b, "%.6g", v); return b;
}

}  // namespace ckt
#endif
