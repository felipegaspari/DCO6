#pragma once
#include <stdint.h>
#include "hardware/sync.h"

// Compile-time immediate constants
static constexpr float MAX_LIN_DEPTH     = 0.85f;
static constexpr float MAX_EXPO_OCTAVES  = 2.5f;

static constexpr float LIN_DEPTH_SCALER  = MAX_LIN_DEPTH / 32768.0f;
static constexpr float EXPO_DEPTH_SCALER = MAX_EXPO_OCTAVES / 32768.0f;

// 0 to 32767 (Q15 format)
uint16_t crossmod_depth = 0;
volatile uint8_t crossmod_mode = 1;
volatile uint8_t crossmod_shape = 0;      // Default: Triangle
volatile uint8_t crossmod_ratio = 2;      // Default: Index 2 (1.0x Ratio)
volatile uint8_t crossmod_detune = 128;   // Default: Center
volatile uint8_t crossmod_symmetry = 128; // Default: 50%

#ifndef NUM_VOICES_TOTAL
#define NUM_VOICES_TOTAL 16
#endif

extern uint32_t shadow_phase_osc2_q32[NUM_VOICES_TOTAL];

// 32-byte aligned cache structure (Perfect Cortex-M Cache Line Size)
struct alignas(32) XmodPrebaked {
    int32_t  base_xmod;       // 4 bytes (offset 0)
    float    expo_depth;      // 4 bytes (offset 4)
    float    expo_bias;       // 4 bytes (offset 8)
    float    lin_norm;        // 4 bytes (offset 12)
    float    lin_depth_norm;  // 4 bytes (offset 16)
    float    mod_freq_scalar; // 4 bytes (offset 20) - Ratio * Detune multiplier
    uint32_t symmetry_q32;    // 4 bytes (offset 24) - For Square PWM
    uint8_t  shape;           // 1 byte  (offset 28) - Selected waveshape
    bool     active;          // 1 byte  (offset 29)
    uint8_t  _pad[2];         // 2 bytes explicit padding to 32 bytes
};

// Global instance matching your existing placement
inline XmodPrebaked xmod_cache;

// Explicit pre-computed constant to replace inline divisions in the hot loop
static constexpr float Q31_TO_FLOAT      = 4.656612873077392578125e-10f;
static constexpr float FOUR_Q31_TO_FLOAT = 4.0f * Q31_TO_FLOAT;

struct XmodCurveFit {
    float expo_c1;  // Primary Taylor coefficient
    float expo_c2;  // Higher-order correction coefficient
    float lin_var;  // RMS variance scalar for Linear FM
};

// Precisely tuned Minimax polynomial coefficients for perfect Expo FM pitch tracking
static constexpr XmodCurveFit XMOD_CURVES[4] = {
    {0.115525f, 0.001655f, 1.0f}, // 0: Triangle
    {0.115525f, 0.001655f, 1.0f}, // 1: Sawtooth
    {0.334000f, 0.014000f, 3.0f}, // 2: Square
    {0.208000f, 0.008500f, 1.6f}  // 3: Parabolic Sine
};

static constexpr float XMOD_RATIO_TABLE[7] = {
    0.25f, 0.5f, 0.75f, 1.0f, 2.0f, 3.0f, 4.0f
};

// Aligned to 16 bytes for Cortex-M33 128-bit bus transfers
alignas(16) static uint32_t shadow_phase_xmod_q32[NUM_VOICES_TOTAL] = {0};

/**
 * @brief Restored exact function name: call this when your depth parameter changes.
 * Pre-calculates curves outside the audio loop.
 */
