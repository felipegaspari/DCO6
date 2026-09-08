
// Compile mo-lfo into the sketch TU
#include "_build_libs/mo-lfo/mo-lfo.cpp"

void init_LFOs() {
  init_LFO1();
  init_LFO2();
}

void init_DRIFT_LFOs() {
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    init_DRIFT_LFO(LFO_DRIFT_CLASS[i], i);
  }
}

void init_DRIFT_LFO(lfo &LFO, byte LFONumber) {
  LFO_DRIFT_SPEED_OFFSET[LFONumber] = (float)(1.00f - (float)((float)analogDriftSpread * 0.005) + (float)((float)analogDriftSpread * 0.00125f * (float)LFONumber)) * (float)expConverterFloat((float)analogDriftSpeed, 5000);
  LFO.setWaveForm(LFO_DRIFT_WAVEFORM);
  LFO.setAmplQ15(MO_LFO_Q15_ONE);
  LFO.setMode(false);
  LFO.setMode0Freq(LFO_DRIFT_SPEED_OFFSET[LFONumber], micros());
}

void init_LFO1() {
  LFO1_class.setWaveForm(LFO1Waveform);
  LFO1_class.setAmplQ15(MO_LFO_Q15_ONE);
  LFO1_class.setMode(false);
  LFO1_class.setMode0Freq(0.5f);
}

void init_LFO2() {
  LFO2_class.setWaveForm(LFO2Waveform);
  LFO2_class.setAmplQ15(MO_LFO_Q15_ONE);
  LFO2_class.setMode(false);
  LFO2_class.setMode0Freq(5.0f);
}

// Pure float scale constant (Q15 wave * Q24 depth -> Float Octaves)
static constexpr float LFO_RAW_TO_OCTAVES_F = 1.0f / 549755813888.0f;

void SRAM_HOT(LFO1)() {
  LFO1Level = LFO1_class.getWaveQ15(micros()); // Retain Q15 wave

#if defined(USE_VOICE_ENGINE_FLOAT) || defined(USE_FLOAT_VOICE_TASK)
  const float wave = (float)LFO1Level;

  // Single-cycle VFMA: (wave * depth) + comp
  lfo1_pitch_mod_f[LFO1_PITCH_OSC1] = (wave * (LFO1toDCO_f + LFO1toOSC1_f));
  lfo1_pitch_mod_f[LFO1_PITCH_OSC2] = (wave * (LFO1toDCO_f + LFO1toOSC2_f));
  lfo1_pitch_mod_f[LFO1_PITCH_OSC3] = (wave * (LFO1toDCO_f + LFO1toOSC3_f));
#else
  // RP2040 fixed-point fallback
  lfo1_pitch_mod_q24[LFO1_PITCH_OSC1] = applyDepthQ24(LFO1Level, LFO1toDCO_q24 + LFO1toOSC1_q24);
  lfo1_pitch_mod_q24[LFO1_PITCH_OSC2] = applyDepthQ24(LFO1Level, LFO1toDCO_q24 + LFO1toOSC2_q24);
  lfo1_pitch_mod_q24[LFO1_PITCH_OSC3] = applyDepthQ24(LFO1Level, LFO1toDCO_q24 + LFO1toOSC3_q24);
#endif
  __dmb();
}

void SRAM_HOT(LFO2)() {
  LFO2Level = LFO2_class.getWaveQ15(micros()); // Retain Q15 wave

#if defined(USE_VOICE_ENGINE_FLOAT) || defined(USE_FLOAT_VOICE_TASK)
  const float wave = (float)LFO2Level;
  // 1 VADD.F32 + 1 VMUL.F32 per slot (Result in Float Octaves)
  lfo2_pitch_mod_f[LFO2_PITCH_OSC2] = wave * (LFO2toOSC2_f + LFO2toOSC2_coarse_f);
  lfo2_pitch_mod_f[LFO2_PITCH_OSC3] = wave * (LFO2toOSC3_f + LFO2toOSC3_coarse_f);
#else
  // RP2040 fixed-point fallback
  lfo2_pitch_mod_q24[LFO2_PITCH_OSC2] = applyDepthQ24(LFO2Level, LFO2toOSC2_q24 + LFO2toOSC2_coarse_q24);
  lfo2_pitch_mod_q24[LFO2_PITCH_OSC3] = applyDepthQ24(LFO2Level, LFO2toOSC3_q24 + LFO2toOSC3_coarse_q24);
#endif
  __dmb();
}


void SRAM_HOT(DRIFT_LFOs)() {
  unsigned long currentMicros = micros();
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    LFO_DRIFT_LEVEL[i] = (int16_t)(LFO_DRIFT_CLASS[i].getWaveQ15(currentMicros));
  }
  __dmb();
}