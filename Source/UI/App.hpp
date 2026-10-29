#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

class BotManager;
class ScriptEngine;

class App {
public:
    App(BotManager& manager, ScriptEngine& scripts);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    struct Options {
        int autoCloseSeconds = 0;
        bool idleThrottle = true;
        std::string tab;
        std::string script;
    };

    bool Run(const Options& options);
    bool Run() { return Run(Options{}); }

    uint64_t Frames() const noexcept { return frames_; }
    uint64_t UiCycles() const noexcept { return uiCycles_; }
    uint64_t GpuCycles() const noexcept { return gpuCycles_; }
    uint64_t FirstFrameCycles() const noexcept { return firstFrameCycles_; }
    std::chrono::steady_clock::time_point FirstFrameAt() const noexcept { return firstFrameAt_; }

private:
    struct State;
    std::unique_ptr<State> state_;
    BotManager& manager_;
    ScriptEngine& scripts_;
    uint64_t frames_ = 0;
    uint64_t uiCycles_ = 0;
    uint64_t gpuCycles_ = 0;
    uint64_t firstFrameCycles_ = 0;
    std::chrono::steady_clock::time_point firstFrameAt_{};

    void Frame();
    void DrawHeader();
    void DrawBotList();
    void DrawOverviewTab();
    void DrawWorldTab();
    void DrawInventoryTab();
    void DrawPlayersTab();
    void DrawScriptsTab();
    void DrawEditor(float height);
    void DrawLogTab();
    void DrawSettingsTab();
    void DrawStatusBar();
    void Refresh();
};
