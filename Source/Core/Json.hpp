#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : value_(value) {}
    Json(double value) : value_(value) {}
    Json(int value) : value_(static_cast<double>(value)) {}
    Json(long long value) : value_(static_cast<double>(value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(const char* value) : value_(std::string(value)) {}
    Json(Array value) : value_(std::make_shared<Array>(std::move(value))) {}
    Json(Object value) : value_(std::make_shared<Object>(std::move(value))) {}

    static Json Parse(std::string_view text);
    std::string Dump(int indent = 2) const;

    bool IsNull() const noexcept { return std::holds_alternative<std::monostate>(value_); }
    bool IsBool() const noexcept { return std::holds_alternative<bool>(value_); }
    bool IsNumber() const noexcept { return std::holds_alternative<double>(value_); }
    bool IsString() const noexcept { return std::holds_alternative<std::string>(value_); }
    bool IsArray() const noexcept { return std::holds_alternative<std::shared_ptr<Array>>(value_); }
    bool IsObject() const noexcept { return std::holds_alternative<std::shared_ptr<Object>>(value_); }

    const Json& operator[](std::string_view key) const;
    const Json& operator[](size_t index) const;
    size_t Size() const noexcept;

    std::string String(std::string_view fallback = {}) const;
    long long Int(long long fallback = 0) const;
    double Number(double fallback = 0.0) const;
    bool Bool(bool fallback = false) const;

    const Array& Items() const;
    const Object& Members() const;

    Json& Set(std::string key, Json value);
    Json& Push(Json value);

private:
    std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> value_;

    void DumpTo(std::string& out, int indent, int depth) const;
};
