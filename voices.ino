#include "clkdiv.h"
#include "globals.h"
#include "include_all.h"
#include <limits.h>
#include <math.h>
#include "hardware/sync.h"
#include "hardware/structs/pio.h" // Required for direct MMIO struct access

// Enable/disable detailed DCO debug report (including OSC1 frequency stages)
#define DCO_DEBUG_REPORT 0

void SRAM_HOT(amp_chan_levels_fixed)(int64_t freq_q24_A, int64_t freq_q24_B,
                                  uint8_t oscA, uint8_t oscB, uint16_t *outA,
                                  uint16_t *outB);

#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT
float SRAM_HOT(interpolateRatioFloat_fast)(float x);
#endif
#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
int32_t SRAM_HOT(interpolateRatioQ16_fast)(int32_t xQ16);
#endif
#if PITCH_INTERP_MODE == PITCH_INTERP_Q12
int32_t SRAM_HOT(interpolatePitchMultiplierIntQ16_cached)(int32_t xQ16, int dcoIndex);
#endif
#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_CACHED
float SRAM_HOT(interpolateRatioFloat_cached_fast)(float x, int dcoIndex);
#endif

// Live pitch interp: compile-time wrappers (always_inline; not function
// pointers). Fixed-voice wrappers only — float voice uses
// interpolate_live_ratio_f (FLOAT_FAST would otherwise type-check the Q12 #else
// and fail: IntQ16 is not compiled).
#ifndef USE_FLOAT_VOICE_TASK
int32_t SRAM_HOT(modifiers_q24_to_xQ16)(int64_t modifiers_q24) {
#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  // Round to nearest Q16 integer instead of truncating:
  return (int32_t)((modifiers_q24 + 128) >> 8);
#else
  int64_t x_q24s = modifiers_q24 * (int64_t)multiplierTableScale;
  return (int32_t)((x_q24s + 128) >> 8);
#endif
}

int32_t SRAM_HOT(interpolate_live_ratio_q16)(int32_t xQ16, int dcoIndex) {
#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  return interpolateRatioQ16_fast(xQ16);
#elif PITCH_INTERP_MODE == PITCH_INTERP_Q12
  int32_t yTab = interpolatePitchMultiplierIntQ16_cached(xQ16, dcoIndex);
  uint64_t num = ((uint64_t)(uint32_t)yTab << 16) + 5000u;
  return (int32_t)((num * 0xD1B71759ULL) >> 45);
#else
#error                                                                      
    "interpolate_live_ratio_q16: PITCH_INTERP_FLOAT / FLOAT_FAST require USE_FLOAT_VOICE_TASK"
#endif
}
#endif // !USE_FLOAT_VOICE_TASK

float SRAM_HOT(interpolate_live_ratio_f)(float modifiers, int dcoIndex) {
#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_CACHED
  return interpolateRatioFloat_cached_fast(modifiers, dcoIndex);
#elif PITCH_INTERP_MODE == PITCH_INTERP_FLOAT
  return interpolateRatioFloat_fast(modifiers);
#elif PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  int32_t xQ16 = (int32_t)lroundf(modifiers * 65536.0f);
  return (float)interpolateRatioQ16_fast(xQ16) * (1.0f / 65536.0f);
#else
  float x = modifiers * (float)multiplierTableScale;
  int32_t xQ16 = (int32_t)lroundf(x * 65536.0f);
  return (float)interpolatePitchMultiplierIntQ16_cached(xQ16, dcoIndex) /
         (float)multiplierTableScale;
#endif
}

static float SRAM_HOT(fast_exp2f_audio)(float p) {
  // Fast floor
  int32_t i = (int32_t)p;
  if (p < 0.0f) i--; 
  float f = p - (float)i; // Fractional part [0.0, 1.0)
  
  // Quadratic approximation for 2^f
  float approx = 1.0f + f * (0.695847f + f * 0.304153f);
  
  // Fast exponent reconstruction via IEEE-754
  union { uint32_t i; float f; } v;
  v.i = (uint32_t)(i + 127) << 23; 
  
  return approx * v.f;
}


// Boot init: seed notes, build pitch tables, apply voice mode, run one
// voice_task_main().
void init_voices() {
  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    VOICE_NOTES[i] = DCO_calibration_start_note;
    VOICES[i] = 0;
  }

#ifdef ENABLE_MB_MOD_STREAM
  // ADSR_update() bails out early on this build and never refreshes the levels,
  // so the allocator estimates a release tail from the time instead (see
  // voice_alloc()).
  voiceAlloc.begin();
#else
  voiceAlloc.begin(ADSR_VCA_Level_q15);
#endif

  initMultiplierTables();
  setVoiceMode(voiceMode);
  voice_task_main();
}

// interpolation on the sNotePitches_q24 table. Used in slew-rate mode.
// Fast 32-bit helper: convert Q16 semitone note to Q24 frequency
static int64_t SRAM_HOT(noteQ16_to_freqQ24)(int32_t note_q16) {
  const size_t NOTE_TABLE_LEN =
      sizeof(sNotePitches_q24) / sizeof(sNotePitches_q24[0]);

  int32_t noteInt = note_q16 >> 16;
  if (noteInt <= 0) return sNotePitches_q24[0];
  if ((size_t)noteInt >= NOTE_TABLE_LEN - 1) return sNotePitches_q24[NOTE_TABLE_LEN - 1];

  uint32_t frac = (uint32_t)note_q16 & 0xFFFF;
  if (frac == 0) return sNotePitches_q24[noteInt]; // Instant lookup at semitone boundary

  int64_t f0 = sNotePitches_q24[noteInt];
  int64_t f1 = sNotePitches_q24[noteInt + 1];

  // Pre-shift df so multiplication is 100% 32-bit hardware MULS (zero __aeabi_lmul):
  uint32_t df_high = (uint32_t)((f1 - f0) >> 16);
  return f0 + (int64_t)(df_high * frac);
}

// Helper: convert float Hz to Q24 fixed-point (Hz * 2^24)
static int64_t SRAM_HOT(float_to_q24)(float f) {
  return (int64_t)lrintf(f * (float)(1 << 24));
}


// =============================================================================
// FIXED-POINT PORTAMENTO FUNCTIONS (Always available)
// =============================================================================

// Common endpoint latch
static void SRAM_HOT(porta_latch_endpoints_q16)(uint8_t osc, int32_t start_q16, int32_t target_q16) {
  porta_note_start_q16[osc] = start_q16;
  porta_note_stop_q16[osc] = target_q16;
  porta_note_cur_q16[osc] = start_q16;
  porta_note_valid[osc] = true;

  int32_t targetInt = target_q16 >> 16;
  const size_t LEN = sizeof(sNotePitches_q24) / sizeof(sNotePitches_q24[0]);
  if (targetInt >= 0 && (size_t)targetInt < LEN) {
    portamento_stop_q24[osc] = sNotePitches_q24[targetInt];
  } else {
    portamento_stop_q24[osc] = noteQ16_to_freqQ24(target_q16);
  }
}

// TIME: fixed duration T_fixed µs for any interval; linear in semitones.
static void SRAM_HOT(porta_setup_time_q16)(uint8_t osc, int32_t start_q16,
  int32_t target_q16, int32_t T_fixed) {
if (T_fixed < 256)
T_fixed = 256;
porta_latch_endpoints_q16(osc, start_q16, target_q16);

int32_t dNote_q16 = target_q16 - start_q16;
uint32_t u_total = (uint32_t)T_fixed >> 8;
if (u_total == 0)
u_total = 1;

// Pure 32-bit division on trigger (SIO hardware divider on RP2040)
porta_note_step_q16[osc] = dNote_q16 / (int32_t)u_total;
}

// SLEW: constant rate = 12 semitones / T_slew (one octave takes the slew-time knob).
static void SRAM_HOT(porta_setup_slew_q16)(uint8_t osc, int32_t start_q16,
  int32_t target_q16, int32_t T_slew) {
if (T_slew < 256)
T_slew = 256;
porta_latch_endpoints_q16(osc, start_q16, target_q16);

int32_t dNote_q16 = target_q16 - start_q16;
uint32_t u_slew = (uint32_t)T_slew >> 8;
if (u_slew == 0)
u_slew = 1;

// 12 semitones in Q16 = 786432
int32_t rate_per_u = 786432 / (int32_t)u_slew;
if (rate_per_u == 0)
rate_per_u = 1;

porta_note_step_q16[osc] = (dNote_q16 >= 0) ? rate_per_u : -rate_per_u;
}

static void SRAM_HOT(porta_setup_glide_q16)(uint8_t osc, int32_t start_q16,
   int32_t target_q16, uint8_t mode) {
if (mode == PORTA_MODE_TIME) {
int32_t T =
(portamento_time_fixed == 0) ? 1 : (int32_t)portamento_time_fixed;
porta_setup_time_q16(osc, start_q16, target_q16, T);
} else {
int32_t T = (portamento_time_slew == 0) ? 1 : (int32_t)portamento_time_slew;
porta_setup_slew_q16(osc, start_q16, target_q16, T);
}
}

// Resolve start note (Q16) directly from existing state (ZERO table searches)
static int32_t SRAM_HOT(porta_resolve_start_note_q16)(uint8_t osc, int32_t target_q16) {
  if (porta_note_valid[osc]) {
    return porta_note_cur_q16[osc];
  }
  porta_note_valid[osc] = true;
  return target_q16;
}

// =============================================================================
// FLOAT PORTAMENTO FUNCTIONS (Guarded for float-enabled builds)
// =============================================================================
#ifdef USE_FLOAT_VOICE_TASK

static float SRAM_HOT(noteIndex_to_freqFloat)(float noteIndex) {
  const size_t LEN = sizeof(sNotePitches) / sizeof(sNotePitches[0]);
  if (LEN == 0)
    return 0.0f;
  if (noteIndex <= 0.0f)
    return sNotePitches[0];
  if (noteIndex >= (float)(LEN - 1))
    return sNotePitches[LEN - 1];

  int n0 = (int)floorf(noteIndex);
  int n1 = n0 + 1;
  float t = noteIndex - (float)n0;
  float f0 = sNotePitches[n0];
  float f1 = sNotePitches[n1];
  return f0 + (f1 - f0) * t;
}

