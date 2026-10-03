#include "_shared/voices.h"

// Sketch-owned pitch entry: OSC A via TXF period word, OSC B via DMA SM clkdiv.
// Calibration / shared must call this instead of clkdiv macros or pio_sm_put.
void dco_set_osc_freq(uint8_t osc, float freqHz);
