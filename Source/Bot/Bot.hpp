#pragma once

#include "../Game/Pathfinder.hpp"
#include "../Game/Protocol.hpp"
#include "../Game/World.hpp"
#include "../Main/Config.hpp"
#include "Events.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

enum class BotStatus {
    Idle,
    LoggingIn,
    Connecting,
    EnteringGame,
    InGame,
    JoiningWorld,
    InWorld,
    Waiting,
    Offline,
    Stopped,
};

const char* StatusName(BotStatus status) noexcept;
const char* StatusKey(BotStatus status) noexcept;

struct InventoryItem {
    uint16_t id = 0;
    uint8_t amount = 0;
    uint8_t flags = 0;
};

struct PlayerInfo {
    int32_t netID = 0;
    int32_t userID = 0;
    std::string name;
    std::string country;
    float x = 0.0f;
    float y = 0.0f;
    bool moderator = false;
};

struct BotSnapshot {
    int index = 0;
    std::string growId;
    std::string name;
    BotStatus status = BotStatus::Idle;
    std::string statusText;
    std::string proxy;
    std::string targetWorld;
    std::string world;
    uint32_t worldWidth = 0;
    uint32_t worldHeight = 0;
    int32_t netID = 0;
    int32_t userID = 0;
    float x = 0.0f;
    float y = 0.0f;
    int32_t gems = 0;
    int level = 0;
    uint32_t pingMs = 0;
    uint32_t inventorySize = 0;
    std::map<uint16_t, InventoryItem> inventory;
    std::map<int32_t, PlayerInfo> players;

    /* Pathfinding: the walk in progress and how the last one ended. */
    bool walking = false;
    int pathTargetX = 0, pathTargetY = 0;
    size_t pathLeft = 0;
    uint64_t pathDone = 0; /* the last walk request that ended */
    bool pathOk = false;
    std::string pathError;

    bool autoCollect = false;
    int collectRadius = Pathfinder::REACH;
};

class Bot {
public:
    Bot(int id, Config::BotEntry entry, const Config::Common& settings);
    ~Bot();
    Bot(const Bot&) = delete;
    Bot& operator=(const Bot&) = delete;

    void Start(int delayMs = 0);
    void Stop();
    bool Running() const noexcept { return running_; }
    void Connect(int delayMs = 0);
    void Disconnect();
    void Reconnect();

    BotSnapshot Snapshot() const;
    template <typename F> auto Read(F&& reader) const {
        std::lock_guard lock(mutex_);
        return reader(static_cast<const BotSnapshot&>(state_));
    }
    /* The bot's current world (tiles, dropped items). Keep the reader short
       and don't call other Bot methods from inside it. */
    template <typename F> auto ReadWorld(F&& reader) const {
        std::lock_guard lock(worldMutex_);
        return reader(static_cast<const WorldState&>(world_));
    }
    int Id() const noexcept { return id_; }
    const std::string& GrowId() const noexcept { return entry_.growId; }
    const Config::BotEntry& Entry() const noexcept { return entry_; }
    BotStatus Status() const;
    std::string World() const;
    std::string TargetWorld() const;
    int32_t Gems() const;
    uint32_t Ping() const;
    std::pair<float, float> Position() const;
    std::pair<int, int> Tile() const;
    std::vector<InventoryItem> Inventory() const;
    int ItemCount(uint16_t itemID) const;
    bool HasItem(uint16_t itemID, int amount = 1) const;
    std::vector<PlayerInfo> Players() const;
    bool AutoReconnect() const noexcept { return autoReconnect_; }

    void SetTargetWorld(const std::string& world);
    void SetAutoReconnect(bool enabled);
    void Warp(const std::string& world, const std::string& door = {});
    void LeaveWorld();
    void Say(const std::string& text);
    /* Walks there with A*, a tile per packet (see Bot.cpp). Returns the walk
       request id for PathDone. A new walk replaces the running one. */
    uint64_t Walk(int tileX, int tileY);
    void StopWalk();
    /* True once walk `request` has ended; `ok` = it arrived. */
    bool PathDone(uint64_t request, bool* ok = nullptr, std::string* error = nullptr) const;
    Pathfinder::Result FindPath(int tileX, int tileY, std::vector<Pathfinder::Point>& path) const;
    void Drop(uint16_t itemID, int amount);
    void Trash(uint16_t itemID, int amount);
    void Wear(uint16_t itemID);
    /* Tile actions only reach the server inside the 5x5 square around the
       bot (Pathfinder::REACH); anything further is dropped, since the server
       bans for it. The return value is that check at call time. */
    bool InReach(int tileX, int tileY, bool placesBlock = false) const;
    bool Punch(int tileX, int tileY);
    bool Place(int tileX, int tileY, uint16_t itemID);
    bool Wrench(int tileX, int tileY);
    bool Activate(int tileX, int tileY);
    /* Picks up the dropped items within `radius` tiles (at most REACH).
       Returns how many are in range now. */
    int Collect(int radius = Pathfinder::REACH);
    void SetAutoCollect(bool enabled, int radius = Pathfinder::REACH);
    void Respawn();
    void DialogReturn(const std::string& dialog, const std::vector<std::pair<std::string, std::string>>& fields);
    void SendRawText(const std::string& text);
    void SendRawGameMessage(const std::string& text);
    void SendRawTank(const Protocol::TankPacket& tank, bool reliable = true);

