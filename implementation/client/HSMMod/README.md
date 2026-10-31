# HSMMod: HalfSwordMP drop-in mod

One DLL, no game files changed. It installs as `dwmapi.dll` next to the game exe (a DLL the game loads at
startup, the same load point UE4SS uses), forwards every dwmapi export to the real system DLL, and (when enabled in
`HSMMod.ini`) adds host-authoritative online play for up to 8 fighters, each in their own loadout. Delete the DLL to
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

Output: `build/dwmapi.dll` and `build/dwmapi.map`. `tools/check.sh all` from the repo root runs every offline check
(see `AGENTS.md`).

## New in 0.7.0 (pre-release)

- **Name tags** above other fighters, team colours in team formats; they fade out behind walls (a visibility trace from the camera, 10 times a second per fighter) and `name_tags=0` under `[net]` hides them.
- **Same strength for everyone** (from 0.8.0-pre20; `force_strength=0` under `[net]` turns it off, the host's setting rules the match): every player is built like a fighter of the lobby's tier (Brawl: always tier 5), whatever their own character, so nobody is heavier or has a stronger grip than another and a grab works the same both ways. The lobby's Tier row sets it, with your own gear too. Survival and Zombies have a Gear row (own loadouts, random by tier, everyone identical), kept for the whole run. `force_strength=0`: everyone keeps their own build.
- **Scabbard fix** (on by default after 0.8.0-pre18; `scabbard_fix=0` under `[net]` turns it off): belt weapons that hang in
  a scabbard (most swords and maces) stay at the other player's hip on host physics and owner sync alike, and a belt weapon
  that ends up far from its socket is put back. `scabbard_fix=0` gives the 0.8.0-pre18 behaviour.
- **Grab drag** (on by default after 0.8.0-pre16; `grab_drag=0` under `[net]` turns it off): a grab on any part of
  another player drags them the way a grab on a down body's foot does; the held part follows the hand without the
  rest of the body pulling it apart.
- **Structures stream softly** (from 0.8.0-pre18; `sync_structures` under `[net]`): things built of several parts joined
  together, such as the Pit's bridge or the Cellar's chains, are simulated by the host and sent to the others as one
  snapshot, 20 times a second while any part moves. The other players' games never teleport a part: they steer it toward
  the host's with velocity, so the joints hold and the bridge swings the same on every screen. Only when a copy is far
  off (more than 1.5 m, or stuck far off for 2 s) are all its parts placed at once, at most every 5 s. Nobody takes over
  a structure when they touch it (their own game pushes their copy, and their fighter pushes the host's), the map reset
  leaves it as it is, and when a part breaks off it breaks for everyone and then moves as any loose piece.
  `sync_structures=soft` (default), `local` (nothing is sent: every game simulates its own copy) or `full` (as
  0.8.0-pre17: sent and teleported like loose pieces). An old `sync_structures=0` reads as `soft` and `1` as `full`.
- **Lock-on under host sync** (on by default after 0.8.0-pre18; `host_lock_on=0` under `[net]` turns it off, per machine): a client
  in a Host-sync match gets the camera lock back: guarding with the camera lock on turns your camera towards the fighter
  in front of you, and your fighter faces it on every screen. Short key taps (back, run, jump) are no longer lost
  when the network drops a packet.
- **Lighter host (after 0.8.0-pre18; host physics, host only):** `drive_lean=0` under `[net]` goes back to playing the
  other players' input the old way (default 1: a held guard is not fed again every frame, an unchanged aim is not set
  again). `host_input_delay` under `[net]` (HSMMod.ini only, not in the lobby) delays the host's own buttons and
  movement by what the other players' input loses on the way (half the round trip plus half a send interval, the
  median over the players, re-measured every 5 s, at most 10 ms more or less each time) so the fight is even:
  `auto` (default), `0` (off, as before) or a number of milliseconds (0 to 200). Turning and aiming are never delayed,
  so the camera answers at once. Nothing changes under owner sync.
- **Mesh (experimental, on by default from 0.7.0-pre6; `mesh=0` under `[net]` turns it off):** in a Steam lobby the
  players also send their movement straight to each other, which should cut the delay between two clients. It is used
  where the host and the client both have it on. The host stops relaying movement a client already receives directly and relays it again within a few
  seconds if the direct path breaks. From 0.7.0-pre5 hits, cuts, stuck blades, blood and wounds, drawn weapons, grabs
  and prop movement also go straight to the players they concern, with a copy to the host, which still checks, scores
  and relays everything; whichever copy arrives first is used and the second is dropped. `mesh=0` is exactly the old behaviour. With the mesh on, detours (`detour=0` turns them off) let the
  host route a pair of players through a third when that is faster, from their measured pings.
