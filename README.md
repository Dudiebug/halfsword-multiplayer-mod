# Half Sword Multiplayer Mod (HSMMod)

Online multiplayer for Half Sword in one drop-in DLL.
No game files are changed; delete the DLL to go back to vanilla.

> **Status: release.** The current release is v0.7.2. Expect rough edges, and please report what you find
> with `/bug` in the game. Everyone in a session needs the same version (0.7.2 connects only to 0.7.2).
> **Prerelease:** v0.8.0-pre1 tries out the new object sync (below); it connects only to 0.8.0-pre1.

## What it does

- **Steam lobbies:** a MULTIPLAYER button on the main menu; host for friends or in public, invite through the Steam
  overlay, join from the friends list. Traffic goes through Steam's relay network, so no port forwarding.
- **Up to 8 players** with their own loadouts, carried over from each player's game, plus bots.
- **Host migration:** if the host of a Steam lobby leaves, another player takes over the lobby and reopens the map,
  and everyone else follows (scores start over). `host_migration=0` under `[net]` in `HSMMod.ini` turns it off.
- **Voice chat (off by default):** the host turns it on under VOICE CHAT on the multiplayer page: **Proximity** (you
  hear players near you, from the side they stand on, fading out by 30 m) or **Everyone** (every living player hears
  every living one). Each player picks their microphone there or in the Esc menu: push to talk (on `V` by default;
  click CHANGE to pick another key), open mic with a threshold, or muted; the recording device; voice volume; and
  HEAR MYSELF to check the microphone, with a level meter. A badly hurt, knocked-out or dead fighter goes quiet; the
  dead talk only to each other. A `> ` before a name tag shows who is talking. Steam must be running.
- **Multiplayer Esc menu:** Esc in a session match opens PLAYERS (mute or unmute each player, set their volume, and
  for the host kick or ban; everyone else can start a vote kick and vote YES or NO), VOICE (your microphone settings)
  and NETWORK (role, ping to the host, direct links). GAME MENU opens the game's own pause menu (settings, quit), and
  MULTIPLAYER MENU there brings you back. RESUME, REPORT A BUG and LEAVE SESSION are at the bottom.
- **Owner sync (default):** your own machine simulates your fighter, so your body has no added latency. Other players
  are physics stand-ins driven by a 60 Hz pose stream.
- **Hits through the game's own damage code:** the attacker claims a hit; the victim's machine replays it on its own
  fighter, so armour, wounds, bleeding, knockouts and deaths work as in solo.
- **Gore, grabs, dropped weapons and loose objects** are synced: severed limbs, stuck blades, dislocations, grabbing
  and carrying another player (alive or down), loose weapons everyone sees in the same place, and the level's barrels, stools and other loose
  props, which the host names for everyone. From 0.8.0-pre1 anything that starts moving mid-match (a thrown helmet,
  armour knocked off, a bridge pulled down, a snapped plank) gets the same name on every machine, each of its parts
  moves the same everywhere, and a joint that breaks on one screen breaks on all of them.
- Match modes, scores, chat and spectating with a follow camera behind the fighter you watch. **Brawl** is free-for-all with bare fists: everyone spawns naked with no
  weapons, and your own gear is untouched afterwards.
- **Name tags** over other fighters, hidden behind walls (team colours in team modes; `name_tags=0` turns them off), and **hold G to give
  up** in FFA, team matches and Duel: you die and the last player who hit you in the past 60 s gets the kill.