static float SRAM_HOT(freqFloat_to_noteIndex)(float hz) {
  const size_t LEN = sizeof(sNotePitches) / sizeof(sNotePitches[0]);
  if (LEN == 0)
    return 0.0f;
  if (LEN == 1)
    return 0.0f;
  if (hz <= sNotePitches[0])
    return 0.0f;
  if (hz >= sNotePitches[LEN - 1])
    return (float)(LEN - 1);

  size_t lo = 0;
  size_t hi = LEN - 1;
  while (hi - lo > 1) {
    size_t mid = lo + ((hi - lo) >> 1);
    if (sNotePitches[mid] <= hz) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  float f0 = sNotePitches[lo];
  float f1 = sNotePitches[hi];
  float df = f1 - f0;
  if (df <= 0.0f)
    return (float)lo;
  return (float)lo + (hz - f0) / df;
}

static float SRAM_HOT(porta_resolve_start_note_f)(uint8_t osc, float target) {
  if (porta_note_valid[osc]) {
    return porta_note_cur_f[osc];
  }
  if (porta_freq_cur_f[osc] > 0.0f) {
    porta_note_valid[osc] = true;
    return freqFloat_to_noteIndex(porta_freq_cur_f[osc]);
  }
  porta_note_valid[osc] = true;
  return target;
}

static void SRAM_HOT(porta_latch_endpoints_f)(uint8_t osc, float startNote,
                                           float targetNote) {
  porta_note_start_f[osc] = startNote;
  porta_note_stop_f[osc] = targetNote;
  porta_note_cur_f[osc] = startNote;
  porta_note_valid[osc] = true;
  float startHz = noteIndex_to_freqFloat(startNote);
  float stopHz = noteIndex_to_freqFloat(targetNote);
  porta_freq_start_f[osc] = startHz;
  porta_freq_stop_f[osc] = stopHz;
  porta_freq_cur_f[osc] = startHz;
}

// TIME: fixed duration T_fixed µs for any interval; linear in semitones.
static void SRAM_HOT(porta_setup_time_f)(uint8_t osc, float startNote,
                                      float targetNote, float T_fixed) {
  if (T_fixed < 1.0f)
    T_fixed = 1.0f;
  porta_latch_endpoints_f(osc, startNote, targetNote);
  porta_note_step_f[osc] = (targetNote - startNote) / T_fixed;
}

// SLEW: constant rate = 12 semitones / T_slew (one octave takes the slew-time knob).
static void SRAM_HOT(porta_setup_slew_f)(uint8_t osc, float startNote,
                                      float targetNote, float T_slew) {
  if (T_slew < 1.0f)
    T_slew = 1.0f;
  porta_latch_endpoints_f(osc, startNote, targetNote);

  float dNote = targetNote - startNote;
  if (dNote == 0.0f) {
    porta_note_step_f[osc] = 0.0f;
  } else {
    float rate = 12.0f / T_slew;
    porta_note_step_f[osc] = (dNote > 0.0f) ? rate : -rate;
  }
}

static void SRAM_HOT(porta_setup_glide_f)(uint8_t osc, float startNote,
                                       float targetNote, uint8_t mode) {
  if (mode == PORTA_MODE_TIME) {
    float T =
        (portamento_time_fixed == 0) ? 1.0f : (float)portamento_time_fixed;
    porta_setup_time_f(osc, startNote, targetNote, T);
  } else {
    float T = (portamento_time_slew == 0) ? 1.0f : (float)portamento_time_slew;
    porta_setup_slew_f(osc, startNote, targetNote, T);
  }
}

// Q24 → float modifier/Hz scale (multiply avoids per-sample divide by 2^24).
static constexpr float Q24_TO_FLOAT = 1.0f / 16777216.0f;

static float SRAM_HOT(q24_to_float)(int32_t q) { return (float)q * Q24_TO_FLOAT; }

#endif // USE_FLOAT_VOICE_TASK



//=================================================================================
// Helper to instantly phase-update the PIO countdown for a frequency change.
// This prevents audible clicks and lag by scaling the currently executing chunk's
// remaining time to match the new frequency's period ratio.
//
// OPTIMIZATION: Compile-Time PIO Instruction Opcodes
// Eliminates the runtime overhead of pio_encode_*() SDK functions.
// =========================================================================
static constexpr uint32_t PIO_INSTR_IN_X_32   = 0x4020; 
static constexpr uint32_t PIO_INSTR_PUSH      = 0x8020; 
static constexpr uint32_t PIO_INSTR_PULL      = 0x80a0; 
static constexpr uint32_t PIO_INSTR_MOV_X_OSR = 0xa027; 

// =============================================================================
// HEAVILY OPTIMIZED INSTANT PIO DIVIDER UPDATE (250 MHz / 10 µs Loop1)
// =============================================================================
static constexpr int32_t BUS_LATENCY_CYCLES = 43;  ///< Exactly compensates the 20-cent drop
static constexpr int32_t SAFEGUARD_CYCLES   = 150; ///< ~600 ns threshold at 250 MHz

static inline void SRAM_HOT(update_osc_clk_div_instantly)(PIO pio, uint sm, uint8_t osc, uint32_t new_div) {
  const uint32_t old_div = osc_last_clk_div[osc];
  if (__builtin_expect(old_div == new_div, 1)) return;

  const uint32_t pc = pio->sm[sm].addr; 
  if (__builtin_expect(pc != 0, 1)) {
      
      const uint32_t empty_mask = (1u << (PIO_FSTAT_RXEMPTY_LSB + sm));
      while (!(pio->fstat & empty_mask)) {
          (void)pio->rxf[sm];
      }
      
      pio->sm[sm].instr = PIO_INSTR_IN_X_32;
      pio->sm[sm].instr = PIO_INSTR_PUSH;
      
      const int32_t current_x = (int32_t)pio->rxf[sm];
      
      // Safeguard 1: Do not rescale if the chunk has less than 600 ns left
      if (__builtin_expect(current_x > SAFEGUARD_CYCLES, 1)) {
          const float ratio = (float)new_div / (float)old_div;
          const int32_t new_x = (int32_t)((float)current_x * ratio) - BUS_LATENCY_CYCLES;
          
          // Safeguard 2: Ensure an upward pitch jump doesn't compress new_x below 30 cycles
          // (Protects the OSR pipeline race condition)
          if (__builtin_expect(new_x > 30, 1)) {
              pio->txf[sm] = (uint32_t)new_x;
              pio->sm[sm].instr = PIO_INSTR_PULL;
              pio->sm[sm].instr = PIO_INSTR_MOV_X_OSR;
              __dmb(); // Guarantees X latches before queuing new_div
          }
      }
  }
  
  // Queue new_div in OSR for all subsequent chunk loops
  pio->txf[sm] = new_div;
  pio->sm[sm].instr = PIO_INSTR_PULL;
  osc_last_clk_div[osc] = new_div;
}

uint32_t get_osc_clk_div(uint8_t osc, float freqHz) {
  
  uint32_t total_cycles1 = clkdiv_live_total_cycles(sysClock_Hz_cached_float, freqHz);
  
  uint32_t wA, kA, wB, kB;
  get_osc_params(osc, wA, kA);
  
  uint32_t clk_div = pio_clk_div_for_y(total_cycles1, osc_last_y[osc], wA, kA);
  
  return clk_div;
}

#ifndef USE_FLOAT_VOICE_TASK
// Fixed-point realtime voice engine (portamento, modifiers, clkdiv, amp,
// PIO/PWM/PW). Selected by voice_task_main() when USE_FLOAT_VOICE_TASK is not
// defined.
void SRAM_HOT(voice_task_fixed_point)() {
  static uint32_t last_portamento_time = 0;
  static uint8_t last_portamento_mode = PORTA_MODE_TIME;
  static uint8_t lastNote1[NUM_VOICES_TOTAL] = {};
  static uint8_t lastNote2[NUM_VOICES_TOTAL] = {};
  uint32_t portaTime = portamento_time;
  uint8_t portaMode = portamento_mode;
  bool portaTimeChanged = (portaTime != last_portamento_time);
  bool portaModeChanged = (portaMode != last_portamento_mode);

  int32_t calcPitchbend_q24;

  BENCH_BEGIN(vt_pitchbend);
  int32_t bend_normalized_q24 = ((int32_t)midi_pitch_bend << 11) - (1 << 24);
  calcPitchbend_q24 =
      (int32_t)(((int64_t)bend_normalized_q24 * pitchBendMultiplier_q24) >> 24);
  BENCH_END(vt_pitchbend);

  last_midi_pitch_bend = midi_pitch_bend;

  for (int i = 0; i < NUM_VOICES; i++) {

    if (note_on_flag[i] == 1) {
      note_on_flag_flag[i] = true;
      note_on_flag[i] = 0;
      __dmb(); // Memory barrier: Ensure flag is cleared before reading note indices
    }

#if DCO_DEBUG_REPORT
    float dbg_freq_base_Hz = 0.0f;
    float dbg_freq_after_mod_Hz = 0.0f;
#endif

    // =========================================================================
    // INSTANT ZERO-MATH FETCH (Replaces runtime midi_offset_to_table_index)
    // =========================================================================
    // Fetch the pre-baked pitch table indices directly from SRAM (1 clock cycle).
    // All octave shifting, interval math, and folding were completed at note_on.
    const uint8_t note1 = VOICE_NOTE_OSC1[i];
    const uint8_t note2 = VOICE_NOTE_OSC2[i];

    // Detect if the target pitch has changed for glide / portamento re-targeting
    const bool pitchTargetChanged =
        note1 != lastNote1[i] || note2 != lastNote2[i];
    lastNote1[i] = note1;
    lastNote2[i] = note2;

    int64_t freq_q24_A;
    int64_t freq_q24_B;
    const uint8_t DCO_A = (uint8_t)(i * 2);
    const uint8_t DCO_B = (uint8_t)(i * 2 + 1);

    // OSC2 Fine Detune
    BENCH_BEGIN(vt_osc_detune);
    static constexpr int32_t DETUNE_SCALE_Q24 = (int32_t)(0.0002f * (float)(1 << 24) + 0.5f);
    int32_t detune_steps = ((int32_t)OSC2_detune - 256);
    int32_t detune_q24 = (1 << 24) + (detune_steps * DETUNE_SCALE_Q24);
    BENCH_END(vt_osc_detune);


    BENCH_BEGIN(vt_portamento);
    if (portaTime > 0) {
      uint32_t now_us = micros();
      portamentoTimer[i] = now_us - portamentoStartMicros[i];

      if (note_on_flag_flag[i]) {
        portamentoStartMicros[i] = now_us;
        portamentoTimer[i] = 0;

        int32_t targetNoteA_q16 = ((int32_t)note1) << 16;
        int32_t targetNoteB_q16 = ((int32_t)note2) << 16;
        porta_setup_glide_q16(
            DCO_A, porta_resolve_start_note_q16(DCO_A, targetNoteA_q16),
            targetNoteA_q16, portaMode);
        porta_setup_glide_q16(
            DCO_B, porta_resolve_start_note_q16(DCO_B, targetNoteB_q16),
            targetNoteB_q16, portaMode);
      }

      const bool portaDoRetime =
          (portaTimeChanged || portaModeChanged || pitchTargetChanged) &&
          !note_on_flag_flag[i];

      int64_t curA;
      int64_t curB;

      if (portaDoRetime) {
        portamentoStartMicros[i] = now_us;
        portamentoTimer[i] = 0;
      
        // DIRECT note position (no table lookups or binary searches!)
        porta_setup_glide_q16(DCO_A, porta_note_cur_q16[DCO_A], (float)note1, portaMode);
        porta_setup_glide_q16(DCO_B, porta_note_cur_q16[DCO_B], (float)note2, portaMode);
        curA = portamento_cur_freq_q24[DCO_A];
        curB = portamento_cur_freq_q24[DCO_B];
      } else if (porta_note_cur_q16[DCO_A] == porta_note_stop_q16[DCO_A] &&
                 porta_note_cur_q16[DCO_B] == porta_note_stop_q16[DCO_B]) {
        curA = portamento_stop_q24[DCO_A];
        curB = portamento_stop_q24[DCO_B];
      } else {
        int32_t elapsed_u = (int32_t)((uint32_t)portamentoTimer[i] >> 8);

        // 100% 32-bit native MULS + ADDS
        int32_t curNoteA_q16 = porta_note_start_q16[DCO_A] + (porta_note_step_q16[DCO_A] * elapsed_u);
        int32_t curNoteB_q16 = porta_note_start_q16[DCO_B] + (porta_note_step_q16[DCO_B] * elapsed_u);

        int32_t stopA  = porta_note_stop_q16[DCO_A];
        int32_t stopB  = porta_note_stop_q16[DCO_B];
        int32_t startA = porta_note_start_q16[DCO_A];
        int32_t startB = porta_note_start_q16[DCO_B];

        if ((stopA >= startA && curNoteA_q16 >= stopA) ||
            (stopA < startA && curNoteA_q16 <= stopA)) {
          curNoteA_q16 = stopA;
        }
        if ((stopB >= startB && curNoteB_q16 >= stopB) ||
            (stopB < startB && curNoteB_q16 <= stopB)) {
          curNoteB_q16 = stopB;
        }

        porta_note_cur_q16[DCO_A] = curNoteA_q16;
        porta_note_cur_q16[DCO_B] = curNoteB_q16;

        curA = (curNoteA_q16 == stopA) ? portamento_stop_q24[DCO_A] : noteQ16_to_freqQ24(curNoteA_q16);
        curB = (curNoteB_q16 == stopB) ? portamento_stop_q24[DCO_B] : noteQ16_to_freqQ24(curNoteB_q16);
      }

      portamento_cur_freq_q24[DCO_A] = curA;
      portamento_cur_freq_q24[DCO_B] = curB;
    } else {
      // Instant lookup when portamento is off (ZERO math)
      portamento_cur_freq_q24[DCO_A] = sNotePitches_q24[note1];
      portamento_cur_freq_q24[DCO_B] = sNotePitches_q24[note2];
      portamento_stop_q24[DCO_A] = portamento_cur_freq_q24[DCO_A];
      portamento_stop_q24[DCO_B] = portamento_cur_freq_q24[DCO_B];
      porta_note_cur_q16[DCO_A] = ((int32_t)note1) << 16;
      porta_note_cur_q16[DCO_B] = ((int32_t)note2) << 16;
      porta_note_stop_q16[DCO_A] = porta_note_cur_q16[DCO_A];
      porta_note_stop_q16[DCO_B] = porta_note_cur_q16[DCO_B];
      porta_note_valid[DCO_A] = true;
      porta_note_valid[DCO_B] = true;
    }
    BENCH_END(vt_portamento);

    BENCH_BEGIN(vt_adsr_mod);
    int32_t ADSRModifier_q24 = 0;
    if (ADSR3toDETUNE1_scale_q24 != 0) {
      ADSRModifier_q24 = applyDepthQ24(ADSR3Level_q15[i], ADSR3toDETUNE1_scale_q24);
    }
    // ADSR3→pitch: 0=A, 1=B, 2=A+B (legacy), 3/4 ignored or map 4→A+B
    int32_t ADSRModifierOSC1_q24 =
        (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 ||
         ADSR3ToOscSelect == 4)
            ? ADSRModifier_q24
            : 0;
    int32_t ADSRModifierOSC2_q24 =
        (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 ||
         ADSR3ToOscSelect == 4)
            ? ADSRModifier_q24
            : 0;
    BENCH_END(vt_adsr_mod);

    BENCH_BEGIN(vt_unison_mod);
    static constexpr int32_t UNISON_SCALE_Q24 =
        (int32_t)(0.0001f * (float)(1 << 24) + 0.5f);
    const int32_t unisonBase = (int32_t)unisonDetune * UNISON_SCALE_Q24;
    int32_t voiceMag = (i >> 1) + 1;
    int32_t voiceSign = ((i & 0x01) == 0) ? 1 : -1;
    int32_t unisonMODIFIER_q24 = unisonBase * (voiceSign * voiceMag);
    BENCH_END(vt_unison_mod);

    BENCH_BEGIN(vt_drift_mod);
    const int32_t driftScale_q24 = drift_pitch_scale_q24;
    const int16_t driftA = LFO_DRIFT_LEVEL[DCO_A];
    const int16_t driftB = LFO_DRIFT_LEVEL[DCO_B];
    int32_t DETUNE_DRIFT_OSC1_q24 =
        (driftScale_q24 != 0) ? applyDepthQ24(driftA, driftScale_q24) : 0;
    int32_t DETUNE_DRIFT_OSC2_q24 =
        (driftScale_q24 != 0) ? applyDepthQ24(driftB, driftScale_q24) : 0;
    BENCH_END(vt_drift_mod);

    int32_t modifiersBase_q24;
    int32_t freqModifiers_q24;
    int32_t freq2Modifiers_q24;
    {
      const int32_t local_lfo1_osc1 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC1];
      const int32_t local_lfo1_osc2 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC2];
      const int32_t local_lfo2_osc2 = lfo2_pitch_mod_q24[LFO2_PITCH_OSC2];
      BENCH_BEGIN(vt_modifiers);
      modifiersBase_q24 = calcPitchbend_q24 + Q24_ONE_EPS +
                          matrix_pitch_mod_q24[i] + unisonMODIFIER_q24;
      if (char_pitch_scale_q15) {
        modifiersBase_q24 += character_pitch_delta_q24();
      }
      freqModifiers_q24 = ADSRModifierOSC1_q24 + DETUNE_DRIFT_OSC1_q24 + modifiersBase_q24 + local_lfo1_osc1 + matrix_osc1_pitch_mod_q24[i];
      freq2Modifiers_q24 = ADSRModifierOSC2_q24 + DETUNE_DRIFT_OSC2_q24 + modifiersBase_q24 + local_lfo1_osc2 + local_lfo2_osc2 + matrix_osc2_pitch_mod_q24[i];
      BENCH_END(vt_modifiers);
    }

    BENCH_BEGIN(vt_freq_scale_x);
    int32_t xScaled1_Q16 = modifiers_q24_to_xQ16(freqModifiers_q24);
    int32_t xScaled2_Q16 = modifiers_q24_to_xQ16(freq2Modifiers_q24);
    BENCH_END(vt_freq_scale_x);

    BENCH_BEGIN(vt_ratio_interp);
    int32_t ratio1_Q16 = interpolate_live_ratio_q16(xScaled1_Q16, DCO_A);
    int32_t ratio2_Q16 = interpolate_live_ratio_q16(xScaled2_Q16, DCO_B);
    BENCH_END(vt_ratio_interp);

    BENCH_BEGIN(vt_freq_scale_post);
    freq_q24_A = (portamento_cur_freq_q24[DCO_A] * (int64_t)ratio1_Q16) >> 16;
    int32_t detune_Q16 = (int32_t)((((int64_t)detune_q24) + 128) >> 8);
    int32_t combined_Q16 =
        (int32_t)((((int64_t)ratio2_Q16 * (int64_t)detune_Q16) + (1LL << 15)) >>
                  16);
    freq_q24_B = (portamento_cur_freq_q24[DCO_B] * (int64_t)combined_Q16) >> 16;

        // Fast 1-cycle Q24 -> Q16 conversion with rounding (+128 >> 8)
    // Both fA_q16 and fB_q16 are now stored in fast 32-bit registers
    const uint32_t fA_q16 = (freq_q24_A <= 0) ? 0 : (uint32_t)((freq_q24_A + 128) >> 8);
    const uint32_t fB_q16 = (freq_q24_B <= 0) ? 0 : (uint32_t)((freq_q24_B + 128) >> 8);

#if DCO_DEBUG_REPORT
    dbg_freq_after_mod_Hz = (float)freq_q24_A / (float)(1 << 24);
#endif
    BENCH_END(vt_freq_scale_post);

   
    BENCH_BEGIN(vt_clk_div);
    PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
    PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
    uint8_t smAN = VOICE_TO_SM[DCO_A];
    uint8_t smBN = VOICE_TO_SM[DCO_B];

    uint32_t total_cycles1, total_cycles2;
    const uint32_t sys_hz = sysClock_Hz;
   
    total_cycles1 = clkdiv_q16_total_cycles(sys_hz, fA_q16);
    total_cycles2 = clkdiv_q16_total_cycles(sys_hz, fB_q16);
    
    // Fetch parameters using normal function calls (no angle brackets)
    uint32_t wA, kA, wB, kB;
    get_osc_params(DCO_A, wA, kA);
    get_osc_params(DCO_B, wB, kB);

    uint32_t clk_div1 = pio_clk_div_for_y(total_cycles1, osc_last_y[DCO_A], wA, kA);
    uint32_t clk_div2 = pio_clk_div_for_y(total_cycles2, osc_last_y[DCO_B], wB, kB);

    BENCH_END(vt_clk_div);

    uint32_t phaseHoldX = 0;
    PioPeriod retrig_p1{};
    PioPeriod retrig_p2{};
    if (note_on_flag_flag[i] && oscPhaseSync > 1) {
      BENCH_BEGIN(vt_phase_align);
      phaseHoldX = osc_phase_hold_x(total_cycles2, phaseAlignOSC2);
      BENCH_END(vt_phase_align);
    }
    if (note_on_flag_flag[i] && oscPhaseSync >= 1 &&
        note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
      BENCH_BEGIN(vt_retrig_split);
      retrig_p1 = pio_period_split(total_cycles1, wA, kA);
      retrig_p2 = pio_period_split(total_cycles2, wB, kB);
      BENCH_END(vt_retrig_split);
    }

    BENCH_BEGIN(vt_chan_level);

    // 2. Direct calls (Compiles straight to inlined register assembly)
    const uint16_t chanLevel  = get_chan_level_q16_fast(fA_q16, DCO_A);
    const uint16_t chanLevel2 = get_chan_level_q16_fast(fB_q16, DCO_B);
    BENCH_END(vt_chan_level);

    if (note_on_flag_flag[i]) {
      BENCH_BEGIN(vt_note_retrig);
#if DCO_DEBUG_REPORT
      uint32_t actual_total_osr_val = clk_div1 * wA;
      uint32_t actual_total_period =
          osc_last_y[DCO_A] + actual_total_osr_val + kA;
      float expected_freq = (double)sysClock_Hz / (double)actual_total_period;
      PioPeriod p1dbg = pio_period_split(total_cycles1, wA, kA);
      Serial.println("----------------[ DCO DEBUG REPORT ]----------------");
      Serial.printf("Target Freq In:   %.2f Hz\n",
                    (float)freq_q24_A / (float)(1 << 24));
      Serial.printf("Total Cycles Calc:  %lu (Target for the whole period)\n",
                    total_cycles1);
      Serial.printf("Reset pulse (Y):    %lu cycles (incl. period remainder)\n",
                    p1dbg.y);
      Serial.printf("Period Overhead:    %lu cycles (program constant)\n", kA);
      Serial.printf("Total OSR Delay:    %lu cycles (Remaining for loops)\n",
                    p1dbg.clk_div * wA);
      Serial.printf("clk_div (Exact):    %lu (Value sent to PIO)\n",
                    p1dbg.clk_div);
      Serial.println("---");
      Serial.printf(
          "Actual Period Gen:  %lu cycles (Y + (clk_div*%u) + overhead)\n",
          actual_total_period, (unsigned)wA);
      Serial.printf("==> Expected Freq Out: %.2f Hz\n", expected_freq);
      Serial.println("---");
      Serial.println("OSC1 Frequency Stages:");
      Serial.printf("  Base after portamento:     %.4f Hz\n", dbg_freq_base_Hz);
      Serial.printf("  After modifiers (Q24):     %.4f Hz\n",
                    dbg_freq_after_mod_Hz);
      Serial.printf("  Quantized by PIO (clkdiv): %.4f Hz\n", expected_freq);
      Serial.println("----------------------------------------------------\n");
#endif

      if (oscPhaseSync >= 1) {
        BENCH_BEGIN(vt_retrig_sm_apply);
        if (note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
          uint32_t maskAB = (1u << smAN) | (1u << smBN);
          pio_set_sm_mask_enabled(pioN_A, maskAB, false);

          osc_load_periods_stopped_noclear(DCO_A, retrig_p1.y,
                                           retrig_p1.clk_div, DCO_B,
                                           retrig_p2.y, retrig_p2.clk_div);

          pio_sm_exec(pioN_A, smAN, pio_encode_jmp(osc_restart_target(DCO_A)));

          if (phaseHoldX != 0) {
            osc_phase_align_hold_stopped(DCO_B, phaseHoldX);
          } else {
            pio_sm_exec(pioN_B, smBN,
                        pio_encode_jmp(osc_restart_target(DCO_B)));
          }

          pio_enable_sm_mask_in_sync(pioN_A, maskAB);
        } else {
          pio_sm_exec(pioN_A, smAN, pio_encode_jmp(osc_restart_target(DCO_A)));
          pio_sm_exec(pioN_B, smBN, pio_encode_jmp(osc_restart_target(DCO_B)));
        }
        BENCH_END(vt_retrig_sm_apply);
      }

      BENCH_END(vt_note_retrig);
    } else {
      BENCH_BEGIN(vt_pio_write);
      pio_sm_put(pioN_A, smAN, clk_div1);
      pio_sm_put(pioN_B, smBN, clk_div2);
      pio_sm_exec(pioN_A, smAN, pio_encode_pull(false, false));
      pio_sm_exec(pioN_B, smBN, pio_encode_pull(false, false));
      osc_last_clk_div[DCO_A] = clk_div1;
      osc_last_clk_div[DCO_B] = clk_div2;
      BENCH_END(vt_pio_write);
    }
    
      BENCH_BEGIN(vt_range_pwm);
      write_range_pwm(DCO_A, chanLevel);
      write_range_pwm(DCO_B, chanLevel2);
      BENCH_END(vt_range_pwm);

      if (timer99microsFlag2) {
      if (pulseWaveOn) {
        BENCH_FBEGIN(vt_pwm_calc);

        const int16_t local_LFO2Level = LFO2Level;
        const int16_t local_LFO2toPW = LFO2toPW;
        const int16_t local_ADSR3toPWM =
            ADSR3toPWM; // Note: ADSR3toPWM is signed (-512 .. +512)

        // 1. Calculate Q15 modulation deltas
        const int32_t adsr3_delta =
            ((int32_t)ADSR3Level_q15[i] * (int32_t)local_ADSR3toPWM) >> 15;
        const int32_t lfo2_delta =
            ((int32_t)local_LFO2Level * (int32_t)local_LFO2toPW) >> 15;

        // 2. ✅ Clean logical sum: Knob + LFO + Envelope + mod_matrix + Jitter 
        int32_t pw_calc = (int32_t)PW[0] + lfo2_delta + adsr3_delta + matrix_pw_mod[i] + (int32_t)character_pw_delta();
        pw_calc = (pw_calc < 0) ? 0 : ((pw_calc > max_pw) ? max_pw : pw_calc);

        PW_PWM[i] = (uint16_t)pw_calc;
        BENCH_FEND(vt_pwm_calc);
        BENCH_BEGIN(vt_pwm_write);
        // 4. Pass pitch (Q24) for 3-point key tracking
        voice_write_pw(i, get_PW_level_interpolated(PW_PWM[i], DCO_A, freq_q24_A));
        BENCH_END(vt_pwm_write);
      } else {
        BENCH_BEGIN(vt_pwm_write);
        voice_write_pw(i, 0);
        BENCH_END(vt_pwm_write);
      }
    }
  }

  for (int k = 0; k < NUM_VOICES_TOTAL; k++) {  // <-- Change to NUM_VOICES_TOTAL
    note_on_flag_flag[k] = false;
  }

  last_portamento_time = portaTime;
  last_portamento_mode = portaMode;
}

