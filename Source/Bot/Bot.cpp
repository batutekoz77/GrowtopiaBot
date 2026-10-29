#include "Bot.hpp"
#include "../Core/Crypto.hpp"
#include "../Core/Log.hpp"
#include "../Core/Text.hpp"
#include "../Game/Items.hpp"
#include "../Game/Login.hpp"
#include "../Net/Socks5Udp.hpp"

#include "ENET/enet.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <random>
#include <unordered_map>

using namespace Protocol;
using namespace std::chrono_literals;

namespace {
    constexpr int TWO_FA_MS = 120'000;
    constexpr int OVERLOAD_MS = 30'000;
    constexpr int TOO_MANY_LOGINS_MS = 5'000;
    constexpr int MAINTENANCE_MS = 600'000;
    constexpr int NETWORK_RETRY_MS = 5'000;
    constexpr int LOGIN_RETRY_MS = 15'000;
    constexpr auto JOIN_TIMEOUT = 15s;

    constexpr int CONNECTED_WAIT_MS = 50;
    constexpr int IDLE_WAIT_MS = 250;

    constexpr uint16_t ITEM_FIST = 18;
    constexpr uint16_t ITEM_WRENCH = 32;
    constexpr uint16_t ITEM_GEMS = 112;

    /* Walking, as AquaProxy's /path: a few tiles back to back, then a random
       pause. Mori walks one tile per 500 ms and doesn't get banned. */
    constexpr int STEP_MIN_MS = 80;
    constexpr int STEP_MAX_MS = 200;
    static_assert(STEP_MIN_MS <= STEP_MAX_MS, "STEP_MIN_MS must not be above STEP_MAX_MS");
    constexpr int STEPS_PER_SEND = 2;
    /* Route changes (door closed, pushed back, ...) before giving up. */
    constexpr int MAX_REPLANS = 25;

    constexpr size_t COLLECT_PER_CALL = 32;
    constexpr auto COLLECT_RETRY = 1500ms;
    constexpr auto AUTO_COLLECT_EVERY = 500ms;

    int RandomMs(int low, int high) {
        static thread_local std::mt19937 rng{ std::random_device{}() };
        return std::uniform_int_distribution<int>(low, high)(rng);
    }

    std::atomic<bool> gLegacyWait{ false };

    std::string StripColors(std::string_view text) {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '`') { ++i; continue; }
            out += text[i];
        }
        return out;
    }

    template <typename T>
    bool ReadAt(std::span<const uint8_t> data, size_t offset, T& value) {
        if (offset > data.size() || data.size() - offset < sizeof(T)) return false;
        std::memcpy(&value, data.data() + offset, sizeof(T));
        return true;
    }

    Events::Value ToValue(const Variant& v) {
        switch (v.type) {
            case Variant::Type::Int: case Variant::Type::UInt: case Variant::Type::Int64: return v.integer;
            case Variant::Type::Float: return static_cast<double>(v.values[0]);
            default: return v.ToString();
        }
    }
}

const char* StatusName(BotStatus status) noexcept {
    switch (status) {
        case BotStatus::Idle:         return "idle";
        case BotStatus::LoggingIn:    return "logging in";
        case BotStatus::Connecting:   return "connecting";
        case BotStatus::EnteringGame: return "entering game";
        case BotStatus::InGame:       return "in game (menu)";
        case BotStatus::JoiningWorld: return "joining world";
        case BotStatus::InWorld:      return "in world";
        case BotStatus::Waiting:      return "waiting";
        case BotStatus::Offline:      return "offline";
        case BotStatus::Stopped:      return "stopped";
    }
    return "?";
}

const char* StatusKey(BotStatus status) noexcept {
    switch (status) {
        case BotStatus::Idle:         return "idle";
        case BotStatus::LoggingIn:    return "logging_in";
        case BotStatus::Connecting:   return "connecting";
        case BotStatus::EnteringGame: return "entering_game";
        case BotStatus::InGame:       return "menu";
        case BotStatus::JoiningWorld: return "joining";
        case BotStatus::InWorld:      return "in_world";
        case BotStatus::Waiting:      return "waiting";
        case BotStatus::Offline:      return "offline";
        case BotStatus::Stopped:      return "stopped";
    }
    return "?";
}

void Bot::SetLegacyWait(bool legacy) noexcept { gLegacyWait = legacy; }

struct Bot::Impl {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    bool connected = false;

    std::optional<Net::Socks5UdpRelay> relay;
    Net::HttpClient http;

    std::string ltoken;
    std::string meta;
    std::string serverIP;
    uint16_t serverPort = 0;
    bool alternate = false;
    bool needLogin = true;
    std::optional<Redirect> redirect;
    std::string lastRedirectToken;
    std::string lastRedirectUuid;

    std::string mac = Crypto::RandomMac();
    int32_t hash = Crypto::HashString(mac + "RT");
    int32_t hash2 = Crypto::HashString(Crypto::RandomHex(16) + "RT");
    std::string wk = Crypto::RandomHex(32);
    std::string rid = Crypto::GenerateRid();

    bool pending2fa = false;
    bool pendingOverload = false;
    bool pendingTooMany = false;
    bool pendingRelogon = false;
    bool pendingUpdate = false;
    bool pendingMaintenance = false;

    Clock::time_point reconnectAt{};
    std::optional<Clock::time_point> joinAt;
    std::optional<Clock::time_point> joinDeadline;
    std::string joining;
    std::string joiningDoor;

    uint32_t hackType = 0;
    uint8_t buildLength = 0;
    uint8_t punchLength = 0;
    float gravity = 0.0f;
    float velocity = 0.0f;
    bool inWorld = false;

    uint32_t lastPing = 0;
    Clock::time_point started = Clock::now();

    struct Path {
        bool running = false;
        uint64_t request = 0;
        Pathfinder::Point target;
        std::vector<Pathfinder::Point> nodes;
        size_t next = 0;
        int replans = 0;
        std::string world;
        Clock::time_point nextStep{};
    } path;
    bool facingLeft = false;

    bool autoCollect = false;
    int collectRadius = Pathfinder::REACH;
    Clock::time_point nextAutoCollect{};
    std::unordered_map<uint32_t, Clock::time_point> collectAsked; /* object uid -> retry after */

    explicit Impl(const std::optional<Net::Socks5Config>& proxy) : http(proxy) {}
};

Bot::Bot(int id, Config::BotEntry entry, const Config::Common& settings)
    : id_(id), entry_(std::move(entry)), settings_(settings), tag_(entry_.growId), impl_(std::make_unique<Impl>(entry_.proxy)) {
    state_.index = id_;
    state_.growId = entry_.growId;
    state_.name = entry_.growId;
    state_.proxy = entry_.proxy ? entry_.proxy->ToString() : std::string();
    world_.tag = tag_;

    Net::Startup();
    const SOCKET wake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (wake != INVALID_SOCKET) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(address);
        u_long nonBlocking = 1;
        if (bind(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            getsockname(wake, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
            ioctlsocket(wake, FIONBIO, &nonBlocking) == 0) {
            wake_ = static_cast<uintptr_t>(wake);
            wakePort_ = ntohs(address.sin_port);
        }
        else closesocket(wake);
    }
}

Bot::~Bot() {
    Stop();
    if (static_cast<SOCKET>(wake_) != INVALID_SOCKET) closesocket(static_cast<SOCKET>(wake_));
}

void Bot::Start(int delayMs) { Connect(delayMs); }

void Bot::Connect(int delayMs) {
    std::lock_guard lock(lifecycle_);
    offline_ = false;
    if (running_) {
        Post([this, delayMs] { if (!impl_->host) impl_->reconnectAt = Clock::now() + std::chrono::milliseconds(delayMs); });
        return;
    }
    if (thread_.joinable()) thread_.join();
    SetStatus(BotStatus::Idle);
    stop_ = false;
    running_ = true;
    thread_ = std::thread([this, delayMs] { Run(delayMs); });
}

void Bot::Stop() {
    std::lock_guard lock(lifecycle_);
    stop_ = true;
    wakePending_ = false;
    Wake();
    if (thread_.joinable()) thread_.join();
}

void Bot::Disconnect() {
    offline_ = true;
    wakePending_ = false;
    Wake();
}

void Bot::Reconnect() {
    if (!running_) {
        Connect();
        return;
    }
    offline_ = false;
    Post([this] {
        Impl& d = *impl_;
        if (d.host) Log::Info(tag_, "reconnecting");
        CloseConnection();
        d.redirect.reset();
        d.needLogin = true;
        d.reconnectAt = Clock::now() + 500ms;
        SetStatus(BotStatus::Waiting, "reconnecting");
    });
}

BotSnapshot Bot::Snapshot() const { std::lock_guard lock(mutex_); return state_; }
BotStatus Bot::Status() const { std::lock_guard lock(mutex_); return state_.status; }
std::string Bot::World() const { std::lock_guard lock(mutex_); return state_.world; }
std::string Bot::TargetWorld() const { std::lock_guard lock(mutex_); return state_.targetWorld; }
int32_t Bot::Gems() const { std::lock_guard lock(mutex_); return state_.gems; }
uint32_t Bot::Ping() const { std::lock_guard lock(mutex_); return state_.pingMs; }

std::pair<float, float> Bot::Position() const {
    std::lock_guard lock(mutex_);
    return { state_.x, state_.y };
}

std::pair<int, int> Bot::Tile() const {
    const Pathfinder::Point tile = CurrentTile();
    return { tile.x, tile.y };
}

Pathfinder::Point Bot::CurrentTile() const {
    std::lock_guard lock(mutex_);
    return Pathfinder::TileOf(state_.x, state_.y);
}

std::vector<InventoryItem> Bot::Inventory() const {
    std::lock_guard lock(mutex_);
    std::vector<InventoryItem> items;
    items.reserve(state_.inventory.size());
    for (const auto& [id, item] : state_.inventory) items.push_back(item);
    return items;
}

int Bot::ItemCount(uint16_t itemID) const {
    std::lock_guard lock(mutex_);
    const auto it = state_.inventory.find(itemID);
    return it == state_.inventory.end() ? 0 : it->second.amount;
}

bool Bot::HasItem(uint16_t itemID, int amount) const { return ItemCount(itemID) >= amount; }

std::vector<PlayerInfo> Bot::Players() const {
    std::lock_guard lock(mutex_);
    std::vector<PlayerInfo> players;
    players.reserve(state_.players.size());
    for (const auto& [netID, player] : state_.players) players.push_back(player);
    return players;
}

void Bot::Wake() {
    if (gLegacyWait.load(std::memory_order_relaxed)) return;
    const SOCKET wake = static_cast<SOCKET>(wake_);
    if (wake == INVALID_SOCKET || wakePending_.exchange(true)) return;
    sockaddr_in self{};
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    self.sin_port = htons(wakePort_);
    const char byte = 1;
    sendto(wake, &byte, 1, 0, reinterpret_cast<const sockaddr*>(&self), sizeof(self));
}

void Bot::Post(std::function<void()> command) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(command));
    }
    Wake();
}

