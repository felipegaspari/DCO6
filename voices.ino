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

//===================================================================================================
// TRIANGLE WAVEFORM BAKING FUNCTIONS
//===================================================================================================

#define STATE_UP   1  // Pin HIGH -> DG411 Switch Open (Ramp Up)
#define STATE_DOWN 0  // Pin LOW -> DG411 Switch Closed (Ramp Down)
#define PIO_OVERHEAD 4 // pull(1) + out_pins(1) + out_y(1) + jmp(1) = 4 cycles

//int32_t symmetry_trim = (int32_t)test1; 

static inline uint32_t pack_segment(uint32_t state, uint32_t cycles) {
    if (cycles < PIO_OVERHEAD) cycles = PIO_OVERHEAD; // Minimum is 4 cycles
    uint32_t y_val = cycles - PIO_OVERHEAD;
    
    // PIO 'out' shifts RIGHT. Bit 0 holds state, remaining bits hold Y.
    return (y_val << 1) | (state & 1);
}

// Greatest odd integer <= x. x is Q8. Used to pick the fold band's slope.
static inline int fold_left_odd(int32_t x_q8) {
    int32_t floor_x;
    if (x_q8 >= 0) {
        floor_x = x_q8 >> 8;
    } else {
        floor_x = -(((-x_q8) + 255) >> 8);
    }
    if (floor_x & 1) return (int)floor_x;
    return (int)floor_x - 1;
}

// Serial fold into ±1. Slope is constant between odd integers:
// left-odd % 4 == 3 ramps up as the input rises, == 1 ramps down.
static inline uint8_t fold_dir_from_left_odd(int left) {
    int m = left % 4;
    if (m < 0) m += 4;
    return (m == 3) ? (uint8_t)STATE_UP : (uint8_t)STATE_DOWN;
}

// |test2| folds whichever test4 carrier is selected.
// Slider is param 251, -4096..4096. Negative mirrors positive:
//   |test2| 0..4096 maps g linearly from 1.0 to 3.0.
// |test5| 0..5 picks the transfer. All styles start the walk at -x_off
// so g = 1 matches the unfolded carrier:
//   0 serial odd (±1,±3,±5,±7)
//   1 even integers (±2,±4,±6)
//   2 positive-only (odd k, x_hit > 0)
//   3 negative-only (odd k, x_hit < 0)
//   4 late / skip-inner (|k|>=3, q = (2g-1)·x/x_peak)
//   5 bias (q += (g-1)/2)
// Hairpins shorter than ~100 us become an 8-pair chamfer.
static void wave_emit(WaveSeg* seg, int* n, uint8_t dir, uint32_t cycles) {
    if (cycles == 0 || *n >= DCO_WAVE_SLOTS) return;
    if (*n > 0 && seg[*n - 1].dir == dir) {
        seg[*n - 1].cycles += cycles;
        return;
    }
    seg[*n].dir = dir;
    seg[*n].cycles = cycles;
    (*n)++;
}

static void wave_emit_pair(WaveSeg* seg, int* n, uint32_t pair_len, uint32_t up_q8) {
    if (up_q8 > 256) up_q8 = 256;
    uint32_t up = (uint32_t)(((uint64_t)pair_len * up_q8) >> 8);
    uint32_t dn = pair_len - up;
    if (up > 0 && up < PIO_OVERHEAD) { dn += up; up = 0; }
    if (dn > 0 && dn < PIO_OVERHEAD) { up += dn; dn = 0; }
    wave_emit(seg, n, STATE_UP, up);
    wave_emit(seg, n, STATE_DOWN, dn);
}

static void wave_seal(WaveSeg* seg, int n, uint32_t t_up, uint32_t t_down) {
    uint32_t up = 0;
    uint32_t dn = 0;
    int last_up = -1;
    int last_dn = -1;
    for (int i = 0; i < n; i++) {
        if (seg[i].dir == STATE_UP) {
            up += seg[i].cycles;
            last_up = i;
        } else {
            dn += seg[i].cycles;
            last_dn = i;
        }
    }
    if (last_up >= 0 && up != t_up) {
        const int32_t c = (int32_t)seg[last_up].cycles + ((int32_t)t_up - (int32_t)up);
        if (c >= (int32_t)PIO_OVERHEAD) seg[last_up].cycles = (uint32_t)c;
    }
    if (last_dn >= 0 && dn != t_down) {
        const int32_t c = (int32_t)seg[last_dn].cycles + ((int32_t)t_down - (int32_t)dn);
        if (c >= (int32_t)PIO_OVERHEAD) seg[last_dn].cycles = (uint32_t)c;
    }
}

