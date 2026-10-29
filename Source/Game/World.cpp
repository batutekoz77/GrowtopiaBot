#include "World.hpp"
#include "../Core/Log.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <utility>

/* Bounds-checked cursor, same contract as the items.dat one: a read past the
   end flips `ok` and yields zeros. Loops over wire counts must check `ok`,
   since a garbage count would otherwise spin on zero reads. */
class WorldReader {
public:
    explicit WorldReader(std::span<const uint8_t> data) : data_(data) {}

    size_t pos = 0;
    bool ok = true;

    template <typename T>
    T Read() {
        T value{};
        if (!Need(sizeof(T))) return value;
        std::memcpy(&value, data_.data() + pos, sizeof(T));
        pos += sizeof(T);
        return value;
    }

    uint8_t U8() { return Read<uint8_t>(); }
    uint16_t U16() { return Read<uint16_t>(); }
    uint32_t U32() { return Read<uint32_t>(); }
    int32_t I32() { return Read<int32_t>(); }
    float F32() { return Read<float>(); }

    void Skip(size_t count) {
        if (Need(count)) pos += count;
    }

    /* u16 length, then bytes. */
    std::string String() {
        const uint16_t length = U16();
        if (!Need(length)) return {};
        std::string value(reinterpret_cast<const char*>(data_.data() + pos), length);
        pos += length;
        return value;
    }

    std::vector<uint32_t> U32List(uint32_t count) {
        std::vector<uint32_t> values;
        for (uint32_t i = 0; i < count && ok; ++i) values.push_back(U32());
        return values;
    }

    size_t Remaining() const { return data_.size() - std::min(pos, data_.size()); }

private:
    bool Need(size_t count) {
        if (ok && pos <= data_.size() && data_.size() - pos >= count) return true;
        ok = false;
        return false;
    }

    std::span<const uint8_t> data_;
};

namespace {
    constexpr uint16_t MAP_VERSION_MIN = 0x19;
    constexpr uint32_t MAX_TILE_COUNT = 65026;
    constexpr uint32_t MAX_WORLD_OBJECTS = 0x493E1;

    constexpr uint16_t GUILD_LOCK = 5814;

    /* Resync after an unknown extra kind: how far to look for the next tile,
       how many following tiles must read cleanly, and the highest item id
       a real tile can hold. */
    constexpr size_t RESYNC_MAX_SKIP = 512;
    constexpr uint32_t RESYNC_CHECK_TILES = 16;
    constexpr uint16_t MAX_ITEM_ID = 20000;

    constexpr uint16_t BUILDERS_LOCK = 4994;
    /* Small / Big / Huge / Builder's Lock: area locks, never the world lock. */
    constexpr uint16_t AREA_LOCKS[] = { 202, 204, 206, BUILDERS_LOCK };

    /* Tiles that carry a raw CBOR blob after their extra data. */
    constexpr uint16_t CBOR_TILES[] = {
        15376, 15546, 3548, 14662, 14666, 8624, 8630, 8636, 8642, 8648, 8654, 8660, 8666, 8672,
        8678, 8684, 8690, 8696, 8702, 8708, 8714,
    };

    template <size_t N>
    bool Contains(const uint16_t (&list)[N], uint16_t id) {
        return std::find(std::begin(list), std::end(list), id) != std::end(list);
    }

    bool IsWorldLock(const WorldTile& tile) {
        return tile.extraKind == TileExtra::LOCK && !Contains(AREA_LOCKS, tile.fg);
    }