- **Crash notes:** if the game crashes, the last lines of `HSMMod.log` name the fault, and `HSMMod.crash.dmp` (a small
  crash dump, next to the log in the game's `Binaries\Win64` folder) is written. Please attach both and the newest
  folder under `%LOCALAPPDATA%\HalfSwordUE5\Saved\Crashes\` to an issue.
- **Kick and ban:** the host uses the Esc menu, or types `/kick <name>` or `/ban <name> [30m|2h|7d|perm]` in the chat
  (`/players` lists everyone, `/bans` and `/unban` manage bans). Players can start a vote kick from the Esc menu or
  with `/votekick <name>`, then `/yes` or `/no`; the host votes too. A passed vote kick keeps the player out for 15
  minutes. On a `role=server` host players can also start a vote ban (VOTE BAN, or `/voteban <name>`): it needs two
  thirds of the votes and bans the player for 7 days. `votekick=0` in the host's `HSMMod.ini` turns votes off.
- **In-game bug reports:** found a bug? Open the chat and type `/bug <what went wrong>` (or use REPORT A BUG on the
  multiplayer page, or on the pause menu in a session fight, which opens the chat with `/bug ` typed) to open an issue
  here with your mod log attached privately. You see "Sending...", then the issue link once it is filed, or a short
  reason if it was not. Other players in the session are asked whether to add theirs (`/sendlog` or `/nolog`).
- **Direct client-to-client movement (experimental, on by default):** players in a Steam lobby send their movement,
  blows and other fight events straight to each other instead of through the host; the host relays again by itself if
  a direct path fails. `mesh=0` under `[net]` in `HSMMod.ini` sends everything through the host as before.
- **Detours (experimental, on by default):** the host picks a faster path for each pair of players from their measured
  pings: when A to C through B is quicker than A to C directly, B passes A's movement on as well. Only A and C can make
  or check those packets. `detour=0` turns it off.
- **Hosting without playing (experimental):** `role=server` runs matches and rotates maps with no fighter of its own;
  players spawn at the map's start points. It runs a normal copy of the game and opens a public Steam lobby that
  players find in the game's lobby browser.

## Known limitations

- **Time of day follows the map.** You cannot pick day or night in a lobby yet; a lobby setting is planned.
- **Brawl:** weapons on the level's own racks and floors can still be picked up.
- **Direct IP is deprecated and will be removed in 0.8.0.** Use Steam lobbies. Direct-IP sessions have no lobby, so they
  play Free-for-all with default settings.
- Loadouts with no fabric armour show no team colour.

## Bug reports and privacy

`/bug` sends your text, the mod and game version, the match setup and your OS version to the report server, which
opens a public issue in this repository with those details. The end of your `HSMMod.log` goes along but is stored
privately with the maintainers, never on the issue. Before it leaves your PC, Steam IDs and IP addresses in the log
are replaced with one-off hashes and your Windows user name is removed from file paths. Player names stay in the log.

Apart from the update check below, the mod makes no web request unless you type `/bug` or `/sendlog`, or turn
on **HELP IMPROVE THE MOD** on the multiplayer page (`help_improve=1`, off by default). With it on, your log (redacted
the same way) also goes to the report server by itself when someone in your lobby files a report, and if your game
crashed, the next start files a crash report: its public text is the crash line only (error code and code offset),
your logs stay private. Set `reports=off` under `[net]` in `HSMMod.ini` to turn all of it off.

## Update check

Each time the game starts, the mod makes one HTTPS request to `api.github.com` to read this repository's list of
releases, and tells you (in `HSMMod.log`, on the multiplayer page and once in the chat of your first fight) if a newer
release exists. The request carries only a `User-Agent: HSMMod/<version>` header, no IDs and no names, and nothing is
downloaded or installed. If you run a prerelease you are told about prereleases and full releases, otherwise only
about full releases. Set `update_check=0` under `[net]` in `HSMMod.ini` to turn it off.

## Install

1. Download `dwmapi.dll` from the [latest release](../../releases).
2. Copy it into `<Half Sword>/HalfswordUE5/Binaries/Win64/`.
3. **Linux / Steam Deck (Proton) only:** Proton loads its own built-in `dwmapi.dll` unless told otherwise. In Steam,
   right-click Half Sword → Properties → General → Launch Options and set:
   ```
   WINEDLLOVERRIDES="dwmapi=n,b" %command%
   ```
4. Start the game. `HSMMod.ini` is created next to the DLL on first run.

Every player needs the same mod version and the same game build (Steam build 24185754). On any other game build the
mod logs "unsupported build" and does nothing.

## Build from source (Linux, no MSVC)

Needs [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (set `LLVM_MINGW` to its folder) and, for the
tests, a host `g++`/`clang++` with ASan/UBSan.

```bash
implementation/client/HSMMod/build.sh            # -> implementation/client/HSMMod/build/dwmapi.dll
implementation/shared/run_tests.sh               # game-independent logic under ASan/UBSan
implementation/shared/run_fuzz.sh 300000         # protocol decoder fuzzing
```

`implementation/client/HSMMod` is the DLL (engine hooks, gameplay, transport, UI);
`implementation/shared` is the game-independent, unit-tested logic it builds on.
See [implementation/client/HSMMod/README.md](implementation/client/HSMMod/README.md) for details.

## License

HSMMod is released under the [MIT License](LICENSE). Share builds and modified versions freely; keep the
copyright and license notice.

## Credits

### Contributors

- [Dudiebug](https://github.com/Dudiebug): author and maintainer.
- [CSLewin](https://github.com/CSLewin) (Craig Chouraki-Lewin): Steamworks 1.57 struct layouts, Steam
  networking and lobby fixes, and no combat outside the live match phase
  ([#1](https://github.com/Dudiebug/halfsword-multiplayer-mod/pull/1)).

### Third-party code

Parts of the sync design are ported from [Cyrex0/HalfSword-MP](https://github.com/Cyrex0/HalfSword-MP),
Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT. MinHook and ENet are vendored under their own
licenses. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Half Sword is a game by Half Sword Games. This is an unofficial fan mod, not affiliated with or endorsed by them.