static void wave_absorb_shorts(WaveSeg* seg, int* n) {
    for (int guard = 0; guard < DCO_WAVE_SLOTS; guard++) {
        int short_i = -1;
        for (int i = 0; i < *n; i++) {
            if (seg[i].cycles > 0 && seg[i].cycles < PIO_OVERHEAD) {
                short_i = i;
                break;
            }
        }
        if (short_i < 0) break;
        int host = -1;
        for (int j = *n - 1; j >= 0; --j) {
            if (j == short_i || seg[j].dir != seg[short_i].dir || seg[j].cycles == 0) continue;
            host = j;
            break;
        }
        if (host < 0) break;
        seg[host].cycles += seg[short_i].cycles;
        for (int j = short_i + 1; j < *n; j++) seg[j - 1] = seg[j];
        (*n)--;
    }
}

static int bake_base_shape(WaveSeg* seg, uint8_t shape, uint32_t t_up, uint32_t t_down) {
    int n = 0;
    uint32_t up_accum = 0;
    uint32_t dn_accum = 0;

    if (shape == 0) {
        wave_emit(seg, &n, STATE_UP, t_up);
        wave_emit(seg, &n, STATE_DOWN, t_down);
    } else if (shape == 1) {
        const uint32_t up_50 = (t_up * 50) / 100;
        const uint32_t up_25 = (t_up * 25) / 100;
        const uint32_t up_rem = t_up - (up_50 + up_25);
        const uint32_t dn_50 = (t_down * 50) / 100;
        const uint32_t dn_25 = (t_down * 25) / 100;
        const uint32_t dn_rem = t_down - (dn_50 + dn_25);
        wave_emit(seg, &n, STATE_UP,   up_50);
        wave_emit(seg, &n, STATE_DOWN, dn_25);
        wave_emit(seg, &n, STATE_UP,   up_25 + up_rem);
        wave_emit(seg, &n, STATE_DOWN, dn_50);
        wave_emit(seg, &n, STATE_UP,   up_25);
        wave_emit(seg, &n, STATE_DOWN, dn_25 + dn_rem);
    } else if (shape == 2) {
        const uint32_t up_fast = (t_up * 4) / 100;
        const uint32_t dn_fast = (t_down * 4) / 100;
        for (int j = 0; j < 7; j++) {
            wave_emit(seg, &n, STATE_UP,   up_fast); up_accum += up_fast;
            wave_emit(seg, &n, STATE_DOWN, dn_fast); dn_accum += dn_fast;
        }
        wave_emit(seg, &n, STATE_UP,   t_up - up_accum);
        wave_emit(seg, &n, STATE_DOWN, t_down - dn_accum);
    } else if (shape == 3) {
        const uint32_t up_big = (t_up * 22) / 100;
        const uint32_t dn_sml = (t_down * 5) / 100;
        for (int j = 0; j < 4; j++) {
            wave_emit(seg, &n, STATE_UP,   up_big); up_accum += up_big;
            wave_emit(seg, &n, STATE_DOWN, dn_sml); dn_accum += dn_sml;
        }
        const uint32_t up_sml = (t_up * 3) / 100;
        const uint32_t dn_big = (t_down * 20) / 100;
        for (int j = 0; j < 3; j++) {
            wave_emit(seg, &n, STATE_UP,   up_sml); up_accum += up_sml;
            wave_emit(seg, &n, STATE_DOWN, dn_big); dn_accum += dn_big;
        }
        wave_emit(seg, &n, STATE_UP,   t_up - up_accum);
        wave_emit(seg, &n, STATE_DOWN, t_down - dn_accum);
    } else if (shape == 4) {
        const uint32_t u1 = (t_up * 35) / 100;   const uint32_t d1 = (t_down * 2) / 100;
        const uint32_t u2 = (t_up * 2) / 100;    const uint32_t d2 = (t_down * 9) / 100;
        const uint32_t u3 = (t_up * 13) / 100;   const uint32_t d3 = (t_down * 2) / 100;
        const uint32_t u4 = (t_up * 35) / 100;   const uint32_t d4 = (t_down * 2) / 100;
        const uint32_t u5 = (t_up * 2) / 100;    const uint32_t d5 = (t_down * 35) / 100;
        const uint32_t u6 = (t_up * 9) / 100;    const uint32_t d6 = (t_down * 2) / 100;
        const uint32_t u7 = (t_up * 2) / 100;    const uint32_t d7 = (t_down * 13) / 100;
        wave_emit(seg, &n, STATE_UP,   u1); up_accum += u1;  wave_emit(seg, &n, STATE_DOWN, d1); dn_accum += d1;
        wave_emit(seg, &n, STATE_UP,   u2); up_accum += u2;  wave_emit(seg, &n, STATE_DOWN, d2); dn_accum += d2;
        wave_emit(seg, &n, STATE_UP,   u3); up_accum += u3;  wave_emit(seg, &n, STATE_DOWN, d3); dn_accum += d3;
        wave_emit(seg, &n, STATE_UP,   u4); up_accum += u4;  wave_emit(seg, &n, STATE_DOWN, d4); dn_accum += d4;
        wave_emit(seg, &n, STATE_UP,   u5); up_accum += u5;  wave_emit(seg, &n, STATE_DOWN, d5); dn_accum += d5;
        wave_emit(seg, &n, STATE_UP,   u6); up_accum += u6;  wave_emit(seg, &n, STATE_DOWN, d6); dn_accum += d6;
        wave_emit(seg, &n, STATE_UP,   u7); up_accum += u7;  wave_emit(seg, &n, STATE_DOWN, d7); dn_accum += d7;
        wave_emit(seg, &n, STATE_UP,   t_up - up_accum);
        wave_emit(seg, &n, STATE_DOWN, t_down - dn_accum);
    } else {
        const uint32_t min_t = PIO_OVERHEAD;
        const uint32_t u_unit = (t_up - (3 * min_t)) / 6;
        const uint32_t d_unit = (t_down - (2 * min_t)) / 6;
        wave_emit(seg, &n, STATE_UP,   u_unit); up_accum += u_unit;
        wave_emit(seg, &n, STATE_DOWN, min_t);  dn_accum += min_t;
        wave_emit(seg, &n, STATE_UP,   u_unit); up_accum += u_unit;
        wave_emit(seg, &n, STATE_DOWN, d_unit); dn_accum += d_unit;
        wave_emit(seg, &n, STATE_UP,   u_unit); up_accum += u_unit;
        wave_emit(seg, &n, STATE_DOWN, d_unit); dn_accum += d_unit;
        wave_emit(seg, &n, STATE_UP,   min_t);  up_accum += min_t;
        wave_emit(seg, &n, STATE_DOWN, d_unit); dn_accum += d_unit;
        wave_emit(seg, &n, STATE_UP,   min_t);  up_accum += min_t;
        wave_emit(seg, &n, STATE_DOWN, d_unit); dn_accum += d_unit;
        wave_emit(seg, &n, STATE_UP,   min_t);  up_accum += min_t;
        wave_emit(seg, &n, STATE_DOWN, d_unit); dn_accum += d_unit;
        wave_emit(seg, &n, STATE_UP,   u_unit); up_accum += u_unit;
        wave_emit(seg, &n, STATE_DOWN, d_unit); dn_accum += d_unit;
        wave_emit(seg, &n, STATE_UP,   t_up - up_accum);
        wave_emit(seg, &n, STATE_DOWN, t_down - dn_accum);
    }
    wave_seal(seg, n, t_up, t_down);
    return n;
}