- **Hold G to give up** in FFA, team matches and Duel, downed or not: your fighter dies, and the last player who hit
  you within 60 s gets the kill (in a Duel, the round too); with none, nobody scores. The hold shows as a bar in the
  middle of the screen. Holding G with nothing to give up shows why.
- **Brawl** (a mode card next to the formats): free-for-all with bare fists. Every fighter spawns and respawns with no
  armour and no weapons (tier 0), in the host's lobby, whatever loadout they brought; the Gear and Tier rows are hidden
  and nobody's saved loadout is touched. Scoring, limits and respawns are free-for-all's. Weapons the level has lying
  around can still be picked up.
- **Kick, ban and votekick:** the host types `/kick <name>` or `/ban <name> [30m|2h|7d|perm]` in the chat
  (`/players`, `/bans`, `/unban`); players can `/votekick <name>`, and on a `role=server` host also `/voteban <name>`
  (7 days). See *Kick, ban and votekick*.
- **Respawns retry** up to 5 times and log each attempt; a Duel always starts with both fighters alive.
- **Leavers:** a fighter who quits mid-fight counts as dead; in a Duel the one left standing takes the rounds.
  A rejoin within the reservation keeps its team; a reused FFA slot starts at 0.
- **Spectating** follows only living fighters with a follow camera behind and above their head (Left/Right switch);
  the pause menu shows the cursor so Continue works without Alt+Tab, and Esc no longer stacks a second pause menu.
- **Map picker** offers only the arenas (the Abyss only for Zombies); **PROGRESSION (CO-OP)** card takes everyone to
  the Tavern hub; the lobby columns scroll.
- **Bug reports from the game:** `/bug <what went wrong>` in the match or lobby chat (`/report` works too), **REPORT A
  BUG** on the multiplayer page, or **REPORT A BUG** on the pause menu in a session fight (it opens the chat with `/bug `
  typed). The report goes to the report server at reports.dudiebug.net, which opens a public GitHub issue with your
  text; your redacted log stays private. You first see "Sending...", then exactly one answer: the issue link once it is
  filed, "not sent" when the server could not be reached, "not filed" with the reason when it refused, or "may have
  been filed" when the answer was lost after the report went out (look at the issues before sending it again).
- **Lobby chat** on the multiplayer page, under the players: the last 8 lines, a text box and SEND (Enter sends too).
  Lines go to everyone in the lobby as Steam lobby chat, so they work before a match starts, with the sender's name in
  their team colour; "<name> joined/left" lines show too. The chat keeps the 120-byte cap and the one-line-a-second
  rate limit of the match chat, and the lines are still in the match chat after the travel. `/bug`, `/sendlog` and
  `/nolog` work in the box as in the match chat.
- **HELP IMPROVE THE MOD** toggle on the multiplayer page (`help_improve=1` under `[net]`, default off); see
  *In-game bug reports*.
- **Flails:** the stand-in's chain no longer drags on the floor and is moved along with teleports.
- **Saves:** a stale or empty backup never overwrites a newer save (it is renamed `HSMMP_stale_*`); leftover backups
  are checked at every game start, even with `role=off`.
- **Safety:** clients only follow travels to known maps; size caps on network input; `-hsm*` flags are ignored on a
  Steam join launch (`+connect_lobby`).
