#pragma once

#include "ScriptEngine.hpp"
#include "../Bot/Events.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <vector>

struct Script final : Events::Listener {
    using Clock = std::chrono::steady_clock;

    Script(ScriptEngine& engine, int id, std::string name, std::string code, int bot, bool echo, bool trace);
    ~Script() override;

    ScriptEngine& engine;
    const int id;
    const std::string name;
    const std::string code;
    const int defaultBot;
    const bool echo;

    std::thread thread;
    std::atomic<bool> stop{ false };
    std::atomic<bool> exitRequested{ false };
    std::atomic<bool> running{ true };
    std::atomic<bool> failed{ false };
    std::atomic<uint64_t> handled{ 0 };
    std::atomic<uint64_t> dropped{ 0 };

    mutable std::mutex infoMutex;
    Clock::time_point started = Clock::now();
    Clock::time_point finished{};
    std::string error;

    mutable std::mutex eventMutex;
    std::condition_variable cv;
    std::deque<Events::Event> queue;
    std::map<std::string, int, std::less<>> wanted;

    bool Wants(std::string_view name) const override;
    void Push(const Events::Event& event) override;
    void AddWanted(const std::string& event);
    void RemoveWanted(const std::string& event);
    void RequestStop();

    struct Handler {
        int id = 0;
        std::string event;
        int ref = 0;
        int bot = 0;
    };
    std::vector<Handler> handlers;
    int nextHandler = 1;
    int handlerDepth = 0;
    std::deque<Events::Event> deferred;

    std::atomic<bool> trace{ false };
    const int lineCount;
    std::unique_ptr<std::atomic<uint8_t>[]> lines;
    struct Frame {
        const void* ci = nullptr;
        int defined = 0;
        int line = 0;
        int resetFrom = 0, resetTo = 0;
        std::vector<uint8_t> saved;
    };
    std::vector<Frame> frames;
    bool tracing = false;
    bool everTraced = false;
    const char* chunkSource = nullptr;
    void SetLine(int line, uint8_t state) { if (line >= 1 && line <= lineCount) lines[line - 1].store(state, std::memory_order_relaxed); }
    uint8_t GetLine(int line) const { return line >= 1 && line <= lineCount ? lines[line - 1].load(std::memory_order_relaxed) : 0; }
    void SettleLoop(Frame& frame);
    void FinishLines(const std::string& failure, bool stopped);

    void Start();

private:
    void Main();
};