void Bot::SetTargetWorld(const std::string& world) {
    const std::string name = Text::Upper(Text::Trim(world));
    Update([&](BotSnapshot& s) { s.targetWorld = name; });
    Post([this, name] { if (!name.empty() && impl_->connected && World() != name) RequestJoin(name); });
}

void Bot::SetAutoReconnect(bool enabled) { autoReconnect_ = enabled; }

void Bot::Warp(const std::string& world, const std::string& door) {
    const std::string name = Text::Upper(Text::Trim(world));
    const std::string doorID = Text::Upper(Text::Trim(door));
    Post([this, name, doorID] { if (!name.empty()) RequestJoin(name, doorID); });
}

void Bot::LeaveWorld() {
    Post([this] { SendGameMessage("action|quit_to_exit\n"); });
}

void Bot::Say(const std::string& text) {
    Post([this, text] { SendText("action|input\n|text|" + text + "\n"); });
}

uint64_t Bot::Walk(int tileX, int tileY) {
    const uint64_t request = ++pathRequests_;
    Post([this, tileX, tileY, request] { StartPath(tileX, tileY, request); });
    return request;
}

void Bot::StopWalk() {
    Post([this] { if (impl_->path.running) EndPath(false, "stopped"); });
}

bool Bot::PathDone(uint64_t request, bool* ok, std::string* error) const {
    std::lock_guard lock(mutex_);
    if (state_.pathDone < request) return false;
    /* A later walk ending first means this one was replaced. */
    const bool mine = state_.pathDone == request;
    if (ok) *ok = mine && state_.pathOk;
    if (error) *error = mine ? state_.pathError : std::string("replaced by a newer walk");
    return true;
}

Pathfinder::Result Bot::FindPath(int tileX, int tileY, std::vector<Pathfinder::Point>& path) const {
    const Pathfinder::Point from = CurrentTile();
    const auto userID = static_cast<uint32_t>(Read([](const BotSnapshot& s) { return s.userID; }));
    std::lock_guard lock(worldMutex_);
    return Pathfinder::Find(world_, from, { tileX, tileY }, userID, path);
}

bool Bot::InReach(int tileX, int tileY, bool placesBlock) const {
    return Pathfinder::InReach(CurrentTile(), tileX, tileY, placesBlock);
}

void Bot::Drop(uint16_t itemID, int amount) {
    Post([this, itemID, amount] {
        if (amount <= 0 || !HasItem(itemID, amount)) {
            Log::Warn(tag_, "drop: not enough of item {} (have {}, want {})", itemID, ItemCount(itemID), amount);
            return;
        }
        SendText(std::format("action|drop\n|itemID|{}\n", itemID));
        SendText(std::format("action|dialog_return\ndialog_name|drop_item\nitemID|{}|\ncount|{}\n", itemID, amount));
    });
}

void Bot::Trash(uint16_t itemID, int amount) {
    Post([this, itemID, amount] {
        if (amount <= 0 || !HasItem(itemID, amount)) {
            Log::Warn(tag_, "trash: not enough of item {} (have {}, want {})", itemID, ItemCount(itemID), amount);
            return;
        }
        SendText(std::format("action|trash\n|itemID|{}\n", itemID));
        SendText(std::format("action|dialog_return\ndialog_name|trash_item\nitemID|{}|\ncount|{}\n", itemID, amount));
    });
}

void Bot::Wear(uint16_t itemID) {
    Post([this, itemID] {
        TankPacket tank;
        tank.type = PACKET_ITEM_ACTIVATE_REQUEST;
        tank.value = itemID;
        SendTank(tank, true);
    });
}

bool Bot::Punch(int tileX, int tileY) {
    Post([this, tileX, tileY] { if (CheckReach("punch", tileX, tileY, false)) SendTileAction(tileX, tileY, ITEM_FIST, STATE_PUNCH); });
    return InReach(tileX, tileY);
}

bool Bot::Place(int tileX, int tileY, uint16_t itemID) {
    Post([this, tileX, tileY, itemID] {
        if (!HasItem(itemID)) {
            Log::Warn(tag_, "place: no item {} in the inventory", itemID);
            return;
        }
        if (CheckReach("place", tileX, tileY, !ItemDB::IsBackground(itemID))) SendTileAction(tileX, tileY, itemID, STATE_PLACE);
    });
    return HasItem(itemID) && InReach(tileX, tileY, !ItemDB::IsBackground(itemID));
}

bool Bot::Wrench(int tileX, int tileY) {
    Post([this, tileX, tileY] { if (CheckReach("wrench", tileX, tileY, false)) SendTileAction(tileX, tileY, ITEM_WRENCH, STATE_PUNCH); });
    return InReach(tileX, tileY);
}

bool Bot::Activate(int tileX, int tileY) {
    Post([this, tileX, tileY] {
        if (!CheckReach("enter", tileX, tileY, false)) return;
        const auto [x, y] = Position();
        TankPacket tank;
        tank.type = PACKET_TILE_ACTIVATE_REQUEST;
        tank.x = x;
        tank.y = y;
        tank.punchX = tileX;
        tank.punchY = tileY;
        SendTank(tank, true);
    });
    return InReach(tileX, tileY);
}

int Bot::Collect(int radius) {
    radius = std::clamp(radius, 0, Pathfinder::REACH);
    Post([this, radius] { CollectNear(radius, false); });
    return static_cast<int>(CollectTargets(radius).size());
}

void Bot::SetAutoCollect(bool enabled, int radius) {
    radius = std::clamp(radius, 0, Pathfinder::REACH);
    Update([&](BotSnapshot& s) { s.autoCollect = enabled; s.collectRadius = radius; });
    Post([this, enabled, radius] {
        impl_->autoCollect = enabled;
        impl_->collectRadius = radius;
        impl_->nextAutoCollect = Clock::now();
    });
}

void Bot::Respawn() {
    Post([this] { SendText("action|respawn\n"); });
}

void Bot::DialogReturn(const std::string& dialog, const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string text = "action|dialog_return\ndialog_name|" + dialog + "\n";
    for (const auto& [key, value] : fields) text += key + "|" + value + "\n";
    Post([this, text = std::move(text)] { SendText(text); });
}

