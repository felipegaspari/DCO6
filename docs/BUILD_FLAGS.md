# DCO6 Build Flags Catalog

Complete inventory of **live compile-time flags** that change codegen, RAM, IO, or A/B math
paths.

- **Deep float/fixed math:** [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md)
- **Profiler usage:** [`BENCHMARKING.md`](BENCHMARKING.md)
- **SRAM / heap / stack:** [`MEMORY.md`](MEMORY.md)
- **Autotune algorithms:** [`../_shared/docs/AUTOTUNE.md`](../_shared/docs/AUTOTUNE.md)
- **Live source of truth for engine/IO/cal toggles:** [`../settings.h`](../settings.h)

**Where flags live.** [`DCO6.ino`](../DCO6.ino) contains **no flags of its own** — it includes
[`project_config.h`](../../project_config.h) and [`settings.h`](../settings.h) first, under the
`SETTINGS FILE !!!! CRITICAL` banner. Header `#ifndef` / library fallbacks apply only when
`settings.h` left them unset.

**Out of scope:** include guards, X-macro / dirty-bit / note-frequency tables, MIDI_Library and
PID examples.

---

## Shipping snapshot

Board defaults in `settings.h` differ by MCU (`PICO_RP2350` vs else). The overrides block after
that can force either shape.

| Area | RP2350 | RP2040 |
|------|--------|--------|
| Voice / amp | `USE_FLOAT_VOICE_TASK` + `USE_FLOAT_AMP_COMP` | Fixed (`USE_FLOAT_*` undefined) |
| Pitch | **`PITCH_INTERP_FLOAT` (0)** | `PITCH_INTERP_RATIO_Q16` (1) |
| Amp method default | **`1` (`LUT`)** | `2` (`FIXED`) |
| Clkdiv | `CLKDIV_FLOAT` (1) | `CLKDIV_Q16` (2) |
| CV outs | **`USE_FLOAT_CV_OUTS` on** | off (soft-float would choke Core 1) |
| Mod matrix | **`USE_MOD_MATRIX_FLOAT_ENGINE` on** | off |
| Noise | **`NOISE_ENGINE 2`** (PrimeHybridNoise), `ENABLE_NOISE_OUT` off | same |
| Autotune | FREQ_TRACE / INTERP / CALC (`AUTOTUNE_*_DEFAULT` = 1 / 1 / 1) | same |
| ADSR | fixed Q22 phase, micros, native Q15 | same |
| USB panel | `ENABLE_USB_CONTROL` on | same |
| Mainboard UART | `ENABLE_MAINBOARD_LINK` on; `ENABLE_MB_MOD_STREAM` **off** | same |
| Serial DMA | `DCO_PROTOCOL_IMPLEMENT_DMA` on; `SERIAL_FRAMING_COBS` off | same |
| CV / mux / aux HW | off (`ENABLE_CV_OUTS` / `WAVE_MUX` / `VOICE_AUX` commented) — **retained**, unused on this 4×2 board. PW PWM is independent and always live. | same |
| PIO RESET | `ENABLE_PIO_RESET_INVERT` **on** | same |
| PIO clkdiv update | `UPDATE_CLK_DIV_INSTANTLY` **on** | same |
| RANGE amp PWM | `RANGE0_PIO_DITHER_TEST` **off** (HW slice; dither not feasible for 8 oscs) | same |
| Profiler | `BENCHMARKING_ENABLED` on, **`ENABLE_SWD_TELEMETRY` on**, `RUNNING_AVERAGE` **off** | same |
| Mem diag | `ENABLE_MEM_DIAG` **off** (and unreachable — see §1.5) | same |
| Presets | `REMEMBER_LAST_PRESET` **off** → boots into `preset_store_load(4)` | same |
| SRAM placement | `SRAM_HOT_ENABLE 1`, `SRAM_DATA_ENABLE 1` | same |

---

## 1. [`settings.h`](../settings.h)

### 1.1 Engine — pitch mode ids

