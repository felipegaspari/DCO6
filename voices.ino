#include "settings.h"
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
// -----------------------------------------------------------------------------
// HARDWARE LATENCY DOMAINS
// -----------------------------------------------------------------------------
// In the ultra-optimized version, snapshot-to-swap takes ~22 CPU cycles.
static constexpr float BASE_PIPELINE_LATENCY = 22.0f; 

// TUNING CALIBRATION TRIM:
// If pitch goes SHARP under extreme modulation -> DECREASE this value (e.g. -2.0f, -4.0f)
// If pitch goes FLAT under extreme modulation  -> INCREASE this value (e.g. +2.0f, +4.0f)
static constexpr float LATENCY_CALIBRATION_TRIM = 16.0f; 

// Combined pre-scale latency applied in the OLD frequency domain
static constexpr float TOTAL_PRE_SCALE_LATENCY = BASE_PIPELINE_LATENCY + LATENCY_CALIBRATION_TRIM;

// Post-swap PIO hardware stalls: PULL (1) + MOV_X_OSR (1) = 2 cycles
static constexpr int32_t PRE_SWAP_STALL_CYCLES = 2;
static constexpr float   ROUND_STALL_OFFSET    = -1.5f; // (+0.5f round - 2.0f stall)

// Safety thresholds
static constexpr int32_t SAFEGUARD_CYCLES      = 120;
static constexpr int32_t MIN_NEW_X_CYCLES      = 36;
// Maximum voice allocation (scale to your actual synth voice count)
static constexpr size_t  MAX_SYNTH_OSCS          = NUM_OSCILLATORS; 

// Packed 16-byte cache-aligned state for single-cycle LDRD/STRD instructions
struct alignas(16) OscState {
    uint32_t last_div;    ///< Previous clock divider
    float    inv_div;     ///< 1.0f / last_div (converts VDIV to 1-cycle VMUL)
    float    frac_carry;  ///< Sub-cycle fractional phase carry
};

// Placed in fast SRAM (.time_critical / .scratch_y)
static OscState osc_state[MAX_SYNTH_OSCS] __attribute__((section(".scratch_y")));

static inline void SRAM_HOT(update_osc_clk_div_instantly)(PIO pio, uint sm, uint8_t osc, uint32_t new_div) {
    OscState* const __restrict st = &osc_state[osc];
    const uint32_t old_div = st->last_div;

    // 1. Pitch unchanged: zero cycles stolen, perfect pitch stability
    if (__builtin_expect(old_div == new_div, 1)) return;

    io_rw_32*    const txf   = &pio->txf[sm];
    pio_sm_hw_t* const sm_hw = &pio->sm[sm];

    // 2. Uninitialized startup
    if (__builtin_expect(old_div == 0, 0)) {
        st->last_div   = new_div;
        st->inv_div    = 1.0f / (float)new_div;
        st->frac_carry = 0.0f;
        *txf = new_div;
        sm_hw->instr = PIO_INSTR_PULL;
        return;
    }

    // 3. Ratio pre-calculated via reciprocal multiply (1 cycle)
    const float ratio = (float)new_div * st->inv_div;

    // 4. Snapshot current countdown
    sm_hw->instr = PIO_INSTR_IN_X_32;
    sm_hw->instr = PIO_INSTR_PUSH;
    const int32_t current_x = (int32_t)pio->rxf[sm];

    // 5. Safeguard check
    if (__builtin_expect(current_x > SAFEGUARD_CYCLES, 1)) {
        
        // PRE-SCALE COMPENSATION:
        // Subtracted in float to maintain fractional cycle precision across updates!
        const float eff_x = (float)current_x - TOTAL_PRE_SCALE_LATENCY;

        // Sub-cycle phase tracking
        const float exact_x = (eff_x * ratio) + st->frac_carry;
        const int32_t new_x = (int32_t)(exact_x + ROUND_STALL_OFFSET);

        if (__builtin_expect(new_x > MIN_NEW_X_CYCLES, 1)) {
            // --- CRITICAL PATH: Push ONLY new_x and swap immediately ---
            *txf = (uint32_t)new_x;
            sm_hw->instr = PIO_INSTR_PULL;       // OSR = new_x
            sm_hw->instr = PIO_INSTR_MOV_X_OSR;  // X = new_x (SWAP COMPLETE)

            // --- POST-CRITICAL PATH: Queue new_div for next period ---
            *txf = new_div;
            sm_hw->instr = PIO_INSTR_PULL;       // OSR = new_div

            // Update sub-cycle fractional carry
            st->frac_carry = exact_x - (float)(new_x + PRE_SWAP_STALL_CYCLES);
            st->last_div   = new_div;
            st->inv_div    = 1.0f / (float)new_div;
            return;
        }
    }

    // Fallback: Safe cycle-boundary update
    *txf = new_div;
    sm_hw->instr = PIO_INSTR_PULL;
    st->last_div = new_div;
    st->inv_div  = 1.0f / (float)new_div;
}

