#include "Crypto.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <format>
#include <mutex>
#include <random>
#include <stdexcept>

namespace Crypto {
    namespace {
        constexpr std::string_view KEY1 = "832aac071ffbcfc15bfe1d0a7ad15221";
        constexpr std::string_view KEY2 = "709296ddd04fc4074a7b443ecc0799aa";
        constexpr std::string_view KEY3 = "623de1e8fff22a2b3e0d7e01593e7c22";
        constexpr std::string_view KEY4 = "bb835e5a57e6c88e2449499ca487ced2";
        constexpr std::string_view KEY5 = "ea76e4d6009282186063fe9465f2d9ab";

        std::mt19937_64& Rng() {
            static thread_local std::mt19937_64 rng(std::random_device{}() ^
                static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
            return rng;
        }

        uint8_t RandomByte() { return static_cast<uint8_t>(Rng()() & 0xFF); }
    }

    std::string Md5(std::string_view text) {
        static BCRYPT_ALG_HANDLE algorithm = [] {
            BCRYPT_ALG_HANDLE handle = nullptr;
            if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0) throw std::runtime_error("BCrypt MD5 unavailable");
            return handle;
        }();

        uint8_t digest[16]{};
        if (BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()), digest, sizeof(digest)) != 0) {
            throw std::runtime_error("BCryptHash failed");
        }

        static constexpr char HEX[] = "0123456789ABCDEF";
        std::string out(32, '0');
        for (size_t i = 0; i < 16; ++i) {
            out[i * 2] = HEX[digest[i] >> 4];
            out[i * 2 + 1] = HEX[digest[i] & 0xF];
        }
        return out;
    }

    int32_t HashString(std::string_view text) noexcept {
        uint32_t h = 0x55555555;
        const auto step = [&](uint8_t b) { h = ((h << 5) | (h >> 27)) + b; };
        for (const char c : text) step(static_cast<uint8_t>(c));
        step(0);
        return static_cast<int32_t>(h);
    }

    std::string Klv(std::string_view gameVersion, std::string_view protocol, std::string_view rid, int32_t hash) {
        std::string combined;
        combined += Md5(Md5(gameVersion));
        combined += KEY1;
        combined += Md5(Md5(Md5(protocol)));
        combined += KEY2;
        combined += KEY3;
        combined += Md5(Md5(rid));
        combined += KEY4;
        combined += Md5(Md5(std::to_string(hash)));
        combined += KEY5;
        return Md5(combined);
    }

    std::string RandomHex(size_t count) {
        static constexpr char HEX[] = "0123456789ABCDEF";
        std::string out(count, '0');
        for (char& c : out) c = HEX[RandomByte() & 0xF];
        return out;
    }

    std::string RandomMac() {
        return std::format("02:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}", RandomByte(), RandomByte(), RandomByte(), RandomByte(), RandomByte());
    }

    std::string GenerateRid() {
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return std::format("{:016X}{:016X}", 0ull, static_cast<unsigned long long>(nanos));
    }
}
