#ifndef __NOISE_H__
#define __NOISE_H__

#ifndef NOISE_ENGINE
#define NOISE_ENGINE 0
#endif

// Sketch-side noise objects (Arduino style). Engines / DcoNoiseGen live in DCO_Noise.
// Ctor: (color, seed). Output is always full int16 Q15 (−32768…32767).
// Set NOISE_ENGINE / ENABLE_NOISE_OUT in DCO.ino before includes.
// Fleet is two gens: noise0 white (Character amp/PW), noise1 pink (Character pitch).

#include "../_build_libs/DCO_Noise/DCO_Noise.h"

static constexpr uint8_t NUM_NOISE_GENS = 2;

DcoNoiseGen noise0(NOISE_WHITE, 0xC0FFEE01u);
DcoNoiseGen noise1(NOISE_PINK,  0xC0FFEE02u);

// For mod matrix / indexed access (PrimeHybridNoise is not copyable).
DcoNoiseGen* const noiseGens[NUM_NOISE_GENS] = {
  &noise0, &noise1
};

volatile int16_t noiseLevel[NUM_NOISE_GENS];

#endif  // __NOISE_H__
