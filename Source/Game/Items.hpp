#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

/* @info: the item table, parsed from items.dat (ported from AquaProxy).
   Shared by every bot: loaded once on a background thread, read-only after
   that, so lookups need no lock. They return nullptr until it has finished. */
namespace ItemDB {
    namespace Type {
        enum : uint8_t {
            FIST = 0x00, WRENCH = 0x01, DOOR = 0x02, LOCK = 0x03, GEM = 0x04, TREASURE = 0x05, DEADLY = 0x06,
            TRAMPOLINE = 0x07, CONSUMEABLE = 0x08, ENTRANCE = 0x09, SIGN = 0x0a, SFX_BLOCK = 0x0b,
            TOGGLEABLE_ANIMATED_BLOCK = 0x0c, MAIN_DOOR = 0x0d, PLATFORM = 0x0e, STRONG = 0x0f, FIRE_PAIN = 0x10,
            FOREGROUND = 0x11, BACKGROUND = 0x12, SEED = 0x13, CLOTHING = 0x14, ANIMATED = 0x15,
            SFX_BACKGROUND = 0x16, ART_WALL = 0x17, BOUNCY = 0x18, STING_PAIN = 0x19, PORTAL = 0x1a,
            CHECKPOINT = 0x1b, MUSIC_SHEET = 0x1c, SLIPPERY = 0x1d, TOGGLEABLE_BLOCK = 0x1f, CHEST = 0x20,
            MAILBOX = 0x21, BULLETIN = 0x22, PINATA = 0x23, RANDOM = 0x24, COMPONENT = 0x25, PROVIDER = 0x26,
            CHEMICAL_COMBINER = 0x27, ACHIEVEMENT = 0x28, WEATHER_MACHINE = 0x29, SCOREBOARD = 0x2a,
            SUNGATE = 0x2b, TOGGLEABLE_DEADLY = 0x2d, HEART_MONITOR = 0x2e, DONATION_BOX = 0x2f,
            MANNEQUIN = 0x31, CCTV = 0x32, MAGIC_EGG = 0x33, GAME_BLOCK = 0x34, GAME_GENERATOR = 0x35,
            XENONITE = 0x36, BOOTH = 0x37, CRYSTAL = 0x38, CRIME_IN_PROGRESS = 0x39, GRINDER = 0x3a,
            SPOTLIGHT = 0x3b, PUSHING_BLOCK = 0x3c, DISPLAY_BLOCK = 0x3d, VENDING_MACHINE = 0x3e,
            FISH_TANK_PORT = 0x3f, FISH = 0x40, SOLAR_COLLECTOR = 0x41, FORGE = 0x42, GIVING_TREE = 0x43,
            GIVING_TREE_STUMP = 0x44, STEAM_BLOCK = 0x45, STEAM_VENT = 0x46, STEAM_ORGAN = 0x47,
            SILKWORM = 0x48, SEWING_MACHINE = 0x49, COUNTRY_FLAG = 0x4a, LOBSTER_TRAP = 0x4b,
            PAINTING_EASEL = 0x4c, BATTLE_PET_CAGE = 0x4d, PET_TRAINER = 0x4e, STEAM_ENGINE = 0x4f,
            LOCK_BOT = 0x50, SFX_WEATHER_MACHINE = 0x51, SPIRIT_STORAGE = 0x52, DISPLAY_SHELF = 0x53,
            VIP_ENTRANCE = 0x54, CHALLENGE_TIMER = 0x55, CHALLENGE_FLAG = 0x56, FISH_MOUNT = 0x57,
            PORTRAIT = 0x58, SPRITE_WEATHER_MACHINE = 0x59, FOSSIL = 0x5a, FOSSIL_PREP_STATION = 0x5b,
            DNA_PROCESSOR = 0x5c, TRICKSTER = 0x5d, VALHOWLA_TREASURE = 0x5e, CHEMSYNTH_PROCESSOR = 0x5f,
            CHEMSYNTH_TANK = 0x60, STORAGE_BOX = 0x61, COOKING_OVEN = 0x62, AUDIO_BLOCK = 0x63,
            GEIGER_CHARGER = 0x64, THE_ADVENTURE_BEGINS = 0x65, TOMB_ROBBER = 0x66, BALLOON_O_MATIC = 0x67,
            TEAM_ENTRANCE_PUNCH = 0x68, TEAM_ENTRANCE_GROW = 0x69, TEAM_ENTRANCE_BUILD = 0x6a, AURA = 0x6b,
            LEMON_JELLY_BLOCK = 0x6c, TRAINING_PORT = 0x6d, FISHING_BLOCK = 0x6e, MAGPLANT = 0x6f,
            MAGPLANT_REMOTE = 0x70, CYBLOCK_BOT = 0x71, CYBLOCK_COMMAND = 0x72, LUCKY_TOKEN = 0x73,
            GROWSCAN = 0x74, CONTAINMENT_FIELD_POWER_NODE = 0x75, SPIRIT_BOARD = 0x76, WORLD_ARCHITECT = 0x77,
            STARTOPIA_BLOCK = 0x78, TOGGLEABLE_MULTI_FRAME_BLOCK = 0x7a, AUTO_BREAK_BLOCK = 0x7b,
            AUTO_BREAK_TREE = 0x7c, AUTO_BREAK = 0x7d, STORM_CLOUD = 0x7e, DISAPPEAR_WHEN_STEPPED_ON = 0x7f,
            PUDDLE_BLOCK = 0x80, ROOT_CUTTING = 0x81, SAFE_VAULT = 0x82, ANGELIC_COUNTING_CLOUD = 0x83,
            MINING_EXPLOSIVE = 0x84, INFINITY_WEATHER_MACHINE = 0x86, GHOST_BLOCK = 0x87, ACID = 0x88,
            WAVING_INFLATABLE_ARM_GUY = 0x8a, PINEAPPLE_GUZZLER = 0x8c, KRANKEN_GALACTIC = 0x8d,
            FRIEND_ENTRANCE = 0x8e,
        };
    }

