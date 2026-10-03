## DCO6 Project: AI Codebase Reference

This document is a **semantic map** of the DCO board firmware for **DCO6** (RP2040 /
RP2350-class, **4 MIDI voices × 2 oscillators = 8 physical oscillators**). It explains what
each file does and how the main subsystems (voices, modulation, calibration, storage, I/O) fit
together.

> **Naming.** DCO6 is a revision of DCO4-REBORN on the same codebase. The sketch is `DCO6.ino`
> in `DCO6/`, and `PROJECT_INSTRUMENT` is still **4** — the "6" is a revision name, not a voice
> count.

Related docs:
- Flat file + function + call-site inventory: [`FILE_INDEX.md`](FILE_INDEX.md)
- System topology (other boards): [`SYSTEM_OVERVIEW.md`](SYSTEM_OVERVIEW.md)
- Serial & parameter how-to: [`README_serial_and_params.md`](README_serial_and_params.md)
- Preset store / directory protocol: [`PRESET_STORE.md`](PRESET_STORE.md)
- Complete compile-time flag catalog: [`BUILD_FLAGS.md`](BUILD_FLAGS.md)
- Float vs fixed engine math (depth): [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md)
- Hot-path profiling: [`BENCHMARKING.md`](BENCHMARKING.md)
- SRAM / heap / stack: [`MEMORY.md`](MEMORY.md)
- Autotune algorithms: [`../_shared/docs/AUTOTUNE.md`](../_shared/docs/AUTOTUNE.md) (this board:
  [`AUTOTUNE.md`](AUTOTUNE.md))
- Repo entry point: [`../README.md`](../README.md)

---

## 1. Top-Level Sketch, Cores and Aggregated Includes

- **`DCO6.ino`**
  - Main application for the RP2040 / RP2350. Four entry points on the Arduino dual-core API:
    - `setup()` / `loop()` (**core 0**): USB/serial/MIDI I/O, LFO evaluation, **envelopes**,
      noise, CV outs.
    - `setup1()` / `loop1()` (**core 1**): FS init, calibration/autotune, real-time voice
      engine.
  - **Engine build options are NOT in this file.** They live in
    [`settings.h`](../settings.h), included near the top under the `SETTINGS FILE !!!!
    CRITICAL` banner, in the order: **pitch ids → clkdiv ids → board defaults → overrides →
    guards → noise → board/IO → calibration → presets/SRAM**.
    - Board defaults: **RP2350** float voice + float amp + float CV + float matrix,
      `PITCH_INTERP_FLOAT` (0), amp method **`LUT` (1)**, `CLKDIV_FLOAT`; **RP2040** fixed
      voice/amp/CV, `PITCH_INTERP_RATIO_Q16` (1), amp method `FIXED` (2), `CLKDIV_Q16`.
      No `USE_FLOAT_ENGINE` umbrella.
    - Overrides can `#undef` / `#define` those flags (pitch A/B needs `#undef
      PITCH_INTERP_MODE` first).
    - Full catalog: [`BUILD_FLAGS.md`](BUILD_FLAGS.md). Math depth:
      [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md).
  - The include order is load-bearing and annotated in the file: amp-comp and autotune must
    precede `bench.h`.
  - `init_usb()` sets the USB product strings via Adafruit TinyUSB.
    ⚠️ **They still read `"DCO4-REBORN"` / `"DCO4"` in the code** — a leftover, not a doc error.
  - Board pin fix: `pinMode(24, OUTPUT); digitalWrite(24, HIGH);` for every board **except**
    `DCO_MCU_WEACT_RP2350`, which is `#if`-guarded out.
  - Core 0 writes LFO pitch mods into `lfo1_pitch_mod_q24[]` / `lfo2_pitch_mod_q24[]`; core 1
    reads them in the voice task (float path converts Q24 → float each frame).
  - **`loop1()` opens with the calibration trap:**
    ```cpp
    if (__builtin_expect(calibrationFlag || calibrationVerifyRequested, 0)) {
        autotune_loop_task();
        return; // EARLY EXIT: voice_task_main() is never reached while calibrating!
    }
    ```
    One entry point, early return. `DCO_calibration()` and `DCO_calibration_debug()` are called
    **inside** `autotune_loop_task()`. Then `pio_defer_service()` → `voice_task_main()` →
    `bench_service(1)` → `mem_diag_poll_core1()`.
  - `setup1()` ends with `bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS` — the audio
    core owns the bus.
  - **Profiling** (`RUNNING_AVERAGE`, optionally `_FINE` / `_PERIOD`): both loops are bracketed
    by `BENCH_*` probes from [`bench.h`](../bench.h); core 0 does all printing from
    `bench_poll_core0()`. See [`BENCHMARKING.md`](BENCHMARKING.md).
  - **RAM dump** (`PARAM_DEBUG_COMMAND` 13, needs `ENABLE_MEM_DIAG`): `mem_diag_request` →
    Core1 stack snapshot → Core0 prints heap/stacks. Runtime 14/15 disable/enable polls.
    `__not_in_flash_func` is static `.time_critical` SRAM (not heap); pin policy is hot leaves
    only. See [`MEMORY.md`](MEMORY.md).

- **`settings.h`** — the engine flag file. See §1 above and
  [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md).

- **`project_config.h`** — the single declaration of `PROJECT_INSTRUMENT` (**4**) and
  `DCO_MCU_BOARD`. Read through a symlink by the shared submodules.

- **`include_all.h`** — convenience umbrella used by the `.ino` implementation files. Pulls in
  the RP2040/Arduino headers and the project modules.

