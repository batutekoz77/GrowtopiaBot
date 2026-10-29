#include "Bot.hpp"
#include "../Core/Crypto.hpp"
#include "../Core/Log.hpp"
#include "../Core/Text.hpp"
#include "../Game/Login.hpp"
#include "../Net/Socks5Udp.hpp"

#include "ENET/enet.h"

#include <algorithm>
#include <cstring>
#include <format>

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

    explicit Impl(const std::optional<Net::Socks5Config>& proxy) : http(proxy) {}
};

Bot::Bot(int id, Config::BotEntry entry, const Config::Common& settings)
    : id_(id), entry_(std::move(entry)), settings_(settings), tag_(entry_.growId), impl_(std::make_unique<Impl>(entry_.proxy)) {
    state_.index = id_;
    state_.growId = entry_.growId;
    state_.name = entry_.growId;
    state_.proxy = entry_.proxy ? entry_.proxy->ToString() : std::string();

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
    std::lock_guard lock(mutex_);
    return { static_cast<int>(state_.x / 32.0f), static_cast<int>(state_.y / 32.0f) };
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

void Bot::Walk(int tileX, int tileY) {
    Post([this, tileX, tileY] {
        const auto [x, y] = Position();
        TankPacket tank;
        tank.type = PACKET_STATE;
        tank.x = static_cast<float>(tileX) * 32.0f;
        tank.y = static_cast<float>(tileY) * 32.0f + 2.0f;
        tank.punchX = tank.punchY = -1;
        tank.flags = STATE_WALK | STATE_STANDING | (tank.x < x ? STATE_FACING_LEFT : 0u);
        SendTank(tank, false);
        Update([&](BotSnapshot& s) { s.x = tank.x; s.y = tank.y - 2.0f; });
    });
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

void Bot::Punch(int tileX, int tileY) {
    Post([this, tileX, tileY] { SendTileAction(tileX, tileY, ITEM_FIST, STATE_PUNCH); });
}

void Bot::Place(int tileX, int tileY, uint16_t itemID) {
    Post([this, tileX, tileY, itemID] {
        if (!HasItem(itemID)) {
            Log::Warn(tag_, "place: no item {} in the inventory", itemID);
            return;
        }
        SendTileAction(tileX, tileY, itemID, STATE_PLACE);
    });
}

void Bot::Wrench(int tileX, int tileY) {
    Post([this, tileX, tileY] { SendTileAction(tileX, tileY, ITEM_WRENCH, STATE_PUNCH); });
}

void Bot::Activate(int tileX, int tileY) {
    Post([this, tileX, tileY] {
        const auto [x, y] = Position();
        TankPacket tank;
        tank.type = PACKET_TILE_ACTIVATE_REQUEST;
        tank.x = x;
        tank.y = y;
        tank.punchX = tileX;
        tank.punchY = tileY;
        SendTank(tank, true);
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
void Bot::SendRawTank(const TankPacket& tank, bool reliable) { Post([this, tank, reliable] { SendTank(tank, reliable); }); }

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

    Wait(CONNECTED_WAIT_MS);
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
    uint16_t nameLength = 0;
    uint32_t width = 0, height = 0;
    std::string name;
    if (ReadAt(extended, 6, nameLength) && extended.size() >= 8u + nameLength) {
        name.assign(reinterpret_cast<const char*>(extended.data() + 8), nameLength);
        ReadAt(extended, 8u + nameLength, width);
        ReadAt(extended, 12u + nameLength, height);
    }
    const std::string world = Text::Upper(name);

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
    Log::Info(tag_, "entered {} ({} x {})", world, width, height);
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
    d.inWorld = false;
    d.joinDeadline.reset();
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