    /* Fills tile.extra for one kind byte. False on a kind whose layout is not
       known: its size is unknown too, so nothing after it can be trusted. */
    bool ParseExtra(WorldReader& r, uint8_t kind, uint16_t fg, WorldTile& tile) {
        using namespace TileExtra;
        Data out;

        switch (kind) {
            case DOOR: {
                Door door;
                door.label = r.String();
                door.flags = r.U8();
                out = std::move(door);
                break;
            }
            case SIGN: {
                Sign sign;
                sign.label = r.String();
                r.Skip(4); /* always 0xFFFFFFFF */
                out = std::move(sign);
                break;
            }
            case LOCK: {
                Lock lock;
                lock.settings = r.U8();
                lock.ownerUID = r.U32();
                const uint32_t count = r.U32();
                /* The access list doubles as storage for the music BPM, which
                   is the one negative entry. */
                for (uint32_t i = 0; i < count && r.ok; ++i) {
                    const int32_t id = r.I32();
                    if (id < 0) lock.bpm = id;
                    else lock.accessUIDs.push_back(static_cast<uint32_t>(id));
                }
                lock.minimumLevel = r.U32();
                lock.worldTimer = r.U32();
                if (fg == GUILD_LOCK) r.Skip(16);
                out = std::move(lock);
                break;
            }
            case SEED: {
                Seed seed;
                seed.age = r.U32();
                seed.itemOnTree = r.U8();
                seed.seen = std::chrono::steady_clock::now();
                out = seed;
                break;
            }
            case DICE: out = Dice{ r.U8() }; break;
            case PROVIDER: {
                Provider provider{ r.U32() };
                if (fg == 10656) r.Skip(4);
                out = provider;
                break;
            }
            case ACHIEVEMENT_BLOCK: {
                AchievementBlock block;
                block.data = r.U32();
                block.tileType = r.U8();
                out = block;
                break;
            }
            case HEART_MONITOR: {
                HeartMonitor monitor;
                monitor.playerID = r.U32();
                monitor.playerName = r.String();
                out = std::move(monitor);
                break;
            }
            case MANNEQUIN: {
                Mannequin m;
                m.label = r.String();
                r.Skip(5); /* u8, u16, u16: unknown */
                m.hat = r.U16();
                m.shirt = r.U16();
                m.pants = r.U16();
                m.boots = r.U16();
                m.face = r.U16();
                m.hand = r.U16();
                m.back = r.U16();
                m.hair = r.U16();
                m.neck = r.U16();
                out = std::move(m);
                break;
            }
            case BUNNY_EGG: out = BunnyEgg{ r.U32() }; break;
            case GAME_GRAVE: out = GameGrave{ r.U8() }; break;
            case GAME_GENERATOR: break;
            case XENONITE_CRYSTAL: r.Skip(5); break; /* u8, u32: unknown */
            case PHONE_BOOTH: {
                PhoneBooth booth;
                booth.hat = r.U16();
                booth.shirt = r.U16();
                booth.pants = r.U16();
                booth.shoes = r.U16();
                booth.face = r.U16();
                booth.hand = r.U16();
                booth.back = r.U16();
                booth.hair = r.U16();
                booth.neck = r.U16();
                out = booth;
                break;
            }
            case CRYSTAL: {
                Crystal crystal;
                const uint16_t count = r.U16();
                for (uint16_t i = 0; i < count && r.ok; ++i) crystal.crystals.push_back(r.U8());
                out = std::move(crystal);
                break;
            }
            case CRIME_IN_PROGRESS: {
                CrimeInProgress crime{ r.String() };
                r.Skip(5); /* u32, u8: unknown */
                out = std::move(crime);
                break;
            }
            case SPOTLIGHT: break;
            case DISPLAY_BLOCK: out = DisplayBlock{ r.U32() }; break;
            case VENDING_MACHINE: {
                VendingMachine vend;
                vend.itemID = r.U32();
                vend.price = r.I32();
                out = vend;
                break;
            }
            case FISH_TANK_PORT: {
                FishTankPort tank;
                tank.flags = r.U8();
                const uint32_t count = r.U32(); /* counts u32 values, two per fish */
                for (uint32_t i = 0; i < count / 2 && r.ok; ++i) {
                    const uint32_t id = r.U32();
                    tank.fishes.emplace_back(id, r.U32());
                }
                out = std::move(tank);
                break;
            }
            case SOLAR_COLLECTOR: {
                SolarCollector solar;
                for (auto& b : solar.data) b = r.U8();
                out = solar;
                break;
            }
            case FORGE: out = Forge{ r.U32() }; break;
            case GIVING_TREE: {
                GivingTree tree;
                tree.harvested = r.U8();
                tree.age = r.U16();
                r.Skip(2); /* unknown */
                tree.decorationPercentage = r.U8();
                out = tree;
                break;
            }
            case STEAM_ORGAN: {
                SteamOrgan organ;
                organ.instrumentType = r.U8();
                organ.note = r.U32();
                out = organ;
                break;
            }
            case SILK_WORM: {
                SilkWorm worm;
                worm.flags = r.U8();
                worm.name = r.String();
                worm.age = r.U32();
                r.Skip(8); /* u32, u32: unknown */
                worm.canBeFed = r.U8();
                worm.foodSaturation = r.U32();
                worm.waterSaturation = r.U32();
                worm.color = r.U32();
                worm.sickDuration = r.U32();
                out = std::move(worm);
                break;
            }
            case SEWING_MACHINE: out = SewingMachine{ r.U32List(r.U32()) }; break;
            case COUNTRY_FLAG: out = CountryFlag{ fg == 3394 ? r.String() : std::string{} }; break;
            case LOBSTER_TRAP: break;
            case PAINTING_EASEL: {
                PaintingEasel easel;
                easel.itemID = r.U32();
                easel.label = r.String();
                out = std::move(easel);
                break;
            }
            case PET_BATTLE_CAGE: {
                PetBattleCage cage;
                cage.label = r.String();
                cage.basePet = r.U32();
                cage.pet1 = r.U32();
                cage.pet2 = r.U32();
                out = std::move(cage);
                break;
            }
            case PET_TRAINER: {
                PetTrainer trainer;
                trainer.label = r.String();
                const uint32_t count = r.U32();
                r.Skip(4); /* unknown */
                trainer.petIDs = r.U32List(count);
                out = std::move(trainer);
                break;
            }
            case STEAM_ENGINE: out = SteamEngine{ r.U32() }; break;
            case LOCK_BOT: out = LockBot{ r.U32() }; break;
            case WEATHER_MACHINE: out = WeatherMachine{ r.U32() }; break;
            case SPIRIT_STORAGE_UNIT: out = SpiritStorageUnit{ r.U32() }; break;
            case DATA_BEDROCK: r.Skip(21); break;
            case SHELF: {
                Shelf shelf;
                shelf.topLeft = r.U32();
                shelf.topRight = r.U32();
                shelf.bottomLeft = r.U32();
                shelf.bottomRight = r.U32();
                out = shelf;
                break;
            }
            case VIP_ENTRANCE: {
                VipEntrance vip;
                r.Skip(1); /* unknown */
                vip.ownerUID = r.U32();
                vip.accessUIDs = r.U32List(r.U32());
                out = std::move(vip);
                break;
            }
            case CHALLENGE_TIMER: break;
            case FISH_WALL_MOUNT: {
                FishWallMount mount;
                mount.label = r.String();
                mount.itemID = r.U32();
                mount.lb = r.U8();
                out = std::move(mount);
                break;
            }
            case PORTRAIT: {
                Portrait portrait;
                portrait.label = r.String();
                r.Skip(16); /* u32, u32, u8[5], u8, u16: unknown */
                portrait.face = r.U16();
                portrait.hat = r.U16();
                portrait.hair = r.U16();
                r.Skip(4); /* unknown */
                if (portrait.hat == 12958) portrait.infinityCrownData = r.String();
                out = std::move(portrait);
                break;
            }
            case WEATHER_MACHINE_2: {
                WeatherMachine2 weather;
                r.Skip(4); /* unknown */
                weather.gravity = r.U32();
                weather.flags = r.U8();
                out = weather;
                break;
            }
            case FOSSIL_PREP_STATION: r.Skip(4); break;
            case DNA_EXTRACTOR: break;
            case HOWLER: break;
            case CHEMSYNTH_TANK: {
                ChemsynthTank tank;
                tank.currentChem = r.U32();
                tank.targetChem = r.U32();
                out = tank;
                break;
            }
            case STORAGE_BLOCK: {
                StorageBlock storage;
                const uint16_t size = r.U16(); /* bytes, 13 per item */
                for (uint16_t i = 0; i < size / 13 && r.ok; ++i) {
                    r.Skip(3);
                    const uint32_t id = r.U32();
                    r.Skip(2);
                    storage.items.emplace_back(id, r.U32());
                }
                out = std::move(storage);
                break;
            }
            case COOKING_OVEN: {
                CookingOven oven;
                oven.temperatureLevel = r.U32();
                const uint32_t count = r.U32(); /* u32 values, two per ingredient */
                for (uint32_t i = 0; i < count / 2 && r.ok; ++i) {
                    const uint32_t id = r.U32();
                    oven.ingredients.emplace_back(id, r.U32());
                }
                r.Skip(12); /* u32 x3: unknown */
                out = std::move(oven);
                break;
            }
            case AUDIO_RACK: {
                AudioRack rack;
                rack.note = r.String();
                rack.volume = r.U32();
                out = std::move(rack);
                break;
            }
            case GEIGER_CHARGER: r.Skip(4); break;
            case ADVENTURE_BEGINS: break;
            case TOMB_ROBBER: break;
            case BALLOON_O_MATIC: {
                BalloonOMatic balloon;
                balloon.totalRarity = r.U32();
                balloon.teamType = r.U8();
                out = balloon;
                break;
            }
            case TRAINING_PORT: {
                TrainingPort port;
                port.fishLb = r.U32();
                port.fishStatus = r.U16();
                port.fishID = r.U32();
                port.fishTotalExp = r.U32();
                r.Skip(8); /* unknown */
                port.fishLevel = r.U32();
                r.Skip(9); /* u32, u8[5]: unknown */
                out = port;
                break;
            }
            case ITEM_SUCKER: {
                ItemSucker sucker;
                sucker.itemID = r.U32();
                sucker.amount = r.U32();
                sucker.flags = r.U16();
                sucker.limit = r.U32();
                out = sucker;
                break;
            }
            case CYBOT: {
                CyBot bot;
                const uint32_t count = r.U32();
                for (uint32_t i = 0; i < count && r.ok; ++i) {
                    const uint32_t id = r.U32();
                    const uint32_t used = r.U32();
                    r.Skip(7);
                    bot.commands.emplace_back(id, used);
                }
                bot.syncTimer = r.U32();
                bot.activated = r.U32();
                out = std::move(bot);
                break;
            }
            case GUILD_ITEM: r.Skip(17); break;
            case GROWSCAN: r.Skip(1); break;
            case CONTAINMENT_FIELD_POWER_NODE: {
                ContainmentFieldPowerNode node;
                node.time = r.U32();
                node.linkedNodes = r.U32List(r.U32());
                out = std::move(node);
                break;
            }
            case SPIRIT_BOARD: {
                SpiritBoard board;
                board.playerRequired = r.U32();
                r.String(); /* unknown */
                board.command = r.String();
                board.requiredItems = r.U32List(r.U32());
                out = std::move(board);
                break;
            }
            case TESSERACT_MANIPULATOR: {
                TesseractManipulator tesseract;
                tesseract.gems = r.U32();
                tesseract.nextUpdateMs = r.U32();
                tesseract.itemID = r.U32();
                tesseract.enabled = r.U32();
                out = tesseract;
                break;
            }
            case STORMY_CLOUD: {
                StormyCloud cloud;
                cloud.stingDuration = r.U32();
                cloud.isSolid = r.U32();
                cloud.nonSolidDuration = r.U32();
                out = cloud;
                break;
            }
            case TEMPORARY_PLATFORM: r.Skip(4); break;
            case SAFE_VAULT: break;
            case ANGELIC_COUNTING_CLOUD: {
                AngelicCountingCloud cloud;
                cloud.state = r.U32();
                r.Skip(2); /* unknown */
                if (cloud.state == 2) cloud.asciiCode = r.U8();
                out = cloud;
                break;
            }
            case INFINITY_WEATHER_MACHINE: {
                InfinityWeatherMachine weather;
                weather.intervalMinutes = r.U32();
                weather.weatherMachines = r.U32List(r.U32());
                out = std::move(weather);
                break;
            }
            case PINEAPPLE_GUZZLER: out = PineappleGuzzler{ r.U32() }; break;
            case KRAKEN_GALACTIC_BLOCK: {
                KrakenGalacticBlock block;
                block.patternIndex = r.U8();
                r.Skip(4); /* unknown */
                block.r = r.U8();
                block.g = r.U8();
                block.b = r.U8();
                out = block;
                break;
            }
            case FRIENDS_ENTRANCE: {
                FriendsEntrance entrance;
                entrance.ownerUID = r.U32();
                r.Skip(2); /* unknown */
                entrance.allowedUIDs = r.U32List(r.U16());
                out = std::move(entrance);
                break;
            }
            default:
                return false;
        }
        if (!std::holds_alternative<std::monostate>(out)) tile.extra = std::make_unique<Data>(std::move(out));
        return r.ok;
    }

