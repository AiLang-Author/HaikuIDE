/* Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT. */
#include "json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace haikuide {
namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool ParseValue(Value* out, std::string* err) {
        Skip();
        if (!ValueOf(out, err)) {
            return false;
        }
        Skip();
        if (i_ != text_.size()) {
            *err = "trailing data";
            return false;
        }
        return true;
    }

private:
    void Skip() {
        while (i_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[i_])) != 0) {
            ++i_;
        }
    }

    bool ValueOf(Value* out, std::string* err) {
        Skip();
        if (i_ >= text_.size()) {
            *err = "unexpected end";
            return false;
        }
        const char c = text_[i_];
        if (c == 'n') {
            return Literal("null", out, err);
        }
        if (c == 't') {
            return Literal("true", out, err);
        }
        if (c == 'f') {
            return Literal("false", out, err);
        }
        if (c == '"') {
            return StringOf(out, err);
        }
        if (c == '[') {
            return ArrayOf(out, err);
        }
        if (c == '{') {
            return ObjectOf(out, err);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return NumberOf(out, err);
        }
        *err = "invalid value";
        return false;
    }

    bool Literal(const char* word, Value* out, std::string* err) {
        const size_t n = std::char_traits<char>::length(word);
        if (text_.substr(i_, n) != word) {
            *err = "invalid literal";
            return false;
        }
        i_ += n;
        if (word[0] == 'n') {
            *out = JsonNull();
        } else {
            *out = JsonBool(word[0] == 't');
        }
        return true;
    }

    bool StringOf(Value* out, std::string* err) {
        ++i_;
        std::string s;
        while (i_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[i_++]);
            if (c == '"') {
                *out = JsonString(std::move(s));
                return true;
            }
            if (c == '\\') {
                if (i_ >= text_.size()) {
                    *err = "bad escape";
                    return false;
                }
                const char e = text_[i_++];
                switch (e) {
                case '"':
                case '\\':
                case '/':
                    s.push_back(e);
                    break;
                case 'b':
                    s.push_back('\b');
                    break;
                case 'f':
                    s.push_back('\f');
                    break;
                case 'n':
                    s.push_back('\n');
                    break;
                case 'r':
                    s.push_back('\r');
                    break;
                case 't':
                    s.push_back('\t');
                    break;
                case 'u':
                    if (!Unicode(&s, err)) {
                        return false;
                    }
                    break;
                default:
                    *err = "bad escape";
                    return false;
                }
                continue;
            }
            if (c < 0x20) {
                *err = "raw control in string";
                return false;
            }
            s.push_back(static_cast<char>(c));
        }
        *err = "unterminated string";
        return false;
    }

    bool Hex4(uint32_t* cp, std::string* err) {
        if (i_ + 4 > text_.size()) {
            *err = "short unicode escape";
            return false;
        }
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const unsigned char h = static_cast<unsigned char>(text_[i_++]);
            v <<= 4;
            if (h >= '0' && h <= '9') {
                v += static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                v += static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                v += static_cast<uint32_t>(h - 'A' + 10);
            } else {
                *err = "bad unicode escape";
                return false;
            }
        }
        *cp = v;
        return true;
    }

    static void Utf8(std::string* s, uint32_t cp) {
        if (cp <= 0x7f) {
            s->push_back(static_cast<char>(cp));
        } else if (cp <= 0x7ff) {
            s->push_back(static_cast<char>(0xc0 | (cp >> 6)));
            s->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            s->push_back(static_cast<char>(0xe0 | (cp >> 12)));
            s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            s->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            s->push_back(static_cast<char>(0xf0 | (cp >> 18)));
            s->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            s->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    bool Unicode(std::string* s, std::string* err) {
        uint32_t cp = 0;
        if (!Hex4(&cp, err)) {
            return false;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 2 <= text_.size() && text_[i_] == '\\' &&
            text_[i_ + 1] == 'u') {
            const size_t save = i_;
            i_ += 2;
            uint32_t low = 0;
            if (!Hex4(&low, err)) {
                return false;
            }
            if (low < 0xDC00 || low > 0xDFFF) {
                i_ = save;
            } else {
                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
            }
        }
        Utf8(s, cp);
        return true;
    }

    bool NumberOf(Value* out, std::string* err) {
        const size_t begin = i_;
        if (text_[i_] == '-') {
            ++i_;
        }
        if (i_ >= text_.size() || text_[i_] < '0' || text_[i_] > '9') {
            *err = "bad number";
            return false;
        }
        if (text_[i_] == '0') {
            ++i_;
        } else {
            while (i_ < text_.size() && text_[i_] >= '0' && text_[i_] <= '9') {
                ++i_;
            }
        }
        bool integral = true;
        if (i_ < text_.size() && text_[i_] == '.') {
            integral = false;
            ++i_;
            if (i_ >= text_.size() || text_[i_] < '0' || text_[i_] > '9') {
                *err = "bad number";
                return false;
            }
            while (i_ < text_.size() && text_[i_] >= '0' && text_[i_] <= '9') {
                ++i_;
            }
        }
        if (i_ < text_.size() && (text_[i_] == 'e' || text_[i_] == 'E')) {
            integral = false;
            ++i_;
            if (i_ < text_.size() && (text_[i_] == '+' || text_[i_] == '-')) {
                ++i_;
            }
            if (i_ >= text_.size() || text_[i_] < '0' || text_[i_] > '9') {
                *err = "bad number";
                return false;
            }
            while (i_ < text_.size() && text_[i_] >= '0' && text_[i_] <= '9') {
                ++i_;
            }
        }
        const std::string token(text_.substr(begin, i_ - begin));
        Value v;
        v.kind = Value::Kind::Number;
        if (integral) {
            try {
                size_t used = 0;
                v.i = std::stoll(token, &used, 10);
                if (used == token.size()) {
                    v.is_int = true;
                    v.d = static_cast<double>(v.i);
                    *out = std::move(v);
                    return true;
                }
            } catch (const std::exception&) {
            }
        }
        try {
            size_t used = 0;
            v.d = std::stod(token, &used);
            if (used != token.size() || !std::isfinite(v.d)) {
                *err = "bad number";
                return false;
            }
        } catch (const std::exception&) {
            *err = "bad number";
            return false;
        }
        *out = std::move(v);
        return true;
    }

    bool ArrayOf(Value* out, std::string* err) {
        ++i_;
        Value arr = JsonArray();
        Skip();
        if (i_ < text_.size() && text_[i_] == ']') {
            ++i_;
            *out = std::move(arr);
            return true;
        }
        while (i_ < text_.size()) {
            Value item;
            if (!ValueOf(&item, err)) {
                return false;
            }
            Push(arr, std::move(item));
            Skip();
            if (i_ >= text_.size()) {
                break;
            }
            if (text_[i_] == ',') {
                ++i_;
                continue;
            }
            if (text_[i_] == ']') {
                ++i_;
                *out = std::move(arr);
                return true;
            }
            *err = "expected comma or ]";
            return false;
        }
        *err = "unterminated array";
        return false;
    }

    bool ObjectOf(Value* out, std::string* err) {
        ++i_;
        Value obj = JsonObject();
        Skip();
        if (i_ < text_.size() && text_[i_] == '}') {
            ++i_;
            *out = std::move(obj);
            return true;
        }
        while (i_ < text_.size()) {
            Skip();
            if (i_ >= text_.size() || text_[i_] != '"') {
                *err = "expected string key";
                return false;
            }
            Value key;
            if (!StringOf(&key, err)) {
                return false;
            }
            Skip();
            if (i_ >= text_.size() || text_[i_] != ':') {
                *err = "expected colon";
                return false;
            }
            ++i_;
            Value item;
            if (!ValueOf(&item, err)) {
                return false;
            }
            Put(obj, key.s, std::move(item));
            Skip();
            if (i_ >= text_.size()) {
                break;
            }
            if (text_[i_] == ',') {
                ++i_;
                continue;
            }
            if (text_[i_] == '}') {
                ++i_;
                *out = std::move(obj);
                return true;
            }
            *err = "expected comma or }";
            return false;
        }
        *err = "unterminated object";
        return false;
    }

    std::string_view text_;
    size_t i_ = 0;
};

void Escape(std::string* out, std::string_view s) {
    out->push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out->append("\\\"");
            break;
        case '\\':
            out->append("\\\\");
            break;
        case '\b':
            out->append("\\b");
            break;
        case '\f':
            out->append("\\f");
            break;
        case '\n':
            out->append("\\n");
            break;
        case '\r':
            out->append("\\r");
            break;
        case '\t':
            out->append("\\t");
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out->append(buf);
            } else {
                out->push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out->push_back('"');
}

void Write(std::string* out, const Value& v) {
    switch (v.kind) {
    case Value::Kind::Null:
        out->append("null");
        break;
    case Value::Kind::Bool:
        out->append(v.b ? "true" : "false");
        break;
    case Value::Kind::Number:
        if (v.is_int) {
            out->append(std::to_string(v.i));
        } else {
            std::ostringstream ss;
            ss.setf(std::ios::fmtflags(0), std::ios::floatfield);
            ss << v.d;
            out->append(ss.str());
        }
        break;
    case Value::Kind::String:
        Escape(out, v.s);
        break;
    case Value::Kind::Array:
        out->push_back('[');
        for (size_t i = 0; i < v.a.size(); ++i) {
            if (i != 0) {
                out->push_back(',');
            }
            Write(out, v.a[i]);
        }
        out->push_back(']');
        break;
    case Value::Kind::Object:
        out->push_back('{');
        for (size_t i = 0; i < v.o.size(); ++i) {
            if (i != 0) {
                out->push_back(',');
            }
            Escape(out, v.o[i].first);
            out->push_back(':');
            Write(out, v.o[i].second);
        }
        out->push_back('}');
        break;
    }
}

}  // namespace

