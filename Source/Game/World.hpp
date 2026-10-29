#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

/* @info: a world, parsed from PACKET_SEND_MAP_DATA and kept in sync by the
   tile/object update packets. Ported from AquaProxy (layout from the Mori bot,
   src/world/mod_impl.rs, map version 0x19+). Each bot owns one. */

namespace TileFlags {
    inline constexpr uint16_t HAS_EXTRA_DATA = 0x0001;
    inline constexpr uint16_t HAS_PARENT = 0x0002;
    inline constexpr uint16_t WAS_SPLICED = 0x0004;
    inline constexpr uint16_t WILL_SPAWN_SEEDS_TOO = 0x0008;
    inline constexpr uint16_t IS_SEEDLING = 0x0010;
    inline constexpr uint16_t FLIPPED_X = 0x0020;
    inline constexpr uint16_t IS_ON = 0x0040;
    inline constexpr uint16_t IS_OPEN_TO_PUBLIC = 0x0080;
    inline constexpr uint16_t BG_IS_ON = 0x0100;
    inline constexpr uint16_t FG_ALT_MODE = 0x0200;
    inline constexpr uint16_t IS_WET = 0x0400;
    inline constexpr uint16_t GLUED = 0x0800;
    inline constexpr uint16_t ON_FIRE = 0x1000;
    inline constexpr uint16_t PAINTED_RED = 0x2000;
    inline constexpr uint16_t PAINTED_GREEN = 0x4000;
    inline constexpr uint16_t PAINTED_BLUE = 0x8000;
}

namespace LockFlags {
    inline constexpr uint8_t IGNORE_EMPTY_AIR = 0x01;
    inline constexpr uint8_t DISABLE_MUSIC_NOTE = 0x10;
    inline constexpr uint8_t INVISIBLE_MUSIC_NOTE = 0x20;
    inline constexpr uint8_t BREAK_ONLY = 0x40;
    inline constexpr uint8_t ADMIN_LIMITED = 0x80;
}

enum class TilePermission : uint8_t { None = 0, Build = 1, Break = 2, FullAccess = 3 };

/* Parsed "tile extra data", one struct per kind byte. Kinds without a payload
   (Spotlight, LobsterTrap, ...) and the bytes nobody understands yet are not
   stored; WorldTile::extraKind still tells which kind it was. */
namespace TileExtra {
    enum Kind : uint8_t {
        NONE = 0,
        DOOR = 1, SIGN = 2, LOCK = 3, SEED = 4, DICE = 8, PROVIDER = 9, ACHIEVEMENT_BLOCK = 10,
        HEART_MONITOR = 11, MANNEQUIN = 14, BUNNY_EGG = 15, GAME_GRAVE = 16, GAME_GENERATOR = 17,
        XENONITE_CRYSTAL = 18, PHONE_BOOTH = 19, CRYSTAL = 20, CRIME_IN_PROGRESS = 21, SPOTLIGHT = 22,
        DISPLAY_BLOCK = 23, VENDING_MACHINE = 24, FISH_TANK_PORT = 25, SOLAR_COLLECTOR = 26, FORGE = 27,
        GIVING_TREE = 28, STEAM_ORGAN = 30, SILK_WORM = 31, SEWING_MACHINE = 32, COUNTRY_FLAG = 33,
        LOBSTER_TRAP = 34, PAINTING_EASEL = 35, PET_BATTLE_CAGE = 36, PET_TRAINER = 37, STEAM_ENGINE = 38,
        LOCK_BOT = 39, WEATHER_MACHINE = 40, SPIRIT_STORAGE_UNIT = 41, DATA_BEDROCK = 42, SHELF = 43,
        VIP_ENTRANCE = 44, CHALLENGE_TIMER = 45, FISH_WALL_MOUNT = 47, PORTRAIT = 48, WEATHER_MACHINE_2 = 49,
        FOSSIL_PREP_STATION = 50, DNA_EXTRACTOR = 51, HOWLER = 52, CHEMSYNTH_TANK = 53, STORAGE_BLOCK = 54,
        COOKING_OVEN = 55, AUDIO_RACK = 56, GEIGER_CHARGER = 57, ADVENTURE_BEGINS = 58, TOMB_ROBBER = 59,
        BALLOON_O_MATIC = 60, TRAINING_PORT = 61, ITEM_SUCKER = 62, CYBOT = 63, GUILD_ITEM = 65, GROWSCAN = 66,
        CONTAINMENT_FIELD_POWER_NODE = 67, SPIRIT_BOARD = 68, TESSERACT_MANIPULATOR = 69, STORMY_CLOUD = 72,
        TEMPORARY_PLATFORM = 73, SAFE_VAULT = 74, ANGELIC_COUNTING_CLOUD = 75, INFINITY_WEATHER_MACHINE = 77,
        PINEAPPLE_GUZZLER = 79, KRAKEN_GALACTIC_BLOCK = 80, FRIENDS_ENTRANCE = 81,
    };