- **Crash notes:** a fault writes `crash: fault ... at <module>+<offset>` to `HSMMod.log` before the game closes, with
  the return address and code addresses on the stack, and a small minidump `HSMMod.crash.dmp` next to the log (the
  previous one becomes `HSMMod.crash.prev.dmp`; one per game run, written with the system `dbghelp.dll`). Send both with
  the newest folder under `%LOCALAPPDATA%\HalfSwordUE5\Saved\Crashes\`. A call on a pointer that is no longer a game
  object is refused instead of crashing: `engine: call of <function> refused` and a stack line in the log.
- **`role=server` (experimental):** the host runs matches without a fighter of its own and cycles maps by itself.
  Keys under `[net]`: `server_format` (ffa, teams, survival, zombies, duel), `server_maps` (comma list or `all`),
  `server_score_limit`, `server_time_limit_s`, `server_countdown_s` (30), `server_min_players` (2).
  The server opens a public Steam lobby by itself (named after `name=`) and keeps it open;
  players find it in the game's lobby browser and Steam relays the traffic. It needs Steam running and signed in on
  that machine, with an account that owns the game.
  This is a hosted copy of the retail game, not a dedicated server. With no host fighter to spawn around, players
  spawn at the map's PlayerStart, else a navmesh point, else the floor found by a trace, and the map origin only as
  the last resort.
- Protocol revision 22: 0.7.0-pre5 connects only to pre5 (8-player lobbies changed every fighter and bot number;
  rev 22 also adds the Voice message).

### New in 0.7.0-pre5

- **8-player lobbies**, with up to 10 bots on top. A `role=server` host has no fighter and does not take a place:
  it seats 8 players too (its Steam lobby lists the server and the 8 players as `8/8`).
- **Host migration:** when a Steam lobby's host leaves, Steam makes another player the lobby owner. That player
  becomes the host of the same lobby and opens the current map again with the lobby's settings (scores start over);
  everyone else connects to them and follows. If Steam names nobody within 30 s, or the new host does not answer within
  20 s, players go back to the menu as before. `host_migration=0` under `[net]` turns it off.
- **Per-player pings in the log:** in a Steam lobby every client pings the host and every other client every 2 s
  (`peer_ping=1`, the default; the mesh can stay off). Each client logs its own row and the host logs everyone's, in
  the health lines every 5 s, e.g. `client: ping: slot2 -> host 48 ms (steam 45); slot1 61 ms (steam 58) loss 12%`.
  The figure is the median of the last 8 pings; `steam` is Steam's own estimate for the same link.
- **Mesh v2** (with `mesh=1`): see the mesh entry above.
- **Proximity voice chat**, off by default: `voice=1` under `[net]` turns it on. It needs it on the host and on every
  player who wants to talk or listen. It uses Steam voice (your default microphone; Steam must be running) and sends while you speak above a level: no key to hold. You hear other players at full volume
  within 5 m, fading out by 30 m, from the side they stand on. A fighter whose consciousness drops below 30% of full,
  or who is knocked out or dead, is cut off. The dead and waiting players hear the living from their camera, and talk
  only to each other, at full volume. A `> ` before a name tag shows who is talking. Keys under `[net]`:
  `voice=0` (the default) turns it off (the host's `voice=0` turns it off for the whole session); `voice_mode=ptt` sends only while
  `voice_key` is held (default `v`; a letter, a digit, `f1`..`f12`, `mouse4` or `mouse5`); `voice_level_db=-40` is the
  open-microphone level (`-60` picks up quiet speech, `-30` only a firm voice); `voice_near_m=5`, `voice_far_m=30`;
  `voice_cutoff=30` (percent of full consciousness); `voice_volume=100` (0..200). Voice always goes through the host,
  also with `mesh=1`; a `role=server` host relays it and records and plays nothing.

### Fixed in 0.7.0-pre4

- **Players dropped from Steam lobbies:** the host no longer drops a client 15 s after it joins, nor one that is
  still loading a map (up to 60 s) or spectating; "only bots spawn" came from this. Fixed on the host, so it helps
  pre3 clients too.
- **Crash on quit** after a Steam session (the game closing with an error on exit).
- **Mid-fight engine crashes** on a worker thread: the game's vertex-paint tasks read a fighter's mesh while armour came
  off or went on. Armour changes the mod replays now wait (up to 1.5 s) until that fighter's paint tasks are done.
- **Black screen while spectating after a death:** the camera fade is cleared and the game's vignettes (which follow
  your dead fighter's consciousness) are hidden while you spectate, and come back when you respawn.
- **Automatic crash reports** only for real crashes, once each (a fault the game survived is no longer reported).
- The **MULTIPLAYER** button's label shrinks to fit its ribbon.

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

## Up to 8 fighters with their own loadouts (Phase 2)

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
| Transport | Steam relay (ISteamNetworkingMessages): reliable channel (hello/welcome/travel/spawn/blobs/events), unreliable channel (input 60 Hz, snapshots 30 Hz); every packet decoded and validated before the game thread sees it |
| Host | Up to 8 fighters; snapshots 23 body bones + vitals per fighter to every client; announces spawns, despawns and deaths |
| Client | Captures its input and blocks it locally (camera look/turn still local); velocity-tracks its fighter and puppets to host poses; copies vitals; plays deaths on host events |
| Damage authority | Clients block all `...ComponentHitSignature` delegates while joined, so Chaos contacts never run damage locally |

## Install (every player)

1. Copy `build/dwmapi.dll` to `Half Sword/HalfswordUE5/Binaries/Win64/`.
2. Proton/Linux only: set Steam launch options to `WINEDLLOVERRIDES="dwmapi=n,b" %command%`. Launch through Steam:
   starting Proton by hand outside Steam's runtime falls back to software rendering or crashes the D3D12 renderer.
3. Start the game once to create `HSMMod.ini` (same folder; default `role=steam`). Direct IP (`role=host`, `role=client`, `host=`, `port=`) was removed in 0.8.0: an old
   value is logged once as ignored and Steam is used. Players join through Steam lobbies.
4. Logs go to `HSMMod.log` in the same folder.
   **UE4SS (optional):** install UE4SS as usual, then copy HSMMod's `dwmapi.dll` over UE4SS's `dwmapi.dll`. HSMMod
   chain-loads UE4SS from its worker thread (`ue4ss_load.cpp`, never DllMain): `ue4ss/UE4SS.dll` (UE4SS 3.x layout),
   else `UE4SS.dll` next to the exe. Updating UE4SS later overwrites HSMMod's proxy, so copy ours back (the
   multiplayer page warns when the `dwmapi.dll` next to the exe is not ours). `[mod]` in `HSMMod.ini`: `ue4ss=1`
   (default; 0 skips it) and `ue4ss_order=after` (default: after HSMMod's ProcessEvent hook is installed; `before`
   loads UE4SS first). The log says `ue4ss: loaded <path> (<version>)`, `ue4ss: none`, `ue4ss: off (ue4ss=0)` or the
   failure with its Windows error. A UE4SS 2.x install (proxy `xinput1_3.dll`) loads itself and is only noted in the
   log. UE4SS mods are never synced; every player installs their own. Bug reports and crash reports carry the UE4SS
   version and the names (only) of the enabled UE4SS mods (`Mods/mods.txt`, folders with `enabled.txt`). The load
   order is unproven until the in-game spike.
5. Every player needs the same HSMMod build. The lobby names anyone on another build ("Bob on 0.6.5 - update") and the
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
- **Kill credit** comes from weapon hit events: the last weapon holder to hit a fighter within 60 s gets the kill.
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

## Kick, ban and votekick

Chat commands, run by the session host (the console of a `role=server` host takes the same words without the `/`, in
`HSMMod.cmd` next to the log):

- `/players`: the slots and names. `/kick <who>`: out now; they can join again straight away.
- `/ban <who> [30m|2h|7d|perm]`: out and kept out (default permanent); written to `HSMMod.bans`. `/unban <who>`,
  `/bans`. `<who>` is `#slot`, a name or a unique start of one.