static inline void update_crossmod_prebake(int32_t depth_q15) {
    xmod_cache.base_xmod = depth_q15;
    xmod_cache.shape     = crossmod_shape;

    // Pre-bake Ratio & Detune multipliers
    float ratio  = XMOD_RATIO_TABLE[crossmod_ratio & 0x07];
    float detune = __builtin_fmaf((float)crossmod_detune - 128.0f, 0.000226f, 1.0f); 
    xmod_cache.mod_freq_scalar = ratio * detune;

    // Pre-bake Square Symmetry (128 = exact 50% duty / 0 DC offset)
    int32_t sym_offset = (int32_t)crossmod_symmetry - 128; 
    xmod_cache.symmetry_q32 = 0x80000000u + (uint32_t)(sym_offset * 15099494);

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

    // Fetch precise Taylor fit coefficients for the selected waveshape
    const XmodCurveFit& curve = XMOD_CURVES[crossmod_shape & 0x03];

    // Pre-bake Mode 0 (Centered Expo FM) using hardware FMA
    float mod_depth = (float)clamped * EXPO_DEPTH_SCALER;
    float d2        = mod_depth * mod_depth;
    xmod_cache.expo_depth = mod_depth;
    xmod_cache.expo_bias  = __builtin_fmaf(-curve.expo_c2, d2, curve.expo_c1) * d2;

    // Pre-bake Mode 2 (Linear FM) using Horner FMA evaluation
    float lin_depth = (float)clamped * LIN_DEPTH_SCALER;
    float l2        = lin_depth * lin_depth * curve.lin_var;
    // Horner form evaluation for period averaging compensation
    float p         = __builtin_fmaf(0.48f, l2, 0.12f);
    p               = __builtin_fmaf(p, l2, 0.33333334f);
    float lin_norm  = __builtin_fmaf(p, l2, 1.0f);

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
    float approx = __builtin_fmaf(__builtin_fmaf(0.304153f, f, 0.695847f), f, 1.0f);

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

// =============================================================================
// DUAL-SPECIALIZED TEMPLATE ENGINE (Zero Runtime Branches for Mode & Shape)
// =============================================================================
template <uint8_t Mode, uint8_t Shape>
static inline __attribute__((always_inline)) float SRAM_HOT(apply_crossmod_impl)(
    uint8_t voice_idx,
    float carrier_freq_Hz,
    float modulator_freq_Hz,
    float hz_to_phase_inc,
    int32_t mod_matrix_delta
) {
    // -------------------------------------------------------------------------
    // CRITICAL PHASE UPDATE (Now accounts for Ratio and Detune!)
    // ALWAYS advance modulator shadow phase first!
    // Prevents phase-freeze glitches & attack clicks when modulation depth is 0.
    // -------------------------------------------------------------------------
    float actual_mod_hz = modulator_freq_Hz * xmod_cache.mod_freq_scalar;
    uint32_t phase = shadow_phase_xmod_q32[voice_idx];
    phase += (uint32_t)(actual_mod_hz * hz_to_phase_inc);
    shadow_phase_xmod_q32[voice_idx] = phase;

    // Fast 1-cycle hardware saturation [0, 32767]
    uint32_t total_mod_q15 = (uint32_t)__builtin_arm_usat(xmod_cache.base_xmod + mod_matrix_delta, 15);

    if (__builtin_expect(total_mod_q15 == 0, 1)) {
        return carrier_freq_Hz;
    }

    // -------------------------------------------------------------------------
    // 1-CYCLE HARDWARE WAVESHAPE GENERATION (No runtime divisions or switches)
    // -------------------------------------------------------------------------
    float shadow_mod;
    if constexpr (Shape == 1) { // Case 1: Sawtooth
        shadow_mod = (float)((int32_t)phase) * Q31_TO_FLOAT;
    }
    else if constexpr (Shape == 2) { // Case 2: Square (Zero math required, PWM bounded by Symmetry)
        shadow_mod = (phase < xmod_cache.symmetry_q32) ? 1.0f : -1.0f;
    }
    else if constexpr (Shape == 3) { // Case 3: Parabolic Sine (Glassy pure FM)
        // FMA transformation: 4x(1 - |x|) = 4x - 4x|x| = 4x + (4x * -|x|)
        float x  = (float)((int32_t)phase) * Q31_TO_FLOAT;
        float x4 = (float)((int32_t)phase) * FOUR_Q31_TO_FLOAT;
        shadow_mod = __builtin_fmaf(x4, -__builtin_fabsf(x), x4);
    }
    else { // Case 0: Triangle
        int32_t tri_q31 = (int32_t)((phase << 1) ^ ((int32_t)phase >> 31) ^ 0x80000000u);
        shadow_mod = (float)tri_q31 * Q31_TO_FLOAT;
    }

    // -------------------------------------------------------------------------
    // FREQUENCY MODULATION CALCULATION
    // -------------------------------------------------------------------------
    float out_freq_Hz;

    // Static / Prebaked path (Common Case: mod_matrix_delta == 0)
    if (__builtin_expect(mod_matrix_delta == 0, 1)) {
        if constexpr (Mode == 0) { // Centered Exponential FM
            // 1-cycle VFMS / VFMA
            float octaves = __builtin_fmaf(shadow_mod, xmod_cache.expo_depth, -xmod_cache.expo_bias);
            out_freq_Hz = carrier_freq_Hz * fast_exp2f_audio(octaves);
        } 
        else if constexpr (Mode == 1) { // Vintage Uncentered Exponential FM
            float octaves = shadow_mod * xmod_cache.expo_depth;
            out_freq_Hz = carrier_freq_Hz * fast_exp2f_audio(octaves);
        } 
        else { // Mode 2: True Linear FM
            // 1-cycle VFMA + 1-cycle VMUL
            float factor = __builtin_fmaf(shadow_mod, xmod_cache.lin_depth_norm, xmod_cache.lin_norm);
            out_freq_Hz = carrier_freq_Hz * factor;
        }
    } 
    // Dynamic mod-matrix path (When LFO/Env modulates crossmod depth)
    else {
        // Fetch precise Taylor fit coefficients for real-time drift compensation
        // Resolved at compile time as immediate constants (Zero SRAM bus read)
        constexpr float curve_lin_var = XMOD_CURVES[Shape].lin_var;
        constexpr float curve_expo_c1 = XMOD_CURVES[Shape].expo_c1;
        constexpr float curve_expo_c2 = XMOD_CURVES[Shape].expo_c2;

        if constexpr (Mode == 2) { // True Linear FM
            float d = (float)total_mod_q15 * LIN_DEPTH_SCALER;
            float l2 = (d * d) * curve_lin_var;
            // Fully inlined Horner evaluation via VFMA.F32
            float p = __builtin_fmaf(0.48f, l2, 0.12f);
            p       = __builtin_fmaf(p, l2, 0.33333334f);
            float norm = __builtin_fmaf(p, l2, 1.0f);
            float d_norm = d * norm;
            out_freq_Hz = carrier_freq_Hz * __builtin_fmaf(shadow_mod, d_norm, norm);
        } 
        else if constexpr (Mode == 0) { // Centered Exponential FM
            float d = (float)total_mod_q15 * EXPO_DEPTH_SCALER;
            float d2 = d * d;
            float bias = __builtin_fmaf(-curve_expo_c2, d2, curve_expo_c1) * d2;
            float octaves = __builtin_fmaf(shadow_mod, d, -bias);
            out_freq_Hz = carrier_freq_Hz * fast_exp2f_audio(octaves);
        } 
        else { // Mode 1: Vintage Uncentered Exponential FM
            float d = (float)total_mod_q15 * EXPO_DEPTH_SCALER;
            out_freq_Hz = carrier_freq_Hz * fast_exp2f_audio(shadow_mod * d);
        }
    }

    return out_freq_Hz;
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
    switch (xmod_cache.shape & 0x03) {
        case 0:  return apply_crossmod_impl<Mode, 0>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
        case 1:  return apply_crossmod_impl<Mode, 1>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
        case 2:  return apply_crossmod_impl<Mode, 2>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
        case 3:
        default: return apply_crossmod_impl<Mode, 3>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
    }
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
    const uint8_t shape = xmod_cache.shape & 0x03;
    switch (crossmod_mode) {
        case 0: // Centered Exponential FM
            switch (shape) {
                case 0:  return apply_crossmod_impl<0, 0>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
                case 1:  return apply_crossmod_impl<0, 1>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
                case 2:  return apply_crossmod_impl<0, 2>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
                case 3:
                default: return apply_crossmod_impl<0, 3>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
            }
        case 1: // Vintage Uncentered Exponential FM
            switch (shape) {
                case 0:  return apply_crossmod_impl<1, 0>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
                case 1:  return apply_crossmod_impl<1, 1>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
                case 2:  return apply_crossmod_impl<1, 2>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
                case 3:
                default: return apply_crossmod_impl<1, 3>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
            }
        case 2: // True Linear FM
        default:
            switch (shape) {
                case 0:  return apply_crossmod_impl<2, 0>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
                case 1:  return apply_crossmod_impl<2, 1>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
                case 2:  return apply_crossmod_impl<2, 2>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
                case 3:
                default: return apply_crossmod_impl<2, 3>(voice_idx, carrier_freq_Hz, modulator_freq_Hz, hz_to_phase_inc, mod_matrix_delta);
            }
    }
}