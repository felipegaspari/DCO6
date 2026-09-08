// =============================================================================
// ENGINE — pitch mode ids (needed by board defaults + overrides below)
// =============================================================================
// Deep detail: docs/ENGINE_OPTIONS.md
//   0 FLOAT (updated no docs)
//   1 RATIO_Q16 (slopeQ16 + fused y→ratio; fixed default / float A/B)
//   2 Q12 (slope A/B: IntQ16 y + reciprocal; float A/B OK)
//   3 FLOAT_CACHED (trunc+clamp±1 find; same float tables; needs float voice)
#define PITCH_INTERP_FLOAT 0 /// default for rp2350, much faster than cached
#define PITCH_INTERP_RATIO_Q16 1
#define PITCH_INTERP_Q12 2
#define PITCH_INTERP_FLOAT_CACHED 3

// Clkdiv methods (CLKDIV_MODE). Accuracy order. Fixed: Q24 via clkdiv_live_total_cycles.
// Float: Hz via clkdiv_live_hz_total_cycles (Q16/Q8/FAST_Q4 convert Hz→Q24).
//   0 GOLD     — double llround(sys / Hz) from Q24 (gold standard / A/B)
//   1 FLOAT    — Q24 → float Hz → fminf(sys/hz + 0.5) (float-engine math)
//   2 Q16      — Q16 Hz → 64/32 (shipping)
//   3 Q8       — Q8 Hz → 32/32 + remainder; <16 Hz → internal precise Q8
//   4 FAST_Q4  — Q4 Hz → 32/32 (fastest, least accurate)
// Value 0 is GOLD, not the old HP0 Q4 path (that is FAST_Q4 = 4).
// Value 1 is FLOAT, not the old PRECISE_Q8 path (Q8 is 3).
#define CLKDIV_GOLD 0
#define CLKDIV_FLOAT 1 // RP2350 default
#define CLKDIV_Q16 2
#define CLKDIV_Q8 3
#define CLKDIV_FAST_Q4 4

// =============================================================================
// ENGINE — board defaults (Arduino core: PICO_RP2350 / else)
// =============================================================================
#if defined(PICO_RP2350)
// RP2350 has an FPU: float voice + float amp-comp dual-build (LUT + Q8 for A/B).
#ifndef USE_FLOAT_VOICE_TASK
#define USE_FLOAT_VOICE_TASK
#endif
#ifndef PITCH_INTERP_MODE
#define PITCH_INTERP_MODE PITCH_INTERP_FLOAT
#endif
#ifndef USE_FLOAT_AMP_COMP
#define USE_FLOAT_AMP_COMP
#endif
#ifndef AMP_COMP_METHOD_DEFAULT
#define AMP_COMP_METHOD_DEFAULT 1  // FLOAT_QUAD (0); LUT=1, FIXED=2 — cmds 20–22
#endif
#ifndef CLKDIV_MODE
#define CLKDIV_MODE CLKDIV_FLOAT  // native Hz on float voice
#endif
#ifndef USE_FLOAT_CV_OUTS
#define USE_FLOAT_CV_OUTS
#endif
#ifndef USE_MOD_MATRIX_FLOAT_ENGINE
#define USE_MOD_MATRIX_FLOAT_ENGINE
#endif
#else
// RP2040 / fallback: fixed voice + lean Q8 amp (no float amp tables / LUT RAM).
// CV outs stay fixed-point (no USE_FLOAT_CV_OUTS) — soft-float would choke Core1.
#ifndef AMP_COMP_METHOD_DEFAULT
#define AMP_COMP_METHOD_DEFAULT 2  // FIXED
#endif
#ifndef PITCH_INTERP_MODE
#define PITCH_INTERP_MODE PITCH_INTERP_RATIO_Q16
#endif
#ifndef CLKDIV_MODE
#define CLKDIV_MODE CLKDIV_Q16  // Q16 64/32
#endif
#endif

// Q24 voice task -- experimental
// #define USE_VOICE_TASK_Q24

// Note-on sync retrigger (oscPhaseSync >= 1): 0 = EXACT_Y (Y load + phase hold), 1 = SYNC_JMP
// (restart jmp only; degree offsets need EXACT_Y). Runtime: cmds 26/27.
#ifndef NOTE_RETRIG_MODE_DEFAULT
#define NOTE_RETRIG_MODE_DEFAULT 0
#endif

