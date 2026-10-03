# Hot-path benchmarking

How to measure where the DCO's realtime budget goes, and how to read the numbers without
fooling yourself.

- Flags: [`BUILD_FLAGS.md`](BUILD_FLAGS.md) §1.5 · engine math:
  [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md)
- Code: [`../bench.h`](../bench.h), probes in [`../DCO6.ino`](../DCO6.ino) and
  [`../voices.ino`](../voices.ino)
- Flags live in [`../settings.h`](../settings.h), **not** in the sketch.

---

## 0. Two profilers, one master switch

`bench.h` implements **two completely different** measurement backends under a single
`BENCHMARKING_ENABLED` gate:

| Mode | Selected by | How you read it | Serial cost |
|---|---|---|---|
| **SWD telemetry** (PlotJuggler) | `ENABLE_SWD_TELEMETRY` | Debugger reads a live RAM struct | **none** |
| **Running average** (serial dump) | `RUNNING_AVERAGE` | ASCII report over USB CDC | paced TX |

**As checked in, the tree runs SWD telemetry.** `ENABLE_SWD_TELEMETRY` is defined and
`RUNNING_AVERAGE` is commented out.

### 🔴 The selection is an `#if` / `#elif` chain — mind the dead branch

```c
#define BENCHMARKING_ENABLED
#ifdef BENCHMARKING_ENABLED

#define ENABLE_SWD_TELEMETRY
//#define RUNNING_AVERAGE

#if defined(ENABLE_SWD_TELEMETRY)
   #define ENABLE_SWD_PERIOD

#elif defined(RUNNING_AVERAGE)
//#define ENABLE_MEM_DIAG
#define RUNNING_AVERAGE_PERIOD      // ← never compiled while SWD is on
#if defined(RUNNING_AVERAGE_PERIOD)
#elif defined(RUNNING_AVERAGE_FINE)
#endif
#else
#endif
```

Because `ENABLE_SWD_TELEMETRY` is defined, the first branch wins and **everything inside the
`#elif` never compiles** — including the `#define RUNNING_AVERAGE_PERIOD` and
`#define ENABLE_MEM_DIAG` written there. They look active in the file and are not.

**To switch to the serial profiler:** comment out `ENABLE_SWD_TELEMETRY` **and** uncomment
`RUNNING_AVERAGE`. Doing only one of the two leaves you with no profiler at all (the `#else`
branch is empty), and `bench.h` will trip its own guard at
`#if !defined(ENABLE_SWD_TELEMETRY) && !defined(RUNNING_AVERAGE)`.

**Consequence for everything below:** §3–§12 describe the **serial** profiler. While SWD
telemetry is active, debug commands **10 / 11 / 12** are **no-ops** — `bench_request_dump()`,
`bench_reset_all()`, `bench_toggle_periodic()` and `bench_service()` are all empty inlines in
that build — and the amp / pitch / clkdiv one-shots (24/25, 28/29, 32/33) are unavailable
because they need `RUNNING_AVERAGE`.

---

## 1. Mode A — SWD telemetry (active in the tree)

Zero-tax measurement: nothing is printed, nothing is buffered, no serial bandwidth is used. The
firmware keeps a live struct in RAM and a debugger reads it out over SWD.

### The telemetry struct

```cpp
struct __attribute__((packed, aligned(4))) PicoBenchTelemetry {
  uint32_t <one field per probe>;   // generated from BENCH_PROBES
};
extern volatile __attribute__((used)) PicoBenchTelemetry bench_telemetry;
extern volatile __attribute__((used)) const char bench_meta_json[];
```

Both symbols are `volatile` + `used` so the optimiser and the linker leave them alone.
`DCO6.ino` additionally does `volatile char _keep_json = bench_meta_json[0];` — a deliberate
trick to stop the linker garbage-collecting the JSON.

### Self-describing layout

`bench_meta_json[]` embeds a JSON array between scannable markers:

```
@@BENCH_META_BEGIN@@[
{"id":"loop0_period","core":0,"parent":"","label":"loop period"},
...
{}
]@@BENCH_META_END@@
```

A host tool can locate those markers in the ELF/binary and learn every probe's **id, core,
parent and label** without being rebuilt alongside the firmware. Field order in the JSON
matches field order in `PicoBenchTelemetry`, so index *i* in the metadata is offset *i* in the
struct.

### Two sub-modes

`ENABLE_SWD_PERIOD` is defined automatically when SWD telemetry is on, giving **period-only**
measurement:

