#ifndef __WAVE_MUX_H__
#define __WAVE_MUX_H__

void init_waveSelector();
// Rebuild all 9 OSC×wave bits from waveEnable[][] and shift out.
void update_waveSelector();
// Manual cal: all off, then enable that osc's saw (sub 0) or pulse (sub 1/2 —
// the 440 Hz substage plays the square).
void waveSelector_manual_calibration(byte stage);

#endif
