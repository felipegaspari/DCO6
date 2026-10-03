# What a sketch has to provide

These headers are sketch fragments, not a library: they are compiled as part of
the sketch's single translation unit and they reach *back* into it for the
board's topology. This page is the contract — include order, the macros that
select a board, the `.ino`-shim rule, and every symbol the board must define.

Nothing here is board-specific by itself. Everything that differs between DCO3
and DCO4 is a count (`NUM_*`), a compile-time gate (`ENABLE_*`), or an array the
sketch owns.

## Board selection: `PROJECT_INSTRUMENT`

`project_config.h` is a symlink at the root of each sketch folder
(`DCO/project_config.h -> ../project_config.h`), so both boards see the same file
name and a different value:

| Value | Board |
|---|---|
| `3` | DCO3-MONOSYNTH |
| `4` | DCO4-REBORN |

Only two places in this repo read it, and both must keep their gate:

- `FS_impl.h` — `#if PROJECT_INSTRUMENT == 4` guards `ensure_pw_fs_banks()`, the
  DCO4 PW bank repair. It must never run on DCO3, whose 6-byte PW bank the
  migration would treat as stale and overwrite. See
  [`CALIBRATION_STORAGE.md`](CALIBRATION_STORAGE.md).
- `bench.h` — `#if PROJECT_INSTRUMENT == 3` guards the DCO3-only profiler probes.

Prefer a `NUM_*` or `ENABLE_*` macro over a new `PROJECT_INSTRUMENT` test. An
instrument check is right only when the difference is genuinely per-board and not
expressible as a count.

## Include order

`include_all.h` is the sketch's umbrella header and the one place that fixes the
order. The parts that matter:

```cpp
#include "globals.h"        // all NUM_* / ENABLE_* / pin maps / PW arrays

#include "FS.h"             // sizes chanLevelVoiceDataSize
#include "preset_store.h"
#include "noteList.h"
#include "amp_comp.h"       // sizes its arrays with chanLevelVoiceDataSize
// …
#include "autotune.h"       // last: needs amp_comp, PWM, noteList
```

Three ordering rules are load-bearing:

1. **`NUM_PW_CHANNELS` before `FS.h`.** `FS.h` sizes the PW banks with it.
   `include_all.h` reaches `FS.h` well before `autotune.h`, so the
   `#ifndef NUM_PW_CHANNELS` fallback inside `autotune.h` is far too late — it
   would size the banks from the default and then silently disagree with the rest
   of the build. DCO3 repeats the guarded define in `globals.h` for this reason;
   the fallback in `autotune.h` then no-ops.
2. **`FS.h` before `amp_comp.h`.** The dependency runs both ways: `amp_comp.h`
   sizes its arrays with `chanLevelVoiceDataSize` from `FS.h`, and `FS_impl.h`
   fills arrays that `amp_comp.h` declares.
3. **`FS.h` does not include `include_all.h`.** It is included *from* it. The
   `*_impl.h` files are the opposite: they start with `#include "../include_all.h"`,
   which resolves to the sketch's own copy when reached through `DCO/_shared/`.

## The `*_impl.h` and `.ino` shim rule

Three modules here split declarations from definitions, and the definitions are
reached through a one-line `.ino` shim in the sketch rather than a normal include:

| Definitions | Shim | Declarations |
|---|---|---|
| `FS_impl.h` | `DCO/FS.ino` | `FS.h` |
| `autotune_impl.h` | `DCO/autotune.ino` | `autotune.h` |
| `autotune_search_impl.h` | `DCO/autotune_search.ino` | `autotune.h` |
| `mcp4728_impl.h` | board `MCP4728.ino` | `mcp4728.h` |

```cpp
// DCO/FS.h
#include "_shared/FS.h"
// DCO/FS.ino
#include "_shared/FS_impl.h"
```

**Why shims and not includes.** arduino-cli merges the sketch's `.ino` files
alphabetically into one translation unit. Keeping these shim names is what
preserves that order, and with it the visibility of each file's `static` helpers
to the one after it: `FS.ino` sorts first, so the FS definitions land ahead of
the autotune impls that call them, and `autotune_search.ino` sorts after
`autotune.ino`.

