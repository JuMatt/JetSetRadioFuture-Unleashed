# Changelog

Releases are tagged `vX.Y.Z` and built from that tag: the Mac app and the
Linux build both say which version they are (Finder › Get Info, or
`./jsrf-unleashed --version` on Linux), and so does the first line of every
log. Downloads are on the
[Releases](https://github.com/JuMatt/JetSetRadioFuture-Unleashed/releases)
page.

## 0.1.2 — 2 October 2026

No change to the game.

- Both downloads are built from the tagged source and carry its version. (The
  0.1.0 and 0.1.1 Mac apps call themselves "0.1".)
- The repository builds the game on its own: `port/CMakeLists.txt`, given the
  generated code in `port/recomp/gen`.
- `launcher/package_mac.sh` and `launcher/linux/package.sh` make the release
  downloads.

## 0.1.1 — 2 October 2026

- Linux: New Game no longer stays on "Now Loading" for good. The game asks for
  its files in a different case from the one on the disc
  (`TypeA.bin` / `typea.bin`); file names are now found whatever their case.
- The Mac app is the same as in 0.1.0.

## 0.1.0 — 2 October 2026

First public release.

- macOS app for Apple Silicon, and a first Linux (x86_64) build — whose New
  Game hangs on "Now Loading"; use 0.1.1 or later.
- Playable from New Game through the tutorial, the DJ K scene and Dogenzaka
  Hill: skating, grinding, tricks, graffiti, the pause map, music, sound
  effects and voices, saving and loading.
- Controllers and the keyboard.
- Widescreen 16:9 with a wider view, not a stretch; the HUD keeps its shape,
  the title screen stays 4:3.
