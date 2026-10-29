#pragma once

#include "../Game/Login.hpp"
#include "../Net/Socket.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Config {
    struct BotEntry {
        std::string growId;
        std::string password;
        std::optional<Net::Socks5Config> proxy;
        std::string proxyText;
    };

    struct Common {
        Login::ClientInfo client{ "5.61", 225, -716928004 };
        int loginIntervalMs = 3000;
        int rejoinDelayMs = 5000;
        bool debug = false;
        std::string benchServer;
    };

    struct Settings : Common {
        std::vector<BotEntry> bots;
    };

    std::filesystem::path ExeDirectory();
    bool SetProxy(BotEntry& entry, const std::string& text);
}
