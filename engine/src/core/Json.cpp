#include "crossrender/core/Json.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace crossrender {
namespace {

const JsonValue& NullValue() {
    static const JsonValue v;
    return v;
}

void EscapeString(const std::string& s, std::string& out) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void FormatNumber(double v, std::string& out) {
    if (v == static_cast<double>(static_cast<i64>(v)) && std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out += buf;
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    out += buf;
}

// ---------------------------------------------------------------------------
// Рекурсивный парсер с рекурсивным спуском
// ---------------------------------------------------------------------------
class Parser {
public:
    Parser(const std::string& text, std::string* error) : s_(text), error_(error) {}

    bool Parse(JsonValue* out) {
        SkipWhitespace();
        if (!ParseValue(out)) return false;
        SkipWhitespace();
        if (pos_ != s_.size()) return Fail("trailing characters after JSON value");
        return true;
    }

private:
    bool Fail(const char* msg) {
        if (error_ && error_->empty()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s at offset %zu", msg, pos_);
            *error_ = buf;
        }
        return false;
    }

    void SkipWhitespace() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    bool ParseValue(JsonValue* out) {
        SkipWhitespace();
        if (pos_ >= s_.size()) return Fail("unexpected end of input");
        char c = s_[pos_];
        switch (c) {
            case '{': return ParseObject(out);
            case '[': return ParseArray(out);
            case '"': {
                std::string str;
                if (!ParseString(&str)) return false;
                *out = JsonValue(std::move(str));
                return true;
            }
            case 't':
                if (s_.compare(pos_, 4, "true") == 0) {
                    pos_ += 4;
                    *out = JsonValue(true);
                    return true;
                }
                return Fail("invalid literal");
            case 'f':
                if (s_.compare(pos_, 5, "false") == 0) {
                    pos_ += 5;
                    *out = JsonValue(false);
                    return true;
                }
                return Fail("invalid literal");
            case 'n':
                if (s_.compare(pos_, 4, "null") == 0) {
                    pos_ += 4;
                    *out = JsonValue();
                    return true;
                }
                return Fail("invalid literal");
            default: return ParseNumber(out);
        }
    }

    bool ParseNumber(JsonValue* out) {
        usize start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        bool any = false;
        if (pos_ < s_.size() && s_[pos_] == '0') {
            ++pos_;
            any = true;
            if (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9')
                return Fail("leading zero in number");
        } else {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                ++pos_;
                any = true;
            }
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            const usize fractionStart = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                ++pos_;
                any = true;
            }
            if (pos_ == fractionStart) return Fail("digits required after decimal point");
        }
        if (any && pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
            const usize exponentStart = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            if (pos_ == exponentStart) return Fail("digits required after exponent");
        }
        if (!any) return Fail("invalid number");
        // Ключевые слова NaN / Infinity невалидны для JSON, но встречаются на практике.
        std::string num = s_.substr(start, pos_ - start);
        *out = JsonValue(std::strtod(num.c_str(), nullptr));
        return true;
    }

    bool ParseString(std::string* out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return Fail("expected string");
        ++pos_;
        out->clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) return Fail("unterminated escape");
            char e = s_[pos_++];
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) return Fail("bad \\u escape");
                    u32 cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = s_[pos_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9')
                            cp |= static_cast<u32>(h - '0');
                        else if (h >= 'a' && h <= 'f')
                            cp |= static_cast<u32>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            cp |= static_cast<u32>(h - 'A' + 10);
                        else
                            return Fail("bad hex digit in \\u escape");
                    }
                    // Суррогатная пара
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' &&
                        s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        u32 lo = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s_[pos_++];
                            lo <<= 4;
                            if (h >= '0' && h <= '9')
                                lo |= static_cast<u32>(h - '0');
                            else if (h >= 'a' && h <= 'f')
                                lo |= static_cast<u32>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F')
                                lo |= static_cast<u32>(h - 'A' + 10);
                            else
                                return Fail("bad hex digit in low surrogate");
                        }
                         if (lo < 0xDC00 || lo > 0xDFFF) return Fail("invalid low surrogate");
                         cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                     }
                     if (cp >= 0xD800 && cp <= 0xDFFF) return Fail("unpaired surrogate");
                    // Кодирование в UTF-8
                    if (cp < 0x80) {
                        out->push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
                        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return Fail("unknown escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseArray(JsonValue* out) {
        ++pos_;  // '['
        JsonArray arr;
        SkipWhitespace();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            *out = JsonValue(std::move(arr));
            return true;
        }
        while (true) {
            JsonValue v;
            if (!ParseValue(&v)) return false;
            arr.push_back(std::move(v));
            SkipWhitespace();
            if (pos_ >= s_.size()) return Fail("unterminated array");
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                break;
            }
            return Fail("expected ',' or ']'");
        }
        *out = JsonValue(std::move(arr));
        return true;
    }

    bool ParseObject(JsonValue* out) {
        ++pos_;  // '{'
        JsonObject obj;
        SkipWhitespace();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            *out = JsonValue(std::move(obj));
            return true;
        }
        while (true) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(&key)) return false;
            SkipWhitespace();
            if (pos_ >= s_.size() || s_[pos_] != ':') return Fail("expected ':'");
            ++pos_;
            JsonValue v;
            if (!ParseValue(&v)) return false;
            obj[key] = std::move(v);
            SkipWhitespace();
            if (pos_ >= s_.size()) return Fail("unterminated object");
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                break;
            }
            return Fail("expected ',' or '}'");
        }
        *out = JsonValue(std::move(obj));
        return true;
    }

    const std::string& s_;
    std::string* error_;
    usize pos_ = 0;
};