// Uncomment to enable instant PIO divider update (helps not to have note transition clicks, but creates clkdiv x delays and jitter when modulation is too fast)
#define UPDATE_CLK_DIV_INSTANTLY

// =============================================================================
// ENGINE — overrides (uncomment to force; after board defaults)
// =============================================================================
// #define USE_FLOAT_VOICE_TASK         // float voice (needs FPU; soft-float on RP2040)
// #define USE_FLOAT_AMP_COMP           // float amp dual-build (large RAM)
// #define USE_FLOAT_CV_OUTS            // float VCA/VCF path (soft-float tax on RP2040)
// #define CLKDIV_MODE CLKDIV_FAST_Q4   // Q4 32/32
// #define CLKDIV_MODE CLKDIV_Q8        // Q8 32/32+corr (faster than Q16)
// #define CLKDIV_MODE CLKDIV_Q16       // Q16 64/32 (shipping)
// #define CLKDIV_MODE CLKDIV_GOLD      // double llround; gold standard / A/B
// #define CLKDIV_MODE CLKDIV_FLOAT     // Q24 → float Hz (same math as float voice)
// #define AMP_COMP_METHOD_DEFAULT 1    // 0 FLOAT_QUAD / 1 LUT / 2 FIXED; needs USE_FLOAT_AMP_COMP for 0/1
// Pitch A/B (ids above; default already set — #undef then redefine):
// #undef PITCH_INTERP_MODE
// #define PITCH_INTERP_MODE PITCH_INTERP_FLOAT       // walk find A/B (needs float voice)
// #define PITCH_INTERP_MODE PITCH_INTERP_FLOAT_CACHED  // trunc+clamp±1 (needs float voice)
// #define PITCH_INTERP_MODE PITCH_INTERP_RATIO_Q16   // shipping default both MCUs
// #define PITCH_INTERP_MODE PITCH_INTERP_Q12

// =============================================================================
// ENGINE — guards
// =============================================================================
#if (PITCH_INTERP_MODE == PITCH_INTERP_FLOAT || PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_CACHED) && !defined(USE_FLOAT_VOICE_TASK)
#error "PITCH_INTERP_FLOAT / FLOAT_FAST require USE_FLOAT_VOICE_TASK (board default or override)"
#endif
#if CLKDIV_MODE > 4
#error "CLKDIV_MODE must be CLKDIV_GOLD, FLOAT, Q16, Q8, or FAST_Q4"
#endif

// =============================================================================
// ENGINE — noise (see noise.h)
// =============================================================================
// NOISE_ENGINE — which DCO_Noise class noise0..1 use (see noise.h):
//   0 ColoredNoise     — Voss pink / 1-pole brown / white; whites from PioNoiseWhite
//   1 FastNoiseGen     — economy Voss pink / leaky brown / local xorshift white
//   2 PrimeHybridNoise — per-gen prime tables (997/1499/1999); dither + rephase
//   3 ProNoise32       — Q16.15 Kellett pink / DC-corrected brown / xorshift white
// Objects: noise0..1 in noise.h (ctor sets min/max/color/seed); next() in loop1.
// PIO white: dcoNoisePioBegin / dcoNoisePioRefill (library reads these flags).
// Bench: parent noise_gens + "noise refill".
#define NOISE_ENGINE 2
// #undef NOISE_ENGINE
// #define NOISE_ENGINE 0
// #define NOISE_ENGINE 1
// #define NOISE_ENGINE 2
// #define NOISE_ENGINE 3

// ENABLE_NOISE_OUT — PIO1 LFSR 1-bit white on GP2 (listen/scope). Comment out to
// free the pin. Engine 0 still uses LFSR FIFO seed (no GPIO). Engines 1/2/3 with
// this off skip PIO noise MMIO (clean benches). Library reads this flag.
//#define ENABLE_NOISE_OUT

