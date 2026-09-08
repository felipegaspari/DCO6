#pragma once
#include <stdint.h>
#include "hardware/sync.h"

// Compile-time immediate constants
static constexpr float MAX_LIN_DEPTH     = 0.85f;
static constexpr float MAX_EXPO_OCTAVES  = 2.5f;

static constexpr float LIN_DEPTH_SCALER  = MAX_LIN_DEPTH / 32768.0f;
static constexpr float EXPO_DEPTH_SCALER = MAX_EXPO_OCTAVES / 32768.0f;

// 128-bit aligned cache structure to enable dual-word (LDRD) burst loads in SRAM
struct alignas(16) XmodPrebaked {
    int32_t base_xmod;       // 4 bytes (offset 0)
    float   expo_depth;      // 4 bytes (offset 4)  - Scaled octaves
    float   expo_bias;       // 4 bytes (offset 8)  - Pre-calculated 2.5-octave bias
    float   lin_norm;        // 4 bytes (offset 12) - Pre-calculated atanh period compensation
    float   lin_depth_norm;  // 4 bytes (offset 16) - Fused constant (lin_depth * lin_norm) for 1-cycle VFMA
    bool    active;          // 1 byte  (offset 20)
    uint8_t _pad[3];         // 3 bytes explicit padding to maintain 32-bit alignment
};

// Global instance matching your existing placement
inline XmodPrebaked xmod_cache;

// FIX 1: Matched volatile qualifier with globals.h
extern volatile uint8_t crossmod_mode;
extern uint32_t shadow_phase_osc2_q32[NUM_VOICES_TOTAL];

/**
 * @brief Restored exact function name: call this when your depth parameter changes.
 * Pre-calculates curves outside the audio loop.
 */
static inline void update_crossmod_prebake(int32_t depth_q15) {
    xmod_cache.base_xmod = depth_q15;

    if (depth_q15 <= 0) {
        xmod_cache.active         = false;
        xmod_cache.expo_depth     = 0.0f;
        xmod_cache.expo_bias      = 0.0f;
        xmod_cache.lin_norm       = 1.0f;
        xmod_cache.lin_depth_norm = 0.0f;
        return;
    }

    // RP2350 1-cycle hardware saturation [0, 32767]
    uint32_t clamped = (uint32_t)__builtin_arm_usat(depth_q15, 15);
    xmod_cache.active = true;

    // Pre-bake Mode 0 (Centered Expo FM)
    float mod_depth = (float)clamped * EXPO_DEPTH_SCALER;
    float depth_sq  = mod_depth * mod_depth;
    xmod_cache.expo_depth = mod_depth;
    xmod_cache.expo_bias  = (0.115525f - (0.001655f * depth_sq)) * depth_sq;

    // Pre-bake Mode 2 (Linear FM)
    float lin_depth = (float)clamped * LIN_DEPTH_SCALER;
    float d2        = lin_depth * lin_depth;
    // Horner form evaluation for period averaging compensation
    float lin_norm  = 1.0f + d2 * (0.333333f + d2 * (0.12f + d2 * 0.48f));

    xmod_cache.lin_norm       = lin_norm;
    xmod_cache.lin_depth_norm = lin_depth * lin_norm;
}

// =============================================================================
// HEAVILY OPTIMIZED FAST EXP2 (Cortex-M33 ARMv8-M FPU)
// =============================================================================
static inline __attribute__((always_inline)) float fast_exp2f_audio(float p) {
#if defined(__ARM_ARCH) && !defined(__riscv)
    // 1. Single-cycle FPU floor (rounds towards minus infinity directly in FPU register)
    float flr;
    __asm__ ("vrintm.f32 %0, %1" : "=t"(flr) : "t"(p));

    // 2. Fractional part in FPU
    float f = p - flr;

    // 3. FIX 2: flr is already a whole integer float; standard cast lets GCC emit
    //    vcvt.s32.f32 and vmov cleanly without inline assembly constraint errors.
    int32_t i = (int32_t)flr;
#else
    int32_t i = (int32_t)p;
    i -= (p < 0.0f);
    float f = p - (float)i;
#endif

    // 4. Horner form quadratic approximation: (0.304153 * f + 0.695847) * f + 1.0
    // Compiles into 2 single-cycle VFMA.F32 instructions
    float approx = (0.304153f * f + 0.695847f) * f + 1.0f;

    // 5. Exponent reconstruction via IEEE-754
    uint32_t exp_bits = (uint32_t)(i + 127) << 23;
    float exp_scale;
#if defined(__ARM_ARCH) && !defined(__riscv)
    __asm__ ("vmov %0, %1" : "=t"(exp_scale) : "r"(exp_bits));
#else
    union { uint32_t u; float f; } v;
    v.u = exp_bits;
    exp_scale = v.f;
#endif

    return approx * exp_scale;
}

static uint32_t shadow_phase_xmod_q32[NUM_VOICES_TOTAL] = {0};

