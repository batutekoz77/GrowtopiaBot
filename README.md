# 💧 Growtopia Bot — Growtopia Bot Manager
Growtopia Bot is a fast, lightweight, multi-bot manager for Growtopia. It logs in and runs as many bots as you want from one modern panel, with no game client needed. You can control each bot by hand or automate everything with **Lua 5.4** scripts: connect, warp, chat, drop, walk, punch/place, send raw packets, and react to in-game events.

---

## 🛠 Features
- **Headless multi-bot:** run tens or hundreds of bots without opening the Growtopia client
- **Modern panel (ImGui + DirectX 11):** Overview, World, Inventory, Players, Scripts, Log and Settings tabs
- **SOCKS5 proxy support for each bot** (UDP over SOCKS5)
- **Auto reconnect:** a bot rejoins its target world after kicks and disconnects
- **Login queue:** spaced-out logins (`Connect all`) to avoid *"Too many people logging in"*
- **Built-in Lua 5.4 engine:** every script runs in its own thread and its own Lua state
- **In-app script editor:** syntax view, live line tracking, Output panel and a one-line Lua console
- **Event system:** `console`, `talk`, `world_enter`, `player_join`, `moderator`, `disconnected` and more
- **Raw packets:** send `GENERIC_TEXT`, `GAME_MESSAGE` and tank packets yourself
- **Pathfinding:** A* over the real world data (doors, locks, entrances, one-way blocks, platforms), walked one tile per packet
- **Ban protection (5x5 reach guard):** punches, places, wrenches, door entries and item pickups outside the game's own 5x5 reach are never sent, raw packets included
- **World data:** every tile (foreground, background, doors, signs, locks, trees...), dropped items and item names from `items.dat`
- **Dropped item collection:** collect what is in reach, or let the bot auto-collect while it walks
- **Very low resource usage:** 100 bots use ~14.5 % of one CPU core, and the panel idles at ~1 % CPU
- **Nothing is saved to disk:** bots and settings only last until the program closes

---

## 🚀 Tech Stack

- **Language:** ISO C++23 Standard (Visual Studio 2026 Community)
- **Compiler Standards:**
  - C++23 (`stdcpp23`)
  - Toolset: `v145`
  - Target: `x64/Release`
- **Libraries (bundled in `Library/`, no vcpkg needed):**
  - ENet (network)
  - Lua 5.4 (scripting)
  - Dear ImGui + DirectX 11 (panel)
  - Windows SChannel / BCrypt (HTTPS & crypto)

---

## 🎮 Roadmap
- [x] Change settings from the panel (game version, protocol, fhash, timings)
- [x] HTTPS login (server_data / token)
- [x] ENet network client:
    - [x] Multi-bot, event-driven network loop (one wake socket per bot)
    - [x] Auto reconnect & target world
    - [x] Raw packet sending
- [x] SOCKS5 proxy support
- [x] Lua script support
- [x] In-app script editor with line tracking
- [x] Advanced log system (ring buffer, ~2.5M lines/s)
- [x] Built-in benchmarks & self tests (`--bench`, `--bench-ui`, `--selftest`)
- [x] Pathfinding (ported from AquaProxy) with the 5x5 reach guard
- [x] Tile data, dropped item collection & item names (items.dat)

---

## 🖥 Panel

## Overview:
- [x] Live bot card: status, world, tile, gems, level, ping, inventory, players, netID / userID
- [x] Connect / Disconnect / Reconnect / Auto reconnect / Remove bot
- [x] Travel: warp (with door id), set target world, leave world, respawn
- [x] Chat & packets: say messages or `/commands`, send raw text packets

## World:
- [x] Tile grid around the bot (its 5x5 reach) with block / background names, solid tiles, other players and moderators
- [x] Tools: **Punch**, **Place**, **Wrench**, **Door**, **Walk** (keys `1-5`)
- [x] Move with `WASD` / arrow keys or walk to any tile (pathfinding), with walk status and **Stop walking**
- [x] Dropped items: nearest drops, **Collect nearby**, **Auto collect**, walk to a drop

