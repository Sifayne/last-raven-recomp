# Scenarios — scripted pad input

A scenario is a button sequence the runtime can replay. It exists so that "I
skipped the intro, picked New Game, and it faded to black and died" becomes a
command anyone can run, instead of a thing that happened once to one person
holding a gamepad.

```bash
scripts/09-replay.sh scenarios/new-game.pad
```

No game data lives here. These are button presses and timestamps.

## Why the timebase is the pad poll

Events are keyed on **`@N`, the count of calls the game has made to
`sceCtrlPeekBufferPositive`/`sceCtrlReadBufferPositive`** — one per turn of the
game's own loop, counted by the runtime at the single point where the guest
reads the pad.

That is the only quantity that means the same thing in every configuration.
Wall seconds do not: `PSPRECOMP_WINDOW=1` switches the guest clock to real time
(`host/present.c` calls `psp_clock_realtime`), so `3.5s` in a window and `3.5s`
headless are different amounts of game. A poll count is a count of things the
game did either way.

Seconds are still available, and are much easier to write by hand. Use them to
draft, then record the run and keep the `@N` version.

## Format

Line-based. `#` starts a comment. Blank lines are ignored.

```
[<when>] <directive> [arguments]
```

### `<when>`

| form | meaning |
|---|---|
| `@N` | at poll `N` |
| `+N` | `N` polls after the previous event fired |
| `T.Ts` | at `T.T` guest seconds |
| `+T.Ts` | `T.T` guest seconds after the previous event fired |
| *omitted* | `+1` — the next poll |

### Directives

| directive | effect |
|---|---|
| `down <btn>[,<btn>…]` | press and keep held |
| `up <btn>[,…]` | release |
| `tap <btn>[,…] [polls]` | `down`, hold, `up`; holds `minhold` polls by default |
| `state <btns> <x> <y>` | assign the whole lane at once — what the recorder writes |
| `analog <x> <y>` / `analog center` | 0–255 each; takes the stick from live input |
| `neutral` | release everything and hand the stick back |
| `wait <n>` / `wait <n>s` | move the cursor, change nothing |
| `mark "<text>"` | narrate to stderr, with the poll and guest time |
| `stop` | end the run here |

Headers, which must come before the first timed line:

| header | effect |
|---|---|
| `drain <seconds>` | the run length this scenario wants |
| `minhold <polls>` | how long a bare `tap` holds (default 2) |

Button names: `select start up right down left ltrigger rtrigger l r triangle
circle cross square`, case-insensitive, plus `0x<hex>` for a bit that table
does not name. `none` and `-` are the empty set.

## Three rules that make a replay deterministic

**1. File order is the contract; timestamps are floors.** Events fire in the
order written, each no earlier than its stamp. Because ordering never depends
on comparing one timebase against another, mixing `@N` and `T.Ts` in one file
is unambiguous.

**2. At most one visible edge per poll.** A `tap` is a `down` and an `up`. When
the game stops polling for a while — which it does for the whole intro movie —
both come due at the same poll, and applying both would hand the game an
unchanged pad. It would never observe the button down, and *a press the guest
cannot observe is a press that did not happen*. So the second edge waits for
the next poll.

The cost: a scenario's absolute timeline stretches wherever the game polls
slowly. That is the right way round. The alternative is a timeline honoured
exactly that delivers nothing.

**3. `stop` ends the run at a defined poll.** Without it the run ends at the
drain, which is a *wall-clock* deadline — so how much of the game fits inside
it varies between runs on the same build. Two replays are only comparable if
they end at the same place, and `stop` is what makes that true.

## The scenarios

| file | reaches | polls |
|---|---|---|
| `empty.pad` | nothing; the base for recording | — |
| `title-idle.pad` | the title menu, pressing nothing — the control | 1,800 |
| `skip-intro.pad` | the title menu via circle-skip | 2,500 |
| `hanger.pad` | the option menu over the 3D hangar | 430 |
| `new-game.pad` | the sound-settings panel | 6,000 |
| `mission-1.pad` | name entry, garage, mission select, into a sortie | 1,790 |
| `mission-effects.pad` | same route, then combat firing and boost input | 2,210 |
| `main-menu.pad` | the main menu with the AC behind it — M2's first frame | 748 |
| `garage.pad` | the sortie launch, the AC alone in the hangar — M2's second frame | 925 |
| `option.pad` | the option box, by taps rather than a recording | 3,900 |

`hanger.pad` and `new-game.pad` reach the same scene; the short one exists
because a run that takes fourteen seconds gets measured twenty times and one
that takes minutes gets measured twice. Pair either with `title-idle.pad`.

`render-checks.json` selects six poll-stamped frames from `mission-effects.pad`
for fixed-input software/GL regression checks. See
[the renderer-check guide](../docs/RENDER-CHECKS.md) for capture/replay commands
and the distinction between pixel parity and shared unsupported features.

## Authoring: play it, then keep the recording

Hand-writing exact poll numbers is guesswork. Record instead:

```bash
scripts/09-replay.sh --window --record scenarios/my-run.pad scenarios/empty.pad
```

Play with the keyboard or a gamepad. The recorder writes a line every time the
pad the *game saw* changed, stamped `@N` with the seconds in a comment. What
comes out is directly replayable — the recorder and the player share one
counter at one point in the code, so a recording cannot describe a session the
player is unable to reproduce.

You can also record *on top of* a replay: run an existing scenario with
`--record`, take over with the gamepad when it gets stuck, and the recording
carries the whole thing. A run with live input in it is flagged `[TAINTED]` in
the summary, because it is no longer reproducible from the scenario alone —
but its recording is.

## What lives here

| file | what |
|---|---|
| `empty.pad` | nothing but a `drain`. For recording a fresh session. |
| `skip-intro.pad` | boot to the title menu and stop. The fastest check that the harness works. |
| `title-idle.pad` | reach the menu, press nothing, wait, stop. **The control.** |
| `new-game.pad` | the crash: skip the intro, pick New Game. |

`title-idle.pad` is not optional. Without a run that reaches the same place and
does nothing, "New Game causes the fault" and "anything past forty seconds
causes the fault" produce identical evidence.
