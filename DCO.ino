// #include <stdint.h>
// #include <stdio.h>
// #include <stdlib.h>
// #include <math.h>

/*  *** TO DO ***
- Fix PULSE PWN not received or updated when loading patches.
- Ask the AI to optimize and clean the autotune code. 
*/

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
#define CLKDIV_FLOAT 1
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

#define ENABLE_SWD_TELEMETRY

//#define RUNNING_AVERAGE

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

#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
#include <stdint.h>
#include <math.h>

#include "_shared/memory_port.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "hardware/irq.h"
#include "LittleFS.h"
#include "pico-dco.pio.h"


// 1. Protocol & Framework Libraries
#include "_build_libs/DCO-PROTOCOL/params_def.h"
#include "_build_libs/DCO-PROTOCOL/param_router.h"

#include "_build_libs/DCO-PROTOCOL/serial_param_protocol.h"
#include "_build_libs/DCO-PROTOCOL/serial_frame.h"
#include "_build_libs/DCO-PROTOCOL/serial_parser.h"

// 2. Base Configuration, Globals & Tables
#include "globals.h"
#include "_shared/FS.h"         // Provides chanLevelVoiceDataSize for amp_comp.h
#include "_shared/noteList.h"   // Provides sNotePitches for autotune.h

// 3. Amplitude Compensation (MUST be before bench.h!)
#include "_shared/amp_comp.h"

// 4. Autotune (MUST be before bench.h!)
#include "autotune.h"

// 5. Modulation & Drivers (MUST be before bench.h!)
#include "noise.h"
#include "_shared/character_jitter.h"
#include "LFO.h"
#include "adsr.h"

// 6. Profiling & Diagnostics (MUST be AFTER amp_comp, autotune, LFO, and adsr!)
#include "bench.h"
#include "mem_diag.h"

// 7. Subsystems, CV & Voice Pipeline (Everything else)

#include "mod_matrix.h"
#include "voice_alloc_state.h"
#include "wave_mux.h"
#include "preset_store.h"
#include "Serial.h"
#include "midi.h"
#include "midi_cc.h"
#include "midi_cc_map.h"
#include "PWM.h"
#include "voices.h"
#include "state_machines.h"
#include "_shared/utils.h"
#include "Timer_micros.h"

#include "cv_state.h"
#include "cv_out.h"


#if defined(BENCHMARKING_ENABLED) && defined(ENABLE_SWD_TELEMETRY)
extern "C" {
    volatile __attribute__((used)) PicoBenchTelemetry bench_telemetry = {};
    volatile __attribute__((used)) const char bench_meta_json[] = BENCH_META_JSON_STR;
    volatile uint32_t bench_acc[BENCH_COUNT] = {0};
    const uint8_t bench_probe_core[BENCH_COUNT] = {
    #define BENCH_X_CORE(id, core, kind, tier, parent, label) core,
      BENCH_PROBES(BENCH_X_CORE)
    #undef BENCH_X_CORE
    };
}
#endif
// ****************************************************************************************** //

// Core 0 boot: USB, UART serial, MIDI handlers, LFOs, calibration input pin.
void setup() {
  sys_clock_hz_refresh();  // Arduino already set clk_sys; cache real Hz for clkdiv
  // EEPROM.begin(512);
  #ifdef BENCHMARKING_ENABLED
  #if defined(ENABLE_SWD_TELEMETRY)
  volatile char _keep_json = bench_meta_json[0]; // Fools the linker into keeping the JSON!
  (void)_keep_json;
  #endif
    bench_init_core();  // SysTick is per core; core 1 arms its own in setup1()
  #endif
  init_micros_timers();
  init_usb();
  init_serial();
  init_param_router();
  init_midi();

  init_LFOs();
  init_DRIFT_LFOs();

  #ifdef REMEMBER_LAST_PRESET
  // One-shot recall of the last saved/loaded preset once both cores are up.
  preset_store_boot_task();
  // One chunk of a pending 'N' directory push, paced for the Mainboard relay.
  preset_store_dir_push_task();
#endif


  pinMode(DCO_calibration_pin, INPUT);
  pinMode(24, OUTPUT);
  digitalWrite(24, HIGH);
}

// Core 1 boot: LittleFS cal load, ADSR, amp-comp precompute, PWM/PIO, voices.
void setup1() {

  sys_clock_hz_refresh();  // Arduino already set clk_sys; cache real Hz for clkdiv

  #ifdef BENCHMARKING_ENABLED
    bench_init_core();
  #endif
  init_micros_timers();

  // Create voiceTables only if the file is missing (before init_FS stubs it).
  // Force overwrite: PARAM_DEBUG_COMMAND 30 / dco_control Calibration tab.
  seed_fake_calibration_tables(false);
  init_FS();
  preset_store_init_ram();

  init_ADSR();
  init_cv_out();
  mod_matrix_init();
  init_waveSelector();

  // Select amplitude-compensation precompute based on engine type.
  precompute_amp_comp_for_engine();

  precompute_pw_regions();

  calibrationFlag = false;
  manualCalibrationFlag = false;
  firstTuneFlag = false;

  init_pwm();
  init_pio();
#ifdef RANGE0_PIO_DITHER_TEST
  init_range_pio_dither();
#endif
#if NOISE_ENGINE == 0
  dcoNoisePioBegin(pio[NOISE_PIO], NOISE_SM);
#endif
  init_voices();
}

