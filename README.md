# JSRF Unleashed

**Jet Set Radio Future, running natively on Mac and Linux.** Not an emulator — the
game's own code, statically recompiled into C and built as a native
application.

> **This project is about one game.** It is not an Xbox emulator and it will
> not run other Xbox titles. Everything here — the graphics translation, the
> audio path, the file layout, the per-function fixes — is aimed at getting
> *Jet Set Radio Future* right. If you want to run Xbox games in general, you
> want [xemu](https://xemu.app); if you want to port a different Xbox title,
> you want the [toolkit this is built on](docs/xboxrecomp-toolkit.md).

**You need your own copy of the game.** The download is the engine only — no
game files, no textures, no music, no video. It plays the files from a disc
you own.

---

## Download

| Platform | Download |
|---|---|
| macOS 12 or later, Apple Silicon (M1 or later) | [JSRF-Unleashed-macOS-arm64.zip](https://github.com/JuMatt/JetSetRadioFuture-Unleashed/releases/latest/download/JSRF-Unleashed-macOS-arm64.zip) |
| Linux, x86_64 (glibc 2.34 or later: Ubuntu 22.04, Debian 12, Fedora 36 and newer) | [JSRF-Unleashed-linux-x86_64.tar.gz](https://github.com/JuMatt/JetSetRadioFuture-Unleashed/releases/latest/download/JSRF-Unleashed-linux-x86_64.tar.gz) — new, less tested than the Mac build |
| Windows, iPhone | not started |

Every version is on the [Releases](https://github.com/JuMatt/JetSetRadioFuture-Unleashed/releases) page; what changed is in the [changelog](CHANGELOG.md).

### Installing on macOS

1. Unzip, and move **JSRF Unleashed** to Applications (or wherever you like).
2. Open it. The app is not notarised by Apple — that needs a paid developer
   account — so the first time, macOS says it cannot check it. Click
   **Done**, open **System Settings › Privacy & Security**, find
   *"JSRF Unleashed" was blocked* and click **Open Anyway**. (On macOS 13 or
   earlier, right-click the app and choose **Open** instead.) This is needed
   once.
3. It asks where your game is: pick the folder that contains `default.xbe`,
   or a disc image (`.iso`), which it unpacks once if
   [extract-xiso](https://github.com/XboxDev/extract-xiso) is installed
   (`brew install extract-xiso`). It remembers the choice.

Saves are kept in `~/Library/Application Support/JSRF Unleashed/save`. Each
run writes `last-run.log` into the game folder (with a few older ones in
`logs/`) — attach it when you report a bug.

### Installing on Linux

Extract the archive anywhere and run `./jsrf-unleashed`. The first time, it
asks for the folder that contains `default.xbe` (with a folder picker if
`zenity` or `kdialog` is installed, otherwise in the terminal) and remembers
it. `./jsrf-unleashed --install` adds it to your applications menu. It needs
OpenGL 3.3 drivers and an X11 or Wayland desktop; SDL2 is bundled. The
`README.txt` inside lists the options (`--scale`, `--fullscreen`, `--choose`)
and where settings, saves and logs are kept.

On Linux the keyboard layout is the same as below; **F11** (or Alt+Return)
toggles full screen and **F10** widescreen.

### Your game files

You need the **USA (NTSC-U) release** of *Jet Set Radio Future* — the game
code inside the app is translated from that version's executable, so other
regions' discs will not work. Make an image of your own disc and extract it:
you want the folder with `default.xbe`, `Media/` and the rest.

## Playing

**Controller:** any pad macOS recognises — Xbox, DualSense, DualShock 4 and
others, over Bluetooth or USB. On Linux, any pad SDL knows.

**Keyboard.** Keys go by position, so `W A S D` on a QWERTY keyboard is
`Z Q S D` on an AZERTY one:

| Key | Does | Key | Does |
|---|---|---|---|
| W A S D | move (left stick), menus | Arrow keys | camera (right stick) |
| Space or J | A — jump | K | B |
| H | X | L | Y |
| E | R trigger — talk, spray | Q | L trigger |
| R | Black | F | White |
| Return | Start | Esc | Back |
| C / V | left / right stick click | | |

**Controls › Keyboard…** in the menu bar shows the same list.

**Video menu:**
- **Resolution** — the internal resolution, 1× to 4× (2× by default). The
  game restarts to apply it.
- **Full Screen** — Ctrl ⌘ F.
- **Widescreen (16:9)** — ⇧ ⌘ W. The game is shown at 16:9 with a wider
  field of view, not stretched; the HUD and text keep their shape. The title
  screen stays 4:3.

## Status

Playable, early, and not yet played from start to finish.

| | |
|---|---|
| Title, menus, new game, saving and loading | works |
| New Game through the tutorial, the DJ K scene and Dogenzaka Hill | works — the part played and tested most |
| Skating, grinding, tricks, graffiti, the pause map | works |
| Music, sound effects, voices | works |
| Widescreen 16:9 | works in game; the title screen stays 4:3 |
| Linux build | new: boots, menus, starting a new game; far less played than the Mac build |
| Later areas | little tested — expect bugs |
| Known issues | an occasional "disc error" screen during a load; a 1-pixel dark line along the right edge |

Found a bug? [Open an issue](https://github.com/JuMatt/JetSetRadioFuture-Unleashed/issues)
with your `last-run.log` and what you were doing.

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

## Building from source

The generated code — hundreds of thousands of lines of C translated from the
game's executable — is not in this repository and will not be: it is derived
from SEGA's binary. You produce it on your own machine, from your own
`default.xbe`, with the pipeline in [`tools/recomp`](tools/recomp) — the
[getting-started guide](docs/GETTING_STARTED.md) walks through it (it is written
for the toolkit in general), and `--exclude-manual port/recomp_manual.c` tells
it about the hand-written overrides in [`port/`](port). Put the result in
`port/recomp/gen`, then:

```sh
cmake -S port -B build -DCMAKE_BUILD_TYPE=Release    # on Linux add -DNV2A_GL_CONTEXT=sdl
cmake --build build
launcher/package_mac.sh build/jsrf_recomp             # macOS: the app, zipped
```

On Linux, [`launcher/linux/package.sh`](launcher/linux/package.sh) packages the
build and records how the release one is made. The version comes from the git
tag the tree is at.

## Where this is going

The target is the shape [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright)
set: you own the game, you point the app at it, it plays. Next:

1. The rest of the game — later areas get far less testing than the first.
2. Windows.
3. The Metal renderer, and with it an iPhone build, with touch controls.

## Legality

The releases contain the engine: the runtime (the kernel layer and the
graphics and audio hardware models) and the game's own code, machine-translated
to native code. They contain **no game data** — no textures, models, music,
video or text — and do nothing without the files from your own copy of the
game. This repository holds no game code at all; that is generated on your
machine from your own disc. It is the same approach as
[Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) and
[Zelda 64: Recompiled](https://github.com/Zelda64Recomp/Zelda64Recomp).

*Jet Set Radio Future* is © SEGA. This is an unofficial fan project, not
affiliated with or endorsed by SEGA. If you represent a rights holder and have
a concern, please open an issue.

## Licence

MIT for this project's own code; the MCPX audio-processor code extracted from
xemu is LGPL-2.1-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) — the static
  recompilation toolkit this is a port on top of. Its documentation is kept at
  [docs/xboxrecomp-toolkit.md](docs/xboxrecomp-toolkit.md).
- [xemu](https://xemu.app) — the NV2A and MCPX APU hardware models, without
  which none of the graphics or audio would exist.
- Smilebit and SEGA, who made the game.
