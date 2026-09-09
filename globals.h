#ifndef __GLOBALS_H__
#define __GLOBALS_H__

#include <cstddef>
#include <stdint.h>
#include <stdbool.h>
#include "project_config.h"


// Voice / oscillator counts (4 MIDI voices × 2 analog DCOs).
//   NUM_OSCILLATORS     — physical DCOs (RANGE/RESET/PIO, amp-comp, drift).
//   NUM_VOICES_TOTAL    — voice-slot capacity (MIDI/ADSR/flags/PW).
//   NUM_VOICES / STACK_VOICES (runtime below) — from setVoiceMode():
//     0 mono:  one MIDI voice → osc pair 0/1
//     1 poly:  4 independent 2-osc voices
//     2 stack: all 4 voices, same note
#define NUM_VOICES_TOTAL 4
#define NUM_OSCILLATORS 8
#define NUM_PW_CHANNELS NUM_VOICES_TOTAL
#ifndef NUM_FILTERS
#define NUM_FILTERS 2
#endif


#define MIDI_CHANNEL 1
//#define USE_ADC_STACK_VOICES // gpio 28 (adc 2)
//#define USE_ADC_DETUNE       // gpio 27 (adc 1)

// Unused SPI (kept commented — MOSI collided with PW_PINS[0]=3).
// #define SCK 2
// #define MOSI 3
// #define MISO 4
// #define CS 5

#define ENABLE_FS_CALIBRATION

// Arduino Tools → CPU Speed sets clk_sys (and F_CPU) before setup()/setup1().
// Cache actual Hz: clock_get_hz is not free — do not put it on the voice hot path.
// Call sys_clock_hz_refresh() once per core at boot before PIO/clkdiv runs.
uint32_t sysClock_Hz_cached = F_CPU;
float sysClock_Hz_cached_float = (float)F_CPU;
static inline void sys_clock_hz_refresh(void) {
  sysClock_Hz_cached = clock_get_hz(clk_sys);
  sysClock_Hz_cached_float = (float)sysClock_Hz_cached;
}
#define sysClock_Hz (sysClock_Hz_cached)
#define sysClock    (sysClock_Hz_cached / 1000u)

static constexpr uint16_t DIV_COUNTER = RANGE_PWM_WRAP;
static_assert(DIV_COUNTER >= 1, "RANGE_PWM_WRAP must be >= 1");
static constexpr uint16_t DIV_COUNTER_PW = PW_PWM_WRAP;

// Reset pulse width in system clock cycles (Y). Runtime-settable via
// PARAM_DEBUG_COMMAND 160 with value in [200, 50000] (dco_control Calibration).
uint32_t pioPulseLength = 12000;

// --- PIO Program Timing Constants ---
// `jmp x-- lp` executes X+1 times: it jumps while X is non-zero, then spends one
// more cycle falling through on X == 0. All five delay loops therefore cost one
// cycle beyond their count, which the previous T_LOW_OVERHEAD_CYCLES = 5 did not
// account for (it was short by exactly those five cycles).
//
// frequency_sync_4_jumps, one full period, offset-relative addresses:
//   a11 set pins,1        1
//   a0  lp0               Y + 1
//   a1  mov x, OSR        1
//   a2  set pins,0        1
//   a3  lp1               clk_div + 1
//   a4  mov x, OSR        1
//   a5  lp2               clk_div + 1
//   a6  mov x, OSR        1
//   a7  lp3               clk_div + 1
//   a8  mov x, OSR        1
//   a9  loop_final        clk_div + 1
//   a10 mov x, y          1
// => period = Y + 4*clk_div + 12
static constexpr uint32_t T_HIGH_OVERHEAD_CYCLES = 2;
static constexpr uint32_t T_LOW_OVERHEAD_CYCLES = 10;
static constexpr uint32_t NUM_OSR_CHUNKS = 4;