const JsonArray kEmptyArray;
const JsonObject kEmptyObject;

}  // namespace

const JsonValue& JsonValue::operator[](usize i) const {
    if (type_ != Type::Array || !arr_ || i >= arr_->size()) return NullValue();
    return (*arr_)[i];
}

JsonValue& JsonValue::operator[](usize i) {
    static JsonValue dummy;
    if (type_ != Type::Array || !arr_) return dummy;
    if (i >= arr_->size()) arr_->resize(i + 1);
    return (*arr_)[i];
}

const JsonValue& JsonValue::operator[](const std::string& key) const {
    const JsonValue* v = Find(key);
    return v ? *v : NullValue();
}

JsonValue& JsonValue::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        obj_ = std::make_shared<JsonObject>();
    }
    return (*obj_)[key];
}

const JsonObject& JsonValue::Object() const {
    static const JsonObject empty;
    return (type_ == Type::Object && obj_) ? *obj_ : empty;
}

const JsonArray& JsonValue::Array() const {
    static const JsonArray empty;
    return (type_ == Type::Array && arr_) ? *arr_ : empty;
}

void JsonValue::Push(JsonValue v) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        arr_ = std::make_shared<JsonArray>();
    }
    arr_->push_back(std::move(v));
}

void JsonValue::Set(const std::string& key, JsonValue v) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        obj_ = std::make_shared<JsonObject>();
    }
    (*obj_)[key] = std::move(v);
}

std::string JsonValue::Dump(int indent) const {
    std::string out;
    DumpTo(out, indent, 0);
    return out;
}

void JsonValue::DumpTo(std::string& out, int indent, int depth) const {
    auto newlineIndent = [&](int d) {
        if (indent < 0) return;
        out.push_back('\n');
        out.append(static_cast<usize>(indent * d), ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: FormatNumber(num_, out); break;
        case Type::String: EscapeString(str_, out); break;
        case Type::Array: {
            const JsonArray& a = Array();
            if (a.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (usize i = 0; i < a.size(); ++i) {
                newlineIndent(depth + 1);
                a[i].DumpTo(out, indent, depth + 1);
                if (i + 1 < a.size()) out.push_back(',');
            }
            newlineIndent(depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            const JsonObject& o = Object();
            if (o.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            usize i = 0;
            for (const auto& kv : o) {
                newlineIndent(depth + 1);
                EscapeString(kv.first, out);
                out.push_back(':');
                if (indent >= 0) out.push_back(' ');
                kv.second.DumpTo(out, indent, depth + 1);
                if (++i < o.size()) out.push_back(',');
            }
            newlineIndent(depth);
            out.push_back('}');
            break;
        }
    }
}

JsonValue JsonValue::Parse(const std::string& text, std::string* error) {
    JsonValue out;
    std::string err;
    Parser p(text, &err);
    if (!p.Parse(&out)) {
        if (error) *error = err;
        ENG_LOGW("json", "parse error: %s", err.c_str());
        return JsonValue();
    }
    if (error) error->clear();
    return out;
}

bool JsonValue::ParseFile(const std::string& path, JsonValue* out, std::string* error) {
    std::string text = ReadTextFile(path);
    if (text.empty()) {
        if (error) *error = "file not found or empty: " + path;
        return false;
    }
    std::string err;
    JsonValue v = Parse(text, &err);
    if (!err.empty()) {
        if (error) *error = err;
        return false;
    }
    if (out) *out = std::move(v);
    return true;
}

JsonValue JsonMakeArray() {
    JsonValue v;
    v.Push(JsonValue());
    // Начинаем пустым: пересоздание через присваивание.
    JsonValue empty(JsonArray{});
    return empty;
}

JsonValue JsonMakeObject() { return JsonValue(JsonObject{}); }

}  // namespace crossrender
