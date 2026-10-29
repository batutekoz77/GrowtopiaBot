#include "Text.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace Text {
    std::string_view Trim(std::string_view text) noexcept {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
        return text;
    }

    std::string Lower(std::string_view text) {
        std::string out(text);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    std::string Upper(std::string_view text) {
        std::string out(text);
        for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }

    bool Contains(std::string_view text, std::string_view needle) noexcept {
        return text.find(needle) != std::string_view::npos;
    }

    bool IContains(std::string_view text, std::string_view needle) {
        return Lower(text).find(Lower(needle)) != std::string::npos;
    }

    std::vector<std::string_view> Split(std::string_view text, char separator) {
        std::vector<std::string_view> parts;
        size_t start = 0;
        for (;;) {
            const size_t end = text.find(separator, start);
            parts.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        return parts;
    }

    std::optional<std::string_view> Value(std::string_view text, std::string_view key) noexcept {
        size_t pos = 0;
        while ((pos = text.find(key, pos)) != std::string_view::npos) {
            const bool atStart = pos == 0 || text[pos - 1] == '\n' || text[pos - 1] == '|';
            const size_t after = pos + key.size();
            if (atStart && after < text.size() && text[after] == '|') {
                const size_t begin = after + 1;
                size_t end = text.find('\n', begin);
                if (end == std::string_view::npos) end = text.size();
                std::string_view value = text.substr(begin, end - begin);
                if (!value.empty() && value.back() == '\r') value.remove_suffix(1);
                return value;
            }
            pos = after;
        }
        return std::nullopt;
    }

    int Int(std::string_view text, std::string_view key, int fallback) noexcept {
        const auto value = Value(text, key);
        if (!value) return fallback;
        const auto number = ToInt(*value);
        return number ? static_cast<int>(*number) : fallback;
    }

    std::unordered_map<std::string, std::string> PipeMap(std::string_view text) {
        std::unordered_map<std::string, std::string> map;
        for (std::string_view line : Split(text, '\n')) {
            const size_t bar = line.find('|');
            const std::string_view key = Trim(line.substr(0, bar));
            if (key.empty()) continue;
            map[std::string(key)] = bar == std::string_view::npos ? std::string() : std::string(line.substr(bar + 1));
        }
        return map;
    }

    std::optional<long long> ToInt(std::string_view text) noexcept {
        text = Trim(text);
        long long value = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || ptr != text.data() + text.size()) return std::nullopt;
        return value;
    }

    std::string UrlEncode(std::string_view text) {
        static constexpr char HEX[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(text.size() * 3);
        for (const unsigned char c : text) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
            else if (c == ' ') out += '+';
            else {
                out += '%';
                out += HEX[c >> 4];
                out += HEX[c & 0xF];
            }
        }
        return out;
    }

    std::string HtmlDecode(std::string_view text) {
        static constexpr std::pair<std::string_view, char> ENTITIES[] = {
            { "&amp;", '&' }, { "&quot;", '"' }, { "&#39;", '\'' }, { "&#039;", '\'' }, { "&lt;", '<' }, { "&gt;", '>' },
        };
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size();) {
            bool replaced = false;
            if (text[i] == '&') {
                for (const auto& [entity, c] : ENTITIES) {
                    if (text.substr(i).starts_with(entity)) {
                        out += c;
                        i += entity.size();
                        replaced = true;
                        break;
                    }
                }
            }
            if (!replaced) out += text[i++];
        }
        return out;
    }
}