// Per-program period model: period = Y + RAMP_WEIGHT * clk_div + PERIOD_OVERHEAD.
// Soft-sync poll variants spend 2 cycles per iteration in each trailing polled
// chunk (jmp pin + jmp x--), so each such chunk counts twice toward the weight.
// Index by softSyncChunks (0 = free/hard). Aliases keep the N=1 names readable.
static constexpr uint32_t PIO_RAMP_WEIGHT_BY_CHUNKS[4] = { 4, 5, 6, 7 };
static constexpr uint32_t PIO_PERIOD_OVERHEAD_BY_CHUNKS[4] = { 12, 13, 14, 15 };
static constexpr uint32_t PIO_RAMP_WEIGHT_FREE = PIO_RAMP_WEIGHT_BY_CHUNKS[0];
static constexpr uint32_t PIO_PERIOD_OVERHEAD_FREE = PIO_PERIOD_OVERHEAD_BY_CHUNKS[0];
static constexpr uint32_t PIO_RAMP_WEIGHT_SYNC = PIO_RAMP_WEIGHT_BY_CHUNKS[1];
static constexpr uint32_t PIO_PERIOD_OVERHEAD_SYNC = PIO_PERIOD_OVERHEAD_BY_CHUNKS[1];

#define halfSysClock_Hz (sysClock_Hz / 2u)
#define eightSysClock_Hz_u (sysClock_Hz / 8u)
#define eightSysClockMinusPulseLength_Hz_u ((sysClock_Hz - pioPulseLength - 8u) / 8u)
#define T_HIGH_TOTAL_CYCLES (pioPulseLength + T_HIGH_OVERHEAD_CYCLES)
// Q24-scaled clock constants to avoid per-loop shifts
// (removed) Q24-scaled clock constants; direct shift used at call-site


uint32_t loop0_micros;
uint32_t loop1_micros;
uint32_t loop0_microsLast;
uint32_t loop1_microsLast;

volatile uint8_t NUM_VOICES = 1;
volatile uint8_t STACK_VOICES = 1;

volatile uint8_t voiceMode = 1;
volatile uint8_t syncMode = 0;
volatile uint8_t oscPhaseSync = 0;


// 0 to 32767 (Q15 format)
uint16_t crossmod_depth = 0;
volatile uint8_t crossmod_mode = 1;

bool pulseWaveOn = false;

// Voice allocation policy (PARAM_VOICE_ALLOC_MODE) lives on the voiceAlloc
// instance in voice_alloc_state.h; VoiceAllocMode comes from the shared library.

volatile uint16_t phaseAlignOSC2 = 0;

uint8_t unisonDetune = 0;
uint8_t analogDrift = 0;
uint8_t analogDriftSpeed = 0;
uint8_t analogDriftSpread = 0;

// Character / imperfection knobs (dco_control Character tab).
// DSP: character_jitter.h — pitch every voice_task; amp/PW ~10 kHz.
// Hot path uses precomputed char_*_scale_q15 (recomputed on knob change).
uint8_t character = 0;                    // PARAM_CHARACTER 0..128 (master scale)
uint8_t ampCompJitter = 0;                // diag 0..128 (PARAM_DEBUG_COMMAND 0xC8xx)
uint8_t pitchJitter = 0;                  // diag 0..128 (PARAM_DEBUG_COMMAND 0xCAxx)
uint8_t pulsewidthJitter = 0;             // diag 0..128 (PARAM_DEBUG_COMMAND 0xCBxx)
// noise_q15 * scale >> 15 → ±peak; 0 when Character / axis off.
int32_t char_pitch_scale_q15 = 0;
int32_t char_amp_scale_q15 = 0;
int32_t char_pw_scale_q15 = 0;

float BASE_NOTE = 440.0f;