static int32_t fold_gain_q8(int32_t g_q8, uint8_t mode) {
    return (mode == 4) ? (2 * g_q8 - 256) : g_q8;
}

static int32_t fold_bias_q8(int32_t g_q8, uint8_t mode) {
    return (mode == 5) ? ((g_q8 - 256) / 2) : 0;
}

// RANGE multiplier so each style's RMS matches serial (mode 0) at the same g.
// Identity (g=1) is always 256. Never scale below identity.
static uint32_t fold_amp_comp_q8(uint32_t g_q8, uint8_t mode) {
    uint32_t s;
    switch (mode) {
        case 1:
            s = (g_q8 <= 512u) ? 256u : (g_q8 >> 1);
            break;
        case 2:
        case 3: {
            const float g = (float)g_q8 * (1.0f / 256.0f);
            const float sc = g * sqrtf(2.0f / (g * g + 1.0f));
            s = (uint32_t)(sc * 256.0f + 0.5f);
            break;
        }
        case 4:
            s = (g_q8 <= 512u) ? 256u : (2u * g_q8 - 256u) / 3u;
            break;
        case 5: {
            const float g = (float)g_q8 * (1.0f / 256.0f);
            const float gp = (3.0f * g - 1.0f) * 0.5f;
            const float gn = (g + 1.0f) * 0.5f;
            const float sc = sqrtf(2.0f) * gp * gn / sqrtf(gp * gp + gn * gn);
            s = (uint32_t)(sc * 256.0f + 0.5f);
            break;
        }
        default:
            s = g_q8;
            break;
    }
    if (s < 256u) s = 256u;
    return s;
}

