Armored Core Portable — Linux x86-64 test build

Make the AppImage executable, then open it. On Steam Deck, do the first setup
in Desktop Mode. Select Add Game, browse to your PSP ISO, and choose Open.
Drives opens removable media locations. You can also drag an ISO onto the app.
Keep the app open while preparation runs; then choose Save & Play.

Higher FPS is available for missions in all three games. In Graphics, enable
Higher FPS and set FPS cap to 60 to start. The arrows cycle common rates;
Enter accepts a custom rate from 30 to 1000 or unlimited. Save the preset.
Simulation stays at 30 Hz. Menus, movies and some effects keep their original
cadence. Higher FPS is off by default; achievable rates depend on the scene
and graphics settings. See usr/share/last-raven/FPS.md for details.

Updating from the earlier Deck test build: choose Prepare Game once for each
title to compile the FPS changes. Existing ISO locations, saves and presets
are retained. No need to add the same ISO again unless it has moved.

Quit the game: hold View + Menu (Select + Start) together for 2 seconds.
Releasing either button cancels the hold. Keyboard: Ctrl+Shift+Q.
This closes the game normally; save your progress in-game before quitting.

Supported executable versions (US PSN):
  Armored Core 3 Portable: NPUH10023 v1.01
  Armored Core: Silent Line: NPUH10025 v1.00
  Armored Core: Last Raven: NPUH10024 v1.00
Other regions, revisions, modified executables and compressed images are not
supported by this build. An ISO with a matching ID but different executable
is rejected. No game code or assets are included with the download.

The app includes its preparation tools, C compiler, Python runtime and media
libraries. No compiler, Python or FFmpeg installation is needed. First setup
is CPU intensive and needs at least 2 GB of available library space. Leave the
ISO in an accessible location; the game reads its assets from that file.

Launcher-only updates keep prepared games ready. Changes to game fixes rebuild
only the affected titles; runtime/compiler changes can require rebuilding all
games. Existing ISO locations, settings and per-game saves are preserved.
Moving from an older preview requires preparing each game once to record its
game fingerprint. If an ISO moves, use Add Game to
select it again; a verified prepared game can be reused. Cancellation or a
failed import preserves existing installations and saves.

Default persistent locations (XDG overrides are supported):
  Settings: ~/.config/last-raven/settings.ini
  Games:    ~/.local/share/last-raven/games
  Saves:    ~/.local/share/last-raven/saves/<game>/ms/PSP/SAVEDATA
  Logs:     ~/.local/state/last-raven/logs

For Gaming Mode, add the AppImage as a non-Steam game after Desktop Mode setup.
Use the native Linux executable; no Proton compatibility override is needed.
The launcher and game accept Steam Input's virtual gamepad automatically.

Command-line options: --help, --print-paths, --import /path/to/game.iso,
--list-games, --check-startup, --print-settings, --list-presets.

This software uses FFmpeg under the LGPL v2.1 or later: https://ffmpeg.org/
Exact source and rebuild instructions accompany the AppImage in the matching
sources archive. Libraries remain separate; compatible replacement libraries
can be supplied using LD_LIBRARY_PATH or in an extracted AppDir. See licenses/.
pspdecrypt is a separate GPLv3 helper; its corresponding source is included.
The icon is the unchanged GNOME Adwaita application icon, CC BY-SA 3.0 US.

This is a test build. A successful launcher startup does not by itself verify
gameplay, audio or save behavior on a particular device.
