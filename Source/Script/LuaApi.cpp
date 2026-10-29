#include "ScriptRuntime.hpp"
#include "../Bot/BotManager.hpp"
#include "../Core/Text.hpp"

#include "Lua/lua.h"
#include "Lua/lualib.h"
#include "Lua/lauxlib.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <unordered_map>

using namespace std::chrono_literals;

namespace {
    constexpr const char* BOT_META = "GrowtopiaBot.Bot";
    constexpr const char* BOT_CACHE = "GrowtopiaBot.BotCache";
    constexpr int HOOK_INSTRUCTIONS = 10000;
    constexpr size_t QUEUE_LIMIT = 20000;
    constexpr int DEFAULT_WAIT_MS = 30000;

    Script& Self(lua_State* L) { return **static_cast<Script**>(lua_getextraspace(L)); }

    [[noreturn]] void Raise(lua_State* L, const char* message) {
        luaL_error(L, "%s", message);
        std::unreachable();
    }

    void CheckStop(lua_State* L) {
        Script& s = Self(L);
        if (s.exitRequested) Raise(L, "script exit");
        if (s.stop.load(std::memory_order_relaxed)) Raise(L, "script stopped");
    }

    using LineState = ScriptEngine::LineState;
    void Hook(lua_State* L, lua_Debug* ar);

    void SetTracing(lua_State* L, Script& s, bool on) {
        s.tracing = on;
        s.everTraced |= on;
        if (!on) s.frames.clear();
        lua_sethook(L, Hook, LUA_MASKCOUNT | (on ? LUA_MASKLINE : 0), HOOK_INSTRUCTIONS);
    }

    void TraceLine(lua_State* L, lua_Debug* ar, Script& s) {
        if (!lua_getinfo(L, "S", ar)) return;
        if (ar->source != s.chunkSource) {
            if (s.chunkSource || std::strcmp(ar->source, ("@" + s.name).c_str()) != 0) return;
            s.chunkSource = ar->source;
        }
        const int line = ar->currentline;
        if (line < 1 || line > s.lineCount) return;
        const auto done = [&](int n) { if (s.GetLine(n) == LineState::LineCurrent) s.SetLine(n, LineState::LineDone); };

        auto& frames = s.frames;
        size_t i = frames.size();
        while (i > 0 && frames[i - 1].ci != ar->i_ci) --i;
        if (i == 0) {
            frames.push_back({ ar->i_ci, ar->linedefined, 0 });
            i = frames.size();
        }
        while (frames.size() > i) {
            s.SettleLoop(frames.back());
            done(frames.back().line);
            frames.pop_back();
        }
        Script::Frame& frame = frames.back();
        if (frame.defined != ar->linedefined) {
            s.SettleLoop(frame);
            done(frame.line);
            frame = { ar->i_ci, ar->linedefined, 0 };
        }
        if (frame.resetTo != 0) {
            if (line <= frame.resetFrom || line > frame.resetTo) s.SettleLoop(frame);
            frame.resetTo = 0;
        }
        if (frame.line >= line) {
            frame.resetFrom = line;
            frame.resetTo = frame.line;
            frame.saved.resize(static_cast<size_t>(frame.line - line + 1));
            for (int n = line; n <= frame.line; ++n) {
                const uint8_t state = s.GetLine(n);
                frame.saved[static_cast<size_t>(n - line)] = state == LineState::LineCurrent ? LineState::LineDone : state;
                if (state != LineState::LineError) s.SetLine(n, LineState::LineNone);
            }
        }
        else done(frame.line);
        frame.line = line;
        s.SetLine(line, LineState::LineCurrent);
    }

    void Hook(lua_State* L, lua_Debug* ar) {
        Script& s = Self(L);
        if (s.trace.load(std::memory_order_relaxed) != s.tracing) SetTracing(L, s, !s.tracing);
        if (ar->event == LUA_HOOKLINE) {
            if (s.tracing) TraceLine(L, ar, s);
            return;
        }
        CheckStop(L);
    }

    int Traceback(lua_State* L) {
        const char* message = lua_tostring(L, 1);
        if (!message) message = luaL_tolstring(L, 1, nullptr);
        luaL_traceback(L, L, message, 1);
        return 1;
    }

    std::string JoinArgs(lua_State* L, int from = 1) {
        std::string out;
        const int top = lua_gettop(L);
        for (int i = from; i <= top; ++i) {
            size_t length = 0;
            const char* text = luaL_tolstring(L, i, &length);
            if (i > from) out += '\t';
            out.append(text, length);
            lua_pop(L, 1);
        }
        return out;
    }

    struct BotHandle { std::shared_ptr<Bot> bot; };

