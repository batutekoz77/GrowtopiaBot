# GrowtopiaBot Lua API

GrowtopiaBot lets you control the bots with **Lua 5.4** scripts: connect / disconnect, warp, chat,
drop, walk (pathfinding), punch/place, collect dropped items, send raw packets and read everything
about a bot (name, inventory, world, tiles, players, gems, ping...). You can attach handlers to
in-game events (`console`, `talk`, `world_enter`, `disconnected`...).

- Scripts live in the **`scripts/`** folder next to `GrowtopiaBot.exe` (`.lua`).
- Write, save (Ctrl+S), load and run (F5) them from the **Scripts** tab of the panel.
- The **Lua >** line at the bottom of the same tab runs one line at a time: type `getBot().gems`
  and it prints the result.
- Examples: `Docs/examples/` (copied into `scripts/` during the build).

---

## Contents

1. [How it works](#1-how-it-works)
2. [Global functions](#2-global-functions)
3. [Bot object — properties](#3-bot-object--properties)
4. [Bot object — methods](#4-bot-object--methods)
5. [Events](#5-events)
6. [Status values](#6-status-values)
7. [Examples](#7-examples)
8. [Performance notes and limits](#8-performance-notes-and-limits)

---

## 1. How it works

- Every script runs **in its own thread and its own Lua state**. An infinite loop in one script
  slows down neither the bots nor the panel; you can run as many scripts at once as you like.
- Bot commands (`warp`, `say`, `drop`...) are **queued** and run on the bot's own thread; the
  function returns right away. Use the `waitFor...` functions or `wait()` to wait for the result.
- Reads (`bot.world`, `bot.gems`...) always return the **current** value (locked, thread-safe).
- **Event handlers** (`on(...)`) only run while the script is inside `sleep()`, `wait()`,
  `listen()` or a `waitFor...` call. So handlers never run at the same time as your main code —
  no locks, no race conditions.
- When a script is stopped (**Stop** in the panel or `stopScript(id)`), the running code is
  interrupted within a few milliseconds wherever it is (infinite loop, `sleep`, inside `pcall`).
- `getBot()` without an argument returns the bot **selected in the panel** (or the first bot in
  the list). The "Run for every bot" button starts one copy of the script per bot; in each copy
  `getBot()` returns that bot.

On an error the script stops, and the error is written with its line number to the **Output**
panel and the log.

---

## 2. Global functions

### Output

| Function | Description |
|---|---|
| `print(...)` | Writes to the script Output panel (values separated by tabs). |
| `log(...)` | Writes to Output **and** the main Log tab (tag: `LUA:<script>`). |
| `warn(...)` | Like `log`, but as a warning (yellow). |

### Time and flow

| Function | Returns | Description |
|---|---|---|
| `sleep(ms)` | – | Waits `ms` milliseconds; runs event handlers meanwhile. |
| `millis()` | integer | Milliseconds since the script started. |
| `listen([ms])` | – | Waits while only processing events. No argument: until the script is stopped. |
| `wait(event, [timeoutMs], [bot])` | `bot, values...` or `nil` | Waits for an event (see below). No / negative `timeoutMs` = forever. |
| `exit()` | – | Ends the script without an error. |
| `scriptId()` | integer | This script's id. |
| `scriptName()` | string | This script's name (file name). |

`pcall` / `xpcall` work as in normal Lua; the only difference is that they can't catch the
script being **stopped** (so even `while true do pcall(sleep, 100) end` can be stopped).

### Accessing bots

| Function | Returns | Description |
|---|---|---|
| `getBot()` | Bot | The bot selected in the panel (or the bot given to the script). |
| `getBot(n)` | Bot / nil | The `n`th bot in the list (starts at 1). |
| `getBot("name")` | Bot / nil | By GrowID (case-insensitive), then by in-game name. |
| `getBotById(id)` | Bot / nil | By the fixed bot id (`bot.id`). |
| `getBots()` | array of Bot | All bots, in list order. |
| `addBot(growid, password, [proxy])` | Bot or `nil, error` | Adds a new bot (doesn't start it; call `bot:connect()`). |
| `removeBot(bot \| name \| n)` | boolean | Stops the bot and removes it from the list. |
| `connectAll()` | – | Connects every offline bot, spaced by `login_interval_ms`. |
| `disconnectAll()` | – | Disconnects every bot (they stay offline). |

Proxy formats: `socks5://user:pass@host:port`, `socks5://host:port`, `host:port:user:pass` or `host:port`.

### Events

| Function | Returns | Description |
|---|---|---|
| `on(event, function(bot, ...) end)` | handler id | Adds a handler for all bots. `event = "*"` receives every event: `function(bot, eventName, ...)`. |
| `off(id)` | removed count | Removes one handler. |
| `off(event)` | removed count | Removes every handler of that event. |

### Script management

| Function | Returns | Description |
|---|---|---|
| `runScript(file, [bot])` | id or `nil, error` | Starts another script from `scripts/` in its own thread. |
| `stopScript(id)` | – | Stops a script. |
| `getScripts()` | array | `{ id, name, running, seconds }` tables. |

### Item info

| Function | Returns | Description |
|---|---|---|
| `getItemName(id)` | string | `"Dirt"`, or `"#2"` while items.dat isn't loaded. |
| `getItemId(name)` | integer / nil | Case-insensitive: `getItemId("dirt seed")`. |
| `getItemInfo(id \| name)` | table / nil | `{ id, name, type, collision, category, rarity, hits, growTime, background, info }`. |
| `itemsReady()` | boolean | items.dat is loaded. |

Item names come from `items.dat`: first the `items.dat` next to `GrowtopiaBot.exe`, otherwise the
game's cache (`%LOCALAPPDATA%\Growtopia\cache\items.dat`). While it isn't loaded, names are `"#id"`.

---

## 3. Bot object — properties

All are read-only, and every read returns the current value: `bot.world`, `bot.gems`...

| Property | Type | Description |
|---|---|---|
| `id` | integer | Fixed bot id (`#` in the panel). Same as `index`. |
| `growid` | string | GrowID. |
| `name` | string | In-game name (no color codes). |
| `status` | string | Status key, see [section 6](#6-status-values). |
| `statusText` | string | Why it's waiting / stopped ("server overloaded", "wrong GrowID / password"...). |
| `online` | boolean | In game (menu, joining or in_world). |
| `inWorld` | boolean | Inside a world. |
| `running` | boolean | The bot thread is running (not stopped). |
| `world` | string | Current world (`""` when outside). |
| `worldWidth`, `worldHeight` | integer | World size in tiles. |
| `targetWorld` | string | World it returns to after a kick / reconnect. |
| `x`, `y` | number | Position in pixels. |
| `tileX`, `tileY` | integer | Position in tiles (the avatar's centre, same rule as AquaProxy). |
| `gems` | integer | Gem count. |
| `level` | integer | Level. |
| `ping` | integer | ms. |
| `netID`, `userID` | integer | Ids given by the server. |
| `inventorySize` | integer | Inventory slot count. |
| `proxy` | string | `host:port` (password hidden) or `""`. |
| `autoReconnect` | boolean | Reconnects automatically when disconnected. |
| `walking` | boolean | A `walk` is in progress. |
| `autoCollect` | boolean | Collects dropped items by itself. |

`tostring(bot)` → `Bot(#1 GROWID)`. Two bot objects can be compared with `==`.

---

## 4. Bot object — methods

Methods are called with `:`: `bot:warp("START")`.

### Connection

| Method | Description |
|---|---|
| `bot:connect([delayMs])` | Logs in. Restarts the bot if it is stopped or offline. |
| `bot:disconnect()` | Disconnects and stays **offline** (doesn't reconnect by itself). |
| `bot:reconnect()` | Disconnects and logs in again right away. |
| `bot:setAutoReconnect(bool)` | Reconnect automatically when the server drops the bot (default `true`). |

### Movement and world

| Method | Description |
|---|---|
| `bot:warp(world, [doorId])` | Warps to a world (once). |
| `bot:warpAndWait(world, [timeoutMs=30000], [doorId])` | Warps and waits until it's inside → `true/false`. |
| `bot:leave()` | Leaves the world (to the world menu). |
| `bot:setTargetWorld(world)` | World to stay in; it returns there after kicks / reconnects. `""` = off. |
| `bot:walk(tileX, tileY)` | Walks to that tile **with pathfinding** (A*) and returns right away. A new `walk` replaces the running one. |
| `bot:walkAndWait(tileX, tileY, [timeoutMs=30000])` | Walks and waits until it arrives → `true` / `false, reason`. |
| `bot:stopWalk()` | Stops walking. |
| `bot:findPath(tileX, tileY)` | Computes the route without walking → `{ {x, y}, ... }` (both ends included) / `nil, reason`. |
| `bot:move(dx, [dy=0])` | Walks relative to the current tile (same as `walk`). |
| `bot:respawn()` | Respawns. |
| `bot:enter(tileX, tileY)` | Enters the door / portal on that tile (if inside the 5x5 reach) → `boolean`. |

> **Pathfinding (AquaProxy's `/path`):** A* runs on the world's real tile data: solid blocks,
> closed doors, entrances we have no access to, VIP / friends entrances, one-way blocks and
> platforms are respected, and harmful tiles like fire / spikes are avoided when possible. The bot
> moves **one tile per packet** (two steps back to back, then a random 80–200 ms pause), and the
> steps are sent reliably (a lost step would look like a teleport to the server). If the route
> closes (a door shuts, a block is placed, the server moves the bot), it plans again from where it
> really is (at most 25 times). Without items.dat every foreground block counts as solid, so it only
> walks through air.

### Chat and dialogs

| Method | Description |
|---|---|
| `bot:say(text)` / `bot:talk(text)` | Writes to chat. `/commands` work too (`bot:say("/wave")`). |
| `bot:dialogReturn(name, fields)` | Sends `action|dialog_return`. `fields`: `{ key = value }` or, when order matters, `{ {"key", value}, ... }`. |

### Inventory and items

| Method | Returns | Description |
|---|---|---|
| `bot:getInventory()` | array | `{ id, name, count, flags }` tables. |
| `bot:getItemCount(id)` | integer | How many of that item it has. |
| `bot:hasItem(id, [count=1])` | boolean | |
| `bot:drop(id, [count])` | boolean | Drops the item. No `count` = all of them. `false` if there aren't enough. |
| `bot:trash(id, [count])` | boolean | Trashes the item. No `count` = all of them. |
| `bot:wear(id)` / `bot:unwear(id)` / `bot:use(id)` | – | Wear / unwear / use (same packet: item activate). |

### Acting on the world

| Method | Returns | Description |
|---|---|---|
| `bot:punch(tileX, tileY)` | boolean | Punches a tile. |
| `bot:place(tileX, tileY, itemId)` | boolean | Places a block / seed (does nothing if it isn't in the inventory). |
| `bot:wrench(tileX, tileY)` | boolean | Uses the wrench on a tile. |
| `bot:inReach(tileX, tileY)` | boolean | The tile is inside the bot's 5x5 reach. |

> ⚠️ **The 5x5 rule (ban protection):** the game only punches / places / wrenches / enters doors
> inside the 5x5 square around the player (at most 2 tiles on either axis). A character spawned
> with mstate / smstate can reach further, but the server flags those packets and **bans**.
> AquaProxy blocks this by dropping the far packets; the bot does the same: every tile action
> outside the 5x5 square (raw `sendPacket` included) is **not sent**, and a warning goes to the
> Log. A block can't be placed on the bot's own tile (a background can). The return value is that
> check at call time; if you are too far, get closer with `walkAndWait` first.

### Tile data and dropped items

| Method | Returns | Description |
|---|---|---|
| `bot:getTile(tileX, tileY)` | table / nil | One tile (`nil` outside the world). |
| `bot:getTiles()` | array | Every tile in the world (100x60 = 6000 tables; don't call it often). |
| `bot:findTiles(itemId)` | array | Tiles with that item as foreground or background: `{ {x, y}, ... }`. |
| `bot:getObjects()` | array | Dropped items in the world. |
| `bot:collect([radius=2])` | integer | Collects the items within `radius` tiles (at most 2) and returns how many are in range. Skips what doesn't fit (full stack of 200, full backpack). |
| `bot:setAutoCollect(bool, [radius=2])` | – | Collects by itself while walking, when something drops and every 0.5 s. |

Tile table: `{ x, y, fg, bg, fgName, bgName, flags, extraKind, walkable }`, plus `label` on doors /
signs, `owner` (userID) on locks and `age` (seconds), `fruit` and `ready` (fully grown) on trees.
Object table: `{ uid, id, name, count, x, y, tileX, tileY }`.

Tiles are read from the map packet when the bot enters a world and kept up to date with the
server's tile updates (punches, places, tile updates, harvested trees).

### Players

| Method | Returns | Description |
|---|---|---|
| `bot:getPlayers()` | array | The other players in the world. |
| `bot:getPlayer(name \| netID)` | table / nil | |

Player table: `{ netID, userID, name, country, x, y, tileX, tileY, moderator }`.

### Status queries and waiting

Waiting methods run event handlers while they wait.

| Method | Returns | Description |
|---|---|---|
| `bot:getInfo()` | table | Every property from section 3 in one table. |
| `bot:isOnline()` | boolean | Same as `bot.online`. |
| `bot:isInWorld([world])` | boolean | In a world (or in the given world). |
| `bot:waitForOnline([timeoutMs=60000])` | boolean | Waits until it's logged in. |
| `bot:waitForWorld([world], [timeoutMs=30000])` | boolean | Waits until it enters a world (or the given one). |
| `bot:waitForStatus(status, [timeoutMs=30000])` | boolean | Waits until `bot.status == status`. |
| `bot:on(event, function(bot, ...) end)` | handler id | Events of **this bot only**. |
| `bot:wait(event, [timeoutMs])` | `bot, values...` / nil | Waits for an event of this bot only. |

### Raw packets (advanced)

| Method | Description |
|---|---|
| `bot:sendText(text)` | `NET_MESSAGE_GENERIC_TEXT` (2). E.g. `"action|respawn\n"`. |
| `bot:sendGameMessage(text)` | `NET_MESSAGE_GAME_MESSAGE` (3). E.g. `"action|quit_to_exit\n"`. |
| `bot:sendPacket(table)` | Tank packet (`NET_MESSAGE_GAME_PACKET`). |

`sendPacket` fields (all optional, default 0): `type, objectType, count1, count2, netID,
targetNetID, flags, floatValue, value, x, y, xSpeed, ySpeed, particle, punchX, punchY`, and
`reliable` (default `true`). Tile packets go through the same 5x5 reach guard, and a state packet
moves the bot's position to its `x, y`.

```lua
-- state packet with a punch animation
bot:sendPacket({ type = 0, x = bot.x, y = bot.y, punchX = 10, punchY = 20, flags = 0x200 | 0x20, reliable = false })
```

---

## 5. Events

The first handler argument is **always the bot object**, followed by the values in the table:

```lua
on("console", function(bot, text, raw) print(bot.name, text) end)
local bot, world, w, h = wait("world_enter", 10000)
```

| Event | Values (after bot) | When |
|---|---|---|
| `status` | `status, statusText` | The status changed. |
| `connected` | – | ENet connection established (before login). |
| `login` | – | Login accepted, entering the game. |
| `logon_fail` | `reason` | Login rejected (2FA, overload, maintenance...). |
| `disconnected` | `reason` | Connection lost. |
| `world_enter` | `world, width, height` | Entered a world. |
| `world_menu` | `leftWorld` | Back at the world menu (left / kicked). |
| `join_fail` | `world` | Couldn't enter the world. |
| `spawn` | `netID, userID, x, y` | The bot spawned in the world. |
| `player_join` | `netID, name, userID, country, isModerator` | Someone entered the world. |
| `player_leave` | `netID, name` | Someone left the world. |
| `moderator` | `name, netID` | A moderator / invisible player entered the world. |
| `console` | `text, rawText` | Console message (`text` without color codes). |
| `talk` | `netID, name, text` | Chat bubble (including your own). |
| `dialog` | `dialogText` | The server opened a dialog (`OnDialogRequest`). |
| `inventory` | `itemCount, slotCount` | The full inventory arrived. |
| `item` | `itemId, change, newCount` | Item gained (+) / lost (−). |
| `gems` | `gems` | The gem count changed. |
| `variant` | `functionName, arg1, arg2, ...` | **Every** `OnXxx` call from the server (vec2/vec3 values as `"x, y"` text). |
| `packet` | `type, netID, value, x, y, punchX, punchY, flags` | Every incoming tank packet. **Heavy**: only listen when you need it. |
| `text` | `text` | Raw `GENERIC_TEXT` message. |
| `game_message` | `text` | Raw `GAME_MESSAGE` (`action|log` etc.). |
| `arrived` | `tileX, tileY` | A `walk` reached its target. |
| `path_fail` | `reason, tileX, tileY` | A `walk` ended without arriving (no way, world changed, stopped...). |
| `tile_change` | `tileX, tileY, fg, bg` | A tile changed (broken, placed, updated). |
| `object_drop` | `uid, itemId, count, x, y` | Something was dropped in the world. |
| `object_take` | `netID, itemId, count` | Another player picked something up. |
| `collect` | `itemId, count, uid` | The bot picked something up. |
| `*` | `eventName, values...` | Everything (heavy). |

Notes:

- An event nobody listens to is **never created** (~0.5 ns cost). Only listen to heavy events like
  `packet` / `*` when you need them, and `off(...)` them when you're done.
- Each script's event queue holds at most 20 000 events; if a script stays busy for a long time
  without calling `sleep`/`wait`, the oldest events are dropped (the panel shows a "dropped" count).
- `sleep`/`wait` can be used inside a handler; events arriving meanwhile are handled after it.
- If a handler fails, the error goes to Output and the script **keeps running**.

---

## 6. Status values

| `bot.status` | Meaning |
|---|---|
| `idle` | Not started yet. |
| `logging_in` | HTTP login (server_data / token). |
| `connecting` | Connecting to the server. |
| `entering_game` | Login accepted, waiting for the menu. |
| `menu` | In game, at the world select menu. |
| `joining` | Joining a world. |
| `in_world` | Inside a world. |
| `waiting` | Waiting (2FA, overload, maintenance, network error, login queue). Reason: `bot.statusText`. |
| `offline` | Closed with `disconnect()`; waiting for `connect()`. |
| `stopped` | Stopped (wrong password, out of login attempts, update required, disconnected while auto-reconnect is off). `connect()` restarts it. |

---

## 7. Examples

### Read bot info

```lua
local bot = getBot()
print(bot.name, bot.status, bot.world, bot.gems .. " gems", bot.ping .. " ms")
for _, item in ipairs(bot:getInventory()) do print(item.id, item.name, item.count) end
for _, p in ipairs(bot:getPlayers()) do print(p.name, p.tileX, p.tileY, p.moderator) end
```

### Connect, warp, chat

```lua
local bot = getBot()
if not bot.online then
    bot:connect()
    assert(bot:waitForOnline(60000), "login failed: " .. bot.statusText)
end
if bot:warpAndWait("START") then bot:say("hello!") end
```

### All bots

```lua
for _, bot in ipairs(getBots()) do
    if not bot.online then bot:connect() sleep(3000) end
end
for _, bot in ipairs(getBots()) do
    if bot:waitForOnline(60000) then bot:setTargetWorld("MYWORLD") end
end
```

### React to events

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

### Wait for something

```lua
local bot = getBot()
bot:say("/time")
local b, text = bot:wait("console", 5000)
if b then print("server answer:", text) else print("no answer") end
```

### Drop items when there are enough

```lua
local bot = getBot()
while true do
    if bot.inWorld and bot:getItemCount(242) >= 50 then
        bot:warpAndWait("DEPOSIT")
        if bot:walkAndWait(10, 20) then
            bot:drop(242)          -- all of them
        end
    end
    sleep(5000)
end
```

### Walk, break, collect

```lua
local bot = getBot()
local ok, why = bot:walkAndWait(30, 20)
if not ok then error("couldn't get there: " .. why) end
local tile = bot:getTile(31, 20)
print(tile.fgName, tile.walkable)
while bot:getTile(31, 20).fg ~= 0 do
    bot:punch(31, 20)          -- inside the 5x5 reach
    sleep(250)
end
sleep(500)
print("collecting:", bot:collect())
```

### Fill in a dialog

```lua
bot:dialogReturn("drop_item", { {"itemID", "242|"}, {"count", 10} })
```

### Add a new bot

```lua
local bot, err = addBot("NEWBOT", "password", "socks5://user:pass@1.2.3.4:1080")
if not bot then error(err) end
bot:connect()   -- stays in the list until the program closes; nothing is saved to disk
```

More: `Docs/examples/` (`example_basic.lua`, `example_events.lua`, `example_multibot.lua`,
`example_deposit.lua`).

---

## 8. Performance notes and limits

- Read cost: `bot.gems` ≈ 0.1 µs, `bot:getInventory()` ≈ 0.25 µs, `bot:getPlayers()` ≈ 0.8 µs.
  Fine even in tight loops (measurements: `Docs/PERFORMANCE.md`).
- Queuing a command (`say`, `drop`...) ≈ 0.3 µs; the bot thread wakes up immediately (~0.3 ms).
- Reading a map ~70 µs, A* 7–50 µs (100x60 world, `--bench` section 6); the CPU use doesn't change
  even with 100 bots in worlds.
- **The game itself** doesn't like fast commands: spamming warp / drop / say can get you kicked or
  banned. Put `sleep` between commands and space out logins (e.g. `connectAll()`).
- Punches / places / wrenches / door entries outside the 5x5 reach and pickups of items more than
  2 tiles away are **never sent**.
- Standard libraries like `io` / `os` are enabled in scripts (files can be read and written) — only
  run scripts you trust.
