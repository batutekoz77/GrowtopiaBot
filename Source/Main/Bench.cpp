#include "Bench.hpp"
#include "Config.hpp"
#include "../Bot/BotManager.hpp"
#include "../Core/Log.hpp"
#include "../Core/Text.hpp"
#include "../Game/Protocol.hpp"
#include "../Script/ScriptEngine.hpp"
#include "../UI/App.hpp"

#include "ENET/enet.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <format>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace Protocol;
using namespace std::chrono_literals;

namespace {
    using Clock = std::chrono::steady_clock;

    std::string gReport;

    template <typename... Args>
    void Out(std::format_string<Args...> fmt, Args&&... args) {
        std::string line = std::format(fmt, std::forward<Args>(args)...) + "\n";
        std::fputs(line.c_str(), stdout);
        std::fflush(stdout);
        gReport += line;
    }

    void JoinBenchWorld(BotManager& manager) {
        for (const auto& bot : manager.All()) bot->SetTargetWorld("BENCH");
    }

    double CyclesPerSecond() {
        static const double hz = [] {
            LARGE_INTEGER frequency, start, end;
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&start);
            const uint64_t c0 = __rdtsc();
            Sleep(250);
            QueryPerformanceCounter(&end);
            const uint64_t c1 = __rdtsc();
            return static_cast<double>(c1 - c0) * static_cast<double>(frequency.QuadPart) / static_cast<double>(end.QuadPart - start.QuadPart);
        }();
        return hz;
    }

    double ProcessCpu() {
        ULONG64 cycles = 0;
        QueryProcessCycleTime(GetCurrentProcess(), &cycles);
        return static_cast<double>(cycles) / CyclesPerSecond();
    }

    double ThreadCpu(HANDLE thread) {
        ULONG64 cycles = 0;
        QueryThreadCycleTime(thread, &cycles);
        return static_cast<double>(cycles) / CyclesPerSecond();
    }

    using Arg = std::variant<std::string, int32_t>;

    std::vector<uint8_t> Variants(std::initializer_list<Arg> args) {
        std::vector<uint8_t> out{ static_cast<uint8_t>(args.size()) };
        uint8_t index = 0;
        for (const auto& arg : args) {
            out.push_back(index++);
            if (const auto* text = std::get_if<std::string>(&arg)) {
                out.push_back(2);
                const uint32_t length = static_cast<uint32_t>(text->size());
                out.insert(out.end(), reinterpret_cast<const uint8_t*>(&length), reinterpret_cast<const uint8_t*>(&length) + 4);
                out.insert(out.end(), text->begin(), text->end());
            }
            else {
                out.push_back(9);
                const int32_t value = std::get<int32_t>(arg);
                out.insert(out.end(), reinterpret_cast<const uint8_t*>(&value), reinterpret_cast<const uint8_t*>(&value) + 4);
            }
        }
        return out;
    }

    void Send(ENetPeer* peer, const std::vector<uint8_t>& payload, bool reliable = true) {
        enet_peer_send(peer, 0, enet_packet_create(payload.data(), payload.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0));
    }

    void SendCall(ENetPeer* peer, std::initializer_list<Arg> args) {
        TankPacket tank;
        tank.type = PACKET_CALL_FUNCTION;
        tank.netID = -1;
        const auto extended = Variants(args);
        Send(peer, MakeTank(tank, extended));
    }

    class FakeServer {
    public:
        explicit FakeServer(int packetsPerBot) : rate_(packetsPerBot) {}
        ~FakeServer() { Stop(); }
        bool sendInventory = false;

        bool Start() {
            for (uint16_t port = 27091; port < 27191 && !host_; ++port) {
                ENetAddress address{};
                address.type = ENET_ADDRESS_TYPE_IPV4;
                enet_address_set_host_ip(&address, "127.0.0.1");
                address.port = port;
                host_ = enet_host_create(ENET_ADDRESS_TYPE_IPV4, &address, 1024, 2, 0, 0);
                port_ = port;
            }
            if (!host_) return false;
            host_->checksum = enet_crc32;
            enet_host_compress_with_range_coder(host_);
            host_->usingNewPacketForServer = 1;
            thread_ = std::thread([this] { Loop(); });
            return true;
        }

        void Stop() {
            stop_ = true;
            if (thread_.joinable()) thread_.join();
            if (host_) enet_host_destroy(host_);
            host_ = nullptr;
        }

        uint16_t Port() const noexcept { return port_; }
        HANDLE Thread() { return thread_.native_handle(); }
        uint64_t Sent() const noexcept { return sent_; }

        bool WaitMarker(int marker, Clock::time_point& at, std::chrono::milliseconds timeout) {
            std::unique_lock lock(markerMutex_);
            if (!markerCv_.wait_for(lock, timeout, [&] { return markers_.contains(marker); })) return false;
            at = markers_[marker];
            markers_.erase(marker);
            return true;
        }

    private:
        struct Peer { bool inWorld = false; int netID = 0; float x = 0.0f; };

        ENetHost* host_ = nullptr;
        uint16_t port_ = 0;
        std::thread thread_;
        std::atomic<bool> stop_{ false };
        std::atomic<uint64_t> sent_{ 0 };
        int rate_ = 20;
        std::unordered_map<ENetPeer*, Peer> peers_;
        int nextNetID_ = 1;

        std::mutex markerMutex_;
        std::condition_variable markerCv_;
        std::unordered_map<int, Clock::time_point> markers_;

        void OnReceive(ENetPeer* peer, std::span<const uint8_t> data) {
            const int32_t type = MessageOf(data);
            const std::string_view text = TextOf(data);
            if (type == NET_MESSAGE_GENERIC_TEXT) {
                if (Text::Contains(text, "ltoken|")) {
                    SendCall(peer, { std::string("OnSuperMainStartAcceptLogonHrdxs47254722215a"), 0 });
                }
                else if (Text::Contains(text, "action|enter_game")) {
                    SendCall(peer, { std::string("OnRequestWorldSelectMenu"), std::string("default|\n") });
                }
                else if (const auto pos = text.find("lat:"); pos != std::string_view::npos) {
                    const auto now = Clock::now();
                    const int marker = static_cast<int>(Text::ToInt(Text::Trim(text.substr(pos + 4))).value_or(-1));
                    {
                        std::lock_guard lock(markerMutex_);
                        markers_[marker] = now;
                    }
                    markerCv_.notify_all();
                }
            }
            else if (type == NET_MESSAGE_GAME_MESSAGE && Text::Contains(text, "action|join_request")) {
                std::string world = "BENCH";
                if (const auto name = Text::Value(text, "name")) world = std::string(Text::Split(*name, '|')[0]);
                Peer& p = peers_[peer];
                p.netID = nextNetID_++;

                std::vector<uint8_t> map(8, 0);
                const uint16_t length = static_cast<uint16_t>(world.size());
                std::memcpy(map.data() + 6, &length, 2);
                map.insert(map.end(), world.begin(), world.end());
                const uint32_t width = 100, height = 60;
                map.insert(map.end(), reinterpret_cast<const uint8_t*>(&width), reinterpret_cast<const uint8_t*>(&width) + 4);
                map.insert(map.end(), reinterpret_cast<const uint8_t*>(&height), reinterpret_cast<const uint8_t*>(&height) + 4);
                TankPacket tank;
                tank.type = PACKET_SEND_MAP_DATA;
                Send(peer, MakeTank(tank, map));
                SendCall(peer, { std::string("OnSpawn"), std::format("spawn|avatar\nnetID|{}\nuserID|{}\nposXY|320|320\nname|bench{}\ntype|local\n", p.netID, p.netID, p.netID) });
                SendCall(peer, { std::string("OnSpawn"), std::string("spawn|avatar\nnetID|999999\nuserID|7\nposXY|0|320\nname|walker\ncountry|us\n") });
                if (sendInventory) {
                    const struct { uint16_t id; uint8_t amount, flags; } items[] = { { 2, 200, 0 }, { 3, 87, 0 }, { 14, 12, 0 }, { 48, 1, 1 }, { 242, 35, 0 } };
                    std::vector<uint8_t> inventory(7, 0);
                    const uint32_t slots = 36;
                    const uint16_t count = static_cast<uint16_t>(std::size(items));
                    std::memcpy(inventory.data() + 1, &slots, 4);
                    std::memcpy(inventory.data() + 5, &count, 2);
                    for (const auto& item : items) {
                        inventory.insert(inventory.end(), { static_cast<uint8_t>(item.id & 0xff), static_cast<uint8_t>(item.id >> 8), item.amount, item.flags });
                    }
                    TankPacket inv;
                    inv.type = PACKET_SEND_INVENTORY_STATE;
                    Send(peer, MakeTank(inv, inventory));
                }
                p.inWorld = true;
            }
        }

        void Traffic(int perPeer) {
            for (auto& [peer, p] : peers_) {
                if (!p.inWorld) continue;
                for (int i = 0; i < perPeer; ++i) {
                    TankPacket state;
                    state.type = PACKET_STATE;
                    state.netID = 999999;
                    p.x = p.x > 3000.0f ? 0.0f : p.x + 4.0f;
                    state.x = p.x;
                    state.y = 320.0f;
                    state.flags = STATE_WALK | STATE_STANDING;
                    Send(peer, MakeTank(state), false);
                    ++sent_;
                }
            }
        }

        void Pings() {
            for (auto& [peer, p] : peers_) {
                if (!p.inWorld) continue;
                TankPacket ping;
                ping.type = PACKET_PING_REQUEST;
                ping.value = 12345;
                Send(peer, MakeTank(ping));
                SendCall(peer, { std::string("OnConsoleMessage"), std::string("`oBench `2tick``") });
                sent_ += 2;
            }
        }

        void Loop() {
            const auto tick = 50ms;
            auto nextTick = Clock::now() + tick;
            auto nextPing = Clock::now() + 1s;
            double carry = 0.0;
            while (!stop_) {
                ENetEvent event{};
                while (enet_host_service(host_, &event, 1) > 0) {
                    switch (event.type) {
                        case ENET_EVENT_TYPE_CONNECT: {
                            peers_[event.peer] = {};
                            const std::vector<uint8_t> hello{ 1, 0, 0, 0, 0 };
                            Send(event.peer, hello);
                            break;
                        }
                        case ENET_EVENT_TYPE_RECEIVE:
                            OnReceive(event.peer, { event.packet->data, event.packet->dataLength });
                            enet_packet_destroy(event.packet);
                            break;
                        case ENET_EVENT_TYPE_DISCONNECT:
                            peers_.erase(event.peer);
                            break;
                        default: break;
                    }
                    if (stop_) break;
                }
                const auto now = Clock::now();
                if (now >= nextTick) {
                    carry += rate_ * 0.05;
                    const int count = static_cast<int>(carry);
                    carry -= count;
                    if (count > 0) Traffic(count);
                    nextTick += tick;
                }
                if (now >= nextPing) {
                    Pings();
                    nextPing += 1s;
                }
                enet_host_flush(host_);
            }
        }
    };

    struct LoopResult {
        double cpuPercentOfCore = 0.0;
        double wakeupsPerBot = 0.0;
        double packetsPerSecond = 0.0;
        double latencyAvgMs = 0.0, latencyP99Ms = 0.0, latencyMaxMs = 0.0;
        size_t slow = 0, samples = 0;
        int inWorld = 0;
        double loginMs = 0.0;
    };

    LoopResult NetworkPhase(const Config::Settings& settings, bool legacy, int seconds, FakeServer& server,
        const std::function<void(BotManager&)>& extra) {
        LoopResult r;
        Bot::SetLegacyWait(legacy);
        BotManager manager(settings);
        const auto bots = manager.All();
        JoinBenchWorld(manager);

        const auto loginStart = Clock::now();
        manager.StartAll();
        const auto loginDeadline = loginStart + 30s;
        for (;;) {
            r.inWorld = static_cast<int>(std::ranges::count_if(bots, [](const auto& b) { return b->Status() == BotStatus::InWorld; }));
            if (r.inWorld == static_cast<int>(bots.size()) || Clock::now() > loginDeadline) break;
            std::this_thread::sleep_for(10ms);
        }
        r.loginMs = std::chrono::duration<double, std::milli>(Clock::now() - loginStart).count();
        if (r.inWorld != static_cast<int>(bots.size())) {
            std::map<std::string, int> states;
            for (const auto& b : bots) ++states[std::string(StatusName(b->Status())) + " / " + b->Read([](const BotSnapshot& s) { return s.statusText; })];
            for (const auto& [state, count] : states) Out("    !! {} bots: {}", count, state);
            for (const auto& e : Log::Since(0)) if (e.level != Log::Level::Info) Out("    log [{}] {}", e.tag, e.message);
        }
        std::this_thread::sleep_for(1s);

        uint64_t wakeups0 = 0, packets0 = 0;
        for (const auto& b : bots) { wakeups0 += b->Counters().wakeups; packets0 += b->Counters().packets; }
        const double cpu0 = ProcessCpu() - ThreadCpu(server.Thread());
        const auto t0 = Clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        const double cpu1 = ProcessCpu() - ThreadCpu(server.Thread());
        const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
        uint64_t wakeups1 = 0, packets1 = 0;
        for (const auto& b : bots) { wakeups1 += b->Counters().wakeups; packets1 += b->Counters().packets; }

        r.cpuPercentOfCore = (cpu1 - cpu0) / wall * 100.0;
        r.wakeupsPerBot = static_cast<double>(wakeups1 - wakeups0) / wall / static_cast<double>(bots.size());
        r.packetsPerSecond = static_cast<double>(packets1 - packets0) / wall;

        std::vector<double> samples;
        static int marker = 0;
        for (int i = 0; i < 300; ++i) {
            const auto& bot = bots[static_cast<size_t>(i) % bots.size()];
            const int m = ++marker;
            const auto sent = Clock::now();
            bot->Say("lat:" + std::to_string(m));
            Clock::time_point seen;
            if (server.WaitMarker(m, seen, 500ms)) samples.push_back(std::chrono::duration<double, std::milli>(seen - sent).count());
            else if (i - static_cast<int>(samples.size()) > 10) break;
            std::this_thread::sleep_for(std::chrono::microseconds(1500 + (i * 7919) % 3000));
        }
        if (!samples.empty()) {
            std::ranges::sort(samples);
            r.latencyAvgMs = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
            r.latencyP99Ms = samples[std::min(samples.size() - 1, samples.size() * 99 / 100)];
            r.latencyMaxMs = samples.back();
            r.samples = samples.size();
            r.slow = static_cast<size_t>(std::ranges::count_if(samples, [](double ms) { return ms > 5.0; }));
        }

        if (extra) extra(manager);
        manager.StopAll();
        return r;
    }

    void PrintLoop(const char* name, const LoopResult& r, int bots) {
        Out("  [{}]", name);
        Out("    logged in + in world : {}/{} bots in {:.0f} ms", r.inWorld, bots, r.loginMs);
        Out("    CPU (bot threads)    : {:.2f} % of one core  ({:.3f} % per bot)", r.cpuPercentOfCore, r.cpuPercentOfCore / bots);
        Out("    loop wake-ups        : {:.0f} /s per bot", r.wakeupsPerBot);
        Out("    packets handled      : {:.0f} /s", r.packetsPerSecond);
        Out("    command latency      : avg {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms ({}/{} samples > 5 ms)", r.latencyAvgMs, r.latencyP99Ms, r.latencyMaxMs, r.slow, r.samples);
    }

    std::vector<std::string> RunScript(ScriptEngine& engine, const std::string& name, const std::string& code, int bot, std::chrono::seconds timeout,
        bool trace = false) {
        const uint64_t before = engine.OutputSince(0).empty() ? 0 : engine.OutputSince(0).back().seq;
        const int id = engine.Run(name, code, bot, false, trace);
        const auto deadline = Clock::now() + timeout;
        for (;;) {
            const auto list = engine.List();
            const auto it = std::ranges::find(list, id, &ScriptEngine::Info::id);
            if (it == list.end() || !it->running) {
                if (it != list.end() && it->failed) Out("    script error: {}", it->error);
                break;
            }
            if (Clock::now() > deadline) {
                engine.Stop(id);
                Out("    (script timed out)");
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
        std::vector<std::string> lines;
        for (const auto& line : engine.OutputSince(before)) {
            if (line.script == id && line.text.starts_with("BENCH")) lines.push_back(line.text.substr(6));
        }
        return lines;
    }

    constexpr const char* LUA_BENCH = R"lua(
local bot = getBot()
local function measure(name, n, fn)
    local t0 = os.clock()
    fn(n)
    local dt = os.clock() - t0
    print(string.format("BENCH %-28s %10.0f ops/s  (%.3f us/op)", name, n / dt, dt / n * 1e6))
end
measure("bot.gems (property)", 1000000, function(n) local g = 0 for i = 1, n do g = g + bot.gems end end)
measure("bot.world (string property)", 500000, function(n) for i = 1, n do local w = bot.world end end)
measure("bot:getItemCount(2)", 500000, function(n) for i = 1, n do local c = bot:getItemCount(2) end end)
measure("bot:getInventory()", 100000, function(n) for i = 1, n do local inv = bot:getInventory() end end)
measure("bot:getPlayers()", 100000, function(n) for i = 1, n do local p = bot:getPlayers() end end)
measure("getBot()", 500000, function(n) for i = 1, n do local b = getBot() end end)
measure("bot:sendText (queued cmd)", 200000, function(n) for i = 1, n do bot:sendText("action|bench\n") end end)
measure("plain Lua loop (baseline)", 10000000, function(n) local x = 0 for i = 1, n do x = x + i end end)
)lua";

    constexpr const char* TRACE_BENCH = R"lua(
local n = 3000000
local t0 = os.clock()
local x = 0
for i = 1, n do
    x = x + i
    x = x - 1
end
local dt = os.clock() - t0
print(string.format("BENCH %10.0f loop rounds/s  (%.3f us/round, 3 lines each)", n / dt, dt / n * 1e6))
)lua";

    constexpr const char* EVENT_BENCH = R"lua(