## Inventory:
- [x] Every item with its name, id and count (search by name or id)
- [x] Drop / trash / wear / use

## Players:
- [x] Every player in the world with name, country, position and moderator flag

## Scripts:
- [x] Script file browser (`scripts/` folder)
- [x] Editor: Save (`Ctrl + S`), Load, Run (`F5`), **Run for every bot**, Stop all
- [x] Output panel and running scripts list
- [x] `Lua >` console: run one line at a time, e.g. `getBot().gems`

## Log:
- [x] Every log line from all bots and scripts, with filtering

## Settings:
- [x] Game client: version, protocol, fhash (raise these after a Growtopia update)
- [x] Timing: login interval, rejoin delay
- [x] Debug logging
- [x] items.dat status (where it was loaded from)

---

## 📸 Screenshots

![Overview](Showcase/Overview.png)
![World](Showcase/World.png)
![Inventory](Showcase/Inventory.png)
![Players](Showcase/Players.png)
![Scripts](Showcase/Scripts.png)
![Log](Showcase/Log.png)
![Settings](Showcase/Settings.png)

<p align="center"><img src="Showcase/AddBot.png" alt="Add Bot" width="436"></p>

---

## Getting Started

### 1. Install Visual Studio
Download and install **Visual Studio 2026 Community** with the **Desktop development with C++** workload:
https://visualstudio.microsoft.com/

### 2. Clone the Repository
Open **CMD** and run:
```
git clone https://github.com/batutekoz77/GrowtopiaBot.git
cd GrowtopiaBot
```

> No dependency setup needed: ENet, Lua and ImGui are already in the `Library/` folder.

> **items.dat:** item names and tile collisions come from `items.dat`. The bot reads the one next to
> `GrowtopiaBot.exe` first, then the game's cache (`%LOCALAPPDATA%\Growtopia\cache\items.dat`).
> Without it everything still works, but items show as `#id` and the pathfinder only walks through air.

## Building the Bot
1. Open `GrowtopiaBot.slnx` in **Visual Studio Community**
2. Select `x64/Release`
3. Build the solution (`Ctrl + Shift + B`)
4. Run `GrowtopiaBot.exe` from `x64/Release`
5. Click **+ Add bot**, enter your GrowID, password and an optional proxy, then click **Connect**

Note: The example scripts in `Docs/examples/` are copied to `x64/Release/scripts/` during the build. Files you edited are not overwritten.

### 🧪 Benchmarks & Self Tests
None of these touch the real Growtopia server or any account. The bots connect to a **fake local ENet server** on `127.0.0.1`.
```
GrowtopiaBot.exe --bench [bots=50] [seconds=10] [packets/s per bot=20]   -> bench_result.txt
GrowtopiaBot.exe --bench-ui [bots=100] [seconds=8]                       -> bench_ui_result.txt
GrowtopiaBot.exe --selftest                                              -> selftest_result.txt
```

| Bot network loop (100 bots, 20 packets/s each) | Before | After | Difference |
|---|---|---|---|
| CPU (one core) | 61.3 % | 14.5 % | **−76 %** |
| Wake-ups per bot | 205 /s | 31 /s | **−85 %** |
| Command latency (avg) | 2.76 ms | 0.36 ms | **7.7× faster** |
| Command latency (p99) | 37.8 ms | 2.28 ms | |

See [`Docs/PERFORMANCE.md`](Docs/PERFORMANCE.md) for all results.

---

## 🌙 Lua Scripting

Scripts are in the `scripts/` folder next to `GrowtopiaBot.exe`. Write, save, load and run them from the **Scripts** tab.

> ⚠️ **Important:** Event handlers (`on(...)`) only run while the script is inside `sleep()`, `wait()`, `listen()` or a `waitFor...` call. Your handlers never run at the same time as your main code, so you don't need locks.

---

### 📁 How It Works

