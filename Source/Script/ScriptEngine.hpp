#pragma once

#include "../Core/Log.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class BotManager;
struct Script;

class ScriptEngine {
public:
    explicit ScriptEngine(BotManager& manager);
    ~ScriptEngine();
    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    struct Info {
        int id = 0;
        std::string name;
        int bot = 0;
        bool running = false;
        bool failed = false;
        std::string error;
        double seconds = 0.0;
        uint64_t events = 0;
        uint64_t dropped = 0;
    };

    struct Line {
        uint64_t seq = 0;
        int script = 0;
        Log::Level level = Log::Level::Info;
        std::string text;
    };

    int Run(const std::string& name, const std::string& code, int botId = 0, bool echo = false, bool trace = false);

    enum LineState : uint8_t { LineNone = 0, LineCurrent = 1, LineDone = 2, LineError = 3 };
    void SetTrace(int id);
    bool Lines(int id, std::vector<uint8_t>& out) const;
    void Stop(int id);
    void StopAll();
    void ClearFinished();
    std::vector<Info> List() const;
    size_t RunningCount() const;

    std::vector<Line> OutputSince(uint64_t after) const;
    void ClearOutput();
    void Print(int script, Log::Level level, std::string text);

    BotManager& Manager() noexcept { return manager_; }

    static std::filesystem::path Folder();
    static std::vector<std::string> ListFiles();
    static bool LoadFile(const std::string& name, std::string& code, std::string& error);
    static bool SaveFile(const std::string& name, const std::string& code, std::string& error);
    static bool RemoveFile(const std::string& name, std::string& error);

private:
    BotManager& manager_;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Script>> scripts_;
    int nextId_ = 1;

    mutable std::mutex outputMutex_;
    std::deque<Line> output_;
    uint64_t outputSeq_ = 0;
};