void Bot::SendRawText(const std::string& text) { Post([this, text] { SendText(text); }); }
void Bot::SendRawGameMessage(const std::string& text) { Post([this, text] { SendGameMessage(text); }); }
void Bot::SendRawTank(const TankPacket& tank, bool reliable) {
    Post([this, tank, reliable] {
        /* Raw packets get the same 5x5 reach guard as the built-in actions. */
        const bool tileAction = tank.type == PACKET_TILE_CHANGE_REQUEST || tank.type == PACKET_TILE_ACTIVATE_REQUEST ||
            (tank.type == PACKET_STATE && (tank.flags & (STATE_PUNCH | STATE_PLACE)) != 0);
        if (tileAction) {
            const bool placesBlock = tank.type == PACKET_TILE_CHANGE_REQUEST && tank.value != ITEM_FIST && tank.value != ITEM_WRENCH &&
                !ItemDB::IsBackground(tank.value);
            if (!CheckReach("raw packet", tank.punchX, tank.punchY, placesBlock)) return;
        }
        /* The server now has us where the packet says. */
        if (tank.type == PACKET_STATE) Update([&](BotSnapshot& s) { s.x = tank.x; s.y = tank.y; });
        SendTank(tank, reliable);
    });
}

void Bot::SetStatus(BotStatus status, std::string text) {
    bool changed = false;
    Update([&](BotSnapshot& s) {
        changed = s.status != status || s.statusText != text;
        s.status = status;
        s.statusText = text;
    });
    if (changed && Events::Wanted("status")) Emit("status", { std::string(StatusKey(status)), std::move(text) });
}

void Bot::Emit(const char* name, std::initializer_list<Events::Value> args) {
    Events::Emit({ id_, name, std::vector<Events::Value>(args) });
}

void Bot::Run(int delayMs) {
    Log::Info(tag_, "started{}", entry_.proxy ? " via SOCKS5 " + entry_.proxy->ToString() : std::string(" (no proxy)"));
    Impl& d = *impl_;

    d.reconnectAt = Clock::now() + std::chrono::milliseconds(delayMs);
    if (delayMs > 0) SetStatus(BotStatus::Waiting, std::format("login queue ({} s)", delayMs / 1000));

    while (!stop_) {
        if (Status() == BotStatus::Stopped) break;

        if (offline_) {
            if (d.host) {
                CloseConnection();
                if (Events::Wanted("disconnected")) Emit("disconnected", { std::string("disconnected by the user") });
            }
            d.redirect.reset();
            d.needLogin = true;
            if (Status() != BotStatus::Offline) {
                Log::Info(tag_, "offline");
                SetStatus(BotStatus::Offline, "disconnected");
            }
            RunCommands();
            Wait(IDLE_WAIT_MS);
            continue;
        }

        if (!d.host) {
            const auto now = Clock::now();
            if (now < d.reconnectAt) {
                RunCommands();
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(d.reconnectAt - now).count();
                Wait(static_cast<int>(std::clamp<long long>(left, 1, IDLE_WAIT_MS)));
                continue;
            }
            if (d.needLogin && !Authenticate()) continue;
            if (!OpenConnection(d.serverIP, d.serverPort)) {
                ScheduleReconnect(NETWORK_RETRY_MS, "couldn't open the connection");
                continue;
            }
        }
        Service();
    }

    CloseConnection();
    if (d.relay) d.relay->Close();
    if (Status() != BotStatus::Stopped) SetStatus(BotStatus::Stopped, "stopped");
    Log::Info(tag_, "stopped ({})", Read([](const BotSnapshot& s) { return s.statusText; }));
    running_ = false;
}

void Bot::ScheduleReconnect(int delayMs, std::string reason) {
    impl_->reconnectAt = Clock::now() + std::chrono::milliseconds(delayMs);
    Log::Warn(tag_, "{} -- retrying in {} s", reason, delayMs / 1000);
    SetStatus(BotStatus::Waiting, std::move(reason));
}

bool Bot::Authenticate() {
    SetStatus(BotStatus::LoggingIn);
    Impl& d = *impl_;
    std::string error;

    if (!settings_.benchServer.empty()) {
        const auto parts = Text::Split(settings_.benchServer, ':');
        d.serverIP = std::string(parts[0]);
        d.serverPort = static_cast<uint16_t>(parts.size() > 1 ? Text::ToInt(parts[1]).value_or(17091) : 17091);
        d.ltoken = "bench";
        d.needLogin = false;
        return true;
    }

    Log::Info(tag_, "fetching server_data ({})", d.alternate ? "growtopia2" : "growtopia1");
    const auto server = Login::FetchServerData(d.http, Client(), d.alternate, error);
    if (!server) {
        d.alternate = !d.alternate;
        ScheduleReconnect(NETWORK_RETRY_MS, "server_data: " + error);
        return false;
    }
    d.serverIP = server->server;
    d.serverPort = server->port;
    d.meta = server->meta;

    if (!d.ltoken.empty()) {
        std::string refreshed;
        const auto result = Login::CheckToken(d.http, d.ltoken, LoginData(), refreshed);
        if (result) {
            Log::Info(tag_, "token refreshed");
            d.ltoken = refreshed;
            d.needLogin = false;
            return true;
        }
        Log::Warn(tag_, "{} -- doing a full login", result.message);
        d.ltoken.clear();
    }

    Log::Info(tag_, "logging in as {}", entry_.growId);
    d.http.ClearCookies();
    std::string token;
    const auto result = Login::LegacyToken(d.http, Client(), *server, entry_.growId, entry_.password, token);
    switch (result.error) {
        case Login::Error::None:
            d.ltoken = token;
            d.needLogin = false;
            Log::Info(tag_, "got the login token");
            return true;
        case Login::Error::WrongCredentials:
            Log::Error(tag_, "wrong GrowID or password: {}", result.message);
            SetStatus(BotStatus::Stopped, "wrong GrowID / password");
            return false;
        case Login::Error::Exhausted:
            Log::Error(tag_, "{}", result.message);
            SetStatus(BotStatus::Stopped, "login attempts exhausted (try again in 24 h)");
            return false;
        case Login::Error::Network:
            ScheduleReconnect(NETWORK_RETRY_MS, result.message);
            return false;
        default:
            ScheduleReconnect(LOGIN_RETRY_MS, result.message);
            return false;
    }
}

bool Bot::OpenConnection(const std::string& ip, uint16_t port) {
    Impl& d = *impl_;
    CloseConnection();
    SetStatus(BotStatus::Connecting);

    ENetAddress target{};
    target.type = ENET_ADDRESS_TYPE_IPV4;
    if (enet_address_set_host_ip(&target, ip.c_str()) != 0) {
        Log::Error(tag_, "bad server address {}", ip);
        return false;
    }
    target.port = port;

    ENetAddress connectTo = target;
    if (entry_.proxy) {
        std::string error;
        if (!d.relay || !d.relay->IsOpen()) {
            d.relay.emplace();
            if (!d.relay->Open(*entry_.proxy, error)) {
                Log::Error(tag_, "{}", error);
                d.relay.reset();
                return false;
            }
            Log::Info(tag_, "SOCKS5 UDP relay ready");
        }
        const auto real = Net::Resolve(ip, port);
        if (!real) return false;
        d.relay->SetTarget(*real);

        const sockaddr_in local = d.relay->LocalAddress();
        char localIP[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &local.sin_addr, localIP, sizeof(localIP));
        enet_address_set_host_ip(&connectTo, localIP);
        connectTo.port = ntohs(local.sin_port);
    }

    d.host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, nullptr, 1, 2, 0, 0);
    if (!d.host) {
        Log::Error(tag_, "enet_host_create failed");
        return false;
    }
    d.host->checksum = enet_crc32;
    enet_host_compress_with_range_coder(d.host);
    d.host->usingNewPacketForClient = 1;

    d.peer = enet_host_connect(d.host, &connectTo, 2, 0);
    if (!d.peer) {
        Log::Error(tag_, "enet_host_connect failed");
        CloseConnection();
        return false;
    }
    Log::Info(tag_, "connecting to {}:{}", ip, port);
    return true;
}

void Bot::CloseConnection() {
    Impl& d = *impl_;
    if (d.path.running) EndPath(false, "disconnected");
    if (d.peer && d.connected) enet_peer_disconnect_now(d.peer, 0);
    if (d.host) enet_host_destroy(d.host);
    d.host = nullptr;
    d.peer = nullptr;
    d.connected = false;
    d.inWorld = false;
    d.lastPing = 0;
    d.joinAt.reset();
    d.joinDeadline.reset();
    d.joining.clear();
    d.collectAsked.clear();
    EditWorld([](WorldState& world) { world.Clear(); });
    Update([](BotSnapshot& s) {
        s.world.clear();
        s.worldWidth = s.worldHeight = 0;
        s.players.clear();
        s.pingMs = 0;
    });
}