**The consequence.** Arduino's prototype generator only scans `.ino` files, and a
one-line shim gives it nothing. So:

- Every function callable from outside an `*_impl.h` needs a prototype written
  **by hand** in the matching declarations header.
- File-scope statics used before their definition are forward-declared at the top
  of each `*_impl.h`.

A function added to an `*_impl.h` and called from anywhere earlier will not
compile until you add that declaration yourself.

## Required symbols

### From `globals.h`

| Symbol | Used by |
|---|---|
| `NUM_OSCILLATORS`, `NUM_VOICES_TOTAL` | everything count-driven |
| `NUM_PW_CHANNELS` | `FS.h` banks, `PWM.h`, PW calibration (**define before `FS.h`**) |
| `DIV_COUNTER`, `DIV_COUNTER_PW` | amp-comp and PW ranges. `DIV_COUNTER` aliases `RANGE_PWM_WRAP` in `project_config.h` |
| `DCO_calibration_pin` | the duty sense input (DCO3 GP6, DCO4 GP10) |
| `RANGE_*`, `PW_*` pin maps | `PWM.h`, calibration |
| `VOICE_TO_PIO` / `VOICE_TO_SM` | driving one oscillator during calibration |
| `kPwCenterDefault[NUM_PW_CHANNELS]` | fake seed, DCO4 bank repair |
| `PW_CENTER[]`, `PW_LOW_LIMIT[]`, `PW_HIGH_LIMIT[]` | runtime PW state the FS loader fills |
| `ENABLE_FS_CALIBRATION` | gates everything after the `voiceTables` open |

### From the sketch's own code

| Symbol | Contract |
|---|---|
| `voice_task_autotune()` | Drive one oscillator; mode 4 reads `calibrationFreqHz` |
| `start_voice_sms()` | Restart the PIO state machines after a calibration pass |
| `serialSendParam32()` + `PARAM_GAP_FROM_DCO` | Report the live duty error upstream |

### From other headers in this repo

`amp_comp.h` (`freq_to_amp_comp_array`, `ampCompArray`, `ampCompFrequencyHz` /
`ampCompFrequencyArray`, `ampCompTableSize`, `precompute_amp_comp_for_engine()`),
`autotune.h` (`calibrationData[]`, `manualCalibrationOffset[]`, `ampComp440[]`,
`ampCompDutyOffset[]`, `initManualAmpCompCalibrationVal[]` (`uint16_t` PWM counts
scaled from wrap 14000), `DCO_calibration_start_note`, `calibration_note_interval`,
`ampCompLowestFreqVal`), `noteList.h` (`sNotePitches[]`), and `PWM.h`.

### From the build

A LittleFS partition — `flash=4194304_524288` on both boards. Without it
`init_FS()` cannot mount, and neither calibration nor presets survive a reboot.
What is on that partition and how full it is: [`FILESYSTEM.md`](FILESYSTEM.md).

## Note numbering

Calibration note numbers are **not** MIDI note numbers. `note_to_freq()` reads
`sNotePitches[n - 12]`, and `sNotePitches[0]` is `NOTE_C_1` (MIDI 0), so a note
number here names the pitch **one octave below** the MIDI note of the same
number:

| Constant | Value | `note_to_freq()` |
|---|---|---|
| `manual_DCO_calibration_start_note` | 24 | 16.35 Hz (`NOTE_C0`) |
| `DCO_calibration_start_note` | 29 | 21.83 Hz (`NOTE_F0`) |
| `manual_cal_reference_note` | 81 | 440.00 Hz (`NOTE_A4`) |

The whole autotune path shares the convention — `voice_task_autotune()` looks up
`VOICE_NOTES[0] - 12` the same way — but code outside it does not, so a value
crossing that boundary needs the offset applied. Getting this wrong once had
manual step 2 trimming at 220 Hz while the panel said 440.
