# Windows audio mixer companion

This local prototype adds independent source levels to JSRF Unleashed and an **Audio** menu next to **Video** and **Controls**. It is based on upstream `main` at `7ef9e989259cdba8c4c7157bfce866d87d0f97cf` (after the v0.6.0 release).

## Using the mixer

- Open **JSRF-Mixer.exe** from the trial shortcut. It starts this engine build if needed, or reopens its existing mixer.
- In JSRF, press **F6** or choose **Audio > Audio mixer...**. Press **F6 again in the mixer** to hide it and return focus to the game. F6 also works in full screen, where the menu bar is hidden. Repeated openings reuse the same panel and game; a held key does not toggle repeatedly.
- Click anywhere along a fader to jump directly there. Drag to adjust continuously; the thumb turns orange while held. Arrow keys change one percent, Shift+arrow changes ten; Home/End select 0/100.
- **Ctrl+Alt+M** sets Music to one quarter of its current level; press again to restore it. Moving Music manually starts a new baseline. This shortcut changes the visible Music fader, with no extra attenuation stage.
- **Mute** preserves a group's fader position. **Reset mixer** restores all levels to 100% and clears mutes.
- Closing the panel hides it in the notification area. F6, the tray icon or the trial shortcut reopens it. **Exit mixer** in the tray menu exits the companion; reopening it later attaches to the same running trial game.

The companion, taskbar/tray icon and all added text use English. The mixer and native game title/menu bars follow Windows app light/dark mode and respond to theme changes. High contrast uses system colours. No Windows theme setting is changed.

| Group | Sources |
| --- | --- |
| Master | Final output, applied once |
| Music | `Media/Z_ADX/BGM/*.adx` |
| Effects / samples | Unclassified hardware voices and software sample playback |
| Dialogue / radio | `MUSEN` and `MUSENJ` ADX streams |
| DJ | `DJ` and `DJ_DEMO` ADX streams |
| Ambience | `ENV` ADX streams |
| Other streams | Other ADX paths or conflicting classified routes |

Vocals recorded inside a song stay with Music. Vocal samples in a sound bank stay with Effects / samples. The mixer does not separate already mixed recordings. Meters and active counts are diagnostic indicators, not calibrated loudness meters; inactive groups become useful when the game plays those sources.

## Building and packaging

Build the engine normally from the accompanying source changes, supplying your own legally obtained USA game executable and regenerated code. Derived game code and assets are excluded from this proposal.

Run `build.ps1 -OutputDirectory <trial-folder>` in Windows PowerShell. It uses the Windows .NET Framework 4.x C# compiler and WinForms. Place the built engine and its normal runtime DLLs in `<trial-folder>/StageMixer/`. Create `<trial-folder>/Profile/save`, `<trial-folder>/Logs`, and a trial `Profile/settings`; supply a copied save profile if desired. Put the extracted game directory in `game-directory.txt`, or place it in `SandboxGame` next to the companion. Start `JSRF-Mixer.exe`.

`JSRF-Audio-Mixer.exe` is a deliberately separate trial launcher. Its rendering defaults are explicit test defaults; upstream launchers should keep their own settings and only add the mixer environment variables. It does not implement cloud-save synchronisation.

The engine integration uses:

| Variable | Purpose |
| --- | --- |
| `JSRF_MIXER_FILE` | Shared fader/mute INI |
| `JSRF_MIXER_STATUS_FILE` | Diagnostic JSON read by the companion |
| `JSRF_MIXER_APP` | Absolute UTF-8 path to the opener used by F6/the Audio menu |

Without a mixer config, all engine gains default to 100%. The backend has no legacy duck filter. The companion migrates old `music_duck=1` preferences once into the actual Music percentage, then saves only faders/mutes. Current trial preferences have already been migrated.

`build_icon.py` optionally rebuilds the original SVG/PNG/ICO assets using Pillow. The checked-in ICO already contains 16, 20, 24, 32, 48, 64, 128 and 256 pixel images. No image-generation service is needed to build the companion.

## Implementation notes

Guest-side hooks classify ADXT filenames and follow CRI output -> DirectSound buffer -> hardware voice IDs. The APU applies a short gain ramp before source summing, preserves original envelopes/spatialisation, bypasses multipass reapplication, and applies Master only to final PCM. Audio callbacks use fixed-size arrays and C11 atomics, with no file I/O or locks. Software playback uses the Effects gain.

The guest thread polls preferences every 100 ms and publishes diagnostics every 250 ms. WinForms queues writes and retries after sharing conflicts. Existing configs are replaced with `File.Replace`; readers opt into shared delete. A pending journal preserves the latest requested state if a reader temporarily blocks saving. An I/O failure never escapes a fader event as a JIT error dialog.

Diagnostic reads validate all seven groups before updating the panel. Empty, truncated, null or malformed JSON during an in-place telemetry write is ignored until the next complete snapshot. This fixes the observed `NullReferenceException` in `TickMixer` without suppressing unrelated programmer errors. Repeated opens use a per-game panel mutex/event; a separate launcher mutex serialises engine startup.

Windows chrome uses supported owner-drawn menus, a window-procedure subclass, system preference notifications and dynamically loaded DWM caption attributes. F6 is handled once in the native procedure. The opener transfers foreground permission to the existing companion before signalling it. Other platform menus are unchanged; the companion is Windows-only.

## Verification and remaining acceptance

`test.ps1 -EvidenceDirectory <folder>` runs save-contention and UI interaction regressions. The backend C test under `tests/` checks classification, independent gains/mutes, a single Music level, Master once, multipass bypass and full restoration. Compile it together with `src/apu/jsrf_mixer.c` using the engine's C11 toolchain.

On the ASUS laptop, the v0.6.0-based engine builds and boots, the new menu opens the companion, F6 raises the existing hidden panel without restarting the game, and Ctrl+Alt+M changes Music from 100 to 25 and back while other percentages stay fixed. Native diagnostics show the matching 0.25 Music gain. Light and dark visuals were inspected through process-only previews; Windows settings were not changed. Source grouping and save-conflict regressions pass.

The F6 follow-up also verifies hiding the panel returns keyboard focus to the running full-screen game, reopening keeps the same panel/game processes, twelve concurrent opener requests leave one panel/game, and six launcher requests exit behind the startup guard. The telemetry regression reproduces empty JSON deserialising to null, rejects every incomplete prefix and validates recovery during 200 concurrent in-place writes.

Human acceptance is still required for extended in-game use, the new interaction feel, full-screen F6, and listening to each distinct dialogue/DJ/ambience source in suitable scenes. Earlier music-only listening acceptance does not automatically validate every new group or the updated engine.

Published for maintainer feedback as a draft contribution. This is a Windows prototype; extended listening and platform validation remain open. Developed with AI assistance and tested locally.