    std::string Hex(std::span<const uint8_t> data, size_t from, size_t count) {
        std::string hex;
        for (size_t i = from; i < data.size() && i < from + count; ++i) hex += std::format("{:02x} ", data[i]);
        return hex;
    }
}

/* fg u16, bg u16, parent u16, flags u16, [u16], [kind u8 + extra], [CBOR]. */
bool WorldState::ParseTile(WorldReader& r, WorldTile& tile, bool quiet) {
    tile = {};
    tile.fg = r.U16();
    tile.bg = r.U16();
    tile.parent = r.U16();
    tile.flags = r.U16();

    if (tile.Has(TileFlags::HAS_PARENT)) tile.parentExtra = r.U16();
    if (tile.Has(TileFlags::HAS_EXTRA_DATA)) {
        tile.extraKind = r.U8();
        if (!r.ok) return false;
        if (!ParseExtra(r, tile.extraKind, tile.fg, tile)) {
            if (!r.ok) return false;
            const auto learned = learnedExtra_.find({ tile.extraKind, tile.fg });
            if (learned == learnedExtra_.end()) {
                if (!quiet) Log::Debug(tag, "unknown tile extra kind {} on item {}", tile.extraKind, tile.fg);
                return false;
            }
            /* Layout unknown, only its size: the tile keeps fg/bg, no extra. */
            tile.extraKind = TileExtra::NONE;
            tile.extra.reset();
            r.Skip(learned->second);
        }
    }
    if (Contains(CBOR_TILES, tile.fg)) r.Skip(r.U32());
    return r.ok;
}