void Bot::Wait(int timeoutMs) {
    Impl& d = *impl_;
    wakeups_.fetch_add(1, std::memory_order_relaxed);

    const bool legacy = gLegacyWait.load(std::memory_order_relaxed);
    if (legacy) {
        if (!d.host) { std::this_thread::sleep_for(50ms); return; }
        timeoutMs = 5;
    }

    fd_set readable;
    FD_ZERO(&readable);
    const SOCKET wake = static_cast<SOCKET>(wake_);
    if (!legacy && wake != INVALID_SOCKET) FD_SET(wake, &readable);
    if (d.host) FD_SET(d.host->socket, &readable);
    if (d.relay && d.relay->IsOpen()) {
        FD_SET(d.relay->LocalSocket(), &readable);
        FD_SET(d.relay->ProxySocket(), &readable);
    }
    if (readable.fd_count == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        return;
    }
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    select(0, &readable, nullptr, nullptr, &tv);

    if (!legacy && wake != INVALID_SOCKET && FD_ISSET(wake, &readable)) {
        char buffer[64];
        while (recv(wake, buffer, sizeof(buffer), 0) > 0) {}
        wakePending_.store(false);
    }
}

void Bot::Service() {
    Impl& d = *impl_;

    if (d.relay && !d.relay->Pump()) {
        Log::Warn(tag_, "SOCKS5 proxy closed the UDP association");
        d.relay.reset();
        CloseConnection();
        d.needLogin = !d.redirect;
        ScheduleReconnect(NETWORK_RETRY_MS, "SOCKS5 association lost");
        return;
    }

    ENetEvent event{};
    while (d.host && enet_host_service(d.host, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                d.connected = true;
                Log::Info(tag_, "connected, waiting for hello");
                if (Events::Wanted("connected")) Emit("connected");
                break;

            case ENET_EVENT_TYPE_RECEIVE:
                OnPacket({ event.packet->data, event.packet->dataLength });
                enet_packet_destroy(event.packet);
                break;

            case ENET_EVENT_TYPE_DISCONNECT: {
                const bool wasConnected = d.connected;
                CloseConnection();
                if (d.redirect && ++d.redirect->attempts <= 3) {
                    Log::Info(tag_, "redirecting to {}:{}", d.redirect->server, d.redirect->port);
                    if (!OpenConnection(d.redirect->server, d.redirect->port)) {
                        d.redirect.reset();
                        d.needLogin = true;
                        ScheduleReconnect(NETWORK_RETRY_MS, "redirect connection failed");
                    }
                    break;
                }
                d.redirect.reset();
                d.needLogin = true;
                if (Events::Wanted("disconnected"))
                    Emit("disconnected", { std::string(wasConnected ? "disconnected by the server" : "couldn't reach the server") });
                if (Status() == BotStatus::Stopped || stop_ || offline_) break;
                if (!autoReconnect_) {
                    SetStatus(BotStatus::Stopped, "disconnected (auto-reconnect is off)");
                    break;
                }
                if (Clock::now() < d.reconnectAt) break;
                ScheduleReconnect(wasConnected ? 1000 : NETWORK_RETRY_MS, wasConnected ? "disconnected by the server" : "couldn't reach the server");
                break;
            }
            default: break;
        }
    }

    RunCommands();
    RunTimers();

    if (d.host) {
        if (d.peer && d.peer->roundTripTime != d.lastPing) {
            d.lastPing = d.peer->roundTripTime;
            Update([&](BotSnapshot& s) { s.pingMs = d.lastPing; });
        }
        enet_host_flush(d.host);
    }
    if (d.relay) d.relay->Pump();

    int waitMs = CONNECTED_WAIT_MS;
    if (d.path.running) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(d.path.nextStep - Clock::now()).count();
        waitMs = static_cast<int>(std::clamp<long long>(left, 0, CONNECTED_WAIT_MS));
    }
    Wait(waitMs);
}

void Bot::RunCommands() {
    std::deque<std::function<void()>> commands;
    {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return;
        commands.swap(queue_);
    }
    commands_.fetch_add(commands.size(), std::memory_order_relaxed);
    for (auto& command : commands) {
        try { command(); }
        catch (const std::exception& e) { Log::Error(tag_, "command failed: {}", e.what()); }
    }
}

void Bot::RunTimers() {
    Impl& d = *impl_;
    if (!d.connected) return;
    const auto now = Clock::now();

    if (d.joinAt && now >= *d.joinAt) {
        d.joinAt.reset();
        const std::string target = TargetWorld();
        if (!target.empty() && World() != target) RequestJoin(target);
    }
    if (d.joinDeadline && now >= *d.joinDeadline) {
        d.joinDeadline.reset();
        Log::Warn(tag_, "no answer to joining {} -- asking again", d.joining);
        RequestJoin(d.joining, d.joiningDoor);
    }
    if (d.path.running && now >= d.path.nextStep) PathStep();
    if (d.autoCollect && d.inWorld && now >= d.nextAutoCollect) {
        d.nextAutoCollect = now + AUTO_COLLECT_EVERY;
        CollectNear(d.collectRadius, true);
    }
}

void Bot::SendText(std::string_view text) {
    if (!impl_->peer || !impl_->connected) return;
    const auto payload = MakeText(NET_MESSAGE_GENERIC_TEXT, text);
    enet_peer_send(impl_->peer, 0, enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE));
}

void Bot::SendGameMessage(std::string_view text) {
    if (!impl_->peer || !impl_->connected) return;
    const auto payload = MakeText(NET_MESSAGE_GAME_MESSAGE, text);
    enet_peer_send(impl_->peer, 0, enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE));
}

void Bot::SendTank(const TankPacket& tank, bool reliable) {
    if (!impl_->peer || !impl_->connected) return;
    const auto payload = MakeTank(tank);
    enet_peer_send(impl_->peer, 0, enet_packet_create(payload.data(), payload.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0));
}

void Bot::RequestJoin(const std::string& world, const std::string& door) {
    Impl& d = *impl_;
    d.joining = world;
    d.joiningDoor = door;
    d.joinDeadline = Clock::now() + JOIN_TIMEOUT;
    SetStatus(BotStatus::JoiningWorld);
    Log::Info(tag_, "joining {}{}", world, door.empty() ? "" : " (door " + door + ")");
    SendGameMessage(std::format("action|join_request\nname|{}|{}\ninvitedWorld|0\n", world, door));
}

void Bot::SendTileAction(int tileX, int tileY, uint16_t itemID, uint32_t stateFlag) {
    const auto [x, y] = Position();
    TankPacket request;
    request.type = PACKET_TILE_CHANGE_REQUEST;
    request.value = itemID;
    request.x = x;
    request.y = y;
    request.punchX = tileX;
    request.punchY = tileY;
    SendTank(request, true);

    TankPacket state;
    state.type = PACKET_STATE;
    state.x = x;
    state.y = y;
    state.punchX = tileX;
    state.punchY = tileY;
    state.flags = stateFlag | STATE_STANDING | (static_cast<float>(tileX) * 32.0f < x ? STATE_FACING_LEFT : 0u);
    SendTank(state, false);
}

std::string Bot::LoginData() const {
    const Impl& d = *impl_;
    const std::string protocol = std::to_string(Client().protocol);
    const std::string klv = Crypto::Klv(Client().gameVersion, protocol, d.rid, d.hash);
    return std::format(
        "tankIDName|\ntankIDPass|\nrequestedName|\nf|1\nprotocol|{}\ngame_version|{}\nfz|22243512\ncbits|1024\nplayer_age|20\n"
        "GDPR|2\nFCMToken|\ncategory|_-5100\ntotalPlaytime|0\nklv|{}\nhash2|{}\nmeta|{}\nfhash|{}\nrid|{}\nplatformID|0,1,1\n"
        "deviceVersion|0\ncountry|jp\nhash|{}\nmac|{}\nwk|{}\nzf|31631978\nlmode|1\n",
        protocol, Client().gameVersion, klv, d.hash2, d.meta, Client().fhash, d.rid, d.hash, d.mac, d.wk);
}

std::string Bot::RedirectPacket(const Redirect& r) const {
    const Impl& d = *impl_;
    const std::string klv = Crypto::Klv(Client().gameVersion, std::to_string(Client().protocol), d.rid, d.hash);
    std::string data = std::format(
        "tankIDName|{}\ntankIDPass|\nrequestedName|\nf|1\nprotocol|211\ngame_version|{}\nfz|47142936\ncbits|1536\nplayer_age|18\n"
        "GDPR|1\nFCMToken|\ncategory|_-5100\ntotalPlaytime|0\nklv|{}\nhash2|{}\nmeta|{}\nfhash|{}\nrid|{}\nplatformID|0,1,1\n"
        "deviceVersion|0\ncountry|ma\nhash|{}\nmac|{}\nwk|{}\nzf|-821693372\nlmode|{}\nuser|{}\ntoken|{}\nUUIDToken|{}\n",
        r.tankIDName, Client().gameVersion, klv, d.hash2, d.meta, Client().fhash, d.rid, d.hash, d.mac, d.wk,
        r.lmode, r.user, r.token, r.uuid);
    if (!r.doorID.empty()) data += "doorID|" + r.doorID + "\n";
    data += "aat|2\n";
    return data;
}