// Core 0 forever loop: MIDI every iter; Serial2 + USB CDC on 1 ms; ~50 µs LFO1 + LFO2 + drift.
// Core 0 forever loop: MIDI every iter; Serial2 + USB CDC on 1 ms; ~50 µs LFO1 + LFO2 + drift.
void SRAM_HOT(loop)() {
  BENCH_PERIOD(loop0_period);
  BENCH_SAMPLE_TICK();

  {
    BENCH_BEGIN(loop0_microsTimer);
    microsTimer();
    BENCH_END(loop0_microsTimer);
  }

  {
    BENCH_BEGIN(loop0_midi);

    // 1. USB MIDI (TinyUSB)
    {
      BENCH_BEGIN(loop0_midi_usb);
      if (TinyUSBDevice.mounted()) {
        uint8_t midi_budget = MIDI_DRAIN_BYTE_BUDGET;
        while (midi_budget > 0 && MIDI_USB.read()) {
          midi_budget--;
        }
      }
      BENCH_END(loop0_midi_usb);
    }

    // 2. Hardware DIN MIDI (Lock-Free SRAM Ring Buffer)
    {
      BENCH_BEGIN(loop0_midi_din);
      uint8_t midi_budget = MIDI_DRAIN_BYTE_BUDGET;
      while (midi_budget > 0 && MIDI_SERIAL.read()) {
        midi_budget--;
      }
      BENCH_END(loop0_midi_din);
    }

    BENCH_END(loop0_midi);
  }

  {
    BENCH_BEGIN(loop0_serial);
    if (timer1msFlag) {
      if (Serial2.available() > 0) {
        serial_panel_task();
      }
    }
    if (timer1msFlag) {
#ifdef ENABLE_USB_CONTROL
      serial_usb_task();
#endif

      preset_store_dir_push_task();
    }
    BENCH_END(loop0_serial);
  }


  if (timer5msFlag2 == 1) {
    BENCH_BEGIN(loop0_set_parameters);
    ADSR_set_parameters();
    BENCH_END(loop0_set_parameters);
  }

    BENCH_BEGIN(loop0_lfo1);
    LFO1();
    BENCH_END(loop0_lfo1);

    BENCH_BEGIN(loop0_lfo2);
    LFO2();
    BENCH_END(loop0_lfo2);




  if (timer51microsFlag == 1) {
    BENCH_BEGIN(loop0_drift);
    DRIFT_LFOs();
    BENCH_END(loop0_drift);
  }

  if (timer49microsFlag == 1) {
    BENCH_BEGIN(loop0_adsr);
    ADSR_update();
    BENCH_END(loop0_adsr);
  }

  {
    BENCH_BEGIN(loop0_noise);
    {
      #if NOISE_ENGINE == 0
      BENCH_BEGIN(loop0_noise_refill);
      dcoNoisePioRefill();
      BENCH_END(loop0_noise_refill);
      #endif
    }
    noiseLevel[0] = noise0.next();
    noiseLevel[1] = noise1.next();
    BENCH_END(loop0_noise);
  }

  BENCH_BEGIN(loop0_cv_outs);
  update_CV_outs();
  BENCH_END(loop0_cv_outs);

  // Snapshot core 0's probes and print once core 1 has handed its own over. All profiler
  // serial traffic happens here, never on the audio core.
  BENCH_BEGIN(loop0_housekeeping);
  bench_poll_core0();
  mb_bench_text_drain();
  mem_diag_poll_core0();
  BENCH_END(loop0_housekeeping);
}

// Core 1 forever loop: soft timers; auto/manual calibration OR ADSR + voice_task_main.
void SRAM_HOT(loop1)() {
  BENCH_PERIOD(loop1_period);
  BENCH_SAMPLE_TICK();

  {
    BENCH_BEGIN(loop1_microsTimer);
    microsTimer2();
    BENCH_END(loop1_microsTimer);
  }

  // ===============================================
  // CALIBRATION TRAP (Costs exactly 1 clock cycle to bypass during normal play)
  // ===============================================
  if (__builtin_expect(calibrationFlag || calibrationVerifyRequested, 0)) {
    autotune_loop_task(); 
    return; // EARLY EXIT: voice_task_main() is never reached while calibrating!
  }

  {
    BENCH_BEGIN(loop1_pio_defer);
    pio_defer_service();
    BENCH_END(loop1_pio_defer);
  }

  if (timer50microsFlag2 == 1) {
    // BENCH_BEGIN(loop1_cv_outs);
    // update_CV_outs();
    // BENCH_END(loop1_cv_outs);

    BENCH_BEGIN(loop1_adsr_sync);
    for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
      ADSR3Level_q15[i] = ADSR3Level_q15_volatile[i];
      ADSR_VCA_Level_q15[i] = ADSR_VCA_Level_q15_volatile[i];
      ADSR_VCF_Level_q15[i] = ADSR_VCF_Level_q15_volatile[i];
      ADSR_VCF2_Level_q15[i] = ADSR_VCF2_Level_q15_volatile[i];
    }
    BENCH_END(loop1_adsr_sync);
  }

  {
    BENCH_BEGIN(loop1_voice_task);
    voice_task_main();
    BENCH_END(loop1_voice_task);
  }

  // Hand this core's counters to core 0, which does all the printing.
  BENCH_BEGIN(loop1_housekeeping);
  bench_service(1);
  mem_diag_poll_core1();
  BENCH_END(loop1_housekeeping);
}