| | Period-only (`ENABLE_SWD_PERIOD`, **active**) | Full stage breakdown |
|---|---|---|
| `BENCH_BEGIN` / `END` / `FBEGIN` / `FEND` | compile to `do {} while (0)` | accumulate into `bench_acc[]` |
| `BENCH_SAMPLE_TICK()` | compiles to nothing | folds `bench_acc[]` into `bench_telemetry` |
| `BENCH_PERIOD` | writes `bench_telemetry.<id>` directly | accumulates into `bench_acc[]` |
| Cost | essentially a SysTick read per loop | one read per probe |

To get the full breakdown, remove `ENABLE_SWD_PERIOD` from the SWD branch.

### Smoothing — the numbers are not raw

Both sub-modes apply an **asymmetric exponential moving average**, tuned to catch spikes and
forget them slowly:

| Path | Attack (sample > current) | Decay (sample < current) |
|---|---|---|
| `BENCH_PERIOD`, period-only | `cur + ((raw-cur) >> 2) + 1` | `cur - ((cur-raw) >> 3) + 1` |
| `BENCH_SAMPLE_TICK`, full | `cur + ((span-cur) >> 1) + 1` | `cur - ((cur-span) >> 4) + 1` |

So a value you read over SWD is a **smoothed** figure, not an instantaneous one, and not a mean
over a fixed window. Rising edges converge in a handful of iterations; falling edges take
roughly 8× (period) or 16× (stage) longer. **Do not quote these as min/max.** For a hard
worst-case number, use the serial profiler's `max` column instead.

Units are raw **SysTick counts** masked with `BENCH_TIMER_MASK`, not microseconds — divide by
`clk_sys` MHz.

`BENCH_SAMPLE_TICK()` only folds probes whose `bench_probe_core[i]` matches the calling core,
so each core owns its own fields and there is no cross-core write race.

### What still prints

`bench_poll_core0()` remains alive in this mode: it drains any pending PIO probe report
(`pio_probe_report_flush()`) through the chunked output path. That is the only serial traffic
the profiler produces here.

---

## 2. Mode B — running-average serial profiler

*Currently **off**. Everything from here down requires `RUNNING_AVERAGE` (see §0).*

### 2.1 Turning it on

In [`../settings.h`](../settings.h):

```c
// comment this out first:
//#define ENABLE_SWD_TELEMETRY

#define RUNNING_AVERAGE         // profiler on (main stage probes)
#define RUNNING_AVERAGE_FINE    // plus the tiny per-stage probes
#define RUNNING_AVERAGE_PERIOD  // only loop / loop1 periods (no stage probes)
#define BENCH_PATH_STATS        // all path bumps + -- Path counters -- dump
#define ENABLE_MEM_DIAG         // cmd 13 RAM dump + loop polls
```

These three are already defined **outside** the mode chain, so they apply either way:

```c
#define BENCH_STAGE_STRIDE 1      // MAIN/FINE every Nth loop; 1 = every iter
#define BENCH_USE_SYSTICK  1      // PERIOD + stages on SysTick; 0 = 1 us timer
#define BENCH_PERIOD_MAX_US 20000 // discard PERIOD samples longer than this
```

> The comment above `BENCH_STAGE_STRIDE` says *"default 9"*, but the actual `#define` in the
> tree is **1** (every iteration — the high-tax setting).

With both modes off, every `BENCH_*` macro expands to nothing: no runtime cost, no storage.

**`RUNNING_AVERAGE_PERIOD`** (needs `RUNNING_AVERAGE`): stage `BENCH_BEGIN`/`END` compile out;
only `BENCH_PERIOD` for `loop period` / `loop1 period` collects. Path counters are disabled
too. Use it for a true loop-time baseline without probe tax. The dump banner says
`period only`. Do not combine with FINE expecting stage rows — PERIOD wins.

**`BENCH_PATH_STATS`**: all `BENCH_PATH_INC` bumps (amp / ratio / porta) plus walk-step sums and
the `-- Path counters --` dump block. Leave off for shipping. No-op under PERIOD. Banner
`bench:` shows `path_stats=0/1`.

