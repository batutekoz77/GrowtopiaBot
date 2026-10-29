#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Text {
    std::string_view Trim(std::string_view text) noexcept;
    std::string Lower(std::string_view text);
    std::string Upper(std::string_view text);
    bool Contains(std::string_view text, std::string_view needle) noexcept;
    bool IContains(std::string_view text, std::string_view needle);
    std::vector<std::string_view> Split(std::string_view text, char separator);

    std::optional<std::string_view> Value(std::string_view text, std::string_view key) noexcept;
    int Int(std::string_view text, std::string_view key, int fallback) noexcept;

    std::unordered_map<std::string, std::string> PipeMap(std::string_view text);

    std::optional<long long> ToInt(std::string_view text) noexcept;

    std::string UrlEncode(std::string_view text);
    std::string HtmlDecode(std::string_view text);
}
