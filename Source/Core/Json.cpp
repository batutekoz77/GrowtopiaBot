#include "Json.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <stdexcept>

namespace {
    const Json NULL_JSON;
    const Json::Array EMPTY_ARRAY;
    const Json::Object EMPTY_OBJECT;

    class Parser {
    public:
        explicit Parser(std::string_view text) : text_(text) {}

        Json Document() {
            Json value = Value();
            Skip();
            if (pos_ != text_.size()) Fail("trailing characters");
            return value;
        }

    private:
        std::string_view text_;
        size_t pos_ = 0;

        [[noreturn]] void Fail(std::string_view what) const {
            throw std::runtime_error(std::format("JSON: {} at offset {}", what, pos_));
        }

        void Skip() {
            while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) ++pos_;
        }

        char Peek() {
            Skip();
            if (pos_ >= text_.size()) Fail("unexpected end");
            return text_[pos_];
        }

        void Expect(std::string_view word) {
            if (text_.substr(pos_, word.size()) != word) Fail("unexpected token");
            pos_ += word.size();
        }

        Json Value() {
            switch (Peek()) {
                case '{': return ObjectValue();
                case '[': return ArrayValue();
                case '"': return Json(StringValue());
                case 't': Expect("true"); return Json(true);
                case 'f': Expect("false"); return Json(false);
                case 'n': Expect("null"); return Json(nullptr);
                default:  return NumberValue();
            }
        }

        Json ObjectValue() {
            Json::Object object;
            ++pos_;
            if (Peek() == '}') { ++pos_; return Json(std::move(object)); }
            for (;;) {
                if (Peek() != '"') Fail("expected a key");
                std::string key = StringValue();
                if (Peek() != ':') Fail("expected ':'");
                ++pos_;
                object[std::move(key)] = Value();
                const char c = Peek();
                ++pos_;
                if (c == '}') break;
                if (c != ',') Fail("expected ',' or '}'");
            }
            return Json(std::move(object));
        }

        Json ArrayValue() {
            Json::Array array;
            ++pos_;
            if (Peek() == ']') { ++pos_; return Json(std::move(array)); }
            for (;;) {
                array.push_back(Value());
                const char c = Peek();
                ++pos_;
                if (c == ']') break;
                if (c != ',') Fail("expected ',' or ']'");
            }
            return Json(std::move(array));
        }

        static void AppendUtf8(std::string& out, uint32_t cp) {
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        uint32_t Hex4() {
            if (pos_ + 4 > text_.size()) Fail("bad \\u escape");
            uint32_t value = 0;
            const auto [ptr, ec] = std::from_chars(text_.data() + pos_, text_.data() + pos_ + 4, value, 16);
            if (ec != std::errc{} || ptr != text_.data() + pos_ + 4) Fail("bad \\u escape");
            pos_ += 4;
            return value;
        }

        std::string StringValue() {
            std::string out;
            ++pos_;
            while (pos_ < text_.size()) {
                const char c = text_[pos_++];
                if (c == '"') return out;
                if (c != '\\') { out += c; continue; }
                if (pos_ >= text_.size()) break;
                const char e = text_[pos_++];
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
                        uint32_t cp = Hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF && text_.substr(pos_, 2) == "\\u") {
                            pos_ += 2;
                            const uint32_t low = Hex4();
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default: Fail("bad escape");
                }
            }
            Fail("unterminated string");
        }

        Json NumberValue() {
            const size_t start = pos_;
            while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '-' ||
                   text_[pos_] == '+' || text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) ++pos_;
            double value = 0.0;
            const auto [ptr, ec] = std::from_chars(text_.data() + start, text_.data() + pos_, value);
            if (start == pos_ || ec != std::errc{} || ptr != text_.data() + pos_) Fail("bad number");
            return Json(value);
        }
    };

    void Escape(std::string& out, std::string_view text) {
        out += '"';
        for (const unsigned char c : text) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) out += std::format("\\u{:04x}", c);
                    else out += static_cast<char>(c);
            }
        }
        out += '"';
    }
}

