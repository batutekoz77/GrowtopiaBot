#include "App.hpp"
#include "../Bot/BotManager.hpp"
#include "../Core/Log.hpp"
#include "../Core/Text.hpp"
#include "../Game/Items.hpp"
#include "../Script/ScriptEngine.hpp"

#include "ImGui/imgui.h"
#include "ImGui/imgui_internal.h"
#include "ImGui/imgui_impl_dx11.h"
#include "ImGui/imgui_impl_win32.h"
#include "ImGui/imgui_stdlib.h"

#include <windows.h>
#include <d3d11.h>
#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <thread>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    constexpr auto REFRESH_EVERY = 100ms;
    constexpr size_t LOG_LIMIT = 4000;
    constexpr int REACH = Pathfinder::REACH;

    const char* DEFAULT_SCRIPT =
        "-- GrowtopiaBot Lua script -- see Docs/LUA_API.md\n"
        "local bot = getBot()\n"
        "print(\"hello from\", bot.name, \"status:\", bot.status)\n"
        "\n"
        "if not bot.online then\n"
        "    bot:connect()\n"
        "    bot:waitForOnline(60000)\n"
        "end\n"
        "\n"
        "if bot:warpAndWait(\"START\", 20000) then\n"
        "    bot:say(\"hi!\")\n"
        "end\n";

    ID3D11Device* gDevice = nullptr;
    ID3D11DeviceContext* gContext = nullptr;
    IDXGISwapChain* gSwapChain = nullptr;
    ID3D11RenderTargetView* gTarget = nullptr;
    UINT gResizeWidth = 0, gResizeHeight = 0;
    bool gOccluded = false;

    void CreateTarget() {
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(gSwapChain->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
            gDevice->CreateRenderTargetView(back, nullptr, &gTarget);
            back->Release();
        }
    }

    void ReleaseTarget() {
        if (gTarget) { gTarget->Release(); gTarget = nullptr; }
    }

    bool CreateDevice(HWND hwnd) {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
            &sd, &gSwapChain, &gDevice, &level, &gContext);
        if (hr == DXGI_ERROR_UNSUPPORTED) {
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                &sd, &gSwapChain, &gDevice, &level, &gContext);
        }
        if (FAILED(hr)) return false;
        CreateTarget();
        return true;
    }

    void ReleaseDevice() {
        ReleaseTarget();
        if (gSwapChain) { gSwapChain->Release(); gSwapChain = nullptr; }
        if (gContext) { gContext->Release(); gContext = nullptr; }
        if (gDevice) { gDevice->Release(); gDevice = nullptr; }
    }

    LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
        switch (msg) {
            case WM_SIZE:
                if (wParam == SIZE_MINIMIZED) return 0;
                gResizeWidth = LOWORD(lParam);
                gResizeHeight = HIWORD(lParam);
                return 0;
            case WM_SYSCOMMAND:
                if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
                break;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    float gScale = 1.0f;
    float S(float v) { return v * gScale; }

    namespace Theme {
        constexpr ImVec4 Bg{ 0.039f, 0.055f, 0.086f, 1.0f };
        constexpr ImVec4 Surface{ 0.063f, 0.086f, 0.133f, 1.0f };
        constexpr ImVec4 Raised{ 0.086f, 0.118f, 0.180f, 1.0f };
        constexpr ImVec4 Hover{ 0.110f, 0.153f, 0.231f, 1.0f };
        constexpr ImVec4 Line{ 0.137f, 0.184f, 0.267f, 1.0f };
        constexpr ImVec4 Text{ 0.910f, 0.937f, 0.973f, 1.0f };
        constexpr ImVec4 Muted{ 0.525f, 0.588f, 0.690f, 1.0f };
        constexpr ImVec4 Faint{ 0.340f, 0.392f, 0.478f, 1.0f };
        constexpr ImVec4 Aqua{ 0.000f, 0.714f, 1.000f, 1.0f };
        constexpr ImVec4 AquaDeep{ 0.031f, 0.420f, 0.776f, 1.0f };
        constexpr ImVec4 Teal{ 0.176f, 0.831f, 0.749f, 1.0f };
        constexpr ImVec4 Gold{ 0.961f, 0.769f, 0.318f, 1.0f };
        constexpr ImVec4 Green{ 0.204f, 0.827f, 0.600f, 1.0f };
        constexpr ImVec4 Yellow{ 0.984f, 0.749f, 0.141f, 1.0f };
        constexpr ImVec4 Orange{ 0.984f, 0.573f, 0.235f, 1.0f };
        constexpr ImVec4 Red{ 0.973f, 0.443f, 0.443f, 1.0f };
        constexpr ImVec4 Violet{ 0.655f, 0.545f, 0.980f, 1.0f };
    }

    ImVec4 Alpha(ImVec4 c, float a) { return { c.x, c.y, c.z, a }; }
    ImU32 Col(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
    ImU32 Col(ImVec4 c, float a) { return ImGui::ColorConvertFloat4ToU32(Alpha(c, a)); }

    ImVec4 StatusColor(BotStatus status) {
        switch (status) {
            case BotStatus::InWorld:      return Theme::Green;
            case BotStatus::InGame:
            case BotStatus::JoiningWorld: return Theme::Aqua;
            case BotStatus::LoggingIn:
            case BotStatus::Connecting:
            case BotStatus::EnteringGame: return Theme::Yellow;
            case BotStatus::Waiting:      return Theme::Orange;
            case BotStatus::Stopped:      return Theme::Red;
            default:                      return Theme::Muted;
        }
    }

    ImVec4 LevelColor(Log::Level level) {
        switch (level) {
            case Log::Level::Warn:  return Theme::Yellow;
            case Log::Level::Error: return Theme::Red;
            case Log::Level::Debug: return Alpha(Theme::Aqua, 0.85f);
            default:                return Theme::Text;
        }
    }

    ImVec4 LineColor(uint8_t state) {
        switch (state) {
            case ScriptEngine::LineCurrent: return Theme::Yellow;
            case ScriptEngine::LineDone:    return Theme::Green;
            case ScriptEngine::LineError:   return Theme::Red;
            default:                        return Theme::Faint;
        }
    }

    void ApplyStyle(float scale) {
        ImGuiStyle& style = ImGui::GetStyle();
        ImGui::StyleColorsDark();
        style.WindowRounding = 0.0f;
        style.ChildRounding = 12.0f;
        style.FrameRounding = 8.0f;
        style.GrabRounding = 8.0f;
        style.PopupRounding = 10.0f;
        style.TabRounding = 8.0f;
        style.ScrollbarRounding = 10.0f;
        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.TabBarBorderSize = 0.0f;
        style.TabBarOverlineSize = 0.0f;
        style.WindowPadding = { 16.0f, 14.0f };
        style.FramePadding = { 11.0f, 7.0f };
        style.ItemSpacing = { 10.0f, 9.0f };
        style.ItemInnerSpacing = { 8.0f, 6.0f };
        style.CellPadding = { 10.0f, 7.0f };
        style.ScrollbarSize = 12.0f;
        style.GrabMinSize = 12.0f;
        style.IndentSpacing = 18.0f;
        style.SeparatorTextBorderSize = 1.0f;
        style.SelectableTextAlign = { 0.0f, 0.5f };

        using namespace Theme;
        ImVec4* c = style.Colors;
        c[ImGuiCol_Text] = Text;
        c[ImGuiCol_TextDisabled] = Muted;
        c[ImGuiCol_WindowBg] = Bg;
        c[ImGuiCol_ChildBg] = Surface;
        c[ImGuiCol_PopupBg] = Surface;
        c[ImGuiCol_Border] = Line;
        c[ImGuiCol_BorderShadow] = { 0, 0, 0, 0 };
        c[ImGuiCol_FrameBg] = Raised;
        c[ImGuiCol_FrameBgHovered] = Hover;
        c[ImGuiCol_FrameBgActive] = { 0.13f, 0.18f, 0.27f, 1.0f };
        c[ImGuiCol_TitleBg] = Bg;
        c[ImGuiCol_TitleBgActive] = Surface;
        c[ImGuiCol_MenuBarBg] = Surface;
        c[ImGuiCol_ScrollbarBg] = { 0, 0, 0, 0 };
        c[ImGuiCol_ScrollbarGrab] = Line;
        c[ImGuiCol_ScrollbarGrabHovered] = { 0.20f, 0.26f, 0.36f, 1.0f };
        c[ImGuiCol_ScrollbarGrabActive] = AquaDeep;
        c[ImGuiCol_Header] = Alpha(Aqua, 0.16f);
        c[ImGuiCol_HeaderHovered] = Alpha(Aqua, 0.10f);
        c[ImGuiCol_HeaderActive] = Alpha(Aqua, 0.24f);
        c[ImGuiCol_Button] = Raised;
        c[ImGuiCol_ButtonHovered] = Hover;
        c[ImGuiCol_ButtonActive] = { 0.13f, 0.18f, 0.27f, 1.0f };
        c[ImGuiCol_Tab] = { 0, 0, 0, 0 };
        c[ImGuiCol_TabHovered] = Alpha(Aqua, 0.12f);
        c[ImGuiCol_TabSelected] = Alpha(Aqua, 0.18f);
        c[ImGuiCol_TabDimmed] = { 0, 0, 0, 0 };
        c[ImGuiCol_TabDimmedSelected] = Alpha(Aqua, 0.10f);
        c[ImGuiCol_CheckMark] = Aqua;
        c[ImGuiCol_SliderGrab] = Aqua;
        c[ImGuiCol_SliderGrabActive] = Teal;
        c[ImGuiCol_ResizeGrip] = { 0, 0, 0, 0 };
        c[ImGuiCol_ResizeGripHovered] = Alpha(Aqua, 0.4f);
        c[ImGuiCol_ResizeGripActive] = Aqua;
        c[ImGuiCol_PlotHistogram] = Aqua;
        c[ImGuiCol_TableHeaderBg] = Raised;
        c[ImGuiCol_TableBorderStrong] = Line;
        c[ImGuiCol_TableBorderLight] = Alpha(Line, 0.6f);
        c[ImGuiCol_TableRowBg] = { 0, 0, 0, 0 };
        c[ImGuiCol_TableRowBgAlt] = { 1.0f, 1.0f, 1.0f, 0.022f };
        c[ImGuiCol_Separator] = Line;
        c[ImGuiCol_SeparatorHovered] = Alpha(Aqua, 0.6f);
        c[ImGuiCol_SeparatorActive] = Aqua;
        c[ImGuiCol_TextSelectedBg] = Alpha(Aqua, 0.30f);
        c[ImGuiCol_NavCursor] = Alpha(Aqua, 0.8f);
        c[ImGuiCol_ModalWindowDimBg] = { 0.0f, 0.0f, 0.0f, 0.55f };
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;
    }

    bool ColorButton(const char* label, ImVec4 color, ImVec2 size = {}) {
        ImGui::PushStyleColor(ImGuiCol_Button, Alpha(color, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Alpha(color, 0.70f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.02f, 0.05f, 0.09f, 1.0f));
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(4);
        return pressed;
    }

    bool TintButton(const char* label, ImVec4 color, ImVec2 size = {}) {
        ImGui::PushStyleColor(ImGuiCol_Button, Alpha(color, 0.13f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Alpha(color, 0.26f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Alpha(color, 0.38f));
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(4);
        return pressed;
    }

    bool PrimaryButton(const char* label, ImVec2 size = {}) { return ColorButton(label, Theme::Aqua, size); }
    bool DangerButton(const char* label, ImVec2 size = {}) { return TintButton(label, Theme::Red, size); }

    void Chip(const char* text, ImVec4 color, bool dot = true) {
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const float padX = S(9.0f), height = textSize.y + S(5.0f);
        const float dotSpace = dot ? S(12.0f) : 0.0f;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size{ textSize.x + padX * 2 + dotSpace, height };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, { pos.x + size.x, pos.y + size.y }, Col(color, 0.14f), height * 0.5f);
        if (dot) dl->AddCircleFilled({ pos.x + padX + S(3.0f), pos.y + height * 0.5f }, S(3.5f), Col(color));
        dl->AddText({ pos.x + padX + dotSpace, pos.y + (height - textSize.y) * 0.5f }, Col(color), text);
        ImGui::Dummy(size);
    }

    bool BeginCard(const char* id, float width = 0.0f) {
        return ImGui::BeginChild(id, ImVec2(width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    }
    void EndCard() { ImGui::EndChild(); }

    void Caption(const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }

    enum class Tool { Punch, Place, Wrench, Door, Walk };
    struct ToolInfo { const char* name; const char* verb; ImVec4 color; };
    constexpr std::array<ToolInfo, 5> TOOLS{ {
        { "Punch", "punch", Theme::Orange },
        { "Place", "place", Theme::Green },
        { "Wrench", "wrench", Theme::Gold },
        { "Door", "enter", Theme::Violet },
        { "Walk", "walk to", Theme::Aqua },
    } };
}

struct App::State {
    HWND hwnd = nullptr;
    ImFont* mono = nullptr;
    ImFont* bold = nullptr;

    struct Row {
        int id = 0;
        std::string growId, name, world, statusText;
        BotStatus status = BotStatus::Idle;
        int32_t gems = 0;
        uint32_t ping = 0;
        size_t items = 0;
        size_t players = 0;
    };
    std::vector<Row> rows;
    std::shared_ptr<Bot> selected;
    BotSnapshot snapshot;
    Clock::time_point nextRefresh{};
    std::string botFilter;

    std::string warpWorld, warpDoor, targetWorld, sayText, rawText;
    int walkX = 0, walkY = 0, placeItem = 2, dropAmount = 0;
    Tool tool = Tool::Punch;
    std::string inventoryFilter;
    std::string openTab;

    std::string newGrowId, newPassword, newProxy;
    bool startNew = true;
    std::string addError;

    std::deque<Log::Entry> log;
    uint64_t logSeq = 0;
    std::deque<uint64_t> logView;
    std::string logFilter, appliedFilter;
    bool showInfo = true, showWarn = true, showError = true, showDebug = true;
    bool appliedLevels[4]{ true, true, true, true };
    bool logAutoScroll = true;
    bool logDirty = true;

    bool LogPasses(const Log::Entry& e) const {
        return appliedLevels[static_cast<int>(e.level)] &&
            (appliedFilter.empty() || Text::IContains(e.message, appliedFilter) || Text::IContains(e.tag, appliedFilter));
    }

    std::vector<std::string> files;
    Clock::time_point nextFileScan{};
    std::string fileName = "new.lua";
    std::string editor = DEFAULT_SCRIPT;
    std::string savedEditor = DEFAULT_SCRIPT;
    std::string scriptMessage;
    std::vector<ScriptEngine::Line> output;
    uint64_t outputSeq = 0;
    bool outputAutoScroll = true;
    std::string console;
    std::vector<std::string> history;
    int historyPos = -1;
    int traceId = 0;
    std::string traceCode;
    std::vector<uint8_t> lineStates;

    bool settingsLoaded = false;
    Config::Settings settings;
    std::string settingsMessage;

    Clock::time_point nextStats{};
    ULONGLONG lastCpu = 0;
    Clock::time_point lastCpuAt{};
    double cpuPercent = 0.0;
    double memoryMb = 0.0;
    unsigned cores = std::max(1u, std::thread::hardware_concurrency());
};

App::App(BotManager& manager, ScriptEngine& scripts) : state_(std::make_unique<State>()), manager_(manager), scripts_(scripts) {}
App::~App() = default;

bool App::Run(const Options& options) {
    State& st = *state_;
    frames_ = uiCycles_ = gpuCycles_ = 0;
    const auto closeAt = options.autoCloseSeconds > 0 ? Clock::now() + std::chrono::seconds(options.autoCloseSeconds) : Clock::time_point::max();
    auto lastInput = Clock::now();
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    gScale = scale;

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, LoadCursorW(nullptr, IDC_ARROW),
        nullptr, nullptr, L"GrowtopiaBotPanel", nullptr };
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    st.hwnd = CreateWindowW(wc.lpszClassName, L"GrowtopiaBot", WS_OVERLAPPEDWINDOW, 80, 60,
        static_cast<int>(1480 * scale), static_cast<int>(900 * scale), nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDevice(st.hwnd)) {
        ReleaseDevice();
        DestroyWindow(st.hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }
    ShowWindow(st.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(st.hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    static const std::string iniPath = (Config::ExeDirectory() / "panel.ini").string();
    io.IniFilename = iniPath.c_str();
    ApplyStyle(scale);

    const std::filesystem::path fonts = "C:\\Windows\\Fonts";
    const auto load = [&](const char* file) -> ImFont* {
        return std::filesystem::exists(fonts / file) ? io.Fonts->AddFontFromFileTTF((fonts / file).string().c_str()) : nullptr;
    };
    ImGui::GetStyle().FontSizeBase = 16.0f;
    ImFont* ui = load("segoeui.ttf");
    if (!ui) ui = io.Fonts->AddFontDefault();
    st.bold = load("seguisb.ttf");
    if (!st.bold) st.bold = ui;
    st.mono = load("consola.ttf");

    ImGui_ImplWin32_Init(st.hwnd);
    ImGui_ImplDX11_Init(gDevice, gContext);

    st.openTab = options.tab;
    if (!options.script.empty()) {
        std::string error;
        if (ScriptEngine::LoadFile(options.script, st.editor, error)) {
            st.fileName = options.script;
            st.savedEditor = st.editor;
            st.traceId = scripts_.Run(st.fileName, st.editor, 0, false, true);
            st.traceCode = st.editor;
        }
    }

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if ((msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) || (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST))
                lastInput = Clock::now();
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done || Clock::now() >= closeAt) break;

        if (IsIconic(st.hwnd) || (gOccluded && gSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            continue;
        }
        gOccluded = false;
        if (GetForegroundWindow() != st.hwnd) MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        else if (options.idleThrottle && Clock::now() - lastInput > 1s) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
        }

        if (gResizeWidth != 0 && gResizeHeight != 0) {
            ReleaseTarget();
            gSwapChain->ResizeBuffers(0, gResizeWidth, gResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            gResizeWidth = gResizeHeight = 0;
            CreateTarget();
        }

        ULONG64 c0 = 0, c1 = 0, c2 = 0;
        QueryThreadCycleTime(GetCurrentThread(), &c0);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Frame();
        ImGui::Render();
        QueryThreadCycleTime(GetCurrentThread(), &c1);

        const float clear[4] = { Theme::Bg.x, Theme::Bg.y, Theme::Bg.z, 1.0f };
        gContext->OMSetRenderTargets(1, &gTarget, nullptr);
        gContext->ClearRenderTargetView(gTarget, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        gOccluded = gSwapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
        QueryThreadCycleTime(GetCurrentThread(), &c2);
        uiCycles_ += c1 - c0;
        gpuCycles_ += c2 - c1;
        if (++frames_ == 1) {
            firstFrameCycles_ = c2;
            firstFrameAt_ = Clock::now();
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ReleaseDevice();
    DestroyWindow(st.hwnd);
    MSG leftover;
    while (PeekMessageW(&leftover, nullptr, 0, 0, PM_REMOVE)) {}
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    gOccluded = false;
    gResizeWidth = gResizeHeight = 0;
    return true;
}

void App::Refresh() {
    State& st = *state_;
    const auto now = Clock::now();

    auto fresh = Log::Since(st.logSeq);
    if (!fresh.empty()) {
        st.logSeq = fresh.back().seq;
        if (!st.log.empty() && fresh.front().seq != st.log.back().seq + 1) {
            st.log.clear();
            st.logDirty = true;
        }
        for (auto& e : fresh) {
            if (!st.logDirty && st.LogPasses(e)) st.logView.push_back(e.seq);
            st.log.push_back(std::move(e));
        }
        while (st.log.size() > LOG_LIMIT) st.log.pop_front();
        while (!st.logView.empty() && st.logView.front() < st.log.front().seq) st.logView.pop_front();
    }
    auto lines = scripts_.OutputSince(st.outputSeq);
    if (!lines.empty()) {
        st.outputSeq = lines.back().seq;
        for (auto& l : lines) st.output.push_back(std::move(l));
        if (st.output.size() > 3000) st.output.erase(st.output.begin(), st.output.begin() + static_cast<std::ptrdiff_t>(st.output.size() - 3000));
    }

    if (now >= st.nextFileScan) {
        st.files = ScriptEngine::ListFiles();
        st.nextFileScan = now + 3s;
    }

    if (now < st.nextRefresh) return;
    st.nextRefresh = now + REFRESH_EVERY;

    const auto bots = manager_.All();
    st.rows.clear();
    st.rows.reserve(bots.size());
    for (const auto& bot : bots) {
        st.rows.push_back(bot->Read([&](const BotSnapshot& s) {
            State::Row row;
            row.id = bot->Id();
            row.growId = s.growId;
            row.name = s.name;
            row.world = s.world;
            row.statusText = s.statusText;
            row.status = s.status;
            row.gems = s.gems;
            row.ping = s.pingMs;
            row.items = s.inventory.size();
            row.players = s.players.size();
            return row;
        }));
    }
    if (st.selected) {
        if (!manager_.ById(st.selected->Id())) st.selected.reset();
        else st.snapshot = st.selected->Snapshot();
    }
    if (!st.selected && !bots.empty()) {
        st.selected = bots.front();
        st.snapshot = st.selected->Snapshot();
    }
}

void App::Frame() {
    State& st = *state_;
    Refresh();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    DrawHeader();

    const float statusHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(12)));
    ImGui::BeginChild("##bots", ImVec2(S(300), -statusHeight), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    DrawBotList();
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##main", ImVec2(0, -statusHeight), ImGuiChildFlags_None);
    ImGui::PopStyleColor();
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        const std::string open = std::exchange(st.openTab, {});
        const auto tab = [&](const char* label, void (App::*draw)()) {
            const bool select = !open.empty() && std::string_view(label).ends_with("###" + open);
            if (ImGui::BeginTabItem(label, nullptr, select ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::Dummy(ImVec2(0, S(2)));
                (this->*draw)();
                ImGui::EndTabItem();
            }
        };
        tab("   Overview   ###Overview", &App::DrawOverviewTab);
        tab("   World   ###World", &App::DrawWorldTab);
        tab("   Inventory   ###Inventory", &App::DrawInventoryTab);
        tab("   Players   ###Players", &App::DrawPlayersTab);
        tab("   Scripts   ###Scripts", &App::DrawScriptsTab);
        tab("   Log   ###Log", &App::DrawLogTab);
        tab("   Settings   ###Settings", &App::DrawSettingsTab);
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    DrawStatusBar();
    ImGui::End();
}

void App::DrawHeader() {
    State& st = *state_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = S(60);
    const ImVec2 end{ p.x + width, p.y + height };

    dl->AddRectFilled(p, end, Col(Theme::Surface), S(14));
    dl->AddRectFilledMultiColor({ p.x + S(14), p.y }, { p.x + width * 0.5f, end.y },
        Col(Theme::Aqua, 0.10f), Col(Theme::Aqua, 0.0f), Col(Theme::Aqua, 0.0f), Col(Theme::Aqua, 0.10f));
    dl->AddRectFilled(p, { p.x + S(14), end.y }, Col(Theme::Aqua, 0.10f), S(14), ImDrawFlags_RoundCornersLeft);
    dl->AddRect(p, end, Col(Theme::Line), S(14));

    const float logo = S(36);
    const ImVec2 lp{ p.x + S(14), p.y + (height - logo) * 0.5f };
    dl->AddRectFilledMultiColor(lp, { lp.x + logo, lp.y + logo }, Col(Theme::Aqua), Col(Theme::Teal), Col(Theme::AquaDeep), Col(Theme::AquaDeep));
    dl->AddRect(lp, { lp.x + logo, lp.y + logo }, Col(Theme::Bg), S(4), 0, S(2));
    const ImVec2 dc{ lp.x + logo * 0.5f, lp.y + logo * 0.58f };
    dl->AddCircleFilled(dc, logo * 0.20f, Col(Theme::Text));
    dl->AddTriangleFilled({ dc.x - logo * 0.19f, dc.y - logo * 0.06f }, { dc.x + logo * 0.19f, dc.y - logo * 0.06f }, { dc.x, dc.y - logo * 0.40f }, Col(Theme::Text));

    const float fontSize = ImGui::GetFontSize();
    dl->AddText(st.bold, fontSize * 1.35f, { lp.x + logo + S(12), p.y + S(9) }, Col(Theme::Text), "GrowtopiaBot");
    const float titleWidth = st.bold->CalcTextSizeA(fontSize * 1.35f, FLT_MAX, 0.0f, "GrowtopiaBot").x;
    dl->AddText(st.bold, fontSize * 1.35f, { lp.x + logo + S(12) + titleWidth, p.y + S(9) }, Col(Theme::Aqua), ".");
    dl->AddText({ lp.x + logo + S(13), p.y + S(9) + fontSize * 1.35f }, Col(Theme::Muted), "Growtopia bot manager");

    struct Action { const char* label; int kind; };
    const Action actions[] = { { "Connect all", 0 }, { "Disconnect all", 1 }, { "+ Add bot", 1 }, { "Stop scripts", 2 } };
    const ImGuiStyle& style = ImGui::GetStyle();
    float total = 0.0f;
    for (const auto& a : actions) total += ImGui::CalcTextSize(a.label).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
    ImGui::SetCursorScreenPos({ end.x - total - S(8), p.y + (height - ImGui::GetFrameHeight()) * 0.5f });
    for (size_t i = 0; i < std::size(actions); ++i) {
        if (i) ImGui::SameLine();
        const Action& a = actions[i];
        const bool pressed = a.kind == 0 ? PrimaryButton(a.label) : a.kind == 2 ? DangerButton(a.label) : ImGui::Button(a.label);
        if (!pressed) continue;
        switch (i) {
            case 0: manager_.StartAll(); break;
            case 1: manager_.DisconnectAll(); break;
            case 2: ImGui::OpenPopup("Add bot"); break;
            case 3: scripts_.StopAll(); break;
        }
    }

    ImGui::SetNextWindowSize(ImVec2(S(440), 0));
    if (ImGui::BeginPopupModal("Add bot", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::PushFont(st.bold, ImGui::GetStyle().FontSizeBase * 1.2f);
        ImGui::TextUnformatted("Add a bot");
        ImGui::PopFont();
        Caption("Kept until the program closes; nothing is saved to disk.");
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::PushItemWidth(-FLT_MIN);
        Caption("GrowID");
        ImGui::InputText("##growid", &st.newGrowId);
        Caption("Password");
        ImGui::InputText("##password", &st.newPassword, ImGuiInputTextFlags_Password);
        Caption("Proxy (optional)");
        ImGui::InputTextWithHint("##proxy", "socks5://user:pass@host:port", &st.newProxy);
        ImGui::PopItemWidth();
        ImGui::Checkbox("Connect now", &st.startNew);
        if (!st.addError.empty()) ImGui::TextColored(Theme::Red, "%s", st.addError.c_str());
        ImGui::Dummy(ImVec2(0, S(4)));
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (PrimaryButton("Add", ImVec2(half, 0))) {
            auto bot = manager_.AddBot(st.newGrowId, st.newPassword, st.newProxy, st.addError);
            if (bot) {
                if (st.startNew) bot->Connect();
                st.selected = bot;
                st.nextRefresh = {};
                st.newGrowId.clear();
                st.newPassword.clear();
                st.newProxy.clear();
                st.addError.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(half, 0))) {
            st.addError.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::Dummy(ImVec2(0, S(2)));
}

void App::DrawBotList() {
    State& st = *state_;
    size_t online = 0, inWorld = 0;
    for (const auto& row : st.rows) {
        if (row.status == BotStatus::InWorld) ++inWorld;
        if (row.status == BotStatus::InWorld || row.status == BotStatus::InGame || row.status == BotStatus::JoiningWorld) ++online;
    }
    ImGui::PushFont(st.bold, 0.0f);
    ImGui::Text("Bots");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu", st.rows.size());

    Chip(std::format("{} online", online).c_str(), Theme::Aqua);
    ImGui::SameLine(0, S(6));
    Chip(std::format("{} in world", inWorld).c_str(), Theme::Green);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##botfilter", "Search name, world, status", &st.botFilter);
    ImGui::Dummy(ImVec2(0, S(2)));

    std::vector<const State::Row*> visible;
    visible.reserve(st.rows.size());
    for (const auto& row : st.rows) {
        if (st.botFilter.empty() || Text::IContains(row.name, st.botFilter) || Text::IContains(row.growId, st.botFilter) ||
            Text::IContains(row.world, st.botFilter) || Text::IContains(StatusName(row.status), st.botFilter))
            visible.push_back(&row);
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##botrows");
    ImGui::PopStyleColor();
    if (st.rows.empty()) {
        ImGui::Dummy(ImVec2(0, S(20)));
        Caption("No bots yet.");
        Caption("Use \"+ Add bot\" in the header.");
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowHeight = S(54);
    const float fontSize = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(4)));
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()), rowHeight + S(4));
    while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const State::Row& row = *visible[static_cast<size_t>(i)];
        ImGui::PushID(row.id);
        const bool isSelected = st.selected && st.selected->Id() == row.id;
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##row", isSelected, ImGuiSelectableFlags_None, ImVec2(width, rowHeight))) {
            st.selected = manager_.ById(row.id);
            st.nextRefresh = {};
        }
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (auto bot = manager_.ById(row.id)) {
                if (ImGui::MenuItem("Connect")) bot->Connect();
                if (ImGui::MenuItem("Disconnect")) bot->Disconnect();
                if (ImGui::MenuItem("Reconnect")) bot->Reconnect();
            }
            ImGui::EndPopup();
        }
        if (!row.statusText.empty() && hovered) ImGui::SetTooltip("%s", row.statusText.c_str());
        ImGui::PopID();

        const ImVec2 max{ min.x + width, min.y + rowHeight };
        const ImVec4 status = StatusColor(row.status);
        if (!isSelected && !hovered) dl->AddRectFilled(min, max, Col(Theme::Raised, 0.55f), S(10));
        if (isSelected) {
            dl->AddRect(min, max, Col(Theme::Aqua, 0.55f), S(10));
            dl->AddRectFilled({ min.x, min.y + S(12) }, { min.x + S(3), max.y - S(12) }, Col(Theme::Aqua), S(2));
        }
        const ImVec2 avatar{ min.x + S(26), min.y + rowHeight * 0.5f };
        dl->AddCircleFilled(avatar, S(15), Col(status, 0.16f));
        dl->AddCircle(avatar, S(15), Col(status, 0.85f), 0, S(1.6f));
        const char initial[2] = { row.name.empty() ? '?' : static_cast<char>(std::toupper(static_cast<unsigned char>(row.name[0]))), 0 };
        const ImVec2 initialSize = st.bold->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, initial);
        dl->AddText(st.bold, fontSize, { avatar.x - initialSize.x * 0.5f, avatar.y - initialSize.y * 0.5f }, Col(status), initial);

        const float textX = min.x + S(50);
        dl->AddText(st.bold, fontSize, { textX, min.y + S(8) }, Col(Theme::Text), row.name.c_str());
        const std::string sub = row.world.empty() ? std::string(StatusName(row.status)) : row.world;
        dl->AddText({ textX, min.y + S(8) + fontSize + S(2) }, Col(row.world.empty() ? status : Theme::Muted), sub.c_str());
        const std::string ping = row.ping ? std::format("{} ms", row.ping) : "-";
        const ImVec2 pingSize = ImGui::CalcTextSize(ping.c_str());
        const ImVec4 pingColor = !row.ping ? Theme::Faint : row.ping < 120 ? Theme::Green : row.ping < 250 ? Theme::Yellow : Theme::Red;
        dl->AddText({ max.x - pingSize.x - S(12), min.y + S(8) }, Col(pingColor), ping.c_str());
        const std::string id = std::format("#{}", row.id);
        const ImVec2 idSize = ImGui::CalcTextSize(id.c_str());
        dl->AddText({ max.x - idSize.x - S(12), min.y + S(8) + fontSize + S(2) }, Col(Theme::Faint), id.c_str());
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

namespace {
    bool NoBot(const std::shared_ptr<Bot>& bot) {
        if (bot) return false;
        ImGui::Dummy(ImVec2(0, S(30)));
        Caption("No bot selected. Add one with \"+ Add bot\" in the header.");
        return true;
    }

    void StatTile(ImFont* bold, const char* label, const std::string& value, ImVec4 accent, float width) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float height = S(66);
        dl->AddRectFilled(p, { p.x + width, p.y + height }, Col(Theme::Surface), S(12));
        dl->AddRect(p, { p.x + width, p.y + height }, Col(Theme::Line), S(12));
        dl->AddRectFilled({ p.x + S(12), p.y + S(14) }, { p.x + S(15), p.y + height - S(14) }, Col(accent), S(2));
        const float fontSize = ImGui::GetFontSize();
        dl->AddText({ p.x + S(24), p.y + S(11) }, Col(Theme::Muted), label);
        ImGui::PushClipRect(p, { p.x + width - S(8), p.y + height }, true);
        dl->AddText(bold, fontSize * 1.18f, { p.x + S(24), p.y + S(13) + fontSize }, Col(Theme::Text), value.c_str());
        ImGui::PopClipRect();
        ImGui::Dummy(ImVec2(width, height));
    }

    float TileWidth(float minimum, int& perRow) {
        const float avail = ImGui::GetContentRegionAvail().x, gap = ImGui::GetStyle().ItemSpacing.x;
        perRow = std::max(1, static_cast<int>((avail + gap) / (minimum + gap)));
        return (avail - gap * static_cast<float>(perRow - 1)) / static_cast<float>(perRow);
    }

    void CardTitle(ImFont* bold, const char* title, const char* subtitle = nullptr) {
        ImGui::PushFont(bold, 0.0f);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        if (subtitle) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", subtitle);
        }
        ImGui::Dummy(ImVec2(0, S(1)));
    }
}

void App::DrawOverviewTab() {
    State& st = *state_;
    if (NoBot(st.selected)) return;
    Bot& bot = *st.selected;
    const BotSnapshot& s = st.snapshot;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##overview");
    ImGui::PopStyleColor();

    if (BeginCard("##hero")) {
        ImGui::PushFont(st.bold, ImGui::GetStyle().FontSizeBase * 1.55f);
        ImGui::TextUnformatted(s.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, S(14));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(5));
        Chip(StatusName(s.status), StatusColor(s.status));
        if (!s.statusText.empty()) {
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(5));
            ImGui::TextColored(StatusColor(s.status), "%s", s.statusText.c_str());
        }
        ImGui::TextDisabled("GrowID %s   ·   bot #%d   ·   %s", s.growId.c_str(), s.index, s.proxy.empty() ? "no proxy" : s.proxy.c_str());
        ImGui::Dummy(ImVec2(0, S(2)));
        if (PrimaryButton("Connect")) bot.Connect();
        ImGui::SameLine();
        if (ImGui::Button("Disconnect")) bot.Disconnect();
        ImGui::SameLine();
        if (ImGui::Button("Reconnect")) bot.Reconnect();
        ImGui::SameLine(0, S(18));
        bool autoReconnect = bot.AutoReconnect();
        if (ImGui::Checkbox("Auto reconnect", &autoReconnect)) bot.SetAutoReconnect(autoReconnect);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Remove bot").x - ImGui::GetStyle().FramePadding.x * 2);
        if (DangerButton("Remove bot")) ImGui::OpenPopup("Remove bot?");
        if (ImGui::BeginPopupModal("Remove bot?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Disconnect and remove %s from the list?", s.growId.c_str());
            ImGui::Dummy(ImVec2(0, S(4)));
            if (ColorButton("Remove", Theme::Red, ImVec2(S(120), 0))) {
                manager_.RemoveBot(bot.Id());
                st.selected.reset();
                st.nextRefresh = {};
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(S(120), 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    EndCard();
    if (!st.selected) {
        ImGui::EndChild();
        return;
    }

    ImGui::Dummy(ImVec2(0, S(2)));
    const auto stats = bot.Counters();
    const std::pair<const char*, std::string> tiles[] = {
        { "World", s.world.empty() ? "-" : std::format("{}  {}x{}", s.world, s.worldWidth, s.worldHeight) },
        { "Tile", std::format("{}, {}", Pathfinder::TileOf(s.x, s.y).x, Pathfinder::TileOf(s.x, s.y).y) },
        { "Gems", std::format("{}", s.gems) },
        { "Level", std::to_string(s.level) },
        { "Ping", s.pingMs ? std::format("{} ms", s.pingMs) : "-" },
        { "Inventory", std::format("{} / {}", s.inventory.size(), s.inventorySize) },
        { "Players here", std::to_string(s.players.size()) },
        { "Target world", s.targetWorld.empty() ? "none" : s.targetWorld },
        { "netID / userID", std::format("{} / {}", s.netID, s.userID) },
        { "Packets / commands", std::format("{} / {}", stats.packets, stats.commands) },
    };
    const ImVec4 accents[] = { Theme::Aqua, Theme::Teal, Theme::Gold, Theme::Violet, Theme::Green, Theme::Aqua, Theme::Orange, Theme::Teal, Theme::Faint, Theme::Faint };
    int perRow = 1;
    const float tileWidth = TileWidth(S(190), perRow);
    for (size_t i = 0; i < std::size(tiles); ++i) {
        if (i % static_cast<size_t>(perRow) != 0) ImGui::SameLine();
        StatTile(st.bold, tiles[i].first, tiles[i].second, accents[i], tileWidth);
    }
    ImGui::Dummy(ImVec2(0, S(2)));

    const bool wide = ImGui::GetContentRegionAvail().x > S(820);
    const float half = wide ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f : 0.0f;
    if (BeginCard("##travel", half)) {
        CardTitle(st.bold, "Travel");
        const float doorWidth = S(110), buttonWidth = S(90);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - doorWidth - buttonWidth - ImGui::GetStyle().ItemSpacing.x * 2);
        const bool warpEnter = ImGui::InputTextWithHint("##warp", "World name", &st.warpWorld, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(doorWidth);
        ImGui::InputTextWithHint("##door", "Door id", &st.warpDoor);
        ImGui::SameLine();
        if ((PrimaryButton("Warp", ImVec2(buttonWidth, 0)) || warpEnter) && !st.warpWorld.empty()) bot.Warp(st.warpWorld, st.warpDoor);

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##target", "Target world (rejoined after kicks; empty = none)", &st.targetWorld, ImGuiInputTextFlags_CharsUppercase);
        ImGui::SameLine();
        if (ImGui::Button("Set target", ImVec2(buttonWidth, 0))) bot.SetTargetWorld(st.targetWorld);
        if (ImGui::Button("Leave world")) bot.LeaveWorld();
        ImGui::SameLine();
        if (ImGui::Button("Respawn")) bot.Respawn();
    }
    EndCard();
    if (wide) ImGui::SameLine();
    if (BeginCard("##chat", half)) {
        CardTitle(st.bold, "Chat & packets");
        const float buttonWidth = S(90);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::InputTextWithHint("##say", "Message or /command", &st.sayText, ImGuiInputTextFlags_EnterReturnsTrue) && !st.sayText.empty()) {
            bot.Say(st.sayText);
            st.sayText.clear();
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SameLine();
        if (PrimaryButton("Say", ImVec2(buttonWidth, 0)) && !st.sayText.empty()) {
            bot.Say(st.sayText);
            st.sayText.clear();
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##raw", "Raw packet, e.g. action|respawn   (\\n = new line)", &st.rawText);
        ImGui::SameLine();
        if (ImGui::Button("Send raw", ImVec2(buttonWidth, 0)) && !st.rawText.empty()) {
            std::string text = st.rawText;
            for (size_t p; (p = text.find("\\n")) != std::string::npos;) text.replace(p, 2, "\n");
            if (text.back() != '\n') text += '\n';
            bot.SendRawText(text);
        }
        Caption("Raw text goes out as NET_MESSAGE_GENERIC_TEXT.");
    }
    EndCard();
    ImGui::EndChild();
}

void App::DrawWorldTab() {
    State& st = *state_;
    if (NoBot(st.selected)) return;
    Bot& bot = *st.selected;
    const BotSnapshot& s = st.snapshot;
    const bool inWorld = s.status == BotStatus::InWorld && !s.world.empty();
    const Pathfinder::Point botTile = Pathfinder::TileOf(s.x, s.y);
    const int botX = botTile.x, botY = botTile.y;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##worldtab");
    ImGui::PopStyleColor();

    const float sideWidth = S(330);
    const bool wide = ImGui::GetContentRegionAvail().x > sideWidth + S(470);
    const float gridCardWidth = wide ? ImGui::GetContentRegionAvail().x - sideWidth - ImGui::GetStyle().ItemSpacing.x : 0.0f;

    if (BeginCard("##reach", gridCardWidth)) {
        CardTitle(st.bold, "Reach", inWorld ? std::format("{}  ·  standing on {}, {}", s.world, botX, botY).c_str() : "not in a world");

        for (size_t i = 0; i < TOOLS.size(); ++i) {
            if (i) ImGui::SameLine(0, S(6));
            const ToolInfo& t = TOOLS[i];
            const std::string label = std::format("{}  {}", i + 1, t.name);
            const bool active = st.tool == static_cast<Tool>(i);
            if (active ? ColorButton(label.c_str(), t.color) : TintButton(label.c_str(), Alpha(t.color, 0.9f))) st.tool = static_cast<Tool>(i);
        }
        if (!ImGui::GetIO().WantTextInput) {
            for (int i = 0; i < static_cast<int>(TOOLS.size()); ++i) {
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false)) st.tool = static_cast<Tool>(i);
            }
        }
        if (st.tool == Tool::Place) {
            const auto it = s.inventory.find(static_cast<uint16_t>(st.placeItem));
            const std::string preview = it != s.inventory.end() ? std::format("#{}  x{}", st.placeItem, it->second.amount) : std::format("#{}  (not in inventory)", st.placeItem);
            ImGui::SetNextItemWidth(S(220));
            if (ImGui::BeginCombo("##placeitem", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
                for (const auto& [id, item] : s.inventory) {
                    if (ImGui::Selectable(std::format("#{}  x{}", id, item.amount).c_str(), id == st.placeItem)) st.placeItem = id;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(S(110));
            ImGui::InputInt("item id", &st.placeItem, 0);
            st.placeItem = std::clamp(st.placeItem, 0, 65535);
        }
        ImGui::Dummy(ImVec2(0, S(4)));

        std::array<std::vector<const PlayerInfo*>, (2 * REACH + 1) * (2 * REACH + 1)> here;
        if (inWorld) {
            for (const auto& [netID, p] : s.players) {
                const Pathfinder::Point tile = Pathfinder::TileOf(p.x, p.y);
                const int dx = tile.x - botX, dy = tile.y - botY;
                if (std::abs(dx) <= REACH && std::abs(dy) <= REACH) here[static_cast<size_t>((dy + REACH) * (2 * REACH + 1) + dx + REACH)].push_back(&p);
            }
        }

        struct CellTile { bool known = false; uint16_t fg = 0, bg = 0; bool walkable = false; };
        std::array<CellTile, (2 * REACH + 1) * (2 * REACH + 1)> cells{};
        if (inWorld) {
            const auto userID = static_cast<uint32_t>(s.userID);
            bot.ReadWorld([&](const WorldState& world) {
                for (int dy = -REACH; dy <= REACH; ++dy) {
                    for (int dx = -REACH; dx <= REACH; ++dx) {
                        const WorldTile* t = world.Tile(botX + dx, botY + dy);
                        if (!t) continue;
                        cells[static_cast<size_t>((dy + REACH) * (2 * REACH + 1) + dx + REACH)] =
                            { true, t->fg, t->bg, Pathfinder::EnterCost(world, botX + dx, botY + dy, 0, 0, userID) >= 0 };
                    }
                }
                return 0;
            });
        }

        const ToolInfo& tool = TOOLS[static_cast<size_t>(st.tool)];
        const float gap = S(8);
        const float cell = std::clamp((std::min(ImGui::GetContentRegionAvail().x, S(560)) - gap * 4) / 5.0f, S(54), S(96));
        const float gridWidth = cell * 5 + gap * 4;
        const ImVec2 origin{ ImGui::GetCursorScreenPos().x + (ImGui::GetContentRegionAvail().x - gridWidth) * 0.5f, ImGui::GetCursorScreenPos().y };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float fontSize = ImGui::GetFontSize();
        for (int dy = -REACH; dy <= REACH; ++dy) {
            for (int dx = -REACH; dx <= REACH; ++dx) {
                const int tx = botX + dx, ty = botY + dy;
                const bool center = dx == 0 && dy == 0;
                const bool valid = inWorld && tx >= 0 && ty >= 0 && (s.worldWidth == 0 || tx < static_cast<int>(s.worldWidth)) &&
                    (s.worldHeight == 0 || ty < static_cast<int>(s.worldHeight));
                const ImVec2 min{ origin.x + static_cast<float>(dx + REACH) * (cell + gap), origin.y + static_cast<float>(dy + REACH) * (cell + gap) };
                const ImVec2 max{ min.x + cell, min.y + cell };
                ImGui::SetCursorScreenPos(min);
                ImGui::PushID((dy + REACH) * 8 + dx + REACH);
                ImGui::BeginDisabled(!valid);
                const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(cell, cell));
                ImGui::EndDisabled();
                const bool hovered = valid && ImGui::IsItemHovered();
                const bool held = valid && ImGui::IsItemActive();
                ImGui::PopID();
                const auto& players = here[static_cast<size_t>((dy + REACH) * (2 * REACH + 1) + dx + REACH)];
                const CellTile& cellTile = cells[static_cast<size_t>((dy + REACH) * (2 * REACH + 1) + dx + REACH)];

                if (clicked) {
                    switch (st.tool) {
                        case Tool::Punch: bot.Punch(tx, ty); break;
                        case Tool::Place: if (st.placeItem > 0) bot.Place(tx, ty, static_cast<uint16_t>(st.placeItem)); break;
                        case Tool::Wrench: bot.Wrench(tx, ty); break;
                        case Tool::Door: bot.Activate(tx, ty); break;
                        case Tool::Walk: if (!center) bot.Walk(tx, ty); break;
                    }
                }
                if (hovered) {
                    ImGui::BeginTooltip();
                    ImGui::TextColored(tool.color, "%s", tool.name);
                    ImGui::SameLine();
                    ImGui::Text("tile %d, %d", tx, ty);
                    ImGui::TextDisabled("offset %+d, %+d from the bot", dx, dy);
                    if (cellTile.known) {
                        ImGui::Text("fg: %s", cellTile.fg ? std::format("{} (#{})", ItemDB::Name(cellTile.fg), cellTile.fg).c_str() : "-");
                        ImGui::Text("bg: %s", cellTile.bg ? std::format("{} (#{})", ItemDB::Name(cellTile.bg), cellTile.bg).c_str() : "-");
                        ImGui::TextColored(cellTile.walkable ? Theme::Teal : Theme::Orange, "%s", cellTile.walkable ? "walkable" : "solid / no access");
                    }
                    for (const PlayerInfo* p : players) ImGui::TextColored(p->moderator ? Theme::Red : Theme::Teal, "%s%s", p->name.c_str(), p->moderator ? "  [MOD]" : "");
                    ImGui::EndTooltip();
                }

                const float rounding = S(12);
                ImVec4 fill = valid ? Theme::Raised : Alpha(Theme::Surface, 0.6f);
                if (center && inWorld) fill = Alpha(Theme::Aqua, 0.20f);
                else if (valid && cellTile.known && !cellTile.walkable) fill = Alpha(Theme::Orange, 0.10f);
                dl->AddRectFilled(min, max, Col(fill), rounding);
                if (hovered) dl->AddRectFilled(min, max, Col(tool.color, held ? 0.34f : 0.20f), rounding);
                const ImVec4 border = hovered ? tool.color : center && inWorld ? Theme::Aqua : Theme::Line;
                dl->AddRect(min, max, Col(border, hovered || center ? 1.0f : 0.8f), rounding, 0, hovered || center ? S(2) : S(1));
                if (!valid) {
                    dl->PushClipRect(min, max, true);
                    for (float off = -cell; off < cell; off += S(10)) dl->AddLine({ min.x + off, max.y }, { min.x + off + cell, min.y }, Col(Theme::Line, 0.5f), S(1));
                    dl->PopClipRect();
                    continue;
                }
                if (center) {
                    const ImVec2 c{ (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f - S(4) };
                    dl->AddCircleFilled({ c.x, c.y - cell * 0.13f }, cell * 0.12f, Col(Theme::Aqua));
                    dl->AddRectFilled({ c.x - cell * 0.17f, c.y + cell * 0.02f }, { c.x + cell * 0.17f, c.y + cell * 0.24f }, Col(Theme::Aqua), cell * 0.08f);
                    const char* you = "BOT";
                    const ImVec2 size = st.bold->CalcTextSizeA(fontSize * 0.75f, FLT_MAX, 0.0f, you);
                    dl->AddText(st.bold, fontSize * 0.75f, { c.x - size.x * 0.5f, max.y - size.y - S(5) }, Col(Theme::Aqua), you);
                }
                else if (hovered) {
                    const ImVec2 size = st.bold->CalcTextSizeA(fontSize * 0.85f, FLT_MAX, 0.0f, tool.name);
                    dl->AddText(st.bold, fontSize * 0.85f, { (min.x + max.x - size.x) * 0.5f, (min.y + max.y - size.y) * 0.5f }, Col(tool.color), tool.name);
                }
                const std::string coords = std::format("{},{}", tx, ty);
                dl->AddText(nullptr, fontSize * 0.72f, { min.x + S(7), min.y + S(5) }, Col(Theme::Faint), coords.c_str());
                if (!center && !hovered && cellTile.known && (cellTile.fg || cellTile.bg)) {
                    const float nameSize = fontSize * 0.72f;
                    dl->PushClipRect({ min.x + S(4), min.y }, { max.x - S(4), max.y }, true);
                    if (cellTile.fg) dl->AddText(nullptr, nameSize, { min.x + S(7), max.y - S(6) - nameSize * 2 }, Col(Theme::Text, 0.9f), ItemDB::Name(cellTile.fg).c_str());
                    if (cellTile.bg) dl->AddText(nullptr, nameSize, { min.x + S(7), max.y - S(5) - nameSize }, Col(Theme::Muted, 0.8f), ItemDB::Name(cellTile.bg).c_str());
                    dl->PopClipRect();
                }
                for (size_t k = 0; k < std::min<size_t>(players.size(), 4); ++k) {
                    dl->AddCircleFilled({ max.x - S(10) - static_cast<float>(k) * S(10), max.y - S(10) }, S(3.5f), Col(players[k]->moderator ? Theme::Red : Theme::Teal));
                }
            }
        }
        ImGui::SetCursorScreenPos({ ImGui::GetCursorStartPos().x + ImGui::GetWindowPos().x, origin.y + gridWidth + gap });
        ImGui::Dummy(ImVec2(0, 0));
        Caption(inWorld ? std::format("Click a tile to {} it. Keys 1-5 switch the tool; WASD / arrows walk while the mouse is over this card.", tool.verb).c_str()
                        : "Join a world to act on the tiles around the bot.");

        if (inWorld && !ImGui::GetIO().WantTextInput && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            const auto pressed = [](ImGuiKey a, ImGuiKey b) { return ImGui::IsKeyPressed(a) || ImGui::IsKeyPressed(b); };
            if (pressed(ImGuiKey_A, ImGuiKey_LeftArrow)) bot.Walk(botX - 1, botY);
            if (pressed(ImGuiKey_D, ImGuiKey_RightArrow)) bot.Walk(botX + 1, botY);
            if (pressed(ImGuiKey_W, ImGuiKey_UpArrow)) bot.Walk(botX, botY - 1);
            if (pressed(ImGuiKey_S, ImGuiKey_DownArrow)) bot.Walk(botX, botY + 1);
        }
    }
    EndCard();

    if (wide) ImGui::SameLine();
    ImGui::BeginGroup();
    if (BeginCard("##move", wide ? sideWidth : 0.0f)) {
        CardTitle(st.bold, "Move");
        const float b = S(44), g = ImGui::GetStyle().ItemSpacing.x;
        const float padLeft = (ImGui::GetContentRegionAvail().x - (b * 3 + g * 2)) * 0.5f;
        const float x0 = ImGui::GetCursorPosX() + padLeft;
        ImGui::BeginDisabled(!inWorld);
        ImGui::SetCursorPosX(x0 + b + g);
        if (ImGui::Button("W##up", ImVec2(b, b))) bot.Walk(botX, botY - 1);
        ImGui::SetCursorPosX(x0);
        if (ImGui::Button("A##left", ImVec2(b, b))) bot.Walk(botX - 1, botY);
        ImGui::SameLine();
        if (ImGui::Button("S##down", ImVec2(b, b))) bot.Walk(botX, botY + 1);
        ImGui::SameLine();
        if (ImGui::Button("D##right", ImVec2(b, b))) bot.Walk(botX + 1, botY);
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0, S(2)));
        Caption("Walk to a tile");
        const float field = (ImGui::GetContentRegionAvail().x - S(70) - g * 2) * 0.5f;
        ImGui::SetNextItemWidth(field); ImGui::InputInt("##wx", &st.walkX, 0);
        ImGui::SameLine(); ImGui::SetNextItemWidth(field); ImGui::InputInt("##wy", &st.walkY, 0);
        ImGui::SameLine();
        if (PrimaryButton("Go", ImVec2(S(70), 0))) bot.Walk(st.walkX, st.walkY);
        if (ImGui::Button("Here", ImVec2(-FLT_MIN, 0))) { st.walkX = botX; st.walkY = botY; }
        if (s.walking) {
            Caption(std::format("Walking to {}, {}  ·  {} tile(s) left", s.pathTargetX, s.pathTargetY, s.pathLeft).c_str());
            if (DangerButton("Stop walking", ImVec2(-FLT_MIN, 0))) bot.StopWalk();
        }
        else if (s.pathDone > 0 && !s.pathOk) Caption(std::format("Last walk: {}", s.pathError).c_str());
        else Caption("Walks tile by tile around blocks, doors and locks.");
    }
    EndCard();

    if (BeginCard("##drops", wide ? sideWidth : 0.0f)) {
        struct Drop { uint32_t uid; uint16_t id; uint8_t count; int dx, dy; };
        size_t total = 0;
        std::vector<Drop> drops;
        if (inWorld) {
            bot.ReadWorld([&](const WorldState& world) {
                total = world.objects.size();
                drops.reserve(std::min<size_t>(total, 64));
                for (const WorldObject& o : world.objects) {
                    const Pathfinder::Point tile = Pathfinder::TileOf(o.x - 2.0f, o.y - 7.0f);
                    drops.push_back({ o.uid, o.id, o.count, tile.x - botX, tile.y - botY });
                }
                return 0;
            });
        }
        const auto distance = [](const Drop& d) { return std::max(std::abs(d.dx), std::abs(d.dy)); };
        const size_t shown = std::min<size_t>(drops.size(), 6);
        std::partial_sort(drops.begin(), drops.begin() + static_cast<std::ptrdiff_t>(shown), drops.end(),
            [&](const Drop& a, const Drop& b) { return distance(a) < distance(b); });

        CardTitle(st.bold, "Dropped items", std::format("{} in the world", total).c_str());
        ImGui::BeginDisabled(!inWorld);
        bool autoCollect = s.autoCollect;
        if (ImGui::Checkbox("Auto collect", &autoCollect)) bot.SetAutoCollect(autoCollect);
        ImGui::SameLine();
        if (TintButton("Collect nearby", Theme::Aqua)) bot.Collect();
        ImGui::EndDisabled();
        for (size_t i = 0; i < shown; ++i) {
            const Drop& d = drops[i];
            ImGui::PushID(static_cast<int>(d.uid));
            const bool inReach = distance(d) <= REACH;
            ImGui::BeginDisabled(inReach || !inWorld);
            if (ImGui::SmallButton("Walk")) bot.Walk(botX + d.dx, botY + d.dy);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextColored(inReach ? Theme::Teal : Theme::Text, "%s x%u", ItemDB::Name(d.id).c_str(), static_cast<unsigned>(d.count));
            ImGui::SameLine();
            ImGui::TextDisabled("%+d, %+d", d.dx, d.dy);
            ImGui::PopID();
        }
        Caption(std::format("Collects within {} tiles (the game's own reach).", REACH).c_str());
    }
    EndCard();

    if (BeginCard("##legend", wide ? sideWidth : 0.0f)) {
        CardTitle(st.bold, "On the grid");
        Chip("bot", Theme::Aqua);
        ImGui::SameLine(0, S(6));
        Chip("player", Theme::Teal);
        ImGui::SameLine(0, S(6));
        Chip("moderator", Theme::Red);
        Caption("Hover a tile for its coordinates and who stands on it.");
        Caption(std::format("{} other player(s) in this world.", s.players.size()).c_str());
    }
    EndCard();
    ImGui::EndGroup();
    ImGui::EndChild();
}

void App::DrawInventoryTab() {
    State& st = *state_;
    if (NoBot(st.selected)) return;
    Bot& bot = *st.selected;
    const BotSnapshot& s = st.snapshot;

    if (BeginCard("##invhead")) {
        CardTitle(st.bold, "Inventory", std::format("{} items  ·  {} slots", s.inventory.size(), s.inventorySize).c_str());
        const float used = s.inventorySize ? static_cast<float>(s.inventory.size()) / static_cast<float>(s.inventorySize) : 0.0f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, used > 0.9f ? Theme::Red : used > 0.7f ? Theme::Yellow : Theme::Aqua);
        ImGui::ProgressBar(used, ImVec2(-FLT_MIN, S(6)), "");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(S(220));
        ImGui::InputTextWithHint("##invfilter", "Search name or id", &st.inventoryFilter);
        ImGui::SameLine(0, S(18));
        Caption("Amount");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(200));
        ImGui::SliderInt("##amount", &st.dropAmount, 0, 200, st.dropAmount == 0 ? "all" : "%d");
        st.dropAmount = std::clamp(st.dropAmount, 0, 200);
    }
    EndCard();
    if (s.inventory.empty()) {
        ImGui::Dummy(ImVec2(0, S(12)));
        Caption("The inventory is empty (or the bot hasn't logged in yet).");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(12), S(8)));
    if (ImGui::BeginTable("##inventory", 4, flags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthFixed, S(240));
        ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, S(80));
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, S(330));
        ImGui::TableHeadersRow();
        for (const auto& [id, item] : s.inventory) {
            const std::string name = ItemDB::Name(id);
            if (!st.inventoryFilter.empty() && !Text::Contains(std::to_string(id), st.inventoryFilter) && !Text::IContains(name, st.inventoryFilter)) continue;
            ImGui::TableNextRow();
            ImGui::PushID(id);
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::PushFont(st.bold, 0.0f);
            ImGui::TextUnformatted(name.c_str());
            ImGui::PopFont();
            if (!name.starts_with('#')) {
                ImGui::SameLine();
                ImGui::TextDisabled("#%u", id);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            const std::string count = std::to_string(item.amount);
            ImGui::TextUnformatted(count.c_str());
            ImGui::SameLine(0, std::max(S(8), S(40) - ImGui::CalcTextSize(count.c_str()).x));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFrameHeight() * 0.5f - S(3));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, item.amount >= 200 ? Theme::Gold : Theme::Aqua);
            ImGui::ProgressBar(static_cast<float>(item.amount) / 200.0f, ImVec2(std::min(ImGui::GetContentRegionAvail().x, S(180)), S(6)), "");
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(2);
            ImGui::AlignTextToFramePadding();
            if (item.flags & 1) Chip("worn", Theme::Violet, false);
            else ImGui::TextDisabled("0x%02X", item.flags);
            ImGui::TableSetColumnIndex(3);
            const int amount = st.dropAmount == 0 ? item.amount : std::min<int>(st.dropAmount, item.amount);
            if (TintButton("Drop", Theme::Aqua)) bot.Drop(id, amount);
            ImGui::SameLine(0, S(6));
            if (TintButton(item.flags & 1 ? "Unwear" : "Wear / use", Theme::Violet)) bot.Wear(id);
            ImGui::SameLine(0, S(6));
            if (TintButton("Place", Theme::Green)) {
                st.placeItem = id;
                st.tool = Tool::Place;
                st.openTab = "World";
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick it for the Place tool on the World tab");
            ImGui::SameLine(0, S(6));
            if (DangerButton("Trash")) bot.Trash(id, amount);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void App::DrawPlayersTab() {
    State& st = *state_;
    if (NoBot(st.selected)) return;
    const BotSnapshot& s = st.snapshot;
    CardTitle(st.bold, "Players", s.world.empty() ? "not in a world" : std::format("{}  ·  {} other(s)", s.world, s.players.size()).c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(12), S(8)));
    if (ImGui::BeginTable("##players", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("netID");
        ImGui::TableSetupColumn("userID");
        ImGui::TableSetupColumn("Country");
        ImGui::TableSetupColumn("Tile");
        ImGui::TableHeadersRow();
        for (const auto& [netID, p] : s.players) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(p.name.c_str());
            if (p.moderator) {
                ImGui::SameLine();
                Chip("MOD", Theme::Red, false);
            }
            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%d", netID);
            ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%d", p.userID);
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(p.country.c_str());
            const Pathfinder::Point tile = Pathfinder::TileOf(p.x, p.y);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%d, %d", tile.x, tile.y);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void App::DrawEditor(float height) {
    State& st = *state_;
    if (st.mono) ImGui::PushFont(st.mono, 0.0f);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineHeight = ImGui::GetTextLineHeight();
    const int lineCount = static_cast<int>(std::ranges::count(st.editor, '\n')) + 1;
    const float gutter = ImGui::CalcTextSize("0000").x + S(22);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 editorMin{ pos.x + gutter, pos.y };

    ImGui::SetCursorScreenPos(editorMin);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::InputTextMultiline("##editor", &st.editor, ImVec2(width - gutter, height), ImGuiInputTextFlags_AllowTabInput);
    ImGui::PopStyleColor();
    const bool focused = ImGui::IsItemFocused() || ImGui::IsItemActive();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiWindow* box = window->DC.ChildWindows.Size > 0 ? window->DC.ChildWindows.back() : nullptr;
    const float scrollY = box ? box->Scroll.y : 0.0f;

    const bool traced = st.traceId != 0 && st.editor == st.traceCode && scripts_.Lines(st.traceId, st.lineStates);
    if (!traced) st.lineStates.clear();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end{ pos.x + width, pos.y + height };
    const float rounding = style.FrameRounding;
    dl->AddRectFilled(pos, end, Col(ImVec4(0.050f, 0.070f, 0.110f, 1.0f)), rounding);
    dl->AddRectFilled(pos, { editorMin.x, end.y }, Col(Theme::Surface), rounding, ImDrawFlags_RoundCornersLeft);
    dl->AddLine({ editorMin.x, pos.y }, { editorMin.x, end.y }, Col(Theme::Line));
    dl->AddRect(pos, end, Col(focused ? Alpha(Theme::Aqua, 0.6f) : Theme::Line), rounding);

    dl->PushClipRect({ pos.x, pos.y + S(1) }, { end.x, end.y - S(1) }, true);
    const float top = editorMin.y + style.FramePadding.y - scrollY;
    const int first = std::max(0, static_cast<int>((scrollY - style.FramePadding.y) / lineHeight));
    const int last = std::min(lineCount, first + static_cast<int>(height / lineHeight) + 2);
    char number[16];
    for (int i = first; i < last; ++i) {
        const float y = top + static_cast<float>(i) * lineHeight;
        const uint8_t state = static_cast<size_t>(i) < st.lineStates.size() ? st.lineStates[static_cast<size_t>(i)] : 0;
        const ImVec4 color = LineColor(state);
        if (state != ScriptEngine::LineNone) {
            const float alpha = state == ScriptEngine::LineCurrent ? 0.20f : state == ScriptEngine::LineError ? 0.24f : 0.10f;
            dl->AddRectFilled({ editorMin.x, y }, { end.x - S(1), y + lineHeight }, Col(color, alpha));
            dl->AddRectFilled({ editorMin.x - S(4), y + S(1) }, { editorMin.x, y + lineHeight - S(1) }, Col(color), S(2));
        }
        const int n = std::snprintf(number, sizeof(number), "%d", i + 1);
        const float numberWidth = ImGui::CalcTextSize(number, number + n).x;
        dl->AddText({ editorMin.x - S(12) - numberWidth, y }, Col(state != ScriptEngine::LineNone ? color : Theme::Faint), number, number + n);
    }
    dl->PopClipRect();
    if (st.mono) ImGui::PopFont();

    if (focused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        std::string error;
        if (ScriptEngine::SaveFile(st.fileName, st.editor, error)) {
            st.savedEditor = st.editor;
            st.scriptMessage = "saved " + st.fileName;
            st.nextFileScan = {};
        }
        else st.scriptMessage = error;
    }
}

void App::DrawScriptsTab() {
    State& st = *state_;
    const int selectedId = st.selected ? st.selected->Id() : 0;
    const bool dirty = st.editor != st.savedEditor;

    const auto save = [&] {
        std::string error;
        if (ScriptEngine::SaveFile(st.fileName, st.editor, error)) {
            st.savedEditor = st.editor;
            st.scriptMessage = "saved " + st.fileName;
            st.nextFileScan = {};
        }
        else st.scriptMessage = error;
    };
    const auto load = [&](const std::string& name) {
        std::string code, error;
        if (ScriptEngine::LoadFile(name, code, error)) {
            st.fileName = name;
            st.editor = st.savedEditor = code;
            st.scriptMessage = "loaded " + name;
        }
        else st.scriptMessage = error;
    };
    const auto run = [&] {
        st.traceId = scripts_.Run(st.fileName, st.editor, selectedId, false, true);
        st.traceCode = st.editor;
    };

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(12)));
    ImGui::BeginChild("##files", ImVec2(S(220), 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    CardTitle(st.bold, "Files", "scripts/");
    const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(4), S(5)));
    if (TintButton("New", Theme::Aqua, ImVec2(third, 0))) {
        st.fileName = "new.lua";
        st.editor = st.savedEditor = DEFAULT_SCRIPT;
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(third, 0))) st.nextFileScan = {};
    ImGui::SameLine();
    if (ImGui::Button("Folder", ImVec2(third, 0))) ShellExecuteW(nullptr, L"open", ScriptEngine::Folder().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, S(2)));
    for (const auto& name : st.files) {
        if (ImGui::Selectable(name.c_str(), name == st.fileName, ImGuiSelectableFlags_None, ImVec2(0, S(26)))) load(name);
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Run")) {
                std::string code, error;
                if (ScriptEngine::LoadFile(name, code, error)) scripts_.Run(name, code, selectedId);
            }
            if (ImGui::MenuItem("Delete")) {
                std::string error;
                if (ScriptEngine::RemoveFile(name, error)) st.scriptMessage = "deleted " + name;
                else st.scriptMessage = error;
                st.nextFileScan = {};
            }
            ImGui::EndPopup();
        }
    }
    if (st.files.empty()) Caption("No .lua files yet.");
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##editorpane");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(S(200));
    ImGui::InputText("##filename", &st.fileName);
    ImGui::SameLine();
    if (ImGui::Button(dirty ? "Save *" : "Save")) save();
    ImGui::SameLine();
    if (ImGui::Button("Load")) load(st.fileName);
    ImGui::SameLine(0, S(18));
    if (ColorButton("Run  F5", Theme::Green)) run();
    ImGui::SameLine();
    if (TintButton("Run for every bot", Theme::Green)) {
        int traced = 0;
        for (const auto& bot : manager_.All()) {
            const bool trace = bot->Id() == selectedId || (selectedId == 0 && traced == 0);
            const int id = scripts_.Run(st.fileName, st.editor, bot->Id(), false, trace);
            if (trace) traced = id;
        }
        if (traced) {
            st.traceId = traced;
            st.traceCode = st.editor;
        }
    }
    ImGui::SameLine();
    if (DangerButton("Stop all")) scripts_.StopAll();
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) run();

    const bool traced = st.traceId != 0 && st.editor == st.traceCode && !st.lineStates.empty();
    Chip(st.selected ? std::format("getBot() = {}", st.selected->GrowId()).c_str() : "getBot() = first bot", Theme::Aqua, false);
    if (traced) {
        int current = 0;
        bool error = false;
        for (size_t i = 0; i < st.lineStates.size(); ++i) {
            if (st.lineStates[i] == ScriptEngine::LineCurrent) current = static_cast<int>(i) + 1;
            if (st.lineStates[i] == ScriptEngine::LineError) error = true;
        }
        ImGui::SameLine(0, S(6));
        if (error) Chip("stopped on an error", Theme::Red);
        else if (current) Chip(std::format("running line {}", current).c_str(), Theme::Yellow);
        else Chip("finished", Theme::Green);
    }
    else if (st.traceId != 0 && st.editor != st.traceCode) {
        ImGui::SameLine(0, S(6));
        Chip("edited since the run: colours hidden", Theme::Faint, false);
    }
    if (!st.scriptMessage.empty()) {
        ImGui::SameLine(0, S(12));
        ImGui::TextDisabled("%s", st.scriptMessage.c_str());
    }
    {
        const char* keys[] = { "running", "done", "error" };
        const ImVec4 colors[] = { Theme::Yellow, Theme::Green, Theme::Red };
        float width = 0.0f;
        for (const char* k : keys) width += ImGui::CalcTextSize(k).x + S(30) + S(6);
        const float x = ImGui::GetContentRegionMax().x - width;
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() < x) {
            ImGui::SetCursorPosX(x);
            for (int i = 0; i < 3; ++i) {
                if (i) ImGui::SameLine(0, S(6));
                Chip(keys[i], colors[i]);
            }
        }
        else ImGui::NewLine();
    }

    const float avail = ImGui::GetContentRegionAvail().y;
    DrawEditor(std::max(S(160), avail * 0.58f));

    const auto list = scripts_.List();
    const size_t running = static_cast<size_t>(std::ranges::count_if(list, [](const auto& i) { return i.running; }));
    const float consoleHeight = ImGui::GetFrameHeightWithSpacing();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(8)));
    ImGui::BeginChild("##bottom", ImVec2(0, -consoleHeight), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    if (ImGui::BeginTabBar("##outtabs")) {
        if (ImGui::BeginTabItem("   Output   ###output")) {
            if (ImGui::SmallButton("Clear##out")) {
                scripts_.ClearOutput();
                st.output.clear();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll##out", &st.outputAutoScroll);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            ImGui::BeginChild("##output");
            ImGui::PopStyleColor();
            if (st.mono) ImGui::PushFont(st.mono, 0.0f);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(st.output.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto& line = st.output[static_cast<size_t>(i)];
                    ImGui::TextColored(Theme::Faint, "%3d", line.script);
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(line.level));
                    ImGui::TextUnformatted(line.text.c_str());
                    ImGui::PopStyleColor();
                }
            }
            if (st.mono) ImGui::PopFont();
            if (st.outputAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5.0f) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(std::format("   Running  {}   ###running", running).c_str())) {
            if (ImGui::SmallButton("Clear finished")) scripts_.ClearFinished();
            if (ImGui::BeginTable("##running", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, S(30));
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Bot", ImGuiTableColumnFlags_WidthFixed, S(40));
                ImGui::TableSetupColumn("State");
                ImGui::TableSetupColumn("Time / events");
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(60));
                ImGui::TableHeadersRow();
                for (auto it = list.rbegin(); it != list.rend(); ++it) {
                    const auto& info = *it;
                    ImGui::TableNextRow();
                    ImGui::PushID(info.id);
                    ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("%d", info.id);
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(info.name.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%d", info.bot);
                    ImGui::TableSetColumnIndex(3);
                    if (info.running) Chip("running", Theme::Yellow);
                    else if (info.failed) {
                        Chip("error", Theme::Red);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.error.c_str());
                    }
                    else Chip("finished", Theme::Green);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.1f s / %llu%s", info.seconds, static_cast<unsigned long long>(info.events),
                        info.dropped ? std::format(" ({} dropped)", info.dropped).c_str() : "");
                    ImGui::TableSetColumnIndex(5);
                    if (info.running && ImGui::SmallButton("Stop")) scripts_.Stop(info.id);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(-FLT_MIN);
    const auto historyCallback = [](ImGuiInputTextCallbackData* data) -> int {
        auto& s = *static_cast<State*>(data->UserData);
        if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || s.history.empty()) return 0;
        if (data->EventKey == ImGuiKey_UpArrow) s.historyPos = s.historyPos < 0 ? static_cast<int>(s.history.size()) - 1 : std::max(0, s.historyPos - 1);
        else if (data->EventKey == ImGuiKey_DownArrow && s.historyPos >= 0) {
            if (++s.historyPos >= static_cast<int>(s.history.size())) s.historyPos = -1;
        }
        data->DeleteChars(0, data->BufTextLen);
        if (s.historyPos >= 0) data->InsertChars(0, s.history[static_cast<size_t>(s.historyPos)].c_str());
        return 0;
    };
    if (st.mono) ImGui::PushFont(st.mono, 0.0f);
    if (ImGui::InputTextWithHint("##console", "Lua >  getBot():warp(\"START\")   or   getBot().gems", &st.console,
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, historyCallback, &st)) {
        if (!Text::Trim(st.console).empty()) {
            scripts_.Print(0, Log::Level::Debug, "> " + st.console);
            scripts_.Run("console", st.console, selectedId, true);
            if (st.history.empty() || st.history.back() != st.console) st.history.push_back(st.console);
        }
        st.console.clear();
        st.historyPos = -1;
        ImGui::SetKeyboardFocusHere(-1);
    }
    if (st.mono) ImGui::PopFont();
    ImGui::EndChild();
}

