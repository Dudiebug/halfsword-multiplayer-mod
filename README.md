# Half Sword Multiplayer Mod (HSMMod)

Online multiplayer for Half Sword in one drop-in DLL.
No game files are changed; delete the DLL to go back to vanilla.

> **Status: pre-release.** The current build (v0.7.0-pre1) passes every offline check (unit tests under ASan/UBSan
> and protocol fuzzing). Its new features have not been play-tested yet; each pre-release fixes what the last test
> found. Expect bugs. Everyone in a session needs the same version (0.7.0 does not connect to 0.6.x).

## What it does

- **Steam lobbies:** a MULTIPLAYER button on the main menu; host for friends or in public, invite through the Steam
  overlay, join from the friends list. Traffic goes through Steam's relay network, so no port forwarding.
- **Up to 6 fighters** with their own loadouts, carried over from each player's game.
- **Owner sync (default):** your own machine simulates your fighter, so your body has no added latency. Other players
  are physics stand-ins driven by a 60 Hz pose stream.
- **Hits through the game's own damage code:** the attacker claims a hit; the victim's machine replays it on its own
  fighter, so armour, wounds, bleeding, knockouts and deaths work as in solo.
- **Gore, grabs, dropped weapons and level props** are synced: severed limbs, stuck blades, dislocations, grabbing
  and carrying another player (alive or down), loose weapons everyone sees in the same place, and the level's barrels, stools and other loose
  props, which the host names for everyone.
- Match modes, scores, chat and spectating.
- **Name tags** over other fighters (team colours in team modes; `name_tags=0` turns them off), and **give up while
  downed** in FFA and team matches (hold G): the last player who hit you in the past 60 s gets the kill.
- **Crash notes:** if the game crashes, the last lines of `HSMMod.log` name the fault. Please attach the log and the
  newest folder under `%LOCALAPPDATA%\HalfSwordUE5\Saved\Crashes\` to an issue.
- **In-game bug reports:** type `/report <what went wrong>` in the chat (or use REPORT A BUG on the multiplayer
  page) to open an issue here with your mod log attached privately. Other players in the session are asked whether
  to add theirs (`/sendlog` or `/nolog`). **The report server is not online yet**, so reports currently fail; open an
  issue on GitHub instead.

## Bug reports and privacy

`/report` sends your text, the mod and game version, the match setup and your OS version to the report server, which
opens a public issue in this repository with those details. The end of your `HSMMod.log` goes along but is stored
privately with the maintainers, never on the issue. Before it leaves your PC, Steam IDs and IP addresses in the log
are replaced with one-off hashes and your Windows user name is removed from file paths. Player names stay in the log.

The mod makes no web request unless you type `/report` or `/sendlog`. Set `reports=off` under `[net]` in
`HSMMod.ini` to turn the commands off.

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

To use the older host-simulates-everyone sync, the **host** sets `sync_mode=host` under `[net]` in `HSMMod.ini`.

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