static int32_t fold_q_from_x(int32_t x, int32_t g_q8, int32_t x_peak, uint8_t mode) {
    const int32_t gain = fold_gain_q8(g_q8, mode);
    int32_t q = (int32_t)(((int64_t)gain * x) / x_peak);
    q += fold_bias_q8(g_q8, mode);
    return q;
}

static int32_t fold_x_from_q(int32_t tq, int32_t g_q8, int32_t x_peak, uint8_t mode) {
    const int32_t gain = fold_gain_q8(g_q8, mode);
    if (gain == 0) return 0;
    const int32_t q_unbiased = tq - fold_bias_q8(g_q8, mode);
    return (int32_t)(((int64_t)q_unbiased * x_peak) / gain);
}

static int fold_push_split(uint32_t* splits, int ns, uint32_t t, uint32_t C) {
    if (t == 0 || t >= C || ns >= 8) return ns;
    for (int i = 0; i < ns; i++) if (splits[i] == t) return ns;
    splits[ns] = t;
    return ns + 1;
}

static uint32_t fold_t_at_x(int32_t x0, int32_t x1, int32_t x_hit) {
    return (x1 > x0) ? (uint32_t)(x_hit - x0) : (uint32_t)(x0 - x_hit);
}

static int fold_x_in_open(int32_t x0, int32_t x1, int32_t xh) {
    if (x1 > x0) return (x0 < xh && xh < x1);
    return (x1 < xh && xh < x0);
}

static int fold_collect_splits(int32_t x0, int32_t x1, uint32_t C, int32_t g_q8, int32_t x_peak, uint8_t mode, uint32_t* splits) {
    if (mode == 2 && x0 <= 0 && x1 <= 0) return 0;
    if (mode == 3 && x0 >= 0 && x1 >= 0) return 0;

    const int32_t q0 = fold_q_from_x(x0, g_q8, x_peak, mode);
    const int32_t q1 = fold_q_from_x(x1, g_q8, x_peak, mode);
    const int k0 = (mode == 1) ? -6 : -7;
    const int k1 = (mode == 1) ? 6 : 7;
    int ns = 0;
    for (int k = k0; k <= k1; k += 2) {
        if (mode == 1 && k == 0) continue;
        if (mode == 4 && (k == 1 || k == -1)) continue;
        const int32_t tq = (int32_t)k << 8;
        const bool cross = (q0 < tq && tq < q1) || (q1 < tq && tq < q0);
        if (!cross) continue;
        const int32_t x_hit = fold_x_from_q(tq, g_q8, x_peak, mode);
        if (mode == 2 && x_hit <= 0) continue;
        if (mode == 3 && x_hit >= 0) continue;
        if (!fold_x_in_open(x0, x1, x_hit)) continue;
        ns = fold_push_split(splits, ns, fold_t_at_x(x0, x1, x_hit), C);
        if (ns >= 8) break;
    }
    for (int i = 1; i < ns; i++) {
        uint32_t v = splits[i];
        int j = i;
        while (j > 0 && splits[j - 1] > v) { splits[j] = splits[j - 1]; j--; }
        splits[j] = v;
    }
    return ns;
}

static int fold_carrier(const WaveSeg* base, int nb, int32_t g_q8, uint8_t mode, WaveSeg* out) {
    int32_t x = 0;
    int32_t xmin = 0;
    int32_t xmax = 0;
    for (int i = 0; i < nb; i++) {
        if (base[i].dir == STATE_UP) x += (int32_t)base[i].cycles;
        else x -= (int32_t)base[i].cycles;
        if (x < xmin) xmin = x;
        if (x > xmax) xmax = x;
    }
    const int32_t x_off = (xmin + xmax) / 2;
    int32_t x_peak = xmax - x_off;
    if (x_off - xmin > x_peak) x_peak = x_off - xmin;
    if (x_peak < 1) return 0;

    x = -x_off;
    int n = 0;
    for (int i = 0; i < nb; i++) {
        const uint32_t C = base[i].cycles;
        if (C == 0) continue;
        const int32_t dx = (base[i].dir == STATE_UP) ? (int32_t)C : -(int32_t)C;
        const int32_t x1 = x + dx;
        uint32_t splits[8];
        const int ns = fold_collect_splits(x, x1, C, g_q8, x_peak, mode, splits);
        uint32_t t0 = 0;
        for (int s = 0; s <= ns; s++) {
            const uint32_t t1 = (s == ns) ? C : splits[s];
            const uint32_t piece = t1 - t0;
            if (piece == 0) { t0 = t1; continue; }
            const int32_t x_mid = x + ((dx > 0) ? (int32_t)(t0 + piece / 2) : -(int32_t)(t0 + piece / 2));
            int32_t q_mid = fold_q_from_x(x_mid, g_q8, x_peak, mode);
            // Skip-inner: |q|<3 stays identity; map |q|>=3 onto serial's first folded bands.
            if (mode == 4) {
                if (q_mid >= 768) q_mid -= 512;
                else if (q_mid <= -768) q_mid += 512;
            }
            const uint8_t fold_slope = fold_dir_from_left_odd(fold_left_odd(q_mid));
            const uint8_t out_dir = (fold_slope == base[i].dir) ? (uint8_t)STATE_UP : (uint8_t)STATE_DOWN;
            wave_emit(out, &n, out_dir, piece);
            t0 = t1;
        }
        x = x1;
    }
    wave_absorb_shorts(out, &n);
    return n;
}