| Define | Value | Role |
|--------|------:|------|
| `PITCH_INTERP_FLOAT` | 0 | Float interp — **RP2350 default**, *"much faster than cached"* |
| `PITCH_INTERP_RATIO_Q16` | 1 | Native Q16 slope path — **RP2040 default** |
| `PITCH_INTERP_Q12` | 2 | Legacy ×10000 slope A/B |
| `PITCH_INTERP_FLOAT_CACHED` | 3 | Trunc+clamp±1 float find (needs float voice) |

> **Renamed:** id 3 was `PITCH_INTERP_FLOAT_FAST`. `bench.h` and the guard `#error` text still
> print the old name.

### 1.2 Engine — clkdiv mode ids

| Define | Value | Role |
|--------|------:|------|
| `CLKDIV_GOLD` | 0 | `double llround(sys / Hz)` from Q24 — gold standard / A/B |
| `CLKDIV_FLOAT` | 1 | Q24 → float Hz — **RP2350 default** |
| `CLKDIV_Q16` | 2 | Q16 Hz → 64/32 — **RP2040 default (shipping)** |
| `CLKDIV_Q8` | 3 | Q8 Hz → 32/32 + remainder; `< 16 Hz` → internal precise Q8 |
| `CLKDIV_FAST_Q4` | 4 | Q4 Hz → 32/32; fastest, least accurate. This is the old HP0 path |

> Value `0` is GOLD, **not** the old HP0 Q4 path (that is `FAST_Q4` = 4). Value `1` is FLOAT,
> **not** the old PRECISE_Q8 path (Q8 is 3).

### 1.3 Engine — board defaults (`PICO_RP2350` / else)

| Define | RP2350 | RP2040 | Effect | Consumed |
|--------|--------|--------|--------|----------|
| `USE_FLOAT_VOICE_TASK` | **on** | off | Compile `voice_task_float`; otherwise `voice_task_fixed_point` from `voice_task_backup.ino` | `_shared/voices.h`, `voices.ino` |
| `USE_FLOAT_AMP_COMP` | **on** | off | Float amp tables + LUT (`NUM_OSCILLATORS × 7001 × 2` bytes; ~109 KB at 8 osc) dual-build | `_shared/amp_comp.h`, `FS.ino` |
| `USE_FLOAT_CV_OUTS` | **on** | off | Float VCA/VCF/keytrack/drift/velocity math | `cv_out.ino`, `cv_state.h` |
| `USE_MOD_MATRIX_FLOAT_ENGINE` | **on** | off | Float mod-matrix evaluation | `mod_matrix.h`, `_shared/mod_matrix_engine.h` |
| `AMP_COMP_METHOD_DEFAULT` | **`1` (LUT)** | `2` (FIXED) | Initial `amp_comp_method` | `_shared/amp_comp.h`, runtime cmds 20–22 |
| `PITCH_INTERP_MODE` | `PITCH_INTERP_FLOAT` | `PITCH_INTERP_RATIO_Q16` | Selects one interpolator + only its tables | `voices.ino`, pitch benches |
| `CLKDIV_MODE` | `CLKDIV_FLOAT` | `CLKDIV_Q16` | See §1.2. Fixed voice → `clkdiv_live_total_cycles`; float → `clkdiv_live_hz_total_cycles`. Cmds 32–33 A/B | `voices.ino`, `voice_task_backup.ino`, `_shared/clkdiv.h`, `clkdiv_bench.ino` |

### 1.4 Engine — overrides (commented A/B)

The overrides block is commented in the tree. Uncomment to force a path or A/B.

| Define | State | Effect |
|--------|-------|--------|
| `USE_FLOAT_VOICE_TASK` | commented | Force float voice on RP2040 |
| `USE_FLOAT_AMP_COMP` | commented | Force float amp dual-build |
| `USE_FLOAT_CV_OUTS` | commented | Force float CV path |
| `CLKDIV_MODE` | 5 commented variants | Force any clkdiv method |
| `AMP_COMP_METHOD_DEFAULT` | commented | Force amp method (0/1 need float amp) |
| `PITCH_INTERP_MODE` | commented, needs `#undef` first | Force any interpolator |
| `USE_VOICE_TASK_Q24` | **commented** | Experimental Q24 voice task in `voice_task_backup.ino` |