- `/votekick <who>` (`/vk`), `/yes`, `/no`, in every hosted session (the Esc menu's VOTE KICK, YES and NO send the
  same). A passed vote (more than half of the players besides the target) keeps the player out for 15 minutes.
- `role=server` only: `/voteban <who>` (`/vb`, or VOTE BAN in the Esc menu). It passes with two thirds of the players
  besides the target (rounded up) and bans the player for 7 days (in `HSMMod.bans`; `/unban` lifts it early).
- `server_admins=` (Steam IDs) may kick and ban from the chat; `votekick=0` (or `server_votekick=0`) turns votes off.

A kicked or banned player goes back to the main menu, and the multiplayer page opens with the reason in red.

## In-game bug reports

Type `/bug <what went wrong>` (or `/report ...`) in the chat, or use REPORT A BUG on the multiplayer page. The first
session fight of each game run shows this hint in the chat. The mod sends the text, a few facts (mod and game
build, protocol, host/client, sync mode, level, match mode and phase, player count, Windows or
Wine version) and the end of `HSMMod.log` and `HSMMod.prev.log` (at most 3 MB each) over HTTPS to the report relay
(`report_url` in `HSMMod.ini`, default `https://reports.dudiebug.net`).
The relay opens a public GitHub issue with the text and the facts only (label `in-game-report`) and keeps the logs
in a private repository. Before sending, Steam IDs and IP addresses in the logs become salted hashes (`steam:1a2b3c4d`,
`ip:5e6f7a8b`, IPv6 `ip6:...`; the salt is new for every report, so reports cannot be matched up) and the profile folder name in
paths becomes `<user>`. Player names stay in the logs.

When a session player files a report, every other player sees "<name> filed a bug report. Type /sendlog to add
your log, or /nolog." `/sendlog` within 2 minutes adds that player's own log to the same report; nothing is sent
otherwise. One report per player every 30 seconds (the relay allows 20 an hour and 60 a day per address; when it
refuses, its own reason is shown); the host relays one share per player per minute.

The only web requests are `/bug` and `/sendlog` (below) and the update check (next section); WinHTTP is loaded from
System32 only then. `reports=off` in `HSMMod.ini` turns the report commands off.

## Update check