// =============================================================================
// CALIBRATION — auto-cal boot defaults (runtime: Calibration tab debug cmds)
// =============================================================================
// Amp-comp method: 0 CLASSIC (per-note range-PWM search), 1 FREQ_TRACE
// (fixed-PWM frequency bisection; needs the manual 440 Hz anchor). Cmds 34/35.
#ifndef AUTOTUNE_AMP_METHOD_DEFAULT
#define AUTOTUNE_AMP_METHOD_DEFAULT 1
#endif
// Frequency-search close-in: 0 BISECT, 1 INTERP, 2 GATED. Cmds 37/38/39.
#ifndef AUTOTUNE_SEARCH_MODE_DEFAULT
#define AUTOTUNE_SEARCH_MODE_DEFAULT 1
#endif
// Amp-comp-0 endpoint (pair 0): 0 MEASURE (live hunt), 1 CALC (bottom-rung fit).
// Cmds 40/41.
#ifndef AUTOTUNE_AMP0_MODE_DEFAULT
#define AUTOTUNE_AMP0_MODE_DEFAULT 1
#endif
// Overrides (uncomment to force; #undef first):
// #undef AUTOTUNE_AMP_METHOD_DEFAULT
// #define AUTOTUNE_AMP_METHOD_DEFAULT 0   // CLASSIC
// #define AUTOTUNE_AMP_METHOD_DEFAULT 1   // FREQ_TRACE
// #undef AUTOTUNE_SEARCH_MODE_DEFAULT
// #define AUTOTUNE_SEARCH_MODE_DEFAULT 0  // BISECT
// #define AUTOTUNE_SEARCH_MODE_DEFAULT 1  // INTERP
// #define AUTOTUNE_SEARCH_MODE_DEFAULT 2  // GATED
// #undef AUTOTUNE_AMP0_MODE_DEFAULT
// #define AUTOTUNE_AMP0_MODE_DEFAULT 0    // MEASURE
// #define AUTOTUNE_AMP0_MODE_DEFAULT 1    // CALC

// =============================================================================
// PROFILING / BENCH (see docs/BENCHMARKING.md)
// =============================================================================
// RUNNING_AVERAGE: hot-path profiler in bench.h (count/mean/min/max/total + core share).
// Off = zero cost. Needed for paced bench_out_* TX (profiler dump, amp/pitch benches).
// RUNNING_AVERAGE_FINE: also probes tiny stages; every probe is an opt barrier — changes
// codegen; for measuring that distortion, not for leaving on.
// RUNNING_AVERAGE_PERIOD: only loop/loop1 BENCH_PERIOD; stage probes compile out.
// Overrides FINE. Needs RUNNING_AVERAGE.
// BENCH_PATH_STATS: all path bumps (amp/ratio/porta + walk-step sums) and dump
// `-- Path counters --`. Needs RUNNING_AVERAGE; still no-op under PERIOD. Leave off for shipping.
// BENCH_STAGE_STRIDE: MAIN/FINE stage probes every Nth loop (default 9). 1 = every iter.
// BENCH_PERIOD is always every iter (speed truth). Note-on family always records.
// BENCH_USE_SYSTICK: 1 = SysTick for PERIOD + stages; 0 = 1 us timer for all probes.
// Dump window (1 s gate) always uses bench_us_now(). BENCH_PERIOD_MAX_US: discard PERIOD
// samples longer than this (autotune / wrap-looking stalls).
// ENABLE_SWD_TELEMETRY : Enable SWD telemetry for PlotJuggler

#define BENCHMARKING_ENABLED


#ifdef BENCHMARKING_ENABLED

//#define ENABLE_SWD_TELEMETRY

#define RUNNING_AVERAGE

#if defined(ENABLE_SWD_TELEMETRY)
  // SWD telemetry enabled, use its specific functions.
  #define ENABLE_SWD_PERIOD 

#elif defined(RUNNING_AVERAGE)
  // Basic running average sampling enabled.
  // ENABLE_MEM_DIAG: SRAM/heap dump (cmd 13) + loop/loop1 polls. Default on.
// Comment out for a zero-cost match to pre-mem_diag period-only dumps.
// Runtime 14/15 disable/enable polls without rebuild (dump 13 ignored while off).
//#define ENABLE_MEM_DIAG
#define RUNNING_AVERAGE_PERIOD
#if defined(RUNNING_AVERAGE_PERIOD)
  // Only period probes active, no fine-grained sampling.
#elif defined(RUNNING_AVERAGE_FINE)
  // Fine-grained sampling enabled.
#endif
#else
  // BENCHMARKING_ENABLED is defined, but no specific mode selected.

#endif

// #define BENCH_PATH_STATS

#ifndef BENCH_STAGE_STRIDE
#define BENCH_STAGE_STRIDE 1
#endif
#ifndef BENCH_USE_SYSTICK
#define BENCH_USE_SYSTICK 1
#endif
#ifndef BENCH_PERIOD_MAX_US
#define BENCH_PERIOD_MAX_US 20000
#endif