void App::DrawLogTab() {
    State& st = *state_;
    ImGui::SetNextItemWidth(S(260));
    ImGui::InputTextWithHint("##logfilter", "Filter text or bot name", &st.logFilter);
    const auto toggle = [](const char* label, bool& value, ImVec4 color) {
        ImGui::SameLine(0, S(6));
        if (value ? TintButton(label, color) : ImGui::Button(label)) value = !value;
    };
    toggle("Info", st.showInfo, Theme::Text);
    toggle("Warn", st.showWarn, Theme::Yellow);
    toggle("Error", st.showError, Theme::Red);
    toggle("Debug", st.showDebug, Theme::Aqua);
    ImGui::SameLine(0, S(18)); ImGui::Checkbox("Auto-scroll", &st.logAutoScroll);
    ImGui::SameLine();
    bool debug = Log::DebugEnabled();
    if (ImGui::Checkbox("Debug logging", &debug)) Log::EnableDebug(debug);
    ImGui::SameLine();
    if (DangerButton("Clear")) {
        Log::Clear();
        st.log.clear();
        st.logDirty = true;
    }

    const bool levels[4]{ st.showInfo, st.showWarn, st.showError, st.showDebug };
    if (st.logFilter != st.appliedFilter || !std::equal(levels, levels + 4, st.appliedLevels)) {
        st.appliedFilter = st.logFilter;
        std::copy(levels, levels + 4, st.appliedLevels);
        st.logDirty = true;
    }
    if (st.logDirty) {
        st.logView.clear();
        for (const auto& e : st.log) {
            if (st.LogPasses(e)) st.logView.push_back(e.seq);
        }
        st.logDirty = false;
    }

    ImGui::BeginChild("##logview", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    if (st.mono) ImGui::PushFont(st.mono, 0.0f);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(st.logView.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& e = st.log[static_cast<size_t>(st.logView[static_cast<size_t>(i)] - st.log.front().seq)];
            ImGui::TextColored(Theme::Faint, "%s", e.time);
            ImGui::SameLine();
            ImGui::TextColored(Theme::Aqua, "%-10s", e.tag.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(e.level));
            ImGui::TextUnformatted(e.message.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (st.mono) ImGui::PopFont();
    if (st.logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void App::DrawSettingsTab() {
    State& st = *state_;
    if (!st.settingsLoaded) {
        st.settings = manager_.Settings();
        st.settingsLoaded = true;
    }
    Config::Settings& s = st.settings;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##settings");
    ImGui::PopStyleColor();
    const bool wide = ImGui::GetContentRegionAvail().x > S(820);
    const float half = wide ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f : 0.0f;
    const float label = S(170);
    const auto field = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        Caption(text);
        ImGui::SameLine(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
    };

    if (BeginCard("##client", half)) {
        CardTitle(st.bold, "Game client", "raise these after a Growtopia update");
        field("Game version"); ImGui::InputText("##gamever", &s.client.gameVersion);
        field("Protocol"); ImGui::InputInt("##protocol", &s.client.protocol);
        int fhash = s.client.fhash;
        field("fhash"); if (ImGui::InputInt("##fhash", &fhash)) s.client.fhash = fhash;
    }
    EndCard();
    if (wide) ImGui::SameLine();
    if (BeginCard("##timing", half)) {
        CardTitle(st.bold, "Timing");
        field("Login interval (ms)"); ImGui::InputInt("##login", &s.loginIntervalMs, 500);
        field("Rejoin delay (ms)"); ImGui::InputInt("##rejoin", &s.rejoinDelayMs, 500);
        s.loginIntervalMs = std::max(0, s.loginIntervalMs);
        s.rejoinDelayMs = std::max(0, s.rejoinDelayMs);
        Caption("Gap between logins / wait after a failed world join.");
    }
    EndCard();

    if (BeginCard("##misc")) {
        CardTitle(st.bold, "Logging");
        ImGui::Checkbox("Debug logging", &s.debug);
    }
    EndCard();

    if (BeginCard("##items")) {
        CardTitle(st.bold, "Items (items.dat)", ItemDB::Ready() ? "loaded" : "not loaded");
        ImGui::TextWrapped("%s", ItemDB::Status().c_str());
        Caption("Read from items.dat next to GrowtopiaBot.exe, else from the game's cache folder.");
        Caption("Without it item names are unknown and the pathfinder only walks through air.");
    }
    EndCard();

    if (PrimaryButton("Apply")) {
        manager_.SetCommon(s);
        st.settingsMessage = "applied";
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) st.settingsLoaded = false;
    if (!st.settingsMessage.empty()) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Theme::Teal, "%s", st.settingsMessage.c_str());
    }
    Caption("Changes apply to bots added / restarted afterwards. Nothing is written to disk:");
    Caption("bots and settings last until the program closes.");
    ImGui::EndChild();
}

void App::DrawStatusBar() {
    State& st = *state_;
    const auto now = Clock::now();
    if (now >= st.nextStats) {
        FILETIME created, exited, kernel, user;
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            const ULONGLONG total = (ULARGE_INTEGER{ { kernel.dwLowDateTime, kernel.dwHighDateTime } }.QuadPart) +
                (ULARGE_INTEGER{ { user.dwLowDateTime, user.dwHighDateTime } }.QuadPart);
            if (st.lastCpu != 0) {
                const double wall = std::chrono::duration<double>(now - st.lastCpuAt).count();
                st.cpuPercent = static_cast<double>(total - st.lastCpu) / 1e7 / wall / st.cores * 100.0;
            }
            st.lastCpu = total;
            st.lastCpuAt = now;
        }
        PROCESS_MEMORY_COUNTERS memory{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) st.memoryMb = static_cast<double>(memory.WorkingSetSize) / (1024.0 * 1024.0);
        st.nextStats = now + 1s;
    }
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(2));
    const auto item = [](const char* label, const std::string& value, ImVec4 color) {
        ImGui::TextColored(Theme::Faint, "%s", label);
        ImGui::SameLine(0, S(5));
        ImGui::TextColored(color, "%s", value.c_str());
        ImGui::SameLine(0, S(22));
    };
    item("CPU", std::format("{:.1f} %", st.cpuPercent), Theme::Text);
    item("RAM", std::format("{:.1f} MB", st.memoryMb), Theme::Text);
    item("Bots", std::to_string(st.rows.size()), Theme::Aqua);
    item("Scripts", std::format("{} running", scripts_.RunningCount()), Theme::Green);
    item("FPS", std::format("{:.0f}", ImGui::GetIO().Framerate), Theme::Muted);
    ImGui::NewLine();
}