static void chamfer_emit_window(WaveSeg* out, int* n, uint32_t W, uint32_t depth_q8, uint8_t arrive_dir) {
    const uint32_t pair_len = W / 8;
    if (pair_len < PIO_OVERHEAD) {
        wave_emit(out, n, arrive_dir, W);
        return;
    }
    static const uint16_t tent_arrive[4] = {32, 96, 160, 224};
    static const uint16_t tent_leave[4] = {224, 160, 96, 32};
    for (int j = 0; j < 4; j++) {
        const uint32_t lean = (depth_q8 * tent_arrive[j]) >> 8;
        const uint32_t up = (arrive_dir == STATE_UP) ? (256 - lean) : lean;
        wave_emit_pair(out, n, pair_len, up);
    }
    for (int j = 0; j < 4; j++) {
        const uint32_t lean = (depth_q8 * tent_leave[j]) >> 8;
        const uint32_t up = (arrive_dir == STATE_UP) ? lean : (256 - lean);
        wave_emit_pair(out, n, pair_len, up);
    }
}

static int chamfer_hairpins(WaveSeg* seg, int n, uint32_t t_corner, uint32_t period) {
    if (n < 4 || t_corner < 32) return n;
    uint32_t w_cap = period / 4;
    if (w_cap < t_corner) t_corner = w_cap;

    for (int pass = 0; pass < 8; pass++) {
        int hit = -1;
        int left = -1;
        int right = -1;
        for (int i = 0; i < n - 1; i++) {
            if (seg[i].dir == seg[i + 1].dir) continue;
            if (seg[i].cycles >= t_corner || seg[i + 1].cycles >= t_corner) continue;
            const int l = (i == 0) ? (n - 1) : (i - 1);
            const int r = (i + 1 == n - 1) ? 0 : (i + 2);
            if (l == i || l == i + 1 || r == i || r == i + 1 || l == r) continue;
            if (l > i || r < i) continue;
            hit = i;
            left = l;
            right = r;
            break;
        }
        if (hit < 0) break;
        const uint32_t h = seg[hit].cycles + seg[hit + 1].cycles;
        uint32_t W = t_corner;
        if (W < h) W = h;
        uint32_t extra = W - h;
        uint32_t take_l = extra / 2;
        uint32_t take_r = extra - take_l;
        if (seg[left].cycles < take_l + PIO_OVERHEAD) take_l = (seg[left].cycles > PIO_OVERHEAD) ? (seg[left].cycles - PIO_OVERHEAD) : 0;
        if (seg[right].cycles < take_r + PIO_OVERHEAD) take_r = (seg[right].cycles > PIO_OVERHEAD) ? (seg[right].cycles - PIO_OVERHEAD) : 0;
        W = h + take_l + take_r;
        if (W / 8 < PIO_OVERHEAD) break;
        W = (W / 8) * 8;
        extra = W - h;
        take_l = extra / 2;
        take_r = extra - take_l;
        if (seg[left].cycles < take_l + PIO_OVERHEAD || seg[right].cycles < take_r + PIO_OVERHEAD) break;

        uint32_t depth_q8 = (uint32_t)(((uint64_t)h << 8) / t_corner);
        if (depth_q8 > 256) depth_q8 = 256;
        const uint8_t arrive_dir = seg[left].dir;

        WaveSeg tmp[DCO_WAVE_SLOTS];
        int m = 0;
        for (int i = 0; i < left; i++) wave_emit(tmp, &m, seg[i].dir, seg[i].cycles);
        wave_emit(tmp, &m, seg[left].dir, seg[left].cycles - take_l);
        chamfer_emit_window(tmp, &m, W, depth_q8, arrive_dir);
        wave_emit(tmp, &m, seg[right].dir, seg[right].cycles - take_r);
        for (int i = right + 1; i < n; i++) wave_emit(tmp, &m, seg[i].dir, seg[i].cycles);
        if (m < 2 || m > DCO_WAVE_SLOTS) break;
        n = m;
        for (int i = 0; i < n; i++) seg[i] = tmp[i];
    }
    return n;
}