    struct Stats { uint64_t wakeups = 0; uint64_t packets = 0; uint64_t commands = 0; };
    Stats Counters() const noexcept { return { wakeups_.load(), packets_.load(), commands_.load() }; }
    static void SetLegacyWait(bool legacy) noexcept;

private:
    struct Redirect {
        std::string server;
        uint16_t port = 0;
        std::string token;
        std::string user;
        std::string doorID;
        std::string uuid;
        std::string lmode;
        std::string tankIDName;
        int attempts = 0;
    };

    using Clock = std::chrono::steady_clock;

    const int id_;
    const Config::BotEntry entry_;
    const Config::Common settings_;
    const Login::ClientInfo& Client() const noexcept { return settings_.client; }
    const std::string tag_;

    mutable std::mutex mutex_;
    BotSnapshot state_;
    /* @important: lock order is worldMutex_ before mutex_, never the other
       way. Only the bot thread writes world_, so it reads it without the lock. */
    mutable std::mutex worldMutex_;
    WorldState world_;
    std::atomic<uint64_t> pathRequests_{ 0 };
    std::deque<std::function<void()>> queue_;

    std::mutex lifecycle_;
    std::thread thread_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> running_{ false };
    std::atomic<bool> autoReconnect_{ true };
    std::atomic<bool> offline_{ false };

    uintptr_t wake_ = ~uintptr_t(0);
    uint16_t wakePort_ = 0;
    std::atomic<bool> wakePending_{ false };
    void Wake();

    std::atomic<uint64_t> wakeups_{ 0 };
    std::atomic<uint64_t> packets_{ 0 };
    std::atomic<uint64_t> commands_{ 0 };

    struct Impl;
    std::unique_ptr<Impl> impl_;

    void Run(int delayMs);
    void Post(std::function<void()> command);
    template <typename F> void EditWorld(F&& change) {
        std::lock_guard lock(worldMutex_);
        change(world_);
    }
    template <typename F> void Update(F&& change) {
        std::lock_guard lock(mutex_);
        change(state_);
    }
    void SetStatus(BotStatus status, std::string text = {});
    void Emit(const char* name, std::initializer_list<Events::Value> args = {});

    bool Authenticate();
    bool OpenConnection(const std::string& ip, uint16_t port);
    void CloseConnection();
    void ScheduleReconnect(int delayMs, std::string reason);
    std::string LoginData() const;
    std::string RedirectPacket(const Redirect& redirect) const;

    void Service();
    void RunCommands();
    void RunTimers();
    void Wait(int timeoutMs);

    void OnPacket(std::span<const uint8_t> data);
    void OnServerHello();
    void OnGameMessage(std::string_view text);
    void OnTrack(std::string_view text);
    void OnGamePacket(std::span<const uint8_t> data);
    void OnCallFunction(std::span<const uint8_t> data);
    void OnPingRequest(uint32_t challenge);
    void OnInventoryState(std::span<const uint8_t> extended);
    void OnMapData(std::span<const uint8_t> extended);
    void OnWorldMenu();

    void SendText(std::string_view text);
    void SendGameMessage(std::string_view text);
    void SendTank(const Protocol::TankPacket& tank, bool reliable);
    void RequestJoin(const std::string& world, const std::string& door = {});
    void SendTileAction(int tileX, int tileY, uint16_t itemID, uint32_t stateFlag);

    Pathfinder::Point CurrentTile() const;
    bool CheckReach(const char* what, int tileX, int tileY, bool placesBlock);
    void StartPath(int tileX, int tileY, uint64_t request);
    void PathStep();
    void EndPath(bool arrived, std::string reason);
    void FinishPath(uint64_t request, bool arrived, std::string reason);
    bool RouteBlocked() const;
    void WalkTo(Pathfinder::Point tile, bool facingLeft);
    std::vector<WorldObject> CollectTargets(int radius) const;
    void CollectNear(int radius, bool quiet);

    void OnTileChange(const Protocol::TankPacket& tank);
    void OnTileUpdate(const Protocol::TankPacket& tank, std::span<const uint8_t> extended);
    void OnItemChangeObject(const Protocol::TankPacket& tank);
};
