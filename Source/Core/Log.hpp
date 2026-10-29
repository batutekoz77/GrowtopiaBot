#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace Log {
    enum class Level : uint8_t { Info, Warn, Error, Debug };

    struct Entry {
        uint64_t seq = 0;
        Level level = Level::Info;
        char time[9]{};
        std::string tag;
        std::string message;
    };

    void Write(Level level, std::string_view tag, std::string_view message);
    bool DebugEnabled() noexcept;

    template <typename... Args>
    void Info(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
        Write(Level::Info, tag, std::format(fmt, std::forward<Args>(args)...));
    }
    template <typename... Args>
    void Warn(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
        Write(Level::Warn, tag, std::format(fmt, std::forward<Args>(args)...));
    }
    template <typename... Args>
    void Error(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
        Write(Level::Error, tag, std::format(fmt, std::forward<Args>(args)...));
    }
    template <typename... Args>
    void Debug(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
        if (!DebugEnabled()) return;
        Write(Level::Debug, tag, std::format(fmt, std::forward<Args>(args)...));
    }

    void EnableDebug(bool enabled) noexcept;
    void EnableConsole(bool enabled) noexcept;

    std::vector<Entry> Since(uint64_t after);
    uint64_t LastSeq() noexcept;
    void Clear();
}
