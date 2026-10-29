#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Protocol {

    enum MessageType : int32_t {
        NET_MESSAGE_UNKNOWN = 0,
        NET_MESSAGE_SERVER_HELLO = 1,
        NET_MESSAGE_GENERIC_TEXT = 2,
        NET_MESSAGE_GAME_MESSAGE = 3,
        NET_MESSAGE_GAME_PACKET = 4,
        NET_MESSAGE_ERROR = 5,
        NET_MESSAGE_TRACK = 6,
        NET_MESSAGE_CLIENT_LOG_REQUEST = 7,
        NET_MESSAGE_CLIENT_LOG_RESPONSE = 8,
    };

    enum TankType : uint8_t {
        PACKET_STATE = 0,
        PACKET_CALL_FUNCTION = 1,
        PACKET_UPDATE_STATUS = 2,
        PACKET_TILE_CHANGE_REQUEST = 3,
        PACKET_SEND_MAP_DATA = 4,
        PACKET_SEND_TILE_UPDATE_DATA = 5,
        PACKET_SEND_TILE_UPDATE_DATA_MULTIPLE = 6,
        PACKET_TILE_ACTIVATE_REQUEST = 7,
        PACKET_TILE_APPLY_DAMAGE = 8,
        PACKET_SEND_INVENTORY_STATE = 9,
        PACKET_ITEM_ACTIVATE_REQUEST = 10,
        PACKET_ITEM_ACTIVATE_OBJECT_REQUEST = 11,
        PACKET_SEND_TILE_TREE_STATE = 12,
        PACKET_MODIFY_ITEM_INVENTORY = 13,
        PACKET_ITEM_CHANGE_OBJECT = 14,
        PACKET_SEND_LOCK = 15,
        PACKET_SEND_ITEM_DATABASE_DATA = 16,
        PACKET_SEND_PARTICLE_EFFECT = 17,
        PACKET_SET_ICON_STATE = 18,
        PACKET_ITEM_EFFECT = 19,
        PACKET_SET_CHARACTER_STATE = 20,
        PACKET_PING_REPLY = 21,
        PACKET_PING_REQUEST = 22,
        PACKET_GOT_PUNCHED = 23,
        PACKET_APP_CHECK_RESPONSE = 24,
        PACKET_APP_INTEGRITY_FAIL = 25,
        PACKET_DISCONNECT = 26,
        PACKET_BATTLE_JOIN = 27,
        PACKET_BATTLE_EVENT = 28,
        PACKET_USE_DOOR = 29,
        PACKET_SEND_PARENTAL = 30,
        PACKET_GONE_FISHIN = 31,
        PACKET_STEAM = 32,
        PACKET_PET_BATTLE = 33,
        PACKET_NPC = 34,
        PACKET_SPECIAL = 35,
    };

    enum StateFlags : uint32_t {
        STATE_WALK = 0x1,
        STATE_EXTENDED = 0x8,
        STATE_FACING_LEFT = 0x10,
        STATE_STANDING = 0x20,
        STATE_JUMP = 0x80,
        STATE_PUNCH = 0x200,
        STATE_PLACE = 0x400,
    };

#pragma pack(push, 1)
    struct TankPacket {
        uint8_t  type = 0;
        uint8_t  objectType = 0;
        uint8_t  count1 = 0;
        uint8_t  count2 = 0;
        int32_t  netID = 0;
        int32_t  targetNetID = 0;
        uint32_t flags = 0;
        float    floatValue = 0.0f;
        int32_t  value = 0;
        float    x = 0.0f;
        float    y = 0.0f;
        float    xSpeed = 0.0f;
        float    ySpeed = 0.0f;
        int32_t  particle = 0;
        int32_t  punchX = 0;
        int32_t  punchY = 0;
        uint32_t extendedSize = 0;
    };
#pragma pack(pop)
    static_assert(sizeof(TankPacket) == 56, "TankPacket must match the wire layout");

    inline constexpr size_t TANK_OFFSET = 4;
    inline constexpr size_t EXTENDED_OFFSET = TANK_OFFSET + sizeof(TankPacket);

    int32_t MessageOf(std::span<const uint8_t> data) noexcept;
    std::string_view TextOf(std::span<const uint8_t> data) noexcept;
    std::optional<TankPacket> TankOf(std::span<const uint8_t> data) noexcept;
    std::span<const uint8_t> ExtendedOf(std::span<const uint8_t> data) noexcept;

    std::vector<uint8_t> MakeText(int32_t type, std::string_view text);
    std::vector<uint8_t> MakeTank(const TankPacket& tank, std::span<const uint8_t> extended = {});

    class Variant {
    public:
        enum class Type : uint8_t { Unknown = 0, Float = 1, String = 2, Vec2 = 3, Vec3 = 4, UInt = 5, Int64 = 6, Int = 9 };

        Type type = Type::Unknown;
        std::string text;
        float values[3]{};
        int64_t integer = 0;

        bool IsString() const noexcept { return type == Type::String; }
        std::string_view String() const noexcept { return text; }
        int32_t Int() const noexcept;
        float X() const noexcept { return values[0]; }
        float Y() const noexcept { return values[1]; }
        std::string ToString() const;
    };

    class VariantList {
    public:
        static std::optional<VariantList> Parse(std::span<const uint8_t> extended);

        size_t Size() const noexcept { return values_.size(); }
        bool Empty() const noexcept { return values_.empty(); }
        const Variant& operator[](size_t index) const noexcept;
        std::string_view Function() const noexcept;

    private:
        std::vector<Variant> values_;
    };
}