- Every script runs **in its own thread and its own Lua state**. An infinite loop never slows down the bots or the panel.
- Bot commands (`warp`, `say`, `drop`...) are **queued** and run on the bot's own thread. They return right away. Use `waitFor...` or `wait()` to wait for the result.
- Reads (`bot.world`, `bot.gems`...) always return the **current** value and are thread-safe.
- When you stop a script (**Stop** or `stopScript(id)`), it is interrupted within a few milliseconds wherever it is, even inside `sleep` or `pcall`.
- `getBot()` with no argument returns the bot **selected in the panel**. **Run for every bot** starts one copy of the script per bot.

```lua
-- runs once at the start
local bot = getBot()
print("Script started for " .. bot.name)

-- main loop
while true do
    -- your code here
    sleep(1000)
end
```

---

### 🔧 Global Functions

#### Output
| Function | Description |
|---|---|
| `print(...)` | Writes to the script Output panel |
| `log(...)` | Writes to Output **and** the main Log tab (tag: `LUA:<script>`) |
| `warn(...)` | Like `log`, but as a warning (yellow) |

#### Time & Flow
| Function | Returns | Description |
|---|---|---|
| `sleep(ms)` | – | Waits `ms` milliseconds and runs event handlers meanwhile |
| `millis()` | `int` | Milliseconds since the script started |
| `listen([ms])` | – | Only processes events. With no argument it runs until the script is stopped |
| `wait(event, [timeoutMs], [bot])` | `bot, values...` / `nil` | Waits for an event |
| `exit()` | – | Ends the script without an error |
| `scriptId()` | `int` | This script's id |
| `scriptName()` | `string` | This script's name (file name) |