const Value* Value::find(std::string_view key) const {
    if (kind != Kind::Object) {
        return nullptr;
    }
    for (const auto& field : o) {
        if (field.first == key) {
            return &field.second;
        }
    }
    return nullptr;
}

Value JsonNull() { return Value(); }

Value JsonBool(bool v) {
    Value out;
    out.kind = Value::Kind::Bool;
    out.b = v;
    return out;
}

Value JsonInt(int64_t v) {
    Value out;
    out.kind = Value::Kind::Number;
    out.is_int = true;
    out.i = v;
    out.d = static_cast<double>(v);
    return out;
}

Value JsonString(std::string v) {
    Value out;
    out.kind = Value::Kind::String;
    out.s = std::move(v);
    return out;
}

Value JsonArray() {
    Value out;
    out.kind = Value::Kind::Array;
    return out;
}

Value JsonObject() {
    Value out;
    out.kind = Value::Kind::Object;
    return out;
}

void Push(Value& array, Value v) { array.a.push_back(std::move(v)); }

void Put(Value& object, std::string key, Value v) {
    for (auto& field : object.o) {
        if (field.first == key) {
            field.second = std::move(v);
            return;
        }
    }
    object.o.emplace_back(std::move(key), std::move(v));
}

bool Parse(std::string_view text, Value* out, std::string* err) {
    Parser parser(text);
    std::string local;
    std::string* dest = err != nullptr ? err : &local;
    return parser.ParseValue(out, dest);
}

std::string Stringify(const Value& value) {
    std::string out;
    Write(&out, value);
    return out;
}

}  // namespace haikuide