> ⚠️ The `PITCH_INTERP_RATIO_Q16` override line is annotated *"shipping default both MCUs"* —
> that comment is stale; RP2350's board default is `PITCH_INTERP_FLOAT`.

**Guards:** `FLOAT` / `FLOAT_CACHED` without float voice → `#error`. `CLKDIV_MODE > 4` →
`#error`.

### 1.5 Profiling / bench

| Define | State in tree | Effect | Consumed |
|--------|---------------|--------|----------|
| `BENCHMARKING_ENABLED` | **on** | Master gate for the whole bench block | `settings.h`, `DCO6.ino` |
| `ENABLE_SWD_TELEMETRY` | **on** | SWD telemetry for PlotJuggler; defines `ENABLE_SWD_PERIOD`. Keeps `bench_telemetry` / `bench_meta_json` alive in `DCO6.ino` | `bench.h`, `DCO6.ino` |
| `RUNNING_AVERAGE` | **off (commented)** | Hot-path profiler; paced `bench_out_*` | `bench.h`, benches |
| `RUNNING_AVERAGE_FINE` | off | Extra tiny-stage probes (opt barrier) | `BENCH_FBEGIN` / `FEND` |
| `RUNNING_AVERAGE_PERIOD` | **unreachable** | Period probes only | `bench.h` |
| `ENABLE_MEM_DIAG` | **off + unreachable** | Cmd 13 RAM dump + loop polls | `_shared/mem_diag.h` |
| `BENCH_STAGE_STRIDE` | `1` | Stage probes every Nth loop (`1` = every iter); note-on always | `bench.h` |
| `BENCH_USE_SYSTICK` | `1` | SysTick for PERIOD + stages; `0` → 1 µs timer | `bench.h` |
| `BENCH_PERIOD_MAX_US` | `20000` | Discard PERIOD samples longer than this | `bench.h` |
| `BENCH_PATH_STATS` | off | All path bumps + `-- Path counters --` dump | `bench.h`, `voices.ino` |
| `AMP_COMP_BENCHMARK` | off | Amp cmds 24–25; **implies `USE_FLOAT_AMP_COMP`** | `amp_comp_bench.ino` |

> 🔴 **The profiler block is an `#if` / `#elif` chain whose second branch is currently dead.**
> ```c
> #define ENABLE_SWD_TELEMETRY
> //#define RUNNING_AVERAGE
> #if defined(ENABLE_SWD_TELEMETRY)
>    #define ENABLE_SWD_PERIOD
> #elif defined(RUNNING_AVERAGE)
>   //#define ENABLE_MEM_DIAG
>   #define RUNNING_AVERAGE_PERIOD      // ← never compiled
>   ...
> #endif
> ```
> Because `ENABLE_SWD_TELEMETRY` is defined, the first branch wins and **everything inside the
> `#elif` — including `RUNNING_AVERAGE_PERIOD` — never compiles**, no matter what is written
> there. To restore the running-average profiler: comment out `ENABLE_SWD_TELEMETRY` **and**
> uncomment `RUNNING_AVERAGE`.
>
> Consequence: the bench commands that need `RUNNING_AVERAGE` — profiler dump **10**, amp
> **24/25**, pitch **28/29**, clkdiv **32/33** — are **not available in the tree as checked in**.

`BENCH_STAGE_STRIDE`, `BENCH_USE_SYSTICK` and `BENCH_PERIOD_MAX_US` sit **outside** that chain
(still inside `#ifdef BENCHMARKING_ENABLED`), so they are always defined. Note the comment says
*"default 9"* for the stride while the actual `#define` is **1**.

### 1.6 Calibration — auto-cal boot defaults

Enums and `#ifndef` fallbacks live in [`_shared/autotune.h`](../_shared/autotune.h).
Algorithms: [`../_shared/docs/AUTOTUNE.md`](../_shared/docs/AUTOTUNE.md). Board specifics:
[`AUTOTUNE.md`](AUTOTUNE.md).