**`BENCH_STAGE_STRIDE`** (ignored under PERIOD): `BENCH_PERIOD` still fires every loop.
MAIN/FINE probes run only every Nth iteration; the dump **scales `sum`/`n` ×N** so child
**mean** and **`%win`** match a full-rate dump. **min/max** come from sampled iterations only.
The note-on family (`phase align`, `retrig period split`, `note-on retrigger`, `retrig SM
apply`, `retrig RANGE PWM`) is **`BENCH_T_RARE`** — always recorded, never scaled.

**`BENCH_USE_SYSTICK`** (`1`): PERIOD and stages share SysTick. `0` = 1 µs timer for all
probes. The dump window gate always uses `bench_us_now()`.

**`BENCH_PERIOD_MAX_US`** (`20000`): discard a PERIOD sample longer than this — autotune and
wrap-looking stalls.

### 2.2 Period-only vs full MAIN

Same preset, same playing: compare `loop` / `loop1` mean **and** max. Full MAIN without
sampling used to be ~2× period-only (worked example on RP2040 @ 240 MHz: loop1 **109 µs** vs
**59 µs**). That gap is instrumentation, not synth speed:

1. Each child's `BENCH_END` bookkeeping lands in the **parent** `(unattributed)` (~18 MAIN
   children under `voice_task` → ~24 µs/iter + ~10 µs `loop1` unattributed). The overhead
   subtract is only ~2 SysTick cycles.
2. `volatile` BEGIN barriers change codegen (~10–15 µs of extra real work).
3. A slower loop1 makes the ~99 µs PW update fire on more iterations.

Use **period-only** for *"is the synth faster?"*. Use MAIN **`%win` / child means** to rank
where time goes — never compare full vs period absolute loop means.

### 2.3 Getting a report

On the **Diagnostics** tab of [`../tools/dco_control`](../tools/dco_control/README.md), the
**Hot-path profiler** buttons send `PARAM_DEBUG_COMMAND` (id **160**):

| Button | Value | Effect |
|--------|------:|--------|
| Dump profiler once | `10` | Dump once |
| Reset profiler | `11` | Reset all accumulators |
| Toggle ~1 Hz dump | `12` | Toggle the automatic dump |

> Under SWD telemetry these three are **empty inlines** and do nothing.

A dump is asynchronous. Setting the request flag makes each core snapshot and clear its own
probes at its next loop iteration; core 0 then **formats** the report into a RAM buffer and
drains it over USB in small chunks across later `loop()` turns. **Core 1 never prints.**

**Mainboard profiler** (STM32) uses separate opcodes **40 / 41 / 42** on the same
`PARAM_DEBUG_COMMAND` id. The DCO forwards them over Serial2 and the ASCII comes back as slim
`'t'` frames. See §12. Do not reuse DCO 10 / 11 / 12.

While paced TX is active, both cores' PERIOD and stage probes skip samples so dump traffic
cannot inflate `max`. Path counters pause too. After each snapshot/reset, period probes
invalidate their previous timestamp so the first sample of the new window cannot straddle the
old one.

Dumps wait until the collection window is **≥ 1 s** since the last reset or dump
(`BENCH_MIN_WINDOW_US`). Requesting earlier just keeps sampling. Amp-comp method acks print
immediately.

---

## 3. Reading the output

The banner prints grouped flag lines so rebuilds and method switches are obvious. On the
current tree it should read:

```
=================== DCO BENCH ===================
clk_sys 250 MHz   probe overhead 2 cyc   stages every 1
engine: mcu=RP2350 voice=FLOAT pitch=FLOAT amp=FLOAT cv=FLOAT amp_method=LUT clkdiv=FLOAT note_retrig=EXACT_Y
adsr:   phase=22 float=0 micros=1 native_q15=1 dyadic=1 q15_cache=1 sram_hot=1
lfo:    sram_hot=1
noise:  engine=2 out=0
board:  cv_outs=0 wave_mux=0 voice_aux=0 pio_rst_inv=1 fs_cal=1
bench:  amp_comp=0 path_stats=0
```

| Line | Fields |
|------|--------|
| `engine:` | `mcu`; `voice` / `amp` / `cv` (`USE_FLOAT_VOICE_TASK` / `USE_FLOAT_AMP_COMP` / `USE_FLOAT_CV_OUTS`); `pitch` (`PITCH_INTERP_MODE`); `amp_method` (live); `clkdiv` (`CLKDIV_MODE`); `note_retrig` (live) |
| `adsr:` | `ADSR_BEZIER_*` from [`../adsr.h`](../adsr.h) |
| `lfo:` | `MO_LFO_SRAM_HOT` from [`../LFO.h`](../LFO.h) |
| `noise:` | `NOISE_ENGINE`, `ENABLE_NOISE_OUT` |
| `board:` | `ENABLE_CV_OUTS`, `ENABLE_WAVE_MUX`, `ENABLE_VOICE_AUX`, `ENABLE_PIO_RESET_INVERT`, `ENABLE_FS_CALIBRATION` |
| `bench:` | `AMP_COMP_BENCHMARK`, `BENCH_PATH_STATS` |