uint32_t get_osc_clk_div(uint8_t osc, float freqHz) {
  
  uint32_t total_cycles1 = clkdiv_live_total_cycles(sysClock_Hz_cached_float, freqHz);
  
  uint32_t wA, kA, wB, kB;
  get_osc_params(osc, wA, kA);
  
  uint32_t clk_div = pio_clk_div_for_y(total_cycles1, osc_last_y[osc], wA, kA);
  
  return clk_div;
}

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
  // We capture this once and reuse it across the entire task (including portamento).
  // =========================================================================
  const uint32_t now_us_task = timer_hw->timelr;
  
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
  // OPTIMIZATION: Pitchbend calculation caching
  // Only recalculate floating-point scaling when MIDI pitch bend actually changes.
  static constexpr float INV_8192 = 1.0f / 8192.0f;
  static int32_t s_last_midi_pb = -1;
  static float   s_cached_pitchbend = 0.0f;

  const int32_t cur_midi_pb = (int32_t)midi_pitch_bend;
  if (__builtin_expect(cur_midi_pb != s_last_midi_pb, 0)) {
      s_last_midi_pb = cur_midi_pb;
      s_cached_pitchbend = ((float)cur_midi_pb - 8192.0f) * INV_8192 * pitchBendMultiplier;
  }
  const float calcPitchbend = s_cached_pitchbend;
  last_midi_pitch_bend = cur_midi_pb;
  BENCH_END(vt_pitchbend);


  BENCH_BEGIN(vt_task_prep);
  // =========================================================================
  // OPTIMIZATION PILLAR II: Register Hoisting & Restrict Pointers
  // Corrected: Uses 'const volatile float*' to match multicore definitions in
  // mod_matrix_engine.h while retaining __restrict.
  // =========================================================================
  const volatile uint8_t*  __restrict vn_osc1     = VOICE_NOTE_OSC1;
  const volatile uint8_t*  __restrict vn_osc2     = VOICE_NOTE_OSC2;
  const volatile float*    __restrict m_pitch_f   = matrix_pitch_mod_f;
  const volatile float*    __restrict m_osc1_f    = matrix_osc1_pitch_mod_f;
  const volatile float*    __restrict m_osc2_f    = matrix_osc2_pitch_mod_f;
  const int32_t*           __restrict m_pw        = (const int32_t*)matrix_pw_mod;
  const int32_t*           __restrict m_xmod      = (const int32_t*)matrix_xmod_mod;
  const volatile int16_t*  __restrict adsr3_lvl   = ADSR3Level_q15_volatile;
  volatile uint8_t*        __restrict n_on_flag   = note_on_flag;
  volatile bool*           __restrict n_on_flag_f = note_on_flag_flag;

  const float masterTuning_local = masterTuning_f;
  const float OSC1_detune_local  = OSC1_detune_f;
  const float OSC2_detune_local  = OSC2_detune_f;

  // 2. Unison Base & Precalculated Weights (Scalable up to 16 voices)
  static constexpr float UNISON_SCALE = 0.0001f;
  const float unisonBase = (float)unisonDetune * UNISON_SCALE;
  static constexpr float UNISON_VOICE_WEIGHTS[16] = {
      1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f, -4.0f,
      5.0f, -5.0f, 6.0f, -6.0f, 7.0f, -7.0f, 8.0f, -8.0f
  };

  // 3. Global LFOs
  const float lfo1_osc1_f = lfo1_pitch_mod_f[LFO1_PITCH_OSC1];
  const float lfo1_osc2_f = lfo1_pitch_mod_f[LFO1_PITCH_OSC2];
  const float lfo2_osc2_f = lfo2_pitch_mod_f[LFO2_PITCH_OSC2];

  // 4. Constant Epsilon
  static constexpr float EPS_FLOAT = (float)Q24_ONE_EPS * (1.0f / 16777216.0f);

  // 5. Global PWM LFO delta & Base PWM Pre-calculation
  const int32_t lfo2_pw_delta = ((int32_t)LFO2Level * (int32_t)LFO2toPW) >> 15;

  // Character Engine Output
  const float char_pitch_delta_f = char_pitch_scale_q15 ? character_pitch_delta_float() : 0.0f;
  const int32_t char_pw_delta_i = (int32_t)character_pw_delta();
  const int32_t char_amp_mod_factor  = char_amp_scale_q15   ? character_amp_delta()         : 0;

  // Hoist loop-invariant additions out of the voice loop
  const float global_mods = calcPitchbend + EPS_FLOAT + char_pitch_delta_f + masterTuning_local;
  const int32_t base_pw_val = (int32_t)PW[0] + lfo2_pw_delta + char_pw_delta_i;

  // ADSR Routing flags (computed once outside the loop to eliminate branch mispredictions)
  const bool adsr_osc1_en = (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4);
  const bool adsr_osc2_en = (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4);

  //  --- XMOD  ----
  // Pre-calculate loop constants (folded 0.000001f * 4294967296.0f)
  const float hz_to_phase_inc = (float)dt_us * 4294.967296f; // Multiplier to map Hz to Q32 Phase
  // ----------------------------

  const uint8_t sm = syncMode;

    // NEW: Pre-calculate PIO parameters for ALL 8 oscillators once!
  // This removes 8 function calls, 8 branches, and 8 array lookups from the inner loop.
  const uint32_t sync_chunks = soft_sync_chunks_clamped();
  const uint32_t sync_w = PIO_RAMP_WEIGHT_BY_CHUNKS[sync_chunks];
  const uint32_t sync_k = PIO_PERIOD_OVERHEAD_BY_CHUNKS[sync_chunks];

  uint32_t osc_w[NUM_OSCILLATORS];
  uint32_t osc_k[NUM_OSCILLATORS];

  _Pragma("GCC unroll 4")
  for (int j = 0; j < NUM_OSCILLATORS; ++j) {
      if (osc_uses_sync_program[j]) {
          osc_w[j] = sync_w;
          osc_k[j] = sync_k;
      } else {
          osc_w[j] = PIO_RAMP_WEIGHT_FREE;
          osc_k[j] = PIO_PERIOD_OVERHEAD_FREE;
      }
  }

  BENCH_END(vt_task_prep);

  // =========================================================================
  // OPTIMIZATION PILLAR VI: Loop Unrolling
  // Amortize SUBS/BNE branch overhead on short loops running 4-16 iterations.
  // With NUM_VOICES=4, this completely collapses loop branch overhead.
  // =========================================================================
  _Pragma("GCC unroll 4")
  for (int i = 0; i < NUM_VOICES; ++i) {

    BENCH_BEGIN(vt_loop_prep);
      
    // OPTIMIZATION 1: Multicore Safe "Load-Acquire" Atomic Exchange
    // Double-checked acquire: standard 1-cycle LDRB check first to avoid locking 
    // the system bus fabric when no note is pressed, but guarantees atomic
    // synchronization when Core 0 triggers a note-on event.
    bool is_note_on = false;
    if (__builtin_expect(n_on_flag[i] != 0, 0)) {
        is_note_on = (__atomic_exchange_n(&n_on_flag[i], 0, __ATOMIC_ACQUIRE) == 1);
    }

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

    const float noteFreq1 = sNotePitches[note1];
    const float noteFreq2 = sNotePitches[note2];
    float freqA, freqB;

    // OPTIMIZATION 4: Single-cycle bitwise math
    // Replaces multiplication and addition with direct bit-shifts and ORs
    const uint8_t DCO_A = (uint8_t)(i << 1);       // i * 2
    const uint8_t DCO_B = (uint8_t)((i << 1) | 1); // i * 2 + 1

    BENCH_END(vt_loop_prep);


    BENCH_BEGIN(vt_portamento);
    if (portaTime > 0) {
        // Reused now_us_task: Eliminates blocking APB bus read of timer_hw per voice
        const uint32_t now_us = now_us_task;
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
    // OPTIMIZATION: 1-cycle table lookup replaces bit-shifts, float conversions, and branches
    float unisonMODIFIER = unisonBase * UNISON_VOICE_WEIGHTS[i & 0x0F];
    BENCH_END(vt_unison_mod);


    BENCH_BEGIN(vt_modifiers);
    // Pure float mod matrix sums (1.0f = 1 Octave, 0 conversions)
    // Uses hoisted restricted pointers and pre-adds common pitch term
    const float p_mod = m_pitch_f[i];
    const float matrix_osc1_f = p_mod + m_osc1_f[i];
    const float matrix_osc2_f = p_mod + m_osc2_f[i];

    float modifiersBase = global_mods + unisonMODIFIER;

    // lfo2 is hardwired to OSC2 (`freqModifiers2`),
    // while matrix/lfo1 hit both or respective oscillators:
    float freqModifiers1 = ADSRModifierOSC1 + DETUNE_DRIFT_OSC1 + modifiersBase + lfo1_osc1_f + matrix_osc1_f + OSC1_detune_local;
    float freqModifiers2 = ADSRModifierOSC2 + DETUNE_DRIFT_OSC2 + modifiersBase + lfo1_osc2_f + lfo2_osc2_f + matrix_osc2_f + OSC2_detune_local;
    BENCH_END(vt_modifiers);


    BENCH_BEGIN(vt_freq_scale_x);
    BENCH_END(vt_freq_scale_x);

    BENCH_BEGIN(vt_ratio_interp);
    // for testing and debug
    // float bipolar_offset =  freqModifiers1 - 1.0f; // Range: [-1.0f .. +1.0f]
    // float ratio1 = exp2f(freqModifiers1 - 1.0f);
    float ratio1 = interpolate_live_ratio_f(freqModifiers1, DCO_A);
    float ratio2 = interpolate_live_ratio_f(freqModifiers2, DCO_B);
    BENCH_END(vt_ratio_interp);

    BENCH_BEGIN(vt_freq_scale_post);
    float freqA_Hz = freqA * ratio1;
    float freqB_Hz = freqB * ratio2;

    #if DCO_DEBUG_REPORT
    dbg_freq_after_mod_Hz = freqA_Hz;
    #endif

    BENCH_END(vt_freq_scale_post);

    BENCH_BEGIN(vt_cross_mod);

    float pio_freqA_Hz = freqA_Hz;
    float pio_freqB_Hz = freqB_Hz;

    if (sm == 2) {
        // SYNC MODE 2: Osc A is Master, Osc B is Slave
        // Master (A) modulates Slave (B) -> Target pio_freqB_Hz!
        pio_freqB_Hz = apply_crossmod(i, freqB_Hz, freqA_Hz, hz_to_phase_inc, m_xmod[i]);
    } else {
        // DEFAULT / SYNC MODE 1: Osc B is Master (or Sync Off)
        // Osc B modulates Osc A -> Target pio_freqA_Hz!
        pio_freqA_Hz = apply_crossmod(i, freqA_Hz, freqB_Hz, hz_to_phase_inc, m_xmod[i]);
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
// 1. Do the math unconditionally. 
// On the Cortex-M33, these multiplications and shifts execute in CPU registers 
// in exactly 1-2 clock cycles. No stack spilling to SRAM required.
const int32_t amp_j_A = ((int32_t)chanLevel  * char_amp_mod_factor) >> 15;
const int32_t amp_j_B = ((int32_t)chanLevel2 * char_amp_mod_factor) >> 15;

// 2. Hardware Saturation (See note below!)
const int32_t char_val_A = character_clamp_amp((int32_t)chanLevel  + amp_j_A);
const int32_t char_val_B = character_clamp_amp((int32_t)chanLevel2 + amp_j_B);

// 3. Branchless Assignment (Ternary Operator)
// The compiler translates this into an ARM "IT" (If-Then) block and a "MOV" instruction.
// There is NO branch, NO pipeline flush, and execution time is 100% deterministic (zero jitter).
RANGE_PWM[DCO_A] = character ? char_val_A : chanLevel;
RANGE_PWM[DCO_B] = character ? char_val_B : chanLevel2;
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
        // Hoisted base_pw_val eliminates 2 redundant additions per voice
        int32_t pw_calc = base_pw_val + adsr3_delta + (int32_t)m_pw[i];

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