- **`globals.h`**
  - System-wide constants and state:
    - Voice/osc counts: `NUM_VOICES_TOTAL = 4`, `NUM_OSCILLATORS = 8`,
      `NUM_PW_CHANNELS = NUM_VOICES_TOTAL`, `NUM_FILTERS = 2`. Runtime `NUM_VOICES` from
      `setVoiceMode` (0 mono → 1, 1 poly → 4, 2 stack). Each voice: `DCO_A = i*2`,
      `DCO_B = i*2+1`.
    - RANGE PWM wrap: `DIV_COUNTER = RANGE_PWM_WRAP`; PW wrap `DIV_COUNTER_PW = PW_PWM_WRAP`.
    - Clock: `sysClock_Hz` cached from `clock_get_hz(clk_sys)` by `sys_clock_hz_refresh()`,
      called once per core at the top of `setup()` / `setup1()`. **Never call
      `clock_get_hz` on the voice hot path.**
    - **`pioPulseLength` default `3000`** clk_sys cycles; runtime-settable via debug command
      160 in **[200, 50000]**.
    - **Period model** `period = Y + weight*clk_div + overhead`, weights/overheads `{4,5,6,7}`
      / `{12,13,14,15}` indexed by `softSyncChunks` (`PIO_*_BY_CHUNKS[]`).
    - Per-osc PIO state: `osc_uses_sync_program[]`, `osc_last_y[]`, `osc_last_clk_div[]`,
      `softSyncChunks`, `pio_loaded_sync_chunks`, `subOscDivide`.
    - Fixed-point pitch-bend multipliers (`pitchBendMultiplier_q24`); LFO pitch mods live in
      `LFO.h`.
    - Global voice arrays (`VOICE_NOTES`, `VOICES`, `note_on_flag`, `PW[]`).
    - Pin maps from `DCO_MCU_BOARD` — **four** variants: `DCO_MCU_WEACT_RP2040`,
      `DCO_MCU_PICO`, `DCO_MCU_PICO2`, `DCO_MCU_WEACT_RP2350`. RESET/RANGE ×8, PW `{3,2,4,5}`,
      `DCO_calibration_pin = 10`. `SUBOSC_PINS[]` all `0xFF`, and the array only exists under
      `#if defined(PICO_RP2350)`. See [`PINOUT.md`](PINOUT.md).
    - `VOICE_TO_PIO = {0,0,0,0,1,1,1,1}` — voices 0–1 on pio0 (osc 0–3), voices 2–3 on pio1
      (osc 4–7). A voice pair must share a PIO block so hard-sync sideset can share RESET.
      `pio_gpio_init()` on a second block steals the pin from the first;
      `pio_topology_report()` asserts ownership.
    - `VOICE_TO_SM = {0,1,2,3,0,1,2,3}` is **mutable**, rewritten by `assign_sm_mapping()` per
      pair: the slave takes the lower local SM index because when two SMs write a pin on the
      same cycle the higher-numbered one wins.
    - ⚠️ A stale comment labels GP20/21 the *"Input UART"*. It is the **Mainboard** link.

---

## 2. Voice Architecture & Real-Time Engine

- **`voices.h`** (`_shared/voices.h`)
  - Declares `init_voices()` and the voice-engine globals: portamento config and mode
    (`PORTA_MODE_TIME` / `PORTA_MODE_SLEW`), per-DCO portamento state in **Q24 Hz** and **Q16
    semitone** space, parallel float state (`porta_*_f`) under `USE_FLOAT_VOICE_TASK`, and
    pitch-multiplier storage gated by `PITCH_INTERP_MODE`: float tables + `slopeF`
    (`FLOAT` / `FLOAT_CACHED`), or int tables + **`slopeQ16`** (RATIO) / `slopeQ12` (Q12),
    plus `interpSegCache`.
  - **Only the active mode's slope array is allocated.** There is no `slopeQ20` anywhere in the
    tree.
  - No profiler declarations — probe storage is generated from the `BENCH_PROBES` table in
    [`bench.h`](../bench.h).

