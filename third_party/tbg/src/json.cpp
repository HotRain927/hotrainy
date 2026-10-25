// tbg/json.cpp
#include "tbg/json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tbg {
namespace json {

Value Value::makeNull() { return Value(); }
Value Value::makeBool(bool v) { Value x; x.type = BOOL; x.b = v; return x; }
Value Value::makeNum(double v) { Value x; x.type = NUM; x.num = v; return x; }
Value Value::makeStr(const std::string& v) { Value x; x.type = STR; x.str = v; return x; }
Value Value::makeArr() { Value x; x.type = ARR; return x; }
Value Value::makeObj() { Value x; x.type = OBJ; return x; }

const Value* Value::find(const std::string& key) const {
    if (type != OBJ) return nullptr;
    for (const auto& kv : obj) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

bool Value::getBool(const std::string& key, bool def) const {
    const Value* v = find(key);
    if (!v) return def;
    if (v->type == BOOL) return v->b;
    if (v->type == NUM) return v->num != 0.0;
    return def;
}

double Value::getNum(const std::string& key, double def) const {
    const Value* v = find(key);
    if (!v) return def;
    if (v->type == NUM) return v->num;
    if (v->type == BOOL) return v->b ? 1.0 : 0.0;
    return def;
}

std::string Value::getStr(const std::string& key, const std::string& def) const {
    const Value* v = find(key);
    if (!v || v->type != STR) return def;
    return v->str;
}

Value& Value::set(const std::string& key, const Value& v) {
    if (type != OBJ) { type = OBJ; obj.clear(); }
    for (auto& kv : obj) {
        if (kv.first == key) { kv.second = v; return *this; }
    }
    obj.push_back(std::make_pair(key, v));
    return *this;
}

Value& Value::setBool(const std::string& key, bool v) { return set(key, makeBool(v)); }
Value& Value::setNum(const std::string& key, double v) { return set(key, makeNum(v)); }
Value& Value::setStr(const std::string& key, const std::string& v) { return set(key, makeStr(v)); }
void Value::push(const Value& v) { type = ARR; arr.push_back(v); }

// ── 解析 ─────────────────────────────────────────

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    explicit Parser(const std::string& src) : s(src) {}

    void ws() {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
            // 允许 // 行注释（手工编辑配置时更友好）
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            break;
        }
    }

    bool fail(const char* msg) {
        if (err.empty()) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s @%zu", msg, i);
            err = buf;
        }
        return false;
    }

    bool parseValue(Value& out) {
        ws();
        if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') {
            out.type = Value::STR;
            return parseString(out.str);
        }
        if (c == 't' || c == 'f') return parseBool(out);
        if (c == 'n') return parseNull(out);
        return parseNumber(out);
    }

    bool parseObject(Value& out) {
        out = Value::makeObj();
        ++i;  // {
        ws();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            ws();
            if (i >= s.size() || s[i] != '"') return fail("expected key");
            std::string key;
            if (!parseString(key)) return false;
            ws();
            if (i >= s.size() || s[i] != ':') return fail("expected ':'");
            ++i;
            Value v;
            if (!parseValue(v)) return false;
            out.obj.push_back(std::make_pair(key, v));
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(Value& out) {
        out = Value::makeArr();
        ++i;  // [
        ws();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        while (true) {
            Value v;
            if (!parseValue(v)) return false;
            out.arr.push_back(v);
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parseString(std::string& out) {
        out.clear();
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i >= s.size()) return fail("bad escape");
                char e = s[i++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (i + 4 > s.size()) return fail("bad \\u");
                        unsigned cp = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                        i += 4;
                        // UTF-16 代理对
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() &&
                            s[i] == '\\' && s[i + 1] == 'u') {
                            unsigned lo = (unsigned)strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                i += 6;
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                        }
                        encodeUtf8(cp, out);
                        break;
                    }
                    default: return fail("unknown escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return fail("unterminated string");
    }

    static void encodeUtf8(unsigned cp, std::string& out) {
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    bool parseBool(Value& out) {
        if (s.compare(i, 4, "true") == 0) { i += 4; out = Value::makeBool(true); return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; out = Value::makeBool(false); return true; }
        return fail("bad literal");
    }

    bool parseNull(Value& out) {
        if (s.compare(i, 4, "null") == 0) { i += 4; out = Value::makeNull(); return true; }
        return fail("bad literal");
    }

    bool parseNumber(Value& out) {
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' ||
                                s[i] == 'e' || s[i] == 'E' || s[i] == '-' || s[i] == '+')) {
            ++i;
        }
        if (start == i) return fail("bad number");
        std::string tok = s.substr(start, i - start);
        char* endp = nullptr;
        double v = strtod(tok.c_str(), &endp);
        if (!endp || *endp != '\0') return fail("bad number");
        out = Value::makeNum(v);
        return true;
    }
};

void escapeTo(const std::string& in, std::string& out) {
    out.push_back('"');
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    out.push_back('"');
}

void dumpTo(const Value& v, std::string& out, int indent, bool pretty) {
    std::string pad(pretty ? (size_t)(indent * 2) : 0, ' ');
    std::string padIn(pretty ? (size_t)((indent + 1) * 2) : 0, ' ');
    const char* nl = pretty ? "\n" : "";

    switch (v.type) {
        case Value::NUL: out += "null"; break;
        case Value::BOOL: out += v.b ? "true" : "false"; break;
        case Value::NUM: {
            char buf[64];
            if (v.num == (double)(int64_t)v.num) {
                snprintf(buf, sizeof(buf), "%lld", (long long)(int64_t)v.num);
            } else {
                snprintf(buf, sizeof(buf), "%.10g", v.num);
            }
            out += buf;
            break;
        }
        case Value::STR: escapeTo(v.str, out); break;
        case Value::ARR: {
            if (v.arr.empty()) { out += "[]"; break; }
            out += "[";
            out += nl;
            for (size_t k = 0; k < v.arr.size(); ++k) {
                out += padIn;
                dumpTo(v.arr[k], out, indent + 1, pretty);
                if (k + 1 < v.arr.size()) out += ",";
                out += nl;
            }
            out += pad;
            out += "]";
            break;
        }
        case Value::OBJ: {
            if (v.obj.empty()) { out += "{}"; break; }
            out += "{";
            out += nl;
            for (size_t k = 0; k < v.obj.size(); ++k) {
                out += padIn;
                escapeTo(v.obj[k].first, out);
                out += pretty ? ": " : ":";
                dumpTo(v.obj[k].second, out, indent + 1, pretty);
                if (k + 1 < v.obj.size()) out += ",";
                out += nl;
            }
            out += pad;
            out += "}";
            break;
        }
    }
}

}  // namespace

bool parse(const std::string& text, Value& out, std::string& err) {
    Parser p(text);
    if (!p.parseValue(out)) {
        err = p.err.empty() ? "parse error" : p.err;
        return false;
    }
    p.ws();
    if (p.i != text.size()) {
        err = "trailing characters";
        return false;
    }
    err.clear();
    return true;
}

std::string dump(const Value& v, bool pretty) {
    std::string out;
    out.reserve(2048);
    dumpTo(v, out, 0, pretty);
    return out;
}

}  // namespace json
}  // namespace tbg