void Bot::OnPacket(std::span<const uint8_t> data) {
    packets_.fetch_add(1, std::memory_order_relaxed);
    switch (MessageOf(data)) {
        case NET_MESSAGE_SERVER_HELLO: OnServerHello(); break;
        case NET_MESSAGE_GAME_MESSAGE: OnGameMessage(TextOf(data)); break;
        case NET_MESSAGE_GENERIC_TEXT:
            Log::Debug(tag_, "text: {}", TextOf(data));
            if (Events::Wanted("text")) Emit("text", { std::string(TextOf(data)) });
            break;
        case NET_MESSAGE_TRACK:        OnTrack(TextOf(data)); break;
        case NET_MESSAGE_GAME_PACKET:  OnGamePacket(data); break;
        default: break;
    }
}

void Bot::OnServerHello() {
    Impl& d = *impl_;
    if (d.redirect) {
        Log::Info(tag_, "hello (sub-server, door '{}')", d.redirect->doorID);
        SendText(RedirectPacket(*d.redirect));
        d.redirect.reset();
    }
    else {
        Log::Info(tag_, "hello, sending the login token");
        SendText(std::format("protocol|{}\nltoken|{}\nplatformID|2\n", Client().protocol, d.ltoken));
    }
}

void Bot::OnGameMessage(std::string_view text) {
    Impl& d = *impl_;
    Log::Debug(tag_, "game message: {}", text);
    if (Events::Wanted("game_message")) Emit("game_message", { std::string(text) });

    if (Text::Contains(text, "action|log")) {
        if (const auto msg = Text::Value(text, "msg")) Log::Info(tag_, "{}", StripColors(*msg));
        if (Text::Contains(text, "Advanced Account Protection")) d.pending2fa = true;
        if (Text::Contains(text, "SERVER OVERLOADED")) d.pendingOverload = true;
        if (Text::Contains(text, "Too many people logging in")) d.pendingTooMany = true;
        if (Text::Contains(text, "UPDATE REQUIRED")) d.pendingUpdate = true;
        if (Text::Contains(text, "undergoing maintenance")) d.pendingMaintenance = true;
        if (Text::Contains(text, "Server requesting that you re-logon")) {
            d.redirect.reset();
            d.pendingRelogon = true;
        }
    }

    if (!Text::Contains(text, "action|logon_fail")) return;

    int delay = 1000;
    std::string reason = "logon failed";
    if (std::exchange(d.pending2fa, false)) { delay = TWO_FA_MS; reason = "2FA (Advanced Account Protection)"; }
    else if (std::exchange(d.pendingOverload, false)) { delay = OVERLOAD_MS; reason = "server overloaded"; }
    else if (std::exchange(d.pendingTooMany, false)) { delay = TOO_MANY_LOGINS_MS; reason = "too many people logging in"; }
    else if (std::exchange(d.pendingMaintenance, false)) { delay = MAINTENANCE_MS; reason = "server maintenance"; }
    else if (std::exchange(d.pendingUpdate, false)) {
        Log::Error(tag_, "UPDATE REQUIRED -- raise the game version in Settings");
        SetStatus(BotStatus::Stopped, "update required (raise the game version in Settings)");
        if (Events::Wanted("logon_fail")) Emit("logon_fail", { std::string("update required") });
        if (d.peer) enet_peer_disconnect(d.peer, 0);
        return;
    }
    else if (std::exchange(d.pendingRelogon, false)) reason = "server asked for a re-logon";

    if (Events::Wanted("logon_fail")) Emit("logon_fail", { reason });
    d.redirect.reset();
    d.needLogin = true;
    ScheduleReconnect(delay, "logon failed: " + reason);
    if (d.peer) enet_peer_disconnect(d.peer, 0);
}

void Bot::OnTrack(std::string_view text) {
    const auto map = Text::PipeMap(text);
    if (const auto it = map.find("Level"); it != map.end()) {
        const int level = static_cast<int>(Text::ToInt(it->second).value_or(0));
        Update([&](BotSnapshot& s) { s.level = level; });
    }
}

void Bot::OnGamePacket(std::span<const uint8_t> data) {
    const auto tank = TankOf(data);
    if (!tank) return;
    Impl& d = *impl_;

    if (Events::Wanted("packet")) {
        Emit("packet", { int64_t(tank->type), int64_t(tank->netID), int64_t(tank->value), double(tank->x), double(tank->y),
            int64_t(tank->punchX), int64_t(tank->punchY), int64_t(tank->flags) });
    }

    switch (tank->type) {
        case PACKET_CALL_FUNCTION:
            OnCallFunction(ExtendedOf(data));
            break;

        case PACKET_PING_REQUEST:
            OnPingRequest(static_cast<uint32_t>(tank->value));
            break;

        case PACKET_SET_CHARACTER_STATE:
            d.hackType = static_cast<uint32_t>(tank->value);
            d.buildLength = tank->count1 > 126 ? static_cast<uint8_t>(tank->count1 - 126) : 0;
            d.punchLength = tank->count2 > 126 ? static_cast<uint8_t>(tank->count2 - 126) : 0;
            d.gravity = tank->xSpeed;
            d.velocity = tank->ySpeed;
            break;

        case PACKET_SEND_INVENTORY_STATE:
            OnInventoryState(ExtendedOf(data));
            break;

        case PACKET_MODIFY_ITEM_INVENTORY: {
            if (tank->value <= 0 || tank->targetNetID != 0) break;
            const auto id = static_cast<uint16_t>(tank->value);
            int amount = 0;
            Update([&](BotSnapshot& s) {
                auto& item = s.inventory[id];
                item.id = id;
                if (tank->count2 > 0) item.amount = static_cast<uint8_t>(std::min(200, item.amount + tank->count2));
                else item.amount = static_cast<uint8_t>(std::max(0, item.amount - tank->count1));
                amount = item.amount;
                if (item.amount == 0) s.inventory.erase(id);
            });
            if (Events::Wanted("item")) Emit("item", { int64_t(id), int64_t(tank->count2 > 0 ? tank->count2 : -tank->count1), int64_t(amount) });
            break;
        }

        case PACKET_SEND_MAP_DATA:
            OnMapData(ExtendedOf(data));
            break;

        case PACKET_TILE_CHANGE_REQUEST:
            OnTileChange(*tank);
            break;

        case PACKET_SEND_TILE_UPDATE_DATA:
            OnTileUpdate(*tank, ExtendedOf(data));
            break;

        case PACKET_SEND_TILE_UPDATE_DATA_MULTIPLE: {
            const auto extended = ExtendedOf(data);
            EditWorld([&](WorldState& world) { world.UpdateTiles(extended); });
            break;
        }

        case PACKET_SEND_TILE_TREE_STATE: {
            /* A harvested tree is gone. */
            int64_t bg = -1;
            EditWorld([&](WorldState& world) {
                if (WorldTile* tile = world.Tile(tank->punchX, tank->punchY)) {
                    tile->ClearForeground();
                    bg = tile->bg;
                }
            });
            if (bg >= 0 && Events::Wanted("tile_change")) Emit("tile_change", { int64_t(tank->punchX), int64_t(tank->punchY), int64_t(0), bg });
            break;
        }

        case PACKET_SEND_LOCK:
            EditWorld([&](WorldState& world) {
                if (WorldTile* tile = world.Tile(tank->punchX, tank->punchY)) tile->fg = static_cast<uint16_t>(tank->value);
            });
            break;

        case PACKET_ITEM_CHANGE_OBJECT:
            OnItemChangeObject(*tank);
            break;

        case PACKET_STATE: {
            Update([&](BotSnapshot& s) {
                if (tank->netID == s.netID) {
                    s.x = tank->x;
                    s.y = tank->y;
                }
                else if (const auto it = s.players.find(tank->netID); it != s.players.end()) {
                    it->second.x = tank->x;
                    it->second.y = tank->y;
                }
            });
            break;
        }
        default: break;
    }
}

