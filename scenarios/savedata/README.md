# Savedata game regression

`aclr.pad` enters Last Raven's System menu, saves allowed slots 01 and 02,
then loads slot 01. `aclr.dialog` supplies the utility decisions separately
from guest pad input. These fixtures require an empty disposable card; they
must not run against a player's memory stick. The same initial campaign state
is saved twice, so this is not a distinct-progress acceptance test.

Build the optimized ACLR host first (`OPT=1 BOOT_NO_RUN=1 scripts/06-boot.sh`,
after the usual emitted-code build). From the repository root:

```bash
repo="$PWD"
card="$(mktemp -d /tmp/aclr-savedata-XXXXXX)"
cd "$card"
PSPRECOMP_REPLAY="$repo/scenarios/savedata/aclr.pad" \
PSPRECOMP_SAVEDATA_SCRIPT="$repo/scenarios/savedata/aclr.dialog" \
PSPRECOMP_SAVEDATA_LOG=1 PSPRECOMP_MPEG_DECODE=1 PSPRECOMP_DRAIN=120 \
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy PSPRECOMP_RENDER=gl \
    "$repo/build/host-opt/boot" "$repo/game/extracted/ACLR_App.elf" \
    "$repo/game/Armored Core - Last Raven Portable.iso" > replay.log 2>&1
```

Expected: modes 5, 5, 4 select names `ACLRSAVELIST01`, `ACLRSAVELIST02`,
`ACLRSAVELIST01`; each result is `00000000`, the replay stops at poll 1600,
and bad guest accesses are zero. Both directories under
`$card/ms/PSP/SAVEDATA/` contain 28,316-byte `SAVEDATA.BIN` files, metadata,
and game-supplied icons. The response script refuses unexpected identities.
For software/headless execution, omit `PSPRECOMP_RENDER=gl`; the same explicit
utility responses work without a window.

Real controller, relaunch with distinct progress, cancellation/overwrite in
each game's menu, and the AC3/Silent Line routes remain player acceptance
checks in `docs/SAVEDATA-UI.md`. The ROM-independent HLE and host fixtures in
`scripts/test_savedata.sh` cover the shared decision and I/O behavior.
