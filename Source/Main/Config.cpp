#include "Config.hpp"
#include "../Core/Text.hpp"

#include <windows.h>

namespace Config {
    std::filesystem::path ExeDirectory() {
        wchar_t buffer[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return std::filesystem::path(buffer).parent_path();
    }

    bool SetProxy(BotEntry& entry, const std::string& text) {
        entry.proxyText = std::string(Text::Trim(text));
        entry.proxy.reset();
        if (entry.proxyText.empty()) return true;
        entry.proxy = Net::Socks5Config::Parse(entry.proxyText);
        return entry.proxy.has_value();
    }
}