void Bot::OnPingRequest(uint32_t challenge) {
    const Impl& d = *impl_;
    TankPacket reply;
    reply.type = PACKET_PING_REPLY;
    reply.targetNetID = Crypto::HashString(std::to_string(challenge));
    reply.value = static_cast<int32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - d.started).count());
    reply.x = (d.buildLength == 0 ? 2.0f : static_cast<float>(d.buildLength)) * 32.0f;
    reply.y = (d.punchLength == 0 ? 2.0f : static_cast<float>(d.punchLength)) * 32.0f;
    if (d.inWorld) {
        reply.netID = static_cast<int32_t>(d.hackType);
        reply.xSpeed = d.velocity;
        reply.ySpeed = d.gravity;
    }
    SendTank(reply, true);
    Log::Debug(tag_, "ping reply (challenge {})", challenge);
}

void Bot::OnInventoryState(std::span<const uint8_t> extended) {
    uint32_t slots = 0;
    uint16_t count = 0;
    if (!ReadAt(extended, 1, slots) || !ReadAt(extended, 5, count)) return;

    std::map<uint16_t, InventoryItem> items;
    for (size_t i = 0, offset = 7; i < count && offset + 4 <= extended.size(); ++i, offset += 4) {
        InventoryItem item;
        ReadAt(extended, offset, item.id);
        ReadAt(extended, offset + 2, item.amount);
        ReadAt(extended, offset + 3, item.flags);
        items[item.id] = item;
    }
    Update([&](BotSnapshot& s) {
        s.inventorySize = slots;
        s.inventory = std::move(items);
    });
    Log::Info(tag_, "inventory: {} items, {} slots", count, slots);
    if (Events::Wanted("inventory")) Emit("inventory", { int64_t(count), int64_t(slots) });
}

void Bot::OnMapData(std::span<const uint8_t> extended) {
    Impl& d = *impl_;
    if (d.path.running) EndPath(false, "entered a new world");
    d.collectAsked.clear();

    bool complete = false;
    std::string name;
    uint32_t width = 0, height = 0;
    size_t objects = 0;
    EditWorld([&](WorldState& w) {
        complete = w.Parse(extended);
        name = w.name;
        width = w.width;
        height = w.height;
        objects = w.objects.size();
    });
    const std::string world = Text::Upper(name);
    if (complete) Log::Debug(tag_, "map {}: {} x {}, {} dropped objects", world, width, height, objects);
    else Log::Warn(tag_, "map of {} only partly understood -- walking and tile data may be limited", world);

    d.inWorld = true;
    d.joinDeadline.reset();
    d.joinAt.reset();
    Update([&](BotSnapshot& s) {
        s.world = world;
        s.worldWidth = width;
        s.worldHeight = height;
        s.players.clear();
        s.status = BotStatus::InWorld;
        s.statusText.clear();
    });
    Log::Info(tag_, "entered {} ({} x {}, {} dropped items)", world, width, height, objects);
    if (Events::Wanted("status")) Emit("status", { std::string(StatusKey(BotStatus::InWorld)), std::string() });
    if (Events::Wanted("world_enter")) Emit("world_enter", { world, int64_t(width), int64_t(height) });

    const std::string target = TargetWorld();
    const bool requested = !d.joining.empty() && Text::Upper(d.joining) == world;
    if (!requested && !target.empty() && world != target) {
        Log::Info(tag_, "placed in {} by the server, leaving for {}", world, target);
        SendGameMessage("action|quit_to_exit\n");
    }
}

void Bot::OnWorldMenu() {
    Impl& d = *impl_;
    const bool wasInWorld = d.inWorld;
    if (d.path.running) EndPath(false, "left the world");
    d.inWorld = false;
    d.joinDeadline.reset();
    d.collectAsked.clear();
    EditWorld([](WorldState& world) { world.Clear(); });
    std::string left;
    Update([&](BotSnapshot& s) {
        left = std::move(s.world);
        s.world.clear();
        s.worldWidth = s.worldHeight = 0;
        s.players.clear();
        s.status = BotStatus::InGame;
        s.inventory.erase(1424);
        s.inventory.erase(5640);
    });
    if (Events::Wanted("status")) Emit("status", { std::string(StatusKey(BotStatus::InGame)), std::string() });
    if (Events::Wanted("world_menu")) Emit("world_menu", { left });

    const std::string target = TargetWorld();
    if (target.empty()) return;
    const bool failed = !d.joining.empty() && !wasInWorld;
    d.joinAt = Clock::now() + (failed ? std::chrono::milliseconds(settings_.rejoinDelayMs) : 500ms);
    if (wasInWorld) Log::Warn(tag_, "back at the world menu -- rejoining {}", target);
}

void Bot::OnCallFunction(std::span<const uint8_t> extended) {
    const auto parsed = VariantList::Parse(extended);
    if (!parsed || parsed->Empty()) return;
    const VariantList& list = *parsed;
    const std::string_view function = list.Function();
    Impl& d = *impl_;
    Log::Debug(tag_, "call: {}", function);

    if (Events::Wanted("variant")) {
        Events::Event event{ id_, "variant", {} };
        event.args.reserve(list.Size());
        for (size_t i = 0; i < list.Size(); ++i) event.args.push_back(ToValue(list[i]));
        Events::Emit(event);
    }

    if (function == "OnSendToServer") {
        Redirect r;
        r.port = static_cast<uint16_t>(list[1].Int());
        const int32_t token = list[2].Int();
        r.user = std::to_string(list[3].Int());
        const auto parts = Text::Split(list[4].String(), '|');
        r.server = std::string(Text::Trim(parts[0]));
        r.doorID = parts.size() > 1 && !Text::Trim(parts[1]).empty() ? std::string(Text::Trim(parts[1])) : "0";
        std::string uuid = parts.size() > 2 ? std::string(Text::Trim(parts[2])) : std::string();
        r.lmode = std::to_string(list[5].Int());
        r.tankIDName = std::string(list[6].String());

        if (token >= 0) d.lastRedirectToken = r.token = std::to_string(token);
        else r.token = d.lastRedirectToken.empty() ? std::to_string(token) : d.lastRedirectToken;
        if (!uuid.empty() && uuid != "-1") d.lastRedirectUuid = r.uuid = uuid;
        else r.uuid = d.lastRedirectUuid.empty() ? uuid : d.lastRedirectUuid;

        Log::Info(tag_, "sent to sub-server {}:{}", r.server, r.port);
        d.redirect = std::move(r);
        if (d.peer) enet_peer_disconnect(d.peer, 0);
        return;
    }

    if (function == "OnSuperMainStartAcceptLogonHrdxs47254722215a") {
        Log::Info(tag_, "logon accepted, entering the game");
        SetStatus(BotStatus::EnteringGame);
        SendText("action|enter_game\n");
        if (Events::Wanted("login")) Emit("login");
        return;
    }

    if (function == "OnRequestWorldSelectMenu") {
        OnWorldMenu();
        return;
    }

    if (function == "OnFailedToEnterWorld") {
        Log::Warn(tag_, "couldn't enter {} -- trying again in {} s", d.joining, settings_.rejoinDelayMs / 1000);
        d.joinDeadline.reset();
        d.joinAt = Clock::now() + std::chrono::milliseconds(settings_.rejoinDelayMs);
        SetStatus(BotStatus::InGame, "failed to enter " + d.joining);
        if (Events::Wanted("join_fail")) Emit("join_fail", { d.joining });
        return;
    }

    if (function == "OnSpawn") {
        const auto data = Text::PipeMap(list[1].String());
        const auto get = [&](const char* key) -> std::string {
            const auto it = data.find(key);
            return it == data.end() ? std::string() : it->second;
        };
        const auto number = [&](const char* key) { return static_cast<int32_t>(Text::ToInt(get(key)).value_or(0)); };

        float x = 0.0f, y = 0.0f;
        const std::string posXY = get("posXY");
        if (const auto pos = Text::Split(posXY, '|'); pos.size() >= 2) {
            x = static_cast<float>(std::atof(std::string(pos[0]).c_str()));
            y = static_cast<float>(std::atof(std::string(pos[1]).c_str()));
        }

        if (data.contains("type")) {
            const int32_t netID = number("netID");
            const int32_t userID = number("userID");
            const std::string name = StripColors(get("name"));
            Update([&](BotSnapshot& s) {
                s.netID = netID;
                s.userID = userID;
                s.x = x;
                s.y = y;
                if (!name.empty()) s.name = name;
            });
            Log::Info(tag_, "spawned (netID {}, userID {})", netID, userID);
            if (Events::Wanted("spawn")) Emit("spawn", { int64_t(netID), int64_t(userID), double(x), double(y) });
            return;
        }

        PlayerInfo player;
        player.netID = number("netID");
        player.userID = number("userID");
        player.name = StripColors(get("name"));
        player.country = get("country");
        player.x = x;
        player.y = y;
        player.moderator = number("mstate") > 0 || number("smstate") > 0 || number("invis") != 0;
        Update([&](BotSnapshot& s) { s.players[player.netID] = player; });
        if (player.moderator) Log::Warn(tag_, "moderator in the world: {}", player.name);
        if (Events::Wanted("player_join"))
            Emit("player_join", { int64_t(player.netID), player.name, int64_t(player.userID), player.country, player.moderator });
        if (player.moderator && Events::Wanted("moderator")) Emit("moderator", { player.name, int64_t(player.netID) });
        return;
    }

    if (function == "OnRemove") {
        const int32_t netID = Text::Int(list[1].String(), "netID", 0);
        std::string name;
        Update([&](BotSnapshot& s) {
            if (const auto it = s.players.find(netID); it != s.players.end()) {
                name = std::move(it->second.name);
                s.players.erase(it);
            }
        });
        if (Events::Wanted("player_leave")) Emit("player_leave", { int64_t(netID), name });
        return;
    }

    if (function == "OnSetPos") {
        if (list[1].type == Variant::Type::Vec2) {
            Update([&](BotSnapshot& s) {
                s.x = list[1].X();
                s.y = list[1].Y();
            });
        }
        return;
    }

    if (function == "OnSetBux") {
        const int32_t gems = list[1].Int();
        Update([&](BotSnapshot& s) { s.gems = gems; });
        if (Events::Wanted("gems")) Emit("gems", { int64_t(gems) });
        return;
    }

    if (function == "SetHasGrowID") {
        const std::string name(list[2].String());
        if (!name.empty()) Update([&](BotSnapshot& s) { s.name = name; });
        return;
    }

    if (function == "OnConsoleMessage") {
        const std::string text = StripColors(list[1].String());
        Log::Info(tag_, "{}", text);
        if (Events::Wanted("console")) Emit("console", { text, std::string(list[1].String()) });
        return;
    }

    if (function == "OnTalkBubble") {
        if (Events::Wanted("talk")) {
            const int32_t netID = list[1].Int();
            const std::string name = Read([&](const BotSnapshot& s) {
                if (netID == s.netID) return s.name;
                const auto it = s.players.find(netID);
                return it == s.players.end() ? std::string() : it->second.name;
            });
            Emit("talk", { int64_t(netID), name, StripColors(list[2].String()) });
        }
        return;
    }

    if (function == "OnDialogRequest") {
        Log::Debug(tag_, "dialog: {}", list[1].String());
        if (Events::Wanted("dialog")) Emit("dialog", { std::string(list[1].String()) });
        return;
    }
}