static uint16_t pack_wave(uint32_t* buf, const WaveSeg* seg, int n) {
    WaveSeg tmp[DCO_WAVE_SLOTS];
    if (n < 2) n = 2;
    if (n > DCO_WAVE_SLOTS) n = DCO_WAVE_SLOTS;
    for (int i = 0; i < n; i++) tmp[i] = seg[i];
    while (n < DCO_WAVE_SLOTS) {
        int best = -1;
        uint32_t best_c = 0;
        for (int i = 0; i < n; i++) {
            if (tmp[i].cycles >= (uint32_t)(2 * PIO_OVERHEAD) && tmp[i].cycles > best_c) {
                best = i;
                best_c = tmp[i].cycles;
            }
        }
        if (best < 0) break;
        const uint32_t keep = tmp[best].cycles >> 1;
        const uint32_t rest = tmp[best].cycles - keep;
        for (int j = n; j > best; --j) tmp[j] = tmp[j - 1];
        tmp[best].cycles = keep;
        tmp[best + 1].cycles = rest;
        tmp[best + 1].dir = tmp[best].dir;
        n++;
    }
    for (int i = 0; i < n; i++) buf[i] = pack_segment(tmp[i].dir, tmp[i].cycles);
    return (uint16_t)n;
}

static uint8_t dco_last_shape[NUM_VOICES_TOTAL];
static int16_t dco_last_fold[NUM_VOICES_TOTAL];
static int16_t dco_last_trim[NUM_VOICES_TOTAL];
static uint8_t dco_last_mode[NUM_VOICES_TOTAL];
static uint8_t dco_last_valid[NUM_VOICES_TOTAL];
static uint8_t dco_pong_copies[NUM_VOICES_TOTAL];
static uint8_t dco_pong_last_idle[NUM_VOICES_TOTAL] = { 0xFF, 0xFF, 0xFF, 0xFF };

void dco_wave_invalidate_bake(uint8_t voice) {
    if (voice < NUM_VOICES_TOTAL) {
        dco_last_valid[voice] = 0;
        dco_pong_copies[voice] = 0;
        dco_pong_last_idle[voice] = 0xFF;
    }
}

static void dco_note_pong_commit(uint8_t voice, uint8_t shape, int16_t fold, int16_t trim_key, uint8_t mode, int idle) {
    const bool same = (dco_last_shape[voice] == shape && dco_last_fold[voice] == fold && dco_last_trim[voice] == trim_key && dco_last_mode[voice] == mode);
    if (!same || dco_pong_last_idle[voice] == 0xFF) {
        dco_pong_copies[voice] = 1;
    } else if (dco_pong_last_idle[voice] != (uint8_t)idle) {
        dco_pong_copies[voice] = 2;
    } else {
        dco_pong_copies[voice] = 1;
    }
    dco_pong_last_idle[voice] = (uint8_t)idle;
    dco_last_shape[voice] = shape;
    dco_last_fold[voice] = fold;
    dco_last_trim[voice] = trim_key;
    dco_last_mode[voice] = mode;
    dco_last_valid[voice] = (dco_pong_copies[voice] >= 2) ? 1 : 0;
}

