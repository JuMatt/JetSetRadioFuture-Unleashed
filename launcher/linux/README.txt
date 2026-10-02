JSRF Unleashed for Linux (x86_64)
=================================

Jet Set Radio Future, statically recompiled and running natively.
https://github.com/JuMatt/JetSetRadioFuture-Unleashed

You need your own copy of the game -- the USA release. This package is the
engine only: no game files, no textures, no music, no video.


What you need
-------------
- A 64-bit PC (x86_64) with a recent Linux: glibc 2.34 or later, which means
  Ubuntu 22.04, Debian 12, Fedora 36, Linux Mint 21, current Arch and SteamOS,
  or anything newer.
- Graphics drivers with OpenGL 3.3 (Mesa or NVIDIA's, on any GPU from the last
  ten years).
- A desktop session, X11 or Wayland. Sound goes through PipeWire, PulseAudio
  or ALSA.
- Your game files: make an image of your own disc and extract it. You want
  the folder with default.xbe, Media/ and the rest in it.


Playing
-------
1. Extract this archive anywhere.
2. Run ./jsrf-unleashed
3. The first time, it asks where the game is: pick the folder that contains
   default.xbe. (With zenity or kdialog installed you get a folder picker;
   otherwise it asks in the terminal.) It remembers the choice. You can also
   give the folder -- or a .iso of your disc, if extract-xiso is installed --
   on the command line:  ./jsrf-unleashed /path/to/game

   ./jsrf-unleashed --install     adds JSRF Unleashed to your applications menu
   ./jsrf-unleashed --scale N     internal resolution, 1 to 4 (default 2); remembered
   ./jsrf-unleashed --fullscreen  starts full screen
   ./jsrf-unleashed --choose      asks where the game is again


Controls
--------
Controller: any pad SDL knows -- Xbox, PlayStation, Switch Pro, Steam Deck
and many more.

Keyboard (by key position, so W A S D on QWERTY is Z Q S D on AZERTY):

  W A S D      move (left stick), menus     Arrow keys  camera (right stick)
  Space or J   A -- jump                    K           B
  H            X                            L           Y
  E            R trigger -- talk, spray     Q           L trigger
  R            Black                        F           White
  Return       Start                        Esc         Back
  C / V        left / right stick click

  F11 or Alt+Return   full screen
  F10                 widescreen 16:9 (a wider view, not a stretch; the HUD
                      keeps its shape, the title screen stays 4:3)


Where things are kept
---------------------
  Settings  ~/.config/jsrf-unleashed/settings
  Saves     ~/.local/share/jsrf-unleashed/save
  Logs      ~/.local/state/jsrf-unleashed/last-run.log (and logs/)

Found a bug? Open an issue on GitHub with last-run.log and what you were doing.


Steam Deck
----------
Not tested yet, but it should work: in Desktop Mode, run ./jsrf-unleashed once
to pick the game folder, then add the jsrf-unleashed script to Steam as a
non-Steam game.


Legal
-----
Jet Set Radio Future is (c) SEGA. This is an unofficial fan project, not
affiliated with or endorsed by SEGA. This package contains the engine -- the
runtime and the game's code, machine-translated to native code -- and no game
data.

Licences: MIT for this project's own code; the MCPX audio-processor code
extracted from xemu is LGPL-2.1-or-later; the bundled SDL2 library is under
the zlib licence. See the licences/ folder. Source code:
https://github.com/JuMatt/JetSetRadioFuture-Unleashed