/* ---- reach guard ---------------------------------------------------------
   @important: the trick AquaProxy blocks. The game client only ever punches /
   places / wrenches inside the 5x5 square around the player; with mstate /
   smstate it can be made to reach further, and the server flags those packets
   and bans. A headless bot can send anything, so it checks every tile action
   against where the server has it and drops what a normal client couldn't
   send. */
bool Bot::CheckReach(const char* what, int tileX, int tileY, bool placesBlock) {
    const Pathfinder::Point here = CurrentTile();
    if (Pathfinder::InReach(here, tileX, tileY, placesBlock)) return true;
    if (tileX == here.x && tileY == here.y) Log::Warn(tag_, "{} at ({}, {}) dropped: a block can't be placed on the tile the bot stands on", what, tileX, tileY);
    else Log::Warn(tag_, "{} at ({}, {}) dropped: out of reach from ({}, {}), the game only allows {} tiles -- walk closer first",
        what, tileX, tileY, here.x, here.y, Pathfinder::REACH);
    return false;
}

/* ---- pathfinding (AquaProxy's /path) --------------------------------------- */
void Bot::StartPath(int tileX, int tileY, uint64_t request) {
    Impl& d = *impl_;
    if (d.path.running) EndPath(false, "replaced by a new walk");

    const Pathfinder::Point from = CurrentTile();
    const Pathfinder::Point to{ tileX, tileY };
    if (!d.connected || !d.inWorld) {
        FinishPath(request, false, "not in a world");
        return;
    }

    auto& path = d.path;
    path = {};
    path.request = request;
    path.target = to;
    const auto userID = static_cast<uint32_t>(Read([](const BotSnapshot& s) { return s.userID; }));
    const auto result = Pathfinder::Find(world_, from, to, userID, path.nodes);
    if (result != Pathfinder::Result::Found) {
        path.nodes.clear();
        FinishPath(request, false, Pathfinder::Describe(result));
        return;
    }
    if (path.nodes.size() == 1) {
        path.nodes.clear();
        FinishPath(request, true, {});
        return;
    }

    path.running = true;
    path.next = 1;
    path.world = world_.name;
    path.nextStep = Clock::now();
    Log::Debug(tag_, "walking to ({}, {}): {} tiles", tileX, tileY, path.nodes.size() - 1);
    Update([&](BotSnapshot& s) {
        s.walking = true;
        s.pathTargetX = tileX;
        s.pathTargetY = tileY;
        s.pathLeft = path.nodes.size() - 1;
    });
    PathStep();
}

void Bot::EndPath(bool arrived, std::string reason) {
    auto& path = impl_->path;
    if (!path.running) return;
    path.running = false;
    path.nodes.clear();
    FinishPath(path.request, arrived, std::move(reason));
}

void Bot::FinishPath(uint64_t request, bool arrived, std::string reason) {
    const Pathfinder::Point here = CurrentTile();
    Update([&](BotSnapshot& s) {
        s.walking = false;
        s.pathLeft = 0;
        s.pathDone = std::max(s.pathDone, request);
        s.pathOk = arrived;
        s.pathError = reason;
    });
    if (arrived) {
        Log::Debug(tag_, "arrived at ({}, {})", here.x, here.y);
        if (Events::Wanted("arrived")) Emit("arrived", { int64_t(here.x), int64_t(here.y) });
    }
    else {
        /* Replacing or stopping a walk is normal (WASD in the panel), not a problem. */
        if (reason == "stopped" || reason.starts_with("replaced")) Log::Debug(tag_, "walk ended: {}", reason);
        else Log::Warn(tag_, "walk cancelled: {}", reason);
        if (Events::Wanted("path_fail")) Emit("path_fail", { reason, int64_t(here.x), int64_t(here.y) });
    }
}

/* Any tile of the rest of the route that is no longer open (a door was
   closed, a block was placed, access was removed, ...). */
bool Bot::RouteBlocked() const {
    const auto& path = impl_->path;
    const auto userID = static_cast<uint32_t>(Read([](const BotSnapshot& s) { return s.userID; }));
    for (size_t i = std::max<size_t>(path.next, 1); i < path.nodes.size(); ++i) {
        const auto from = path.nodes[i - 1], to = path.nodes[i];
        if (Pathfinder::EnterCost(world_, to.x, to.y, to.x - from.x, to.y - from.y, userID) < 0) return true;
    }
    return false;
}

void Bot::PathStep() {
    Impl& d = *impl_;
    auto& path = d.path;
    if (!path.running) return;
    if (!d.connected || !d.inWorld || world_.name != path.world) {
        EndPath(false, "left the world");
        return;
    }

    const Pathfinder::Point here = CurrentTile();
    if (here == path.target) {
        EndPath(true, {});
        return;
    }

    /* Off the route (respawned, sent somewhere by the server, a raw packet)
       or the route closed in front of us: plan again from where we really are. */
    const bool offRoute = path.next == 0 || path.next >= path.nodes.size() || path.nodes[path.next - 1] != here;
    if (offRoute || RouteBlocked()) {
        if (++path.replans > MAX_REPLANS) {
            EndPath(false, "the route kept changing, gave up");
            return;
        }
        const auto userID = static_cast<uint32_t>(Read([](const BotSnapshot& s) { return s.userID; }));
        const auto result = Pathfinder::Find(world_, here, path.target, userID, path.nodes);
        if (result != Pathfinder::Result::Found) {
            EndPath(false, Pathfinder::Describe(result));
            return;
        }
        Log::Debug(tag_, "re-planned from ({}, {}): {} tiles", here.x, here.y, path.nodes.size());
        path.next = 1;
    }

    /* A few tiles back to back, then a random pause. */
    Pathfinder::Point from = here;
    for (int i = 0; i < STEPS_PER_SEND && path.next < path.nodes.size(); ++i) {
        const Pathfinder::Point step = path.nodes[path.next++];
        if (step.x != from.x) d.facingLeft = step.x < from.x;
        WalkTo(step, d.facingLeft);
        from = step;
    }
    const size_t left = path.nodes.size() - path.next;
    Update([&](BotSnapshot& s) { s.pathLeft = left; });
    if (d.autoCollect) CollectNear(d.collectRadius, true);

    if (from == path.target) {
        EndPath(true, {});
        return;
    }
    path.nextStep = Clock::now() + std::chrono::milliseconds(RandomMs(STEP_MIN_MS, STEP_MAX_MS));
}

