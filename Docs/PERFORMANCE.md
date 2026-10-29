# Performance

GrowtopiaBot has its own test tools built in. None of them touch the real Growtopia server or any
account: the bots connect to a **fake ENet server** running on `127.0.0.1`. It imitates the
login → world → traffic flow, and its own CPU is subtracted from the measurements.

```
GrowtopiaBot.exe --bench [bots=50] [seconds=10] [packets/s per bot=20]   -> bench_result.txt
GrowtopiaBot.exe --bench-ui [bots=100] [seconds=8]                       -> bench_ui_result.txt (opens a window)
GrowtopiaBot.exe --selftest                                              -> selftest_result.txt (Lua API tests)
```

CPU is measured with `QueryProcessCycleTime` / `QueryThreadCycleTime` (cycle counter).
`GetProcessTimes` counts in 15.6 ms ticks, so it gives wrong results for threads that run in short
bursts (the event-driven bots, the idle panel).

## Results (12 cores, 100 bots, 20 packets/s of world traffic per bot)

### Bot network loop

| | Before (fixed 5 ms poll) | After (wake socket + 50 ms) | Difference |
|---|---|---|---|
| CPU (100 bots) | 61.3 % (of one core) | 14.5 % | **−76 %** |
| Wake-ups per bot | 205 /s | 31 /s | **−85 %** |
| Command latency (avg) | 2.76 ms | 0.36 ms | **7.7× faster** |
| Command latency (p99) | 37.8 ms | 2.28 ms | |
| Packets handled | 2207 /s | 2192 /s | same |

Other runs on an idle machine gave between −72 % and −82 %. On a busy machine the absolute values
change, but the before/after ratio held in every run.

**What changed:**
- Every bot thread used to wake up with `select()` **every 5 ms** (200/s per bot), whether anything
  happened or not. Now the thread sleeps on the ENet / proxy sockets and a **wake socket**
  (127.0.0.1 UDP). It wakes up **immediately** when a packet arrives or a command comes from the
  panel / a script (`Post()`). The timeout (50 ms) is only there for ENet's own timers
  (resends / pings), which run in ≥50 ms steps anyway.
- Offline / waiting bots wake up every 250 ms instead of 50 ms (still immediately on a command).
- The ping value is only written when it changes (the lock used to be taken on every loop).
- `Log::Debug` doesn't format the text at all while debug logging is off.
- Every bot used to keep a copy of the **whole bot list** from the settings (N bots → N² entries,
  passwords included). Now it only keeps the common settings (`Config::Common`).
- Console output (the Windows console is very slow) is off by default. Logs are kept in a ring
  buffer for the panel: ~2.5 million lines per second from 4 threads.

**Bugs fixed (found while testing):**
- *Missed wake-up:* when the wake flag was cleared **before** draining the socket, a command in
  between could wait 50 ms (250 ms when offline). The correctness test in `--bench` found
  **272 misses** in 6000 commands with the old order, **0** after the fix.
- *OnSpawn position:* `posXY` was parsed through a `string_view` of a temporary string (dangling).
  The bot's own X position could get corrupted at random. `--selftest` caught it, fixed.

### World data and pathfinding (`--bench` section 6)

The fake server's 100x60 map (with a wall), average of 2000 repetitions:

| Operation | Time |
|---|---|
| Reading the map packet (48 KB, 6000 tiles) | ~71 µs |
| A* around the wall (25 tiles) | ~7 µs |
| A* across the world (108 tiles) | ~11 µs |
| A* with no way to the target (searches the whole open area) | ~52 µs |

Every bot keeps its own world; 24 bytes per tile (extra data is only allocated for tiles that have
it, like doors, signs, locks and trees), so a 100x60 world is ≈ 144 KB. With 100 bots in a world the
network loop's CPU didn't change: on this machine the old loop used 11.3 %, the new loop **5.0 %**
(of one core), p99 command latency 3.1 ms.

**Bug found:** walk steps were first sent unreliably (like Mori). The fake server in `--selftest`
checks the position change of every state packet and caught 2–3 tile **teleports**: ENet's
throttle can drop unreliable packets, and a lost step looks like a teleport to the server. Steps
are now reliable; 6 runs, 0 teleports, 0 reach (5x5) violations.

### Lua API (one script, bot in a world)

| Operation | Cost |
|---|---|
| `bot.gems`, `bot.world` (property read) | 0.10 – 0.25 µs |
| `bot:getItemCount(id)` | 0.13 – 0.36 µs |
| `bot:getInventory()` | 0.25 – 0.53 µs |
| `bot:getPlayers()` | 0.8 – 1.7 µs |
| `bot:sendText(...)` (queue a command) | 0.3 – 0.8 µs |
| Creating an event (`Emit`) | 150 – 360 ns |
| Events handled in Lua | 300 000 – 385 000 /s |
| Checking an event nobody listens to | ~0.5 ns |

The ranges come from runs on an idle and a busy machine. Every script runs in its own thread; a
busy script never makes the bot threads wait.

### Panel (300 live bots)

| State | FPS | CPU (of one core) |
|---|---|---|
| In use (every frame is drawn) | 60–144 | ~3 % (0.12 ms UI + 0.09 ms DirectX per frame) |
| Idle / in the background | ~9–30 | ~1 % |
| Minimized / fully covered | 0 | ~0 % |

Start-up cost is paid once: DirectX + the font atlas, 0.4–1 s. Bot data is read 10 times a second,
not every frame. Long lists (bot table, log, script output) only draw the visible rows with
`ImGuiListClipper`.

### Script line tracing (the colors in the editor)

Only the **one** script started from the editor (Run / F5) is traced; every other script
(`runScript`, the other bots of "Run for every bot") runs without the line hook at zero cost.
In the traced script every new line means one hook call:

| Plain Lua loop (3 lines per round) | rounds/s |
|---|---|
| Tracing off | ~67 million |
| Tracing on | ~9 million (~30 ns per line) |

In normal scripts, which mostly call the bot API / `sleep`, there is no noticeable difference.
