# HSMMod: HalfSwordMP drop-in mod

One DLL, no game files changed. It installs as `dwmapi.dll` next to the game exe (a DLL the game loads at
startup, the same load point UE4SS uses), forwards every dwmapi export to the real system DLL, and (when enabled in
`HSMMod.ini`) adds host-authoritative online play for up to 6 fighters, each in their own loadout. Delete the DLL to
return to vanilla. (Earlier builds used `xinput1_4.dll`, which the game only loads on its first controller poll, so
the mod never started on PCs without a gamepad; delete any old copy.)

Supported build: Steam 24185754 (exe key `0x8EFA122909712000`). On any other build the mod logs
"unsupported build" and does nothing.

## Build and checks (Linux, no MSVC)

```bash
./build.sh                                  # needs llvm-mingw in ~/.local/opt (see ../../../../re/README.md)
../../shared/run_tests.sh                   # all game-independent logic under ASan/UBSan
.venv/bin/python tools/mutate_shared.py     # from the repo root: the tests must kill known bugs
```

Output: `build/dwmapi.dll`. The repo's workflow (`AGENTS.md`) runs every sensor with
`.agents/workflow/tools/workflowctl.py verify`.

## Steam lobbies (default, Phase 1)

- `role=steam` (the default in a fresh `HSMMod.ini`) adds a **MULTIPLAYER** button under Free Mode on the main menu.
- The Multiplayer panel offers: Host lobby (friends or public), Invite friends (Steam overlay), Find public lobbies,
  and Leave. Friends can also join from the Steam friends list (rich presence) or an invite, which launches with
  `+connect_lobby`.
- Joined players wait on their own main menu. When the host starts any fight or enters the Tavern hub, every lobby
  member's game follows automatically, with the host's mode settings copied. Clients cannot start fights on their own
  while joined, but can always return to the menu. A player who joins while the host is already in a level is sent
  straight there.
- Traffic goes through Steam's relay network, so no port forwarding is needed. Lobbies and joins are checked for mod
  protocol and game build, and incompatible lobbies show the reason.

## Up to 6 fighters with their own loadouts (Phase 2)

- **Loadouts**: each player's own fighter is exported from their game (Free Mode: `Free Mode Player Character 1`;
  Progression: `Player Character`) into an `SG_GameProgress_C` SaveGame serialized in memory. The engine deep-copies
  armor and weapon maps and reloads classes by path on the other side. Blobs of up to 256 KiB are chunked and
  CRC-checked; the host gives every slot a fresh blob id and relays it to all players.
- **Host**: slot 0 is the host's stock fighter. Each client gets a Willie spawned like the stock spawner does it
  (deferred spawn, passport copied before `FinishSpawningActor`, the same height scaling), possessed by a plain
  `AIController` with no brain. The client's movement, arm and action input drive it through Willie's own
  `InpAxisEvt_*` / `InpActEvt_*` events, and the client's camera aim through `SetControlRotation`. Spawn spots
  spread out from the host over the level's `BP_SpawnerPoint_Willies` (farthest-point sampling, ring offsets when
  points run out). A fighter spawns once the client reports Ready in the level (20 s timeout) and its loadout has
  arrived (5 s grace, then the stock player-2 loadout).
- **Clients** fight as their own stock player 0, which mirrors the host's pose for it. Every other fighter appears as
  a brainless puppet wearing that player's loadout, driven by host snapshots.
- The game's stock local "player 2" is no longer used.

| Area | Behaviour |
|---|---|
| Load / vanilla safety | XInput proxy; `role=off` or an unknown build = pure passthrough |
| Engine access | Runtime reflection by name; startup self-test (`Add_IntInt` layout, object index round trip, a ProcessEvent call) or it disables itself |
| Transport | Steam relay (default) or ENet UDP: reliable channel (hello/welcome/travel/spawn/blobs/events), unreliable channel (input 60 Hz, snapshots 30 Hz); every packet decoded and validated before the game thread sees it |
| Host | Up to 6 fighters; snapshots 23 body bones + vitals per fighter to every client; announces spawns, despawns and deaths |
| Client | Captures its input and blocks it locally (camera look/turn still local); velocity-tracks its fighter and puppets to host poses; copies vitals; plays deaths on host events |
| Damage authority | Clients block all `...ComponentHitSignature` delegates while joined, so Chaos contacts never run damage locally |

## Install (every player)

1. Copy `build/dwmapi.dll` to `Half Sword/HalfswordUE5/Binaries/Win64/`.
2. Proton/Linux only: set Steam launch options to `WINEDLLOVERRIDES="dwmapi=n,b" %command%`. Launch through Steam:
   starting Proton by hand outside Steam's runtime falls back to software rendering or crashes the D3D12 renderer.
3. Start the game once to create `HSMMod.ini` (same folder; default `role=steam`). For direct IP instead of Steam, edit it:
   - Host: `role=host`, and forward UDP `27777` (or your `port`) to the host machine.
   - Client: `role=client`, `host=<host IP>`.