    struct Door { std::string label; uint8_t flags = 0; };
    struct Sign { std::string label; };
    struct Lock {
        uint8_t settings = 0; /* LockFlags */
        uint32_t ownerUID = 0;
        std::vector<uint32_t> accessUIDs;
        int32_t bpm = 100;
        uint32_t minimumLevel = 0;
        uint32_t worldTimer = 0;
    };
    struct Seed {
        uint32_t age = 0;     /* seconds since planted, as of `seen` */
        uint8_t itemOnTree = 0;
        std::chrono::steady_clock::time_point seen{};

        uint32_t AgeNow() const {
            const auto passed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - seen).count();
            return age + static_cast<uint32_t>(passed);
        }
    };
    struct Dice { uint8_t symbol = 0; };
    struct Provider { uint32_t age = 0; };
    struct AchievementBlock { uint32_t data = 0; uint8_t tileType = 0; };
    struct HeartMonitor { uint32_t playerID = 0; std::string playerName; };
    struct Mannequin {
        std::string label;
        uint16_t hat = 0, shirt = 0, pants = 0, boots = 0, face = 0, hand = 0, back = 0, hair = 0, neck = 0;
    };
    struct BunnyEgg { uint32_t eggPlaced = 0; };
    struct GameGrave { uint8_t team = 0; };
    struct PhoneBooth { uint16_t hat = 0, shirt = 0, pants = 0, shoes = 0, face = 0, hand = 0, back = 0, hair = 0, neck = 0; };
    struct Crystal { std::vector<uint8_t> crystals; };
    struct CrimeInProgress { std::string label; };
    struct DisplayBlock { uint32_t itemID = 0; };
    struct VendingMachine { uint32_t itemID = 0; int32_t price = 0; }; /* price < 0: that many items per WL */
    struct FishTankPort { uint8_t flags = 0; std::vector<std::pair<uint32_t, uint32_t>> fishes; /* id, lbs */ };
    struct SolarCollector { std::array<uint8_t, 5> data{}; };
    struct Forge { uint32_t temperature = 0; };
    struct GivingTree { uint8_t harvested = 0; uint16_t age = 0; uint8_t decorationPercentage = 0; };
    struct SteamOrgan { uint8_t instrumentType = 0; uint32_t note = 0; };
    struct SilkWorm {
        uint8_t flags = 0;
        std::string name;
        uint32_t age = 0;
        uint8_t canBeFed = 0;
        uint32_t foodSaturation = 0, waterSaturation = 0, color = 0, sickDuration = 0;
    };
    struct SewingMachine { std::vector<uint32_t> boltIDs; };
    struct CountryFlag { std::string country; };
    struct PaintingEasel { uint32_t itemID = 0; std::string label; };
    struct PetBattleCage { std::string label; uint32_t basePet = 0, pet1 = 0, pet2 = 0; };
    struct PetTrainer { std::string label; std::vector<uint32_t> petIDs; };
    struct SteamEngine { uint32_t temperature = 0; };
    struct LockBot { uint32_t age = 0; };
    struct WeatherMachine { uint32_t settings = 0; };
    struct SpiritStorageUnit { uint32_t ghostJarCount = 0; };
    struct Shelf { uint32_t topLeft = 0, topRight = 0, bottomLeft = 0, bottomRight = 0; };
    struct VipEntrance { uint32_t ownerUID = 0; std::vector<uint32_t> accessUIDs; };
    struct FishWallMount { std::string label; uint32_t itemID = 0; uint8_t lb = 0; };
    struct Portrait { std::string label; uint16_t face = 0, hat = 0, hair = 0; std::string infinityCrownData; };
    struct WeatherMachine2 { uint32_t gravity = 0; uint8_t flags = 0; };
    struct ChemsynthTank { uint32_t currentChem = 0, targetChem = 0; };
    struct StorageBlock { std::vector<std::pair<uint32_t, uint32_t>> items; /* id, amount */ };
    struct CookingOven { uint32_t temperatureLevel = 0; std::vector<std::pair<uint32_t, uint32_t>> ingredients; /* id, time added */ };
    struct AudioRack { std::string note; uint32_t volume = 0; };
    struct BalloonOMatic { uint32_t totalRarity = 0; uint8_t teamType = 0; };
    struct TrainingPort { uint32_t fishLb = 0; uint16_t fishStatus = 0; uint32_t fishID = 0, fishTotalExp = 0, fishLevel = 0; };
    struct ItemSucker { uint32_t itemID = 0, amount = 0; uint16_t flags = 0; uint32_t limit = 0; };
    struct CyBot { std::vector<std::pair<uint32_t, uint32_t>> commands; /* id, used */ uint32_t syncTimer = 0, activated = 0; };
    struct ContainmentFieldPowerNode { uint32_t time = 0; std::vector<uint32_t> linkedNodes; };
    struct SpiritBoard { uint32_t playerRequired = 0; std::string command; std::vector<uint32_t> requiredItems; };
    struct TesseractManipulator { uint32_t gems = 0, nextUpdateMs = 0, itemID = 0, enabled = 0; };
    struct StormyCloud { uint32_t stingDuration = 0, isSolid = 0, nonSolidDuration = 0; };
    struct AngelicCountingCloud { uint32_t state = 0; uint8_t asciiCode = 0; };
    struct InfinityWeatherMachine { uint32_t intervalMinutes = 0; std::vector<uint32_t> weatherMachines; };
    struct PineappleGuzzler { uint32_t pineappleCount = 0; };
    struct KrakenGalacticBlock { uint8_t patternIndex = 0, r = 0, g = 0, b = 0; };
    struct FriendsEntrance { uint32_t ownerUID = 0; std::vector<uint32_t> allowedUIDs; };

    using Data = std::variant<std::monostate,
        Door, Sign, Lock, Seed, Dice, Provider, AchievementBlock, HeartMonitor, Mannequin, BunnyEgg, GameGrave,
        PhoneBooth, Crystal, CrimeInProgress, DisplayBlock, VendingMachine, FishTankPort, SolarCollector, Forge,
        GivingTree, SteamOrgan, SilkWorm, SewingMachine, CountryFlag, PaintingEasel, PetBattleCage, PetTrainer,
        SteamEngine, LockBot, WeatherMachine, SpiritStorageUnit, Shelf, VipEntrance, FishWallMount, Portrait,
        WeatherMachine2, ChemsynthTank, StorageBlock, CookingOven, AudioRack, BalloonOMatic, TrainingPort,
        ItemSucker, CyBot, ContainmentFieldPowerNode, SpiritBoard, TesseractManipulator, StormyCloud,
        AngelicCountingCloud, InfinityWeatherMachine, PineappleGuzzler, KrakenGalacticBlock, FriendsEntrance>;
}

