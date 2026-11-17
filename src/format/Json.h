#pragma once

// Lightweight, header-only JSON parser and serializer for `.livewdf`.
//
// This is deliberately a small value type, not a general JSON library: the
// document format needs objects, arrays, strings, numbers and booleans, and the
// serializer needs stable two-space indentation. Keeping it here means the core
// library still has no third-party dependency.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace wdf {
// ==============================================================================
// Lightweight, header-only, zero-dependency JSON Parser & Serializer for .livewdf
// ==============================================================================
namespace json {

enum class Type { Null, Boolean, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<Value> arrVal;
    std::map<std::string, Value> objVal;

    Value() = default;
    explicit Value(Type t) : type(t) {}
    Value(bool b) : type(Type::Boolean), boolVal(b) {}
    Value(double n) : type(Type::Number), numVal(n) {}
    Value(int n) : type(Type::Number), numVal(static_cast<double>(n)) {}
    Value(const char* s) : type(Type::String), strVal(s ? s : "") {}
    Value(std::string s) : type(Type::String), strVal(std::move(s)) {}
    Value(std::vector<Value> a) : type(Type::Array), arrVal(std::move(a)) {}
    Value(std::map<std::string, Value> o) : type(Type::Object), objVal(std::move(o)) {}

    bool isObject() const noexcept { return type == Type::Object; }
    bool isArray() const noexcept { return type == Type::Array; }
    bool isString() const noexcept { return type == Type::String; }
    bool isNumber() const noexcept { return type == Type::Number; }
    bool isBool() const noexcept { return type == Type::Boolean; }

    const Value& operator[](const std::string& key) const {
        static const Value kNull;
        if (type != Type::Object) return kNull;
        auto it = objVal.find(key);
        return (it != objVal.end()) ? it->second : kNull;
    }

    Value& operator[](const std::string& key) {
        if (type != Type::Object) {
            type = Type::Object;
            objVal.clear();
        }
        return objVal[key];
    }

    const Value& operator[](size_t index) const {
        static const Value kNull;
        if (type != Type::Array || index >= arrVal.size()) return kNull;
        return arrVal[index];
    }

    std::string asString(const std::string& def = "") const {
        return (type == Type::String) ? strVal : def;
    }
    double asDouble(double def = 0.0) const {
        return (type == Type::Number) ? numVal : def;
    }
    int asInt(int def = 0) const {
        return (type == Type::Number) ? static_cast<int>(std::round(numVal)) : def;
    }
    bool asBool(bool def = false) const {
        return (type == Type::Boolean) ? boolVal : def;
    }

    std::string serialize(int indent = 2, int currentIndent = 0) const {
        const std::string ind(static_cast<size_t>(currentIndent), ' ');
        const std::string nextInd(static_cast<size_t>(currentIndent + indent), ' ');

        switch (type) {
        case Type::Null: return "null";
        case Type::Boolean: return boolVal ? "true" : "false";
        case Type::Number: {
            // A number must read back as the double it was written from. The
            // shortcut used here once rounded anything within 1e-12 of an
            // integer, which deleted small values outright: a BJT's Is of
            // 1.037e-15 rounds to zero, and an Is of zero is not a
            // transistor, so the saved document stopped being a transistor.
            // Write an integer only when the value is one, and otherwise the
            // shortest digits that read back exactly. Ugly digits are the
            // price of a document that tells the truth.
            if (std::isfinite(numVal)) {
                const double rounded = std::round(numVal);
                if (numVal == rounded && std::abs(numVal) < 1e15) {
                    std::ostringstream ss;
                    ss << static_cast<long long>(numVal);
                    return ss.str();
                }
                for (int precision = 1; precision <= 17; ++precision) {
                    std::ostringstream ss;
                    ss << std::setprecision(precision) << numVal;
                    double readBack = 0.0;
                    std::istringstream back(ss.str());
                    if ((back >> readBack) && readBack == numVal)
                        return ss.str();
                }
            }
            std::ostringstream ss;
            ss << std::setprecision(17) << numVal;
            return ss.str();
        }
        case Type::String: {
            std::ostringstream ss;
            ss << '"';
            for (char c : strVal) {
                switch (c) {
                case '"': ss << "\\\""; break;
                case '\\': ss << "\\\\"; break;
                case '\b': ss << "\\b"; break;
                case '\f': ss << "\\f"; break;
                case '\n': ss << "\\n"; break;
                case '\r': ss << "\\r"; break;
                case '\t': ss << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                    } else {
                        ss << c;
                    }
                    break;
                }
            }
            ss << '"';
            return ss.str();
        }
        case Type::Array: {
            if (arrVal.empty()) return "[]";
            std::ostringstream ss;
            ss << "[\n";
            for (size_t i = 0; i < arrVal.size(); ++i) {
                ss << nextInd << arrVal[i].serialize(indent, currentIndent + indent);
                if (i + 1 < arrVal.size()) ss << ",";
                ss << "\n";
            }
            ss << ind << "]";
            return ss.str();
        }
        case Type::Object: {
            if (objVal.empty()) return "{}";
            std::ostringstream ss;
            ss << "{\n";
            size_t count = 0;
            for (const auto& [k, v] : objVal) {
                ss << nextInd << '"' << k << "\": " << v.serialize(indent, currentIndent + indent);
                if (++count < objVal.size()) ss << ",";
                ss << "\n";
            }
            ss << ind << "}";
            return ss.str();
        }
        }
        return "null";
    }