4. Logs go to `HSMMod.log` in the same folder.
5. Every player needs the same HSMMod build. The lobby names anyone on another build ("Bob on 0.6.3 - update") and the
   host cannot start a match until it matches; `allow_build_mismatch=1` in `HSMMod.ini` overrides this for testing.

## Matches, teams and bots (Phase 3)

- **Lobby settings** (Multiplayer panel, host edits, everyone sees them): mode (Free-for-all, 2 teams, 3 teams,
  Players vs bots), score limit, optional time limit, friendly fire (off by default). Each player picks a team
  ("My team"); the host clicks a player's row to move them, and "Auto-balance teams" clears the host's moves.
  Players without a pick go to the smallest team when the match starts.
- **Match flow** (host-run, in any fight level except the Tavern hub, with 2+ players or Players vs bots):
  5 s countdown, then one point per kill for the killer's team (or the player in Free-for-all). A teamkill or
  suicide costs a point; deaths with nobody to blame cost nothing. The first to the score limit wins; with a time
  limit the leader wins when time runs out, and a tie goes to sudden death. A 10 s results screen is followed by an
  in-place rematch (everyone respawns, no level reload).
- **Duel** is fought in rounds: the last one standing takes the round, and the first to the score limit wins. After
  the respawn delay everyone comes back together, and the weapons dropped in earlier rounds are cleared. Hold **G**
  for 1.5 s during a round to give it up: you are out of that round (one on one, it goes to the other), no kill is credited, and the kill
  feed says "gave up". G is also the game's talk key: while it is held the game's own give-up already shows on your
  fighter (it slows down and cowers), and letting go before 1.5 s cancels both.
- **Kill credit** comes from weapon hit events: the last weapon holder to hit a fighter within 10 s gets the kill.
  With friendly fire off, weapon hits between teammates are dropped before the game computes damage.
- **Respawns** after the respawn delay at the team's side of the map: the level's Willie spawn points are split
  into one contiguous arc per team, and each respawn picks the point farthest from living enemies.
- **Team colours**: armor fabric is tinted red / blue / green / gold on every machine; Free-for-all keeps
  everyone's own colours.
- **Players vs bots**: the host's stock foes become synced bots, and each cleared wave brings one more bot of a
  higher rank (up to 10 at a time) from the game's own random-character generator. Bots never score; the bots win
  when every player is down at the same time.
- **Progression**: fights are co-op against the event's opponents (synced as bots). When they are all down, or every
  player is, the host returns everyone to the Tavern hub.
- **One world for everyone**: stock fighters that are not part of the match are removed on every machine, and the
  stock death/win screens are blocked during a session match (no slow motion, no input lock, and no Progression
  career reset on death).
- **Pause menu**: in an owner-sync match (the default), Esc still opens the game's pause menu and it works, but the
  fight does not pause. A paused game stopped your fighter on every other screen, and a host's pause sent the clients
  back to the main menu after 7 s.
- **Dropped weapons and armour are cleared** in an owner-sync match, so a long fight does not pile them up: in Duel at
  each new round, in Free-for-all and teams at each rematch, and in Survival and Zombies a minute after each piece was
  dropped (standing next to it does not keep it). Level props (barrels, stools) and armour the level already had stay.
- **Survival loot**: about 2 s after a bot falls it lets go of its weapons and sheds two of its armour pieces for the
  players to pick up; its body goes 45 s later with what it still wore. The break between waves is a minute of
  looting; hold **G** to vote to skip it, and the next wave starts once every player has voted. Zombies keep their
  stock disintegration and a 10 s break.
- **HUD** inside the game's own `UI_HUD`: score line and clock, respawn countdown, kill feed, and a scoreboard while
  Tab is held.

## Command-line options and the agent bridge (testing)

All optional; several instances can share one install.

| Flag | Effect |
|---|---|
| `-hsmrole=host\|client\|steam\|off`, `-hsmhost=IP`, `-hsmport=N`, `-hsmname=NAME` | override `HSMMod.ini` |
| `-hsmtag=NAME` | per-instance files: `HSMMod-NAME.log`, `HSMTest-NAME.json` |
| `-hsmauto=/Game/Maps/...` | a host/solo instance opens that Free Mode level by itself |
| `-hsmquit=SECONDS` | quit cleanly after that long |
| `-hsmtest=all` | run the in-game probes (below) |
| `-hsmbridge=PORT` | agent bridge on `127.0.0.1:PORT` (never off-machine) |
| `-hsmmirror=DELAY,JITTER,LOSS[,OFFSET]` | offline: the mirror test bench (a stand-in of your own fighter fed through a simulated link, see `mirror.h`) comes on in every fight level, e.g. `-hsmmirror=80,10,1` |