struct WorldTile {
    uint16_t fg = 0;
    uint16_t bg = 0;
    uint16_t parent = 0;      /* tile index of the area lock owning this tile (HAS_PARENT) */
    uint16_t flags = 0;       /* TileFlags */
    uint16_t parentExtra = 0; /* the extra u16 after flags when HAS_PARENT (meaning unknown) */
    uint8_t extraKind = TileExtra::NONE;
    /* Only the few tiles with extra data pay for it: a tile stays 24 bytes. */
    std::unique_ptr<TileExtra::Data> extra;

    bool Has(uint16_t flag) const noexcept { return (flags & flag) != 0; }
    template <typename T> const T* As() const noexcept { return extra ? std::get_if<T>(extra.get()) : nullptr; }
    void ClearForeground() noexcept {
        fg = 0;
        extraKind = TileExtra::NONE;
        extra.reset();
    }
};

struct WorldObject {
    uint16_t id = 0;
    float x = 0.0f, y = 0.0f; /* pixels */
    uint8_t count = 0;
    uint8_t flags = 0;
    uint32_t uid = 0;
};

class WorldReader;

/* @important: written only by its bot's thread. Other threads read it under
   Bot's world lock (Bot::ReadWorld). */
class WorldState {
public:
    uint16_t version = 0;
    uint32_t flags = 0;
    std::string name;
    uint32_t width = 0, height = 0;
    std::vector<WorldTile> tiles;
    std::vector<WorldObject> objects;
    uint32_t nextObjectUID = 0;
    uint16_t baseWeather = 0, currentWeather = 0;
    /* Log tag (the bot's GrowID). */
    std::string tag = "WORLD";