> ⚠️ **`pitch=FLOAT_FAST` can still appear** in the banner for `PITCH_INTERP_FLOAT_CACHED` —
> `bench.h` prints the pre-rename label. The mode is `FLOAT_CACHED`; the string is stale.

> ⚠️ On an RP2350 build, an `engine:` line reading `voice=FIXED pitch=RATIO_Q16 amp=FIXED`
> means an override is forcing the RP2040 shape — the board defaults would give
> `voice=FLOAT pitch=FLOAT amp=FLOAT`.

Verify the banner against [`BUILD_FLAGS.md`](BUILD_FLAGS.md) before trusting any comparison:
it is the only place the *live* configuration is reported.

---

## 4. How it measures

- Probes are declared once in the `BENCH_PROBES` X-macro table in `bench.h` — id, core, kind,
  tier, parent, label. Storage, the telemetry struct, the JSON metadata and the report rows are
  all generated from that single table.
- `BENCH_T_MAIN` / `BENCH_T_FINE` / `BENCH_T_RARE` tiers control what is sampled under
  stride and what is always recorded.
- Parent/child relationships drive the `%win` column and the `(unattributed)` remainder.
- Timer source is SysTick (`systick_hw->cvr`, down-counting, masked with `BENCH_TIMER_MASK`)
  unless `BENCH_USE_SYSTICK 0`.
- Each core samples only its own probes (`bench_probe_core[]`); core 0 does all formatting and
  printing.

---

## 5. What it cannot tell you

- **Interrupt time** is attributed to whatever probe was open. The DIN MIDI UART IRQ is
  exclusive and can fire anywhere.
- **Cross-core stalls** show up as inflated spans, not as their own line. Core 1 holds bus
  priority (`BUSCTRL_BUS_PRIORITY_PROC1_BITS`), so a Core 0 probe can widen when Core 1 is busy.
- **Flash vs SRAM placement** changes results. `SRAM_HOT_ENABLE` / `SRAM_DATA_ENABLE` are on;
  moving a function in or out of `__not_in_flash_func` changes its timing independently of its
  logic.
- **Smoothed SWD values are not extremes** (§1). For worst case, use the serial profiler.
- **Calibration is not the play path.** While `calibrationFlag || calibrationVerifyRequested`
  is set, `loop1()` early-returns into `autotune_loop_task()` and the voice task never runs —
  period samples from that window are meaningless (hence `BENCH_PERIOD_MAX_US`).

---

## 6. Adding a probe

1. Add a row to the `BENCH_PROBES` table in `bench.h`: `id, core, kind, tier, parent, label`.
2. Bracket the code with `BENCH_BEGIN(id)` / `BENCH_END(id)` (or `BENCH_FBEGIN` / `FEND` for a
   FINE-tier probe, `BENCH_PERIOD(id)` for a loop period).
3. Nothing else. Storage, the telemetry field, the JSON entry and the report row are generated.

Keep the `core` column honest — `BENCH_SAMPLE_TICK()` uses it to decide which core folds the
accumulator, and a wrong value means the probe is never sampled.

---

## 7. Former `CLKDIV_BENCHMARK` (removed)

Superseded by the `CLKDIV_MODE` A/B one-shots in §10. There is no `CLKDIV_BENCHMARK` flag.

---

## 8. Amp-comp methods (`AMP_COMP_BENCHMARK`)

*Needs `RUNNING_AVERAGE` **and** `USE_FLOAT_AMP_COMP`; `AMP_COMP_BENCHMARK` implies the latter.*

| Cmd | Effect |
|----:|--------|
| `20` / `21` / `22` | Select live method: FLOAT_QUAD (0) / LUT (1) / FIXED (2) |
| `24` | Speed one-shot across all built methods |
| `25` | Accuracy one-shot vs the float quadratic reference |

Implementation in [`../amp_comp_bench.ino`](../amp_comp_bench.ino), which can install a
synthetic linear table (`amp_comp_bench_install_synthetic_linear`), snapshot and restore the
live tables around a run, and reset the window cache between candidates.