The bridge takes one command per line and answers with one JSON line (`tools/hsmctl.py PORT CMD...`):
`state`, `widgets`, `threads`, `stack [thread-id]`, `window`, `key NAME [tap|down|up]`, `click X Y`,
`axis NAME VALUE MS`, `action NAME down|up|tap`, `open LEVEL`, `pause on|off`, `exec CONSOLE-COMMAND`, `shot`, `quit`,
`prof start [SECONDS] [TIMELINE_MIN_US]|stop|status` (the Blueprint profiler, below).
`stack` suspends a thread for a moment and returns its unwound `module+offset` frames; resolve them with the Ghidra
project in `re/`. If the game falls behind loading or rendering, a player pawn is spawned by the mod itself.

## Blueprint profiler

Shows where the game thread's time goes: every Blueprint function the game runs, every ProcessEvent call (ticks,
input, hit and overlap delegates, timelines), and HSMMod's own hooks and subsystems. It works in any role except
`role=off`, solo or online.

1. In game, press **Ctrl+Shift+P**. A beep (and, in a match, a chat line) confirms the start.
2. Play the part that is slow. The capture stops by itself after 30 s or when the level changes, or press
   Ctrl+Shift+P again.
3. Next to the game exe (`Binaries/Win64`) there are now two files:
   - `HSMProfile-<date>-<time>.txt`: frame times (avg, p50, p95, p99, max), the top functions by **self** time (the
     function's own Blueprint nodes and the native engine calls it makes directly) and by **inclusive** time (with
     everything it called), per frame, with call counts and the longest single call, and the 10 slowest frames.
   - `HSMProfile-<date>-<time>.json`: a timeline of every call of 20 µs or more and every frame. Open it in
     <https://ui.perfetto.dev> (or `chrome://tracing`, or speedscope) and jump to a slow frame from the text report.

Kinds in the report: `event` = called through ProcessEvent, `bp` = a Blueprint function body run by the script VM
(also when another Blueprint called it), `hook` = HSMMod's hooks on that function, `mod` = an HSMMod subsystem
(`HSMMod session tick`, `HSMMod pump: ...`). Native engine work that no Blueprint asked for (physics simulation,
animation, rendering) happens outside these calls: compare the frame time with the top inclusive rows to see how
much that is.

Costs: nothing measurable when no capture runs. While recording, every Blueprint call costs about two timestamps
and a table lookup on the game thread; the report prints the measured cost per call and the estimated total, which
is included in the times shown, so frames run somewhat slower while recording. Stopping writes the files on the
game thread (a short hitch; the timeline is capped at 1 million calls). The agent bridge's `prof start 120 50`
records for 120 s (at most 300) with a 50 µs timeline threshold (`0` keeps every call).

## In-game test harness

Add `-hsmtest=all` to the launch options (or a comma list of probe names), load into any Free Mode level and wait
about 6 seconds. The harness spawns a test fighter and a test puppet next to you, checks them, removes them, and
writes `HSMTest.json` next to the exe. Check it from the repo root:

```bash
.venv/bin/python tools/check_hsmtest.py "<game>/HalfswordUE5/Binaries/Win64/HSMTest.json"
```

| Probe | Checks |
|---|---|
| `selftest` | Reflection layout and ProcessEvent |
| `passport_export` | Your passport serializes through `SG_GameProgress_C` |
| `passport_roundtrip` | Load-from-memory then save-to-memory reproduces the exact bytes |
| `passport_reject_garbage` | Bytes that are not a SaveGame are refused |
| `spawn_points` | The level has Willie spawn points (0 is expected outside arenas) |
| `spawn_fighter` | A mod-spawned fighter gets a plain `AIController` and `Player=true` |
| `spawn_puppet` | A puppet spawns with no controller |
| `puppet_drive` | Snapshot driving moves a puppet 150 cm toward a target |
| `tint_armor` | Team colour reaches the test fighter's armor (fails on a bare loadout) |
| `spawn_bot` | The random generator yields a bot with the stock NPC brain |
| `weapon_owner` | Weapons you hold resolve back to your fighter (kill credit) |

## Known gaps (next milestones)

- The sections above describe the original host-simulates-everyone sync (`sync_mode=host`). In owner sync (the
  default) each machine simulates its own fighter, the others are stand-ins driven by a 60 Hz pose stream, hits are
  claimed by the attacker and replayed by the victim's machine through the game's own damage code, and gore, grabs,
  dropped weapons and level props are synced; the client-side delay and the unsynced gore listed below apply to
  `sync_mode=host` only.
- Loadouts with no fabric armor show no team colour (adding a tabard is not done yet).
- Progression rewards (payout, rank points, injuries) are not yet applied to clients' own saves after co-op fights.
- Direct-IP (ENet) sessions have no lobby, so they always play Free-for-all with default settings.
- Next: a spectator host (`role=server`): the retail game plus this mod hosting with no fighter of its own and
  automatic match start/rotation. It is a hosted retail instance, not a dedicated server build.
- `sync_mode=host`: no client prediction (the client's own Willie responds with one round trip of delay); weapon
  transforms, grab constraints, dismemberment, gore and level props are not synced, and blocking hit delegates mutes
  client-side hit sounds and blood.
- SaveGame bytes returned by the engine are copied and left on the engine heap (a few KB per export, at most once
  per level per player).