| Define | Shipping | Effect |
|--------|----------|--------|
| `AUTOTUNE_AMP_METHOD_DEFAULT` | `1` (FREQ_TRACE) | Amp-comp calibration search: `0` CLASSIC (per-note range-PWM search), `1` FREQ_TRACE (fixed-PWM frequency bisection from the manual 440 Hz anchor). Runtime 34/35. Header fallback `0` |
| `AUTOTUNE_SEARCH_MODE_DEFAULT` | `1` (INTERP) | How the frequency search closes once it has a bracket: `0` BISECT, `1` INTERP, `2` GATED. Runtime 37/38/39. Header fallback `1` |
| `AUTOTUNE_AMP0_MODE_DEFAULT` | `1` (CALC) | Amp-comp-0 endpoint (pair 0): `0` MEASURE (live hunt), `1` CALC (bottom-rung fit). Runtime 40/41. Header fallback `0` |

### 1.7 Calibration — PW sweep and duty targets

| Define | Value | Effect |
|--------|------:|--------|
| `PW_SWEEP_MODE_DEFAULT` | ⚠️ **ambiguous** | `0` FULL (2 %…98 %), `1` HALF_HIGH (50 %…98 %), `2` HALF_LOW (2 %…50 %). Compile-time template selection (`get_PW_level_interpolated<Mode>`), never a runtime branch |
| `PW_POLARITY_INVERTED` | `0` | PW CV polarity |
| `AMP_DUTY_INVERT_ALL` | `false` | — |
| `AMP_DUTY_INVERT_OSC_A` | `true` | Reverses the **search direction** of the duty bisection (not an output inversion) |
| `AMP_DUTY_INVERT_OSC_B` | `true` | Same, triangle core |
| `AMP_TARGET_DUTY_OSC_A` | `0.50f` | Saw core target |
| `AMP_TARGET_DUTY_OSC_B` | `0.62f` | Triangle core target — **not** 50 % |

> 🔴 **`PW_SWEEP_MODE_DEFAULT` is defined twice.** The `#ifndef` /
> `PROJECT_INSTRUMENT == 4` block sets it to `0` (FULL), then a later line redefines it to `1`
> (HALF_HIGH) with its `#undef` still commented out — a redefinition warning with an ambiguous
> final value. Either uncomment the `#undef` (for HALF_HIGH) or delete the trailing line (for
> FULL).

### 1.8 Noise

| Define | Shipping | Effect |
|--------|----------|--------|
| `NOISE_ENGINE` | **`2`** (PrimeHybridNoise) | Which `DCO_Noise` class `noise0..1` use |
| `ENABLE_NOISE_OUT` | off (commented) | PIO1 LFSR 1-bit white on GP2 |

Values: `0` ColoredNoise (Voss pink / 1-pole brown / white; whites from `PioNoiseWhite`),
`1` FastNoiseGen (economy Voss pink / leaky brown / local xorshift), **`2` PrimeHybridNoise**
(per-gen prime tables 997/1499/1999; dither + rephase), `3` ProNoise32 (Q16.15 Kellett pink /
DC-corrected brown / xorshift). Objects `noise0..1` in `noise.h`; `next()` called from
`loop()` on Core 0.

### 1.9 Board / IO