    /* False until a map has been read completely. A map that failed half-way
       keeps the name, the size and the tiles before the failure. */
    bool loaded = false;

    /* Extended data of PACKET_SEND_MAP_DATA. */
    bool Parse(std::span<const uint8_t> data);
    /* Extended data of PACKET_SEND_TILE_UPDATE_DATA (one tile, no coordinates). */
    bool UpdateTile(uint32_t x, uint32_t y, std::span<const uint8_t> data);
    /* Extended data of PACKET_SEND_TILE_UPDATE_DATA_MULTIPLE. */
    void UpdateTiles(std::span<const uint8_t> data);
    void Clear();

    WorldTile* Tile(uint32_t x, uint32_t y) noexcept;
    const WorldTile* Tile(uint32_t x, uint32_t y) const noexcept;
    WorldTile* Tile(int x, int y) noexcept { return x < 0 || y < 0 ? nullptr : Tile(static_cast<uint32_t>(x), static_cast<uint32_t>(y)); }
    const WorldTile* Tile(int x, int y) const noexcept { return x < 0 || y < 0 ? nullptr : Tile(static_cast<uint32_t>(x), static_cast<uint32_t>(y)); }
    /* The world-wide lock (World Lock, Diamond Lock, ...), not an area lock. */
    const WorldTile* WorldLock() const noexcept;
    TilePermission Permission(uint32_t x, uint32_t y, uint32_t userID) const;

    /* After a tile changed by other means than UpdateTile (punch echo, lock, ...). */
    void TileChanged(uint32_t x, uint32_t y);

private:
    size_t worldLockIndex_ = SIZE_MAX;
    /* (kind, fg) -> extra bytes after the kind byte, found by resyncing.
       Kept for the bot's whole run so the same tile reads directly next time. */
    std::map<std::pair<uint8_t, uint16_t>, size_t> learnedExtra_;

    bool ParseTile(WorldReader& r, WorldTile& tile, bool quiet);
    bool Resync(const WorldReader& r, size_t at, uint8_t kind, uint16_t fg, uint32_t index, uint32_t count);
};
