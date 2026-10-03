// 1-cycle-per-count PWM (sideset). DMA word = (low << 16) | high; autopull 32.
// Optional sideset: 4 instructions — shared by all RANGE SMs (pio1 SM2/3 + pio0 SM3).
// Dither is DMA (3-frame chain, wrap 4666), not extra PIO code.
#pragma once

#include "hardware/pio.h"

#define range_pwm_dither_wrap_target 0
#define range_pwm_dither_wrap 3

static const uint16_t range_pwm_dither_program_instructions[] = {
            //     .wrap_target
    0x6030, //  0: out    x, 16
    0x6050, //  1: out    y, 16
    0x1842, //  2: jmp    x--, 2          side 1
    0x1083, //  3: jmp    y--, 3          side 0
            //     .wrap
};

static const struct pio_program range_pwm_dither_program = {
    .instructions = range_pwm_dither_program_instructions,
    .length = 4,
    .origin = -1,
};

static inline pio_sm_config range_pwm_dither_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + range_pwm_dither_wrap_target,
                       offset + range_pwm_dither_wrap);
    sm_config_set_sideset(&c, 2, true, false);
    sm_config_set_out_shift(&c, true, true, 32);  // shift right, autopull 32
    return c;
}