#### Bots
| Function | Returns | Description |
|---|---|---|
| `getBot()` | `Bot` | Bot selected in the panel (or the bot given to the script) |
| `getBot(n)` | `Bot` / `nil` | The `n`th bot in the list (starts at 1) |
| `getBot("name")` | `Bot` / `nil` | By GrowID (case-insensitive), then by in-game name |
| `getBotById(id)` | `Bot` / `nil` | By the fixed bot id (`bot.id`) |
| `getBots()` | `Bot[]` | All bots, in list order |
| `addBot(growid, password, [proxy])` | `Bot` / `nil, err` | Adds a new bot (doesn't start it, so call `bot:connect()`) |
| `removeBot(bot \| name \| n)` | `bool` | Stops the bot and removes it from the list |
| `connectAll()` | – | Connects all offline bots, spaced by `login_interval_ms` |
| `disconnectAll()` | – | Disconnects every bot |

Proxy formats: `socks5://user:pass@host:port`, `socks5://host:port`, `host:port:user:pass` or `host:port`.

#### Events
| Function | Returns | Description |
|---|---|---|
| `on(event, function(bot, ...) end)` | handler id | Adds a handler for all bots. `"*"` receives every event |
| `off(id)` | removed count | Removes one handler |
| `off(event)` | removed count | Removes every handler of that event |

#### Script Management
| Function | Returns | Description |
|---|---|---|
| `runScript(file, [bot])` | id / `nil, err` | Starts another script from `scripts/` in its own thread |
| `stopScript(id)` | – | Stops a script |
| `getScripts()` | `table[]` | `{ id, name, running, seconds }` tables |

---

### 👤 Bot Properties

All are read-only and always up to date: `bot.world`, `bot.gems`...

| Property | Type | Description |
|---|---|---|
| `id` | `int` | Fixed bot id (`#` in the panel) |
| `growid` | `string` | GrowID |
| `name` | `string` | In-game name (no color codes) |
| `status` | `string` | Status key (see below) |
| `statusText` | `string` | Why it's waiting or stopped (`"server overloaded"`, `"wrong GrowID / password"`...) |
| `online` | `bool` | In game (menu, joining or in_world) |
| `inWorld` | `bool` | Inside a world |
| `running` | `bool` | Bot thread is running (not stopped) |
| `world` | `string` | Current world (`""` when outside) |
| `worldWidth`, `worldHeight` | `int` | World size in tiles |
| `targetWorld` | `string` | World it returns to after a kick or reconnect |
| `x`, `y` | `float` | Position in pixels |
| `tileX`, `tileY` | `int` | Position in tiles (the avatar's centre) |
| `gems` | `int` | Gem count |
| `level` | `int` | Level |
| `ping` | `int` | Ping in ms |
| `netID`, `userID` | `int` | Ids given by the server |
| `inventorySize` | `int` | Inventory slot count |
| `proxy` | `string` | `host:port` (password hidden) or `""` |
| `autoReconnect` | `bool` | Reconnects automatically on disconnect |
| `walking` | `bool` | A `walk` is in progress |
| `autoCollect` | `bool` | Collects dropped items by itself |

---

### 🔌 Connection

| Method | Description |
|---|---|
| `bot:connect([delayMs])` | Logs in. Restarts the bot if it's stopped or offline |
| `bot:disconnect()` | Disconnects and stays **offline** |
| `bot:reconnect()` | Disconnects and logs in again right away |
| `bot:setAutoReconnect(bool)` | Reconnect automatically when kicked (default `true`) |

---

### 🌍 Movement & World

| Method | Description |
|---|---|
| `bot:warp(world, [doorId])` | Warps to a world |
| `bot:warpAndWait(world, [timeoutMs=30000], [doorId])` | Warps and waits until it's inside → `true/false` |
| `bot:leave()` | Leaves the world (to the world menu) |
| `bot:setTargetWorld(world)` | World to return to after kicks / reconnects. `""` = off |
| `bot:walk(tileX, tileY)` | Walks there with pathfinding and returns right away. A new walk replaces the running one |
| `bot:walkAndWait(tileX, tileY, [timeoutMs=30000])` | Walks and waits until it arrives → `true` / `false, reason` |
| `bot:stopWalk()` | Stops walking |
| `bot:findPath(tileX, tileY)` | The route without walking → `{ {x, y}, ... }` (both ends included) / `nil, reason` |
| `bot:move(dx, [dy=0])` | Walks relative to the current tile |
| `bot:respawn()` | Respawns |
| `bot:enter(tileX, tileY)` | Enters the door / portal on that tile → `bool` |
| `bot:punch(tileX, tileY)` | Punches a tile → `bool` |
| `bot:place(tileX, tileY, itemId)` | Places a block / seed (only if it's in the inventory) → `bool` |
| `bot:wrench(tileX, tileY)` | Uses the wrench on a tile → `bool` |
| `bot:inReach(tileX, tileY)` | The tile is inside the bot's 5x5 reach |

```lua
local bot = getBot()
if bot:warpAndWait("START") and bot:walkAndWait(10, 20) then
    bot:punch(11, 20)
end
```

#### 🧭 Pathfinding
The pathfinder is the one from AquaProxy's `/path`: A* over the world's real tiles. Solid blocks, closed doors,
entrances you have no access to, VIP / friends entrances, one-way blocks and platforms are respected, and
fire / spikes are avoided when there is another way. The bot moves **one tile per packet** (two steps back to back,
then a random 80–200 ms pause), and the steps are sent reliably (a lost step would look like a teleport to the server).
If the route closes (a door shuts, a block is placed, the server moves the bot), it plans again from where it really is.

#### 🛡 The 5x5 reach guard (ban protection)
The game only punches / places / wrenches inside the **5x5 square** around the player (2 tiles on either axis).
A character spawned with `mstate` / `smstate` can reach further, but the server flags those packets and **bans**.
AquaProxy blocks this by dropping the packets a normal client could never send, and the bot does the same:
every tile action outside the 5x5 square (raw `sendPacket` included) is **not sent** and a warning is logged.
A block can't be placed on the tile the bot stands on (a background can). The `bool` that `punch` / `place` /
`wrench` / `enter` return is that check at call time, so walk closer first (`walkAndWait`).

---

### 💬 Chat & Dialogs

| Method | Description |
|---|---|
| `bot:say(text)` / `bot:talk(text)` | Writes to chat. `/commands` also work (`bot:say("/wave")`) |
| `bot:dialogReturn(name, fields)` | Sends `action\|dialog_return`. `fields`: `{ key = value }` or `{ {"key", value}, ... }` when order matters |

```lua
bot:dialogReturn("drop_item", { {"itemID", "242|"}, {"count", 10} })
```

---

### 🎒 Inventory

| Method | Returns | Description |
|---|---|---|
| `bot:getInventory()` | `table[]` | `{ id, name, count, flags }` tables |
| `bot:getItemCount(id)` | `int` | Count of that item |
| `bot:hasItem(id, [count=1])` | `bool` | Has the item |
| `bot:drop(id, [count])` | `bool` | Drops the item (all if no count) |
| `bot:trash(id, [count])` | `bool` | Trashes the item (all if no count) |
| `bot:wear(id)` / `bot:unwear(id)` / `bot:use(id)` | – | Wear / unwear / use |

```lua
local bot = getBot()
if bot:hasItem(242, 10) then
    print("Dirt count: " .. bot:getItemCount(242))
    bot:drop(242, 10)
end
```

---

### 🧱 Tiles & Dropped Items

| Method | Returns | Description |
|---|---|---|
| `bot:getTile(tileX, tileY)` | `table` / `nil` | One tile (`nil` outside the world) |
| `bot:getTiles()` | `table[]` | Every tile of the world (6000 tables for 100x60, don't call it in a tight loop) |
| `bot:findTiles(itemId)` | `table[]` | Tiles with that item as foreground or background: `{ {x, y}, ... }` |
| `bot:getObjects()` | `table[]` | Dropped items in the world |
| `bot:collect([radius=2])` | `int` | Collects the dropped items within `radius` tiles (max 2) and returns how many are in range. Skips what doesn't fit (full stack of 200, full backpack) |
| `bot:setAutoCollect(bool, [radius=2])` | – | Collects by itself while walking, when something drops and every 0.5 s |

Tile table: `{ x, y, fg, bg, fgName, bgName, flags, extraKind, walkable }`, plus `label` on doors / signs,
`owner` (userID) on locks and `age` (seconds), `fruit`, `ready` on trees.
Object table: `{ uid, id, name, count, x, y, tileX, tileY }`.

Tiles come from the world's map packet and stay up to date with the server's tile updates
(punches, places, tile updates, harvested trees).

```lua
local bot = getBot()
for _, t in ipairs(bot:findTiles(3)) do   -- 3 = Dirt Seed (getItemId("dirt seed"))
    local tile = bot:getTile(t.x, t.y)
    if tile.ready and bot:walkAndWait(t.x, t.y) then bot:punch(t.x, t.y) sleep(300) bot:collect() end
end
```

#### Item info (global functions)
| Function | Returns | Description |
|---|---|---|
| `getItemName(id)` | `string` | `"Dirt"`, or `"#2"` while items.dat isn't loaded |
| `getItemId(name)` | `int` / `nil` | Case-insensitive: `getItemId("dirt seed")` |
| `getItemInfo(id \| name)` | `table` / `nil` | `{ id, name, type, collision, category, rarity, hits, growTime, background, info }` |
| `itemsReady()` | `bool` | items.dat is loaded |

---

### 👥 Players

| Method | Returns | Description |
|---|---|---|
| `bot:getPlayers()` | `table[]` | Other players in the world |
| `bot:getPlayer(name \| netID)` | `table` / `nil` | One player |

Player table: `{ netID, userID, name, country, x, y, tileX, tileY, moderator }`.

---

### ⏳ Status & Waiting

Waiting methods run event handlers while they wait.

| Method | Returns | Description |
|---|---|---|
| `bot:getInfo()` | `table` | All bot properties in one table |
| `bot:isOnline()` | `bool` | Same as `bot.online` |
| `bot:isInWorld([world])` | `bool` | In a world (or in the given world) |
| `bot:waitForOnline([timeoutMs=60000])` | `bool` | Waits until it's logged in |
| `bot:waitForWorld([world], [timeoutMs=30000])` | `bool` | Waits until it enters a world |
| `bot:waitForStatus(status, [timeoutMs=30000])` | `bool` | Waits until `bot.status == status` |
| `bot:on(event, function(bot, ...) end)` | handler id | Events of **this bot only** |
| `bot:wait(event, [timeoutMs])` | `bot, values...` / `nil` | Waits for an event of this bot |

---

### 📡 Raw Packets

| Method | Description |
|---|---|
| `bot:sendText(text)` | `NET_MESSAGE_GENERIC_TEXT` (2), e.g. `"action\|respawn\n"` |
| `bot:sendGameMessage(text)` | `NET_MESSAGE_GAME_MESSAGE` (3), e.g. `"action\|quit_to_exit\n"` |
| `bot:sendPacket(table)` | Tank packet (`NET_MESSAGE_GAME_PACKET`) |

#### Available fields for `sendPacket` (all optional, default `0`):

| Field | Description |
|---|---|
| `type`, `objectType` | Packet type / object type |
| `count1`, `count2` | Counters |
| `netID`, `targetNetID` | Net ids |
| `flags` | Flags |
| `floatValue`, `value` | Values |
| `x`, `y` | Position |
| `xSpeed`, `ySpeed` | Speed |
| `particle` | Particle |
| `punchX`, `punchY` | Punch tile |
| `reliable` | Reliable packet (default `true`) |

```lua
-- state packet with punch animation
bot:sendPacket({ type = 0, x = bot.x, y = bot.y, punchX = 10, punchY = 20, flags = 0x200 | 0x20, reliable = false })
```

---

### ⚡ Events

The first handler argument is **always the bot**, then the values below:

```lua
on("console", function(bot, text, raw) print(bot.name, text) end)
local bot, world, w, h = wait("world_enter", 10000)
```

| Event | Values (after bot) | When |
|---|---|---|
| `status` | `status, statusText` | Status changed |
| `connected` | – | ENet connection established (before login) |
| `login` | – | Login accepted |
| `logon_fail` | `reason` | Login rejected (2FA, overload, maintenance...) |
| `disconnected` | `reason` | Connection lost |
| `world_enter` | `world, width, height` | Entered a world |
| `world_menu` | `leftWorld` | Back to the world menu (left / kicked) |
| `join_fail` | `world` | Couldn't join the world |
| `spawn` | `netID, userID, x, y` | Bot spawned in the world |
| `player_join` | `netID, name, userID, country, isModerator` | Someone joined the world |
| `player_leave` | `netID, name` | Someone left the world |
| `moderator` | `name, netID` | A moderator / invisible player joined |
| `console` | `text, rawText` | Console message (`text` without color codes) |
| `talk` | `netID, name, text` | Chat bubble (including your own) |
| `dialog` | `dialogText` | Server opened a dialog (`OnDialogRequest`) |
| `inventory` | `itemCount, slotCount` | Full inventory received |
| `item` | `itemId, change, newCount` | Item gained (+) / lost (−) |
| `gems` | `gems` | Gem count changed |
| `variant` | `functionName, arg1, arg2, ...` | **Every** `OnXxx` call from the server |
| `packet` | `type, netID, value, x, y, punchX, punchY, flags` | Every incoming tank packet (**heavy**) |
| `text` | `text` | Raw `GENERIC_TEXT` message |
| `game_message` | `text` | Raw `GAME_MESSAGE` (`action\|log` etc.) |
| `arrived` | `tileX, tileY` | A `walk` reached its target |
| `path_fail` | `reason, tileX, tileY` | A `walk` ended without arriving (no way, world changed, stopped...) |
| `tile_change` | `tileX, tileY, fg, bg` | A tile changed (broken, placed, updated) |
| `object_drop` | `uid, itemId, count, x, y` | Something was dropped in the world |
| `object_take` | `netID, itemId, count` | Another player picked something up |
| `collect` | `itemId, count, uid` | The bot picked something up |
| `*` | `eventName, values...` | All events (heavy) |

> 💡 An event that nobody listens to is never created (~0.5 ns cost). Only listen to heavy events like `packet` / `*` when you need them, and `off(...)` them when you're done.

---

### 📋 Status Values

| `bot.status` | Meaning |
|---|---|
| `idle` | Not started yet |
| `logging_in` | HTTP login (server_data / token) |
| `connecting` | Connecting to the server |
| `entering_game` | Login accepted, waiting for the menu |
| `menu` | In game, on the world menu |
| `joining` | Joining a world |
| `in_world` | Inside a world |
| `waiting` | Waiting (2FA, overload, maintenance, network error, login queue). Reason: `bot.statusText` |
| `offline` | Closed with `disconnect()`, waiting for `connect()` |
| `stopped` | Stopped (wrong password, out of retries, update required...). `connect()` restarts it |

---

### 📝 Example Scripts

#### Read bot info
```lua
local bot = getBot()
print(bot.name, bot.status, bot.world, bot.gems .. " gems", bot.ping .. " ms")
for _, item in ipairs(bot:getInventory()) do print(item.id, item.count) end
for _, p in ipairs(bot:getPlayers()) do print(p.name, p.tileX, p.tileY, p.moderator) end
```

#### Connect, warp & say
```lua
local bot = getBot()
if not bot.online then
    bot:connect()
    assert(bot:waitForOnline(60000), "login failed: " .. bot.statusText)
end
if bot:warpAndWait("START") then bot:say("hello!") end
```

#### All bots
```lua
for _, bot in ipairs(getBots()) do
    if not bot.online then bot:connect() sleep(3000) end
end
for _, bot in ipairs(getBots()) do
    if bot:waitForOnline(60000) then bot:setTargetWorld("MYWORLD") end
end
```

#### React to events
```lua
on("talk", function(bot, netID, name, text)
    if text == "!gems" then bot:say(name .. ", I have " .. bot.gems .. " gems") end
end)
on("moderator", function(bot, name)
    warn("moderator joined: " .. name)
    bot:leave()
end)
on("disconnected", function(bot, reason) log(bot.name .. " disconnected: " .. reason) end)
listen()
```

#### Wait for something
```lua
local bot = getBot()
bot:say("/time")
local b, text = bot:wait("console", 5000)
if b then print("server answer:", text) else print("no answer") end
```

#### Deposit items when there are enough
```lua
local bot = getBot()
while true do
    if bot.inWorld and bot:getItemCount(242) >= 50 then
        if bot:warpAndWait("DEPOSIT") and bot:walkAndWait(10, 20) then
            bot:drop(242)          -- all of them
            sleep(1000)
        end
    end
    sleep(5000)
end
```

#### Add a new bot
```lua
local bot, err = addBot("NEWBOT", "password", "socks5://user:pass@1.2.3.4:1080")
if not bot then error(err) end
bot:connect()   -- stays in the list until the program closes
```

More examples are in [`Docs/examples/`](Docs/examples) and the full API reference is in [`Docs/LUA_API.md`](Docs/LUA_API.md).

---

### ⚡ Performance Notes

| Operation | Cost |
|---|---|
| `bot.gems`, `bot.world` (property read) | 0.10 – 0.25 µs |
| `bot:getItemCount(id)` | 0.13 – 0.36 µs |
| `bot:getInventory()` | 0.25 – 0.53 µs |
| `bot:getPlayers()` | 0.8 – 1.7 µs |
| `bot:sendText(...)` (queue a command) | 0.3 – 0.8 µs |
| Events handled in Lua | 300 000 – 385 000 /s |
| World map parse (100x60) | ~71 µs |
| A* path (100x60 world) | 7 – 52 µs |

> ⚠️ **The game itself** doesn't like fast commands. Spamming warp / drop / say can get you kicked or banned, so put `sleep` between commands and space out logins (`connectAll()`).

> ⚠️ Standard libraries like `io` / `os` are enabled in scripts. Only run scripts you trust.

---

## ⚠️ Disclaimer
This project is for educational purposes only. Using bots may break Growtopia's Terms of Service. Use it at your own risk.