// RESET/RANGE from DCO_MCU_BOARD in project_config.h. Osc 0/1 differ: WeAct
// breaks out GPIO 29; official Pico / Pico 2 use that pad as the VSYS ADC, so
// those two oscillators sit on GP28/26 and GP27/22. Osc 2–7 are identical.
// CV/mux in PINOUT.md is DCO3 leftover — not live.
// GP23/24 from DCO_MCU_BOARD: WeAct KEY + analog fix rail; Pico/Pico 2 SMPS PS.
//
// Hub UART:
//   Input UART  GP20 TX / GP21 RX
//   DIN MIDI    GP0/GP1 (HW UART0 interim; PIO UART later)
// SUB ×4 GPIOs TBD (`SUBOSC_PINS[]` still 0xFF). GP8 is OSC8 RESET — not sub-osc.
//
// CV PWM wrap is 4095 except Reso1 (GP7): shares slice 3 with RANGE OSC2 (GP22),
// so that channel scales duty into DIV_COUNTER (see write_cv_pwm).
#if defined(ENABLE_CV_OUTS) || defined(ENABLE_WAVE_MUX)
static constexpr uint8_t CUTOFF_PINS[NUM_FILTERS] = { 15, 4 };
static constexpr uint8_t RESO_PINS[NUM_FILTERS]   = { 5, 7 };
static constexpr uint8_t VCA_PIN                  = 11;
static constexpr uint8_t DIST_DRIVE_PIN           = 9;
static constexpr uint8_t DIST_MIX_PIN             = 26;
static constexpr uint8_t HC595_DATA_PIN           = 12;
static constexpr uint8_t HC595_LATCH_PIN          = 13;
static constexpr uint8_t HC595_CLK_PIN            = 14;
static constexpr uint8_t OSC1_LEVEL_PIN           = 16;
static constexpr uint8_t OSC2_LEVEL_PIN           = 18;
#ifdef ENABLE_VOICE_AUX
static constexpr uint8_t OSC3_LEVEL_PIN           = 9;   // Dist Drive pin freed on DCO
static constexpr uint8_t SUB_LEVEL_PIN            = 26;  // Dist Mix pin; slice 5 w/ VCA
#else
static constexpr uint8_t OSC3_LEVEL_PIN           = 32;  // RP2350B provisional
static constexpr uint8_t SUB_LEVEL_PIN            = 33;  // RP2350B provisional
#endif
static constexpr uint16_t DIV_COUNTER_CV          = 4095;
#endif

static constexpr uint8_t MCU_PIN_UNASSIGNED = 0xFF;

#if DCO_MCU_BOARD == DCO_MCU_WEACT_RP2040 
static constexpr uint8_t RESET_PINS[NUM_OSCILLATORS] = { 29, 27, 19, 18, 15, 13, 12, 8 };
static constexpr uint8_t RANGE_PINS[NUM_OSCILLATORS] = { 28, 22, 17, 16, 14, 11, 9, 7 };
static constexpr uint8_t SMPS_PS_PIN = MCU_PIN_UNASSIGNED;  // GP23 is the onboard KEY
static constexpr uint8_t USER_KEY_PIN = 23;                 // active-low, INPUT_PULLUP
static constexpr uint8_t BOARD_FIX_PIN = 24;                // analog carrier rail
#elif (DCO_MCU_BOARD == DCO_MCU_PICO) || (DCO_MCU_BOARD == DCO_MCU_PICO2) || (DCO_MCU_BOARD == DCO_MCU_WEACT_RP2350)
static constexpr uint8_t RESET_PINS[NUM_OSCILLATORS] = { 28, 27, 19, 18, 15, 13, 12, 8 };
static constexpr uint8_t RANGE_PINS[NUM_OSCILLATORS] = { 26, 22, 17, 16, 14, 11, 9, 7 };
static constexpr uint8_t SMPS_PS_PIN = 23;                  // RT6150 PS: drive HIGH
static constexpr uint8_t USER_KEY_PIN = MCU_PIN_UNASSIGNED;
static constexpr uint8_t BOARD_FIX_PIN = MCU_PIN_UNASSIGNED;  // GP24 is VBUS sense
#else
#error "DCO_MCU_BOARD must be DCO_MCU_WEACT_RP2040, DCO_MCU_PICO, or DCO_MCU_PICO2"
#endif

// Freq SMs: voices 0–1 on pio0 (osc 0–3), voices 2–3 on pio1 (osc 4–7).
// A voice pair always shares a PIO block so hard-sync sideset can share RESET.
static constexpr uint8_t VOICE_TO_PIO[NUM_OSCILLATORS] = { 0, 0, 0, 0, 1, 1, 1, 1 };

// Mutable local SM index within each PIO block. Slave always takes the lower SM
// so the master outranks it on a same-cycle sideset tie. Rewritten by
// assign_sm_mapping() whenever syncMode changes.
uint8_t VOICE_TO_SM[NUM_OSCILLATORS] = { 0, 1, 2, 3, 0, 1, 2, 3 };

// Noise PIO LFSR is not used (CPU DCO_Noise only). Constants kept for dcoNoisePioBegin no-op.
static constexpr uint8_t NOISE_PIO = 1;
static constexpr uint8_t NOISE_SM = 1;
static constexpr uint8_t NOISE_OUT_PIN = 2;