static inline void bake_waveform(uint8_t voice, uint8_t shape, uint32_t period_cycles) {
    dco_wave_set_clkdiv(voice, period_cycles);
    if (period_cycles < 64) return;
    if (shape > 5) shape = 5;

    int32_t mag = test2;
    if (mag < 0) mag = -mag;
    if (mag > 4096) mag = 4096;
    const int16_t fold = (int16_t)mag;
    const int16_t trim_key = test1;
    int32_t m5 = test5;
    if (m5 < 0) m5 = -m5;
    if (m5 > 5) m5 = 5;
    const uint8_t fold_mode = (uint8_t)m5;
    if (dco_last_valid[voice] && dco_last_shape[voice] == shape && dco_last_fold[voice] == fold && dco_last_trim[voice] == trim_key && dco_last_mode[voice] == fold_mode) {
        return;
    }

    const uint32_t period_native = DCO_WAVE_NATIVE;
    const int32_t half = (int32_t)(period_native / 2);
    const int32_t other = (int32_t)period_native - half;
    int32_t trim = (int32_t)test1 * 100;
    int32_t lim = half / 4;
    if (lim < 0) lim = 0;
    if (trim > lim) trim = lim;
    if (trim < -lim) trim = -lim;
    if (trim > other - 1) trim = other - 1;
    if (trim < -(half - 1)) trim = -(half - 1);

    const uint32_t t_up = (uint32_t)(half + trim);
    const uint32_t t_down = (uint32_t)(other - trim);
    if (t_up < PIO_OVERHEAD || t_down < PIO_OVERHEAD) return;

    WaveSeg base[DCO_WAVE_SLOTS];
    const int nb = bake_base_shape(base, shape, t_up, t_down);
    if (nb < 2) return;

    uint32_t buf[DCO_WAVE_SLOTS];
    if (fold > 0) {
        const int32_t g_q8 = 256 + (int32_t)fold * 512 / 4096;
        WaveSeg folded[DCO_WAVE_SLOTS];
        int nf = fold_carrier(base, nb, g_q8, fold_mode, folded);
        if (nf >= 2) {
            uint32_t t_corner = (uint32_t)(((uint64_t)(sysClock_Hz / 10000u) * period_native) / period_cycles);
            if (t_corner < 32) t_corner = 32;
            nf = chamfer_hairpins(folded, nf, t_corner, period_native);
            wave_seal(folded, nf, t_up, t_down);
            const uint16_t nfold = pack_wave(buf, folded, nf);
            const int idle = dco_wave_commit(voice, buf, nfold);
            if (idle < 0) return;
            dco_note_pong_commit(voice, shape, fold, trim_key, fold_mode, idle);
            return;
        }
    }

    const uint16_t nbase = pack_wave(buf, base, nb);
    const int idle = dco_wave_commit(voice, buf, nbase);
    if (idle < 0) return;
    dco_note_pong_commit(voice, shape, fold, trim_key, fold_mode, idle);
}
//===================================================================================================

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
#if PW_SWEEP_MODE_DEFAULT == PW_SWEEP_FULL
  // DCO4: Bipolar LFO swing
  const int32_t lfo2_pw_delta = ((int32_t)LFO2Level * (int32_t)LFO2toPW) >> 15;
#else
  // HALF Modes: Shift bipolar LFO (-32768..+32767) into unipolar positive (0..32767)
  const int32_t lfo2_pw_delta = ((uint32_t)(LFO2Level + 32768) * (uint32_t)LFO2toPW) >> 15;
