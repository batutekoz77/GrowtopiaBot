#include "Log.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>

namespace Log {
    namespace {
        constexpr size_t RING_SIZE = 4000;

        std::mutex gMutex;
        std::deque<Entry> gRing;
        std::atomic<uint64_t> gSeq{ 0 };
        std::atomic<bool> gDebug{ false };
        std::atomic<bool> gConsole{ false };
        std::once_flag gConsoleSetup;

        const char* Color(Level level) noexcept {
            switch (level) {
                case Level::Info:  return "\033[32m";
                case Level::Warn:  return "\033[33m";
                case Level::Error: return "\033[31m";
                case Level::Debug: return "\033[36m";
            }
            return "\033[0m";
        }

        const char* Name(Level level) noexcept {
            switch (level) {
                case Level::Info:  return "INFO ";
                case Level::Warn:  return "WARN ";
                case Level::Error: return "ERROR";
                case Level::Debug: return "DEBUG";
            }
            return "";
        }

        void FormatTime(char (&out)[9]) {
            thread_local std::time_t cachedAt = 0;
            thread_local char cached[9]{};
            const std::time_t now = std::time(nullptr);
            if (now != cachedAt) {
                std::tm tm{};
                localtime_s(&tm, &now);
                std::snprintf(cached, sizeof(cached), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
                cachedAt = now;
            }
            std::memcpy(out, cached, sizeof(cached));
        }
    }

    void EnableDebug(bool enabled) noexcept { gDebug = enabled; }
    bool DebugEnabled() noexcept { return gDebug.load(std::memory_order_relaxed); }
    void EnableConsole(bool enabled) noexcept { gConsole = enabled; }

    void Write(Level level, std::string_view tag, std::string_view message) {
        if (level == Level::Debug && !gDebug) return;

        Entry entry;
        entry.level = level;
        FormatTime(entry.time);
        entry.tag.assign(tag);
        entry.message.assign(message);

        const bool console = gConsole.load(std::memory_order_relaxed);
        if (console) {
            std::call_once(gConsoleSetup, [] {
                HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
                DWORD mode = 0;
                if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
                SetConsoleOutputCP(CP_UTF8);
            });
        }

        std::lock_guard lock(gMutex);
        entry.seq = gSeq.fetch_add(1, std::memory_order_relaxed) + 1;
        if (console) {
            std::printf("%s[%s] [%s] [%.*s] %.*s\033[0m\n", Color(level), entry.time, Name(level),
                static_cast<int>(tag.size()), tag.data(), static_cast<int>(message.size()), message.data());
            std::fflush(stdout);
        }
        if (gRing.size() >= RING_SIZE) gRing.pop_front();
        gRing.push_back(std::move(entry));
    }

    std::vector<Entry> Since(uint64_t after) {
        std::lock_guard lock(gMutex);
        std::vector<Entry> out;
        if (gRing.empty() || gRing.back().seq <= after) return out;
        const uint64_t first = gRing.front().seq;
        const size_t start = after < first ? 0 : static_cast<size_t>(after - first + 1);
        out.reserve(gRing.size() - start);
        for (size_t i = start; i < gRing.size(); ++i) out.push_back(gRing[i]);
        return out;
    }

    uint64_t LastSeq() noexcept { return gSeq.load(std::memory_order_relaxed); }

    void Clear() {
        std::lock_guard lock(gMutex);
        gRing.clear();
    }
}