| Define | Shipping | Effect | Consumed |
|--------|----------|--------|----------|
| `ENABLE_MAINBOARD_LINK` | **on** | `Serial2` GP20/21 peers with the STM32 Mainboard | `Serial.h` / `Serial.ino` |
| `ENABLE_MB_MOD_STREAM` | **off** | Consume Mainboard `'m'` and skip local LFO1/2 + EnvDCO clocks. Off = DCO runs LFO1/2, all envelopes and matrix→pitch locally; `'m'` is still parsed but ignored for pitch mailboxes | `Serial.ino`, `voices.ino`, `DCO6.ino` |
| `ENABLE_USB_CONTROL` | **on** | Panel protocol on USB CDC (`dco_control`). Comment out for production — stray terminal bytes are read as frame headers | `Serial.h` / `Serial.ino` |
| `DCO_PROTOCOL_IMPLEMENT_DMA` | **on** | Compile the DMA TX implementation | `_build_libs/DCO-PROTOCOL/serial_dma_tx.h`, `Serial.ino` |
| `SERIAL_FRAMING_COBS` | **off** | On-wire `COBS(inner)+0x00` instead of RAW. Host: `dco_control --cobs`. **Must match Input/Screen** | `serial_frame.h` / `serial_parser.h` |
| `UPDATE_CLK_DIV_INSTANTLY` | **on** | Instant PIO divider update — removes note-transition clicks but adds clkdiv delays and jitter under fast modulation | `voices.ino` |
| `NOTE_RETRIG_MODE_DEFAULT` | `0` (EXACT_Y) | `0` = Y load + phase hold, `1` = SYNC_JMP (restart jmp only; degree offsets need EXACT_Y). Runtime 26/27 | `state_machines.ino` |
| `ENABLE_CV_OUTS` | off | Cut/Res/VCA/dist/levels PWM writers — unused here; kept for expansion. **PW is not behind this flag** | `PWM.ino`, `cv_out.ino`, `globals.h` |
| `ENABLE_WAVE_MUX` | off | Wave mux GPIO / shift register — unused here | `wave_mux.ino`, `globals.h` |
| `ENABLE_VOICE_AUX` | off | Skip local Dist/filter writers (an aux RP2040 owns them); also moves `OSC3_LEVEL_PIN` / `SUB_LEVEL_PIN` | `PWM.ino`, `globals.h` |
| `ENABLE_PIO_RESET_INVERT` | **on** | Active-low RESET pad via `GPIO_OVERRIDE_INVERT` (DG411 discharge) | `state_machines.ino` |
| `RANGE0_PIO_DITHER_TEST` | **off** | Off = HW slice `wrap = DIV_COUNTER` on all 8 `RANGE_PINS[]`. On = PIO dither — needs one SM per RANGE pin, none spare at 8 oscs | `PWM.h` / `PWM.ino`, `_shared/autotune_impl.h` |

> ⚠️ The comment above `ENABLE_PIO_RESET_INVERT` reads *"DCO3 (DG411) defines this; DCO4
> (active-high / FET) does not"* — stale. The flag **is** defined here and that is correct: the
> discharge switch is a DG411. There is also a leftover duplicate commented `#define` line
> just below it.

### 1.10 Presets and SRAM

| Define | Shipping | Effect |
|--------|----------|--------|
| `REMEMBER_LAST_PRESET` | **off (commented)** | On = persist the last slot to flash and recall it at boot. Off = `setup()` compiles the `#else` branch and always calls **`preset_store_load(4)`** |
| `SRAM_HOT_ENABLE` | `1` | Move hot **code** into SRAM (`SRAM_HOT` / `__not_in_flash_func`) |
| `SRAM_DATA_ENABLE` | `1` | Move hot **data** into SRAM |

---

## 2. [`project_config.h`](../../project_config.h)

| Define | Value | Role |
|--------|------:|------|
| `PROJECT_INSTRUMENT` | **4** | Which instrument this tree is. Gates `#if PROJECT_INSTRUMENT == 4` in `settings.h` and `_shared/FS_impl.h`. **DCO6 is still 4** |
| `DCO_MCU_BOARD` | — | One of `DCO_MCU_WEACT_RP2040` / `DCO_MCU_PICO` / `DCO_MCU_PICO2` / `DCO_MCU_WEACT_RP2350`. Selects the RESET/RANGE pin maps in `globals.h` |
| `RANGE_PWM_WRAP` | — | `DIV_COUNTER` |
| `PW_PWM_WRAP` | — | `DIV_COUNTER_PW` |

Read through a symlink by the shared submodules — this is the **only** place per-project flags
are declared. Never put them inside a submodule.

> ⚠️ The `#error` in `globals.h` that validates `DCO_MCU_BOARD` lists only three of the four
> accepted values — it omits `DCO_MCU_WEACT_RP2350`.

---

## 3. [`adsr.h`](../adsr.h) → `ADSR_Bezier.h`

Set **before** including the library (the library has its own fallbacks).

