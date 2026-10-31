# Windows prototype validation

Upstream base: `7ef9e989259cdba8c4c7157bfce866d87d0f97cf` (`main`, v0.6.0-era).

## Checks on the contribution source

- `python -m pytest tools/ -q`: **838 passed, 114 skipped, 91 subtests passed**.
- Native Release engine build using the existing llvm-mingw/SDL/OpenGL Windows toolchain: **passed**.
- `build.ps1` for the WinForms companion, opener and trial launcher: **passed**.
- `test.ps1`: **passed** for config replacement contention, 500 rapid fader updates, journal recovery, fader mapping/keyboard/accessibility/drag colour, Music quick-lower/restore, legacy migration and telemetry parsing.
- Backend C11 test: **passed** for path classification, independent groups/mutes, Master applied once, multipass bypass and restoration to unity gain.
- Telemetry regression: empty JSON is confirmed to deserialize to null; empty/null/malformed groups and all truncated prefixes are rejected; 200 concurrent in-place writes recover to a complete snapshot without an exception.
- Manual wrapper scan: the two mixer wrappers are discoverable when regenerating code from the manual source.
- `git diff --check`: **passed**.

`python -m tools.conformance` was also attempted. It exits with **no 32-bit x86 toolchain** on this machine, so differential conformance is **not claimed to pass**. No lifter changes are part of this patch.

## Native prototype observations

Before the publication formatting/API-name cleanup, the same functional implementation was tested in the game on Windows:

- Build, boot and the Audio menu entry work.
- F6 hides the foreground mixer, returns keyboard focus to the full-screen game and reopens the same panel without restarting the game.
- Twelve concurrent opener requests leave one panel and one game. Six duplicate launch requests exit behind the startup mutex.
- Ctrl+Alt+M moves the visible Music fader from 100 to 25 and back; backend diagnostics report a matching 0.25 gain while other percentages stay unchanged.
- Music attenuation was accepted by the local player. The new dialogue/DJ/ambience grouping still needs extended scene-specific listening.
- Both native game chrome and the companion were visually inspected in light/dark process-only previews. Windows theme settings were not changed.

The publication cleanup was rebuilt and retested as listed above. Existing installed game files, saves and cloud launch routes were kept outside the contribution.

## Open review points

- This is a Windows companion prototype, not a finished cross-platform settings integration.
- Guest object-layout assumptions and filename-based routing target the tested USA title executable and need maintainer review for other revisions.
- Already mixed vocals/sample recordings cannot be split into separate buses.
- Extended listening, interaction acceptance, other-platform compilation/runtime checks and the differential suite remain open.
- The system-theme/menu changes can be split out if an audio-only first contribution is preferred.

Developed with AI assistance and tested locally. No game executables, generated game code, game assets or saves are included.
