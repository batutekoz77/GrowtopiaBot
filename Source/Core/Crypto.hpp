#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Crypto {
    std::string Md5(std::string_view text);

    int32_t HashString(std::string_view text) noexcept;

    std::string Klv(std::string_view gameVersion, std::string_view protocol, std::string_view rid, int32_t hash);

    std::string RandomHex(size_t count);
    std::string RandomMac();
    std::string GenerateRid();
}