// Amp-comp speed/accuracy reports (debug cmds 24–25); needs RUNNING_AVERAGE + USE_FLOAT_AMP_COMP.
//#define AMP_COMP_BENCHMARK

#ifdef AMP_COMP_BENCHMARK
#define USE_FLOAT_AMP_COMP
#endif
#endif 

// =============================================================================
// BOARD / IO
// =============================================================================
// Classic PCB: Serial2 GP20/21 peers with STM32 Mainboard (not Input).
// TX 'n'/'o'/'e'/'x'/'p'; RX slim 'p' (+ 'm' if ENABLE_MB_MOD_STREAM). Input talks to Mainboard.
#define ENABLE_MAINBOARD_LINK
// Opt-in: consume Mainboard 'm' and skip local LFO1/2 + EnvDCO clocks.
// Default off: DCO runs LFO1/2, all envelopes, and matrix→pitch locally.
// #define ENABLE_MB_MOD_STREAM

// Accept slim panel protocol on USB CDC too (tools/dco_control). Comment out for
// production: stray terminal bytes are read as frame headers while enabled.
#define ENABLE_USB_CONTROL

// #define SERIAL_FRAMING_COBS  // A/B vs default RAW; host: dco_control --cobs

////////////////////////////////////////////////
// DMA implementation -- uncomment to enable
#define DCO_PROTOCOL_IMPLEMENT_DMA
////////////////////////////////////////////////

// All RANGE pins via dithered PIO PWM (3-frame, period = DIV_COUNTER/3). Off for 8 oscs:
// dither needs one SM per RANGE pin and there are none spare. HW PWM slices instead.
// #define RANGE0_PIO_DITHER_TEST

// Phase 3 CV hardware (provisional pins in globals.h / docs/PINOUT.md).
// Leave commented on benches without filter/VCA/mux/DAC attached.
// #define ENABLE_CV_OUTS
// #define ENABLE_WAVE_MUX

// Dual-MCU: RP2040 voice-aux owns Dist Drive/Mix PWM + filter mode GPIO (later FX).
// Keep apply handlers/state; skip local pin writers so they do not fight the aux.
// Leave commented for solo RP2350B / single-MCU (full local IO). See docs/DUAL_MCU.md.
// #define ENABLE_VOICE_AUX

// Oscillator RESET pad polarity. Uncomment when discharge is through an active-low
// switch (e.g. DG411: IN low = on). PIO still uses logical 1 = assert / discharge;
// GPIO OUTOVER+INOVER invert the pad so soft sync jmp_pin and sub-osc wait keep
// working. Leave commented for active-high / direct FET discharge. See PIO_OSCILLATORS.md.
// DCO3 (DG411) defines this; DCO4 (active-high / FET) does not.
#include "project_config.h"
#if PROJECT_INSTRUMENT == 3
#define ENABLE_PIO_RESET_INVERT
#endif

//#define ENABLE_PIO_RESET_INVERT
// =============================================================================
// CALIBRATION — auto-cal boot defaults
// =============================================================================
// PW Sweep Mode: 0 FULL (DCO4: 2%..98%), 1 HALF_HIGH (DCO3: 50%..98%), 2
// HALF_LOW (DCO3: 2%..50%)
#ifndef PW_SWEEP_MODE_DEFAULT
#if PROJECT_INSTRUMENT == 4
#define PW_SWEEP_MODE_DEFAULT 0 // DCO4: FULL (default)
#else
#define PW_SWEEP_MODE_DEFAULT 2 // DCO3: HALF_LOW (default)
#endif
#endif

// PW Polarity Inversion: 0 NOT INVERTED, 1 INVERTED
#ifndef PW_POLARITY_INVERTED
#define PW_POLARITY_INVERTED 1
#endif
// To manually override without PROJECT_INSTRUMENT:
// #undef PW_SWEEP_MODE_DEFAULT
// #define PW_SWEEP_MODE_DEFAULT PW_SWEEP_HALF_HIGH

// Debug level for autotune
#define AUTOTUNE_DEBUG_LEVEL 1

// =======================================================================
// PRESETS OPTIONS
// =======================================================================

// REMEMBER_LAST_PRESET   if defined, writes in flash the number of the last preset to restore at boot
// #define REMEMBER_LAST_PRESET


////////////////////////////////////////////////
//==========================
// MOVE THINGS TO RAM TO MAKE IT FASTER:
#define SRAM_HOT_ENABLE 1
#define SRAM_DATA_ENABLE 1
//============================================