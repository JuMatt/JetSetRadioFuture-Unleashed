JSRF Unleashed for Linux (x86_64)
=================================

Jet Set Radio Future, statically recompiled and running natively.
https://github.com/JuMatt/JetSetRadioFuture-Unleashed

You need your own copy of the game -- the standalone US disc (not the Sega GT
2002 + JSRF combo disc, whose JSRF is a different build). This package is the
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
- Your game files: an image of your own disc (.iso), or the folder its files
  were extracted into -- the one with default.xbe, Media/ and the rest in it.


Playing
-------
1. Extract this archive anywhere.
2. Run ./jsrf-unleashed
3. The first time, it asks where the game is: pick the folder that contains
   default.xbe, or the folder your disc image (.iso) is in. (With zenity or
   kdialog installed you get a folder picker; otherwise it asks in the
   terminal.) An image is unpacked once, by the game itself -- nothing else to
   install. It remembers the choice. You can also give the folder or the
   image on the command line:  ./jsrf-unleashed /path/to/game.iso
   If the files are not the right ones (another build of the game, another
   game's disc, a zip), it says what they are.

   ./jsrf-unleashed --install     adds JSRF Unleashed to your applications menu
   ./jsrf-unleashed --scale N     internal resolution, 1 to 6 (default 2; 6 is for a
                                  4K screen); remembered
   ./jsrf-unleashed --portable    keep everything in this folder (see below)
   ./jsrf-unleashed --fullscreen  starts full screen
   ./jsrf-unleashed --choose      asks where the game is again
   ./jsrf-unleashed --version     which version this is


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
  F10                 the picture, in turn: 4:3, 16:9 (widescreen) and 21:9
                      (ultrawide) -- a wider view, not a stretch; the HUD keeps
                      its shape and sits at the screen's edges, the title
                      screen stays 4:3; remembered. On an ultrawide screen
                      21:9 is the screen's own shape (up to 2.4:1).
  F9                  resolution: the game is drawn at 1x to 6x the Xbox's
                      640x480 (2x by default), up to 3840x2880 -- 6x is for a
                      4K screen; remembered. The game restarts to apply it,
                      so save first.
  F8                  anti-aliasing: SMAA (a quick pass over each finished
                      frame) and MSAA 2x, 4x or 8x (every edge drawn with
                      that many samples a pixel), either or both; remembered
  F7                  the camera on the right stick, in turn: on (the
                      default), tracking, off. On, the right stick (or the
                      arrow keys) turns the camera around you and tilts it,
                      it eases back behind you when let go, L trigger puts it
                      straight behind at once, and you go where it looks.
                      Tracking, it only looks around: you keep going the way
                      you were. Off, the camera is the original game's.
                      Remembered
  F6                  the map selector: a list of the 14 maps on screen;
                      up/down to choose, A or Return to go there and skate it
                      freely (as after the story), B or Esc to close


Where things are kept
---------------------
  Saves     in the game folder, under UDATA (where an Xbox keeps them): copy
            UDATA somewhere to back your progress up
  Settings  ~/.config/jsrf-unleashed/settings
  Cache     ~/.local/share/jsrf-unleashed/save
  Logs      ~/.local/state/jsrf-unleashed/last-run.log (and logs/)

Portable: with a file named portable.txt in this folder, the settings, the
cache, the logs and unpacked disc images are kept here instead, so the whole
folder can move -- to another machine, onto a USB drive. Run
./jsrf-unleashed --portable once to make the file (your settings come along);
delete it to go back. A game folder (or a disc image) put in this folder is
found on its own, and remembered relative to it.

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