/* Tile `index` (starting at `at`) has an extra kind we can't read. Find the
   extra size that makes it and the next tiles read cleanly, and remember it. */
bool WorldState::Resync(const WorldReader& r, size_t at, uint8_t kind, uint16_t fg, uint32_t index, uint32_t count) {
    const auto key = std::make_pair(kind, fg);
    const uint32_t check = std::min(RESYNC_CHECK_TILES, count - index - 1);
    for (size_t skip = 0; skip <= RESYNC_MAX_SKIP; ++skip) {
        learnedExtra_[key] = skip;
        WorldReader probe = r;
        probe.pos = at;
        WorldTile tile;
        bool good = ParseTile(probe, tile, true);
        for (uint32_t i = 0; good && i < check; ++i) {
            good = ParseTile(probe, tile, true) && tile.fg < MAX_ITEM_ID && tile.bg < MAX_ITEM_ID;
        }
        if (good) {
            Log::Warn(tag, "world {}: extra kind {} on item {} has {} byte(s) we don't know, the map continues", name, kind, fg, skip);
            return true;
        }
    }
    learnedExtra_.erase(key);
    return false;
}

bool WorldState::Parse(std::span<const uint8_t> data) {
    Clear();
    WorldReader r(data);

    /* The header layout predates 0x19, so name and size are kept even when
       the tiles are in a layout we don't know. */
    version = r.U16();
    flags = r.U32();
    name = r.String();
    width = r.U32();
    height = r.U32();
    const uint32_t count = r.U32();
    r.Skip(5);

    if (r.ok && version < MAP_VERSION_MIN) {
        Log::Error(tag, "map version {:#x} is older than {:#x}, tiles not parsed", version, MAP_VERSION_MIN);
        return false;
    }
    if (!r.ok || count >= MAX_TILE_COUNT || width == 0) {
        Log::Error(tag, "bad map header ({} x {}, {} tiles)", width, height, count);
        return false;
    }

    tiles.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = r.pos;
        if (!ParseTile(r, tiles[i], false)) {
            /* An extra kind we don't know: find where the next tile starts
               instead of losing the rest of the map. */
            if (r.ok && tiles[i].Has(TileFlags::HAS_EXTRA_DATA)) {
                Log::Debug(tag, "{}: tile {} ({}, {}) bytes: {}", name, i, i % width, i / width, Hex(data, at, 64));
                const uint8_t kind = tiles[i].extraKind;
                const uint16_t fg = tiles[i].fg;
                if (Resync(r, at, kind, fg, i, count)) {
                    r.pos = at;
                    if (ParseTile(r, tiles[i], false)) {
                        if (IsWorldLock(tiles[i])) worldLockIndex_ = i;
                        continue;
                    }
                }
            }
            Log::Error(tag, "{}: tile {} ({}, {}) failed at byte {}, the rest of the map is unknown", name, i, i % width, i / width, at);
            tiles.resize(i);
            return false;
        }
        if (IsWorldLock(tiles[i])) worldLockIndex_ = i;
    }
    r.Skip(12);

    const uint32_t objectCount = r.U32();
    const uint32_t lastUID = r.U32();
    if (!r.ok || objectCount >= MAX_WORLD_OBJECTS) {
        Log::Error(tag, "{}: bad dropped object count {}", name, objectCount);
        return false;
    }
    objects.reserve(objectCount);
    for (uint32_t i = 0; i < objectCount && r.ok; ++i) {
        WorldObject object;
        object.id = r.U16();
        object.x = r.F32();
        object.y = r.F32();
        object.count = r.U8();
        object.flags = r.U8();
        object.uid = r.U32();
        if (object.id == 5996 || object.id == 1626) object.id = 0;
        if (r.ok) objects.push_back(object);
    }
    nextObjectUID = lastUID + 1;

    baseWeather = r.U16();
    r.Skip(2);
    currentWeather = r.U16();

    if (!r.ok) {
        Log::Error(tag, "{}: map data ended early (object list / weather)", name);
        return false;
    }
    loaded = true;
    return true;
}