    void PushBot(lua_State* L, std::shared_ptr<Bot> bot) {
        if (!bot) {
            lua_pushnil(L);
            return;
        }
        const int id = bot->Id();
        luaL_getsubtable(L, LUA_REGISTRYINDEX, BOT_CACHE);
        if (lua_rawgeti(L, -1, id) == LUA_TUSERDATA) {
            lua_remove(L, -2);
            return;
        }
        lua_pop(L, 1);
        auto* handle = static_cast<BotHandle*>(lua_newuserdatauv(L, sizeof(BotHandle), 0));
        new (handle) BotHandle{ std::move(bot) };
        luaL_setmetatable(L, BOT_META);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, id);
        lua_remove(L, -2);
    }

    void PushBotById(lua_State* L, int id) {
        luaL_getsubtable(L, LUA_REGISTRYINDEX, BOT_CACHE);
        if (lua_rawgeti(L, -1, id) == LUA_TUSERDATA) {
            lua_remove(L, -2);
            return;
        }
        lua_pop(L, 2);
        PushBot(L, Self(L).engine.Manager().ById(id));
    }

    Bot& CheckBot(lua_State* L, int index = 1) {
        return *static_cast<BotHandle*>(luaL_checkudata(L, index, BOT_META))->bot;
    }

    std::shared_ptr<Bot> CheckBotPtr(lua_State* L, int index = 1) {
        return static_cast<BotHandle*>(luaL_checkudata(L, index, BOT_META))->bot;
    }

    std::shared_ptr<Bot> ToBot(lua_State* L, int index) {
        if (lua_isuserdata(L, index)) return CheckBotPtr(L, index);
        if (lua_isinteger(L, index)) return Self(L).engine.Manager().Find(std::to_string(lua_tointeger(L, index)));
        if (lua_type(L, index) == LUA_TSTRING) return Self(L).engine.Manager().Find(lua_tostring(L, index));
        return nullptr;
    }

    void PushValue(lua_State* L, const Events::Value& value) {
        std::visit([L](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) lua_pushboolean(L, v);
            else if constexpr (std::is_same_v<T, int64_t>) lua_pushinteger(L, v);
            else if constexpr (std::is_same_v<T, double>) lua_pushnumber(L, v);
            else lua_pushlstring(L, v.data(), v.size());
        }, value);
    }

    struct DepthGuard {
        Script& s;
        explicit DepthGuard(Script& script) : s(script) { ++s.handlerDepth; }
        ~DepthGuard() { --s.handlerDepth; }
    };

    void Dispatch(lua_State* L, const Events::Event& event) {
        Script& s = Self(L);
        std::vector<std::pair<int, bool>> matches;
        for (const auto& h : s.handlers) {
            const bool wildcard = h.event == "*";
            if ((wildcard || h.event == event.name) && (h.bot == 0 || h.bot == event.bot)) matches.emplace_back(h.id, wildcard);
        }
        if (matches.empty()) return;
        s.handled.fetch_add(1, std::memory_order_relaxed);

        DepthGuard guard(s);
        for (const auto& [handlerId, wildcard] : matches) {
            const auto it = std::ranges::find(s.handlers, handlerId, &Script::Handler::id);
            if (it == s.handlers.end()) continue;
            lua_pushcfunction(L, Traceback);
            const int msgh = lua_gettop(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, it->ref);
            PushBotById(L, event.bot);
            int count = 1;
            if (wildcard) {
                lua_pushlstring(L, event.name.data(), event.name.size());
                ++count;
            }
            luaL_checkstack(L, static_cast<int>(event.args.size()) + 4, "too many event values");
            for (const auto& value : event.args) PushValue(L, value);
            count += static_cast<int>(event.args.size());
            if (lua_pcall(L, count, 0, msgh) != LUA_OK) {
                if (s.stop || s.exitRequested) {
                    lua_pop(L, 2);
                    CheckStop(L);
                }
                s.engine.Print(s.id, Log::Level::Error, std::format("on(\"{}\") handler: {}", event.name, lua_tostring(L, -1)));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
    }

    bool Pump(lua_State* L, Script::Clock::time_point deadline, std::string_view waitFor = {}, int waitBot = 0, Events::Event* out = nullptr) {
        Script& s = Self(L);
        for (;;) {
            CheckStop(L);
            Events::Event event;
            if (s.handlerDepth == 0 && !s.deferred.empty()) {
                event = std::move(s.deferred.front());
                s.deferred.pop_front();
            }
            else {
                std::unique_lock lock(s.eventMutex);
                if (s.queue.empty()) {
                    const auto now = Script::Clock::now();
                    if (now >= deadline) return false;
                    s.cv.wait_until(lock, std::min(deadline, now + 250ms), [&] { return !s.queue.empty() || s.stop.load(); });
                    continue;
                }
                event = std::move(s.queue.front());
                s.queue.pop_front();
            }

            const bool match = !waitFor.empty() && event.name == waitFor && (waitBot == 0 || event.bot == waitBot);
            if (s.handlerDepth == 0) Dispatch(L, event);
            else s.deferred.push_back(event);
            if (match) {
                if (out) *out = std::move(event);
                return true;
            }
            if (Script::Clock::now() >= deadline) return false;
        }
    }

    Script::Clock::time_point DeadlineIn(lua_Integer ms) {
        if (ms < 0) return Script::Clock::time_point::max();
        return Script::Clock::now() + std::chrono::milliseconds(ms);
    }

    template <typename F>
    bool WaitUntil(lua_State* L, lua_Integer timeoutMs, F&& ready) {
        const auto deadline = DeadlineIn(timeoutMs);
        for (;;) {
            if (ready()) return true;
            const auto now = Script::Clock::now();
            if (now >= deadline) return false;
            Pump(L, std::min(deadline, now + 50ms));
        }
    }

    int L_print(lua_State* L) {
        Self(L).engine.Print(Self(L).id, Log::Level::Info, JoinArgs(L));
        return 0;
    }

    int L_log(lua_State* L) {
        Script& s = Self(L);
        const std::string text = JoinArgs(L);
        s.engine.Print(s.id, Log::Level::Info, text);
        Log::Info("LUA:" + s.name, "{}", text);
        return 0;
    }

    int L_warn(lua_State* L) {
        Script& s = Self(L);
        const std::string text = JoinArgs(L);
        s.engine.Print(s.id, Log::Level::Warn, text);
        Log::Warn("LUA:" + s.name, "{}", text);
        return 0;
    }

    int L_sleep(lua_State* L) {
        const lua_Integer ms = luaL_optinteger(L, 1, 0);
        Pump(L, DeadlineIn(std::max<lua_Integer>(ms, 0)));
        return 0;
    }

    int L_millis(lua_State* L) {
        const auto elapsed = Script::Clock::now() - Self(L).started;
        lua_pushinteger(L, std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        return 1;
    }

    int L_scriptId(lua_State* L) { lua_pushinteger(L, Self(L).id); return 1; }
    int L_scriptName(lua_State* L) { lua_pushstring(L, Self(L).name.c_str()); return 1; }

    int FinishPcall(lua_State* L, int status, lua_KContext extra) {
        if (status != LUA_OK && status != LUA_YIELD) {
            CheckStop(L);
            lua_pushboolean(L, 0);
            lua_pushvalue(L, -2);
            return 2;
        }
        return lua_gettop(L) - static_cast<int>(extra);
    }

    int L_pcall(lua_State* L) {
        luaL_checkany(L, 1);
        lua_pushboolean(L, 1);
        lua_insert(L, 1);
        const int status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, FinishPcall);
        return FinishPcall(L, status, 0);
    }

    int L_xpcall(lua_State* L) {
        const int n = lua_gettop(L);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_pushboolean(L, 1);
        lua_pushvalue(L, 1);
        lua_rotate(L, 3, 2);
        const int status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, FinishPcall);
        return FinishPcall(L, status, 2);
    }

    int L_exit(lua_State* L) {
        Self(L).exitRequested = true;
        CheckStop(L);
        return 0;
    }

    int L_getBot(lua_State* L) {
        Script& s = Self(L);
        if (lua_isnoneornil(L, 1)) {
            std::shared_ptr<Bot> bot = s.defaultBot ? s.engine.Manager().ById(s.defaultBot) : nullptr;
            if (!bot) {
                const auto all = s.engine.Manager().All();
                if (!all.empty()) bot = all.front();
            }
            PushBot(L, std::move(bot));
            return 1;
        }
        PushBot(L, ToBot(L, 1));
        return 1;
    }

    int L_getBotById(lua_State* L) {
        PushBot(L, Self(L).engine.Manager().ById(static_cast<int>(luaL_checkinteger(L, 1))));
        return 1;
    }

    int L_getBots(lua_State* L) {
        const auto all = Self(L).engine.Manager().All();
        lua_createtable(L, static_cast<int>(all.size()), 0);
        int i = 0;
        for (const auto& bot : all) {
            PushBot(L, bot);
            lua_rawseti(L, -2, ++i);
        }
        return 1;
    }

    int L_addBot(lua_State* L) {
        const std::string growId = luaL_checkstring(L, 1);
        const std::string password = luaL_checkstring(L, 2);
        const std::string proxy = luaL_optstring(L, 3, "");
        std::string error;
        auto bot = Self(L).engine.Manager().AddBot(growId, password, proxy, error);
        if (!bot) {
            lua_pushnil(L);
            lua_pushstring(L, error.c_str());
            return 2;
        }
        PushBot(L, std::move(bot));
        return 1;
    }

    int L_removeBot(lua_State* L) {
        const auto bot = ToBot(L, 1);
        lua_pushboolean(L, bot && Self(L).engine.Manager().RemoveBot(bot->Id()));
        return 1;
    }

    int L_connectAll(lua_State* L) { Self(L).engine.Manager().StartAll(); return 0; }
    int L_disconnectAll(lua_State* L) { Self(L).engine.Manager().DisconnectAll(); return 0; }

    int AddHandler(lua_State* L, int nameIndex, int fnIndex, int bot) {
        Script& s = Self(L);
        const std::string event = luaL_checkstring(L, nameIndex);
        luaL_checktype(L, fnIndex, LUA_TFUNCTION);
        lua_pushvalue(L, fnIndex);
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        const int id = s.nextHandler++;
        s.handlers.push_back({ id, event, ref, bot });
        s.AddWanted(event);
        lua_pushinteger(L, id);
        return 1;
    }

    int L_on(lua_State* L) { return AddHandler(L, 1, 2, 0); }

    int L_off(lua_State* L) {
        Script& s = Self(L);
        int removed = 0;
        const bool byId = lua_isinteger(L, 1);
        const lua_Integer id = byId ? lua_tointeger(L, 1) : 0;
        const std::string event = byId ? std::string() : luaL_checkstring(L, 1);
        for (auto it = s.handlers.begin(); it != s.handlers.end();) {
            if (byId ? it->id == id : it->event == event) {
                luaL_unref(L, LUA_REGISTRYINDEX, it->ref);
                s.RemoveWanted(it->event);
                it = s.handlers.erase(it);
                ++removed;
            }
            else ++it;
        }
        lua_pushinteger(L, removed);
        return 1;
    }

    int WaitEvent(lua_State* L, const std::string& event, lua_Integer timeoutMs, int bot) {
        Script& s = Self(L);
        s.AddWanted(event);
        Events::Event got;
        bool matched = false;
        try { matched = Pump(L, DeadlineIn(timeoutMs), event, bot, &got); }
        catch (...) {
            s.RemoveWanted(event);
            throw;
        }
        s.RemoveWanted(event);
        if (!matched) {
            lua_pushnil(L);
            return 1;
        }
        PushBotById(L, got.bot);
        luaL_checkstack(L, static_cast<int>(got.args.size()) + 2, "too many event values");
        for (const auto& value : got.args) PushValue(L, value);
        return 1 + static_cast<int>(got.args.size());
    }

    int L_wait(lua_State* L) {
        const std::string event = luaL_checkstring(L, 1);
        const lua_Integer timeout = luaL_optinteger(L, 2, -1);
        const auto bot = lua_isnoneornil(L, 3) ? nullptr : ToBot(L, 3);
        return WaitEvent(L, event, timeout, bot ? bot->Id() : 0);
    }

    int L_listen(lua_State* L) {
        Pump(L, DeadlineIn(luaL_optinteger(L, 1, -1)));
        return 0;
    }

    int L_runScript(lua_State* L) {
        const std::string name = luaL_checkstring(L, 1);
        const auto bot = lua_isnoneornil(L, 2) ? nullptr : ToBot(L, 2);
        std::string code, error;
        if (!ScriptEngine::LoadFile(name, code, error)) {
            lua_pushnil(L);
            lua_pushstring(L, error.c_str());
            return 2;
        }
        lua_pushinteger(L, Self(L).engine.Run(name, code, bot ? bot->Id() : Self(L).defaultBot));
        return 1;
    }

    int L_stopScript(lua_State* L) {
        Self(L).engine.Stop(static_cast<int>(luaL_checkinteger(L, 1)));
        return 0;
    }

    int L_getScripts(lua_State* L) {
        const auto list = Self(L).engine.List();
        lua_createtable(L, static_cast<int>(list.size()), 0);
        int i = 0;
        for (const auto& info : list) {
            lua_createtable(L, 0, 4);
            lua_pushinteger(L, info.id); lua_setfield(L, -2, "id");
            lua_pushstring(L, info.name.c_str()); lua_setfield(L, -2, "name");
            lua_pushboolean(L, info.running); lua_setfield(L, -2, "running");
            lua_pushnumber(L, info.seconds); lua_setfield(L, -2, "seconds");
            lua_rawseti(L, -2, ++i);
        }
        return 1;
    }

    enum class Prop {
        Id, GrowId, Name, Status, StatusText, World, TargetWorld, X, Y, TileX, TileY, Gems, Level, Ping,
        NetId, UserId, Proxy, WorldWidth, WorldHeight, InventorySize, AutoReconnect, Running, Online, InWorld,
    };

    const std::unordered_map<std::string_view, Prop>& Props() {
        static const std::unordered_map<std::string_view, Prop> props = {
            { "id", Prop::Id }, { "index", Prop::Id }, { "growid", Prop::GrowId }, { "name", Prop::Name },
            { "status", Prop::Status }, { "statusText", Prop::StatusText }, { "world", Prop::World },
            { "targetWorld", Prop::TargetWorld }, { "x", Prop::X }, { "y", Prop::Y }, { "tileX", Prop::TileX },
            { "tileY", Prop::TileY }, { "gems", Prop::Gems }, { "level", Prop::Level }, { "ping", Prop::Ping },
            { "netID", Prop::NetId }, { "userID", Prop::UserId }, { "proxy", Prop::Proxy },
            { "worldWidth", Prop::WorldWidth }, { "worldHeight", Prop::WorldHeight },
            { "inventorySize", Prop::InventorySize }, { "autoReconnect", Prop::AutoReconnect },
            { "running", Prop::Running }, { "online", Prop::Online }, { "inWorld", Prop::InWorld },
        };
        return props;
    }

    bool IsOnline(BotStatus status) {
        return status == BotStatus::InGame || status == BotStatus::JoiningWorld || status == BotStatus::InWorld;
    }

    void PushString(lua_State* L, const std::string& text) { lua_pushlstring(L, text.data(), text.size()); }

    void PushProp(lua_State* L, Bot& bot, Prop prop) {
        switch (prop) {
            case Prop::Id: lua_pushinteger(L, bot.Id()); return;
            case Prop::GrowId: PushString(L, bot.GrowId()); return;
            case Prop::Name: PushString(L, bot.Read([](const BotSnapshot& s) { return s.name; })); return;
            case Prop::Status: lua_pushstring(L, StatusKey(bot.Status())); return;
            case Prop::StatusText: PushString(L, bot.Read([](const BotSnapshot& s) { return s.statusText; })); return;
            case Prop::World: PushString(L, bot.World()); return;
            case Prop::TargetWorld: PushString(L, bot.TargetWorld()); return;
            case Prop::X: lua_pushnumber(L, bot.Position().first); return;
            case Prop::Y: lua_pushnumber(L, bot.Position().second); return;
            case Prop::TileX: lua_pushinteger(L, bot.Tile().first); return;
            case Prop::TileY: lua_pushinteger(L, bot.Tile().second); return;
            case Prop::Gems: lua_pushinteger(L, bot.Gems()); return;
            case Prop::Level: lua_pushinteger(L, bot.Read([](const BotSnapshot& s) { return s.level; })); return;
            case Prop::Ping: lua_pushinteger(L, bot.Ping()); return;
            case Prop::NetId: lua_pushinteger(L, bot.Read([](const BotSnapshot& s) { return s.netID; })); return;
            case Prop::UserId: lua_pushinteger(L, bot.Read([](const BotSnapshot& s) { return s.userID; })); return;
            case Prop::Proxy: PushString(L, bot.Read([](const BotSnapshot& s) { return s.proxy; })); return;
            case Prop::WorldWidth: lua_pushinteger(L, bot.Read([](const BotSnapshot& s) { return s.worldWidth; })); return;
            case Prop::WorldHeight: lua_pushinteger(L, bot.Read([](const BotSnapshot& s) { return s.worldHeight; })); return;
            case Prop::InventorySize: lua_pushinteger(L, bot.Read([](const BotSnapshot& s) { return s.inventorySize; })); return;
            case Prop::AutoReconnect: lua_pushboolean(L, bot.AutoReconnect()); return;
            case Prop::Running: lua_pushboolean(L, bot.Running()); return;
            case Prop::Online: lua_pushboolean(L, IsOnline(bot.Status())); return;
            case Prop::InWorld: lua_pushboolean(L, bot.Status() == BotStatus::InWorld); return;
        }
        lua_pushnil(L);
    }

    int Bot_index(lua_State* L) {
        Bot& bot = CheckBot(L, 1);
        lua_pushvalue(L, 2);
        if (lua_rawget(L, lua_upvalueindex(1)) != LUA_TNIL) return 1;
        lua_pop(L, 1);
        size_t length = 0;
        const char* key = lua_tolstring(L, 2, &length);
        if (!key) {
            lua_pushnil(L);
            return 1;
        }
        const auto it = Props().find(std::string_view(key, length));
        if (it == Props().end()) lua_pushnil(L);
        else PushProp(L, bot, it->second);
        return 1;
    }

    int Bot_tostring(lua_State* L) {
        Bot& bot = CheckBot(L, 1);
        lua_pushfstring(L, "Bot(#%d %s)", bot.Id(), bot.GrowId().c_str());
        return 1;
    }

    int Bot_eq(lua_State* L) {
        lua_pushboolean(L, CheckBot(L, 1).Id() == CheckBot(L, 2).Id());
        return 1;
    }

    int Bot_gc(lua_State* L) {
        static_cast<BotHandle*>(luaL_checkudata(L, 1, BOT_META))->~BotHandle();
        return 0;
    }

    uint16_t CheckItem(lua_State* L, int index) {
        const lua_Integer id = luaL_checkinteger(L, index);
        luaL_argcheck(L, id > 0 && id <= 0xFFFF, index, "item id out of range");
        return static_cast<uint16_t>(id);
    }

    int CheckInt(lua_State* L, int index) { return static_cast<int>(luaL_checkinteger(L, index)); }

    int M_connect(lua_State* L) { CheckBot(L).Connect(static_cast<int>(luaL_optinteger(L, 2, 0))); return 0; }
    int M_disconnect(lua_State* L) { CheckBot(L).Disconnect(); return 0; }
    int M_reconnect(lua_State* L) { CheckBot(L).Reconnect(); return 0; }
    int M_say(lua_State* L) { CheckBot(L).Say(luaL_checkstring(L, 2)); return 0; }
    int M_warp(lua_State* L) { CheckBot(L).Warp(luaL_checkstring(L, 2), luaL_optstring(L, 3, "")); return 0; }
    int M_leave(lua_State* L) { CheckBot(L).LeaveWorld(); return 0; }
    int M_walk(lua_State* L) { CheckBot(L).Walk(CheckInt(L, 2), CheckInt(L, 3)); return 0; }
    int M_respawn(lua_State* L) { CheckBot(L).Respawn(); return 0; }
    int M_wear(lua_State* L) { CheckBot(L).Wear(CheckItem(L, 2)); return 0; }
    int M_punch(lua_State* L) { CheckBot(L).Punch(CheckInt(L, 2), CheckInt(L, 3)); return 0; }
    int M_place(lua_State* L) { CheckBot(L).Place(CheckInt(L, 2), CheckInt(L, 3), CheckItem(L, 4)); return 0; }
    int M_wrench(lua_State* L) { CheckBot(L).Wrench(CheckInt(L, 2), CheckInt(L, 3)); return 0; }
    int M_enter(lua_State* L) { CheckBot(L).Activate(CheckInt(L, 2), CheckInt(L, 3)); return 0; }
    int M_setTargetWorld(lua_State* L) { CheckBot(L).SetTargetWorld(luaL_optstring(L, 2, "")); return 0; }
    int M_setAutoReconnect(lua_State* L) { CheckBot(L).SetAutoReconnect(lua_toboolean(L, 2)); return 0; }
    int M_sendText(lua_State* L) { CheckBot(L).SendRawText(luaL_checkstring(L, 2)); return 0; }
    int M_sendGameMessage(lua_State* L) { CheckBot(L).SendRawGameMessage(luaL_checkstring(L, 2)); return 0; }

    int M_move(lua_State* L) {
        Bot& bot = CheckBot(L);
        const auto [x, y] = bot.Tile();
        bot.Walk(x + CheckInt(L, 2), y + static_cast<int>(luaL_optinteger(L, 3, 0)));
        return 0;
    }

    int M_drop(lua_State* L) {
        Bot& bot = CheckBot(L);
        const uint16_t id = CheckItem(L, 2);
        const int count = lua_isnoneornil(L, 3) ? bot.ItemCount(id) : CheckInt(L, 3);
        const bool ok = count > 0 && bot.HasItem(id, count);
        if (ok) bot.Drop(id, count);
        lua_pushboolean(L, ok);
        return 1;
    }

    int M_trash(lua_State* L) {
        Bot& bot = CheckBot(L);
        const uint16_t id = CheckItem(L, 2);
        const int count = lua_isnoneornil(L, 3) ? bot.ItemCount(id) : CheckInt(L, 3);
        const bool ok = count > 0 && bot.HasItem(id, count);
        if (ok) bot.Trash(id, count);
        lua_pushboolean(L, ok);
        return 1;
    }

    int M_dialogReturn(lua_State* L) {
        Bot& bot = CheckBot(L);
        const std::string dialog = luaL_checkstring(L, 2);
        std::vector<std::pair<std::string, std::string>> fields;
        if (lua_istable(L, 3)) {
            const lua_Integer n = luaL_len(L, 3);
            if (n > 0) {
                for (lua_Integer i = 1; i <= n; ++i) {
                    lua_rawgeti(L, 3, i);
                    luaL_checktype(L, -1, LUA_TTABLE);
                    const int item = lua_gettop(L);
                    lua_rawgeti(L, item, 1);
                    std::string key = luaL_tolstring(L, -1, nullptr);
                    lua_pop(L, 2);
                    lua_rawgeti(L, item, 2);
                    std::string value = luaL_tolstring(L, -1, nullptr);
                    lua_pop(L, 3);
                    fields.emplace_back(std::move(key), std::move(value));
                }
            }
            else {
                lua_pushnil(L);
                while (lua_next(L, 3)) {
                    lua_pushvalue(L, -2);
                    const std::string key = luaL_tolstring(L, -1, nullptr);
                    const std::string value = luaL_tolstring(L, -3, nullptr);
                    fields.emplace_back(key, value);
                    lua_pop(L, 4);
                }
            }
        }
        bot.DialogReturn(dialog, fields);
        return 0;
    }

    float FieldNumber(lua_State* L, int table, const char* key, float fallback = 0.0f) {
        lua_getfield(L, table, key);
        const float value = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : fallback;
        lua_pop(L, 1);
        return value;
    }

    lua_Integer FieldInt(lua_State* L, int table, const char* key, lua_Integer fallback = 0) {
        lua_getfield(L, table, key);
        const lua_Integer value = lua_isnumber(L, -1) ? static_cast<lua_Integer>(lua_tonumber(L, -1)) : fallback;
        lua_pop(L, 1);
        return value;
    }

    int M_sendPacket(lua_State* L) {
        Bot& bot = CheckBot(L);
        luaL_checktype(L, 2, LUA_TTABLE);
        Protocol::TankPacket tank;
        tank.type = static_cast<uint8_t>(FieldInt(L, 2, "type"));
        tank.objectType = static_cast<uint8_t>(FieldInt(L, 2, "objectType"));
        tank.count1 = static_cast<uint8_t>(FieldInt(L, 2, "count1"));
        tank.count2 = static_cast<uint8_t>(FieldInt(L, 2, "count2"));
        tank.netID = static_cast<int32_t>(FieldInt(L, 2, "netID"));
        tank.targetNetID = static_cast<int32_t>(FieldInt(L, 2, "targetNetID"));
        tank.flags = static_cast<uint32_t>(FieldInt(L, 2, "flags"));
        tank.floatValue = FieldNumber(L, 2, "floatValue");
        tank.value = static_cast<int32_t>(FieldInt(L, 2, "value"));
        tank.x = FieldNumber(L, 2, "x");
        tank.y = FieldNumber(L, 2, "y");
        tank.xSpeed = FieldNumber(L, 2, "xSpeed");
        tank.ySpeed = FieldNumber(L, 2, "ySpeed");
        tank.particle = static_cast<int32_t>(FieldInt(L, 2, "particle"));
        tank.punchX = static_cast<int32_t>(FieldInt(L, 2, "punchX"));
        tank.punchY = static_cast<int32_t>(FieldInt(L, 2, "punchY"));
        lua_getfield(L, 2, "reliable");
        const bool reliable = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
        lua_pop(L, 1);
        bot.SendRawTank(tank, reliable);
        return 0;
    }

    int M_getInfo(lua_State* L) {
        Bot& bot = CheckBot(L);
        lua_createtable(L, 0, static_cast<int>(Props().size()));
        for (const auto& [key, prop] : Props()) {
            if (key == "index") continue;
            PushProp(L, bot, prop);
            lua_setfield(L, -2, std::string(key).c_str());
        }
        return 1;
    }

    int M_getInventory(lua_State* L) {
        const auto items = CheckBot(L).Inventory();
        lua_createtable(L, static_cast<int>(items.size()), 0);
        int i = 0;
        for (const auto& item : items) {
            lua_createtable(L, 0, 3);
            lua_pushinteger(L, item.id); lua_setfield(L, -2, "id");
            lua_pushinteger(L, item.amount); lua_setfield(L, -2, "count");
            lua_pushinteger(L, item.flags); lua_setfield(L, -2, "flags");
            lua_rawseti(L, -2, ++i);
        }
        return 1;
    }

    int M_getItemCount(lua_State* L) { lua_pushinteger(L, CheckBot(L).ItemCount(CheckItem(L, 2))); return 1; }

    int M_hasItem(lua_State* L) {
        lua_pushboolean(L, CheckBot(L).HasItem(CheckItem(L, 2), static_cast<int>(luaL_optinteger(L, 3, 1))));
        return 1;
    }

    void PushPlayer(lua_State* L, const PlayerInfo& p) {
        lua_createtable(L, 0, 9);
        lua_pushinteger(L, p.netID); lua_setfield(L, -2, "netID");
        lua_pushinteger(L, p.userID); lua_setfield(L, -2, "userID");
        PushString(L, p.name); lua_setfield(L, -2, "name");
        PushString(L, p.country); lua_setfield(L, -2, "country");
        lua_pushnumber(L, p.x); lua_setfield(L, -2, "x");
        lua_pushnumber(L, p.y); lua_setfield(L, -2, "y");
        lua_pushinteger(L, static_cast<lua_Integer>(p.x / 32.0f)); lua_setfield(L, -2, "tileX");
        lua_pushinteger(L, static_cast<lua_Integer>(p.y / 32.0f)); lua_setfield(L, -2, "tileY");
        lua_pushboolean(L, p.moderator); lua_setfield(L, -2, "moderator");
    }

    int M_getPlayers(lua_State* L) {
        const auto players = CheckBot(L).Players();
        lua_createtable(L, static_cast<int>(players.size()), 0);
        int i = 0;
        for (const auto& p : players) {
            PushPlayer(L, p);
            lua_rawseti(L, -2, ++i);
        }
        return 1;
    }

    int M_getPlayer(lua_State* L) {
        const auto players = CheckBot(L).Players();
        const bool byNet = lua_isinteger(L, 2);
        const lua_Integer netID = byNet ? lua_tointeger(L, 2) : 0;
        const std::string name = byNet ? std::string() : Text::Lower(luaL_checkstring(L, 2));
        for (const auto& p : players) {
            if (byNet ? p.netID == netID : Text::Lower(p.name) == name) {
                PushPlayer(L, p);
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }

    int M_isInWorld(lua_State* L) {
        Bot& bot = CheckBot(L);
        const std::string world = bot.World();
        const bool in = !world.empty() && bot.Status() == BotStatus::InWorld &&
            (lua_isnoneornil(L, 2) || Text::Upper(luaL_checkstring(L, 2)) == world);
        lua_pushboolean(L, in);
        return 1;
    }

    int M_isOnline(lua_State* L) { lua_pushboolean(L, IsOnline(CheckBot(L).Status())); return 1; }

    int M_waitForWorld(lua_State* L) {
        Bot& bot = CheckBot(L);
        const std::string wanted = lua_isnoneornil(L, 2) ? std::string() : Text::Upper(luaL_checkstring(L, 2));
        const lua_Integer timeout = luaL_optinteger(L, 3, DEFAULT_WAIT_MS);
        lua_pushboolean(L, WaitUntil(L, timeout, [&] {
            if (bot.Status() != BotStatus::InWorld) return false;
            return wanted.empty() || bot.World() == wanted;
        }));
        return 1;
    }

    int M_waitForStatus(lua_State* L) {
        Bot& bot = CheckBot(L);
        const std::string wanted = luaL_checkstring(L, 2);
        const lua_Integer timeout = luaL_optinteger(L, 3, DEFAULT_WAIT_MS);
        lua_pushboolean(L, WaitUntil(L, timeout, [&] { return wanted == StatusKey(bot.Status()); }));
        return 1;
    }

    int M_waitForOnline(lua_State* L) {
        Bot& bot = CheckBot(L);
        const lua_Integer timeout = luaL_optinteger(L, 2, 60000);
        lua_pushboolean(L, WaitUntil(L, timeout, [&] { return IsOnline(bot.Status()); }));
        return 1;
    }

    int M_warpAndWait(lua_State* L) {
        Bot& bot = CheckBot(L);
        const std::string world = Text::Upper(Text::Trim(luaL_checkstring(L, 2)));
        const lua_Integer timeout = luaL_optinteger(L, 3, DEFAULT_WAIT_MS);
        bot.Warp(world, luaL_optstring(L, 4, ""));
        lua_pushboolean(L, WaitUntil(L, timeout, [&] { return bot.Status() == BotStatus::InWorld && bot.World() == world; }));
        return 1;
    }

    int M_on(lua_State* L) { return AddHandler(L, 2, 3, CheckBot(L).Id()); }

    int M_wait(lua_State* L) {
        Bot& bot = CheckBot(L);
        const std::string event = luaL_checkstring(L, 2);
        return WaitEvent(L, event, luaL_optinteger(L, 3, -1), bot.Id());
    }

    const luaL_Reg BOT_METHODS[] = {
        { "connect", M_connect }, { "disconnect", M_disconnect }, { "reconnect", M_reconnect },
        { "say", M_say }, { "talk", M_say }, { "warp", M_warp }, { "warpAndWait", M_warpAndWait }, { "leave", M_leave },
        { "walk", M_walk }, { "move", M_move }, { "drop", M_drop }, { "trash", M_trash },
        { "wear", M_wear }, { "unwear", M_wear }, { "use", M_wear },
        { "punch", M_punch }, { "place", M_place }, { "wrench", M_wrench }, { "enter", M_enter },
        { "respawn", M_respawn }, { "setTargetWorld", M_setTargetWorld }, { "setAutoReconnect", M_setAutoReconnect },
        { "sendText", M_sendText }, { "sendGameMessage", M_sendGameMessage }, { "sendPacket", M_sendPacket },
        { "dialogReturn", M_dialogReturn },
        { "getInfo", M_getInfo }, { "getInventory", M_getInventory }, { "getItemCount", M_getItemCount },
        { "hasItem", M_hasItem }, { "getPlayers", M_getPlayers }, { "getPlayer", M_getPlayer },
        { "isInWorld", M_isInWorld }, { "isOnline", M_isOnline },
        { "waitForWorld", M_waitForWorld }, { "waitForStatus", M_waitForStatus }, { "waitForOnline", M_waitForOnline },
        { "on", M_on }, { "wait", M_wait },
        { nullptr, nullptr },
    };

    const luaL_Reg GLOBALS[] = {
        { "print", L_print }, { "log", L_log }, { "warn", L_warn },
        { "sleep", L_sleep }, { "millis", L_millis }, { "exit", L_exit },
        { "pcall", L_pcall }, { "xpcall", L_xpcall },
        { "scriptId", L_scriptId }, { "scriptName", L_scriptName },
        { "getBot", L_getBot }, { "getBotById", L_getBotById }, { "getBots", L_getBots },
        { "addBot", L_addBot }, { "removeBot", L_removeBot },
        { "connectAll", L_connectAll }, { "disconnectAll", L_disconnectAll },
        { "on", L_on }, { "off", L_off }, { "wait", L_wait }, { "listen", L_listen },
        { "runScript", L_runScript }, { "stopScript", L_stopScript }, { "getScripts", L_getScripts },
        { nullptr, nullptr },
    };

    void OpenApi(lua_State* L) {
        luaL_newmetatable(L, BOT_META);
        lua_newtable(L);
        luaL_setfuncs(L, BOT_METHODS, 0);
        lua_pushcclosure(L, Bot_index, 1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, Bot_tostring); lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, Bot_eq); lua_setfield(L, -2, "__eq");
        lua_pushcfunction(L, Bot_gc); lua_setfield(L, -2, "__gc");
        lua_pop(L, 1);

        lua_pushglobaltable(L);
        luaL_setfuncs(L, GLOBALS, 0);
        lua_pop(L, 1);
    }
}

Script::Script(ScriptEngine& engine_, int id_, std::string name_, std::string code_, int bot, bool echo_, bool trace_)
    : engine(engine_), id(id_), name(std::move(name_)), code(std::move(code_)), defaultBot(bot), echo(echo_),
      lineCount(echo_ ? 0 : static_cast<int>(std::ranges::count(code, '\n')) + 1),
      lines(std::make_unique<std::atomic<uint8_t>[]>(static_cast<size_t>(lineCount))) {
    trace = trace_;
}

void Script::SettleLoop(Frame& frame) {
    if (frame.resetTo == 0) return;
    for (int n = frame.resetFrom + 1; n <= frame.resetTo; ++n) {
        if (GetLine(n) == ScriptEngine::LineNone) SetLine(n, frame.saved[static_cast<size_t>(n - frame.resetFrom)]);
    }
    frame.resetTo = 0;
}

void Script::FinishLines(const std::string& failure, bool stopped) {
    if (!everTraced) return;
    for (auto& frame : frames) SettleLoop(frame);
    int errorLine = 0;
    if (!failure.empty() && !stopped) {
        const std::string prefix = name + ":";
        if (const size_t at = failure.find(prefix); at != std::string::npos) {
            for (size_t p = at + prefix.size(); p < failure.size() && failure[p] >= '0' && failure[p] <= '9'; ++p)
                errorLine = errorLine * 10 + (failure[p] - '0');
        }
    }
    for (int n = 1; n <= lineCount; ++n) {
        if (GetLine(n) != ScriptEngine::LineCurrent) continue;
        SetLine(n, !failure.empty() && !stopped && errorLine == 0 ? ScriptEngine::LineError : ScriptEngine::LineDone);
    }
    if (errorLine > 0) SetLine(errorLine, ScriptEngine::LineError);
}

Script::~Script() {
    RequestStop();
    if (thread.joinable()) thread.join();
}

bool Script::Wants(std::string_view event) const {
    std::lock_guard lock(eventMutex);
    return wanted.contains(event) || (!wanted.empty() && wanted.contains(std::string_view("*")));
}

void Script::Push(const Events::Event& event) {
    {
        std::lock_guard lock(eventMutex);
        if (queue.size() >= QUEUE_LIMIT) {
            queue.pop_front();
            dropped.fetch_add(1, std::memory_order_relaxed);
        }
        queue.push_back(event);
    }
    cv.notify_one();
}

void Script::AddWanted(const std::string& event) {
    std::lock_guard lock(eventMutex);
    ++wanted[event];
}

void Script::RemoveWanted(const std::string& event) {
    std::lock_guard lock(eventMutex);
    const auto it = wanted.find(event);
    if (it != wanted.end() && --it->second <= 0) wanted.erase(it);
}

void Script::RequestStop() {
    stop = true;
    cv.notify_all();
}

void Script::Start() {
    thread = std::thread([this] { Main(); });
}

void Script::Main() {
    lua_State* L = luaL_newstate();
    std::string failure;
    if (!L) failure = "out of memory";
    else {
        *static_cast<Script**>(lua_getextraspace(L)) = this;
        Events::Subscribe(this);
        try {
            luaL_openlibs(L);
            OpenApi(L);
            SetTracing(L, *this, trace.load());

            lua_pushcfunction(L, Traceback);
            const int msgh = lua_gettop(L);
            const std::string chunk = "@" + name;
            int status = LUA_ERRSYNTAX;
            if (echo) {
                const std::string expression = "return " + code;
                status = luaL_loadbufferx(L, expression.data(), expression.size(), "=console", "t");
                if (status != LUA_OK) lua_pop(L, 1);
            }
            if (status != LUA_OK) status = luaL_loadbufferx(L, code.data(), code.size(), echo ? "=console" : chunk.c_str(), "t");
            if (status == LUA_OK) status = lua_pcall(L, 0, LUA_MULTRET, msgh);

            if (status == LUA_OK) {
                if (echo && lua_gettop(L) > msgh) {
                    std::string out;
                    for (int i = msgh + 1; i <= lua_gettop(L); ++i) {
                        if (i > msgh + 1) out += "\t";
                        out += luaL_tolstring(L, i, nullptr);
                        lua_pop(L, 1);
                    }
                    engine.Print(id, Log::Level::Info, "= " + out);
                }
            }
            else {
                const char* message = lua_tostring(L, -1);
                failure = message ? message : "error object is not a string";
            }
        }
        catch (const std::exception& e) { failure = e.what(); }
        Events::Unsubscribe(this);
        lua_close(L);
    }

    const bool stopped = stop || exitRequested;
    FinishLines(failure, stopped);
    {
        std::lock_guard lock(infoMutex);
        finished = Clock::now();
        if (!failure.empty() && !stopped) error = failure;
    }
    if (!failure.empty() && !stopped) {
        failed = true;
        engine.Print(id, Log::Level::Error, failure);
        Log::Error("LUA:" + name, "{}", failure);
    }
    else if (stop && !exitRequested) engine.Print(id, Log::Level::Warn, "stopped");
    else if (!echo) engine.Print(id, Log::Level::Info, "finished");
    running = false;
}