#endif // !USE_FLOAT_VOICE_TASK

// Dispatch entry point: select float vs fixed-point implementation at compile
// time.
void SRAM_HOT(voice_task_main)() {
#ifdef USE_FLOAT_VOICE_TASK
  #ifdef USE_VOICE_TASK_Q24
    voice_task_Q24();
  #else
    voice_task_float();
  #endif
#else
  voice_task_fixed_point();
#endif
}

#if defined(USE_FLOAT_VOICE_TASK) && !defined(USE_VOICE_TASK_Q24)
// Float realtime voice engine (same stages as voice_task_fixed_point, in Hz).
// Board default on RP2350.
#include "hardware/timer.h"

// =========================================================================
// OPTIMIZATION 1: Move Statics Out of Function Scope
// By putting these in the global/file scope (using 'static' to keep them private),
// we completely eliminate the GCC hidden thread-safe initialization guards.
// =========================================================================
static uint32_t last_portamento_time = 0;
static uint8_t  last_portamento_mode = PORTA_MODE_SLEW;
static uint32_t last_task_us = 0;
static uint8_t  lastNote1[NUM_VOICES_TOTAL] = {};
static uint8_t  lastNote2[NUM_VOICES_TOTAL] = {};


void SRAM_HOT(voice_task_float)() {
  BENCH_BEGIN(vt_task_setup);

  // =========================================================================
  // OPTIMIZATION 2: Bypass SDK 64-bit Timer
  // timer_hw->timelr reads the 32-bit hardware microsecond counter directly in 1 cycle.
  // =========================================================================
  uint32_t now_us_task = timer_hw->timelr;
  
  // Natively handles timer wrap-around safely because of unsigned 32-bit math
  uint32_t dt_us = now_us_task - last_task_us;

  // =========================================================================
  // OPTIMIZATION 3: Integer-Domain Bounds Check
  // 0.01 seconds = 10,000 microseconds. 
  // We check bounds natively in integers (1 cycle) before invoking the FPU.
  // __builtin_expect tells the branch predictor this clamp almost never happens.
  // =========================================================================
  if (__builtin_expect(dt_us > 10000 || last_task_us == 0, 0)) {
      dt_us = 100; // Default to 100 us (0.0001f sec)
  }
  
  last_task_us = now_us_task;

  // Exact 1-cycle conversion to float (VCVT.F32.U32 + VMUL.F32)
  const float dt_sec = (float)dt_us * 0.000001f;

  const uint32_t portaTime = portamento_time;
  const uint8_t  portaMode = portamento_mode;
  
  const bool portaTimeChanged = (portaTime != last_portamento_time);
  const bool portaModeChanged = (portaMode != last_portamento_mode);
  
  BENCH_END(vt_task_setup);


  BENCH_BEGIN(vt_pitchbend);
  static constexpr float INV_8192 = 1.0f / 8192.0f;
  float calcPitchbend = ((float)midi_pitch_bend - 8192.0f) * INV_8192 * pitchBendMultiplier;
  last_midi_pitch_bend = midi_pitch_bend;
  BENCH_END(vt_pitchbend);


  BENCH_BEGIN(vt_task_prep);
  // =========================================================================
  // OPTIMIZATION PILLAR II: Register Hoisting & Restrict Pointers
  // Corrected with explicit 'volatile' qualifiers and exact underlying types.
  // =========================================================================
  const volatile uint8_t* __restrict vn_osc1 = VOICE_NOTE_OSC1;
  const volatile uint8_t* __restrict vn_osc2 = VOICE_NOTE_OSC2;
  const volatile float* __restrict m_pitch_f = matrix_pitch_mod_f;
  const volatile float* __restrict m_osc1_f  = matrix_osc1_pitch_mod_f;
  const volatile float* __restrict m_osc2_f  = matrix_osc2_pitch_mod_f;
  const int32_t* __restrict m_pw   = (const int32_t*)matrix_pw_mod;
  const int32_t* __restrict m_xmod = (const int32_t*)matrix_xmod_mod;
  const int16_t* __restrict adsr3_lvl = ADSR3Level_q15;
  volatile uint8_t* __restrict n_on_flag = note_on_flag;
  volatile bool* __restrict n_on_flag_f = note_on_flag_flag;

  // 1. OSC2 Detune
  const float detuneSteps = (float)((int32_t)OSC2_detune - 256);
  const float osc2DetuneRatio = 1.0f + 0.0002f * detuneSteps;

  // 2. Unison Base
  static constexpr float UNISON_SCALE = 0.0001f;
  const float unisonBase = (float)unisonDetune * UNISON_SCALE;

  // 3. Global LFOs
  const float lfo1_osc1_f = lfo1_pitch_mod_f[LFO1_PITCH_OSC1];
  const float lfo1_osc2_f = lfo1_pitch_mod_f[LFO1_PITCH_OSC2];
  const float lfo2_osc2_f = lfo2_pitch_mod_f[LFO2_PITCH_OSC2];

  // 4. Constant Epsilon
  static constexpr float EPS_FLOAT = (float)Q24_ONE_EPS * (1.0f / 16777216.0f);

  // 5. Global PWM LFO delta
  const int32_t lfo2_pw_delta = ((int32_t)LFO2Level * (int32_t)LFO2toPW) >> 15;

  // Character Engine Output
  const float char_pitch_delta_f = char_pitch_scale_q15 ? character_pitch_delta_float() : 0.0f;
  const int32_t char_pw_delta_i = (int32_t)character_pw_delta();
  const int32_t char_amp_mod_factor  = char_amp_scale_q15   ? character_amp_delta()         : 0;

  // ADSR Routing flags (computed once outside the loop to eliminate branch mispredictions)
  const bool adsr_osc1_en = (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4);
  const bool adsr_osc2_en = (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4);

  //  --- NEW SW SYNC
  static uint32_t shadow_phase_osc2_q32[NUM_VOICES_TOTAL] = {0};
  const int32_t base_xmod = (int32_t)crossmod_depth;
  // Pre-calculate loop constants
  const float hz_to_phase_inc = dt_sec * 4294967296.0f; // Multiplier to map Hz to Q32 Phase
  const float depth_scaler = 0.00003051757f * 2.5f;     // Combines the division and 2.5 octave scale
  // ----------------------------

  const uint8_t sm = syncMode;
  BENCH_END(vt_task_prep);

  // =========================================================================
  // OPTIMIZATION PILLAR VI: Loop Unrolling
  // Amortize SUBS/BNE branch overhead on short loops running 4-16 iterations.
  // =========================================================================
  _Pragma("GCC unroll 4")
  for (int i = 0; i < NUM_VOICES; ++i) {

    BENCH_BEGIN(vt_loop_prep);
      
    // OPTIMIZATION 1: "Load-Acquire" Atomic Exchange
    // Replaces __dmb() entirely! This executes a 1-cycle ARMv8-M atomic 
    // instruction that guarantees memory sync specifically for this flag 
    // WITHOUT halting the entire system bus. __builtin_expect guarantees 
    // the compiler optimizes the I-Cache for the 99% case where no note is pressed.
    const bool is_note_on = (__atomic_exchange_n(&n_on_flag[i], 0, __ATOMIC_ACQUIRE) == 1);

    #if DCO_DEBUG_REPORT
    float dbg_freq_base_Hz = 0.0f;
    float dbg_freq_after_mod_Hz = 0.0f;
    #endif

    // OPTIMIZATION 2: Localize memory to prevent redundant SRAM fetches
    const uint8_t note1  = vn_osc1[i];
    const uint8_t note2  = vn_osc2[i];
    const uint8_t lNote1 = lastNote1[i];
    const uint8_t lNote2 = lastNote2[i];

    // OPTIMIZATION 3: Branchless Pitch Target Check
    // Replacing '||' with Bitwise XOR/OR. 
    // This evaluates in exactly 2 cycles natively (EOR + ORR) with ZERO branch instructions.
    const bool pitchTargetChanged = ((note1 ^ lNote1) | (note2 ^ lNote2)) != 0;

    lastNote1[i] = note1;
    lastNote2[i] = note2;

    float noteFreq1 = sNotePitches[note1];
    float noteFreq2 = sNotePitches[note2];
    float freqA, freqB;

    // OPTIMIZATION 4: Single-cycle bitwise math
    // Replaces multiplication and addition with direct bit-shifts and ORs
    const uint8_t DCO_A = (uint8_t)(i << 1);       // i * 2
    const uint8_t DCO_B = (uint8_t)((i << 1) | 1); // i * 2 + 1

    BENCH_END(vt_loop_prep);


      BENCH_BEGIN(vt_portamento);
      if (portaTime > 0) {
          uint32_t now_us = micros();
          portamentoTimer[i] = now_us - portamentoStartMicros[i];

          if (is_note_on) {
              portamentoStartMicros[i] = now_us;
              portamentoTimer[i] = 0;

              float targetNoteA = (float)note1;
              float targetNoteB = (float)note2;
              porta_setup_glide_f(DCO_A, porta_resolve_start_note_f(DCO_A, targetNoteA), targetNoteA, portaMode);
              porta_setup_glide_f(DCO_B, porta_resolve_start_note_f(DCO_B, targetNoteB), targetNoteB, portaMode);
          }

          const bool portaDoRetime = (portaTimeChanged || portaModeChanged || pitchTargetChanged) && !is_note_on;

          float curA, curB;
          if (portaDoRetime) {
              portamentoStartMicros[i] = now_us;
              portamentoTimer[i] = 0;

              porta_setup_glide_f(DCO_A, porta_note_cur_f[DCO_A], (float)note1, portaMode);
              porta_setup_glide_f(DCO_B, porta_note_cur_f[DCO_B], (float)note2, portaMode);
              curA = porta_freq_cur_f[DCO_A];
              curB = porta_freq_cur_f[DCO_B];
          } else if (porta_note_cur_f[DCO_A] == porta_note_stop_f[DCO_A] &&
              porta_note_cur_f[DCO_B] == porta_note_stop_f[DCO_B]) {
              curA = porta_freq_stop_f[DCO_A];
              curB = porta_freq_stop_f[DCO_B];
          } else {
              int32_t elapsed = (int32_t)portamentoTimer[i];

              float startNoteA = porta_note_start_f[DCO_A];
              float startNoteB = porta_note_start_f[DCO_B];
              float stopNoteA = porta_note_stop_f[DCO_A];
              float stopNoteB = porta_note_stop_f[DCO_B];

              float dNoteA = stopNoteA - startNoteA;
              float dNoteB = stopNoteB - startNoteB;

              float curNoteA = startNoteA + porta_note_step_f[DCO_A] * (float)elapsed;
              float curNoteB = startNoteB + porta_note_step_f[DCO_B] * (float)elapsed;

              // =========================================================================
              // OPTIMIZATION PILLAR I: Branchless Conditional Moves
              // Nested ternary operators map directly to hardware IT (If-Then) blocks
              // without dumping the pipeline. Replaces complex bounds `if` ladders.
              // =========================================================================
              curNoteA = (dNoteA >= 0.0f) ? (curNoteA >= stopNoteA ? stopNoteA : curNoteA)
                                          : (curNoteA <= stopNoteA ? stopNoteA : curNoteA);
              curNoteB = (dNoteB >= 0.0f) ? (curNoteB >= stopNoteB ? stopNoteB : curNoteB)
                                          : (curNoteB <= stopNoteB ? stopNoteB : curNoteB);

              porta_note_cur_f[DCO_A] = curNoteA;
              porta_note_cur_f[DCO_B] = curNoteB;

              curA = (curNoteA == stopNoteA) ? porta_freq_stop_f[DCO_A] : noteIndex_to_freqFloat(curNoteA);
              curB = (curNoteB == stopNoteB) ? porta_freq_stop_f[DCO_B] : noteIndex_to_freqFloat(curNoteB);

              porta_freq_cur_f[DCO_A] = curA;
              porta_freq_cur_f[DCO_B] = curB;
          }

          freqA = curA;
          freqB = curB;

      } else {
          freqA = noteFreq1;
          freqB = noteFreq2;

          porta_freq_cur_f[DCO_A] = freqA;
          porta_freq_cur_f[DCO_B] = freqB;
          porta_freq_stop_f[DCO_A] = freqA;
          porta_freq_stop_f[DCO_B] = freqB;
          porta_note_cur_f[DCO_A] = (float)note1;
          porta_note_cur_f[DCO_B] = (float)note2;
          porta_note_stop_f[DCO_A] = (float)note1;
          porta_note_stop_f[DCO_B] = (float)note2;
          porta_note_valid[DCO_A] = true;
          porta_note_valid[DCO_B] = true;
      }

      #if defined(BENCH_PATH_STATS)
      if (portaTime == 0) {
          BENCH_PATH_INC(porta_off);
      } else if (is_note_on) {
          BENCH_PATH_INC(porta_note_on);
      } else if (portaTimeChanged || portaModeChanged || pitchTargetChanged) {
          BENCH_PATH_INC(porta_retime);
      } else if (portaMode == PORTA_MODE_TIME) {
          BENCH_PATH_INC(porta_steady_time);
      } else {
          BENCH_PATH_INC(porta_steady_slew);
      }
      #endif

      #if DCO_DEBUG_REPORT
      dbg_freq_base_Hz = freqA;
      #endif

      BENCH_END(vt_portamento);


      BENCH_BEGIN(vt_adsr_mod);
      float ADSRModifier = (float)adsr3_lvl[i] * ADSR3toDETUNE1_scale_f;
      float ADSRModifierOSC1 = adsr_osc1_en ? ADSRModifier : 0.0f;
      float ADSRModifierOSC2 = adsr_osc2_en ? ADSRModifier : 0.0f;
      BENCH_END(vt_adsr_mod);

      BENCH_BEGIN(vt_drift_mod);
      float DETUNE_DRIFT_OSC1 = (float)LFO_DRIFT_LEVEL[DCO_A] * drift_pitch_scale_f;
      float DETUNE_DRIFT_OSC2 = (float)LFO_DRIFT_LEVEL[DCO_B] * drift_pitch_scale_f;
      BENCH_END(vt_drift_mod);

      BENCH_BEGIN(vt_unison_mod);
      float voiceMag = (float)((i >> 1) + 1);
      float voiceSign = ((i & 0x01) == 0) ? 1.0f : -1.0f;
      float unisonMODIFIER = unisonBase * (voiceSign * voiceMag);
      BENCH_END(vt_unison_mod);


      BENCH_BEGIN(vt_modifiers);
      // Pure float mod matrix sums (1.0f = 1 Octave, 0 conversions)
      const float matrix_osc1_f = matrix_pitch_mod_f[i] + matrix_osc1_pitch_mod_f[i];
      const float matrix_osc2_f = matrix_pitch_mod_f[i] + matrix_osc2_pitch_mod_f[i];

      float modifiersBase = calcPitchbend + EPS_FLOAT + unisonMODIFIER + char_pitch_delta_f;

      // Preserving your original routing: lfo2 is hardwired to OSC2 (`freqModifiers2`),
      // while matrix/lfo1 hit both or respective oscillators:
      float freqModifiers1 = ADSRModifierOSC1 + DETUNE_DRIFT_OSC1 + modifiersBase + lfo1_osc1_f + matrix_osc1_f;
      float freqModifiers2 = ADSRModifierOSC2 + DETUNE_DRIFT_OSC2 + modifiersBase + lfo1_osc2_f + lfo2_osc2_f + matrix_osc2_f;
      BENCH_END(vt_modifiers);


      BENCH_BEGIN(vt_freq_scale_x);
      BENCH_END(vt_freq_scale_x);

      BENCH_BEGIN(vt_ratio_interp);
      float ratio1 = interpolate_live_ratio_f(freqModifiers1, DCO_A);
      float ratio2 = interpolate_live_ratio_f(freqModifiers2, DCO_B);
      BENCH_END(vt_ratio_interp);

      BENCH_BEGIN(vt_freq_scale_post);
      float freqA_Hz = freqA * ratio1;
      float freqB_Hz = freqB * (ratio2 * osc2DetuneRatio);

      #if DCO_DEBUG_REPORT
      dbg_freq_after_mod_Hz = freqA_Hz;
      #endif

      BENCH_END(vt_freq_scale_post);


      BENCH_BEGIN(vt_cross_mod);
      // Initialize BOTH frequencies for clean state defaults
      float pio_freqA_Hz = freqA_Hz;
      float pio_freqB_Hz = freqB_Hz;

      int32_t total_mod_q15 = base_xmod + m_xmod[i];

      if (total_mod_q15 > 0) {
          // Clamp
          if (total_mod_q15 > 32767) total_mod_q15 = 32767;

          // Advance Osc B shadow phase (Q32 wrap via native overflow)
          uint32_t phase = shadow_phase_osc2_q32[i];
          phase += (uint32_t)(freqB_Hz * hz_to_phase_inc);
          shadow_phase_osc2_q32[i] = phase;

          // Fast Branchless Triangle wave generation
          uint32_t p2 = phase << 1;
          uint32_t tri_u = (phase & 0x80000000) ? ~p2 : p2;
          float shadow_tri = (float)tri_u * 4.65661287e-10f - 1.0f;

          // Exponential FM
          float octaves = shadow_tri * ((float)total_mod_q15 * depth_scaler);
          pio_freqA_Hz = freqA_Hz * fast_exp2f_audio(octaves);
      }
      BENCH_END(vt_cross_mod);


      BENCH_BEGIN(vt_clk_div);
   
      uint32_t total_cycles1 = clkdiv_live_total_cycles(sysClock_Hz_cached_float, pio_freqA_Hz);
      uint32_t total_cycles2 = clkdiv_live_total_cycles(sysClock_Hz_cached_float, pio_freqB_Hz);

      uint32_t wA, kA, wB, kB;
      get_osc_params(DCO_A, wA, kA);
      get_osc_params(DCO_B, wB, kB);

      uint32_t clk_div1 = pio_clk_div_for_y(total_cycles1, osc_last_y[DCO_A], wA, kA);
      uint32_t clk_div2 = pio_clk_div_for_y(total_cycles2, osc_last_y[DCO_B], wB, kB);
      BENCH_END(vt_clk_div);


      // Prep variables for retrig 
      BENCH_BEGIN(vt_note_retrig);
      uint32_t phaseHoldX = 0;
      PioPeriod retrig_p1{};
      PioPeriod retrig_p2{};
      if (is_note_on) {
          if (oscPhaseSync > 1) {
              BENCH_FBEGIN(vt_phase_align);
              phaseHoldX = osc_phase_hold_x(total_cycles2, phaseAlignOSC2);
              BENCH_FEND(vt_phase_align);
          }
          if (oscPhaseSync >= 1 && note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
              BENCH_FBEGIN(vt_retrig_split);
              retrig_p1 = pio_period_split(total_cycles1, wA, kA);
              retrig_p2 = pio_period_split(total_cycles2, wB, kB);
              BENCH_FEND(vt_retrig_split);
          }
      }
      BENCH_END(vt_note_retrig);

      BENCH_BEGIN(vt_chan_level);
      uint16_t chanLevel, chanLevel2;
      switch (sm) {
          case 1: {
              float maxFreq = (freqA_Hz > freqB_Hz) ? freqA_Hz : freqB_Hz;
              chanLevel  = get_chan_level_for_engine(maxFreq, DCO_A);
              chanLevel2 = get_chan_level_for_engine(freqB_Hz, DCO_B);
              break;
          }
          case 2: {
              float maxFreq = (freqA_Hz > freqB_Hz) ? freqA_Hz : freqB_Hz;
              chanLevel  = get_chan_level_for_engine(freqA_Hz, DCO_A);
              chanLevel2 = get_chan_level_for_engine(maxFreq, DCO_B);
              break;
          }
          default:
              chanLevel  = get_chan_level_for_engine(freqA_Hz, DCO_A);
              chanLevel2 = get_chan_level_for_engine(freqB_Hz, DCO_B);
              break;
      }
      BENCH_END(vt_chan_level);

      BENCH_BEGIN(vt_range_pwm);
      // Calculate Range levels
      if (char_amp_mod_factor) {
        // Proportional jitter: ±5% of this specific note's level
        const int32_t amp_j_A = ((int32_t)chanLevel  * char_amp_mod_factor) >> 15;
        const int32_t amp_j_B = ((int32_t)chanLevel2 * char_amp_mod_factor) >> 15;

        RANGE_PWM[DCO_A] = character_clamp_amp((int32_t)chanLevel  + amp_j_A);
        RANGE_PWM[DCO_B] = character_clamp_amp((int32_t)chanLevel2 + amp_j_B);
      } else {
        RANGE_PWM[DCO_A] = chanLevel;
        RANGE_PWM[DCO_B] = chanLevel2;
      }
      BENCH_END(vt_range_pwm);
      BENCH_END(vt_range_pwm);

      PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
      PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
      uint8_t sm1N = VOICE_TO_SM[DCO_A];
      uint8_t sm2N = VOICE_TO_SM[DCO_B];

      if (is_note_on) {
          BENCH_BEGIN(vt_retrig_sm_apply);
          if (oscPhaseSync >= 1) {
              // Phase sync mode: Hard phase reset
              if (note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
                  uint32_t maskAB = (1u << sm1N) | (1u << sm2N);
                  pio_set_sm_mask_enabled(pioN_A, maskAB, false);

                  osc_load_periods_stopped_noclear(DCO_A, retrig_p1.y, retrig_p1.clk_div,
                                                   DCO_B, retrig_p2.y, retrig_p2.clk_div);

                  pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(osc_restart_target(DCO_A)));

                  if (phaseHoldX != 0) {
                      osc_phase_align_hold_stopped(DCO_B, phaseHoldX);
                  } else {
                      pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(osc_restart_target(DCO_B)));
                  }

                  pio_enable_sm_mask_in_sync(pioN_A, maskAB);
              } else {
                  pio_sm_put(pioN_A, sm1N, clk_div1);
                  pio_sm_put(pioN_B, sm2N, clk_div2);
                  pio_sm_exec(pioN_A, sm1N, pio_encode_pull(false, true));
                  pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, true));
                  pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(osc_restart_target(DCO_A)));
                  pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(osc_restart_target(DCO_B)));
                  osc_last_clk_div[DCO_A] = clk_div1;
                  osc_last_clk_div[DCO_B] = clk_div2;
              }
          } else {
              // =================================================================
              // FREE-RUNNING OSCILLATORS (oscPhaseSync == 0):
              // Pull new divider AND force X to take it immediately!
              // This shortens the current ramp to the new note instantly without
              // forcing a hard phase restart.
              // =================================================================
            #if defined(UPDATE_CLK_DIV_INSTANTLY)
              update_osc_clk_div_instantly(pioN_A, sm1N, DCO_A, clk_div1);
              update_osc_clk_div_instantly(pioN_B, sm2N, DCO_B, clk_div2);
            #else
              pio_sm_put(pioN_A, sm1N, clk_div1);
              pio_sm_put(pioN_B, sm2N, clk_div2);
              pio_sm_exec(pioN_A, sm1N, pio_encode_pull(false, true));
              pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, true));
            #endif

              // // Fallback for when the above doesn't work:
              // pio_sm_put(pioN_A, sm1N, clk_div1);
              // pio_sm_put(pioN_B, sm2N, clk_div2);
              // pio_sm_exec(pioN_A, sm1N, pio_encode_pull(false, true));
              // pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, true)); 
              // // instant note change:
              // pio_sm_exec(pioN_A, sm1N, pio_encode_mov(pio_x, pio_osr));
              // pio_sm_exec(pioN_B, sm1N, pio_encode_mov(pio_x, pio_osr));

              osc_last_clk_div[DCO_A] = clk_div1;
              osc_last_clk_div[DCO_B] = clk_div2;
          }
          BENCH_END(vt_retrig_sm_apply);

      } else {
          // Normal running frame: update clk_div continuously for vibrato/LFOs
          BENCH_BEGIN(vt_pio_write);
          #if defined(UPDATE_CLK_DIV_INSTANTLY)
          update_osc_clk_div_instantly(pioN_A, sm1N, DCO_A, clk_div1);
          update_osc_clk_div_instantly(pioN_B, sm2N, DCO_B, clk_div2);
          #else
          pio_sm_put(pioN_A, sm1N, clk_div1);
          pio_sm_put(pioN_B, sm2N, clk_div2);
          pio_sm_exec(pioN_A, sm1N, pio_encode_pull(false, true));
          pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, true));
          #endif
          osc_last_clk_div[DCO_A] = clk_div1;
          osc_last_clk_div[DCO_B] = clk_div2;
          BENCH_END(vt_pio_write);
      }




      if (timer99microsFlag2) {
        if (pulseWaveOn) {
          BENCH_BEGIN(vt_pwm_calc);
          const int32_t adsr3_delta = ((int32_t)adsr3_lvl[i] * (int32_t)ADSR3toPWM) >> 15;
          int32_t pw_calc = (int32_t)PW[0] + lfo2_pw_delta + adsr3_delta + (int32_t)m_pw[i] + char_pw_delta_i;
      
          // RP2350 OPTIMIZED: Truly Branchless Nested Clamp
          // Forces GCC to evaluate bounds completely in registers (IT Blocks)
          const int32_t max_pw = (int32_t)(DIV_COUNTER_PW - 1);
          pw_calc = (pw_calc < 0) ? 0 : ((pw_calc > max_pw) ? max_pw : pw_calc);
      
          PW_PWM[i] = get_PW_level_interpolated<PW_SWEEP_FULL>((uint16_t)pw_calc, DCO_A, freqA_Hz);
          BENCH_END(vt_pwm_calc);
        } else {
          PW_PWM[i] = 0;
        }
      }
  } // end loop

  BENCH_BEGIN(vt_teardown);
  flush_voice_pwm();

  last_portamento_time = portaTime;
  last_portamento_mode = portaMode;
  BENCH_END(vt_teardown);
}
#endif // USE_FLOAT_VOICE_TASK