**Expected order on RP2350 (FPU):** LUT ≪ FLOAT_QUAD ≲ FIXED — which is why the board default
is **LUT**. On RP2040 soft-float, FLOAT_QUAD is usually slowest.

---

## 9. Pitch interpolators (cmds 28 / 29)

*Needs `RUNNING_AVERAGE` only.* Private tables live in
[`../pitch_interp_bench.ino`](../pitch_interp_bench.ino) so a run does not disturb the live
tables.

| Cmd | Effect |
|----:|--------|
| `28` | Speed — prints **seq** and **jump** tables |
| `29` | Accuracy in cents vs a higher-resolution private reference |

Candidates cover the float walk, the float cached/branchless find, the native Q16 ratio path
and the Q12 legacy path. The header line reports `live_pitch=` — the **compiled**
`PITCH_INTERP_MODE`, which is what the voice task actually uses. The private rows always
include both float finds regardless of the compiled mode.

---

## 10. Clkdiv A/B (cmds 32 / 33)

*Needs `RUNNING_AVERAGE` only.* [`../clkdiv_bench.ino`](../clkdiv_bench.ino).

| Cmd | Effect |
|----:|--------|
| `32` | Speed, reported as `pctVsGOLD_REF` |
| `33` | Accuracy in cents vs GOLD |

All methods run on **both** voice engines, with glue matching live behaviour: the fixed engine
uses the Q24 helpers; the float engine stays in native Hz for GOLD/FLOAT and converts Hz→Q24
for the integer methods. The reference is `cdb_clk_div_gold_ref` (double `llround`).

Method ids and math: [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md) §5.

---

## 11. Noise engines (`NOISE_ENGINE`)

Selected at compile time in `settings.h`; **the tree runs `2` (PrimeHybridNoise)**.

| Value | Class | Character |
|------:|-------|-----------|
| `0` | ColoredNoise | Voss pink / 1-pole brown / white; whites from `PioNoiseWhite` |
| `1` | FastNoiseGen | Economy Voss pink / leaky brown / local xorshift white |
| **`2`** | **PrimeHybridNoise** | Per-generator prime tables (997 / 1499 / 1999); dither + rephase |
| `3` | ProNoise32 | Q16.15 Kellett pink / DC-corrected brown / xorshift white |

`noise0` / `noise1` are constructed in `noise.h` (min/max/color/seed) and advanced from
**`loop()` on Core 0**, not the voice task. Bench probes: parent `noise_gens` plus
`noise refill`. `ENABLE_NOISE_OUT` (off) would route a PIO LFSR 1-bit white to GP2 for
scoping; the PIO noise path is otherwise dormant and `dcoNoisePioBegin` is a no-op.

---

## 12. Mainboard profiler (`'t'` relay)

The STM32 Mainboard has its own profiler, driven over the same `PARAM_DEBUG_COMMAND` id (160)
with **distinct opcodes**:

| Value | Effect |
|------:|--------|
| `40` | Mainboard dump once |
| `41` | Mainboard reset |
| `42` | Mainboard toggle periodic |

The DCO forwards these over `Serial2`. The Mainboard answers with `CMD_BENCH_TEXT` (`'t'`,
16-byte payload) frames, handled by `mb_handle_bench_text()` and queued for
`mb_bench_text_drain()`, which runs from `loop()` on Core 0 and emits the ASCII into the same
Board output pane.

`'t'` must have a row in `mainboardSerialCommands[]` on the DCO — it does — and in the
Mainboard's relay tables. **Do not reuse DCO opcodes 10 / 11 / 12 for the Mainboard.**

---

## 13. Quick decision table

| Question | Use |
|---|---|
| Is the synth faster after my change? | Period-only, compare `loop1` mean **and** max |
| Where is the time going inside the voice task? | Full MAIN, read `%win` and child means |
| What is the true worst case? | Serial profiler `max` column — **not** SWD smoothed values |
| Live plot while playing | SWD telemetry + PlotJuggler (current tree) |
| Which amp / pitch / clkdiv method is fastest? | Cmds 24/25, 28/29, 32/33 — all need `RUNNING_AVERAGE` |
| Is my build actually configured how I think? | The `engine:` / `board:` banner lines (§3) |
| How much RAM is left? | Cmd 13 — needs `ENABLE_MEM_DIAG`, currently unreachable (§0) |
