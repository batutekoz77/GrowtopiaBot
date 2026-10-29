#include "Bench.hpp"
#include "Config.hpp"
#include "../Bot/BotManager.hpp"
#include "../Core/Log.hpp"
#include "../Game/Items.hpp"
#include "../Script/ScriptEngine.hpp"
#include "../UI/App.hpp"

#include "ENET/enet.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <format>
#include <string>

namespace {
    void ShowError(const std::string& text) {
        const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<size_t>(std::max(size, 1)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
        MessageBoxW(nullptr, wide.c_str(), L"GrowtopiaBot", MB_OK | MB_ICONERROR);
    }
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--bench") == 0) return Bench::Run(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "--bench-ui") == 0) return Bench::RunUi(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "--selftest") == 0) return Bench::RunTests(argc, argv);

    Config::Settings settings;
    Log::EnableDebug(settings.debug);

    if (enet_initialize() != 0) {
        ShowError("enet_initialize failed");
        return 1;
    }

    Log::Info("MAIN", "Growtopia {} / protocol {}", settings.client.gameVersion, settings.client.protocol);
    ItemDB::LoadAsync();

    int exitCode = 0;
    {
        BotManager manager(settings);
        ScriptEngine scripts(manager);

        App app(manager, scripts);
        if (!app.Run()) {
            ShowError("Couldn't start DirectX 11 for the panel.");
            exitCode = 1;
        }

        Log::Info("MAIN", "shutting down...");
        scripts.StopAll();
        manager.StopAll();
    }
    enet_deinitialize();
    return exitCode;
}