#ifdef USE_VOICE_TASK_Q24
// Float realtime voice engine (Optimized for Cortex-M33 / RP2350)
void SRAM_HOT(voice_task_Q24)() {
  static uint32_t last_portamento_time = 0;
  static uint8_t last_portamento_mode = PORTA_MODE_SLEW;
  static uint8_t lastNote1[NUM_VOICES_TOTAL] = {};
  static uint8_t lastNote2[NUM_VOICES_TOTAL] = {};
  uint32_t portaTime = portamento_time;
  uint8_t portaMode = portamento_mode;
  bool portaTimeChanged = (portaTime != last_portamento_time);
  bool portaModeChanged = (portaMode != last_portamento_mode);

  // --------------------------------------------------------------------------
  // PRE-LOOP SETUP: Keep Modifiers in Native Q24 Integer Math
  // --------------------------------------------------------------------------
  BENCH_BEGIN(vt_pitchbend);
  // Pure Q24 integer pitchbend calc (eliminates float conversions)
  int32_t bend_normalized_q24 = ((int32_t)midi_pitch_bend << 11) - (1 << 24);
  int32_t calcPitchbend_q24 = (int32_t)(((int64_t)bend_normalized_q24 * pitchBendMultiplier_q24) >> 24);
  BENCH_END(vt_pitchbend);

  last_midi_pitch_bend = midi_pitch_bend;

  // OSC2 Detune (kept as float because it is a direct Hz output scaler)
  const float detuneSteps = (float)((int32_t)OSC2_detune - 256);
  const float osc2DetuneRatio = 1.0f + 0.0002f * detuneSteps;

  // Unison Base - Native Q24
  static constexpr int32_t UNISON_SCALE_Q24 = (int32_t)(0.0001f * (float)(1 << 24) + 0.5f);
  const int32_t unisonBase_q24 = (int32_t)unisonDetune * UNISON_SCALE_Q24;

  // PWM LFO delta
  const int32_t lfo2_pw_delta = ((int32_t)LFO2Level * (int32_t)LFO2toPW) >> 15;

// --------------------------------------------------------------------------
  // PILLAR II: Apply const volatile __restrict to match global qualifiers
  // --------------------------------------------------------------------------
  const int16_t* __restrict p_adsr3 = ADSR3Level_q15;
  const volatile int16_t* __restrict p_drift = LFO_DRIFT_LEVEL;
  const volatile int32_t* __restrict p_matrix_pitch = matrix_pitch_mod_q24;
  const volatile int32_t* __restrict p_matrix_osc1 = matrix_osc1_pitch_mod_q24;
  const volatile int32_t* __restrict p_matrix_osc2 = matrix_osc2_pitch_mod_q24;
  const volatile uint8_t* __restrict p_note1 = VOICE_NOTE_OSC1;
  const volatile uint8_t* __restrict p_note2 = VOICE_NOTE_OSC2;

  for (int i = 0; i < NUM_VOICES; ++i) {

    if (note_on_flag[i] == 1) {
      note_on_flag_flag[i] = true;
      note_on_flag[i] = 0;
      __dmb(); // Memory barrier
    }

#if DCO_DEBUG_REPORT
    float dbg_freq_base_Hz = 0.0f;
    float dbg_freq_after_mod_Hz = 0.0f;
#endif

    const uint8_t note1 = p_note1[i];
    const uint8_t note2 = p_note2[i];
    const bool pitchTargetChanged = (note1 != lastNote1[i] || note2 != lastNote2[i]);
    lastNote1[i] = note1;
    lastNote2[i] = note2;

    float noteFreq1 = sNotePitches[note1];
    float noteFreq2 = sNotePitches[note2];

    float freqA, freqB;
    const uint8_t DCO_A = (uint8_t)(i << 1);
    const uint8_t DCO_B = (uint8_t)((i << 1) | 1);

    // =========================================================================
    // PORTAMENTO (Kept in Float due to sNotePitches interaction)
    // =========================================================================
    BENCH_BEGIN(vt_portamento);
    // [Portamento logic remains completely unchanged from original]
    if (portaTime > 0) {
      uint32_t now_us = micros();
      portamentoTimer[i] = now_us - portamentoStartMicros[i];

      if (note_on_flag_flag[i]) {
        portamentoStartMicros[i] = now_us;
        portamentoTimer[i] = 0;
        float targetNoteA = (float)note1;
        float targetNoteB = (float)note2;
        porta_setup_glide_f(DCO_A, porta_resolve_start_note_f(DCO_A, targetNoteA), targetNoteA, portaMode);
        porta_setup_glide_f(DCO_B, porta_resolve_start_note_f(DCO_B, targetNoteB), targetNoteB, portaMode);
      }

      const bool portaDoRetime = (portaTimeChanged || portaModeChanged || pitchTargetChanged) && !note_on_flag_flag[i];

      float curA, curB;
      if (portaDoRetime) {
        portamentoStartMicros[i] = now_us;
        portamentoTimer[i] = 0;
        porta_setup_glide_f(DCO_A, porta_note_cur_f[DCO_A], (float)note1, portaMode);
        porta_setup_glide_f(DCO_B, porta_note_cur_f[DCO_B], (float)note2, portaMode);
        curA = porta_freq_cur_f[DCO_A];
        curB = porta_freq_cur_f[DCO_B];
      } else if (porta_note_cur_f[DCO_A] == porta_note_stop_f[DCO_A] &&
                 porta_note_cur_f[DCO_B] == porta_note_stop_f[DCO_B]) {
        curA = porta_freq_stop_f[DCO_A];
        curB = porta_freq_stop_f[DCO_B];
      } else {
        int32_t elapsed = (int32_t)portamentoTimer[i];
        float startNoteA = porta_note_start_f[DCO_A];
        float startNoteB = porta_note_start_f[DCO_B];
        float stopNoteA = porta_note_stop_f[DCO_A];
        float stopNoteB = porta_note_stop_f[DCO_B];

        float dNoteA = stopNoteA - startNoteA;
        float dNoteB = stopNoteB - startNoteB;
        float curNoteA = startNoteA + porta_note_step_f[DCO_A] * (float)elapsed;
        float curNoteB = startNoteB + porta_note_step_f[DCO_B] * (float)elapsed;

        if ((dNoteA >= 0.0f && curNoteA >= stopNoteA) || (dNoteA < 0.0f && curNoteA <= stopNoteA)) curNoteA = stopNoteA;
        if ((dNoteB >= 0.0f && curNoteB >= stopNoteB) || (dNoteB < 0.0f && curNoteB <= stopNoteB)) curNoteB = stopNoteB;

        porta_note_cur_f[DCO_A] = curNoteA;
        porta_note_cur_f[DCO_B] = curNoteB;
        curA = (curNoteA == stopNoteA) ? porta_freq_stop_f[DCO_A] : noteIndex_to_freqFloat(curNoteA);
        curB = (curNoteB == stopNoteB) ? porta_freq_stop_f[DCO_B] : noteIndex_to_freqFloat(curNoteB);
        porta_freq_cur_f[DCO_A] = curA;
        porta_freq_cur_f[DCO_B] = curB;
      }
      freqA = curA;
      freqB = curB;
    } else {
      freqA = noteFreq1;
      freqB = noteFreq2;
      porta_freq_cur_f[DCO_A] = freqA;
      porta_freq_cur_f[DCO_B] = freqB;
      porta_freq_stop_f[DCO_A] = freqA;
      porta_freq_stop_f[DCO_B] = freqB;
      porta_note_cur_f[DCO_A] = (float)note1;
      porta_note_cur_f[DCO_B] = (float)note2;
      porta_note_stop_f[DCO_A] = (float)note1;
      porta_note_stop_f[DCO_B] = (float)note2;
      porta_note_valid[DCO_A] = true;
      porta_note_valid[DCO_B] = true;
    }

#if DCO_DEBUG_REPORT
    dbg_freq_base_Hz = freqA;
#endif
    BENCH_END(vt_portamento);

    // =========================================================================
    // MODULATORS: Native Q24 Integer Chain (Bypasses Float Conversions)
    // =========================================================================
    BENCH_BEGIN(vt_adsr_mod);
    int32_t ADSRModifier_q24 = 0;
    if (ADSR3toDETUNE1_scale_q24 != 0) {
      ADSRModifier_q24 = applyDepthQ24(p_adsr3[i], ADSR3toDETUNE1_scale_q24);
    }
    int32_t ADSRModifierOSC1_q24 = (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4) ? ADSRModifier_q24 : 0;
    int32_t ADSRModifierOSC2_q24 = (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4) ? ADSRModifier_q24 : 0;
    BENCH_END(vt_adsr_mod);
    
    BENCH_BEGIN(vt_drift_mod);
    int32_t DETUNE_DRIFT_OSC1_q24 = (drift_pitch_scale_q24 != 0) ? applyDepthQ24(p_drift[DCO_A], drift_pitch_scale_q24) : 0;
    int32_t DETUNE_DRIFT_OSC2_q24 = (drift_pitch_scale_q24 != 0) ? applyDepthQ24(p_drift[DCO_B], drift_pitch_scale_q24) : 0;
    BENCH_END(vt_drift_mod);

    BENCH_BEGIN(vt_unison_mod);
    int32_t voiceMag = (i >> 1) + 1;
    // Pillar I: Branchless unison multiplier (turns odd/even to -1 or 1)
    int32_t voiceSign = 1 - ((i & 1) << 1); 
    int32_t unisonMODIFIER_q24 = unisonBase_q24 * (voiceSign * voiceMag);
    BENCH_END(vt_unison_mod);

    int32_t freqModifiers1_q24, freqModifiers2_q24;
    {
      BENCH_BEGIN(vt_modifiers);
      // All done dynamically in integer core pipeline
      int32_t modifiersBase_q24 = calcPitchbend_q24 + Q24_ONE_EPS + p_matrix_pitch[i] + unisonMODIFIER_q24;
      if (char_pitch_scale_q15) {
        modifiersBase_q24 += character_pitch_delta_q24();
      }
      
      freqModifiers1_q24 = ADSRModifierOSC1_q24 + DETUNE_DRIFT_OSC1_q24 + modifiersBase_q24 + 
                           lfo1_pitch_mod_q24[LFO1_PITCH_OSC1] + p_matrix_osc1[i];
                           
      freqModifiers2_q24 = ADSRModifierOSC2_q24 + DETUNE_DRIFT_OSC2_q24 + modifiersBase_q24 + 
                           lfo1_pitch_mod_q24[LFO1_PITCH_OSC2] + lfo2_pitch_mod_q24[LFO2_PITCH_OSC2] + p_matrix_osc2[i];
      BENCH_END(vt_modifiers);
    }

    BENCH_BEGIN(vt_ratio_interp);
    // Convert out to Float ONCE precisely at the step where we need it
    float ratio1 = interpolate_live_ratio_f(q24_to_float(freqModifiers1_q24), DCO_A);
    float ratio2 = interpolate_live_ratio_f(q24_to_float(freqModifiers2_q24), DCO_B);
    BENCH_END(vt_ratio_interp);

    BENCH_BEGIN(vt_freq_scale_post);
    float freqA_Hz = freqA * ratio1;
    float freqB_Hz = freqB * (ratio2 * osc2DetuneRatio);
#if DCO_DEBUG_REPORT
    dbg_freq_after_mod_Hz = freqA_Hz;
#endif
    BENCH_END(vt_freq_scale_post);

    // =========================================================================
    // PIO CLK_DIV & HARDWARE COMMIT (Unchanged float implementations)
    // =========================================================================
    BENCH_BEGIN(vt_clk_div);
    float correction = 0.0f;
    const uint32_t sys_hz = sysClock_Hz;
    uint32_t total_cycles1 = clkdiv_live_total_cycles(sys_hz, freqA_Hz) + (uint32_t)correction;
    uint32_t total_cycles2 = clkdiv_live_total_cycles(sys_hz, freqB_Hz) + (uint32_t)correction;

    const uint32_t wA = osc_ramp_weight(DCO_A), kA = osc_period_overhead(DCO_A);
    const uint32_t wB = osc_ramp_weight(DCO_B), kB = osc_period_overhead(DCO_B);

    uint32_t clk_div1 = pio_clk_div_for_y(total_cycles1, osc_last_y[DCO_A], wA, kA);
    uint32_t clk_div2 = pio_clk_div_for_y(total_cycles2, osc_last_y[DCO_B], wB, kB);
    BENCH_END(vt_clk_div);

    uint32_t phaseHoldX = 0;
    PioPeriod retrig_p1{};
    PioPeriod retrig_p2{};
    if (note_on_flag_flag[i] && oscPhaseSync > 1) {
      BENCH_BEGIN(vt_phase_align);
      phaseHoldX = osc_phase_hold_x(total_cycles2, phaseAlignOSC2);
      BENCH_END(vt_phase_align);
    }
    if (note_on_flag_flag[i] && oscPhaseSync >= 1 && note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
      BENCH_BEGIN(vt_retrig_split);
      retrig_p1 = pio_period_split(total_cycles1, wA, kA);
      retrig_p2 = pio_period_split(total_cycles2, wB, kB);
      BENCH_END(vt_retrig_split);
    }

    BENCH_BEGIN(vt_chan_level);
    uint16_t chanLevel, chanLevel2;
    switch (syncMode) {
    case 1: {
      float maxFreq = (freqA_Hz > freqB_Hz) ? freqA_Hz : freqB_Hz;
      chanLevel = get_chan_level_for_engine(maxFreq, DCO_A);
      chanLevel2 = get_chan_level_for_engine(freqB_Hz, DCO_B);
      break;
    }
    case 2: {
      float maxFreq = (freqA_Hz > freqB_Hz) ? freqA_Hz : freqB_Hz;
      chanLevel = get_chan_level_for_engine(freqA_Hz, DCO_A);
      chanLevel2 = get_chan_level_for_engine(maxFreq, DCO_B);
      break;
    }
    default:
      chanLevel = get_chan_level_for_engine(freqA_Hz, DCO_A);
      chanLevel2 = get_chan_level_for_engine(freqB_Hz, DCO_B);
      break;
    }
    BENCH_END(vt_chan_level);

    PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
    PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
    uint8_t sm1N = VOICE_TO_SM[DCO_A];
    uint8_t sm2N = VOICE_TO_SM[DCO_B];

    if (note_on_flag_flag[i]) {
      BENCH_BEGIN(vt_note_retrig);
      if (oscPhaseSync >= 1) {
        BENCH_BEGIN(vt_retrig_sm_apply);
        if (note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
          uint32_t maskAB = (1u << sm1N) | (1u << sm2N);
          pio_set_sm_mask_enabled(pioN_A, maskAB, false);
          osc_load_periods_stopped_noclear(DCO_A, retrig_p1.y, retrig_p1.clk_div, DCO_B, retrig_p2.y, retrig_p2.clk_div);
          pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(osc_restart_target(DCO_A)));

          if (phaseHoldX != 0) osc_phase_align_hold_stopped(DCO_B, phaseHoldX);
          else pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(osc_restart_target(DCO_B)));

          pio_enable_sm_mask_in_sync(pioN_A, maskAB);
        } else {
          pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(osc_restart_target(DCO_A)));
          pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(osc_restart_target(DCO_B)));
        }
        BENCH_END(vt_retrig_sm_apply);
      }
      BENCH_END(vt_note_retrig);
    } else {
      BENCH_BEGIN(vt_pio_write);
      pio_sm_put(pioN_A, sm1N, clk_div1);
      pio_sm_put(pioN_B, sm2N, clk_div2);
      pio_sm_exec(pioN_A, sm1N, pio_encode_pull(false, false));
      pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, false));
      osc_last_clk_div[DCO_A] = clk_div1;
      osc_last_clk_div[DCO_B] = clk_div2;
      BENCH_END(vt_pio_write);
    }

    write_range_pwm(DCO_A, chanLevel);
    write_range_pwm(DCO_B, chanLevel2);

    if (timer99microsFlag2) {
      if (pulseWaveOn) {
        BENCH_FBEGIN(vt_pwm_calc);

        const int16_t local_ADSR3toPWM = ADSR3toPWM;

        // Clean integer additions
        const int32_t adsr3_delta = ((int32_t)p_adsr3[i] * (int32_t)local_ADSR3toPWM) >> 15;
        int32_t pw_calc = (int32_t)PW[0] + lfo2_pw_delta + adsr3_delta + matrix_pw_mod[i] + (int32_t)character_pw_delta();

        // Pillar I: Branchless Ternary Clamping
        // Compiles down to an inline ARM CSEL Instruction (Conditional Select)
        pw_calc = pw_calc < 0 ? 0 : pw_calc;
        pw_calc = pw_calc > (int32_t)(DIV_COUNTER_PW - 1) ? (int32_t)(DIV_COUNTER_PW - 1) : pw_calc;

        PW_PWM[i] = (uint16_t)pw_calc;
        voice_write_pw(i, get_PW_level_interpolated(PW_PWM[i], DCO_A, freqA_Hz));

        BENCH_FEND(vt_pwm_calc);
      } else {
        voice_write_pw(i, 0);
      }
    }
  }

  // Clear flags at the end of the frame
  for (int k = 0; k < NUM_VOICES_TOTAL; k++) {
    note_on_flag_flag[k] = false;
  }

  last_portamento_time = portaTime;
  last_portamento_mode = portaMode;
}
#endif // USE_FLOAT_VOICE_TASK_Q24