bool WorldState::UpdateTile(uint32_t x, uint32_t y, std::span<const uint8_t> data) {
    WorldReader r(data);
    WorldTile* target = Tile(x, y);
    if (!target) return false;

    WorldTile tile;
    if (!ParseTile(r, tile, false)) return false;

    *target = std::move(tile);
    TileChanged(x, y);
    return true;
}

void WorldState::UpdateTiles(std::span<const uint8_t> data) {
    /* x u32, y u32, tile... repeated; x == 0xFFFFFFFF ends the list. */
    WorldReader r(data);
    while (r.Remaining() >= 16) {
        const uint32_t x = r.U32();
        if (x == UINT32_MAX) break;
        const uint32_t y = r.U32();

        WorldTile* target = Tile(x, y);
        WorldTile tile;
        if (!target || !ParseTile(r, tile, false)) {
            Log::Debug(tag, "multiple tile update failed at ({}, {})", x, y);
            return;
        }
        *target = std::move(tile);
        TileChanged(x, y);
    }
}

void WorldState::TileChanged(uint32_t x, uint32_t y) {
    const WorldTile* tile = Tile(x, y);
    if (!tile) return;
    const size_t index = static_cast<size_t>(y) * width + x;
    if (IsWorldLock(*tile)) worldLockIndex_ = index;
    else if (worldLockIndex_ == index) worldLockIndex_ = SIZE_MAX;
}