// =============================================================================
// TEMPLATE-SPECIALIZED CROSSMOD ENGINE CORE (Zero-Cost Branching)
// =============================================================================
template <uint8_t Mode>
static inline __attribute__((always_inline)) float SRAM_HOT(apply_crossmod_impl)(
    uint8_t voice_idx,
    float carrier_freq_Hz,
    float modulator_freq_Hz,
    float hz_to_phase_inc,
    int32_t mod_matrix_delta
) {
    // -------------------------------------------------------------------------
    // CRITICAL AUDIO FIX: ALWAYS advance modulator shadow phase first!
    // Prevents phase-freeze glitches & attack clicks when modulation depth is 0.
    // -------------------------------------------------------------------------
    uint32_t phase = shadow_phase_xmod_q32[voice_idx];
    phase += (uint32_t)(modulator_freq_Hz * hz_to_phase_inc);
    shadow_phase_xmod_q32[voice_idx] = phase;

    // Fast 1-cycle hardware saturation [0, 32767]
    uint32_t total_mod_q15 = (uint32_t)__builtin_arm_usat(xmod_cache.base_xmod + mod_matrix_delta, 15);

    if (__builtin_expect(total_mod_q15 == 0, 1)) {
        return carrier_freq_Hz;
    }

    // -------------------------------------------------------------------------
    // 1-CYCLE HARDWARE FIXED-POINT CONVERSION & TRIANGLE FOLD
    // -------------------------------------------------------------------------
    int32_t tri_q31 = (int32_t)((phase << 1) ^ ((int32_t)phase >> 31) ^ 0x80000000u);

    float shadow_tri;
#if defined(__ARM_ARCH) && !defined(__riscv)
    __asm__ ("vmov         %0, %1\n\t"
             "vcvt.f32.s32 %0, %0, #31"
             : "=t"(shadow_tri) : "r"(tri_q31));
#else
    shadow_tri = (float)tri_q31 * (1.0f / 2147483648.0f);
#endif

    float out_freq_Hz;

    // Static / Prebaked path (Common Case: mod_matrix_delta == 0)
    if (__builtin_expect(mod_matrix_delta == 0, 1)) {
        if constexpr (Mode == 0) { // Centered Exponential FM
            float octaves = shadow_tri * xmod_cache.expo_depth - xmod_cache.expo_bias;
            out_freq_Hz = carrier_freq_Hz * fast_exp2f_audio(octaves);
            return out_freq_Hz; // Strictly positive: zero clamp stall needed
        } 
        else if constexpr (Mode == 1) { // Vintage Uncentered Exponential FM
            float octaves = shadow_tri * xmod_cache.expo_depth;
            out_freq_Hz = carrier_freq_Hz * fast_exp2f_audio(octaves);
            return out_freq_Hz; // Strictly positive: zero clamp stall needed
        } 
        else { // Mode 2: True Linear FM
            float mod_mult = xmod_cache.lin_norm + shadow_tri * xmod_cache.lin_depth_norm;
            out_freq_Hz = carrier_freq_Hz * mod_mult;
            return (out_freq_Hz < 10.0f) ? 10.0f : out_freq_Hz;
        }
    } 
    // Dynamic mod-matrix path (When LFO/Env modulates crossmod depth)
    else {
        if constexpr (Mode == 2) {
            float d = (float)total_mod_q15 * LIN_DEPTH_SCALER;
            float d2 = d * d;
            float norm = 1.0f + d2 * (0.333333f + d2 * (0.12f + d2 * 0.48f));
            out_freq_Hz = carrier_freq_Hz * (norm + shadow_tri * (d * norm));
            return (out_freq_Hz < 10.0f) ? 10.0f : out_freq_Hz;
        } 
        else if constexpr (Mode == 0) {
            float d = (float)total_mod_q15 * EXPO_DEPTH_SCALER;
            float d2 = d * d;
            float bias = (0.115525f - (0.001655f * d2)) * d2;
            return carrier_freq_Hz * fast_exp2f_audio((shadow_tri * d) - bias);
        } 
        else { // Mode 1
            float d = (float)total_mod_q15 * EXPO_DEPTH_SCALER;
            return carrier_freq_Hz * fast_exp2f_audio(shadow_tri * d);
        }
    }
}

// =============================================================================
// PUBLIC API: DIRECTION-AGNOSTIC CROSSMOD PROCESSOR
// =============================================================================

/**
 * @brief Template-specialized version for outer voice loops where mode is known.
 */
template <uint8_t Mode>
static inline __attribute__((always_inline)) float SRAM_HOT(apply_crossmod)(
    uint8_t voice_idx,
    float carrier_freq_Hz,
    float modulator_freq_Hz,
    float hz_to_phase_inc,
    int32_t mod_matrix_delta
) {
    return apply_crossmod_impl<Mode>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
}

/**
 * @brief Standard drop-in replacement matching your exact existing function signature.
 */
static inline __attribute__((always_inline)) float SRAM_HOT(apply_crossmod)(
    uint8_t voice_idx,
    float carrier_freq_Hz,
    float modulator_freq_Hz,
    float hz_to_phase_inc,
    int32_t mod_matrix_delta
) {
    switch (crossmod_mode) {
        case 0:  return apply_crossmod_impl<0>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
        case 1:  return apply_crossmod_impl<1>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
        case 2:
        default: return apply_crossmod_impl<2>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
    }
}