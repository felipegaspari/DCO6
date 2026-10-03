#include <cstdint>
#ifndef __VOICES_H__
#define __VOICES_H__

void init_voices();

volatile bool note_on_flag_flag[NUM_VOICES_TOTAL];

uint32_t portamentoTimer[NUM_VOICES_TOTAL];
uint32_t portamentoStartMillis[NUM_VOICES_TOTAL];
uint32_t portamentoStartMicros[NUM_VOICES_TOTAL];

bool portamento = true;
uint8_t portamento_parameter_value = 0;  // raw UI / param value (0..255)
uint32_t portamento_time_fixed = 0;      // µs for PORTA_MODE_TIME
uint32_t portamento_time_slew = 0;       // µs for PORTA_MODE_SLEW
uint32_t portamento_time = 0;            // active T for current mode

// Portamento mode (both glide in semitone space):
//   0 TIME — fixed duration portamento_time_fixed for any interval
//   1 SLEW — constant rate (12 semitones / portamento_time_slew); time scales with interval
enum PortamentoMode : uint8_t {
  PORTA_MODE_TIME = 0,
  PORTA_MODE_SLEW = 1
};
uint8_t portamento_mode = PORTA_MODE_SLEW;

// Portamento state in Q24 (Hz * 2^24) — one slot per oscillator
int64_t portamento_start_q24[NUM_OSCILLATORS];
int64_t portamento_stop_q24[NUM_OSCILLATORS];
int64_t portamento_cur_freq_q24[NUM_OSCILLATORS];
// Legacy per-µs Hz step (unused by live glide; kept for bench/compat)
int64_t freqPortaStep_q24[NUM_OSCILLATORS];

// Portamento state in note-space (Q16 semitones)
int32_t porta_note_start_q16[NUM_OSCILLATORS];
int32_t porta_note_stop_q16[NUM_OSCILLATORS];
int32_t porta_note_cur_q16[NUM_OSCILLATORS];
int32_t porta_note_step_q16[NUM_OSCILLATORS];  // Q16 semitones per µs
// False until first real pitch is latched (do not treat table index 0 as "uninit").
bool porta_note_valid[NUM_OSCILLATORS];

// Float portamento state for the float engine (Hz cache + semitone glide)
#ifdef USE_FLOAT_VOICE_TASK
float porta_freq_start_f[NUM_OSCILLATORS];
float porta_freq_stop_f [NUM_OSCILLATORS];
float porta_freq_step_f [NUM_OSCILLATORS];  // legacy Hz/µs (unused by live glide)
float porta_freq_cur_f  [NUM_OSCILLATORS];

float porta_note_start_f[NUM_OSCILLATORS];
float porta_note_stop_f [NUM_OSCILLATORS];
float porta_note_cur_f  [NUM_OSCILLATORS];
float porta_note_step_f [NUM_OSCILLATORS];  // semitones per microsecond
#endif

uint8_t highestNote = 124;

static const int multiplierTableSize = 200;
// Legacy ×10000 scale for Q12 A/B only. RATIO_Q16 stores native Q16 (1.0 = 65536).
const int32_t multiplierTableScale = 10000;

// Pitch-multiplier storage: only the active PITCH_INTERP_MODE is allocated (see DCO.ino).
#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT || \
    PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
float   xMultiplierTableF[multiplierTableSize]; // modifier [-1,3]
float   yMultiplierTableF[multiplierTableSize]; // frequency ratio
float   slopeF[multiplierTableSize - 1];
#else
int32_t xMultiplierTable[multiplierTableSize]; // RATIO_Q16: natural Q16; Q12: ×10000 units
int32_t yMultiplierTable[multiplierTableSize]; // RATIO_Q16: ratio Q16; Q12: ×10000 units
int32_t x0Q16_tbl[multiplierTableSize];
#if PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
// Q16 slope: (dy<<16)/dx. Q20×delta overflows int32 on this table; Q16×delta fits
// so live lerp can use 32-bit MULS (no soft __aeabi_lmul).
int32_t slopeQ16[multiplierTableSize - 1];
#elif PITCH_INTERP_MODE == PITCH_INTERP_Q12
int32_t slopeQ12[multiplierTableSize - 1];
#endif
#endif

// Per-DCO segment cache for interpolation (stores last 'low' index)
int16_t interpSegCache[NUM_OSCILLATORS];

static const uint16_t maxFrequency = 4000;

// Q24 frequency constants
static constexpr int32_t Q24_ONE = (1 << 24);
static constexpr int32_t Q24_EPS_DELTA_1P00001 = 168; // round(0.00001 * 2^24)
static constexpr int32_t Q24_ONE_EPS = Q24_ONE + Q24_EPS_DELTA_1P00001;


// Voice-task probe storage lives in bench.h, generated from the BENCH_PROBES table. There is
// deliberately nothing to declare here: the previous extern block drifted out of step with
// voices.ino (it named probes that had been renamed or deleted) and compiled anyway, because
// an extern that is never odr-used needs no definition.

#endif
