#pragma once
#include <stdint.h>
#include "hardware/sync.h"

// Compile-time immediate constants
static constexpr float MAX_LIN_DEPTH     = 0.85f;
static constexpr float MAX_EXPO_OCTAVES  = 2.5f;

static constexpr float LIN_DEPTH_SCALER  = MAX_LIN_DEPTH / 32768.0f;
static constexpr float EXPO_DEPTH_SCALER = MAX_EXPO_OCTAVES / 32768.0f;
static constexpr float SHADOW_TRI_SCALE  = 1.0f / 2147483648.0f; // 2^-31

struct XmodPrebaked {
    int32_t base_xmod;
    float   expo_depth;      // Scaled octaves
    float   expo_bias;       // Pre-calculated 2.5-octave bias
    float   lin_norm;        // Pre-calculated atanh period compensation
    float   lin_depth_norm;  // Fused constant (lin_depth * lin_norm) for 1-cycle VFMA
    bool    active;
};

// Global instance matching your existing placement
inline XmodPrebaked xmod_cache;
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
    float lin_norm  = 1.0f + d2 * (0.333333f + d2 * (0.12f + d2 * 0.48f));

    xmod_cache.lin_norm       = lin_norm;
    xmod_cache.lin_depth_norm = lin_depth * lin_norm;
}


// --- Optimized Fast Exp2 ---
static inline __attribute__((always_inline)) float fast_exp2f_audio(float p) {
    int32_t i;
  
    // 1. Hardware fast-floor (Single cycle on Cortex-M33 FPU)
    // Replaces: int32_t i = (int32_t)p; if (p < 0.0f) i--;
  #if defined(__ARM_ARCH) 
    __asm__ volatile ("vcvtm.s32.f32 %0, %1" : "=t"(i) : "t"(p));
  #else 
    // Fallback if compiled for RP2350's alternate RISC-V (Hazard3) cores
    i = (int32_t)p;
    i -= (p < 0.0f); // Branchless floor adjustment
  #endif
  
    float f = p - (float)i; 
    
    // Quadratic approximation for 2^f (Compiler will fuse these into FMA instructions)
    float approx = 1.0f + f * (0.695847f + f * 0.304153f);
    
    // Fast exponent reconstruction via IEEE-754
    union { uint32_t i; float f; } v;
    v.i = (uint32_t)(i + 127) << 23; 
    
    return approx * v.f;
  }


  static uint32_t shadow_phase_xmod_q32[NUM_VOICES_TOTAL] = {0};

/**
 * @brief Direction-agnostic Crossmod processor.
 * @param carrier_freq_Hz   The oscillator receiving the modulation (Destination)
 * @param modulator_freq_Hz The oscillator generating the modulation (Source)
 */
 static inline __attribute__((always_inline)) float SRAM_HOT(apply_crossmod)(
    uint8_t voice_idx,
    float carrier_freq_Hz,
    float modulator_freq_Hz,
    float hz_to_phase_inc,
    int32_t mod_matrix_delta
) {
    uint32_t total_mod_q15 = (uint32_t)__builtin_arm_usat(xmod_cache.base_xmod + mod_matrix_delta, 15);

    if (__builtin_expect(total_mod_q15 == 0, 1)) {
        return carrier_freq_Hz;
    }

    // Advance Modulator shadow phase using the MODULATOR's frequency
    uint32_t phase = shadow_phase_xmod_q32[voice_idx];
    phase += (uint32_t)(modulator_freq_Hz * hz_to_phase_inc);
    shadow_phase_xmod_q32[voice_idx] = phase;

    // RP2350 branchless triangle fold (ASRS + EOR)
    uint32_t p2 = phase << 1;
    uint32_t tri_u = p2 ^ (uint32_t)((int32_t)phase >> 31);
    float shadow_tri = (float)tri_u * SHADOW_TRI_SCALE - 1.0f;

    float out_freq_Hz;

    if (__builtin_expect(mod_matrix_delta == 0, 1)) {
        switch (crossmod_mode) {
            case 0: { // Centered Expo
                float octaves = shadow_tri * xmod_cache.expo_depth - xmod_cache.expo_bias;
                out_freq_Hz = carrier_freq_Hz * fast_exp2f_audio(octaves);
                break;
            }
            case 1: { // Vintage Uncentered Expo
                float octaves = shadow_tri * xmod_cache.expo_depth;
                out_freq_Hz = carrier_freq_Hz * fast_exp2f_audio(octaves);
                break;
            }
            case 2:
            default: { // True Linear FM
                float mod_mult = xmod_cache.lin_norm + shadow_tri * xmod_cache.lin_depth_norm;
                out_freq_Hz = carrier_freq_Hz * mod_mult;
                break;
            }
        }
    } else {
        // Dynamic mod-matrix fallback
        if (crossmod_mode == 2) {
            float d = (float)total_mod_q15 * LIN_DEPTH_SCALER;
            float d2 = d * d;
            float norm = 1.0f + d2 * (0.333333f + d2 * (0.12f + d2 * 0.48f));
            out_freq_Hz = carrier_freq_Hz * (norm + shadow_tri * (d * norm));
        } else {
            float d = (float)total_mod_q15 * EXPO_DEPTH_SCALER;
            float bias = (crossmod_mode == 0) ? ((0.115525f - (0.001655f * d * d)) * d * d) : 0.0f;
            out_freq_Hz = carrier_freq_Hz * fast_exp2f_audio((shadow_tri * d) - bias);
        }
    }

    return (out_freq_Hz < 10.0f) ? 10.0f : out_freq_Hz;
}