    static bool parse(const std::string& src, Value& out, std::string& error) {
        size_t idx = 0;
        skipWs(src, idx);
        if (!parseValue(src, idx, out, error))
            return false;
        skipWs(src, idx);
        if (idx < src.size()) {
            error = "Unexpected trailing characters after JSON";
            return false;
        }
        return true;
    }

private:
    static void skipWs(const std::string& s, size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }

    static bool parseValue(const std::string& s, size_t& i, Value& out, std::string& err) {
        skipWs(s, i);
        if (i >= s.size()) {
            err = "Unexpected end of input";
            return false;
        }
        char c = s[i];
        if (c == '{') return parseObject(s, i, out, err);
        if (c == '[') return parseArray(s, i, out, err);
        if (c == '"') return parseString(s, i, out, err);
        if (c == 't' || c == 'f') return parseBool(s, i, out, err);
        if (c == 'n') return parseNull(s, i, out, err);
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(s, i, out, err);
        err = std::string("Unexpected token '") + c + "'";
        return false;
    }

    static bool parseObject(const std::string& s, size_t& i, Value& out, std::string& err) {
        ++i; // skip '{'
        out.type = Type::Object;
        out.objVal.clear();
        skipWs(s, i);
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        while (i < s.size()) {
            skipWs(s, i);
            if (i >= s.size() || s[i] != '"') {
                err = "Expected string key in object";
                return false;
            }
            Value keyVal;
            if (!parseString(s, i, keyVal, err))
                return false;
            skipWs(s, i);
            if (i >= s.size() || s[i] != ':') {
                err = "Expected ':' after object key";
                return false;
            }
            ++i; // skip ':'
            Value val;
            if (!parseValue(s, i, val, err))
                return false;
            out.objVal[keyVal.strVal] = std::move(val);
            skipWs(s, i);
            if (i < s.size() && s[i] == ',') {
                ++i;
            } else if (i < s.size() && s[i] == '}') {
                ++i;
                return true;
            } else {
                err = "Expected ',' or '}' in object";
                return false;
            }
        }
        err = "Unterminated object";
        return false;
    }

    static bool parseArray(const std::string& s, size_t& i, Value& out, std::string& err) {
        ++i; // skip '['
        out.type = Type::Array;
        out.arrVal.clear();
        skipWs(s, i);
        if (i < s.size() && s[i] == ']') {
            ++i;
            return true;
        }
        while (i < s.size()) {
            Value item;
            if (!parseValue(s, i, item, err))
                return false;
            out.arrVal.push_back(std::move(item));
            skipWs(s, i);
            if (i < s.size() && s[i] == ',') {
                ++i;
            } else if (i < s.size() && s[i] == ']') {
                ++i;
                return true;
            } else {
                err = "Expected ',' or ']' in array";
                return false;
            }
        }
        err = "Unterminated array";
        return false;
    }

    static bool parseString(const std::string& s, size_t& i, Value& out, std::string& err) {
        ++i; // skip leading '"'
        std::string res;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') {
                out.type = Type::String;
                out.strVal = std::move(res);
                return true;
            }
            if (c == '\\') {
                if (i >= s.size()) {
                    err = "Unexpected end in escape sequence";
                    return false;
                }
                char esc = s[i++];
                switch (esc) {
                case '"': res += '"'; break;
                case '\\': res += '\\'; break;
                case '/': res += '/'; break;
                case 'b': res += '\b'; break;
                case 'f': res += '\f'; break;
                case 'n': res += '\n'; break;
                case 'r': res += '\r'; break;
                case 't': res += '\t'; break;
                case 'u':
                    if (i + 4 <= s.size()) {
                        i += 4; // skip 4 hex digits for basic ascii compatibility
                        res += '?';
                    }
                    break;
                default: res += esc; break;
                }
            } else {
                res += c;
            }
        }
        err = "Unterminated string";
        return false;
    }

    static bool parseNumber(const std::string& s, size_t& i, Value& out, std::string& err) {
        size_t start = i;
        if (s[i] == '-') ++i;
        while (i < s.size() && (s[i] >= '0' && s[i] <= '9')) ++i;
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && (s[i] >= '0' && s[i] <= '9')) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            while (i < s.size() && (s[i] >= '0' && s[i] <= '9')) ++i;
        }
        const std::string sub = s.substr(start, i - start);
        try {
            out.type = Type::Number;
            out.numVal = std::stod(sub);
            return true;
        } catch (...) {
            err = "Invalid number: " + sub;
            return false;
        }
    }

    static bool parseBool(const std::string& s, size_t& i, Value& out, std::string& err) {
        if (s.substr(i, 4) == "true") {
            i += 4;
            out = Value(true);
            return true;
        }
        if (s.substr(i, 5) == "false") {
            i += 5;
            out = Value(false);
            return true;
        }
        err = "Invalid boolean literal";
        return false;
    }

    static bool parseNull(const std::string& s, size_t& i, Value& out, std::string& err) {
        if (s.substr(i, 4) == "null") {
            i += 4;
            out = Value();
            return true;
        }
        err = "Invalid null literal";
        return false;
    }
};

} // namespace json
} // namespace wdf