/* One tile, like Mori's walk(): WALK | STANDING (| FACING_LEFT),
   x = tile * 32, y = tile * 32 + 2. Reliable, unlike Mori: ENet's throttle
   drops unreliable packets, and a lost step reads as a 2-3 tile jump on the
   server (the --selftest fake server caught exactly that). */
void Bot::WalkTo(Pathfinder::Point tile, bool facingLeft) {
    TankPacket state;
    state.type = PACKET_STATE;
    state.flags = STATE_WALK | STATE_STANDING | (facingLeft ? STATE_FACING_LEFT : 0u);
    state.x = Pathfinder::PixelX(tile.x);
    state.y = Pathfinder::PixelY(tile.y);
    state.punchX = -1;
    state.punchY = -1;
    SendTank(state, true);
    Update([&](BotSnapshot& s) {
        s.x = state.x;
        s.y = state.y;
    });
}

/* ---- dropped items ------------------------------------------------------- */
std::vector<WorldObject> Bot::CollectTargets(int radius) const {
    const Pathfinder::Point here = CurrentTile();
    std::vector<WorldObject> nearby;
    ReadWorld([&](const WorldState& world) {
        for (const WorldObject& object : world.objects) {
            if (object.id == 0 || object.count == 0) continue;
            /* The item's centre (they are 16 px wide). */
            const Pathfinder::Point tile = Pathfinder::TileOf(object.x - 2.0f, object.y - 7.0f);
            if (std::abs(tile.x - here.x) <= radius && std::abs(tile.y - here.y) <= radius) nearby.push_back(object);
        }
        return 0;
    });
    if (nearby.empty()) return nearby;

    /* Only what fits: gems always do, a full stack (200) or a full backpack don't. */
    Read([&](const BotSnapshot& s) {
        std::erase_if(nearby, [&](const WorldObject& object) {
            if (object.id == ITEM_GEMS) return false;
            const auto it = s.inventory.find(object.id);
            if (it != s.inventory.end()) return it->second.amount >= 200;
            return s.inventorySize > 0 && s.inventory.size() >= s.inventorySize;
        });
        return 0;
    });
    return nearby;
}

void Bot::CollectNear(int radius, bool quiet) {
    Impl& d = *impl_;
    if (!d.connected || !d.inWorld) return;
    const auto now = Clock::now();
    size_t sent = 0;
    for (const WorldObject& object : CollectTargets(radius)) {
        if (sent >= COLLECT_PER_CALL) break;
        if (const auto it = d.collectAsked.find(object.uid); it != d.collectAsked.end() && now < it->second) continue;
        TankPacket tank;
        tank.type = PACKET_ITEM_ACTIVATE_OBJECT_REQUEST;
        tank.x = object.x;
        tank.y = object.y;
        tank.value = static_cast<int32_t>(object.uid);
        SendTank(tank, true);
        d.collectAsked[object.uid] = now + COLLECT_RETRY;
        ++sent;
    }
    if (d.collectAsked.size() > 512) std::erase_if(d.collectAsked, [&](const auto& entry) { return entry.second <= now; });
    if (sent && !quiet) Log::Debug(tag_, "collecting {} dropped item(s)", sent);
}

/* ---- tile and object updates ---------------------------------------------- */
void Bot::OnTileChange(const TankPacket& tank) {
    /* The server's echo of a punch / place: what actually happened to the tile. */
    const auto value = static_cast<uint16_t>(tank.value);
    if (value == ITEM_WRENCH) return;
    uint16_t fg = 0, bg = 0;
    bool known = false;
    EditWorld([&](WorldState& world) {
        WorldTile* tile = world.Tile(tank.punchX, tank.punchY);
        if (!tile) return;
        if (value == ITEM_FIST) {
            if (tile->fg != 0) tile->ClearForeground();
            else tile->bg = 0;
        }
        else if (ItemDB::IsBackground(value)) tile->bg = value;
        else {
            tile->ClearForeground();
            tile->fg = value;
        }
        world.TileChanged(static_cast<uint32_t>(tank.punchX), static_cast<uint32_t>(tank.punchY));
        fg = tile->fg;
        bg = tile->bg;
        known = true;
    });
    if (known && Events::Wanted("tile_change")) Emit("tile_change", { int64_t(tank.punchX), int64_t(tank.punchY), int64_t(fg), int64_t(bg) });

    /* Our own place: the game takes the block out of the backpack itself. */
    int32_t netID = 0;
    Read([&](const BotSnapshot& s) { netID = s.netID; return 0; });
    if (tank.netID != netID || netID == 0 || value == ITEM_FIST || value == 0) return;
    int amount = -1;
    Update([&](BotSnapshot& s) {
        const auto it = s.inventory.find(value);
        if (it == s.inventory.end()) return;
        amount = it->second.amount > 0 ? it->second.amount - 1 : 0;
        it->second.amount = static_cast<uint8_t>(amount);
        if (amount == 0) s.inventory.erase(it);
    });
    if (amount >= 0 && Events::Wanted("item")) Emit("item", { int64_t(value), int64_t(-1), int64_t(amount) });
}

void Bot::OnTileUpdate(const TankPacket& tank, std::span<const uint8_t> extended) {
    uint16_t fg = 0, bg = 0;
    bool ok = false;
    EditWorld([&](WorldState& world) {
        ok = tank.punchX >= 0 && tank.punchY >= 0 &&
            world.UpdateTile(static_cast<uint32_t>(tank.punchX), static_cast<uint32_t>(tank.punchY), extended);
        if (ok) {
            const WorldTile* tile = world.Tile(tank.punchX, tank.punchY);
            fg = tile->fg;
            bg = tile->bg;
        }
    });
    if (!ok) Log::Debug(tag_, "tile update failed at ({}, {})", tank.punchX, tank.punchY);
    else if (Events::Wanted("tile_change")) Emit("tile_change", { int64_t(tank.punchX), int64_t(tank.punchY), int64_t(fg), int64_t(bg) });
}

void Bot::OnItemChangeObject(const TankPacket& tank) {
    /* netID -1: new drop, -3: stack count changed, > 0: picked up by that
       player (value = object uid). */
    if (!impl_->inWorld) return;
    if (tank.netID == -1) {
        WorldObject object;
        object.id = static_cast<uint16_t>(tank.value);
        object.x = std::ceil(tank.x);
        object.y = std::ceil(tank.y);
        object.count = static_cast<uint8_t>(tank.floatValue);
        object.flags = tank.objectType;
        EditWorld([&](WorldState& world) {
            object.uid = world.nextObjectUID++;
            world.objects.push_back(object);
        });
        if (Events::Wanted("object_drop"))
            Emit("object_drop", { int64_t(object.uid), int64_t(object.id), int64_t(object.count), double(object.x), double(object.y) });
        if (impl_->autoCollect) CollectNear(impl_->collectRadius, true);
        return;
    }
    if (tank.netID == -3) {
        EditWorld([&](WorldState& world) {
            const auto it = std::ranges::find_if(world.objects, [&](const WorldObject& o) {
                return o.id == static_cast<uint16_t>(tank.value) && o.x == std::ceil(tank.x) && o.y == std::ceil(tank.y);
            });
            if (it != world.objects.end()) it->count = static_cast<uint8_t>(tank.floatValue);
        });
        return;
    }
    if (tank.netID <= 0) return;

    std::optional<WorldObject> taken;
    EditWorld([&](WorldState& world) {
        const auto it = std::ranges::find_if(world.objects, [&](const WorldObject& o) { return o.uid == static_cast<uint32_t>(tank.value); });
        if (it == world.objects.end()) return;
        taken = *it;
        world.objects.erase(it);
    });
    impl_->collectAsked.erase(static_cast<uint32_t>(tank.value));
    if (!taken) return;

    int32_t netID = 0;
    Read([&](const BotSnapshot& s) { netID = s.netID; return 0; });
    if (tank.netID != netID) {
        if (Events::Wanted("object_take")) Emit("object_take", { int64_t(tank.netID), int64_t(taken->id), int64_t(taken->count) });
        return;
    }

    /* Ours: the game adds it to the backpack itself (gems come with OnSetBux). */
    int amount = 0;
    if (taken->id != ITEM_GEMS) {
        Update([&](BotSnapshot& s) {
            auto& item = s.inventory[taken->id];
            item.id = taken->id;
            item.amount = static_cast<uint8_t>(std::min(200, item.amount + taken->count));
            amount = item.amount;
        });
        if (Events::Wanted("item")) Emit("item", { int64_t(taken->id), int64_t(taken->count), int64_t(amount) });
    }
    Log::Debug(tag_, "collected {}x {}", taken->count, ItemDB::Name(taken->id));
    if (Events::Wanted("collect")) Emit("collect", { int64_t(taken->id), int64_t(taken->count), int64_t(taken->uid) });
}
