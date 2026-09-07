// #include <stdint.h>
// #include <stdio.h>
// #include <stdlib.h>
// #include <math.h>

#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
#include <stdint.h>
#include <math.h>

// ========================================================
// SETTINGS FILE !!!! CRITICAL
// ========================================================
#include "settings.h"

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

#include "cross_mod.h"
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