// Rebuild the PIO sync topology and retrigger voices.
// Called from apply_param_sync_mode (Serial2).
void SRAM_HOT(setSyncMode)() {
  // assign_sm_mapping() keeps the slave below its master in SM index;
  // start_voice_sms() re-derives every SM's program, set pin and sideset pin
  // from syncMode and softSyncChunks, then starts them all on the same cycle.
  //
  // The old implementation poked sideset pins in place and called
  // pio_sm_restart(), which cleared the shift counters but left PC, X and Y —
  // it could strand an SM mid-loop with a stale X for one glitched period. The
  // note_on_flag retrigger below already re-pushes everything, so the restart
  // was never needed.
  assign_sm_mapping();
  start_voice_sms();

  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    note_on_flag[i] = 1;
  }
}

// One osc: shipping FIXED = Q24→Q8 lookup. With USE_FLOAT_AMP_COMP, non-FIXED
// methods take Q24→Hz then get_chan_level_by_method (cmds 20–22).
uint16_t SRAM_HOT(amp_level_q24)(int64_t freq_q24,
                                                             uint8_t osc) {
#ifdef USE_FLOAT_AMP_COMP
  if (amp_comp_method != AMP_COMP_FIXED) {
    float hz = (float)freq_q24 * (1.0f / 16777216.0f);
    return get_chan_level_by_method(hz, osc);
  }
#endif
  int32_t freqFx = (int32_t)((freq_q24 + (1LL << 15)) >> 16);
  return get_chan_level_q24_ultra_accurate(freqFx, osc);
}

