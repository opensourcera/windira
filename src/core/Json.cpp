// SPDX-License-Identifier: MIT
#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace windira::json {

const Value* Value::Find(const std::string& key) const {
    for (const auto& kv : object_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

void Value::Set(const std::string& key, Value v) {
    type_ = Type::Object;
    for (auto& kv : object_) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    }
    object_.emplace_back(key, std::move(v));
}

static void WriteString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;  // UTF-8 passes through unchanged
                }
        }
    }
    out += '"';
}

void Value::SerializeTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent <= 0) return;
        out += '\n';
        out.append((size_t)(indent * d), ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            char buf[64];
            if (std::floor(number_) == number_ && std::fabs(number_) < 1e15)
                snprintf(buf, sizeof(buf), "%.0f", number_);
            else
                snprintf(buf, sizeof(buf), "%.17g", number_);
            out += buf;
            break;
        }
        case Type::String: WriteString(out, string_); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                array_[i].SerializeTo(out, indent, depth + 1);
            }
            if (!array_.empty()) newline(depth);
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < object_.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                WriteString(out, object_[i].first);
                out += indent > 0 ? ": " : ":";
                object_[i].second.SerializeTo(out, indent, depth + 1);
            }
            if (!object_.empty()) newline(depth);
            out += '}';
            break;
    }
}

std::string Value::Serialize(int indent) const {
    std::string out;
    SerializeTo(out, indent, 0);
    return out;
}

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string error;

    void SkipWs() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool Fail(const char* msg) {
        if (error.empty()) error = std::string(msg) + " at offset " + std::to_string(i);
        return false;
    }
    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    bool ParseHex4(unsigned& cp) {
        if (i + 4 > s.size()) return Fail("truncated \\u escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= c - '0';
            else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
            else return Fail("bad \\u escape");
        }
        return true;
    }
    bool ParseString(std::string& out) {
        if (s[i] != '"') return Fail("expected string");
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!ParseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        unsigned lo;
                        if (!ParseHex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }
    bool ParseValue(Value& out, int depth) {
        if (depth > 64) return Fail("nesting too deep");
        SkipWs();
        if (i >= s.size()) return Fail("unexpected end");
        char c = s[i];
        if (c == '{') {
            ++i;
            out = Value::MakeObject();
            SkipWs();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                SkipWs();
                std::string key;
                if (i >= s.size() || !ParseString(key)) return Fail("expected key");
                SkipWs();
                if (i >= s.size() || s[i] != ':') return Fail("expected ':'");
                ++i;
                Value v;
                if (!ParseValue(v, depth + 1)) return false;
                out.Set(key, std::move(v));
                SkipWs();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++i;
            out = Value::MakeArray();
            SkipWs();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                Value v;
                if (!ParseValue(v, depth + 1)) return false;
                out.Push(std::move(v));
                SkipWs();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string str;
            if (!ParseString(str)) return false;
            out = Value(std::move(str));
            return true;
        }
        if (s.compare(i, 4, "true") == 0) { i += 4; out = Value(true); return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; out = Value(false); return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; out = Value(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = s.c_str() + i;
            char* end = nullptr;
            double d = std::strtod(start, &end);
            if (end == start) return Fail("bad number");
            i += (size_t)(end - start);
            out = Value(d);
            return true;
        }
        return Fail("unexpected character");
    }
};

}  // namespace

bool Value::Parse(const std::string& text, Value& out, std::string* error) {
    Parser p{text};
    // Tolerate a UTF-8 BOM written by editors such as Notepad.
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        p.i = 3;
    bool ok = p.ParseValue(out, 0);
    if (ok) {
        p.SkipWs();
        if (p.i != text.size()) ok = p.Fail("trailing characters");
    }
    if (!ok && error) *error = p.error;
    return ok;
}

}  // namespace windira::json