local count, firstAt, lastAt = 0, 0, 0
on("bench_event", function(bot, n)
    if count == 0 then firstAt = millis() end
    count = count + 1
    lastAt = millis()
end)
print("BENCH ready")
local last, idle = -1, 0
while true do
    sleep(20)
    if count > 0 and count == last then idle = idle + 1 else idle = 0 end
    last = count
    if idle >= 10 then break end
end
local ms = math.max(lastAt - firstAt, 1)
print(string.format("BENCH handled %d events in Lua in %d ms (%.0f/s)", count, ms, count / ms * 1000))
)lua";
}

int Bench::Run(int argc, char** argv) {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD type = out && out != INVALID_HANDLE_VALUE ? GetFileType(out) : FILE_TYPE_UNKNOWN;
    if (type != FILE_TYPE_PIPE && type != FILE_TYPE_DISK && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
    }
    const int botCount = argc > 2 ? std::max(1, std::atoi(argv[2])) : 50;
    const int seconds = argc > 3 ? std::max(1, std::atoi(argv[3])) : 10;
    const int rate = argc > 4 ? std::max(0, std::atoi(argv[4])) : 20;

    if (enet_initialize() != 0) {
        Out("enet_initialize failed");
        return 1;
    }
    Net::Startup();

    Out("GrowtopiaBot benchmark: {} bots, {} s per phase, {} packets/s per bot of world traffic, {} cores",
        botCount, seconds, rate, std::thread::hardware_concurrency());
    Out("(local fake ENet server; its own CPU is subtracted)\n");

    FakeServer server(rate);
    if (!server.Start()) {
        Out("couldn't start the fake server");
        return 1;
    }

    Config::Settings settings;
    settings.loginIntervalMs = 0;
    settings.benchServer = std::format("127.0.0.1:{}", server.Port());
    for (int i = 0; i < botCount; ++i) {
        Config::BotEntry entry;
        entry.growId = std::format("bench{}", i + 1);
        entry.password = "x";
        settings.bots.push_back(entry);
    }

    Out("1) Bot network loop");
    const LoopResult legacy = NetworkPhase(settings, true, seconds, server, {});
    PrintLoop("before: fixed 5 ms select poll", legacy, botCount);
    const LoopResult current = NetworkPhase(settings, false, seconds, server, [&](BotManager& manager) {
        Out("\n2) Lua API (one script, bot in a world)");
        ScriptEngine engine(manager);
        for (const auto& line : RunScript(engine, "bench", LUA_BENCH, manager.All().front()->Id(), 120s)) Out("    {}", line);
        for (const bool trace : { false, true }) {
            for (const auto& line : RunScript(engine, "tracebench", TRACE_BENCH, 0, 120s, trace))
                Out("    {:<28} {}", trace ? "line tracing on (editor)" : "line tracing off", line);
        }

        Out("\n3) Event dispatch (bot thread -> script handler)");
        const int id = engine.Run("events", EVENT_BENCH, 0);
        for (int i = 0; i < 400; ++i) {
            if (Events::Wanted("bench_event")) break;
            std::this_thread::sleep_for(5ms);
        }
        const auto t0 = Clock::now();
        Events::Event event{ manager.All().front()->Id(), "bench_event", { int64_t(1) } };
        const int total = 200000;
        double emitMs = 0.0;
        for (int burst = 0; burst < total / 10000; ++burst) {
            const auto e0 = Clock::now();
            for (int i = 0; i < 10000; ++i) Events::Emit(event);
            emitMs += std::chrono::duration<double, std::milli>(Clock::now() - e0).count();
            std::this_thread::sleep_for(25ms);
        }
        Out("    {} events, Emit() cost {:.0f} ns each", total, emitMs * 1e6 / total);
        uint64_t dropped = 0;
        for (;;) {
            const auto list = engine.List();
            const auto it = std::ranges::find(list, id, &ScriptEngine::Info::id);
            if (it == list.end() || !it->running) {
                if (it != list.end()) dropped = it->dropped;
                break;
            }
            if (Clock::now() - t0 > 60s) {
                engine.Stop(id);
                Out("    event bench timed out");
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
        for (const auto& line : engine.OutputSince(0)) {
            if (line.script == id && line.text.starts_with("BENCH handled")) Out("    {}, {} dropped by the queue cap", line.text.substr(6), dropped);
        }
        Out("    Events::Wanted() with nobody listening: ");
        {
            const auto w0 = Clock::now();
            int hits = 0;
            for (int i = 0; i < 10000000; ++i) hits += Events::Wanted("nobody_listens") ? 1 : 0;
            const double ns = std::chrono::duration<double, std::nano>(Clock::now() - w0).count() / 1e7;
            Out("      {:.2f} ns per check ({} hits)", ns, hits);
        }
        engine.StopAll();
    });
    Out("");
    PrintLoop("after: wake socket + 50 ms select", current, botCount);

    Out("\n4) Wake-up correctness (offline bots sleep 250 ms: a missed wake shows as >= 200 ms)");
    {
        Config::Settings offline = settings;
        offline.bots.resize(std::min<size_t>(offline.bots.size(), 8));
        BotManager manager(offline);
        const auto bots = manager.All();
        for (const auto& b : bots) { b->Connect(); b->Disconnect(); }
        for (int i = 0; i < 500 && !std::ranges::all_of(bots, [](const auto& b) { return b->Status() == BotStatus::Offline; }); ++i)
            std::this_thread::sleep_for(10ms);

        std::atomic<int> missed{ 0 }, total{ 0 };
        std::atomic<int64_t> worstUs{ 0 };
        std::vector<std::thread> posters;
        for (int t = 0; t < 4; ++t) {
            posters.emplace_back([&, t] {
                for (int i = 0; i < 1500; ++i) {
                    const auto& bot = bots[static_cast<size_t>(t * 2 + (i & 1)) % bots.size()];
                    const uint64_t before = bot->Counters().commands;
                    const auto sent = Clock::now();
                    bot->Respawn();
                    const auto gapUntil = Clock::now() + std::chrono::microseconds((i * 37 + t * 11) % 60);
                    while (Clock::now() < gapUntil) {}
                    bot->Respawn();
                    while (bot->Counters().commands < before + 2) std::this_thread::yield();
                    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - sent).count();
                    ++total;
                    if (us >= 200'000) ++missed;
                    for (int64_t w = worstUs; us > w && !worstUs.compare_exchange_weak(w, us);) {}
                }
            });
        }
        for (auto& t : posters) t.join();
        Out("    {} commands from 4 threads: {} missed wakes, worst {:.2f} ms", total.load(), missed.load(), worstUs / 1000.0);
        manager.StopAll();
    }

    Out("\n5) Log throughput (4 threads, console off)");
    {
        Log::EnableConsole(false);
        const int perThread = 100000;
        const auto t0 = Clock::now();
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) threads.emplace_back([t] { for (int i = 0; i < perThread; ++i) Log::Info("BENCH", "line {} from {}", i, t); });
        for (auto& t : threads) t.join();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        Out("    {} lines in {:.1f} ms = {:.0f} lines/s", perThread * 4, ms, perThread * 4 / ms * 1000.0);
    }

    Out("\nSummary (bot loop, {} bots):", botCount);
    Out("  CPU      : {:.2f} % -> {:.2f} % of a core ({:+.0f} %)", legacy.cpuPercentOfCore, current.cpuPercentOfCore,
        legacy.cpuPercentOfCore > 0 ? (current.cpuPercentOfCore / legacy.cpuPercentOfCore - 1.0) * 100.0 : 0.0);
    Out("  wake-ups : {:.0f} -> {:.0f} per bot per second", legacy.wakeupsPerBot, current.wakeupsPerBot);
    Out("  latency  : avg {:.2f} -> {:.2f} ms, p99 {:.2f} -> {:.2f} ms", legacy.latencyAvgMs, current.latencyAvgMs, legacy.latencyP99Ms, current.latencyP99Ms);

    server.Stop();
    enet_deinitialize();
    std::ofstream(Config::ExeDirectory() / "bench_result.txt", std::ios::binary) << gReport;
    return 0;
}

int Bench::RunUi(int argc, char** argv) {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD type = out && out != INVALID_HANDLE_VALUE ? GetFileType(out) : FILE_TYPE_UNKNOWN;
    if (type != FILE_TYPE_PIPE && type != FILE_TYPE_DISK && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
    }
    const int botCount = argc > 2 ? std::max(1, std::atoi(argv[2])) : 100;
    const int seconds = argc > 3 ? std::max(2, std::atoi(argv[3])) : 8;
    const std::string tab = argc > 4 ? argv[4] : "";
    const std::string script = argc > 5 ? argv[5] : "";
    if (enet_initialize() != 0) return 1;
    Net::Startup();

    FakeServer server(20);
    server.sendInventory = true;
    if (!server.Start()) return 1;
    Config::Settings settings;
    settings.loginIntervalMs = 0;
    settings.benchServer = std::format("127.0.0.1:{}", server.Port());
    for (int i = 0; i < botCount; ++i) settings.bots.push_back({ std::format("bench{}", i + 1), "x", std::nullopt, "" });

    Out("GrowtopiaBot panel benchmark: {} live bots, {} s per run (main thread CPU only)", botCount, seconds);
    {
        BotManager manager(settings);
        ScriptEngine scripts(manager);
        JoinBenchWorld(manager);
        manager.StartAll();
        std::this_thread::sleep_for(1500ms);
        for (const bool throttle : { false, true }) {
            App app(manager, scripts);
            const double cpu0 = ThreadCpu(GetCurrentThread());
            app.Run({ seconds, throttle, tab, script });
            const double cpu = ThreadCpu(GetCurrentThread()) - cpu0;
            const double frames = static_cast<double>(std::max<uint64_t>(app.Frames(), 1));
            ULONG64 endCycles = 0;
            QueryThreadCycleTime(GetCurrentThread(), &endCycles);
            const double steadyWall = std::chrono::duration<double>(Clock::now() - app.FirstFrameAt()).count();
            const double steadyCpu = static_cast<double>(endCycles - app.FirstFrameCycles()) / CyclesPerSecond();
            const double startup = cpu - steadyCpu;
            Out("  [{}] {:.1f} fps, panel CPU {:.2f} % of one core (startup {:.0f} ms once); per frame UI {:.3f} ms + DirectX {:.3f} ms",
                throttle ? "idle throttle on " : "idle throttle off", (frames - 1) / steadyWall, steadyCpu / steadyWall * 100.0, startup * 1000.0,
                app.UiCycles() / CyclesPerSecond() * 1000.0 / frames, app.GpuCycles() / CyclesPerSecond() * 1000.0 / frames);
        }
        manager.StopAll();
    }
    server.Stop();
    enet_deinitialize();
    std::ofstream(Config::ExeDirectory() / "bench_ui_result.txt", std::ios::binary) << gReport;
    return 0;
}

namespace {
    constexpr const char* SELFTEST = R"lua(
local passed, failed = 0, 0
local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    print(string.format("TEST %s %s%s", ok and "PASS" or "FAIL", name, detail and ("  (" .. tostring(detail) .. ")") or ""))
end

local bot = getBot()
check("getBot()", bot ~= nil, tostring(bot))
check("getBots()", #getBots() == 2, #getBots())
check("getBot(2)", getBot(2) ~= nil and getBot(2).growid == "bench2")
check("getBot(name)", getBot("BENCH2") == getBot(2))
check("bot == getBotById(id)", getBotById(bot.id) == bot)
check("waitForOnline", bot:waitForOnline(10000), bot.status)
check("waitForWorld", bot:waitForWorld(nil, 10000), bot.world)
check("world property", bot.world == "BENCH", bot.world)
check("inWorld/isInWorld", bot.inWorld and bot:isInWorld("bench"))
check("online", bot.online)
check("name", bot.name ~= "", bot.name)
check("netID", bot.netID > 0, bot.netID)
check("tile", bot.tileX == 10 and bot.tileY == 10, bot.tileX .. "," .. bot.tileY)
check("ping is number", type(bot.ping) == "number")
local info = bot:getInfo()
check("getInfo()", info.growid == bot.growid and info.world == "BENCH" and info.status == "in_world")
check("unknown property -> nil", bot.doesNotExist == nil)

local players = bot:getPlayers()
check("getPlayers()", #players == 1 and players[1].name == "walker", #players)
check("getPlayer(name)", bot:getPlayer("WALKER") ~= nil)
check("getPlayer(netID)", bot:getPlayer(999999) ~= nil)
check("getInventory() empty", #bot:getInventory() == 0)
check("hasItem false", not bot:hasItem(2))
check("drop without items -> false", bot:drop(2, 1) == false)

-- events: console arrives every second from the fake server
local b, text = wait("console", 3000)
check("wait('console')", b == bot or b ~= nil, text)
local seen = 0
local id = on("console", function(who, t) seen = seen + 1 end)
sleep(2200)
check("on('console') handler", seen >= 2, seen)
check("off(id)", off(id) == 1)
local before = seen
sleep(1200)
check("handler removed", seen == before)
local mine = 0
bot:on("packet", function(...) mine = mine + 1 end)
sleep(300)
check("bot:on('packet')", mine > 0, mine)
off("packet")

-- actions (queued; fake server answers join_request with the map)
check("warpAndWait", bot:warpAndWait("OTHERWORLD", 5000), bot.world)
check("warp with door", (function() bot:warp("THIRD", "DOOR1") return bot:waitForWorld("THIRD", 5000) end)())
bot:say("hello")
bot:walk(12, 10)
sleep(100)
check("walk updates tile", bot.tileX == 12, bot.tileX)
bot:move(-1, 0)
sleep(100)
check("move", bot.tileX == 11, bot.tileX)
bot:punch(11, 11) bot:place(11, 11, 2) bot:wrench(11, 11) bot:enter(11, 11)
bot:wear(48) bot:respawn() bot:leave()
bot:sendText("action|bench\n") bot:sendGameMessage("action|bench\n")
bot:sendPacket({ type = 0, x = 320, y = 320, flags = 0x20 })
bot:dialogReturn("test", { a = 1 })
bot:dialogReturn("test", { { "b", 2 } })
bot:setTargetWorld("BENCH")
check("targetWorld", bot.targetWorld == "BENCH")
bot:setAutoReconnect(true)
check("autoReconnect", bot.autoReconnect == true)

-- lifecycle
bot:disconnect()
check("disconnect -> offline", bot:waitForStatus("offline", 5000), bot.status)
check("online false when offline", not bot.online)
bot:connect()
check("connect -> online again", bot:waitForOnline(10000), bot.status)
check("rejoins target world", bot:waitForWorld("BENCH", 10000), bot.world)
bot:reconnect()
sleep(200)
check("reconnect -> online again", bot:waitForOnline(10000), bot.status)

-- misc
check("millis", millis() > 0)
check("scriptName", scriptName() == "selftest")
check("pcall still works", select(1, pcall(error, "x")) == false)
check("xpcall still works", select(1, xpcall(function() error("y") end, function(e) return e end)) == false)
check("pcall returns values", select(2, pcall(function() return 42 end)) == 42)
check("getScripts", #getScripts() >= 1)
local nb, err = addBot("", "")
check("addBot validates", nb == nil and err ~= nil, err)
print(string.format("TEST RESULT %d passed, %d failed", passed, failed))
)lua";

    constexpr const char* UNSTOPPABLE = R"lua(
while true do pcall(sleep, 50) end
)lua";
}

int Bench::RunTests(int argc, char** argv) {
    (void)argc; (void)argv;
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD type = out && out != INVALID_HANDLE_VALUE ? GetFileType(out) : FILE_TYPE_UNKNOWN;
    if (type != FILE_TYPE_PIPE && type != FILE_TYPE_DISK && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
    }
    if (enet_initialize() != 0) return 1;
    Net::Startup();
    FakeServer server(20);
    if (!server.Start()) return 1;

    Config::Settings settings;
    settings.loginIntervalMs = 0;
    settings.benchServer = std::format("127.0.0.1:{}", server.Port());
    settings.bots.push_back({ "bench1", "x", std::nullopt, "" });
    settings.bots.push_back({ "bench2", "x", std::nullopt, "" });

    int failures = 0;
    {
        BotManager manager(settings);
        ScriptEngine engine(manager);
        JoinBenchWorld(manager);
        manager.StartAll();

        const int id = engine.Run("selftest", SELFTEST, manager.All().front()->Id());
        const auto deadline = Clock::now() + 90s;
        for (;;) {
            const auto list = engine.List();
            const auto it = std::ranges::find(list, id, &ScriptEngine::Info::id);
            if (it == list.end() || !it->running) {
                if (it != list.end() && it->failed) { Out("SCRIPT ERROR: {}", it->error); ++failures; }
                break;
            }
            if (Clock::now() > deadline) { Out("TIMEOUT"); ++failures; engine.Stop(id); break; }
            std::this_thread::sleep_for(20ms);
        }
        for (const auto& line : engine.OutputSince(0)) {
            if (line.script != id || !line.text.starts_with("TEST")) continue;
            Out("{}", line.text.substr(5));
            if (line.text.starts_with("TEST FAIL")) ++failures;
        }

        {
            constexpr const char* TRACE = "local total = 0\n"
                                          "for i = 1, 3 do\n"
                                          "    total = total + i\n"
                                          "end\n"
                                          "local function add(a)\n"
                                          "    return a + 1\n"
                                          "end\n"
                                          "total = add(total)\n"
                                          "sleep(1000)\n"
                                          "for i = 1, 40 do\n"
                                          "    sleep(30)\n"
                                          "    local x = i\n"
                                          "end\n"
                                          "error(\"boom\")\n";
            using E = ScriptEngine;
            const auto check = [&](const char* name, bool ok, const std::vector<uint8_t>& lines) {
                std::string states;
                for (const uint8_t s : lines) states += static_cast<char>('0' + s);
                Out("{} {}  (lines {})", ok ? "PASS" : "FAIL", name, states);
                if (!ok) ++failures;
            };
            const auto at = [](const std::vector<uint8_t>& lines, int n) { return n >= 1 && n <= static_cast<int>(lines.size()) ? lines[static_cast<size_t>(n - 1)] : 255; };
            const auto start = Clock::now();
            const int traced = engine.Run("trace.lua", TRACE, 0, false, true);
            const int plain = engine.Run("plain.lua", TRACE, 0);
            std::vector<uint8_t> lines;

            std::this_thread::sleep_until(start + 400ms);
            engine.Lines(traced, lines);
            check("trace: sleeping line is current, earlier lines done", at(lines, 9) == E::LineCurrent && at(lines, 1) == E::LineDone &&
                at(lines, 3) == E::LineDone && at(lines, 6) == E::LineDone && at(lines, 8) == E::LineDone && at(lines, 11) == E::LineNone, lines);

            std::this_thread::sleep_until(start + 1700ms);
            engine.Lines(traced, lines);
            check("trace: loop iteration starts uncoloured", at(lines, 11) == E::LineCurrent && at(lines, 10) == E::LineDone &&
                at(lines, 12) == E::LineNone && at(lines, 9) == E::LineDone, lines);

            while (Clock::now() - start < 10s) {
                const auto list = engine.List();
                const auto it = std::ranges::find(list, traced, &ScriptEngine::Info::id);
                if (it == list.end() || !it->running) break;
                std::this_thread::sleep_for(20ms);
            }
            engine.Lines(traced, lines);
            check("trace: error line is red", at(lines, 14) == E::LineError && at(lines, 11) == E::LineDone && at(lines, 12) == E::LineDone, lines);
            engine.Lines(plain, lines);
            check("trace: untraced script records nothing", std::ranges::all_of(lines, [](uint8_t s) { return s == E::LineNone; }), lines);
        }

        const int loop = engine.Run("unstoppable", UNSTOPPABLE, 0);
        std::this_thread::sleep_for(300ms);
        const auto t0 = Clock::now();
        engine.Stop(loop);
        bool stopped = false;
        while (Clock::now() - t0 < 3s) {
            const auto list = engine.List();
            const auto it = std::ranges::find(list, loop, &ScriptEngine::Info::id);
            if (it == list.end() || !it->running) { stopped = true; break; }
            std::this_thread::sleep_for(5ms);
        }
        Out("{} stop() ends a `pcall(sleep)` loop ({:.0f} ms)", stopped ? "PASS" : "FAIL",
            std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        if (!stopped) ++failures;

        engine.StopAll();
        manager.StopAll();
    }
    server.Stop();
    enet_deinitialize();
    Out("{}", failures == 0 ? "ALL TESTS PASSED" : std::format("{} FAILURE(S)", failures));
    std::ofstream(Config::ExeDirectory() / "selftest_result.txt", std::ios::binary) << gReport;
    return failures == 0 ? 0 : 1;
}