| Define | DCO shipping | Library fallback | Effect |
|--------|-------------:|------------------|--------|
| `ADSR_BEZIER_PHASE_SHIFT` | `22` | `22` | `22` = uint32 phase; `>22` = uint64 |
| `ADSR_BEZIER_USE_FLOAT` | `0` | `0` | `1` = float time index (soft-float on RP2040) |
| `ADSR_BEZIER_USE_MICROS` | `1` | `1` | `1` micros / `0` millis timebase |
| `ADSR_BEZIER_NATIVE_Q15` | `1` | **`0`** | `1` = primary amp domain Q15 |
| `ADSR_BEZIER_Q15_DYADIC` | `1` | `1` | Peak 32768 vs 32767 A/B |
| `ADSR_BEZIER_UPDATE_Q15_CACHE` | `1` | `1` | Ignored when NATIVE=1 |
| `ADSR_BEZIER_SRAM_HOT` | **`1`** | **`0`** | `__not_in_flash_func` on `getWave` / `noteOn` / `noteOff` |

Derived: `ADSR_BEZIER_PHASE_SCALE_U64` (`1` when shift > 22).
Constants: `ADSR_CV_CC = 4095`, `ADSR_CV_SCALE = 4096`, `ADSR_1_DACSIZE = 4000`,
`ARRAY_SIZE = 512`.

---

## 4. [`LFO.h`](../LFO.h) → `mo-lfo.h`

| Define | DCO shipping | Library fallback | Effect |
|--------|-------------:|------------------|--------|
| `MO_LFO_USE_Q15` | `1` | **`0`** | Preferred-path hint; API always has `getWave` + `getWaveQ15` |
| `MO_LFO_SRAM_HOT` | **`1`** | **`0`** | `__not_in_flash_func` on `getWaveQ15` + `_advanceUnitQ15` |
| `LFO_SINE_TABLE_BITS` | `9` | `9` | Sine LUT size `1 << bits` |

Depth scales (not toggles): `LFO1_PITCH_DEPTH_SCALE` 1700, `LFO2_PITCH_DEPTH_SCALE` 512,
`DRIFT_PITCH_DEPTH_SCALE` 1000.

---

## 5. [`globals.h`](../globals.h)

### Feature flags

| Define | Shipping | Effect |
|--------|----------|--------|
| `ENABLE_FS_CALIBRATION` | **on** | Load LittleFS `voiceTables` / PW cal into the amp-comp arrays |
| `USE_ADC_STACK_VOICES` | commented | Legacy ADC stack voices (GPIO 28) |
| `USE_ADC_DETUNE` | commented | Legacy ADC detune (GPIO 27) |

> ❌ **`ENABLE_PIO_MIDI` does not exist.** Earlier revisions listed it as a comment-only /
> planned flag for DIN-on-PIO-UART. There is no such symbol anywhere in the tree.

### Capacity constants (not toggles)

| Define | Value | Role |
|--------|------:|------|
| `NUM_VOICES_TOTAL` | 4 | MIDI voice-slot capacity (ADSR / flags / PW) |
| `NUM_OSCILLATORS` | 8 | Physical DCOs (2 per voice) |
| `NUM_PW_CHANNELS` | 4 (`= NUM_VOICES_TOTAL`) | PW PWM channels; `cal_pw_channel(osc)` = `osc / 2` |
| `NUM_FILTERS` | 2 (`#ifndef`) | Filter count |
| `MIDI_CHANNEL` | 1 | Default MIDI channel |
| `pioPulseLength` | `3000` | PIO reset pulse in clk_sys cycles; runtime cmd 160, range **[200, 50000]** |

### LittleFS flash partition

Presets (`pb00`…`pb63`, 4 records × 598 B per chunk) plus the calibration banks need a
filesystem slice. The plain `rp2040:rp2040:rpipico2:usbstack=tinyusb` FQBN often allocates
**none**:

```bash
arduino-cli compile \
  --fqbn rp2040:rp2040:rpipico2:usbstack=tinyusb,flash=4194304_524288 \
  --libraries ./_build_libs \
  .
```

`4194304_524288` = 4 MB flash, 512 KB LittleFS. Without it, `init_FS()` and preset save fail at
runtime. **Changing the FS size moves `_FS_start`/`_FS_end` and reformats the filesystem** —
back up calibration + presets with `tools/dco_control` first. Protocol:
[`PRESET_STORE.md`](PRESET_STORE.md).

---