// Q24 + syncMode switch + 2× lookup. SRAM so vt_chan_level is not a flash
// caller.
void SRAM_HOT(amp_chan_levels_fixed)(int64_t freq_q24_A,
                                                int64_t freq_q24_B,
                                                uint8_t oscA, uint8_t oscB,
                                                uint16_t *outA,
                                                uint16_t *outB) {
  const uint8_t sm = syncMode;
  switch (sm) {
  case 1: {
    int64_t maxAB = (freq_q24_A > freq_q24_B) ? freq_q24_A : freq_q24_B;
    *outA = get_chan_level_q24_ultra_accurate(maxAB, oscA);
    *outB = get_chan_level_q24_ultra_accurate(freq_q24_B, oscB);
    break;
  }
  case 2: {
    int64_t maxAB = (freq_q24_A > freq_q24_B) ? freq_q24_A : freq_q24_B;
    *outA = get_chan_level_q24_ultra_accurate(freq_q24_A, oscA);
    *outB = get_chan_level_q24_ultra_accurate(maxAB, oscB);
    break;
  }
  default:
    *outA = get_chan_level_q24_ultra_accurate(freq_q24_A, oscA);
    *outB = get_chan_level_q24_ultra_accurate(freq_q24_B, oscB);
    break;
  }
}


