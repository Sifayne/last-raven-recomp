# Higher FPS

Normal builds of Last Raven Portable, Armored Core 3 Portable and Silent Line
Portable support optional mission rendering above 30 FPS. In the launcher,
open **Graphics**, enable **Higher FPS**, choose **FPS cap**, and save the
preset. The arrows cycle common rates; Enter accepts a custom whole number
from 30 to 1000, or `unlimited`, which follows the display's refresh rate with
vertical sync on the OpenGL renderer. Changes apply on the next launch.

Use `OPT=1 scripts/15-settings.sh` to launch with optimized game builds.

`HIGH_FPS=1` / `PSPRECOMP_HIGH_FPS=1` enables mission interpolation.
`FPS_CAP=60` / `PSPRECOMP_FPS_CAP=60` limits rendered frames; whole numbers
30–1000 and `unlimited` are accepted. The launcher saves both in presets.
Existing presets and direct runs default to Higher FPS off and a 60 FPS cap.
The cap applies to enhanced mission rendering; other menu, movie and results
loops retain their original pacing. It is a ceiling, not a guarantee that the
host can render at that rate.

## Implementation

The shared host clock keeps simulation at 30 Hz. The mission adapter polls
input only on simulation ticks, including catch-up ticks, and rebuilds extra
render frames without consuming controller or mouse samples. A stall is
bounded to four catch-up ticks. Normal menus, results and movie loops keep
their original timing. Headless, non-real-time enhanced runs use a synthetic
display clock at the requested cap (60 for unlimited), for reproducible tests.

`scripts/fps-loop.py` extracts the mission loop from the user's generated
module and inserts hooks at checked instruction boundaries. It emits into
the generated directory: the distributable package contains no generated
game instructions. Original functions remain available and Higher FPS off
defers directly to them.

Camera and AC skeleton interpolation temporarily supply intermediate render
poses. The original tick's exact matrices are restored before simulation
continues. Rebuilding an AC also updates combat effects in these games, so the
generated render rebuild excludes that gameplay tail. It restores cached
authoritative bone matrices instead of resampling a later animation pose.
Extra guest calls preserve the CPU register file. History resets on mission
entry/exit, pause, discontinuities and model replacement.

Layer updates stay on simulation ticks. ACLR additionally separates its HUD
widget lifecycle from HUD drawing: advancing that lifecycle on an inserted
frame could clear a lock-on flag and change firing behavior. The HUD still
draws every frame, and its lifecycle also runs on catch-up ticks.

The render system's frame begin (`0008D8BC`: viewport and projection, view
setup and, through them, the display-list kick) must run on every rendered
frame. Until 17 September the ACLR generator suppressed it with the tick-only
helpers, so an inserted frame executed 1,737 GE commands instead of 22,000:
no world, only the HUD and the full-screen passes drawn over the previous
frame's colour, which dimmed a little more on each inserted frame -- at a cap
of 90 a 30 Hz flicker between a normal frame and two dimmer ones, and menus
and the mission intro reduced to their sky gradient. `PSPRECOMP_GL_FRAME_LOG=1`
prints each present's GE command and draw counts, which is how it was found.
With it restored, every frame draws the full list and the stock-versus-
enhanced comparison still matches all 2,713 control records at caps 30 and 90.

| Adapter | Mission loop | Bone builder | Camera update |
| --- | --- | --- | --- |
| ACLR | `00102018` | `00045DAC` | `000FF280` |
| AC3P | `000E0F10` | `00104EEC` | `000E07F0` |
| ACSL | `000914B0` | `000023C4` | `0009039C` |

These addresses apply to the repository's supported executable fingerprints.
AC3P and ACSL share the adapter with their own addresses and layouts. Package
fingerprints include the generation recipe so a change triggers local rebuilding.

## Validation

Checked 10 September 2026 with optimized builds of all three titles:

- Clock tests cover 30, 60, 90, 120, 144, 165, 240 and 1000 Hz, bounded catch-up,
  long stalls and a backwards timestamp. Pose tests cover both bone layouts,
  intermediate transforms, exact restoration, invalid history and cuts.
- Settings and launcher interaction tests cover defaults, existing presets,
  persistence, overrides, common rates, custom values and unlimited.
- ACLR firing: all 2,713 control records match stock at 30, 60, 144 and unlimited.
  At 144 FPS, walking (3,685 records), pause/resume (2,873) and the mouse step
  (2,495) also match stock exactly.
- AC3P's 9,100-poll movement/combat replay matches all 11,991 original records
  at 144 FPS. ACSL's 14,000-poll mission and return to menus matches all 41,665
  records at 144 FPS.
- A 100 ms clock gap after enhanced frame 1,500 preserves those same control
  logs in all three titles. ACLR catches up four ticks across a firing input
  edge; the other titles catch up three. All runs report zero bad guest accesses.
- An inserted ACLR frame was captured and replayed through the software
  renderer: the mech, scene and complete HUD remain visible. Desktop OpenGL
  mission smoke checks show intact views in all three games. The AC3P
  windowed check reached poll 7,292 before its time budget; its full
  9,100-poll validation above used the null renderer.
- ACLR's final 1280×720 OpenGL firing replay completes with all 2,713 control
  records matching stock and zero bad guest accesses. Over a 30.15-second
  mission interval, presentation measures 60.00 FPS and simulation 29.98 Hz
  (approximately 30 Hz, with integer tick endpoints).

Run the unit and launcher checks with:

```bash
scripts/18-fps-tests.sh --ui
```

Compare stock and enhanced gameplay using locally owned assets:

```bash
python3 scripts/test_fps_replay.py \
  --boot build/host-opt/boot --elf game/extracted/ACLR_App.elf \
  --iso 'game/Armored Core - Last Raven Portable.iso' \
  --scenario scenarios/mission-effects.pad --caps 30 60 144 unlimited \
  --output reports/fps/combat
```

Use the corresponding title's boot, ELF, ISO and scenario for AC3P/ACSL.
`--stop-at 14000` makes the open-ended ACSL recording finite. `--gap-after 1500`
injects a deterministic 100 ms stall. Optional diagnostics:

- `PSPRECOMP_FPS_LOG=path`: frame/tick counts, phase, positions and boundaries.
- `PSPRECOMP_FPS_INTERPOLATE=0|camera|joints`: isolate interpolation paths.
- `PSPRECOMP_FPS_TEST_GAP=1500:100`: synthetic clock gap after a frame, in ms;
  ignored in real-time/windowed play.

Validation covers these recorded missions, rather than every mission or arena.
Projectiles, HUD animations and other objects without mapped interpolation
retain their original animation cadence. The feature remains opt-in.
