# Changelog

Releases are tagged `vX.Y.Z` and built from that tag: the Mac app, the Windows
and Linux builds and the iPhone and Android apps say which version they are
(Finder › Get Info, `JSRF Unleashed.exe --version` on Windows,
`./jsrf-unleashed --version` on Linux, the ⚙ button on iPhone and Android),
and so does the first line of every log. A
download with nothing new for its platform is carried over unchanged and keeps
its version. Downloads are on the
[Releases](https://github.com/JuMatt/JetSetRadioFuture-Unleashed/releases)
page.

## 0.6.6 — 2 November 2026

A map selector. Every platform is new in 0.6.6.

- **Go straight to any map.** The 14 maps, under the game's own names and
  grouped by district (Shibuya-cho, Kogane-cho, Benten-cho), each loaded as
  the free skating that comes after the story: no timer, nothing to race or
  chase. On the Mac and Windows it is the **Maps** menu; on iPhone and
  Android, **Go to a map** in the settings; **F6** on Windows and Linux (and
  **Maps › Map Selector** on the Mac) shows the same list on screen, gone
  through with the pad or the keyboard (up and down, **A** or Return to go,
  **B** or Esc to close). Choose a map while skating, or at the title screen: it goes there once you press Start.
  Each map is reached the way the game itself goes from one mission to the
  next; a map asked for while another loads waits until it is done. These
  are the game's last missions: if it offers to save while you explore, say
  no, or keep a copy of your save.
- Tested on a Mac: all 14 maps, one after the other, from play; the list
  on screen opened at the title, Skyscraper District chosen with A and gone
  to once Start was pressed; out of the garage to Skyscraper District. On
  Linux: F6, the list drawn on screen, chosen with the arrows and Return.
  Windows, the iPhone and Android: built and checked, not run.

## 0.6.5 — 2 November 2026

Photo mode now freezes the world. Every platform is new in 0.6.5.

- **Photo mode stops everything.** In 0.6.4 the game kept running while you
  framed a shot: the skater's idle, the people, the cars, the spray cans
  turning. Now the whole city holds still the moment photo mode starts, and
  goes on from where it was when you go back to the game. The game's frame
  runs the world's update and then a second pass where the poses and the
  cans' turn are worked out; while photo mode is on both are left out, all
  but the camera, so the scene is drawn exactly as it stood. Music and sound
  carry on. `JSRF_PHOTO_FREEZE=0`: the world runs on, as in 0.6.4.
- Tested on a Mac in Skyscraper District: with the camera still, pictures
  taken two and four seconds apart in photo mode were the same (about 20
  pixels in 1.2 million, against about 7,000 before); the camera turned
  freely; after B the world went on. 60 fps throughout. Windows, Linux, the
  iPhone and Android: built and checked, not run.

## 0.6.4 — 2 November 2026

Photo mode, and further draw distance in Skyscraper District. Every platform
is new in 0.6.4.

- **Photo mode.** Hold **Back** (Esc on a keyboard) for about a second while
  skating, or choose **Controls › Photo Mode** (⌘ P on the Mac, the Controls
  menu on Windows, the settings on iPhone and Android). The camera leaves the
  skater: the left stick moves it, the right stick turns it, the triggers
  lower and raise it; **Y** hides the HUD, **A** the help, and **B** or
  **Back** goes back to the game. A panel at the bottom left of the screen
  lists the controls, with the keyboard's keys. The game keeps running around
  you while the skater waits; on a phone it needs a controller, for the right
  stick.
- **Skyscraper District: no more towers popping in.** The game draws a map
  area by area, only those it judges visible from where you are, so the
  towers and satellite dishes on the way to 99th Street appeared as you came
  close — on the Xbox too. In Skyscraper District the areas drawn now reach
  two steps further, as Miggas worked out in his
  [JSRF-Recomp](https://github.com/Miggas/JSRF-Recomp) (thank you). Only the
  drawing changes; the game's own areas still decide everything else. It
  costs a few frames a second there (about 6 on a Mac); elsewhere nothing
  changes. `RECOMP_DRAW_AREAS=game`: as on the Xbox.
- Tested on a Mac in Skyscraper District: photo mode started by holding Back,
  flew the camera over the rooftops, hid the HUD and went back to the game;
  the further draw distance showed the distant light pillars the game leaves
  out. The help panel on Linux. Windows, the iPhone and Android: built and
  checked, not run.

## 0.6.3 — 2 November 2026

The brake sparks and smoke, as on the console. Every platform is new in 0.6.3.

- **Brake sparks and smoke every time.** Braking — the stick forward, then
  back — throws red sparks and smoke from the skates. Here, as in xemu, they
  came out only now and then, and stayed at a fixed spot instead of following
  the skates. Miggas found why in his JSRF-Recomp
  ([Miggas/JSRF-Recomp](https://github.com/Miggas/JSRF-Recomp), thank you):
  the console's Pentium III answers one kind of square-root instruction
  (`rsqrtss`) with a 12-bit approximation, and the recompiled code computed it
  exactly. One place needs the console's value — the routine that normalises
  a vector, which the effect goes through — and now gets it, bit for bit, on
  every CPU (his table, MIT licensed, credited in NOTICE).
  `RECOMP_SSE_P3=0`: the exact value, as before.
- Tested: the Linux build through the title and its demo (the new routine in
  use, no crash); the Windows build and the Mac, iPhone and Android apps were
  built and checked, not run.

## 0.6.2 — 1 November 2026

A second way to have the camera on the right stick, from players' feedback,
and Game Mode on the Mac. Every platform is new in 0.6.2.

- **The camera on the right stick, tracking** ([#44](https://github.com/JuMatt/JetSetRadioFuture-Unleashed/issues/44)).
  The camera on the right stick turns the view around the skater, and until
  now the skater turned with it: up on the left stick goes where the camera
  looks, so panning to look at something steered you towards it. Tracking
  only looks around: the skater keeps going the way they were, and up on the
  left stick is still straight ahead of them. Let go and the view eases back
  behind, as before. On the Mac **Controls › Camera on the Right Stick
  (Tracking)**, beside **Camera on the Right Stick** (choose the one that is
  on again to turn the camera off); on Windows the same in the **Controls**
  menu; **F7** on Windows and Linux goes round on, tracking and off; and the
  iPhone's and the Android app's settings have the three. The camera on the
  right stick stays on by default; `RECOMP_RSTICK_CAMERA=2` for tracking.
- **Mac: Game Mode in full screen** ([#54](https://github.com/JuMatt/JetSetRadioFuture-Unleashed/pull/54),
  thank you, danielgpinheiro). The app now says it is a game, so macOS puts it
  in Game Mode while it is in full screen — the CPU and GPU first, and
  lower-latency Bluetooth for controllers — and takes it back when it leaves.
- Tested on a Mac: in Shibuya Terminal the right stick turned the view about
  half a turn, with the camera on and with tracking; on, the skater's heading
  turned with it, and tracking left it as the game had it. Off, the game's own
  camera stayed (Dogenzaka Hill). The title ran at 60 fps on Metal and OpenGL,
  and macOS put the game in Game Mode in full screen. Windows (under Wine) and
  Linux: F7, and on Windows the Controls menu, go round on, tracking and off
  and keep the choice; the Android and iPhone builds were built and checked,
  not run.

## 0.6.1 — 31 October 2026

Widescreen: a 21:9 picture for ultrawide screens on Mac, Windows and Linux,
the HUD at the edges of the screen, and the boost's heat haze over the whole
picture. Every platform is new in 0.6.1.

- **21:9 on Mac, Windows and Linux.** The Video menu has three pictures now:
  **4:3**, **Widescreen (16:9)** and **Ultrawide (21:9)** — ⇧ ⌘ W and ⇧ ⌘ U on
  the Mac, **Video › Picture** on Windows, and **F10** goes round the three on
  Windows and Linux. On an ultrawide screen 21:9 is the screen's own shape
  (2560×1080 and 5120×2160 are 2.37:1, 3440×1440 2.39:1, up to 2.4:1), so in
  full screen the picture fills it; on any other screen it shows 21:9 between
  bars above and below. As with 16:9 it is a wider view, not a stretch: the
  camera sees more of the city at the sides and the game culls against it. The
  choice is remembered (`widescreen=` and `ultrawide=` in the settings on
  Windows and Linux).
- **The HUD at the screen's edges.** In widescreen the HUD stayed where it
  was on a 4:3 screen, in the middle of the wider one: the health bar ended
  82% of the way across a 16:9 screen and 74% across 21:9. The health bar,
  the spray cans, a race's rival and the gauge now sit at the right edge, as
  far from it as on the console; the timer stays at the top centre, and
  menus, dialogue and their text stay in the middle. On every widescreen
  picture: the iPhone's Fill the screen and Android's 16:9 too.
  `RECOMP_WIDE_HUD=0` puts the HUD back in the middle.
- **The boost's heat haze over the whole picture.** The boost dash bends the
  scene behind you through a quad sized to fill a 4:3 view; in widescreen it
  covered only the middle 4:3, and its edges showed as two vertical lines. It
  now fills the screen, on every platform.
- Tested on a Mac, against 0.6.0: City Rush with the boost at 16:9 and 21:9
  on Metal and on OpenGL (the haze's edges gone, the HUD at the right edge,
  60 frames a second throughout) and in 4:3 (unchanged), the Fortified
  Residential Zone with the timer and a dialogue box (whole, in the middle),
  Dogenzaka Hill, the pause screen (unchanged), and the picture switched
  while playing, 16:9 → 21:9 → 4:3 → 16:9. Linux, and Windows under Wine, on
  a 2560×1080 screen: the window opens at 21:9 from the stored choice, F10
  goes round the three, full screen. The Android and iPhone builds were
  built and checked, not run.

## 0.6.0 — 30 October 2026

Faster where the game used to stall: the textures and shaders made while you
play, and the work done for every drawing command. These are six of the speed
fixes [spriteice](https://github.com/spriteice) found by measuring where the
time goes on a ROG Xbox Ally X, ported here -- thank you, spriteice! Every
platform is new in 0.6.0.

- **Entering an area, on Windows and Linux.** A new area brings 50 to 110
  textures in one frame, and each was decoded to plain pixels before the
  graphics driver got it. Most of the game's textures are DXT (S3TC)
  compressed, which every desktop graphics driver takes as it is: they now go
  to it that way, their mipmaps included -- nothing to decode, and a quarter
  to an eighth of the bytes to hand over. On spriteice's Ally X, a run's
  textures took 82 to 89 ms to make instead of 576 to 600, and the frames
  entering a level came in under 27 ms instead of 42 to 111. Apple's OpenGL
  takes them too, but in testing two frames there stalled for 130 and 157 ms
  where 0.5.10 took 27 and 38, so the Mac decodes them as before for now --
  as Metal, phones, and computers drawing without a graphics card (Mesa's
  llvmpipe and the like, where a texture decoded once draws faster) do.
  Every texture that is still decoded is decoded by a loop of its own for
  its format, texel for texel as before. `RECOMP_GL_DXT=0` decodes them all,
  `RECOMP_GL_DXT=1` hands them over wherever the driver takes them.
- **Fades no longer build shaders.** Each combiner stage reads two constant
  colours, and they were written into the shader itself, so every shade of a
  fade -- the spray can's effects, the sewer's splashes -- was a new shader,
  built in the middle of a frame: up to three a frame, about 8 ms each, on
  spriteice's machine. The shader now reads them as values it is handed
  (uniforms), so a whole fade is one shader. Because the shaders' text
  changed, the shader cache starts afresh once: the first start builds them
  again, ahead of time -- in the background on Metal, on quiet frames on
  OpenGL.
- **Drawing commands.** More than half of what the game sends the GPU are
  vertex indices, up to 2,047 in one command, and each went through three
  function calls on its own before landing in a list: a whole run of them is
  now appended in one go (1.4 ns an index instead of 11.5 in the engine's
  test). On Windows and Linux, the check that a kept draw's vertices haven't
  changed hashes them about twice as fast (SSE2, or AVX2 where the processor
  has it).
- **Linux: less time spinning.** To show a frame at exactly the right moment,
  the engine sleeps until just before it and spins on the clock for the rest,
  1.2 ms of a core every frame. On Linux it now sleeps to an exact moment and
  spins only the last 0.3 ms, widening that by itself when a sleep runs late
  (`RECOMP_PACE_SPIN_US=1200`: as before). Windows, macOS, iOS and Android are
  unchanged.
- **Smaller logs.** A diagnostic probe that wrote about 200 lines every five
  seconds -- nearly half of every log -- is off unless `RECOMP_GL_TIMELINE=1`.
- Tested on a Mac against 0.5.10, back to back on the same machine: the
  Fortified Residential Zone four times on Metal (0.6.0, 0.5.10, 0.5.10,
  0.6.0 -- 60 frames a second throughout, not one slow frame once the level
  was on) and on OpenGL, Dogenzaka Hill on OpenGL with SMAA and MSAA 4x and on
  Metal (the pictures alike), and Shibuya Terminal from an empty save folder
  (the camera 0.109 to 0.110 degree at every one of 197 frames, the sound
  never running dry, as in 0.5.10). Then the pictures side by side with
  0.5.10's at the same moments of play in the Fortified Residential Zone, on
  Metal and on OpenGL, and once with the DXT textures handed to the driver
  as they are (the Windows and Linux way, on the Mac's OpenGL): the same,
  shadows and all, the DXT ones within a level or two of a colour here and
  there. Linux (Mesa's llvmpipe) and Windows (under Wine): the attract demo
  against 0.5.10 on the same machine, the same frame rate. The Android and
  iPhone builds were built and checked, not run. And the engine's new tests:
  every texture format decoded texel for texel as before; DXT through a
  driver within 1 or 2 of the old decoder, transparent texels exact; runs of
  indices leaving the renderer byte for byte as one index at a time; the new
  hash against the same steps in plain C; the shaders the same through new
  constants, every constant read where it should be, and accepted by
  glslangValidator.

## 0.5.10 — 29 October 2026

A hang where the picture stopped for good, now and then, while the game waited
for room for its drawing commands is fixed. [Marice](https://github.com/Marice)
found where to look (pull request #46, merged here with his two fixes) --
thank you, Marice! Every platform is new in 0.5.10.

- **A hang where the picture stopped for good.** When the game needs a lot of
  room in the GPU's command buffer, it asks the GPU to call it back once it has
  caught up (a software method, an interrupt) and sleeps until then. The
  engine's GPU thread clears that interrupt's status whenever no callback is in
  flight, but it asked whether one was in flight first and cleared after, in two
  steps: a callback raised between the two had its status wiped. The game's
  handler then found nothing, never answered, no callback was ever raised
  again, and the game slept for good (its main thread waiting at 0x0019151B in
  the log). The GPU thread now reads the status first, asks after, and clears
  only what it read; and the keeper thread, which ends the vertical blank every
  millisecond, leaves the callback's bit in PMC_INTR_0 to the thread that owns
  it, where it could otherwise wipe a new callback's in a much rarer way.
  `RECOMP_PGRAPH_ACK=1`: as before.
- **Marice's fixes (#46).** The GPU's interrupt status bits are set and
  cleared atomically everywhere: three threads write them, and since 0.5.9 the
  keeper does so every millisecond, so a plain read-modify-write on one could
  undo another's. And a callback whose interrupt bit went missing is raised
  again after three ticks, with a line in the log -- a safety net that should
  now never be needed (`RECOMP_SWM_RESCUE=0` turns it off).
- Tested on a Mac, against 0.5.9 back to back on the same machine: the
  Fortified Residential Zone four times (0.5.10, 0.5.9, 0.5.9, 0.5.10 -- 60
  frames a second throughout, not one slow frame once the level was on, 9 to
  19 callbacks delivered in each), Dogenzaka Hill on OpenGL with SMAA and MSAA
  4x, and Shibuya Terminal from an empty save folder (the camera at a third of
  the stick: 0.109 to 0.110 degree at every one of 197 frames; the sound never
  running dry) -- the same as 0.5.9. Then a test build that holds the GPU
  thread in that gap on purpose: the old way, the callback raised in the gap
  was wiped every time, and without Marice's safety net the game stopped for
  good at the first one, its main thread waiting at 0x0019151B; 0.5.10's way,
  callbacks raised in the gap were kept, and 13 and 25 were delivered in two
  runs without one going missing. Linux (Mesa) and Windows (under Wine), each
  against 0.5.9 on the same machine: the attract demo, the frame rate and the
  vertical blank the same within a few per cent either way; F7 and the
  portable switch. The Android and iPhone builds were built and checked, not
  run.

## 0.5.9 — 27 October 2026

The camera on the right stick turns smoothly, a hang a few seconds into a level
is gone, and the sound chip and the renderer keep closer to the console's
timing. The sound, OpenGL, half-pixel and depth changes come from what
**spriteice** found while working on his own PC port of the game -- thank you,
[spriteice](https://github.com/spriteice)! Every platform is new in 0.5.9.

- **The camera on the right stick turns smoothly.** It turned by the clock,
  while the game moves by its frames, so a slow pan came out in uneven steps
  whenever the frames were not exactly evenly spaced -- "like a really bad
  V-Sync", a player wrote, with the stick a third of the way and the skater
  standing still, while the game's own first-person look panned smoothly. It
  turns by the game's frames now, as the game's camera does: at Shibuya
  Terminal, the same 0.11 degree every frame. And the stick's response is
  softer: a third of the way turns the camera about 7 degrees a second (it
  was under 1), all the way still 150. `JSRF_RCAM_CLOCK=1`: by the clock, as
  before. Thanks to the player who reported it!
- **A hang a few seconds into a level.** CRI's sound library guards its data
  with a lock of its own: whoever takes it is raised to the highest priority
  and the library's server thread is stopped, and on the way out the server
  is started again and the priority put back -- on the console nothing else
  runs in between. Here, stopping or starting another thread let any other
  thread run, and the server could take the same lock right then: the game's
  main thread was left at the highest priority with nothing left to release,
  and went round the library's release loop for ever while every other thread
  waited. The picture stopped a few seconds after a level appeared (`[STALL]`
  in the log). A thread at the highest priority now goes through those two
  calls without letting anything else run, as on the console.
  `RECOMP_TC_HANDOFF=1`: as before.
- **The sound chip, a frame at a time.** The APU was run eight of its frames
  at a time and then rested 5 ms. What the game asks of it -- a voice started
  or stopped -- is answered between frames, so DirectSound could wait up to
  5 ms each time, again and again in a crowd that keeps restarting voices
  (Shibuya Terminal). Each frame is now made at its own moment, every 0.67 ms
  as on the console, kept level with the sound device frame by frame.
  `RECOMP_APU_PACE=8`: as before. And the game's vertical-blank handler,
  which holds back every other game thread while it waits, is now ended
  within a millisecond every frame, not only while the renderer is busy
  (`RECOMP_ACK_KEEPER=2`: as before).
- **OpenGL waits less on its driver.** The renderer asked the driver for
  errors after every draw, which makes a driver working on a thread of its
  own (Mesa's, AMD's, NVIDIA's) catch up with the game first -- hundreds of
  times a frame. Once a frame now (`RECOMP_GL_ERRORS_PER_DRAW=1`: after every
  draw, to find one). On every platform, the commands the game sends the GPU
  are sorted a little quicker, and the clock is read once a frame instead of
  several times a draw.
- **Half a pixel.** The console's driver moves fixed-function draws -- the
  city's scenery, the menus -- half a pixel right and down for its
  rasteriser. The engine kept that, while taking it off the vertex programs'
  draws (the characters, the traffic), so the two did not quite line up.
  Both go without it now. `RECOMP_GL_FF_NUDGE=1`: as before.
- **Depth.** With depth reversed (Metal, and OpenGL where the driver allows
  it), the vertex programs' depth was worked out as one less the depth,
  which keeps at the far end only the steps a 24-bit depth buffer has. It is
  worked out as the far end less the depth now, with all the precision the
  32-bit buffer has there. And polygon offset, which the engine ignored, is
  applied when the game asks for it (`RECOMP_GL_NOPOLYOFFSET=1`: as before).
- AMD graphics on Linux (Mesa's radeonsi driver): the vertex programs without
  0.5.7's invariant position, which has been found to do more harm than good
  there (not tested here: no such GPU). The log says so;
  `RECOMP_GL_INVARIANT=1` keeps it.
- Tested on a Mac, Metal and OpenGL: Shibuya Terminal from an empty save
  folder (the camera at a third of the stick with the skater standing still:
  0.109 to 0.110 degree at every one of 197 frames; the sound chip at 1,500
  frames a second, the sound's output never running dry; 60 frames a
  second), and loaded there 23 more times with and without the hang's fix,
  eight of them with its window held open longer on purpose. The hang came
  once, without the fix, in thirteen loads -- too rare for the eleven loads
  with the fix to say much; the fix rests on the log, which shows exactly
  where the game was stuck. The Garage with and without the half-pixel
  change (as sharp either way). Then the release build on the title, on
  Dogenzaka Hill with SMAA and MSAA 4x (Metal and OpenGL), in the Fortified
  Residential Zone and at Shibuya Terminal. Linux (Mesa): the attract demo,
  and 0.5.9 against 0.5.8's sound chip and vertical-blank timing on that
  slow software renderer -- 7.8 frames a second against 7.2, the game's
  vertical-blank handler 24.8 times a second against 22.7, its main thread
  waiting less than half as long for the others; F7 and the portable switch.
  Windows (under Wine): the title as an upgrade from 0.5.8, F7 and the
  Controls menu. The Android and iPhone builds were built and checked, not
  run.

## 0.5.8 — 26 October 2026

The stutter of a first visit, the sound that stuttered with it, a camera on the
right stick, and two things for Windows laptops. Every platform is new in 0.5.8.

- **Shaders ready from the first visit.** The console's graphics are drawn
  here with shaders made from what the game sets up, and each one used to be
  built the first time the game needed it, with the game waiting: the first
  minute of every area stuttered, on a phone worst of all (one new shader
  froze an iPhone for a quarter of a second). The engine now carries a list of
  the game's shaders -- 343 of them, collected by going through the title, the
  Garage and fourteen areas -- and on the first start of a new version every
  one is prepared (about 30 ms on a Mac) and built ahead: on Metal in the
  background while the title runs, on OpenGL a few milliseconds at a time on
  loading screens. A shader still missing is built when the game asks, as
  before.
  Each machine also keeps what it drew (`states-v1.dat` beside the shader
  cache), so that a later version's first start builds those ahead too.
  `RECOMP_SHADER_SEED=0` leaves the engine's list out. Five areas (the stages
  numbered 50 to 54) are not in the list yet.
- **The sound no longer stutters with the picture.** The thread that runs the
  GPU's work also ended the game's vertical-blank handler, which waits for it
  while holding what every other game thread needs: so whenever the renderer
  was busy -- building a shader, or a frame of eight hundred draws on a phone
  -- every game thread stopped with it, the one that streams the music and the
  voices among them. A small thread now keeps the vertical blank and the
  system clock going while the renderer is busy (`[NV2A] the GPU thread was
  busy for ...` in the log when it was 100 ms or more), and the sound's own
  thread runs above the game's on the Mac and the iPhone, and on Android and
  Linux where the system allows it, as it already did on Windows.
  `RECOMP_ACK_KEEPER=0`: as before.
- **The camera on the right stick**, on every platform, on by default. The
  original game turns its camera only by itself, and the L trigger puts it
  behind the skater. Now the right stick (or the arrow keys) turns it around
  the skater and tilts it, the skater's controls follow the view, and it eases
  back behind them a moment after the stick is let go; the L trigger still
  puts it straight behind at once. Not in conversations and the first-person
  look. Turned off for the original camera: Controls › Camera on the Right
  Stick on the Mac, the Controls menu or F7 on Windows, F7 on Linux, the
  settings on the iPhone and on Android -- remembered.
- **Windows: the dedicated GPU, and full screen remembered.** On a laptop with
  two GPUs, the game now asks for the dedicated one (NVIDIA Optimus, AMD
  PowerXpress) instead of the integrated chip it could be given -- a player's
  flicker in full screen came from that. And the game starts the way it was
  left, in full screen or in a window; `--fullscreen` and the new
  `--windowed` choose for one start.
- On Metal, a slow frame's line in the log (`[HITCH]`) says how busy the GPU
  was meanwhile and how long it was waited for: whether a heavy scene is the
  GPU's or the processor's.
- Tested on a Mac, Metal and OpenGL: Shibuya Terminal and Kibogaoka Hill from
  an empty save folder, with the engine's list (every shader the game asked
  for built ahead: 78 and 102) and without it (75 built while the game
  waited), the right stick turning the camera there and the sound's output
  never running dry; then the release build on the title, on Dogenzaka Hill
  with SMAA and MSAA 4× (Metal and OpenGL) and in the Fortified Residential
  Zone. Linux (Mesa): the attract demo with the list (all but the
  three shaders the title needs first built on loading screens), F7 and the
  portable switch; and, on that slow software renderer, the new sound thread:
  the game's vertical-blank handler ran 46 times a second instead of 39, and
  the game thread's longest wait for a lock was halved. Windows (under Wine):
  the title as an upgrade from 0.5.7 (the list taken in beside the old
  cache), F7 and the Controls menu, the dedicated-GPU switch; full screen kept
  was tried on a test build. The Android and iPhone builds were built and
  checked, not run.

## 0.5.7 — 26 October 2026

What players reported from 0.5.4 to 0.5.6, and two things they asked for. Every
platform is new in 0.5.7.

- **Rails, rafters and walkways are no longer missing.** The renderer kept a
  batch's indices in an array of 16,384 and dropped whatever came after,
  without a word; the console has no such limit. Most of the game's batches
  are far smaller, but a few areas send their scenery in longer ones: on
  Kibogaoka Hill a triangle list of 20,745 indices every frame, in the
  Fortified Residential Zone lists of up to 35,580. Their last thousands of
  triangles were never drawn: on Kibogaoka Hill, among others, a walkway
  and its railing across the back of the square at the start; in the
  Fortified Residential Zone, up to about 12,000 triangles of scenery a frame.
  Batches are kept whole now (up to a million indices), and one past the
  old limit is noted in the log (`[GL] big batch`). Thanks to the players
  who reported it!
- **The boost dash's haze no longer breaks up over shadows and glass.** The
  boost effect draws the frame twice -- sharp, and a small distorted copy --
  and blends the sharp one over the copy by its alpha. The frame is a 16-bit
  surface with no alpha on the console, so that alpha is always 1 there; here
  the targets kept whatever alpha a shadow, a pane of glass or a spray of
  particles had written, and the distorted copy showed through, blocky and
  smeared, exactly there. Surfaces with no alpha channel now keep none
  (`RECOMP_GL_SURF_ALPHA=1`: as before).
- **Less z-fighting in the distance.** Graffiti, windows and signs are drawn
  over the walls they sit on. Two things could make them flicker against the
  wall far away: the position worked out slightly differently by different
  shaders -- it is invariant now -- and a depth buffer with its precision near
  the camera. Depth is now kept reversed, in 32-bit floating point, wherever
  the device allows it (Metal; OpenGL 4.5 or ARB_clip_control, which Windows
  and Linux drivers have; OpenGL ES with EXT_clip_control), which puts the
  precision in the distance. On the Mac's OpenGL and on older drivers depth
  stays as it was; the log says which (`[GL] depth:`).
  `RECOMP_GL_ZREV=0` and `RECOMP_GL_INVARIANT=0` turn either off.
- **Up to 6× the resolution, for 4K screens.** The resolution menus
  (Windows: Video menu and F9; Linux: F9; Mac: Video › Resolution) go to 5×
  and 6×. 6× is 3840×2880: a 4K screen's width one pixel for one, and in
  widescreen its 3840×2160 exactly. Android and iPhone stay at up to 4×.
- **A portable install on Windows and Linux.** On the first start the Windows
  build asks whether to keep its settings, cache, logs and unpacked disc
  images beside **JSRF Unleashed.exe** (portable) or in
  `%LOCALAPPDATA%\JSRF Unleashed`. A file named `portable.txt` beside the exe
  is the switch: `--portable` makes it later (the settings come along), and
  deleting it goes back. A game folder or disc image put in that folder is
  found by itself and remembered relative to it, so the whole folder can move
  to another computer or onto a USB drive. The Linux build has the same switch
  (`portable.txt` beside `jsrf-unleashed`, `--portable`). Both launchers take
  `--scale 1` to `6`.
- For looking into missing geometry: `RECOMP_GL_PRIMLOG=<s>` counts draws and
  vertices by primitive type every s seconds, and the draws dropped before
  they reach the GPU, by reason.
- Tested on a Mac, Metal and OpenGL, 0.5.6 against 0.5.7: Kibogaoka Hill
  (the walkway missing in every one of twelve consecutive frames of 0.5.6,
  there in every one of 0.5.7's) and the Fortified Residential Zone (the
  batch sizes, and where on screen the part past the old limit lands); the
  boost dash in the race against Beat on Dogenzaka Hill (the haze clean);
  Dogenzaka Hill in seven missions with the batch log (none of its batches
  goes past the old limit); then the release build on the title, on
  Dogenzaka Hill with SMAA and MSAA 4× (Metal and OpenGL), in the Fortified
  Residential Zone and on Kibogaoka Hill. Linux (Mesa): the attract demo,
  depth reversed, and the portable switch. Windows (under Wine): the title
  screen, depth reversed, the first-start question and the portable switch.
  The Android and iPhone builds were built and checked, not run.

## 0.5.6 — 25 October 2026

Shadows, which a Windows player reported missing from 0.5.5: they had never
been drawn, on any platform. Every platform is new in 0.5.6.

- **Shadows are drawn.** The game draws its shadows with stencil volumes:
  every frame the shadow volume of each character (made by a vertex
  program) and of the scenery -- buildings, bridges, signs, rails -- is
  counted into the stencil buffer, and then a black quad at half opacity is
  drawn over the screen wherever the count says shadow. That quad is sent
  one vertex at a time (the XDK's `D3DDevice_Begin` / `SetVertexData4f` /
  `End`), a way of drawing the renderer did not know: such a batch arrived
  with no vertices and drew nothing. The volumes were counted and the
  shadows never shown. Now a skater's silhouette falls on the ground under
  them, and the buildings and bridges shade the streets and whoever skates
  through the shade. No other batch of this title is sent that way (the log
  lists each kind, `[IMM]`).
- Metal: the stencil reference is sent with the first draw of each pass. A
  pass starts at 0, and the reference went only with draws that tested the
  stencil, so a test after a pass's first draw could run against 0 -- with
  the shadows drawn, the whole frame was darkened.
- For looking into shadows: `RECOMP_GL_STENCILVIS=1` paints, in place of
  the shadow pass, what the stencil buffer holds there;
  `RECOMP_GL_SKIPVOL=1`/`2` leaves out the characters' or the scenery's
  volumes; `RECOMP_GL_IMMEDIATE=0` leaves the one-vertex-at-a-time batches
  out, as before. `RECOMP_GL_METHTRACE` and `RECOMP_GL_VOLDUMP` log the
  methods between two draws and the scenery's volumes.
- Tested on a Mac (Metal and OpenGL): the title's attract demo and Dogenzaka
  Hill, free and in the race against Beat, with the stencil buffer painted
  in (Metal and OpenGL identical) and with the characters' or the scenery's
  volumes left out; then the release build on the title and on Dogenzaka
  Hill with SMAA and MSAA 4×, on Metal and on OpenGL. Linux (Mesa): the
  attract demo, with its shadows. Windows (under Wine): the game starts and
  runs its title screen. The Android and iPhone builds were built and
  checked, not run.

## 0.5.5 — 24 October 2026

The picture: the game's own mipmaps with anisotropic filtering, anti-aliasing,
and three rendering faults found while looking into a Windows player's
report. Every platform is new in 0.5.5.

- **Distant surfaces are drawn from the game's own mipmaps.** Every texture
  was sampled from its full-size image, so a facade a few hundred units away
  read it at several texels a pixel and shimmered as the camera moved --
  what the player took for z-fighting on distant windows. The console
  sampled the chain of smaller images stored in the texture after its base
  level; that chain is decoded now (swizzled or DXT-compressed, level by
  level), as deep as the texture's registers allow, and sampled with 16×
  anisotropic filtering. `RECOMP_GL_TITLE_MIPS=0` and `RECOMP_GL_ANISO=1`
  turn them off. A chain made by box-filtering the base image is still not
  used: on this game's texture atlases it bleeds the pieces into each other.
- **Anti-aliasing: SMAA and MSAA**, either or both, off by default. SMAA is
  the reference implementation (Jimenez et al., MIT; its licence is in
  NOTICE), a pass over each finished frame. MSAA draws every edge with 2, 4
  or 8 samples a pixel; on OpenGL it follows
  [spriteice's](https://github.com/spriteice/JetSetRadioFuture-Unleashed/commit/e406839d4f3c28618faa3f805a4a2ac96b9afba9),
  and anisotropic filtering came first in his fork too
  ([46c523b](https://github.com/spriteice/JetSetRadioFuture-Unleashed/commit/46c523b19fe1a0f379c44a886df51bc6fe33438b)).
  Mac: Video › Anti-aliasing (on Metal, a new MSAA count restarts the game).
  Windows: the Video menu and F8. Linux: F8. iPhone and iPad: the ⚙ sheet.
  Not on Android yet.
- **Reflective glass and panels reflect.** The console's transform unit makes
  the texture coordinates of reflective surfaces from the direction each one
  faces (`NV097_SET_TEXGEN_*`; this game asks for the eye-space normal), and
  the renderer did not: those surfaces sampled their reflection texture
  wherever their own coordinates pointed. On Dogenzaka Hill's footbridge that
  was dark bars across the roof and glass in odd stripes, shimmering as the
  camera moved. Normal, reflection and sphere maps are made now, as xemu
  makes them.
- **A texture sampled two ways is two textures.** Whichever draw used a
  texture first fixed its filtering and wrapping for every later draw. The
  characters are shaded through one shared texture of tone ramps, which some
  of their materials read clamped and others wrapped, so an arm or a face
  could take another material's tones depending on what was drawn first.
- Tested on a Mac (Metal): Dogenzaka Hill, free and in the race against
  Beat, each change compared on consecutive frames of one run, with and
  without it; then the release build on the title and on Dogenzaka Hill
  with SMAA and MSAA 4×, on Metal and on OpenGL. Linux (Mesa) and Windows
  (under Wine): the game starts and runs its title screen, Windows with
  SMAA on. The Android and iPhone builds were built and checked, not run.

## 0.5.4 — 22 October 2026

More of **spriteice**'s work from
[his fork](https://github.com/spriteice/JetSetRadioFuture-Unleashed/tree/TESTING):
fixes for the hitching some Windows players have seen, a quicker hand-over
between game threads everywhere, and what it takes to build the game from
your own disc, which could not be done before. Thank you again, spriteice!
Each of these commits links the one of his it comes from. Every platform is
new in 0.5.4.

- **Windows: the engine's short waits last as long as asked.** They went
  through winpthreads, part of the MinGW toolchain that builds the Windows
  download, which waits whole milliseconds and then until GetTickCount64,
  moving in 15.6 ms steps, says the time is up. On spriteice's PC (RTX 2080
  Ti, 120 Hz) frame pacing's 1 ms slices took 2–3 ms on average and up to
  17.5 ms, and the GPU thread's 200 µs wait took 12 ms when it timed out.
  That is the wait the game's vertical-blank handler spins on with the guest
  lock held, so every game thread waited with it: the timer thread held the
  lock 13–37% of the time, against 2–5% on Linux. A player's 0.5.3 log (RTX
  3070) has frames of 30 to 65 ms with no shader or texture work in them.
  Both waits now use a high-resolution waitable timer: on spriteice's PC,
  frames reaching the flip late went from 8–26% to 0.7%, and the timer
  thread held the lock 4.8% of the time.
- **A game thread hands the lock over without a pause.** When its time slice
  ran out with another thread queueing, it stood back with Sleep(1): a
  millisecond or more, with no game thread running for much of it. It now
  waits until the other has taken the lock, at most 1 ms
  (`RECOMP_HANDOFF_US`). On a Mac in 99th Street against 0.5.3 (Metal, 225 s
  each): the lock without a holder 1.5% of the time instead of 7.1%, 727
  frames late at the flip instead of 1,288 (of about 13,650), and 8 slow
  frames instead of 34.
- **Building from source works.** The repository now has spriteice's toolkit
  (xboxrecomp v0.15.0's and v0.16.0's pipeline, with his disassembler fixes),
  the runtime pieces the code it generates calls, and the steps in the
  README. Before, code generated from a disc could not link: the downloads'
  generated code came from an earlier version of the tools, with two tables
  (`recomp_seeded.c`, `recomp_holes.c`) made by scripts that were never
  committed. From a clean default.xbe, on a Mac and on Linux: 9333 of 9342
  functions, none failed, and the same code on both. The Mac, Linux and
  Android builds made from it build, and the Mac one plays 99th Street as
  the downloads do. The downloads stay on the older generated code until the
  new one has been played through.
- Windows: MEM_DECOMMIT of guest memory failed every time (guest RAM is one
  mapped view), about once a frame, each one a line flushed to
  `xbox_kernel.log` with the guest lock held. The pages' access is taken away
  instead, as on the other platforms.
- Mac, Linux, iPhone and Android: a read at an offset is one `pread`, not a
  seek and a read that another thread reading the same file could get
  between.
- The APU's and the NV2A's clocks no longer overflow: count × 10⁹ passed
  INT64_MAX after 15 minutes of uptime at Windows' 10 MHz counter, and after
  9 seconds at the 1 GHz the other platforms report.
- Tested on a Mac in 99th Street against 0.5.3, as above: the same missions
  reached and the same frame rate; with code generated from source, the
  same. Linux and Windows (under Wine): the game starts and runs its title
  screen; Wine does not have Windows' timer problem, so it cannot show that
  fix. The Android and iPhone builds were built and checked, not run.

## 0.5.3 — 21 October 2026

Fixes found by **spriteice**, who plays JSRF Unleashed on a ROG Xbox Ally X
and went through the engine in
[his fork](https://github.com/spriteice/JetSetRadioFuture-Unleashed/tree/TESTING).
Thank you, spriteice! Each of these commits links the one of his it comes
from, and says where it was adapted to this engine. Every platform is new
in 0.5.3.

- **The graffiti you pick now shows.** The texture cache knew a texture by
  its address, size, format and 64 bytes sampled from it. The graffiti
  picker loads each chosen design into the same 1024x1024 image, in place,
  and when those 64 bytes did not change, the preview and the posters on
  the walls kept the default graffiti. The whole content of the textures
  being drawn is now compared as well, a megabyte of them at the end of each
  frame, in turn, and a texture rewritten in place is uploaded again a few
  frames later. (Comparing all of them every frame, as first written, took
  0.6 ms a frame on a Mac; in turn, it costs nothing measurable.)
- **Video memory no longer grows at every area.** The game frees an area's
  textures when it leaves it, and the texture cache kept them on the GPU
  until all 4,096 of its slots were full: on the Ally X, from 55 MB to
  306 MB in a minute and a half. Textures whose memory the game has freed
  are dropped once a frame (on a Mac in 99th Street the cache holds 179
  textures, 57 MB), and a texture that changed is uploaded over its old
  entry rather than beside it.
- **The save folder is no longer 10 GB.** The engine backs the Xbox hard
  disk with six image files in the save folder (Partition0.img to
  Partition5.img), and made them the size of the console's partitions,
  9.77 GB together. They were sparse, so on most disks they took almost no
  space, but the folder showed as 10 GB in Explorer, in Finder and in the
  iPhone's Files app, and a copy or a backup that does not keep holes, or an
  exFAT or FAT32 drive, wrote all of it. They now hold only what the game
  writes, about 120 KB, and start empty at each launch, which also shrinks
  the old ones; nothing in them outlived a run anyway (the game formats its
  cache partition at every start). The game's saves are elsewhere, in
  `UDATA` in the game folder, and are not touched.
- **Windows: two per-thread variables were shared by every thread.** MinGW,
  which builds the Windows download, ignores `__declspec(thread)`: the
  file path the kernel had just translated, which the cutscene player reads
  back (another thread's open could replace it, and a cutscene not play),
  and the game's IRQL. Both are thread-local now, as on the other platforms.
- The game's IRQL is read from the register it is passed in (KfRaiseIrql
  and KfLowerIrql are fastcall); it was the low byte of whatever the caller
  had pushed last.
- The guest lock's time slice runs from when the lock last changed hands,
  not from the holder's latest retake after a kernel call, and for up to
  50 ms it does not end at raised IRQL or inside an interrupt, DPC or
  synchronize routine. `RECOMP_SLICE_FROM_RETAKE=1` measures it the old
  way. On spriteice's build the timer thread, which delivers the vertical
  blank and the APU interrupt, queued for the lock most of the time in
  game; on a Mac in 99th Street it made no measurable difference, the slice
  already running out once a frame.
- A draw to a surface of another size under the same viewport now uses that
  surface's size.
- Tested on a Mac in 99th Street against 0.5.2 (Metal, 225 s each, New
  Game and the left stick): the same frame rate and hitches, textures the
  same share of the frame. Linux and Windows (under Wine): the game starts
  and runs its title and attract demo, and the partition images stay small.
  The Android and iPhone builds were built and checked, not run.

## 0.5.2 — 21 October 2026 — Windows only

A second Windows-only fix for the sound, which still stuttered in 0.5.1 for
some players. The Mac, iPhone, Linux and Android downloads are 0.5.0's,
unchanged: the 0.5.2 release carries them over, and they still say 0.5.0.

- **Windows: the game's clock in 15.6 ms steps.** The engine's kernel timed
  everything on Windows' GetTickCount64, which moves in steps of the system
  tick, 15.6 ms, whatever timeBeginPeriod asks: the display interrupt that is
  the game's frame clock, the game's own timers and DPCs, and the 4 ms slice
  after which a game thread hands the guest lock to the next. On Windows
  alone, then, all of those came in 15.6 ms steps. A player's 0.5.1 log shows
  the frame clock slipping a whole step every 15 frames, a 30 ms frame four
  times a second: a 60 Hz clock read off a 64 Hz tick. The 4 ms slice became
  15.6 ms, so the ADX server thread that refills the music waited about four
  times as long for its turn, and the music ran dry.
- On Windows the kernel's GetTickCount and GetTickCount64 are now the
  performance counter in milliseconds, as they are CLOCK_MONOTONIC on the
  other platforms. The engine no longer imports either.
- The game asks Windows 11 not to throttle it when its window is covered or
  minimised, neither its speed nor its timer resolution (on Windows 10, the
  speed alone): the Windows side of the Mac's App Nap opt-out.
- 0.5.1's fix (every game thread at normal priority on Windows) is kept.
- Tested under Wine, where the game starts and runs its title screen as
  before; the stutter itself needs a real Windows PC to show.

## 0.5.1 — 21 October 2026 — Windows only

A Windows-only fix. The Mac, iPhone, Linux and Android downloads are
0.5.0's, unchanged: the 0.5.1 release carries them over, and they still say
0.5.0.

- **Windows: sound that stutters when the PC is busy.** The game gives each
  of its threads a priority, as Xbox games do, and on Windows those were
  applied to real Windows threads; on the Mac and Linux, SetThreadPriority
  is the engine's own and only remembers the number. The game puts CRI's ADX
  worker, the thread that reads and decodes the music stream, at the lowest
  priority, idle. On the console that thread runs whenever the others wait,
  half of every frame; on a PC it runs only when no normal-priority thread
  anywhere wants its core (the game's own threads, a browser, a chat app),
  so under load it starved and the music ran dry. The game's other requests
  were applied too: highest and above normal for three threads, and one
  thread raising itself to time-critical and back about fifty times a
  second.
- Every game thread now runs at normal priority on Windows, as on the other
  platforms, and the game reads back the priorities it set.
  `RECOMP_THREAD_PRIORITIES=1` applies them as before.
- The APU's frame thread, which makes the sound 5 ms at a time and sleeps
  in between, runs at the highest priority below time-critical on Windows,
  so that a busy moment delays the picture rather than the sound (SDL's
  device thread, which plays what it makes, was at time-critical already).
- Tested under Wine, where the game starts and runs its title screen as
  before; Wine does not apply thread priorities, so the stutter itself could
  not be reproduced there. The game's requests were logged on Linux.

## 0.5.0 — 21 October 2026

- **Less stutter: new textures appear about six times faster.** The first
  time the game uses a texture it is converted on the CPU, and a new area
  brings dozens at once: when 99th Street starts, 59 arrive in one frame and
  65 in another, and each was a freeze of about a fifth of a second; new
  parts of an area coming into view mid-play cost smaller ones. A compressed
  (DXT) texture was decoded per pixel, decoding its whole 4×4 block for every
  pixel of it, and a swizzled one worked out where each pixel lives from
  scratch every time. Each block is decoded once now, and the layout is worked
  out once per texture: the same 59 textures take about 35 ms, with exactly
  the same pixels.
- **Less stutter: shaders are kept, and built before the game needs them.**
  The first time the game draws something new, its shaders are built and
  the game waits. Every shader built is now kept, in a `shaders` folder in
  the app's own data folder (on a Mac, `~/Library/Application Support/JSRF
  Unleashed/save`; not the game folder), and at the next start they are built
  again before they are needed: on Metal by two background threads while the
  title screen runs, with the pipeline states each was drawn with; on OpenGL
  a few milliseconds at a time on frames with little to draw (menus, loading
  screens). Where the driver hands out compiled programs (Windows, Linux,
  Android), each is kept compiled, for that driver, and loaded instead of
  built. With OpenGL on a Mac, which keeps nothing between sessions, four
  minutes on 99th Street spent about 430 ms building shaders, and now about
  50 ms; on Linux (Mesa) a shader that took 13.8 ms loads in 1.5 ms. On Metal,
  macOS keeps compiled shaders itself, so the gain there is small. A shader
  needed for the first time on a machine is still built on the spot. The
  folder can be deleted at any time; `RECOMP_SHADER_CACHE=0` turns the cache
  off and `RECOMP_SHADER_BINARIES=0` keeps sources only.
- **No more grey spikes.** Some objects shot long grey spikes across the
  screen for a moment, for instance on the staircase that winds around the
  garage's centre platform. The reciprocal step of the game's vertex
  programs (RCC) lost its sign, so points just behind the camera were thrown
  across the screen; it keeps its sign now, its size held between 2^-64 and
  2^64 as on the console, the perspective divide never meets a zero, and the
  address register is set as the console sets it.
- **Mac: no stutter with the window covered.** macOS slows down the timers
  of an app whose window is covered (App Nap), and the game paces itself,
  streams and plays its music on timers: a covered window stuttered in
  quarter-second stalls. The game declares itself latency-critical work for
  as long as it runs.
- **The log says what each stutter was.** Every frame that took one and a
  half frames or longer is written to `last-run.log` (`[HITCH]`) with the
  shaders built in it, the textures converted and the time left to the game
  itself; the first 200, then one in 50, and a total at the end.
- Tested on the Mac with Metal and OpenGL: New Game, then 99th Street,
  four minutes of skating, each renderer with and without the shader cache
  and with the old and the new texture conversion; the final Mac app ran the
  title on both. The new texture conversion was compared with the old one
  pixel for pixel on DXT1, DXT3 and DXT5 at many sizes, and the swizzle
  offsets on every power-of-two size up to 2048×2048. The shader cache also
  ran on Linux (Mesa) with its files empty, filled and damaged; the final
  Windows build ran under Wine twice, its shaders built and saved the first
  time and loaded already compiled the second, and the final Linux build ran
  on Mesa. The Android and iPhone builds were built and checked, not run.

## 0.4.9 — 20 October 2026

- **The character select shows the skaters where they are.** Each rudie
  on the select screen skates in two small windows, and there they came
  out in the wrong place: too big, too low and too far right, sunk into the
  floor or under the rail they were grinding with the sparks alone on it,
  and in the small window often out of frame. The game draws those windows
  as viewports inside the screen. The scenery and the effects go through
  the console's fixed-function path, which the renderer placed right; the
  skaters are drawn by the game's own vertex programs, whose output the
  renderer stretched over the whole screen, as if each window were the
  screen. They are put back in their windows now. Everything drawn full
  screen, which is every stage, comes out exactly as before.
- **Mac: the game can be set up again.** In "Pick the folder of extracted
  game files, or a disc image (.iso)", choosing the folder and clicking
  Use This only opened it, and inside it the button stayed grey: the window
  accepted .iso files and nothing else. And an .iso was handed to
  extract-xiso, which is not part of macOS, so without Homebrew the app
  could only say to install it. Folders and disc images can both be chosen
  now, and the app unpacks an image by itself, with a progress bar, using
  the unpacker the iPhone, Android and Windows apps already had (the engine
  does it: `--unpack-iso`). A folder with the image in it works too, and
  either can be dropped on the app's icon or given on its command line.
- **Linux: disc images without extract-xiso.** The launcher unpacks an
  .iso by itself as well, with a progress window when zenity is there, so
  nothing has to be installed, on a Steam Deck either.
- **Wrong files are named, before anything is unpacked.** JSRF Unleashed
  is made from one build of the game, address for address: the standalone
  US disc. A player whose files were another build saw the game crash a few
  seconds in (a "misaligned access" on a Mac, with 0.4.6). Every app now
  checks the files first and says what they are instead: another game's
  disc (by its name), a zip or other archive to unpack first, a compressed
  disc image, a disc with no game on it, or another build of Jet Set Radio
  Future -- the European or the Japanese release, or the Sega GT 2002 +
  JSRF combo disc, the copy that came with the Xbox, whose JSRF is a later
  build than the standalone disc's. A disc image is checked before it is
  unpacked (the engine reads its default.xbe out of it: `--check-iso`).
  Only the layout of default.xbe is compared, not the bytes, so a US
  default.xbe with a patch applied (the region, a widescreen hack) still
  runs.
- Tested on the Mac with Metal and OpenGL: the character select from a save,
  and the garage after it, unchanged. The Mac app unpacked the game's disc
  image in a few seconds, all 3,221 files identical to the extracted game's,
  and named another game's image without unpacking it; the check was also
  run on a zip, on a file that is no disc image, and on copies doctored to
  look like a European release and a combo disc, and the Linux launcher
  (with its progress window) and the Windows launcher (under Wine) refused
  the same wrong files. Earlier builds of this version ran on Mesa, under
  Wine and in the Android emulator; the final Android and iPhone builds were
  built and checked, not run.
- Every download is 0.4.9: Mac, iPhone, Linux, Windows and Android.

## 0.4.8 — 20 October 2026

- **The robot at the future site of the Rokkaku Expo Stadium starts.** A
  player on Android 0.4.7 could not finish the Urban Battle there ("Destroy
  the Rokkaku piece of junk!"): the robot hung where it had been made, out
  of reach, and never moved, attacked or could be tagged. Every earlier
  version had the same soft lock. The robot's first step, the one that puts
  it in the stadium and starts it, was one of four functions of the game
  that the translation had never found. Each of the four sits right after
  another function, and the recompiler took it for the end of that one, so
  no earlier scan saw it as missing. The game called it every frame and
  every call was skipped. It is in now: the robot fights at ground level,
  moves around the stadium and attacks, and its arms can be grinded and the
  marks on it sprayed.
- **The Rokkaku robots make their sounds.** Their sound function was another
  of the four. The Terror Drone at Hikage Street calls it four times a frame
  for its thrusters; every call was skipped, so the robots were silent.
- The other two are handlers in two of the game's per-object tables, each
  placed right after a jump table that the recompiler read as code. Nothing
  has been seen calling them yet.
- Found by listing every address that the game's tables or its code's
  constants hold, that follows the end of a function, and that no
  translated function starts at — inside other functions this time.
- Tested on the Mac with the game code of 0.4.7 and of 0.4.8. With 0.4.7's,
  the robot at the future site never leaves its first state and the game
  calls the missing function every frame; with 0.4.8's it goes through its
  whole routine, and a test player put on its arms grinded them and sprayed
  one of its marks off. The Terror Drone at Hikage Street, which worked
  before, was sprayed the same way with both and moved on after each mark.
  The Linux build was started on Mesa, the Windows build under Wine and the
  Android build in the Android emulator.
- Every download is 0.4.8: Mac, iPhone, Linux, Windows and Android.

## 0.4.7 — 15 October 2026 — Android only

An Android-only fix. The Mac, iPhone, Linux and Windows downloads are
0.4.6's, unchanged: the 0.4.7 release carries them over, and they still say
0.4.6.

- **On-screen controls and controllers on phones that call one of their
  own parts a gamepad.** Two players saw the intro play with no
  on-screen controls, and their Bluetooth controllers (an Xbox Series X
  controller, an 8BitDo Ultimate 2) did nothing. Many phones list one of
  their own parts as a gamepad: the device their fingerprint reader or side
  buttons send keys through declares gamepad buttons. The app counted it as
  a controller and hid the on-screen controls, and SDL took it for one too:
  it was opened first, as player 1's controller, and a real controller went
  in as player 2's, which the game does not read.
- A device now counts as a controller only if it has a stick, or a d-pad
  that reports as one (SDL's own rule for this, which the app now turns
  on). On those phones the on-screen controls show, and a controller
  connected to them is player 1's.
- The log says what the app made of each device Android calls a gamepad
  (its name, how many axes it has, whether it counts as a controller) and
  whether the on-screen controls are showing.
- Tested in the Android emulator with a stand-in for such a phone part,
  there from the start, and a controller connected 20 seconds in that
  pressed Start and A. With 0.4.6 the on-screen controls were missing and
  the controller did nothing. With 0.4.7 the controls showed, went away
  when the controller connected, and the controller started the game.

## 0.4.6 — 15 October 2026

- **Skyscraper District and Pharaoh Park: the city is back.** A player on a
  Steam Deck saw his character there and nothing else: the whole city around
  him was black. These areas switch the console's fog on (black, from 300 to
  3000 units away), and the engine got fog wrong for the scenery, which the
  game draws with the console's fixed-function transform: every one of its
  vertices was given a fog factor of zero, which paints it entirely in the
  fog colour. Black fog, black city.
- **The areas with a coloured fog** had the same bug, in their own colour:
  three of the later areas came out as a flat wall of pinkish brown or olive,
  with the characters and a few objects standing in it.
- Fog is now worked out the way the console does it. The fixed-function
  transform measures each vertex's distance from the camera the way the game
  asks it to, the game's own vertex programs supply theirs as before, and
  both become a fog factor through the fog mode and the start and end
  distances the game sets. Distant buildings fade into the night or the haze
  as they should, instead of the scenery vanishing, and the scenery the game
  draws with its own programs, which never faded at all, fades too.
- The renderer is the same code on every platform (Metal on the Mac and
  iPhone, OpenGL on Linux, Windows and Android), so every download has the
  fix.
- Tested in Pharaoh Park with the Mac build on OpenGL, the renderer of the
  Linux and Windows builds, and on Metal, and in the iPhone Simulator, which
  also went through one mission in each of 13 areas, from Shibuya Terminal
  to Pharaoh Park: no crash, no shader that failed to compile, the three
  areas with a coloured fog drawn as they should be, and the areas without
  fog exactly as before. The Linux build was started on Mesa and the
  Windows build under Wine, every shader compiling, and the Android build
  ran in the Android emulator.
- Every download is 0.4.6: Mac, iPhone, Linux, Windows and Android.

## 0.4.5 — 15 October 2026

- **Chuo Street: no more crash at the third tank.** A player's log showed
  the game crashing every time he tagged his third tank. The tanks' sound
  function was one of 27 functions of the game that the translation had
  never found: their addresses appear only as constants in the game's code
  (written into an object, into a table filled at startup, or handed to a
  new thread), which neither the recompiler nor the later scans of the
  game's tables looked at. Each call to it was skipped, and the skip left
  the stack 8 bytes off; when the third tank was tagged, a tank routine came
  back 16 bytes off and the game crashed. The 27 are translated now.
- **Sounds that never played.** 17 of the 27 are sound functions, one for
  each of 17 kinds of objects, the tanks among them. Whatever sounds they
  start never played until now.
- **A missing function costs less.** A call into code that was never
  translated is still skipped, but the stack is now left the way the calling
  code expects, so the next missing function costs a sound rather than the
  game. The log names each one in full four times, then once every thousand
  calls (the player's log was 1.4 MB of the same lines).
- Tested in the iPhone Simulator, jumping straight into the tank fight and
  skating toward the tanks with the same script on 0.4.4 and on 0.4.5:
  0.4.4 skipped the tanks' sound function 793 times; 0.4.5 skipped nothing,
  started 329 sounds against 211 and did not crash. The third tank itself
  has not been reached in a test yet.
- Every download is 0.4.5: Windows, Linux, Mac, Android and iPhone. The
  Mac download, 0.2.0's until now, and the Linux one, 0.3.1's, have every
  change since that applies to them.

## 0.4.4 — 15 October 2026

- **iPhone: the picture can fill the screen.** At 16:9 the game has black
  bars at both ends of a 19.5:9 iPhone. **⚙ › Picture › Fill the screen**
  widens the view to the screen's own shape, so the city reaches both edges,
  while the menus and the HUD keep their shape in the middle. **16:9** stays
  the default, and the console's **4:3** is the third choice; the title
  screen stays 4:3 in all three. An iPad's screen is narrower than 16:9, so
  there Fill the screen is 16:9. Run in the iOS Simulator (an iPhone 16 Pro:
  a new game, the first scenes).
- The picture is a new setting, so if you had switched widescreen off, the
  app starts at 16:9 once after the update: pick 4:3 in **⚙ › Picture** to
  have it back.
- **iPhone: a stability fix the other versions already had.** A repair for
  registers some of the game's translated functions fail to preserve, whose
  absence can crash a long session, was never switched on in the iPhone app:
  its settings were set too late for the part of the engine that reads them.
  It is on from the start now (the first line of `Logs/last-run.log` says
  so).
- The iPhone app is updated through AltStore and SideStore: it shows up in
  **My Apps** as an update, and keeps the game and your saves.
- The Windows download is 0.4.2's, the Android download 0.4.3's, the Linux
  download 0.3.1's and the Mac download 0.2.0's, unchanged.

## 0.4.3 — 14 October 2026

- **Android: the faster renderer** from 0.4.2. Geometry that does not change
  stays on the graphics chip between frames instead of being converted and
  sent again every frame, and the game's constants are sent only when they
  change. On a phone, where moving memory costs the most, that was most of
  the renderer's own work in the busy scenes. Played in the Android emulator;
  not yet on a phone.
- The Windows download is 0.4.2's, the Linux download 0.3.1's, and the Mac
  and iPhone downloads are 0.2.0's, unchanged; AltStore keeps offering 0.2.0.

## 0.4.2 — 14 October 2026

- **Windows: Xbox controllers work.** An Xbox pad was found and opened but
  its input never arrived: SDL hands it to a hidden window, which belonged to
  the game's own thread, and that thread never reads its messages. SDL now
  keeps a thread of its own for it, which also finds pads plugged in later.
  The log lists the input devices SDL sees (`[PAD]` lines in
  `last-run.log`).
- **Windows: a Video menu** at the top of the window, as on the Mac: the
  resolution (1× to 4×), widescreen and full screen. It is hidden in full
  screen; **F9**, **F10** and **F11** still work everywhere. F9 in full
  screen no longer looks like a hang: its box was hidden behind the game
  until Alt+F4 closed it, so the resolution never changed. Full screen is now
  left while the box is up.
- **A faster renderer** (in the Windows build; the Linux and Android ones get
  it when they are next updated). Geometry that does not change stays on the
  graphics card between frames instead of being converted and sent again
  every frame (about 15 MB a frame in the busy scenes), and the game's
  constants are sent only when they change: the renderer's own work in a
  busy frame went from about 9 ms to under 2 on the test machine, which
  leaves more of each sixtieth of a second to the game. Played under
  CrossOver on a Mac.
- The Android download is 0.4.0's, the Linux download 0.3.1's, and the Mac
  and iPhone downloads are 0.2.0's, unchanged; AltStore keeps offering 0.2.0.

## 0.4.1 — 14 October 2026

- **Windows: smoother frames.** Each frame now reaches the screen in step
  with the display, from a thread of its own, as the Metal renderer does it
  on the Mac, and the game is paced to the display's refresh: on a 60 Hz
  screen (and on 120 or 240 Hz ones) each frame is shown once. Before, the
  game's own clock and the display's drifted apart, and every few seconds a
  frame stayed up twice and the next was not shown at all. Played under
  CrossOver on a Mac; not yet on a Windows PC. The log now says every 10
  seconds how many frames were shown and how long each stayed on screen
  (`[PRESENT]` lines in `last-run.log`).
- The Android download is 0.4.0's, the Linux download 0.3.1's, and the Mac
  and iPhone downloads are 0.2.0's, unchanged; AltStore keeps offering 0.2.0.

## 0.4.0 — 14 October 2026

- **Android** (new): `JSRF-Unleashed-android-arm64.apk`, for
  Android 10 and later on 64-bit phones and tablets with OpenGL ES 3.0. Its
  first screen unpacks your disc image (`.iso`) into the app, or takes the
  extracted game folder copied over USB. Controllers through SDL, on-screen
  controls as on iPhone (a stick, the pad's buttons, Start and Back at the
  top), a keyboard; Android's Back is the Xbox Back button. The ⚙ button has
  widescreen, the on-screen controls and the resolution. The renderer is the
  Windows and Linux one, in OpenGL ES. Played in the Android emulator
  (unpacking a disc image, boot, title, attract demo, the controls and the
  settings); not yet on a phone.
- The Windows download is 0.3.0's, the Linux download 0.3.1's, and the Mac
  and iPhone downloads are 0.2.0's, unchanged; AltStore keeps offering 0.2.0.
- Building: `launcher/android/` has the app and `build_apk.sh`, which builds
  it with the Android SDK's own tools (the NDK, aapt2, d8; no Gradle).

## 0.3.1 — 14 October 2026

- Linux: the F9 resolution window's text is no longer cut off at the right
  edge (SDL's own dialog on X11 does not wrap long lines).
- The Windows download is 0.3.0's, and the Mac and iPhone downloads are
  0.2.0's, unchanged.

## 0.3.0 — 14 October 2026

- **Windows** (new, experimental): `JSRF-Unleashed-windows-x86_64.zip`, for
  Windows 10 and 11, 64-bit. Unzip it and run `JSRF Unleashed.exe`: it asks
  for the game folder, or a disc image, which it unpacks once. The same engine
  and OpenGL renderer as the Linux build, controllers through SDL, the
  keyboard. Played under CrossOver on a Mac and under Wine on Linux (boot,
  title, loading a save, the garage); not yet on a Windows PC.
- On Windows and Linux, **F9** sets the resolution — 1× to 4× the Xbox's
  640×480, up to 2560×1920 — and restarts the game to apply it; **F10**
  switches widescreen and **F11** full screen. The resolution and widescreen
  are remembered (on Windows, widescreen was forgotten at every start).
- **Linux**: rebuilt from the 0.3.0 tag for F9; nothing else changes for it.
  (For the first hour after the release, the Linux download was still 0.2.0's
  build.)
- The Mac and iPhone downloads are 0.2.0's, unchanged (they call themselves
  0.2.0); AltStore keeps offering 0.2.0.
- Docs: saves are kept in the game folder, in `UDATA`, on every platform — not
  in Application Support (Mac), `~/.local/share` (Linux) or `Saves` (iPhone),
  as the README said.
- Building: `launcher/windows/` has the Windows launcher, its packaging, and
  the toolchain file for cross-compiling with llvm-mingw.

## 0.2.0 — 12 October 2026

- **iPhone and iPad** (new, experimental): `JSRF-Unleashed-iOS.ipa`, for
  sideloading (AltStore, SideStore, Sideloadly). The game goes into the app's
  folder in the Files app — the extracted folder, or the disc image, which the
  app unpacks itself. Controllers, on-screen controls (a stick and the pad's
  buttons), a hardware keyboard. Played in the iOS Simulator; not yet tried
  on a real device.
- **Mac: Metal is the renderer now.** Each frame goes straight to the window,
  every one of them reaches the screen, and on a 120 Hz display (a MacBook
  Pro's) nearly every frame stays up for exactly a sixtieth of a second:
  smooth 60 fps. Less CPU, too. OpenGL is still there, under Video › Renderer.
- **Fixed: the picture could freeze for good** now and then during play (all
  platforms). The game's graphics driver sometimes waits for an interrupt the
  GPU raises when it reaches a marker in the command stream; nothing raised
  it.
- Widescreen: on the character select the characters stay inside their
  panels; the boot logos fill the 16:9 screen.
- Fixed: a 1-pixel dark line along the right and bottom edges.
- Logs are a small fraction of the size (most of a log was one line per GPU
  submission).
- Building on a Mac needs nothing from Homebrew.

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
