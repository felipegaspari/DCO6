#ifndef DCO_CLKDIV_H
#define DCO_CLKDIV_H

#include <math.h>
#include <stdint.h>

// Live total-cycle helpers (CLKDIV_MODE). Shared with clkdiv_bench.ino so cmds 32/33
// cannot drift from vt_clk_div. GOLD_REF (true-Hz llround) is bench-only.

// GOLD on native Hz (float-voice GOLD_LIVE / GOLD_REF). Soft-double; A/B.
static inline __attribute__((always_inline)) uint32_t clkdiv_gold_hz_total_cycles(uint32_t sys_hz, double hz) {
  if (!(hz > 0.0)) return 0;
  double cyc = (double)sys_hz / hz;
  if (cyc >= 4.0e9) return 4000000000u;
  if (cyc <= 0.0) return 0;
  return (uint32_t)llround(cyc);
}

// GOLD / GOLD_LIVE: Q24 → double Hz → llround(sys / hz). Soft-double; A/B.
static inline __attribute__((always_inline)) uint32_t clkdiv_gold_total_cycles(uint32_t sys_hz, int64_t freq_q24) {
  if (freq_q24 <= 0) return 0;
  double hz = (double)freq_q24 * (1.0 / 16777216.0);
  return clkdiv_gold_hz_total_cycles(sys_hz, hz);
}

// Q16 / CLKDIV_Q16: Q16 Hz → 64/32 (shipping). ±1/65536 Hz vs full Q24.
static inline __attribute__((always_inline)) uint32_t clkdiv_q16_total_cycles(uint32_t sys_hz, int64_t freq_q24) {
  if (freq_q24 <= 0) return 0;
  uint32_t freq_q16 = (uint32_t)((freq_q24 + (int64_t)(1 << 7)) >> 8);
  if (freq_q16 == 0) freq_q16 = 1;
  uint64_t num = ((uint64_t)sys_hz << 16) + (uint64_t)(freq_q16 / 2u);
  return (uint32_t)(num / freq_q16);
}

// Internal Q8 64/32 — not a CLKDIV_MODE. Q8 live path fallback below ~16 Hz.
static inline __attribute__((always_inline)) uint32_t clkdiv_precise_q8_total_cycles(uint32_t sys_hz, int64_t freq_q24) {
  if (freq_q24 <= 0) return 0;
  uint32_t freq_q8 = (uint32_t)((freq_q24 + (int64_t)(1 << 15)) >> 16);
  if (freq_q8 == 0) freq_q8 = 1;
  uint64_t num = ((uint64_t)sys_hz << 8) + (uint64_t)(freq_q8 / 2u);
  return (uint32_t)(num / freq_q8);
}

// Q8 / CLKDIV_Q8: same Q8 quantize + round as precise_q8, two 32/32 divs instead of 64/32.
// (sys<<8 + q8/2)/q8 = q + ((r<<8) - q*f_frac + q8/2)/q8, sys = q*f_int + r.
// q*f_frac fits uint32 when f_int >= 16 (~16 Hz @ 240 MHz); else precise_q8 fallback.
static inline __attribute__((always_inline)) uint32_t clkdiv_q8_total_cycles(uint32_t sys_hz, int64_t freq_q24) {
  if (freq_q24 <= 0) return 0;
  uint32_t freq_q8 = (uint32_t)((freq_q24 + (int64_t)(1 << 15)) >> 16);
  if (freq_q8 == 0) freq_q8 = 1;
  uint32_t f_int = freq_q8 >> 8;
  if (f_int < 16u) {
    return clkdiv_precise_q8_total_cycles(sys_hz, freq_q24);
  }
  uint32_t f_frac = freq_q8 & 0xFFu;
  uint32_t q = sys_hz / f_int;
  uint32_t r = sys_hz - q * f_int;
  uint32_t qf = q * f_frac;
  uint32_t r8 = r << 8;
  uint32_t round = freq_q8 / 2u;
  if (r8 >= qf) {
    return q + (r8 - qf + round) / freq_q8;
  }
  uint32_t diff = qf - r8;
  if (round >= diff) {
    return q + (round - diff) / freq_q8;
  }
  // Negative remainder: unsigned 64/32 is q - ceil(D/q8), not q - floor(D/q8).
  uint32_t D = diff - round;
  return q - (D + freq_q8 - 1u) / freq_q8;
}

// FAST_Q4: round to Q4 Hz, then 32-bit (sys*16 + Q4/2) / Q4.
static inline __attribute__((always_inline)) uint32_t clkdiv_fast_q4_total_cycles(uint32_t sys_hz, int64_t freq_q24) {
  if (freq_q24 <= 0) return 0;
  uint32_t freq_q4 = (uint32_t)((freq_q24 + (1LL << 19)) >> 20);
  if (freq_q4 == 0) freq_q4 = 1;
  return (sys_hz * 16u + (freq_q4 / 2u)) / freq_q4;
}

// FLOAT: same math as voice_task_float — fminf(sys/hz + 0.5f, 4e9).
static inline __attribute__((always_inline)) uint32_t clkdiv_float_hz_total_cycles(uint32_t sys_hz, float hz) {
  if (!(hz > 0.0f)) return 0;
  return (uint32_t)fminf((float)sys_hz / hz + 0.5f, 4.0e9f);
}

// FLOAT / FLOAT_LIVE: Q24 → float Hz → clkdiv_float_hz_total_cycles.
static inline __attribute__((always_inline)) uint32_t clkdiv_float_total_cycles(uint32_t sys_hz, int64_t freq_q24) {
  if (freq_q24 <= 0) return 0;
  float hz = (float)freq_q24 * (1.0f / 16777216.0f);
  return clkdiv_float_hz_total_cycles(sys_hz, hz);
}

// Live fixed-voice entry: compile-time alias of CLKDIV_MODE (not a function pointer).
#if CLKDIV_MODE == CLKDIV_Q16
#define clkdiv_live_total_cycles clkdiv_q16_total_cycles
#elif CLKDIV_MODE == CLKDIV_Q8
#define clkdiv_live_total_cycles clkdiv_q8_total_cycles
#elif CLKDIV_MODE == CLKDIV_FAST_Q4
#define clkdiv_live_total_cycles clkdiv_fast_q4_total_cycles
#elif CLKDIV_MODE == CLKDIV_FLOAT
#define clkdiv_live_total_cycles clkdiv_float_total_cycles
#else
#define clkdiv_live_total_cycles clkdiv_gold_total_cycles
#endif

// Live float-voice entry: native Hz. FLOAT/GOLD stay Hz; Q16/Q8/FAST_Q4 Hz→Q24 then Q24 alias.
static inline __attribute__((always_inline))
uint32_t clkdiv_live_hz_total_cycles(uint32_t sys_hz, float hz) {
#if CLKDIV_MODE == CLKDIV_FLOAT
  return clkdiv_float_hz_total_cycles(sys_hz, hz);
#elif CLKDIV_MODE == CLKDIV_GOLD
  return clkdiv_gold_hz_total_cycles(sys_hz, (double)hz);
#else
  if (!(hz > 0.0f)) return 0;
  int64_t q24 = (int64_t)llround((double)hz * 16777216.0);
  return clkdiv_live_total_cycles(sys_hz, q24);
#endif
}

#endif  // DCO_CLKDIV_H
