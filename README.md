# JSRF Unleashed

**Jet Set Radio Future, running natively.** Not an emulator — the game's own
code, statically recompiled into C and built as a native application.

> **This project is about one game.** It is not an Xbox emulator and it will
> not run other Xbox titles. Everything here — the graphics translation, the
> audio path, the file layout, the per-function fixes — is aimed at getting
> *Jet Set Radio Future* right. If you want to run Xbox games in general, you
> want [xemu](https://xemu.app); if you want to port a different Xbox title,
> you want the [toolkit this is built on](docs/xboxrecomp-toolkit.md).

**You need your own copy of the game.** No game data is distributed here and
none ever will be. You supply the disc you own; this supplies the engine.

---

## Status

Early. It boots, it draws, it plays music, and it is not yet a game you can
finish. Honest state of things:

| | |
|---|---|
| Boots to the title screen | works |
| World geometry, characters, textures | works — correctly placed since the fixed-function viewport fix |
| Streaming music (CRI ADX through the emulated MCPX APU) | works — plays in order and in time |
| Gamepad (DualSense and Xbox pads over Bluetooth) | works via Apple's GameController framework |
| Keyboard fallback | works |
| Getting from the menu into a level | **unreliable** — hangs in the loader |
| Title-screen logo flash, mirrored 2D overlays | known cosmetic faults |
| Metal renderer | early; OpenGL is the working path today |
| Windows / Linux | the code is portable and unproven — not yet built or run |
| iPhone | not started |

If you are looking for "download and play", it is not that yet. It is close
enough to be interesting and far enough to need work.

## Why static recompilation

The game's x86 machine code is translated, function by function, into C, which
is then compiled for whatever CPU you are on. There is no interpreter and no
JIT at runtime. Two things follow from that:

- **It is fast.** The game's logic runs as native code, at native speed.
- **It can ship where JITs cannot.** iOS forbids runtime code generation, which
  is why there is no real Xbox emulator on an iPhone. A statically recompiled
  binary has nothing to generate — which is the reason an iPhone build is on
  the list at all.

What still has to be emulated is the *hardware*: the NV2A graphics processor,
the MCPX audio processor, and the Xbox kernel the game calls into. Those parts
come from the toolkit and from [xemu](https://xemu.app), whose NV2A and APU
work this stands on.

## Getting it running

Today this is a build-it-yourself project on macOS (Apple Silicon).

```sh
# 1. Extract your own disc. You want the folder that contains default.xbe.
extract-xiso -x "Jet Set Radio Future.iso"

# 2. Build.
cmake -B build-arm64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm64 -j

# 3. Run, pointing it at those files.
JSRF_GAME_DIR="/path/to/extracted" ./build-arm64/jsrf_recomp
```

### The launcher

`launcher/build_app.sh` wraps the engine into **JSRF Unleashed.app**: it asks
once where your game files are, remembers them, and starts the game. It asks
again only if they have actually gone — a folder you rename or move is
followed, because being asked a question you cannot usefully answer is not a
feature. Hand it a `.iso` and it unpacks it once (with `extract-xiso`, if you
have it) and remembers the result.

```sh
launcher/build_app.sh
open "JSRF Unleashed.app"
```

## Where this is going

The target is the shape [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright)
set: you own the game, you point the app at it, it plays — on Windows, Linux,
macOS, and iPhone (through AltStore), with a touch layout when no gamepad is
connected. Nothing about the approach rules any of that out; all of it is work
that has not been done yet.

Nearest first:

1. The loader hang between the menu and a level — the last thing standing
   between this and actually playing.
2. Windows and Linux builds, which the code is written for but has not seen.
3. The Metal renderer, and with it the iPhone build.
4. Touch controls.

## Legality

The recompiled code is derived from a copyrighted binary, so **no compiled
game binary is distributed here** — only the tools that turn a disc you own
into one, and the runtime that hosts it. Bring your own disc. This follows the
same line as the Zelda and Mario recompilation projects.

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) — the static
  recompilation toolkit this is a port on top of. Its documentation is kept at
  [docs/xboxrecomp-toolkit.md](docs/xboxrecomp-toolkit.md).
- [xemu](https://xemu.app) — the NV2A and MCPX APU hardware models, without
  which none of the graphics or audio would exist.
- Smilebit and Sega, who made the game.