## 6. Other live headers

| File | Define | Shipping | Effect |
|------|--------|----------|--------|
| `_shared/noise.h` | `NOISE_ENGINE` | fallback `0` | `settings.h` sets `2` |
| `bench.h` | `BENCH_USE_SYSTICK` / `BENCH_PERIOD_MAX_US` | sketch wins | Header `#ifndef` fallbacks only |
| `_shared/amp_comp.h` | `AMP_COMP_METHOD_DEFAULT` | fallback `AMP_COMP_FIXED` | Only if unset before include |
| `_shared/autotune.h` | `AUTOTUNE_AMP_METHOD_DEFAULT` / `_SEARCH_MODE_` / `_AMP0_MODE_` | sketch wins (`1`/`1`/`1`); header fallback `0`/`1`/`0` | Only if unset |
| `voices.ino` | `DCO_DEBUG_REPORT` | `0` | `1` = serial dump of OSC1 frequency stages |
| `voice_alloc_state.h` | `VOICE_ALLOC_SRAM_HOT` | `1` | Allocator into SRAM |
| `_build_libs/DCO-PROTOCOL/serial_frame.h` | `SERIAL_INNER_MAX_PAYLOAD` | `40` | Max inner payload; `#ifndef`, so a board could raise it |

Headers that only **consume** flags: `_shared/voices.h`, `cv_state.h`, `cv_out.h`, `PWM.h`,
`Serial.h`, `_shared/FS.h`, `mod_matrix.h`, `include_all.h`.

---

## 7. Vendored library notes

| Library | Flag interaction |
|---------|------------------|
| `ADSR_Bezier` | Defaults differ on `NATIVE_Q15` (`0` lib vs `1` DCO) and `SRAM_HOT` (`0` vs `1`). Always set via `adsr.h` |
| `mo-lfo` | `MO_LFO_USE_Q15` / `MO_LFO_SRAM_HOT` lib default `0`; DCO forces `1` in `LFO.h` |
| `DCO_Noise` | Reads `NOISE_ENGINE` / `ENABLE_NOISE_OUT`; fallback engine `0` |
| `DCO-PROTOCOL` | Reads `DCO_PROTOCOL_IMPLEMENT_DMA`, `SERIAL_FRAMING_COBS`, `PROJECT_INSTRUMENT` |
| `MIDI_Library` | Vendored, live |
| `PID_v1` | Vendored but **not included by any sketch file** — leftover from before `PID.ino` became `autotune_search.ino` |

---

## 8. Where to change what

| Goal | Where |
|------|--------|
| Float/fixed voice, pitch, amp, CV, matrix, clkdiv | `settings.h` **ENGINE — overrides** (board defaults for MCU policy) |
| Autotune method / search / amp-0 boot defaults | `settings.h` **CALIBRATION** block (`AUTOTUNE_*_DEFAULT`) |
| PW sweep mode, polarity, duty targets | `settings.h` **CALIBRATION** block |
| Noise engine / PIO white pin | `settings.h` **ENGINE — noise** |
| Profiler mode | `settings.h` **PROFILING / BENCH** — mind the `#if`/`#elif` chain in §1.5 |
| USB panel / Mainboard UART / CV HW / aux / RESET invert / DMA / COBS | `settings.h` **BOARD / IO** |
| Boot preset behaviour | `settings.h` `REMEMBER_LAST_PRESET` |
| Instrument id / MCU board / PWM wraps | `project_config.h` |
| ADSR phase / native Q15 / SRAM hot A/B | `adsr.h` before the library include |
| LFO Q15 hint / SRAM hot / sine table bits | `LFO.h` |
| FS cal load | `globals.h` `ENABLE_FS_CALIBRATION` |
| Reset pulse width | `globals.h` `pioPulseLength`, or runtime cmd 160 |
| OSC1 debug prints | `voices.ino` `DCO_DEBUG_REPORT` |

After flag changes: clean rebuild; confirm the LittleFS amp-comp load; listen to low notes and
the amp plateau; optional profiler dump (cmd **10**, needs `RUNNING_AVERAGE` — see §1.5) —
[`BENCHMARKING.md`](BENCHMARKING.md).
