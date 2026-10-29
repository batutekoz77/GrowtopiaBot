#include "Protocol.hpp"

#include <cstring>
#include <format>

namespace Protocol {
    namespace {
        const Variant EMPTY_VARIANT;

        struct Cursor {
            std::span<const uint8_t> data;
            size_t pos = 0;

            template <typename T>
            bool Read(T& value) {
                if (pos + sizeof(T) > data.size()) return false;
                std::memcpy(&value, data.data() + pos, sizeof(T));
                pos += sizeof(T);
                return true;
            }
        };
    }

    int32_t MessageOf(std::span<const uint8_t> data) noexcept {
        if (data.size() < 4) return -1;
        int32_t type = 0;
        std::memcpy(&type, data.data(), 4);
        return type;
    }

    std::string_view TextOf(std::span<const uint8_t> data) noexcept {
        if (data.size() <= 4) return {};
        const char* text = reinterpret_cast<const char*>(data.data() + 4);
        const size_t size = data.size() - 4;
        const void* nul = std::memchr(text, 0, size);
        return { text, nul ? static_cast<size_t>(static_cast<const char*>(nul) - text) : size - 1 };
    }

    std::optional<TankPacket> TankOf(std::span<const uint8_t> data) noexcept {
        if (data.size() < EXTENDED_OFFSET) return std::nullopt;
        TankPacket tank;
        std::memcpy(&tank, data.data() + TANK_OFFSET, sizeof(tank));
        return tank;
    }

    std::span<const uint8_t> ExtendedOf(std::span<const uint8_t> data) noexcept {
        const auto tank = TankOf(data);
        if (!tank || !(tank->flags & STATE_EXTENDED)) return {};
        const size_t available = data.size() - EXTENDED_OFFSET;
        return data.subspan(EXTENDED_OFFSET, std::min<size_t>(tank->extendedSize, available));
    }

    std::vector<uint8_t> MakeText(int32_t type, std::string_view text) {
        std::vector<uint8_t> payload(4 + text.size() + 1, 0);
        std::memcpy(payload.data(), &type, 4);
        std::memcpy(payload.data() + 4, text.data(), text.size());
        return payload;
    }

    std::vector<uint8_t> MakeTank(const TankPacket& source, std::span<const uint8_t> extended) {
        TankPacket tank = source;
        if (!extended.empty()) {
            tank.flags |= STATE_EXTENDED;
            tank.extendedSize = static_cast<uint32_t>(extended.size());
        }
        else {
            tank.flags &= ~static_cast<uint32_t>(STATE_EXTENDED);
            tank.extendedSize = 0;
        }

        std::vector<uint8_t> payload(EXTENDED_OFFSET + extended.size(), 0);
        const int32_t type = NET_MESSAGE_GAME_PACKET;
        std::memcpy(payload.data(), &type, 4);
        std::memcpy(payload.data() + TANK_OFFSET, &tank, sizeof(tank));
        if (!extended.empty()) std::memcpy(payload.data() + EXTENDED_OFFSET, extended.data(), extended.size());
        return payload;
    }

    int32_t Variant::Int() const noexcept {
        switch (type) {
            case Type::Int: case Type::UInt: case Type::Int64: return static_cast<int32_t>(integer);
            case Type::Float: return static_cast<int32_t>(values[0]);
            default: return 0;
        }
    }

    std::string Variant::ToString() const {
        switch (type) {
            case Type::String: return text;
            case Type::Float: return std::format("{}", values[0]);
            case Type::Vec2: return std::format("{}, {}", values[0], values[1]);
            case Type::Vec3: return std::format("{}, {}, {}", values[0], values[1], values[2]);
            case Type::UInt: case Type::Int: case Type::Int64: return std::to_string(integer);
            default: return {};
        }
    }

    std::optional<VariantList> VariantList::Parse(std::span<const uint8_t> extended) {
        Cursor cursor{ extended };
        uint8_t count = 0;
        if (!cursor.Read(count)) return std::nullopt;

        VariantList list;
        list.values_.reserve(count);
        for (uint8_t i = 0; i < count; ++i) {
            uint8_t index = 0, type = 0;
            if (!cursor.Read(index) || !cursor.Read(type)) return std::nullopt;

            Variant v;
            v.type = static_cast<Variant::Type>(type);
            switch (v.type) {
                case Variant::Type::Float:
                    if (!cursor.Read(v.values[0])) return std::nullopt;
                    break;
                case Variant::Type::String: {
                    uint32_t length = 0;
                    if (!cursor.Read(length) || cursor.pos + length > extended.size()) return std::nullopt;
                    v.text.assign(reinterpret_cast<const char*>(extended.data() + cursor.pos), length);
                    cursor.pos += length;
                    break;
                }
                case Variant::Type::Vec2:
                    if (!cursor.Read(v.values[0]) || !cursor.Read(v.values[1])) return std::nullopt;
                    break;
                case Variant::Type::Vec3:
                    if (!cursor.Read(v.values[0]) || !cursor.Read(v.values[1]) || !cursor.Read(v.values[2])) return std::nullopt;
                    break;
                case Variant::Type::UInt: {
                    uint32_t value = 0;
                    if (!cursor.Read(value)) return std::nullopt;
                    v.integer = value;
                    break;
                }
                case Variant::Type::Int: {
                    int32_t value = 0;
                    if (!cursor.Read(value)) return std::nullopt;
                    v.integer = value;
                    break;
                }
                case Variant::Type::Int64: {
                    int64_t value = 0;
                    if (!cursor.Read(value)) return std::nullopt;
                    v.integer = value;
                    break;
                }
                default:
                    return list;
            }

            if (index >= list.values_.size()) list.values_.resize(index + 1);
            list.values_[index] = std::move(v);
        }
        return list;
    }

    const Variant& VariantList::operator[](size_t index) const noexcept {
        return index < values_.size() ? values_[index] : EMPTY_VARIANT;
    }

    std::string_view VariantList::Function() const noexcept {
        return values_.empty() ? std::string_view{} : values_[0].String();
    }
}
