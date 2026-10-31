# Draft: source-aware Windows audio mixer

JSRF Unleashed currently exposes one overall audio path to the operating-system mixer. This prototype makes it possible to lower the soundtrack during gameplay while keeping effects and separate dialogue at their own levels.

The patch adds seven faders, an Audio menu entry beside Video/Controls, F6 to open/hide the companion and return to the game, direct-position mouse interaction, and system light/dark window chrome. Repeated opens reuse the same panel, and a startup mutex prevents duplicate engine launches. Ctrl+Alt+M changes the Music fader itself. It also handles competing config readers without throwing WinForms save exceptions and supplies a dedicated vector taskbar/tray icon.

The engine changes retain upstream main's v0.6.0 rendering/audio fixes. Source gains are applied before summing; Master is applied once at the final output. ADX filenames provide classification and live CRI/DirectSound objects supply hardware voice routing. Existing game fades and positional volumes are preserved. Embedded vocals cannot be separated from their recording.

The companion is a Windows WinForms prototype, not a cross-platform mixer UI. The launcher and packaging are kept separate for local testing; an upstream integration should adapt them to the project's own launchers and configuration conventions. USA guest object layout/routing assumptions and cross-platform build behaviour need maintainer review. No game executable, generated game code, game assets, saves or user-specific paths are part of the source patch.

Validation available: native Windows engine build/boot; Audio menu and F6 attach/reopen; visible Music shortcut and matching backend gain; light/dark visual checks; independent-bus/mute/ramp tests; shared-reader contention, 500 rapid updates and pending-journal recovery; fader accessibility/keyboard/drag tests.

Status: draft contribution for maintainer feedback. Extended listening and human interaction acceptance remain pending; native full-screen F6 checks pass. Developed with AI assistance and tested locally.