void WorldState::Clear() {
    version = 0;
    flags = 0;
    name.clear();
    width = height = 0;
    tiles.clear();
    objects.clear();
    nextObjectUID = 0;
    baseWeather = currentWeather = 0;
    loaded = false;
    worldLockIndex_ = SIZE_MAX;
}

WorldTile* WorldState::Tile(uint32_t x, uint32_t y) noexcept {
    if (x >= width || y >= height) return nullptr;
    const size_t index = static_cast<size_t>(y) * width + x;
    return index < tiles.size() ? &tiles[index] : nullptr;
}

const WorldTile* WorldState::Tile(uint32_t x, uint32_t y) const noexcept {
    return const_cast<WorldState*>(this)->Tile(x, y);
}

const WorldTile* WorldState::WorldLock() const noexcept {
    return worldLockIndex_ < tiles.size() ? &tiles[worldLockIndex_] : nullptr;
}

TilePermission WorldState::Permission(uint32_t x, uint32_t y, uint32_t userID) const {
    using TileExtra::Lock;
    const auto has = [](const std::vector<uint32_t>& list, uint32_t id) {
        return std::find(list.begin(), list.end(), id) != list.end();
    };

    const WorldTile* tile = Tile(x, y);
    if (!tile) return TilePermission::None;

    /* A lock itself belongs only to its owner. */
    if (const Lock* lock = tile->As<Lock>()) {
        return lock->ownerUID == userID ? TilePermission::FullAccess : TilePermission::None;
    }

    const WorldTile* parent = tile->Has(TileFlags::HAS_PARENT) && tile->parent < tiles.size() ? &tiles[tile->parent] : nullptr;

    if (const WorldTile* worldLock = WorldLock()) {
        const Lock* lock = worldLock->As<Lock>();
        if (!lock) return TilePermission::None;

        if (lock->ownerUID == userID) return TilePermission::FullAccess;
        /* An area lock inside the world overrides the world lock's access list. */
        if (!parent && (has(lock->accessUIDs, userID) || worldLock->Has(TileFlags::IS_OPEN_TO_PUBLIC))) {
            return TilePermission::FullAccess;
        }
    }

    if (parent) {
        const Lock* lock = parent->As<Lock>();
        if (!lock) return TilePermission::None;
        if (lock->ownerUID == userID) return TilePermission::FullAccess;

        const bool allowed = has(lock->accessUIDs, userID) || parent->Has(TileFlags::IS_OPEN_TO_PUBLIC);
        if (!allowed) return TilePermission::None;
        if (parent->fg != BUILDERS_LOCK || !(lock->settings & LockFlags::ADMIN_LIMITED)) return TilePermission::FullAccess;
        return (lock->settings & LockFlags::BREAK_ONLY) ? TilePermission::Break : TilePermission::Build;
    }

    return TilePermission::None;
}
