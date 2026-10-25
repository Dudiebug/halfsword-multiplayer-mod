# Half Sword Multiplayer Mod (HSMMod)

Online multiplayer for Half Sword in one drop-in DLL.
No game files are changed; delete the DLL to go back to vanilla.

> **Status: pre-release.** The current build (v0.4) passes every offline check (unit tests under ASan/UBSan
> and protocol fuzzing) and is being play-tested in two-player matches; each pre-release fixes what the last test
> found. Expect bugs.

## What it does

- **Steam lobbies:** a MULTIPLAYER button on the main menu; host for friends or in public, invite through the Steam
  overlay, join from the friends list. Traffic goes through Steam's relay network, so no port forwarding.
- **Up to 6 fighters** with their own loadouts, carried over from each player's game.
- **Owner sync (default):** your own machine simulates your fighter, so your body has no added latency. Other players
  are physics stand-ins driven by a 60 Hz pose stream.
- **Hits through the game's own damage code:** the attacker claims a hit; the victim's machine replays it on its own
  fighter, so armour, wounds, bleeding, knockouts and deaths work as in solo.
- **Gore, grabs, dropped weapons and level props** are synced: severed limbs, stuck blades, dislocations, grabbing
  another player, loose weapons everyone sees in the same place, and the level's barrels, stools and other loose
  props, which the host names for everyone.
- Match modes, scores, chat and spectating.

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

Parts of the sync design are ported from [Cyrex0/HalfSword-MP](https://github.com/Cyrex0/HalfSword-MP),
Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT. MinHook and ENet are vendored under their own
licenses. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Half Sword is a game by Half Sword Games. This is an unofficial fan mod, not affiliated with or endorsed by them.