// Cached variant: pass DCO index to reuse last segment and avoid binary search
#if PITCH_INTERP_MODE == PITCH_INTERP_Q12
int32_t
SRAM_HOT(interpolatePitchMultiplierIntQ16_cached)(int32_t xQ16,
                                                             int dcoIndex) {
  int32_t xInt = xQ16 >> 16;
  if (xInt <= xMultiplierTable[0]) {
    return yMultiplierTable[0];
  }
  if (xInt >= xMultiplierTable[multiplierTableSize - 1]) {
    return yMultiplierTable[multiplierTableSize - 1];
  }
  int low = interpSegCache[dcoIndex];
  if (low < 0 || low > multiplierTableSize - 2 ||
      !(xMultiplierTable[low] <= xInt && xInt < xMultiplierTable[low + 1])) {
    if (low >= 0 && low < multiplierTableSize - 1) {
      if (xInt >= xMultiplierTable[low + 1]) {
        while (low < multiplierTableSize - 2 &&
               xInt >= xMultiplierTable[low + 1])
          low++;
      } else if (xInt < xMultiplierTable[low]) {
        while (low > 0 && xInt < xMultiplierTable[low])
          low--;
      }
    }
    if (!(low >= 0 && low < multiplierTableSize - 1 &&
          xMultiplierTable[low] <= xInt && xInt < xMultiplierTable[low + 1])) {
      int l = 0, h = multiplierTableSize - 1;
      while (l <= h) {
        int m = (l + h) >> 1;
        if (xMultiplierTable[m] <= xInt && xInt < xMultiplierTable[m + 1]) {
          low = m;
          break;
        } else if (xInt < xMultiplierTable[m]) {
          h = m - 1;
        } else {
          l = m + 1;
        }
      }
      if (low < 0)
        low = 0;
      if (low > multiplierTableSize - 2)
        low = multiplierTableSize - 2;
    }
    interpSegCache[dcoIndex] = (int16_t)low;
  }
  int32_t x0 = xMultiplierTable[low];
  int32_t y0 = yMultiplierTable[low];
  int32_t deltaQ12 = (xQ16 - (x0 << 16)) >> 4;
  int32_t y =
      y0 +
      (int32_t)((((int64_t)deltaQ12 * (int64_t)slopeQ12[low]) + (1LL << 23)) >>
                24);
  return y;
}
#endif // Q12