#endif

  // Character Engine Output
  const float char_pitch_delta_f =  character ? character_pitch_delta_float() : 0.0f;
  const int32_t char_pw_delta_i = character ? (int32_t)character_pw_delta() : 0;
  const int32_t char_amp_mod_factor  = character ? character_amp_delta() : 0;

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
    //get_osc_params(DCO_B, wB, kB);
    
    uint32_t clk_div1 = pio_clk_div_for_y(total_cycles1, osc_last_y[DCO_A], wA, kA);
    //uint32_t clk_div2 = pio_clk_div_for_y(total_cycles2, osc_last_y[DCO_B], wB, kB);

        // OSC B: |test2| 0..4096 folds the test4 shape (g = 1..3).
        // |test5| 0..5 selects the fold transfer. test2 == 0 keeps the
        // discrete shape; negative test2/test5 mirror positive.
    int16_t shapeB = test4;
    if (shapeB < 0) shapeB = 0;
    if (shapeB > 5) shapeB = 5;
    bake_waveform(i, (uint8_t)shapeB, total_cycles2);

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
    // Fold shortens analog climb; scale OSC B RANGE so each style's RMS
    // matches serial (mode 0) at the same g. Identity stays *1.
    int32_t fold_mag = test2;
    if (fold_mag < 0) fold_mag = -fold_mag;
    if (fold_mag > 4096) fold_mag = 4096;
    const uint32_t g_q8 = 256u + (uint32_t)fold_mag * 512u / 4096u;
    int32_t m5 = test5;
    if (m5 < 0) m5 = -m5;
    if (m5 > 5) m5 = 5;
    uint32_t range_b = (uint32_t)(character ? char_val_B : chanLevel2);
    range_b = (range_b * fold_amp_comp_q8(g_q8, (uint8_t)m5)) >> 8;
    if (range_b > DIV_COUNTER) range_b = DIV_COUNTER;
    RANGE_PWM[DCO_B] = (uint16_t)range_b;
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
              // OSC A: Standard single-SM mask (OSC B is restarted via DMA helper)
              uint32_t maskA = (1u << sm1N);
              // uint32_t maskAB = (1u << sm1N) | (1u << sm2N);
              pio_set_sm_mask_enabled(pioN_A, maskA, false);

              // OSC A: Load stopped period (single-oscillator variant)
              osc_load_period_stopped_noclear(DCO_A, retrig_p1.y, retrig_p1.clk_div);
              // osc_load_periods_stopped_noclear(DCO_A, retrig_p1.y, retrig_p1.clk_div,
              //                                  DCO_B, retrig_p2.y, retrig_p2.clk_div);

              pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(osc_restart_target(DCO_A)));

              // OSC B (Triangle Core): Hard phase reset for the DMA Streamer
              reset_dco_dma_phase(i, pioN_B, sm2N, VOICE_TO_PIO[DCO_B]);

              /* 
              // Legacy OSC B phase alignment (handled natively by DMA reset above)
              if (phaseHoldX != 0) {
                  osc_phase_align_hold_stopped(DCO_B, phaseHoldX);
              } else {
                  pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(osc_restart_target(DCO_B)));
              }
              */

              pio_enable_sm_mask_in_sync(pioN_A, maskA);
          } else {
              pio_sm_put(pioN_A, sm1N, clk_div1);
              // pio_sm_put(pioN_B, sm2N, clk_div2); // OSC B is fed via DMA!
              pio_sm_exec(pioN_A, sm1N, pio_encode_pull(false, true));
              // pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, true));
              pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(osc_restart_target(DCO_A)));
              // pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(osc_restart_target(DCO_B)));
              osc_last_clk_div[DCO_A] = clk_div1;
              // osc_last_clk_div[DCO_B] = clk_div2;

              // Reset OSC B DMA phase on JMP retrig
              reset_dco_dma_phase(i, pioN_B, sm2N, VOICE_TO_PIO[DCO_B]);
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
          // update_osc_clk_div_instantly(pioN_B, sm2N, DCO_B, clk_div2); // Handled by bake_waveform in RAM
        #else
          pio_sm_put(pioN_A, sm1N, clk_div1);
          // pio_sm_put(pioN_B, sm2N, clk_div2); // Handled by bake_waveform in RAM
          pio_sm_exec(pioN_A, sm1N, pio_encode_pull(false, true));
          // pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, true));
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
          // osc_last_clk_div[DCO_B] = clk_div2; // OSC B is DMA-driven
      }
      BENCH_END(vt_retrig_sm_apply);

    } else {
      // Normal running frame: update clk_div continuously for vibrato/LFOs
      BENCH_BEGIN(vt_pio_write);
      #if defined(UPDATE_CLK_DIV_INSTANTLY)
      update_osc_clk_div_instantly(pioN_A, sm1N, DCO_A, clk_div1);
      // update_osc_clk_div_instantly(pioN_B, sm2N, DCO_B, clk_div2); // Handled by bake_waveform() in RAM via DMA
      #else
      pio_sm_put(pioN_A, sm1N, clk_div1);
      // pio_sm_put(pioN_B, sm2N, clk_div2); // OSC B is fed via DMA!
      pio_sm_exec(pioN_A, sm1N, pio_encode_pull(false, true));
      // pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, true));
      #endif
      osc_last_clk_div[DCO_A] = clk_div1;
      // osc_last_clk_div[DCO_B] = clk_div2; // OSC B is DMA-driven
      BENCH_END(vt_pio_write);
    }

    if (timer99microsFlag2) {
      if (pulseWaveOn) {
        BENCH_BEGIN(vt_pwm_calc);

#if PW_SWEEP_MODE_DEFAULT == PW_SWEEP_FULL
        const int32_t adsr3_delta = ((int32_t)adsr3_lvl[i] * (int32_t)ADSR3toPWM) >> 15;
#else
        // Shift >> 14 scales the unmodified ADSR3toPWM to the full 2048 span
        const int32_t adsr3_delta = ((int32_t)adsr3_lvl[i] * (int32_t)ADSR3toPWM) >> 14;
#endif

        // m_pw is already scaled correctly by the mod matrix - added directly at 1:1
        int32_t pw_calc = base_pw_val + adsr3_delta + (int32_t)m_pw[i];

        // RP2350 OPTIMIZED: Truly Branchless Nested Clamp (0 to 2047)
        const int32_t max_pw = (int32_t)(DIV_COUNTER_PW - 1);
        pw_calc = (pw_calc < 0) ? 0 : ((pw_calc > max_pw) ? max_pw : pw_calc);

        // Unified 3-Point Interpolator natively routes the compile flag
        PW_PWM[i] = get_PW_level_interpolated<PW_SWEEP_MODE_DEFAULT>((uint16_t)pw_calc, DCO_A, freqA_Hz);

        BENCH_END(vt_pwm_calc);
      } else {
#if PW_SWEEP_MODE_DEFAULT == PW_SWEEP_FULL
        PW_PWM[i] = 0;
#else
        PW_PWM[i] = DIV_COUNTER_PW; // HALF modes anchor 50% at 0V
#endif
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