- **`voices.ino`** — central voice engine, **compile-time dual implementation**.
  - `init_voices()` sets initial notes, builds the pitch tables for the active
    `PITCH_INTERP_MODE` (`initMultiplierTables()`), sets voice mode and runs an initial
    `voice_task_main()`.
  - `voice_task_main()` → `voice_task_float()` **or** `voice_task_fixed_point()` depending on
    `USE_FLOAT_VOICE_TASK`.

  - **`voice_task_float()`** (in `voices.ino`; **the RP2350 default path**):
    - Portamento → modifiers → ratio → clkdiv → amp → PIO/PWM/PW, all in **Hz / float**.
    - Float portamento state; pitch bend / LFO / ADSR / drift / DCO_B interval+detune converted
      from Q24 globals where needed.
    - Pitch table via `interpolate_live_ratio_f`: `interpolateRatioFloat_fast` for
      `PITCH_INTERP_FLOAT` (**board default**), `interpolateRatioFloat_cached_fast` for
      `FLOAT_CACHED`, or the fixed interpolators for A/B.
    - Clkdiv via `clkdiv_live_hz_total_cycles`; amp via `get_chan_level_for_engine()`.
    - Details: [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md) §6.

  - **`voice_task_fixed_point()`** — ⚠️ **lives in `voice_task_backup.ino`, not `voices.ino`.**
    Despite the filename that file is **live production code**, guarded by
    `#ifndef USE_FLOAT_VOICE_TASK`. It is the shipping path on **RP2040**. It also holds
    `voice_task_Q24()` under `#ifdef USE_VOICE_TASK_Q24` (off).
    🛠️ Renaming it to `voice_task_fixed.ino` would stop someone deleting the RP2040 engine.
    - For each active MIDI voice (`NUM_VOICES`; `DCO_A = i*2` / `DCO_B = i*2+1`):
      - Portamento in **time-based frequency space** or **slew-rate note space** (Q24 / Q16).
      - Fixed-point modulators: pitch bend (`calcPitchbend_q24`), LFO1 per-osc
        (`lfo1_pitch_mod_q24[]`, includes `LFO1toDCO` + extra), LFO2 → **DCO_B only**, unison
        detune (`+1,-1,+2,-2`), per-osc drift (`LFO_DRIFT_LEVEL`), ADSR→detune in Q24 via
        `ADSR1toDETUNE1_scale_q24` and `env_dco_pitch_wave_q15` (gated by `ADSR3ToOscSelect`:
        0=A, 1=B, 2/4=A+B).
      - Pitch table via `interpolate_live_ratio_q16`: **`interpolateRatioQ16_fast`**
        (`RATIO_Q16`, `slopeQ16`) or `interpolatePitchMultiplierIntQ16_cached` (`Q12`).
      - Final osc frequencies in **Q24 Hz**, then clock dividers per `CLKDIV_MODE`
        (0 GOLD / 1 FLOAT / 2 Q16 / 3 Q8 / 4 FAST_Q4 — see
        [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md) §5), with corrected OSR dividers for A+B
        including DCO_B phase alignment.
      - **Amplitude compensation** via `get_chan_level_lookup_fast()` (Q8 Hz domain) →
        **RANGE PWM** through `write_range_pwm()`.
      - Writes dividers into the eight PIO SMs (pio0+pio1) and amp levels into RANGE channels.
      - At 99 µs (`timer99microsFlag`), updates PW PWM combining ADSR1 and LFO2 in integer math
        via `get_PW_level_interpolated()`.

  - Legacy `voice_task_simple` / `voice_task_debug` / gold reference: **removed**.
  - `setSyncMode()` calls `assign_sm_mapping()` + `start_voice_sms()` to rebuild the sync
    topology, then forces a re-trigger. It no longer pokes sideset pins in place or calls
    `pio_sm_restart()` — that cleared the shift counters but left PC/X/Y, which could strand an
    SM mid-loop with a stale X for one glitched period. **Manual calibration runs it with
    `syncMode` forced to 0**, because cal solo stops the partner of every pair and a synced
    slave cannot reset itself without a running master
    ([`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §7.3).
  - Amplitude compensation helpers: `get_chan_level_lookup_fast()` (fixed Q8 quadratic, always
    built), `get_chan_level_float_quad()` (cached-walk float), `get_chan_level_lut()` (dense
    nearest-Hz), `get_chan_level_float()` / `get_chan_level_for_engine()` (dispatch on
    `amp_comp_method`). `get_PW_level_interpolated()` maps PW counts into calibrated limits —
    it is a **template on the sweep mode**, resolved at compile time, never a runtime branch.
  - `voice_task_autotune()` — per-oscillator routine used during calibration (float-style
    clkdiv + `get_chan_level_for_engine`).

---

## 3. Oscillator, PIO State Machines and PWM

- **`pico-dco.pio.h`** — generated PIO programs producing the DCO rectangular wave trains: high
  and low periods via OSR loads, optional sync (reset / phase-aligned).
  `frequency_sync_4_jumps_program` is the production program; `dco_triangle_init()` configures
  the triangle core on odd indices.

- **`state_machines.h` / `state_machines.ino`**
  - **Full subsystem reference: [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md).** Read it before
    changing anything here.
  - `init_pio()` loads `frequency_sync_4_jumps` plus one soft-sync poll image into **pio0 and
    pio1** (8 freq SMs). RP2350: sub-osc programs on **pio2** SM0–3 (`SUBOSC_PIO = 2`, pins
    TBD). No noise LFSR. Then `assign_sm_mapping()` and `start_voice_sms()`.
  - `start_voice_sms()`:
    - `ensure_soft_sync_program()` so the resident poll image matches `softSyncChunks`.
    - Picks each oscillator's program: the slave runs the poll variant when
      `softSyncChunks > 0`.
    - Picks the sideset pin. **Hard sync**: the master's sideset points at the *slave's* reset
      pin, discharging the slave's integrator while it keeps its own schedule. **Soft sync**:
      the master leaves its own pin alone and the slave polls it through `jmp pin`.
    - Preloads Y with `pioPulseLength` and re-pushes `osc_last_clk_div[]`, because writing Y
      consumes the OSR that also feeds the chunk reads.
    - Starts every SM on the same cycle with `pio_enable_sm_mask_in_sync()`.
  - **Sync flavours:** hard sync costs nothing (weight 4) but is analog-cap-only. Soft sync
    (weights 5/6/7 for 1/2/3 trailing polled chunks) restarts the slave's own count; receptive
    windows are ~40 % / ~67 % / ~86 % of the ramp because polled chunks run at half speed.
  - **Phase offset** (`oscPhaseSync`) is a one-shot X countdown to `loop_final`
    (`osc_phase_align_hold_stopped`, addr 9). Y stays the real pulse; later cycles are
    undistorted. SYNC_JMP is 0° only. Detail: [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §8.
  - **Deferred work:** `pio_defer_request_{cal_restore,period_probe,reset_pulse_all,subosc,sync_mode}()`
    queue from Core 0; `pio_defer_service()` drains them at the top of `loop1()` before the
    voice task.
  - **`pio_reset_pin_apply_polarity()`** applies `GPIO_OVERRIDE_INVERT` when
    `ENABLE_PIO_RESET_INVERT` (**defined in this tree**), so PIO logical 1 = pad low = DG411 on
    = cap discharged.
  - ⚠️ **RESET pins are driven by side-set only.** `sm_config_set_set_pins()` is called nowhere,
    so every `pio_encode_set(pio_pins, x)` in the tree is a **no-op**. Use
    `pio_sm_set_pins_with_mask()` or `pio_encode_nop() | pio_encode_sideset(1, level)` with the
    SM disabled.
  - **Diagnostics:** `pio_topology_report()` (roles + reset-pin ownership, oscillators printed
    **1-based**), `pio_period_probe()` / `pio_solve_period_model()`, `mem_diag` cmd 13.

- **`PWM.h` / `PWM.ino`**
  - Amplitude is written through **`write_range_pwm(osc, level)`** (domain `0..DIV_COUNTER`).
  - **`RANGE0_PIO_DITHER_TEST` off:** hardware PWM slices on all eight `RANGE_PINS[]`. Dither
    needs one SM per RANGE pin and there are none spare with 8 oscillators.
  - PW PWM: `PW_PINS = {3,2,4,5}`, one per MIDI voice, `wrap = DIV_COUNTER_PW`.
  - `scale_level_cv_to_wrap()` / `level_pwm_slice_shares_voice_wrap()` implement the
    Resonance-1-shares-slice-3-with-RANGE-OSC2 workaround ([`PINOUT.md`](PINOUT.md)).

---

## 4. Envelopes (ADSR) and Modulation (LFO & Drift)

- **Vendored library `ADSR_Bezier`** (`_build_libs/ADSR_Bezier`, branch `main`)
  - Compile-time: `ADSR_BEZIER_USE_FLOAT` (`0` = Q24/Q16 fixed default),
    **`ADSR_BEZIER_NATIVE_Q15 = 1`** (shipping — amp domain Q15).
  - Unipolar Q15 primary (`ADSR_Q15_ONE`, `getWave()` / `levelQ15`).
  - Constants: **`ADSR_CV_CC = 4095`**, **`ADSR_CV_SCALE = 4096`** (panel sustain → Q15 via
    `>>12`).

- **`adsr.h` / `adsr.ino`**
  - `ADSR_1_DACSIZE = 4000`, `ARRAY_SIZE = 512`, `linToLogLookup`.
  - One `adsr` object per voice (`adsr1_voice_0..3`) wrapped in `ADSRVoices[]`.
  - Globals: `ADSR1_attack/decay/sustain/release`, curve parameters, restart flag, modulation
    depths (`ADSR1toDETUNE1`, `ADSR1toPWM`) with precomputed `ADSR1toDETUNE1_scale_q24`.
  - `init_ADSR()` — from **`setup1()`**: Bézier tables, `linToLogLookup`, initial A/D/S/R.
  - **`ADSR_update()` runs on CORE 0**, from `loop()` under `timer49microsFlag` (~49 µs).
    Earlier revisions of this page placed it on `loop1()` at ~10 kHz — that is wrong.
    Parameterless `noteOn`/`noteOff`/`getWave()`; EnvDCO/EnvVCA per voice, EnvVCF/EnvVCF2 once.
    With `NATIVE_Q15 = 1`, `getWave` fills `*_q15` only.
  - **`ADSR_set_parameters()`** — also Core 0, on `timer5msFlag2` (5 ms). Debounces A/D/S/R and
    re-applies only what changed.
  - Curve helpers: `ADSR*_change_{attack,decay,release}_curve()` (ids 48–56),
    `ADSR1/2/3_set_mode()` (223–225), `ADSR*_set_restart()`, `set_vcf_trigger_mode()` (57).

- **CV u12 edge (`cv_out.ino`)** — `CV_U12_MAX = 4095`, `CV_U12_SCALE = 4096`
  (`cv_q15_to_u12`: `(q15 * SCALE) >> 15`). Details:
  [`UPDATE_CV_OUTS_HOT_PATH.md`](UPDATE_CV_OUTS_HOT_PATH.md),
  [`CV_MOD_SCALES.md`](CV_MOD_SCALES.md).

- **External library `mo-lfo`** — 32-bit fixed-point phase accumulator driven by `micros()`;
  off / saw / triangle / sine (LUT) / square; free-running or BPM-synced. Basis for LFO1, LFO2
  and the per-DCO drift LFOs.

- **`LFO.h` / `LFO.ino`** — full reference: [`LFO.md`](LFO.md).
  - Live bus is **full-scale Q15** (`getWaveQ15` / `setAmplQ15`).
  - Depth scales in `LFO.h`: `LFO1_PITCH_DEPTH_SCALE` 1700, `LFO2_PITCH_DEPTH_SCALE` 512,
    `DRIFT_PITCH_DEPTH_SCALE` 1000, plus `lfo_pitch_depth_q24` and synth-side `applyDepthQ24`.
  - Instances: `LFO1_class`, `LFO2_class`, `LFO_DRIFT_CLASS[NUM_OSCILLATORS]`.
  - **LFO1 pitch routing:** `PARAM_LFO1_TO_DCO` (40) folded into each `lfo1_pitch_mod_q24[OSCn]`
    (`applyDepthQ24` of `LFO1toDCO_q24 + LFO1toOSCn_q24`). Per-osc extras
    `PARAM_LFO1_TO_OSC1/2/3` (216–218). LFO2 fine + coarse → `lfo2_pitch_mod_q24[]`.
  - `init_LFOs()` / `init_DRIFT_LFOs()` from **`setup()`**; `LFO1()` / `LFO2()` every Core 0
    iteration; `DRIFT_LFOs()` on `timer51microsFlag`.

---

## 5. Tuning, Calibration & Amplitude Compensation

- **`_shared/amp_comp.h`**
  - Per-DCO amplitude compensation with a dual runtime engine.
    - Shared: `freq_to_amp_comp_array`, plateau metadata, `AMP_COMP_MAX_HZ = 7000`,
      `ampCompTableSize = 22`, `ampCompArray` as `int32_t`.
    - Per-window data is array-of-structs: `FixedQuadWindow fixedWin[][]` (Q8 path) and
      `FloatQuadCoeffs floatCoeffs[][]` (Hz path).
    - Fixed Q8 tables always present; under `USE_FLOAT_AMP_COMP` also `ampCompFrequencyHz`,
      `ampCompLut[osc][0..7000]` (`NUM_OSCILLATORS × 7001 × 2` bytes, ~109 KB at 8 osc), and
      selectable `amp_comp_method` (`FLOAT_QUAD` 0 / `LUT` 1 / `FIXED` 2).
    - Fixed path: `ampCompFrequencyArray` in **Q8 Hz** (`FREQ_FRAC_BITS = 8`), per-window
      `y(t) = a*t² + b*t + c` with `T_FRAC = 12`, fields `invDx_q28` / `aQ_fast` / `bQ_fast`.
    - Float quadratic: `y = (a*x+b)*x+c` in Hz via `get_chan_level_float_quad` cached walk.
  - `precompute_amp_comp_for_engine()` runs after FS load in `setup1()`.
  - **Hardware polarity lives in `settings.h`**, not here: `AMP_DUTY_INVERT_OSC_A/B true`
    (reverses the **search direction** of the duty bisection, not the output),
    `AMP_TARGET_DUTY_OSC_A 0.50f`, **`AMP_TARGET_DUTY_OSC_B 0.62f`** (the triangle core does
    not target 50 %).
  - Flag / format details: [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md) §7.

- **`autotune.h` / `autotune.ino` / `autotune_search.ino` / `autotune_task.ino`**
  - `autotune.ino` and `autotune_search.ino` are **one-line shims** onto
    `_shared/autotune_impl.h` and `_shared/autotune_search_impl.h`. `FS.ino` is the same
    pattern onto `_shared/FS_impl.h`, and it deliberately sorts **first** among the sketch
    `.ino` files so its definitions precede the autotune impls that call them. Edit the shared
    copies.
  - `autotune_task.ino` is board-local and holds the real entry point:
    **`autotune_loop_task()`**, plus `autotune_drive_core()`, `autotune_manual_task()` and
    `cal_pin_diagnose()`.
  - Flags and state: `calibrationFlag`, `calibrationVerifyRequested`, `manualCalibrationFlag`,
    `firstTuneFlag`, `manualCalibrationStage`, per-oscillator offsets, PW values, note indices.
  - `calibrationData[]` stores [frequency, amplitude] pairs used to rebuild amp-comp tables.
  - Helper headers: `autotune_constants.h` (sizes, gap constants), `autotune_context.h`
    (`DCOCalibrationContext`), `autotune_measurement.h` (`GapMeasurement`).
  - Boot default amp method is **`FREQ_TRACE`** (`AUTOTUNE_AMP_METHOD_DEFAULT = 1` in
    `settings.h`).
  - `DCO_calibration()` — calibrates PW once per assigned channel via `cal_pw_channel(osc)`
    (`find_PW_center()`, `find_PW_limit_v2()`); this board maps **8 osc → 4 PW channels**
    (`ch = osc / 2`, and `osc_has_pw()` is true only for even indices). Then per oscillator
    `restart_DCO_calibration()` + `calibrate_DCO()` / `calibrate_DCO_freq_trace()`, persisted
    with `update_FS_voice()` and refreshed through `init_FS()` +
    `precompute_amp_comp_for_engine()`.
  - `restart_DCO_calibration()` re-arms RANGE pin/PIO and drives this oscillator's PW channel
    at its stored `PW_CENTER` with the rest at 0 (`apply_pw_center_solo()`).
  - `find_PW_center()` / `find_PW_limit_v2()` — target-duty searches on
    `find_PW_for_target_duty()` (coarse scan → bisection → lock-in) and
    `search_PW_limit_from_center()`; probes go through `set_pw_and_measure()`. Persisted via
    `update_FS_PWCenter()` / `_PW_Low_Limit()` / `_PW_High_Limit()`.
  - `find_gap()` — edge-timing core measuring duty at the calibration pin. Gap constants:
    `kGapTimeoutUs 100000`, `kGapTimeoutMaxUs 500000`, `kGapTimeoutPeriods 2.5`,
    `kGapTimeoutSentinel 1.16999f`, `kGapSamplesDefault 6`, `kGapPolarityInverted false`.
  - **The cal bus is an open-collector wired-NOR**: 8 comparator-driven NPN collectors share
    `DCO_calibration_pin` (GP10) with a pull-up. Any one transistor conducting pulls the whole
    line down. Parking is **asymmetric** (saw → 0 V, triangle → −12 V). See
    [`CALIBRATION_PROCEDURE.md`](CALIBRATION_PROCEDURE.md).
  - `MUTE_PW_CHANNEL` is a hardcoded literal `0` that ignores `pwSweepMode` and
    `PW_POLARITY_INVERTED`.

- **`autotune_search.ino` shim → `_shared/autotune_search_impl.h`**
  - `calibrate_DCO()` — search-based amp-comp loop: interpolated initial guess per note,
    sign-change detection with neighbour probing, ±1/±2 stepping clamped to per-note bounds,
    iteration/time/timeout guards.
  - `find_highest_freq()` / `find_lowest_freq()` — CLASSIC top-out measures frequency at
    fixed amp `DIV_COUNTER` via `find_freq_for_duty50`; lowest is fit/extrapolation to PWM 0.
  - `amp0_search_band()` / `amp0_prescan()` / `measure_lowest_freq_at_amp0()` /
    `apply_measured_lowest_freq()` — the shared amp-comp-0 endpoint: a wide band under the
    first measured pair (`kAmp0BandRatio`, floored at `kAmp0MinFreqHz`), a scan for two
    readings bracketing 50 % duty, a bounded search with amp comp fixed at 0, accepted only
    within `kEndpointAcceptDutyPct`.
  - `calibrate_DCO_freq_trace()` — the `FREQ_TRACE` builder: geometric amp grid from manual
    `A_L` through `DIV_COUNTER` (last rung), nearest-replace `ampComp440`, measure freq at
    duty 50% (ups from ~440 then lows); pair 0 amp0 floor 0.1 Hz; sentinel amp = `DIV_COUNTER`.
  - Interpolation helpers: `quadraticInterpolation`, `logarithmicInterpolation`,
    `linearInterpolation`, `expInterpolationSolveY()`.

  > The `PID_v1` library is still vendored in `_build_libs/` but is **not included anywhere in
  > the sketch** — the PID dependency was removed when `PID.ino` became `autotune_search.ino`.

---

## 6. Storage & State Persistence (LittleFS)

- **`FS.ino` — a one-line shim over `_shared/FS_impl.h`.** Format and invariants:
  [`../_shared/docs/CALIBRATION_STORAGE.md`](../_shared/docs/CALIBRATION_STORAGE.md).
  - **Six** flat little-endian banks, no header or version byte, index = oscillator (or PW
    channel). On this board: `voiceTables` 1408 B (8 osc × 22 `[freq_x100:u32][range_pwm:u32]`
    pairs), **`PWCal3Pt` 72 B** (4 channels × `kPWCalPoints` 3 × 6 B), **`AmpCompTopPair` 8 B**
    (`u8`/osc), `ManualOffset` 8 B (`i8`/osc), `AmpComp440` 16 B (`u16`/osc),
    `AmpCompDutyOffset` 16 B (`i16`/osc, 0.01 %).
    > 🔴 The three separate `PWCenter` / `PWHighLimit` / `PWLowLimit` files are **gone** —
    > consolidated into the single three-point `PWCal3Pt` bank, exposed at runtime as
    > `PW_CAL_LIMITS[NUM_PW_CHANNELS][kPWCalPoints]`. `AmpCompTopPair` is new.
    > See [`PRESET_STORE.md`](PRESET_STORE.md).
  - `init_FS()` — the only reader. Mounts LittleFS, runs the PW bank repair, creates missing
    banks, reads the leading `FS*BankSize` bytes (never the file's on-disk length) and unpacks
    into `ampCompArray` + `ampCompFrequencyHz` (float) or `ampCompFrequencyArray` (fixed),
    `PW_CENTER` / `PW_LOW_LIMIT` / `PW_HIGH_LIMIT`, `manualCalibrationOffset`, `ampComp440`,
    `ampCompDutyOffset`. Idempotent; every write path ends by calling it.
  - `update_FS_voice()` — rewrites one oscillator's 176 B slice from `calibrationData[]`.
  - `update_FS_PWCenter()` / `_PW_High_Limit()` / `_PW_Low_Limit()` — one `u16` at a **PW
    channel** index (`osc / 2`), bounds-checked, opened `"r+"` so the bank must already exist.
    The three functions survived the bank consolidation but now all write **different points
    inside the single `PWCal3Pt` file**, not three separate files.
  - `update_FS_AmpCompTopPair(oscIndex, value)` — the new per-oscillator top-valid-pair index.
  - `update_FS_ManualCalibrationOffset()` / `_AmpComp440()` / `_AmpCompDutyOffset()` —
    per-oscillator manual trims from `apply_param_manual_calibration_store()`.
  - `seed_fake_calibration_tables(force)` — plausible curve + PW defaults + `AmpComp440` =
    `DIV_COUNTER/10` so a virgin board boots and plays. `setup1()` calls it with `false`
    **before** `init_FS()`; debug command 30 forces it.
  - **Instrument-gated:** `ensure_pw_fs_banks()` behind `#if PROJECT_INSTRUMENT == 4` rewrites
    the three PW banks when any is missing or still the old 8-slot size. **Do not un-gate** —
    on DCO3 a legitimate 6 B bank would look stale and a measured PW center would be lost.
  - **Careful:** bank sizes are compile-time constants that `preset_bulk_commit()`,
    `dump_fs_file()` and the host `DCO-CONTROL-PANEL` model all derive independently.

- **`preset_store.h` / `preset_store.ino`** — full reference:
  [`PRESET_STORE.md`](PRESET_STORE.md).
  - **256-slot patch store**: 598-byte records packed 4 per LittleFS chunk file (`pb00`…`pb63`),
    CRC32-validated, plus `pstLast` for boot recall. Needs `flash=4194304_524288`.
  - Save captures the persistable-`'p'` shadow (`presetParamShadow[]` + set bitmap, filled by
    `preset_shadow_capture()` from `update_parameters()`) and the four block payloads read
    straight from their globals.
    ⚠️ Values written directly by a block handler (`'a'`–`'d'`) bypass `update_parameters()` and
    are therefore **not** shadow-captured.
  - Host side: `'p'` 170–173 (save / load / dump / cal dump), `'B'`/`'C'` bulk restore,
    structured CDC answer text (`[pdir]` / `[dump]` / `[preset]` / `[bulk]`).
  - **This board is the preset authority for the whole instrument.** Input has no filesystem —
    a RAM-only 256-entry name cache. `preset_store_send_directory_to_mb()` answers Input's `'N'`
    with 256 `'O'` frames (`[slot][name:16]`), and `serial_send_preset_loaded_to_mb()` sends
    `'L'` `[slot]` at the end of every load so the panel tracks the DCO's actual slot no matter
    who triggered it. Both directions pass through the Mainboard relay.
  - Recall paths: MIDI Program Change (+ Bank Select CC 0/32), `PARAM_PRESET_LOAD` (171), and
    the boot recall.
    ⚠️ **`REMEMBER_LAST_PRESET` is currently commented out**, so `setup()` compiles the `#else`
    branch and the board always boots into **`preset_store_load(4)`**.

---

## 7. MIDI, Serial Protocols and Parameter Updates

- **`midi.h` / `midi.ino`**
  - Adafruit TinyUSB MIDI + `MIDI.h`: `Adafruit_USBD_MIDI usb_midi` / `MIDI_USB`, and
    `MIDI_SERIAL` on the DIN port.
  - **DIN MIDI is raw `uart0`**, not an Arduino `Serial1` object: `uart_init(uart0, 31250)`,
    `gpio_set_function(0/1, GPIO_FUNC_UART)`, and an **exclusive IRQ**
    (`irq_set_exclusive_handler(UART0_IRQ, on_midi_uart_rx)`) feeding a lock-free SRAM ring.
  - `init_midi()` registers note on/off, CC, program change and pitch bend for both ports.
  - `handleControlChange()` uses CC 42 for pitch-bend range (recomputing
    `pitchBendMultiplier_q24`) and passes everything else to `midi_cc_handle()`.
  - **MIDI CC surface** (`midi_cc.h` + generated `midi_cc_map.h`, chart in
    [`MIDI_CC_MAP.md`](MIDI_CC_MAP.md)):
    - `midi_cc_handle()` finds the controller in `midiCcMap[]`, scales
      `lo + ((hi - lo) * cc + 63) / 127`, and runs envelope A/D/R through
      `linearToExponential(v, 50, 25000)` so a CC lands in the same exp domain the `'a'`–`'c'`
      blocks carry.
    - `midi_cc_apply()` dispatches: targets at or above `CC_LOCAL_FIRST` (224) are the ADSR /
      filter block values written straight to their globals and re-sent to the Mainboard;
      everything else — including PW (210) and EnvVCA→VCA (222) — goes to
      `update_parameters()`.
    - The map, the chart and the Open Stage Control session in `tools/panels/` are generated
      from `tools/dco_control/params.py` by `gen_midi_map.py`, which also verifies that each
      mapped `ParamId` is routed by `paramTable[]` and each `CC_LOCAL_*` has a case in
      `midi_cc_apply()`.
  - **Voice allocation lives here** (`note_on`, `note_off`, `voice_alloc`, `voice_mark_on/off/regate`,
    `mono_note_stack_clear`, `setVoiceMode`, `all_notes_off`) — not in `voices.ino`.
    - Note edges stay on the board (`noteStart[]` / `noteEnd[]` → envelopes on Core 0);
      nothing is sent over serial for notes beyond the `'n'` / `'o'` telemetry frames.
    - **Shared implementation**: the allocator and mono stack live in
      `DCO-SHARED-LIBRARIES/voice_alloc.h`, wrapped by `voice_alloc_state.h`, which sets
      `VOICE_ALLOC_SRAM_HOT 1` and declares `voiceAlloc` (`VoiceAllocator<NUM_VOICES_TOTAL>`)
      and `monoStack` (`MonoNoteStack<8>`). DCO3-MONOSYNTH compiles the same header.
    - **Allocation state** (owned by `voiceAlloc`, Core0-only writer): `VOICE_IDLE` /
      `VOICE_HELD` / `VOICE_RELEASING`, with a trigger stamp for age and a release stamp for
      the tail. `VOICES[]` and `VOICE_NOTES[]` stay in `globals.h` because the voice task and
      autotune read them. `alloc()` derives `RELEASING → IDLE` itself from
      `ADSR_VCA_Level_q15[]` rather than letting Core 1 write the state, so there is no window
      where Core 1 frees a slot Core 0 just took. Under `ENABLE_MB_MOD_STREAM` nothing
      refreshes those levels, so the tail is estimated from `ADSR_VCA_release` instead.
    - **Allocation mode** is one setting doing two jobs — poly steal policy and mono note
      priority: `0` round-robin / last, `1` oldest / first, `2` quietest / last, `3` quietest
      keep-lowest / low, `4` quietest keep-highest / high, `5` no stealing / first with denial.
      Every poly mode prefers an idle voice, then release tails, and steals a held note last;
      `5` drops the note-on.
    - **Mono (`voiceMode == 0`)** — held stack, depth 8, Core 0 only. NoteOn: `monoStack.push()`
      re-strikes to the top and returns `false` when mode `5` denies; `monoStack.pick()` picks
      the sounding pitch; unchanged → held silently, otherwise `voice_mark_on(0, …)`.
      NoteOff: `monoStack.remove()`; empty → `voice_mark_off(0)` (keep `VOICE_NOTES` for the
      release pitch); otherwise re-pick, and a changed winner calls `voiceAlloc.regate(0, …)`
      and pulses `note_on_flag[0]` for porta **without** setting `noteStart` (envelopes
      continue).
    - **Poly (`voiceMode == 1`)**: `voiceAlloc.findNote()` first (reuse the voice already on
      that pitch, including one in release), then `voice_alloc()`.
      **Stack (`voiceMode == 2`)**: same note on every slot.

- **`Serial.h` / `Serial.ino`** — how-to:
  [`README_serial_and_params.md`](README_serial_and_params.md).
  - Links: DIN MIDI raw `uart0` GP0 TX / GP1 RX @ 31 250; **`Serial2` RX 21 / TX 20 @ 2 500 000**
    to the STM32 Mainboard (FIFO 2048, polling off, DMA TX via
    `serial_dma_init_rp2040(0, uart1)`); USB CDC `Serial` @ 2 000 000. `Serial2` is the DCO's
    only peer link — the panel and Screen are reached through the Mainboard relay. There is no
    direct DCO ↔ Input wire.
  - **Two `SerialCommandDef[]` tables, nine rows each:** `mainboardSerialCommands[]` for
    `Serial2` (`'p'` `'m'` `'t'` `'a'` `'b'` `'c'` `'d'` `'q'` `'N'`) and
    **`usbSerialCommands[]`** for USB CDC (`'a'` `'b'` `'c'` `'d'` `'p'` `'q'` `'s'` `'B'`
    `'C'`). A command registered in only one is unreachable from the other transport.
  - Wire format: **slim little-endian, no finish byte**, `0x00` delimiter.
    `SERIAL_INNER_MAX_PAYLOAD = 40` in the shared `serial_frame.h` (the largest payload this
    board moves is the 36 B `'B'` chunk). Frame timeout
    **`SERIAL_FRAME_TIMEOUT_US = 4000`**; drain budget **`SERIAL_DRAIN_BYTE_BUDGET = 255`**.
    On-wire default is RAW; `#define SERIAL_FRAMING_COBS` wraps the same inner payloads as
    `COBS(inner)+0x00` (host: `dco_control --cobs`). Must match Input/Screen.
  - **Ingress tagging:** `PARAM_SRC_MAINBOARD = 0` / `PARAM_SRC_USB = 1`. There is **no
    `PARAM_SRC_INPUT`**. The single forwarding gate is `serial_forward_usb_edit_to_mb()`, which
    returns early unless the tag is `PARAM_SRC_USB`.
  - Outgoing helpers: `serialSendParam32()` (`'x'`, gap 154 / cal 155),
    `serialSendParam16()` / `serial_echo_persistable_param16()` (`'p'`),
    `serial_send_adsr_{vca,vcf,dco}_block_to_mb()` / `serial_send_filter_block_to_mb()`,
    `serial_send_preset_loaded_to_mb()` (`'L'`), `serial_send_screen_signal_to_mb()` (`'s'`),
    and the four patch-burst senders (`'v'` `'l'` `'M'` `'Q'`). `'c'` carries no CV — it goes
    out only so the Mainboard can relay EnvDCO to the panel faders and Screen.
  - `serial_panel_task()` / `serial_usb_task()` are the parser pumps, called from `loop()` on
    `timer1msFlag`. USB/DIN MIDI `.read()` runs every iteration with a byte budget.
  - **A new command byte also needs a row in the Mainboard's relay tables**
    (`inputSerial8Commands[]` / `mainSerial2Commands[]`), otherwise it is dropped in transit
    without an error at either end.

- **`params.ino` + shared `params_def.h` / `param_router.h`**
  - `params_def.h` is the **canonical superset**, byte-identical on every board of both
    projects (master `DCO3-MONOSYNTH/DCO/params_def.h`). Edit there, copy out, **never
    renumber**. Each board routes only its subset.
  - Routing is a **boot-built O(1) jump table**, not a table scan:
    `paramTable[]` → `param_router_build_jump()` (inside `init_param_router()`, called from
    `setup()`) → `dcoParamJump[256]` → `param_router_apply()` → `apply_param_*`.
    `update_parameters(uint8_t id, int16_t value)` then calls `preset_shadow_capture()`.
    **IDs must be ≤ 255** — anything higher is dropped silently.
  - ~90 appliers cover oscillator config, LFO settings, voice/stack mode, unison, drift,
    portamento (time vs slew), ADSR mods with precomputed Q24 scales, calibration flags, and
    **Character** (`PARAM_CHARACTER` 221, diagnostic axes on debug 0xC8–0xCB —
    [`CHARACTER.md`](CHARACTER.md)).
  - **Calibration gating:** while `calibrationFlag || calibrationVerifyRequested`, only ids 150
    and 160 pass unless `manualCalibrationFlag` is set, in which case only the ten
    `is_calibration_parameter()` ids do. A panel control that looks dead during calibration is
    being dropped here.

---

## 8. Timing, Utilities and Note Tables

- **`Timer_micros.h` / `Timer_micros.ino`** *(the files are `Timer_micros`, not
  `Timer_millis`)*
  - `init_micros_timers()` from both `setup`s; **`microsTimer()`** on Core 0 and
    **`microsTimer2()`** on Core 1, both every loop iteration.
  - They set the flag variables consumed throughout: `timer49microsFlag` (ADSR update),
    `timer51microsFlag` (drift), `timer99microsFlag` (PW in the fixed voice task),
    `timer1msFlag` (serial pumps), `timer5msFlag2` (ADSR parameter debounce), plus
    100 µs / 223 µs / 11 / 23 / 31 / 67 / 200 / 500 / 1000 ms flags and their `*2` Core-1 twins.
  - The profiler no longer hangs off `timer1000msFlag`: its periodic dump is timed on Core 0 in
    `bench_poll_core0()`, so nothing prints from the audio core.

- **`_shared/utils.h`** *(there is no `utils.ino`, and no board-local `utils.h`)*
  - `uintToStr()`; linear→log/exp mapping helpers (`linearToLogarithmic`,
    `linearToExponential` — the Input board's fader curve, reused by the MIDI CC layer — and
    `expConverter*`); `controls_formula_update()`; `led_blinking_task()`.

- **`_shared/noteList.h`**
  - MIDI note → frequency tables: named defines (e.g. `NOTE_A4 = 440.00`), the contiguous
    `sNotePitches[]` float array (float engine / helpers) and `sNotePitches_q24[]`, the Q24
    fixed-point twin (fixed engine).

---

## 9. USB, System Config and Metadata

- **`tusb_config.h`** — TinyUSB configuration for the RP2040/RP2350 USB stack.
- **`usb_descriptors.c`** — USB MIDI descriptors (much of it commented / legacy). Product
  identity is also set from `init_usb()`.
  ⚠️ **Still advertises `"DCO4-REBORN"` / `"DCO4"`.** Update if the instrument should enumerate
  as DCO6.
- **`irq_tuner.*`** — ❌ **does not exist in the tree.** Earlier revisions of this page listed
  it as excised experimental code; there is no such file anywhere.

---

## 10. External Libraries

**Vendored in `_build_libs/`** (via `--libraries ./_build_libs`):

- **`ADSR_Bezier`** — symlink to the monorepo-root repo, branch `main`; used by `adsr.*` (§4).
- **`mo-lfo`** — vendored from repo-root `mo-lfo/`; used by `LFO.*` (§4).
- **`DCO-PROTOCOL`** — the shared protocol library (§7).
- **`MIDI_Library`** — vendored copy, live.
- **`PID_v1`** — vendored but **not included by any sketch file**; a leftover from before
  `PID.ino` became `autotune_search.ino`.

**Other dependencies** (Arduino core / sketchbook): **Adafruit TinyUSB**, **LittleFS**.

---

## 11. Conventions

- `*.h` — declarations, constants, global state, struct/class definitions.
- `*.ino` — implementation files with function bodies. Arduino concatenates them
  **alphabetically**, which is why the `FS.ino` shim sorts before the autotune shims.
- Engine / IO / ADSR / LFO flags — catalog [`BUILD_FLAGS.md`](BUILD_FLAGS.md); math depth
  [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md). They live in **`settings.h`**, not the sketch.
- Shared serial/param headers — `params_def.h` is copied byte-for-byte between boards, not
  forked; `serial_input_protocol.h` shares command values and lengths but each copy is trimmed
  to that board's commands. Keep `ParamId` numbers stable.
- `_shared/…` = DCO-SHARED-LIBRARIES; `_build_libs/DCO-PROTOCOL/…` = the protocol library.
  Board-local files are the ones with no prefix.

---

### Summary

This firmware implements a **dual-core 4-voice × 2-oscillator DCO polysynth** (RP2040 /
RP2350-class, 8 physical oscillators) with:

- A compile-time **float or fixed-point** voice engine — **float is the default on RP2350**,
  **fixed on RP2040** — and matching amplitude-compensation paths. See
  [`BUILD_FLAGS.md`](BUILD_FLAGS.md) / [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md).
- A table-driven pitch path (portamento, LFOs, drift, ADSR, OSC2 interval + detune) feeding PIO
  clock dividers and RANGE/PW PWM.
- DCO and PW calibration by edge timing on a shared open-collector sense bus, persisted in
  LittleFS.
- The instrument's **only** preset store: 256 LittleFS slots, served to the rest of the system
  over the Mainboard relay.
- MIDI over USB and DIN, plus a 2.5 Mbps UART protocol to the Mainboard for parameters and UI.
- Clean separation between the hot voice loop on Core 1 and everything else on Core 0 —
  serial, MIDI, envelopes, LFOs, CV and all printing.

Use this reference to locate subsystems, understand data flow, and safely extend or optimize
specific parts of the DCO6 firmware.