#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
int32_t SRAM_HOT(interpolateRatioQ16_fast)(int32_t xQ16) {
  // Endpoints match your [-1.0, 3.0] domain
  static constexpr int32_t MIN_X_Q16 = -196608; // -3.0 in Q16 (-3 * 65536)
  static constexpr int32_t MAX_X_Q16 =  327680; // +5.0 in Q16 ( 5 * 65536)
  
  // 1. Pure Hardware Integer Clamping (Zero branches, 1-cycle IT block)
  xQ16 = (xQ16 < MIN_X_Q16) ? MIN_X_Q16 : xQ16;
  xQ16 = (xQ16 > MAX_X_Q16) ? MAX_X_Q16 : xQ16;

  // 2. Shift domain to positive [0, 4.0] in Q16
  // Max value is exactly 262144 (4.0 * 65536)
  uint32_t phase = (uint32_t)(xQ16 - MIN_X_Q16);

  // 3. Map X directly to the Q16 Index Domain.
  // Formula: phase * (size - 1) / Span
  // Because your span is exactly 4.0, dividing by 4 is just a bitshift (>> 2).
  // (If your span was 8.0, you would shift by >> 3).
  uint32_t mapped = (phase * (multiplierTableSize - 1)) >> 3;

  // 4. Extract index and fraction (1-cycle bitwise ops)
  uint32_t idx = mapped >> 16;
  int32_t frac = mapped & 0xFFFF; // The LERP slope weight!
  
  // Safety upper-clamp to prevent reading past the end of the array
  const uint32_t MAX_IDX = multiplierTableSize - 2;
  idx = (idx > MAX_IDX) ? MAX_IDX : idx;

  // 5. Array lookup
  // WE ONLY NEED THE Y TABLE! No X table, no Slope table.
  const int32_t* __restrict yTable = yMultiplierTable;
  int32_t y0 = yTable[idx];
  int32_t y1 = yTable[idx + 1];

  // 6. Hardware DSP Multiply-Accumulate + Q16 Rounding (+32768)
  return y0 + (((y1 - y0) * frac + 32768) >> 16);
}
#endif // PITCH_INTERP_RATIO_Q16

#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_CACHED
// Trunc+clamp±1 find; same lerp as walk. Keep ±1 even when walk_steps≈0 (live
// ballast). noinline: isolate codegen from voice_task_float (distinct SRAM
// symbol).
float SRAM_HOT(interpolateRatioFloat_cached_fast)(float x, int dcoIndex) {
  // 1. Updated Bounds: Table now covers -3.0f to 5.0f
  if (__builtin_expect(x <= -3.0f, 0)) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTableF[0];
  }
  if (__builtin_expect(x >= 5.0f, 0)) {
    BENCH_PATH_INC(ratio_clamp);
    return yMultiplierTableF[multiplierTableSize - 1];
  }

  // 2. Span is now 8.0f (5.0 - (-3.0) = 8.0)
  static constexpr float kPitchInvDx = (float)multiplierTableSize / 8.0f;
  const uint32_t lastSeg = multiplierTableSize - 2;
  
  uint32_t low = (uint32_t)interpSegCache[dcoIndex];
  float x_low;

  if (__builtin_expect(low <= lastSeg && x >= (x_low = xMultiplierTableF[low]) && x < xMultiplierTableF[low + 1], 1)) {
    BENCH_PATH_INC(ratio_hit);
  } 
  else {
    // 3. Offset mapping changes to +3.0f to map x=-3.0 to index 0
    uint32_t cand = (uint32_t)((x + 3.0f) * kPitchInvDx);
    if (cand > lastSeg) cand = lastSeg;

    float c_next_x = xMultiplierTableF[cand + 1];
    
#if defined(BENCH_PATH_STATS)
    uint32_t steps = 0;
#endif

    if (cand < lastSeg && x >= c_next_x) {
      ++cand;
      x_low = c_next_x;
#if defined(BENCH_PATH_STATS)
      steps = 1;
#endif
    } else {
      float c_x = xMultiplierTableF[cand];
      if (cand > 0 && x < c_x) {
        --cand;
        x_low = xMultiplierTableF[cand];
#if defined(BENCH_PATH_STATS)
        steps = 1;
#endif
      } else {
        x_low = c_x;
      }
    }
    
    low = cand;
    BENCH_PATH_INC(ratio_miss_direct);
#if defined(BENCH_PATH_STATS)
    bench_path_walk_steps(steps);
#endif
    interpSegCache[dcoIndex] = (int16_t)low;
  }

  // Hardware FMA Intrinsic executing immediately from registers
  return __builtin_fmaf(slopeF[low], x - x_low, yMultiplierTableF[low]);
}

#endif // PITCH_INTERP_FLOAT_CACHED

#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT

// Pre-calculate the scale factor to convert 'x' to array index
static constexpr float PITCH_MIN_X = -3.0f;
static constexpr float PITCH_MAX_X = 5.0f;
// Scale = (Size - 1) / Span
static constexpr float PITCH_TABLE_SCALE = (float)(multiplierTableSize - 1) / 8.0f;

// NOTE: dcoIndex is removed. We don't need state/cache anymore!
float SRAM_HOT(interpolateRatioFloat_fast)(float x) {
  // 1. Hardware clamp (Zero branches, VMAXNM/VMINNM)
  float clamped_x = __builtin_fmaxf(PITCH_MIN_X, __builtin_fminf(x, PITCH_MAX_X));

  // 2. Map X directly to the table index domain (0.0 to MaxIndex)
  float mapped_idx = (clamped_x - PITCH_MIN_X) * PITCH_TABLE_SCALE;

  // 3. Fast float-to-int cast
  uint32_t idx = (uint32_t)mapped_idx;

  // 4. Branchless Upper Bound Safety
  const uint32_t MAX_IDX = multiplierTableSize - 2;
  idx = (idx >= MAX_IDX) ? MAX_IDX : idx;

  // 5. Extract the fractional remainder [0.0f - 1.0f] for LERP
  float frac = mapped_idx - (float)idx;

  // 6. Direct lookup and LERP
  // We only need the Y table now. No X-table or Slope-table needed!
  const float* __restrict yTable = yMultiplierTableF;
  float y0 = yTable[idx];
  float y1 = yTable[idx + 1];

  // Hardware Fused Multiply-Add: y0 + (y1 - y0) * frac
  return __builtin_fmaf(y1 - y0, frac, y0);
}
#endif // PITCH_INTERP_FLOAT


// Build integer/float pitch-multiplier tables (boot). Called from init_voices().
void initMultiplierTables() {
  float y_value;
  const double divisor = (double)(multiplierTableSize - 1);
  const double fraction = 8.00d / divisor;

  for (int i = 0; i < multiplierTableSize; i++) {
      double x = -3.00d + (fraction * (double)i);

      if (i == 0) {
          x = -3.00d;
          y_value = 0.0625d; // 2^(-4) = -4 octaves
      } else if (i == multiplierTableSize - 1) {
          x = 5.0d;
          y_value = 16.0d;   // 2^(4) = +4 octaves
      } else {
          y_value = expInterpolationSolveY(x + 1.00d, 1.00d, 3.00d, 0.50d, 2.00d);
      }

#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT
      // ---------------------------------------------------------------------
      // FAST FLOAT PATH (RP2350 default): Only populate the Y float table!
      // ---------------------------------------------------------------------
      yMultiplierTableF[i] = y_value;

#elif PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
      // ---------------------------------------------------------------------
      // FAST Q16 PATH (RP2040 default): Only populate the Y Q16 table!
      // ---------------------------------------------------------------------
      yMultiplierTable[i] = (int32_t)((double)y_value * 65536.0 + 0.5);

#elif PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_CACHED
      // LEGACY FLOAT CACHED PATH: Needs both X and Y
      xMultiplierTableF[i] = (float)x;
      yMultiplierTableF[i] = y_value;

#elif PITCH_INTERP_MODE == PITCH_INTERP_Q12
      // LEGACY Q12 PATH
      xMultiplierTable[i] = (int32_t)(x * (double)multiplierTableScale);
      yMultiplierTable[i] = (int32_t)(y_value * (double)multiplierTableScale);
#endif
  }

  // =========================================================================
  // SLOPE COMPUTATION (Only required for legacy cached/search methods)
  // =========================================================================

#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_CACHED
  for (int i = 0; i < multiplierTableSize - 1; ++i) {
      float dxF = xMultiplierTableF[i + 1] - xMultiplierTableF[i];
      if (dxF == 0.0f) dxF = 1.0f;
      slopeF[i] = (yMultiplierTableF[i + 1] - yMultiplierTableF[i]) / dxF;
  }
  for (int d = 0; d < NUM_OSCILLATORS; ++d) {
      interpSegCache[d] = -1;
  }

#elif PITCH_INTERP_MODE == PITCH_INTERP_Q12
  for (int i = 0; i < (multiplierTableSize - 1); ++i) {
      int32_t dx = xMultiplierTable[i + 1] - xMultiplierTable[i];
      if (dx == 0) dx = 1;
      int32_t dy = yMultiplierTable[i + 1] - yMultiplierTable[i];
      int64_t numSlope12 = ((int64_t)dy << 12) + (dx > 0 ? dx / 2 : -dx / 2);
      slopeQ12[i] = (int32_t)(numSlope12 / (int64_t)dx);
  }
  for (int d = 0; d < NUM_OSCILLATORS; ++d) {
      interpSegCache[d] = -1;
  }
#endif
  // PITCH_INTERP_FLOAT and PITCH_INTERP_RATIO_Q16 require ZERO slope precomputations!
}