#ifndef __PWM_H__
#define __PWM_H__

#ifndef NUM_FILTERS
#define NUM_FILTERS 2
#endif

#include "hardware/pwm.h"

#ifdef RANGE0_PIO_DITHER_TEST
#include "range_pwm_dither.pio.h"
#endif

static constexpr uint32_t RANGE_PIO_FRAMES = 3;
static constexpr uint32_t RANGE_PIO_PERIOD = DIV_COUNTER / RANGE_PIO_FRAMES;
static constexpr uint32_t RANGE_PIO_LEVELS = RANGE_PIO_PERIOD * RANGE_PIO_FRAMES;

void init_pwm();
#ifdef RANGE0_PIO_DITHER_TEST
void init_range_pio_dither();
void range_pio_set_level(uint8_t osc, uint16_t level);
#endif

#ifdef ENABLE_CV_OUTS
void init_cv_pwm();
void write_cv_pwm();
void write_cv_pwm_raw(uint16_t cutoff, const uint16_t resonance[NUM_FILTERS], uint16_t vca,
                      uint16_t dist_drive, uint16_t dist_mix);
void init_level_pwm();
void write_level_pwm();
void write_level_pwm_raw(uint16_t osc1, uint16_t osc2, uint16_t osc3, uint16_t sub);
#endif

// RANGE pins → dithered PIO when RANGE0_PIO_DITHER_TEST; else slice PWM.
static inline void write_range_pwm(uint8_t osc, uint16_t level) {
#ifdef RANGE0_PIO_DITHER_TEST
  range_pio_set_level(osc, level);
  return;
#endif
  pwm_set_chan_level(RANGE_PWM_SLICES[osc], RANGE_PWM_CHANNELS[osc], level);
}

#endif
