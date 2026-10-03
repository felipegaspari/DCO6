#ifndef CHARACTER_JITTER_H
#define CHARACTER_JITTER_H

#include <stdint.h>

// Character-tab noise jitter (hot path). Uses noiseLevel[] already filled in loop1.
//
// Knob changes call character_recompute_scales() (param / diag path):
//   eff   = (axis * character) >> 7                    // 0..128
//   scale = (max_delta * eff) >> 7                     // Q15-ish gain
// Hot path (voice_task / PWM):
//   delta = (noise_q15 * scale) >> 15                  // → [-max*eff/128, +…]
// Pitch needs int64 for the mul; amp/PW products fit in int32.

// Full-knob peak excursions (character=128 and axis=128).
static constexpr int32_t CHAR_JITTER_PITCH_MAX_Q24 = (1 << 24) / 20;  // ±0.05 oct
static constexpr int32_t CHAR_JITTER_AMP_MAX       = (int32_t)DIV_COUNTER / 20;  // ±5% RANGE (absolute)
static constexpr int32_t CHAR_JITTER_PW_MAX        = (int32_t)DIV_COUNTER_PW / 10;  // ±10% PW

static inline int32_t character_axis_scale(uint8_t axis_jitter, int32_t max_delta) {
  if (character == 0 || axis_jitter == 0 || max_delta == 0) {
    return 0;
  }
  const uint8_t eff =
      (uint8_t)(((uint16_t)axis_jitter * (uint16_t)character) >> 7);
  return (max_delta * (int32_t)eff) >> 7;
}

static inline void character_recompute_scales(void) {
  char_pitch_scale_q15 = character_axis_scale(pitchJitter, CHAR_JITTER_PITCH_MAX_Q24);
  char_amp_scale_q15   = character_axis_scale(ampCompJitter, CHAR_JITTER_AMP_MAX);
  char_pw_scale_q15    = character_axis_scale(pulsewidthJitter, CHAR_JITTER_PW_MAX);
}

// noise0 white → amp-comp + pulsewidth
// noise1 pink → pitch

// noise1 → pitch every voice_task frame
static inline int32_t character_pitch_delta_q24(void) {
  const int32_t s = char_pitch_scale_q15;
  if (!s) {
    return 0;
  }
  return (int32_t)(((int64_t)(int32_t)noiseLevel[1] * s) >> 15);
}

// noise0 → amp-comp at RANGE PWM write (~10 kHz / note-on).
// Caller must gate on char_amp_scale_q15 != 0.
// Absolute PWM counts; |noise|<=32768, |s|<=700 → product fits int32.
static inline int32_t character_amp_delta(void) {
  return ((int32_t)noiseLevel[0] * char_amp_scale_q15) >> 15;
}

static inline uint16_t character_clamp_amp(int32_t level) {
  if (level < 0) {
    return 0;
  }
  if (level > (int32_t)DIV_COUNTER) {
    return (uint16_t)DIV_COUNTER;
  }
  return (uint16_t)level;
}

// noise0 → pulsewidth at PW PWM write
static inline int32_t character_pw_delta(void) {
  const int32_t s = char_pw_scale_q15;
  if (!s) {
    return 0;
  }
  return ((int32_t)noiseLevel[0] * s) >> 15;
}

#endif  // CHARACTER_JITTER_H
