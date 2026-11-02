# Persistent right-stick camera (opt-in prototype)

This option keeps a manually chosen camera view until the player presses LT.
It targets the camera auto-centering request in issue #45. It does not change
motion blur and does not implement the separate character-relative Tracking
request in #44.

## Controls

- Native assistance remains active until the right stick takes manual control.
- The right stick orbits the camera; free-skating movement is relative to the
  view the player sees. Releasing the right stick keeps its heading/elevation.
- Left-stick release/repress, coasting, speed and elapsed time never request
  a return. There is no automatic wait, blend or independent movement reference.
- LT relinquishes manual ownership and invokes the game's native reset/target
  semantics. The right stick can take control again. LB and boost are unchanged.
- During grinding, orbit is visual: rail heading and movement stay native. A
  validated exit from rail mode keeps manual ownership even with RS released,
  continuing from the last actually shown pose with controls relative to it.
- Photo mode, the map selector, scripts, targets/actions, focus changes, stale
  input and unsupported modes retain native priority. They can invalidate manual
  state rather than forcing orbit through an unsafe context.
- Existing Right-stick Off and Tracking preferences remain authoritative; this
  prototype applies only when Camera mode (1) is selected.

The feature is off by default. There is no new menu, launcher, audio change,
cloud integration or save-format change in this contribution.

## Enable for a trial

Build the engine with the repository's normal dependencies and generated code
from your own supported USA executable. In PowerShell, for the current process:

```powershell
$env:JSRF_CAMERA_EXPERIMENT = "1"
$env:RECOMP_RSTICK_CAMERA = "1"
.\build\jsrf_recomp.exe "C:\path\to\your\game"
```

Unset `JSRF_CAMERA_EXPERIMENT` to return to the upstream camera implementation.
Do not enable the option on an unreviewed executable/layout revision.

## Build and regression tests

The normal `port/CMakeLists.txt` build derives a small input wrapper in the
**build directory**, with an explicit dependency on the generated input source
and the preparation script. It works with an external `JSRF_GEN_DIR`. Missing
outputs are regenerated on the next build; source checkout writes are not
required. The script accepts either an original function name or its `_gen`
wrapper alias and verifies the normalized body against an exact SHA-256 preimage.
Unexpected code fails closed instead of guessing at instruction locations.

For the optional offline tests, add `-DJSRF_CAMERA_TESTS=ON` to the normal CMake
configuration and run:

```text
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The core test is platform-independent. Bridge/trace fixtures currently target
Windows; test assertions stay enabled in Release builds. No game bytes or assets
are embedded in these fixtures.

The focused tests cover:

- Persistent manual view and `control=view`, native reset and renewed RS input.
- Repeated LS release/repress during physical coasting, with no delayed return.
- Native LT precedence, unchanged pad data, actor identity and protected guards.
- Visual-only rail orbit and manual ownership through the tested rail exit.
- A 180-degree native exit pose, scoped continuity reset and guest-write limits.
- Existing Off/Tracking preference fallback.
- Bounded read-only diagnostics, controller mirror resolution, failed reads,
  explicit arming and first-manual-rail automatic arming.

## Implementation boundaries

The bridge permits normal camera/player mode 3 and visual-only rail mode 5 with
input mode 0/2. It validates reciprocal player/input/camera identity and fresh,
focused pad input. The input hook selects a local heading at the verified native
movement branch; it preserves the game's original angular mapping, magnitude,
quantization, flags and history updates. It does not broadly overlay guest
camera fields before native input. Targeting always calls its original body.

The guarded owned5-to3 exit deliberately clears native camera+0x238 once after
the completed native publication, so native direction-continuity memory cannot
emit an old input8E4 command. This is an intentional policy change, not cleanup
of a flag owned by the patch. Camera234, input/actor histories and targeting are
not rewritten. Tokens expire on identity/focus/context changes, more than 250 ms
or a gap over 6 frames. Unknown air modes are not treated as equivalent.

Movement sampling is diagnostic only and cannot affect camera ownership.
The SDL focus adapter is implemented; non-SDL adapters still require platform
review before enabling the feature there.

## Optional local diagnostics

Set `JSRF_CAMERA_TRACE_FILE` to a local CSV path to enable bounded capture
(120 seconds, 8 MiB). `JSRF_CAMERA_TRACE_ARM_FILE` defers capture to a marker at that
path; `JSRF_CAMERA_TRACE_TRIGGER=rail` arms on the first manual rail orbit.
Use a unique marker path per run. The trace queues records without waiting;
a worker handles file I/O. Nothing is uploaded. Leave these variables unset
for ordinary use.

## Validation scope

Human gameplay acceptance was recorded for the isolated Windows 0.6.5-based
prototype: skating, manual orbit, LT reset and the previously problematic
coasting/left-stick case. This contribution is rebased onto 0.6.6. Technical
build/test results are recorded in the proposal; gameplay on that rebased build
and non-Windows platforms still needs confirmation.

No executable, lifted game function body, game asset, graffiti, profile or save
is part of this contribution. The wrapper is reproduced locally from owned
input code and must not be committed or included in a source patch.