    namespace Collision {
        enum : uint8_t {
            NO_COLLISION, FULL, ON_TOP /* platforms */, IF_ACCESS /* entrance */, TOGGLE, HORIZONTAL,
            IF_VIP /* VIP entrance */, VERTICAL, ADVENTURE_ITEM, ACTIVATE, BALLOON_WARZ_TEAM /* team entrance */,
            IF_GUILD /* guild entrance */, STEP_ON,
        };
    }

    namespace Category {
        enum : uint8_t {
            RETURN = 0x02,          /* smashing it returns it to the backpack */
            SURPRISING_FRUIT = 0x08,
            PUBLIC = 0x10,          /* anyone can smash it, even when locked */
            HOLIDAY = 0x40,
            UNTRADEABLE = 0x80,     /* can't be dropped or traded */
        };
    }

    struct Item {
        uint16_t id = 0;
        uint8_t property = 0;
        uint8_t category = 0;  /* Category flags */
        uint8_t type = 0;      /* Type */
        std::string name;      /* exactly as in the game, e.g. "Dirt Seed" */
        int ingredient = 0;
        uint8_t collision = 0; /* Collision */
        uint8_t hits = 0;      /* punches to break */
        int hitReset = 0;      /* seconds */
        uint8_t clothType = 0;
        int16_t rarity = 0;
        int growTime = 0;      /* seconds (trees, providers, ...) */
        std::string info;      /* items.dat v22+ */
        std::array<uint16_t, 2> splice{}; /* items.dat v23+ */
    };

    /* Starts the one-and-only load: items.dat next to GrowtopiaBot.exe, else
       the game's cache (%LOCALAPPDATA%\Growtopia\cache\items.dat). */
    void LoadAsync();
    bool Ready() noexcept;
    /* Where it was loaded from, or why it wasn't (for the panel). */
    std::string Status();
    size_t Count() noexcept;

    const Item* Find(int id) noexcept;
    /* Case-insensitive. */
    const Item* Find(std::string_view name);
    /* "Dirt", or "#2" while items.dat isn't loaded / for an unknown id. */
    std::string Name(int id);

    bool IsBackground(int id) noexcept;
}
