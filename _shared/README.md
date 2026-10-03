# DCO-SHARED-LIBRARIES

Shared C++ headers — plus one Reaper JSFX bench tool — used by more than one DCO
project. Not an Arduino library: there is no `library.properties` and the folder
is deliberately kept off the `arduino-cli --libraries` path, so nothing here is
auto-scanned or auto-included. Sketches pull in exactly the headers they name.

Consumers:

- [DCO4-REBORN](https://github.com/felipegaspari/DCO4-REBORN)
- [DCO3-MONOSYNTH](https://github.com/felipegaspari/DCO3-MONOSYNTH)

Compiled Arduino libraries that happen to be shared (ADSR_Bezier, mo-lfo,
DCO_Noise, DCO-PROTOCOL) each stay in their own repo and are symlinked into
`DCO/_build_libs/`. This repo is for the rest: code that is part of the sketch
rather than a library it links against.

What a sketch has to define before including any of this — the include order,
`PROJECT_INSTRUMENT`, and every symbol these headers expect from the board — is
one page: [`docs/SKETCH_CONTRACT.md`](docs/SKETCH_CONTRACT.md).

## Contents

### Headers

| Header | What it holds |
|---|---|
| `amp_comp.h` | Amp-comp calibration tables and quadratic window eval |
| `autotune.h` | Calibration state, enums, prototypes; pulls in the three below |
| `autotune_constants.h` | Calibration constants and the NORMAL / FINE / FAST precision profiles |
| `autotune_context.h` | `DCOCalibrationContext` |
| `autotune_measurement.h` | `find_gap()` declaration and the `measure_gap()` wrapper |
| `autotune_impl.h` | Definitions: run orchestration, PW searches, `find_gap()`. Include once |
| `autotune_search_impl.h` | Definitions: amp-comp search, FREQ_TRACE, endpoints. Include once |
| `bench.h` | Hot-path profiler; DCO3-only probes/report via `PROJECT_INSTRUMENT` |
| `character_jitter.h` | Character-tab noise jitter scales (pitch / amp / PW) |
| `clkdiv.h` | PIO clock-divider total-cycle helpers (`CLKDIV_MODE`) |
| `cv_bezier.h` | Bézier helpers for the AS2164 VCA linearize table |
| `cv_out.h` | Software CV math prototypes |
| `FS.h` | LittleFS calibration bank sizes, buffers, file handles and the FS API prototypes |
| `FS_impl.h` | Definitions: `init_FS()` loader, `update_FS_*` writers, fake-cal seeding. Include once, from `DCO/FS.ino` |
| `mcp4728.h` | MCP4728 I2C addresses, Fast Write, probe / reattach API (`ENABLE_MCP4728`) |
| `mcp4728_impl.h` | Definitions: attach, probe, reattach, async fast-write. Include once from the board shim |
| `mcu_board.h` | Pico/Pico 2 SMPS PS + WeAct KEY/board-fix; **not included** by the sketch |
| `mem_diag.h` | SRAM / heap / stack snapshot prototypes |
| `midi.h` | USB and serial MIDI instances |
| `midi_cc.h` | 7-bit MIDI CC control surface |
| `midi_cc_map.h` | Generated `MidiCcEntry` table (from `gen_midi_map.py`) |
| `noise.h` | Sketch-side `DcoNoiseGen` objects |
| `noteList.h` | MIDI note Hz / Q24 tables (`__not_in_flash("pitch_tables")`) |
| `PWM.h` | RANGE / CV / level PWM writers |
| `range_pwm_dither.pio.h` | Generated PIO program for RANGE dither |
| `tusb_config.h` | TinyUSB config (sketch keeps a same-name shim) |
| `utils.h` | Log / exp mapping prototypes |
| `voice_alloc.h` | `VoiceAllocator` (poly voice stealing) and `MonoNoteStack` (mono note priority) |
| `voices.h` | Voice init and portamento state |
| `wave_mux.h` | 74HC595 wave-select prototypes |

### Docs

| Page | What it covers |
|---|---|
| [`docs/SKETCH_CONTRACT.md`](docs/SKETCH_CONTRACT.md) | Include order, `PROJECT_INSTRUMENT`, the `.ino`-shim rule, every symbol the board must provide |
| [`docs/AUTOTUNE.md`](docs/AUTOTUNE.md) | Autotune algorithms, file layout, DCO3/DCO4 hardware table |
| [`docs/CALIBRATION_PROCEDURE.md`](docs/CALIBRATION_PROCEDURE.md) | Operator calibration bring-up |
| [`docs/FILESYSTEM.md`](docs/FILESYSTEM.md) | The LittleFS partition: inventory, space budget, who owns what |
| [`docs/CALIBRATION_STORAGE.md`](docs/CALIBRATION_STORAGE.md) | On-flash bank format, per-board sizing, FS invariants |
| [`docs/PRESET_STORAGE.md`](docs/PRESET_STORAGE.md) | The 256-slot preset store and the host dump / bulk-restore protocol |
| [`docs/VOICE_ALLOC.md`](docs/VOICE_ALLOC.md) | Voice stealing and mono note priority: modes, API, build flags |
| [`docs/MCP4728.md`](docs/MCP4728.md) | MCP4728 DAC: the board hooks the shim must define, probe / reattach flow |

### Bench tools

| File | What it is |
|---|---|
| `duty_cycle_meter.jsfx` | Reaper JSFX: duty-cycle / frequency readout and scope for a pulse fed into an audio interface. Stands in for a bench scope during the manual trim stage — see [`docs/CALIBRATION_PROCEDURE.md`](docs/CALIBRATION_PROCEDURE.md) |

## How a sketch consumes it

The repo is a submodule at the root of each superproject, and each sketch that
needs it carries a symlink **inside the sketch folder**:

```
DCO/_shared -> ../DCO-SHARED-LIBRARIES
```

One `../`, because `_shared` sits directly in `DCO/`. The Arduino library
symlinks beside it need two, because they sit one level deeper:

```
DCO/_build_libs/DCO-PROTOCOL -> ../../DCO-PROTOCOL
```

Then include through the symlink:

```cpp
#include "_shared/voice_alloc.h"
```

Header-only, so there is nothing to add to the build command. Where each header
goes in `include_all.h`, and what it expects to already be defined, is
[`docs/SKETCH_CONTRACT.md`](docs/SKETCH_CONTRACT.md).

## Modules

**autotune** — the whole calibration subsystem: the amp-comp table build
(classic per-note search and FREQ_TRACE curve tracing), the refine pass over a
stored table, the PW center and limit searches, and the edge-timing duty
measurement they all run on. Six headers, split into declarations and
definitions; three of them are pulled in transitively by `autotune.h`, so a
sketch names only `autotune.h`, `autotune_impl.h` and `autotune_search_impl.h`.
Algorithms: [`docs/AUTOTUNE.md`](docs/AUTOTUNE.md). Operator workflow:
[`docs/CALIBRATION_PROCEDURE.md`](docs/CALIBRATION_PROCEDURE.md).

**FS** — where the numbers autotune produces are kept: seven flat LittleFS banks
holding the amp-comp tables, PW center and limits, and the manual trims. The
on-flash byte format is fixed and mirrored by the host tool, so the rules for
changing any of it matter: [`docs/CALIBRATION_STORAGE.md`](docs/CALIBRATION_STORAGE.md).
For the partition those banks live on and how much of it is left, see
[`docs/FILESYSTEM.md`](docs/FILESYSTEM.md).

The 256-slot **preset store** is code each sketch keeps (`preset_store.*`), but it
is a shared contract — same record, same wire protocol on both boards, and it calls
the FS layer's `write_fs_bank()` for calibration restores. That contract is
[`docs/PRESET_STORAGE.md`](docs/PRESET_STORAGE.md); per-board differences are in each
board's `DCO/docs/PRESET_STORE.md`.

**voice_alloc** — voice allocation for a polyphonic or paraphonic board, and the
mono held-key stack. One policy value (`PARAM_VOICE_ALLOC_MODE`) drives both
halves of the same decision: in poly which voice gets stolen, in mono which held
key sounds. Modes, API and build flags: [`docs/VOICE_ALLOC.md`](docs/VOICE_ALLOC.md).

**mcp4728** — the MCP4728 I2C DAC driver, behind `ENABLE_MCP4728`. The
implementation is bus-agnostic: the board shim supplies five hooks and the
driver does the rest. [`docs/MCP4728.md`](docs/MCP4728.md).

## Adding a shared header

1. Add the header here, and a row in the Contents table above.
2. If it needs board-specific values, take them as `NUM_*` / `ENABLE_*` macros or
   as `extern` arrays the sketch defines — never hardcode a board's topology.
   Add them to the symbol table in [`docs/SKETCH_CONTRACT.md`](docs/SKETCH_CONTRACT.md).
3. Declarations only, unless the header is an `*_impl.h`. Definitions in a header
   that more than one translation unit includes will not link.
4. For an `*_impl.h`, add the one-line `.ino` shim in both sketches and check the
   alphabetical position — that ordering is what the merged translation unit
   depends on. Every function called from outside it needs a prototype written by
   hand in the matching declarations header. Both rules are explained in
   [`docs/SKETCH_CONTRACT.md`](docs/SKETCH_CONTRACT.md).
5. Compile both sketches, then commit here and update both superprojects (below).

## Keeping the two checkouts in sync

Each superproject has its **own checkout** of this submodule
(`DCO3-MONOSYNTH/DCO-SHARED-LIBRARIES` and `DCO4-REBORN/DCO-SHARED-LIBRARIES`),
so an edit made in one is invisible to the other until it goes through the
remote. They are meant to be byte-identical; when they are not, the two boards
are compiling different code from a file that is supposed to be shared.

```bash
# in the checkout you edited
git -C DCO3-MONOSYNTH/DCO-SHARED-LIBRARIES commit -am "…" && git -C DCO3-MONOSYNTH/DCO-SHARED-LIBRARIES push

# in the other superproject
git -C DCO4-REBORN submodule update --remote DCO-SHARED-LIBRARIES

# then record the new pointer in each superproject
git -C DCO4-REBORN add DCO-SHARED-LIBRARIES && git -C DCO4-REBORN commit -m "Bump DCO-SHARED-LIBRARIES"
```

To check they agree:

```bash
diff -rq --exclude=.git DCO3-MONOSYNTH/DCO-SHARED-LIBRARIES DCO4-REBORN/DCO-SHARED-LIBRARIES
```

A file that legitimately differs per board does not belong here — that is what
`globals.h` and the `NUM_*` / `ENABLE_*` macros are for.
