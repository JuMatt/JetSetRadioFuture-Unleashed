JSRF Unleashed for Windows (x86_64)
===================================

Jet Set Radio Future, statically recompiled and running natively.
https://github.com/JuMatt/JetSetRadioFuture-Unleashed

You need your own copy of the game -- the standalone US disc (not the Sega GT
2002 + JSRF combo disc, whose JSRF is a different build). This package is the
engine only: no game files, no textures, no music, no video.

The Windows build is the same engine as the Mac and Linux builds, tested
under Wine. Bug reports are very welcome.


What you need
-------------
- Windows 10 or 11, 64-bit.
- Graphics drivers with OpenGL 3.3 (any GPU from the last ten years, with its
  maker's driver -- the one Windows Update installs is usually fine). On a
  laptop with two GPUs the game asks for the dedicated one (NVIDIA or AMD);
  last-run.log names the GPU it runs on.
- Your game files: the folder with default.xbe, Media\ and the rest in it,
  extracted from your own disc -- or a disc image (.iso) of it, which the
  launcher unpacks once.


Playing
-------
1. Unzip this archive anywhere. Keep the engine folder beside the .exe.
2. Run "JSRF Unleashed.exe".
3. The first time, it asks where to keep its settings, cache, logs and
   unpacked disc images: in this folder (portable -- see "Where things are
   kept") or in your user profile. Then where the game is: pick the folder
   that contains default.xbe -- or the folder your .iso is in, and it offers
   to unpack it (once). It remembers the choice.

Windows may warn that the app is from an unknown publisher (it is not signed:
that costs money every year). Click "More info", then "Run anyway".

You can also drop a game folder or a .iso onto "JSRF Unleashed.exe", or use
it from a command prompt:

  "JSRF Unleashed.exe" --scale N     internal resolution, 1 to 6 (default 2; 6 is
                                     for a 4K screen; F9 in game does the same);
                                     remembered
  "JSRF Unleashed.exe" --portable    keep everything in this folder from now on
  "JSRF Unleashed.exe" --fullscreen  starts full screen this time
  "JSRF Unleashed.exe" --windowed    starts in a window this time
  "JSRF Unleashed.exe" --choose      asks where the game is again
  "JSRF Unleashed.exe" --version     which version this is


Controls
--------
Controller: any pad SDL knows -- Xbox, PlayStation, Switch Pro and many more.

Keyboard (by key position, so W A S D on QWERTY is Z Q S D on AZERTY):

  W A S D      move (left stick), menus     Arrow keys  camera (right stick)
  Space or J   A -- jump                    K           B
  H            X                            L           Y
  E            R trigger -- talk, spray     Q           L trigger
  R            Black                        F           White
  Enter        Start                        Esc         Back
  C / V        left / right stick click

  F11 or Alt+Enter   full screen; remembered: the game starts the way you
                     left it, in full screen or in a window
  F10                the picture, in turn: 4:3, 16:9 (widescreen) and 21:9
                     (ultrawide) -- a wider view, not a stretch; the HUD keeps
                     its shape and sits at the screen's edges, the title
                     screen stays 4:3; remembered. On an ultrawide screen
                     21:9 is the screen's own shape (up to 2.4:1).
  F9                 resolution: the game is drawn at 1x to 6x the Xbox's
                     640x480 (2x by default), up to 3840x2880 -- 6x is for a
                     4K screen; remembered. The game restarts to apply it,
                     so save first.
  F8                 anti-aliasing: SMAA (a quick pass over each finished
                     frame) and MSAA 2x, 4x or 8x (every edge drawn with
                     that many samples a pixel), either or both; remembered
  F7                 the camera on the right stick, on (the default) or off:
                     the right stick (or the arrow keys) turns the camera
                     around you and tilts it, and it eases back behind you
                     when let go; L trigger puts it straight behind at once.
                     Off, the camera is the original game's. Remembered

The Video menu at the top of the window has the same four, and the Controls
menu the camera. They are hidden in full screen, where the keys still work.


Where things are kept
---------------------
  Saves     in the game folder, under UDATA (where an Xbox keeps them), so
            the game folder has to be one you can write to. Copy UDATA
            somewhere to back your progress up.
  Settings  %LOCALAPPDATA%\JSRF Unleashed\settings
  Cache     %LOCALAPPDATA%\JSRF Unleashed\save
  Logs      %LOCALAPPDATA%\JSRF Unleashed\last-run.log (and logs\)

Portable: with a file named portable.txt beside JSRF Unleashed.exe, all of
that -- and unpacked disc images -- is kept in this folder instead, so the
whole folder can move: to another computer, onto a USB drive or an SSD. The
first start asks which you want; "JSRF Unleashed.exe" --portable switches
later (your settings come along), and deleting portable.txt switches back.
A game folder or a disc image put in this folder is found on its own, and
remembered relative to it. (Not in Program Files, which Windows does not let
programs write to.)

If the game stops unexpectedly, the launcher says so and offers to show the
log. Found a bug? Open an issue on GitHub with last-run.log and what you were
doing.


On a Mac or Linux, through Wine or CrossOver
--------------------------------------------
There are native Mac and Linux builds, which are the better choice. The
Windows one should run under Wine too: in CrossOver, make a Windows 10
64-bit bottle and use "Run Command..." on JSRF Unleashed.exe.


Legal
-----
Jet Set Radio Future is (c) SEGA. This is an unofficial fan project, not
affiliated with or endorsed by SEGA. This package contains the engine -- the
runtime and the game's code, machine-translated to native code -- and no game
data.

Licences: MIT for this project's own code; the MCPX audio-processor code
extracted from xemu is LGPL-2.1-or-later; the bundled SDL2 library is under
the zlib licence, libepoxy under the MIT licence. See the licences folder.
Source code: https://github.com/JuMatt/JetSetRadioFuture-Unleashed