Json Json::Parse(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    return Parser(text).Document();
}

std::string Json::Dump(int indent) const {
    std::string out;
    DumpTo(out, indent, 0);
    return out;
}

void Json::DumpTo(std::string& out, int indent, int depth) const {
    const auto newline = [&](int level) {
        if (indent <= 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * level), ' ');
    };

    if (IsNull()) out += "null";
    else if (IsBool()) out += std::get<bool>(value_) ? "true" : "false";
    else if (IsNumber()) {
        const double n = std::get<double>(value_);
        if (std::floor(n) == n && std::fabs(n) < 9.0e15) out += std::to_string(static_cast<long long>(n));
        else out += std::format("{}", n);
    }
    else if (IsString()) Escape(out, std::get<std::string>(value_));
    else if (IsArray()) {
        const Array& array = Items();
        out += '[';
        for (size_t i = 0; i < array.size(); ++i) {
            if (i) out += ',';
            newline(depth + 1);
            array[i].DumpTo(out, indent, depth + 1);
        }
        if (!array.empty()) newline(depth);
        out += ']';
    }
    else {
        const Object& object = Members();
        out += '{';
        bool first = true;
        for (const auto& [key, value] : object) {
            if (!first) out += ',';
            first = false;
            newline(depth + 1);
            Escape(out, key);
            out += indent > 0 ? ": " : ":";
            value.DumpTo(out, indent, depth + 1);
        }
        if (!object.empty()) newline(depth);
        out += '}';
    }
}

const Json& Json::operator[](std::string_view key) const {
    if (!IsObject()) return NULL_JSON;
    const Object& object = Members();
    const auto it = object.find(key);
    return it == object.end() ? NULL_JSON : it->second;
}

const Json& Json::operator[](size_t index) const {
    if (!IsArray() || index >= Items().size()) return NULL_JSON;
    return Items()[index];
}

size_t Json::Size() const noexcept {
    if (IsArray()) return std::get<std::shared_ptr<Array>>(value_)->size();
    if (IsObject()) return std::get<std::shared_ptr<Object>>(value_)->size();
    return 0;
}

std::string Json::String(std::string_view fallback) const {
    if (IsString()) return std::get<std::string>(value_);
    if (IsNumber()) return Dump(0);
    return std::string(fallback);
}

long long Json::Int(long long fallback) const {
    if (IsNumber()) return static_cast<long long>(std::get<double>(value_));
    if (IsString()) {
        long long value = 0;
        const std::string& s = std::get<std::string>(value_);
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (ec == std::errc{} && ptr == s.data() + s.size()) return value;
    }
    return fallback;
}

double Json::Number(double fallback) const {
    return IsNumber() ? std::get<double>(value_) : fallback;
}

bool Json::Bool(bool fallback) const {
    return IsBool() ? std::get<bool>(value_) : fallback;
}

const Json::Array& Json::Items() const {
    return IsArray() ? *std::get<std::shared_ptr<Array>>(value_) : EMPTY_ARRAY;
}

const Json::Object& Json::Members() const {
    return IsObject() ? *std::get<std::shared_ptr<Object>>(value_) : EMPTY_OBJECT;
}

Json& Json::Set(std::string key, Json value) {
    if (!IsObject()) value_ = std::make_shared<Object>();
    auto& object = std::get<std::shared_ptr<Object>>(value_);
    if (object.use_count() > 1) object = std::make_shared<Object>(*object);
    (*std::get<std::shared_ptr<Object>>(value_))[std::move(key)] = std::move(value);
    return *this;
}

Json& Json::Push(Json value) {
    if (!IsArray()) value_ = std::make_shared<Array>();
    auto& array = std::get<std::shared_ptr<Array>>(value_);
    if (array.use_count() > 1) array = std::make_shared<Array>(*array);
    std::get<std::shared_ptr<Array>>(value_)->push_back(std::move(value));
    return *this;
}