At startup the mod makes one HTTPS request to `api.github.com` (the public releases list of
`Dudiebug/halfsword-multiplayer-mod`) with the header `User-Agent: HSMMod/<version>`; nothing else is sent, no IDs and
no names. The log says `update: HSMMod <new> is available (this is <cur>): <link>`, `update: up to date (<cur>)` or
`update: check failed (<reason>)`. When a newer release exists, the multiplayer page shows "HSMMod 0.7.0-pre3 is out
(you have 0.7.0-pre2). Get it from GitHub: Dudiebug/halfsword-multiplayer-mod", and the chat repeats it once a few
seconds into the first fight level. Nothing is downloaded or installed. A prerelease build is offered prereleases and
full releases; a full release only full releases (`session/hsm_version.h`). `update_check=0` under `[net]` in
`HSMMod.ini` turns it off. `build.sh` passes the nearest `v*` git tag so a prerelease build knows its number
(`kModVersion` is "0.7.0" for every 0.7.0 prerelease): tag the commit before building a release.

**Help improve the mod.** `help_improve=1` under `[net]` (default `0`, off) is the switch behind the **HELP IMPROVE THE
MOD: ON/OFF** button at the top of the multiplayer page and on the Esc menu in a session fight; the button writes the
key back to `HSMMod.ini`, and nothing else in the file changes. The first MULTIPLAYER click asks once (YES turns it on,
NO THANKS or Esc leaves it off; `help_improve_asked=1` records the answer). It is never turned on without that answer.
When on, your HSMMod log (same redaction as above: Steam IDs, IPs and your Windows user name removed; player names
stay) is sent to
the bug report server whenever someone in your lobby files a report, without waiting for you to type `/sendlog`. And if
your last session crashed (`crash: fault` in `HSMMod.prev.log`), the next start files a crash report by itself: its
public text is the crash line only, both logs go privately. When that report is filed in the menu, it is shared with
the next session you join within 30 minutes, so the players there who also turned it on add their logs (their logs
still hold the crashed session if they kept playing). Off, nothing is sent unless you type the command; after a crash
the chat says once where the button is. When on, the session health lines also carry a hit audit (`hit audit (help
improve):` per striking weapon class and component: contacts, sent, gated, limited, coalesced, no-claim, blocked; and
claims replayed on you by striking component with their damage), kept in the log only.

## Command-line options and the agent bridge (testing)

All optional; several instances can share one install.

| Flag | Effect |
|---|---|
| `-hsmrole=steam\|server\|off`, `-hsmname=NAME` | override `HSMMod.ini` |
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

## Recorder (testing)

`record=1` under `[mod]` in `HSMMod.ini` records every fight level to `HSMRec-<date>-<time>.hsmrec` next to
`HSMMod.log`: every fighter's physics bodies, held weapons and grip at 60 Hz, vitals at 15 Hz, every physics object of
the level (furniture, dropped weapons, loose armour) while it moves, your own input (axes, buttons, camera) at 60 Hz, and
events (grabs, pick-ups, drops, grip changes, falls, damage, severs, hits, deaths, breaks, button presses). Off by default;
nothing is uploaded. Files are about 40 MB a minute with 6 fighters. Read them with `tools/hsmrec.py` (`summary`,
`events`, `csv`). Format and limits: `docs/PLAN_RECORDER.md`.

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

- The sections above describe the original host-simulates-everyone sync, which was removed in 0.8.0. Owner sync is
  the only mode: each machine simulates its own fighter, the others are stand-ins driven by a 60 Hz pose stream, hits
  are claimed by the attacker and replayed by the victim's machine through the game's own damage code, and gore,
  grabs, dropped weapons and level props are synced. `sync_mode` in `HSMMod.ini` and `-hsmsync` are ignored (the log
  says so once). It needs the script VM hook: without it multiplayer refuses to start a session.
- Time of day follows the map; there is no way to change it yet (public issue #2). A lobby setting is planned. It is
  not done because the part that needs the game's own data is unknown here: which game object decides a fight level's
  lighting (the save and the game instance carry a `Day Time` / `Current Day Time` byte of `Enum_DayTime`, but the
  reflection index lists no enum values and no owning class, and the maps also ship per-map morning, evening and night
  lighting classes), and whether setting it before the level loads changes anything. It needs a look at the game's
  Blueprints before any code is written.
- Loadouts with no fabric armor show no team colour (adding a tabard is not done yet).
- Progression rewards (payout, rank points, injuries) are not yet applied to clients' own saves after co-op fights.
- Direct IP (ENet) was removed in 0.8.0; servers and players use Steam lobbies.
- Next: a spectator host (`role=server`): the retail game plus this mod hosting with no fighter of its own and
  automatic match start/rotation. It is a hosted retail instance, not a dedicated server build.