// Pulse-width PWM: one pin per MIDI voice (old DCO4). 0xFF = not wired.
static constexpr uint8_t PW_PIN_UNASSIGNED = 0xFF;
static constexpr uint8_t PW_PINS[NUM_PW_CHANNELS] = { 3, 2, 4, 5 };

extern bool pulseWaveOn;

// RP2350: one sub-osc SM per voice on pio2. Pinout later (0xFF = skip gpio init).
static constexpr uint8_t SUBOSC_PIN_UNASSIGNED = 0xFF;
#if defined(PICO_RP2350)
static constexpr uint8_t SUBOSC_PIO = 2;
static constexpr uint8_t SUBOSC_PINS[NUM_VOICES_TOTAL] = {
  SUBOSC_PIN_UNASSIGNED, SUBOSC_PIN_UNASSIGNED, SUBOSC_PIN_UNASSIGNED, SUBOSC_PIN_UNASSIGNED
};
#endif

// Old DCO4 cal sense. GP6 is free (later SUB candidate). GP25 is Pico LED — not cal.
static constexpr int DCO_calibration_pin = 10;

uint8_t RANGE_PWM_SLICES[NUM_OSCILLATORS];
uint8_t RANGE_PWM_CHANNELS[NUM_OSCILLATORS];
uint16_t RANGE_PWM[NUM_OSCILLATORS];
uint8_t VCO_PWM_SLICES[NUM_OSCILLATORS];
uint8_t PW_PWM_SLICES[NUM_PW_CHANNELS];
uint8_t PW_PWM_CHANNELS[NUM_PW_CHANNELS];

#ifdef ENABLE_CV_OUTS
uint8_t CUTOFF_PWM_SLICES[NUM_FILTERS];
uint8_t CUTOFF_PWM_CHANS[NUM_FILTERS];
uint8_t RESO_PWM_SLICES[NUM_FILTERS];
uint8_t RESO_PWM_CHANS[NUM_FILTERS];
uint8_t VCA_PWM_SLICE;
uint8_t VCA_PWM_CHAN;
#ifndef ENABLE_VOICE_AUX
uint8_t DIST_DRIVE_PWM_SLICE;
uint8_t DIST_DRIVE_PWM_CHAN;
uint8_t DIST_MIX_PWM_SLICE;
uint8_t DIST_MIX_PWM_CHAN;
#endif
uint8_t OSC1_LEVEL_PWM_SLICE;
uint8_t OSC1_LEVEL_PWM_CHAN;
uint8_t OSC2_LEVEL_PWM_SLICE;
uint8_t OSC2_LEVEL_PWM_CHAN;
uint8_t OSC3_LEVEL_PWM_SLICE;
uint8_t OSC3_LEVEL_PWM_CHAN;
uint8_t SUB_LEVEL_PWM_SLICE;
uint8_t SUB_LEVEL_PWM_CHAN;
#endif

// Board-specific PW center seeds (fake-seed / bank rebuild defaults).
static constexpr uint16_t kPwCenterDefault[NUM_PW_CHANNELS] = { DIV_COUNTER_PW / 2 };

uint16_t PW_CENTER[NUM_PW_CHANNELS] = { DIV_COUNTER_PW / 2 };
uint16_t PW_LOW_LIMIT[NUM_PW_CHANNELS] = { 0 };
uint16_t PW_HIGH_LIMIT[NUM_PW_CHANNELS] = { DIV_COUNTER_PW };
uint16_t PW_PWM[NUM_PW_CHANNELS];

// Gate flag: 1 while the key is down, 0 from note-off onwards. The allocator in
// voice_alloc_state.h carries the finer distinction it needs on top of this (a
// released voice is still audible); voice_mark_on/off keep the two in step.
volatile uint32_t VOICES[NUM_VOICES_TOTAL];
volatile uint8_t VOICE_NOTES[NUM_VOICES_TOTAL];

uint32_t LED_BLINK_START = 0;

#if defined(PICO_RP2350)
PIO pio[3] = { pio0, pio1, pio2 };
#else
PIO pio[2] = { pio0, pio1 };
#endif

uint8_t VOICE_RAW_NOTE[NUM_VOICES_TOTAL]; // Tracks the untouched MIDI key
volatile uint8_t VOICE_NOTE_OSC1[NUM_VOICES_TOTAL] = {0};
volatile uint8_t VOICE_NOTE_OSC2[NUM_VOICES_TOTAL] = {0};
static uint8_t VOICE_MIDI_NOTE[NUM_VOICES_TOTAL];

uint8_t midi_serial_status = 0;
int midi_pitch_bend = 8192, last_midi_pitch_bend = 8192;
uint8_t midi_aftertouch = 0;
uint8_t midi_expression = 0;
uint8_t midi_breath = 0;
uint8_t midi_mod_wheel = 0;
uint8_t midi_sustain = 0;
uint8_t midi_channel = 1;
uint8_t pitchBendRange = 2;
// MIDI Bank Select (CC 0 / CC 32): 0 = slots 0..127, 1 = slots 128..255.
uint8_t midiPresetBank = 0;

// Precompute 1/12 in Q24 for fast multiplier calculation
static constexpr int32_t RECIP_TWELVE_Q24 = (int32_t)((1.0f / 12.0f) * (float)(1 << 24));
// Precompute 1/360 in Q24 for fast phaseDelay calculation (full 0–360° range)
static constexpr uint32_t RECIP_360_Q24 = (uint32_t)(((1ULL << 24) + 180) / 360);
float pitchBendMultiplier = 1.00f / 12.00f * (float)pitchBendRange;
int32_t pitchBendMultiplier_q24 = 1 << 24;

uint16_t raw;

void init_sm(PIO pio, uint sm, uint offset, uint pin);
void set_frequency(PIO pio, uint sm, float freq);
float get_freq_from_midi_note(uint8_t note);
void led_blinking_task();
uint8_t voice_alloc();

void usb_midi_task();
void serial_midi_task();
void note_on(uint8_t note, uint8_t velocity);
void note_off(uint8_t note);

void adc_task();

// Free-running program + one soft-sync poll image per freq PIO block (pio0 and pio1).
uint32_t pio_offset_free[2] = { 0, 0 };
uint32_t pio_offset_sync[2] = { 0, 0 };
uint8_t pio_loaded_sync_chunks[2] = { 0, 0 };  // 0 = none; else 1..3
uint32_t subosc_offset_div2 = 0;
uint32_t subosc_offset_div4 = 0;

// Per-oscillator PIO state: which resident program the SM runs, and the reset
// pulse width (Y) last pushed to it. Y is only rewritten at note-on — see
// pio_period_split() for why it cannot safely be updated every control frame.
bool osc_uses_sync_program[NUM_OSCILLATORS] = {
  false, false, false, false, false, false, false, false
};
uint32_t osc_last_y[NUM_OSCILLATORS] = {
  pioPulseLength, pioPulseLength, pioPulseLength, pioPulseLength,
  pioPulseLength, pioPulseLength, pioPulseLength, pioPulseLength
};



uint8_t dataArray[4];

float LFOMultiplier = 1;
float voiceFreq[NUM_OSCILLATORS];
uint16_t dato_serial;
float dato_serial_float;
// Global octave offset for note math: table_index = midi - 36 + octave_shift (36 ⇒ unison).
// Wire param remains PARAM_OSC1_INTERVAL (13) / CC 2.
volatile uint8_t octave_shift = 24;
uint8_t OSC2_serial_detune = 127;
volatile uint8_t OSC2_interval = 36;
volatile uint8_t OSC3_interval = 36;

uint16_t masterTuning = 0;
uint16_t OSC1_detune = 127;
uint16_t OSC2_detune = 127;
uint16_t OSC3_detune = 127;

#if defined(USE_FLOAT_VOICE_TASK)
float masterTuning_f = 0.0f;
float OSC1_detune_f = 0.0f;
float OSC2_detune_f = 0.0f;
#else
int32_t masterTuning_q24 = 0;
int32_t OSC1_detune_q24 = 0;
int32_t OSC2_detune_q24 = 0;
#endif

bool PWMPotsControlManual;

uint16_t PW[NUM_PW_CHANNELS];  // panel / mod PW target per voice

void serial_panel_task();
float get_chan_level(float freq_to_amp_comp);

volatile uint8_t note_on_flag[NUM_VOICES_TOTAL];

bool ledstat = false;

#endif
