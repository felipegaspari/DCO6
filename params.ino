/**
 * @file DCO-params.ino
 * @brief Parameter Router, Jump Table Dispatcher, and Preset Applier.
 * 
 * Maps incoming 16-bit parameter IDs to high-performance static appliers,
 * bakes modulation depths and drift LFO frequencies on write, and manages 
 * parameter gating during calibration routines.
 */

 #include "include_all.h"

 #if __has_include("bench.h")
 #include "bench.h"
 #elif __has_include("../bench.h")
 #include "../bench.h"
 #endif
 
 #include "hardware/watchdog.h"
 #include "pico/bootrom.h"
 #include "pico/multicore.h"
 

 // =============================================================================
 // 1. OSCILLATOR & VOICE CONFIGURATION APPLIERS
 // =============================================================================

 static void SRAM_HOT(apply_param_osc1_pulse_enable)(int16_t v) { pulseWaveOn = (v != 0); }
 static void SRAM_HOT(apply_param_osc1_interval)(int16_t v) { octave_shift = (int8_t)v; }
 static void SRAM_HOT(apply_param_osc2_interval)(int16_t v) { OSC2_interval = (int8_t)v; }
 static void SRAM_HOT(apply_param_osc3_interval)(int16_t v) { OSC3_interval = (int8_t)v; }
 static void SRAM_HOT(apply_param_osc2_detune)(int16_t v) { OSC2_detune = (uint16_t)v; }
 static void SRAM_HOT(apply_param_osc3_detune)(int16_t /*v*/) { /* DCO3 monosynth only */ }
 static void SRAM_HOT(apply_param_unison_detune)(int16_t v) { unisonDetune = v; }
 
 static void SRAM_HOT(apply_param_portamento_time)(int16_t v) {
 
   if (v == 0) {
     portamento_time       = 0;
     return;
   }
   portamento_time = v;
   const uint32_t val2 = v * v;
   const uint32_t val3 = val2 * v;
   const uint32_t t    = (v * 60u) + (val2 * 3u) + ((val3 * 200u) >> 10);
   portamento_time_fixed = t ;
   portamento_time_slew  = t ;
 }
 
 static void SRAM_HOT(apply_param_portamento_mode)(int16_t v) {
   portamento_mode = (uint8_t)v;
 }
 
 static void bake_drift_lfo_frequencies() {
   const uint32_t now_us = micros();
 
 #if __has_builtin(__builtin_exp) || defined(USE_FLOAT_VOICE_TASK)
   const float base_speed = fast_exp_speed_5000(analogDriftSpeed);
 #else
   const float base_speed = (float)expConverterFloat((float)analogDriftSpeed, 5000);
 #endif
 
   const float spread_f   = (float)analogDriftSpread;
   const float base_freq  = (1.00f - (spread_f * 0.005f)) * base_speed;
   const float step_freq  = (spread_f * 0.00125f) * base_speed;
 
   for (int i = 0; i < NUM_OSCILLATORS; i++) {
     const float freq = base_freq + ((float)i * step_freq);
     LFO_DRIFT_SPEED_OFFSET[i] = freq;
     LFO_DRIFT_CLASS[i].setMode0Freq(freq, now_us);
   }
 }
 
 static void SRAM_HOT(apply_param_analog_drift_amount)(int16_t v) {
  analogDrift = v;

#if defined(USE_FLOAT_VOICE_TASK)
  // 0.0000005f * 1000.0f = 0.0005 Octaves per drift step, divided by 32768 Q15 units
  static constexpr float DRIFT_PITCH_PER_Q15 = (0.0000005f * 1000.0f) / 32768.0f;
  drift_pitch_scale_f = (float)analogDrift * DRIFT_PITCH_PER_Q15;
#else
  // static constexpr int32_t DRIFT_SCALE_MULT =
  //     (int32_t)(DRIFT_PITCH_UNIT_Q24 * DRIFT_PITCH_DEPTH_SCALE);                //// NEEDS FIX !!!
  // drift_pitch_scale_q24 = (int32_t)((int32_t)analogDrift * DRIFT_SCALE_MULT);
 // vcf_drift_scale_q15 = (int32_t)analogDrift;
#endif
}
 
 static void SRAM_HOT(apply_param_analog_drift_speed)(int16_t v) {
   analogDriftSpeed = v;
   bake_drift_lfo_frequencies();
 }
 
 static void SRAM_HOT(apply_param_analog_drift_spread)(int16_t v) {
   analogDriftSpread = v;
   bake_drift_lfo_frequencies();
 }
 
 static void SRAM_HOT(apply_param_voice_mode)(int16_t v) {
   voiceMode = (uint8_t)constrain(v, 0, 2);
   setVoiceMode(voiceMode);
 }
 
 static void SRAM_HOT(apply_param_voice_alloc_mode)(int16_t v) {
   voiceAlloc.setMode((uint8_t)constrain(v, 0, 8));
 }
 
 static void SRAM_HOT(apply_param_sync_mode)(int16_t v) {
   syncMode = (uint8_t)v;
   setSyncMode();
 }
 
 static void SRAM_HOT(apply_param_phase_align)(int16_t v) {
   oscPhaseSync = v;
   if (oscPhaseSync < 2) {
     phaseAlignOSC2 = 0;
     pio_defer_request_reset_pulse_all();
   } else {
     if (oscPhaseSync > 8) {
       phaseAlignOSC2 = oscPhaseSync * 2;
     } else {
       switch (oscPhaseSync) {
         case 2: phaseAlignOSC2 = 45;  break;
         case 3: phaseAlignOSC2 = 90;  break;
         case 4: phaseAlignOSC2 = 135; break;
         case 5: phaseAlignOSC2 = 180; break;
         case 6: phaseAlignOSC2 = 225; break;
         case 7: phaseAlignOSC2 = 270; break;
         case 8: phaseAlignOSC2 = 315; break;
         default: break;
       }
     }
   }
   for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
     note_on_flag[i] = 1;
   }
 }
 
static void SRAM_HOT(apply_param_soft_sync)(int16_t v) {
   softSyncChunks = (uint8_t)v;
   setSyncMode();
}

static void SRAM_HOT(apply_param_crossmod_depth)(int16_t v) {
  crossmod_depth = (int16_t)v;
  update_crossmod_prebake(crossmod_depth);
}

static void SRAM_HOT(apply_param_crossmod_mode)(int16_t v)   { 
  crossmod_mode = (uint8_t)v; 
}

 static void SRAM_HOT(apply_param_subosc_divide)(int16_t v) { subOscDivide = (uint8_t)v; }
 
 // =============================================================================
 // 2. LFO SPEEDS, WAVEFORMS & PITCH DEPTHS
 // =============================================================================
 
 static void SRAM_HOT(apply_param_lfo1_waveform)(int16_t v) {
   LFO1Waveform = (uint8_t)v;
   LFO1_class.setWaveForm(LFO1Waveform);
 }
 
 static void SRAM_HOT(apply_param_lfo2_waveform)(int16_t v) {
   LFO2Waveform = (uint8_t)v;
   LFO2_class.setWaveForm(LFO2Waveform);
 }
 
 static void SRAM_HOT(apply_param_lfo1_speed) (int16_t v) {
   LFO1SpeedVal = (uint16_t)v;
   LFO1Speed = fast_exp_speed_5000(LFO1SpeedVal);
   LFO1_class.setMode0Freq(LFO1Speed, micros());
 }
 
 static void SRAM_HOT(apply_param_lfo2_speed)(int16_t v) {
   LFO2SpeedVal = (uint16_t)v;
   LFO2Speed = fast_exp_speed_5000(LFO2SpeedVal);
   LFO2_class.setMode0Freq(LFO2Speed, micros());
 }
 
// =============================================================================
// 2. LFO SPEEDS, WAVEFORMS & PITCH DEPTHS
// =============================================================================

// --- LFO1 Pitch Appliers (4 Octaves Max) ---

// --- LFO1 Pitch Appliers ---

static void SRAM_HOT(apply_param_lfo1_to_dco)(int16_t v) {
  LFO1toDCOVal = (uint16_t)constrain(v, 0, 511);
#if defined(USE_FLOAT_VOICE_TASK)
  LFO1toDCO_f = lfo_pitch_depth_f(fast_lfo_depth_norm<511>(LFO1toDCOVal), LFO_4_OCTAVES);
#else
  LFO1toDCO_q24 = lfo_pitch_depth_q24(fast_lfo_depth_norm<511>(LFO1toDCOVal), LFO_4_OCTAVES_Q24);
#endif
}

static void SRAM_HOT(apply_param_lfo1_to_osc1)(int16_t v) {
  uint16_t val = (uint16_t)constrain(v, 0, 255);
#if defined(USE_FLOAT_VOICE_TASK)
  LFO1toOSC1_f = lfo_pitch_depth_f(fast_lfo_depth_norm<255>(val), LFO_4_OCTAVES);
#else
  LFO1toOSC1_q24 = lfo_pitch_depth_q24(fast_lfo_depth_norm<255>(val), LFO_COARSE_2_OCTAVES_Q24);
#endif
}

static void SRAM_HOT(apply_param_lfo1_to_osc2)(int16_t v) {
  uint16_t val = (uint16_t)constrain(v, 0, 255);
#if defined(USE_FLOAT_VOICE_TASK)
  LFO1toOSC2_f = lfo_pitch_depth_f(fast_lfo_depth_norm<255>(val), LFO_4_OCTAVES);
#else
  LFO1toOSC2_q24 = lfo_pitch_depth_q24(fast_lfo_depth_norm<255>(val), LFO_COARSE_2_OCTAVES_Q24);
#endif
}

static void SRAM_HOT(apply_param_lfo1_to_osc3)(int16_t v) {
  uint16_t val = (uint16_t)constrain(v, 0, 255);
#if defined(USE_FLOAT_VOICE_TASK)
  LFO1toOSC3_f = lfo_pitch_depth_f(fast_lfo_depth_norm<255>(val), LFO_4_OCTAVES);
#else
  LFO1toOSC3_q24 = lfo_pitch_depth_q24(fast_lfo_depth_norm<255>(val), LFO_COARSE_2_OCTAVES_Q24);
#endif
}

// --- LFO2 Pitch Appliers ---

static void SRAM_HOT(apply_param_lfo2_to_osc2)(int16_t v) {
  uint16_t val = (uint16_t)constrain(v, 0, 255);
#if defined(USE_FLOAT_VOICE_TASK)
  LFO2toOSC2_f = lfo_pitch_depth_f(fast_lfo_depth_norm<255>(val), LFO_VIBRATO_2_SEMITONES);
#else
  LFO2toOSC2_q24 = lfo_pitch_depth_q24(fast_lfo_depth_norm<255>(val), LFO_VIBRATO_2_SEMITONES_Q24);
#endif
}

static void SRAM_HOT(apply_param_lfo2_to_osc3)(int16_t v) {
  uint16_t val = (uint16_t)constrain(v, 0, 255);
#if defined(USE_FLOAT_VOICE_TASK)
  LFO2toOSC3_f = lfo_pitch_depth_f(fast_lfo_depth_norm<255>(val), LFO_VIBRATO_2_SEMITONES);
#else
  LFO2toOSC3_q24 = lfo_pitch_depth_q24(fast_lfo_depth_norm<255>(val), LFO_VIBRATO_2_SEMITONES_Q24);
#endif
}

static void SRAM_HOT(apply_param_lfo2_to_osc2_coarse)(int16_t v) {
  uint16_t val = (uint16_t)constrain(v, 0, 511); 
#if defined(USE_FLOAT_VOICE_TASK)
  LFO2toOSC2_coarse_f = lfo_pitch_depth_f(fast_lfo_depth_norm<511>(val), LFO_4_OCTAVES);
#else
  LFO2toOSC2_coarse_q24 = lfo_pitch_depth_q24(fast_lfo_depth_norm<511>(val), LFO_4_OCTAVES_Q24);
#endif
}

static void SRAM_HOT(apply_param_lfo2_to_osc3_coarse)(int16_t v) {
  uint16_t val = (uint16_t)constrain(v, 0, 511);
#if defined(USE_FLOAT_VOICE_TASK)
  LFO2toOSC3_coarse_f = lfo_pitch_depth_f(fast_lfo_depth_norm<511>(val), LFO_4_OCTAVES);
#else
  LFO2toOSC3_coarse_q24 = lfo_pitch_depth_q24(fast_lfo_depth_norm<511>(val), LFO_4_OCTAVES_Q24);
#endif
}
 
static void SRAM_HOT(apply_param_lfo2_to_pw)(int16_t v) { 
  // Dynamically scales the input depth to match DIV_COUNTER_PW (normalized against 1024 base)
  LFO2toPW = (uint16_t)(((uint32_t)v * DIV_COUNTER_PW) / 1024); 
}
 
 // =============================================================================
 // 3. ANALOG DRIFT, CHARACTER & PULSE WIDTH
 // =============================================================================
 
 static void SRAM_HOT(apply_param_character)(int16_t v) { 
  character = (uint8_t)constrain(v, 0, 128);
  character_recompute_scales();
}

 static void SRAM_HOT(apply_param_pw_value) (int16_t v) { PW[0] = (uint16_t)(v >> 1);}
 
 // =============================================================================
 // 4. ENVELOPE MODULATIONS, CURVES & TRIGGER MODES
 // =============================================================================
 
 static void SRAM_HOT(apply_param_adsr3_to_osc_select)(int16_t v) {
   ADSR3ToOscSelect = (int8_t)v;
 }
 
 static void SRAM_HOT(apply_param_adsr3_to_pwm)(int16_t v) {
  ADSR3toPWM = (int16_t)(((int32_t)(v - 512) * (int32_t)DIV_COUNTER_PW) / 1024);
  ADSR3toPWM_scale = ADSR3toPWM;
 }
 
/**
 * @brief Bakes EnvDCO (ADSR3) modulation depth to OSC1 pitch.
 * @details Multiplier 65664 maps full panel range (0..511) to +/-2.0 Octaves in Q24.
 * @param v Parameter value (0..511).
 */
 static void SRAM_HOT(apply_param_adsr3_to_detune1)(int16_t v) {
  ADSR3toDETUNE1 = v;

#if defined(USE_FLOAT_VOICE_TASK)
  // (2.0 Octaves / 511 panel steps) / 32768 Q15 units
  static constexpr float ADSR_PITCH_STEP_F = 2.0f / (511.0f * 32768.0f);
  ADSR3toDETUNE1_scale_f = (float)v * ADSR_PITCH_STEP_F;
#else
  ADSR3toDETUNE1_scale_q24 = (int32_t)v * 65664; 
#endif
}
 
static void SRAM_HOT(apply_param_adsr3_mode)(int16_t v) { 
  ADSR3Mode = (uint8_t)v; 
  ADSR3_set_mode(ADSR3Mode);
}

static void SRAM_HOT(apply_param_adsr2_mode)(int16_t v) { 
  ADSR2Mode = (uint8_t)v; 
  ADSR2_set_mode(ADSR2Mode);
}

static void SRAM_HOT(apply_param_adsr1_mode)(int16_t v) { 
  ADSR1Mode = (uint8_t)v; 
  ADSR1_set_mode(ADSR1Mode);
}

 // --- VCA Curves (ADSR1) ---
 static void SRAM_HOT(apply_param_adsr1_attack_curve)(int16_t v) {
   ADSR1AttackCurveVal = (uint8_t)v;
   ADSR_VCA_change_attack_curve(ADSR1AttackCurveVal);
 }
 static void SRAM_HOT(apply_param_adsr1_decay_curve)(int16_t v) {
   ADSR1DecayCurveVal = (uint8_t)v;
   ADSR_VCA_change_decay_curve(ADSR1DecayCurveVal);
 }
 static void SRAM_HOT(apply_param_adsr1_release_curve)(int16_t v) {
   ADSR1ReleaseCurveVal = (uint8_t)v;
   ADSR_VCA_change_release_curve(ADSR1ReleaseCurveVal);
 }
 
 // --- VCF Curves (ADSR2) ---
 static void SRAM_HOT(apply_param_adsr2_attack_curve)(int16_t v) {
   ADSR2AttackCurveVal = (uint8_t)v;
   ADSR_VCF_change_attack_curve(ADSR2AttackCurveVal);
   ADSR_VCF2_change_attack_curve(ADSR2AttackCurveVal);
 }
 static void SRAM_HOT(apply_param_adsr2_decay_curve)(int16_t v) {
   ADSR2DecayCurveVal = (uint8_t)v;
   ADSR_VCF_change_decay_curve(ADSR2DecayCurveVal);
   ADSR_VCF2_change_decay_curve(ADSR2DecayCurveVal);
 }
 static void SRAM_HOT(apply_param_adsr2_release_curve)(int16_t v) {
   ADSR2ReleaseCurveVal = (uint8_t)v;
   ADSR_VCF_change_release_curve(ADSR2ReleaseCurveVal);
   ADSR_VCF2_change_release_curve(ADSR2ReleaseCurveVal);
 }
 
 // --- DCO Curves (ADSR3) ---
 static void SRAM_HOT(apply_param_adsr3_attack_curve)(int16_t v) {
   ADSR3AttackCurveVal = (uint8_t)v;
   ADSR3_change_attack_curve(ADSR3AttackCurveVal);
 }
 static void SRAM_HOT(apply_param_adsr3_decay_curve)(int16_t v) {
   ADSR3DecayCurveVal = (uint8_t)v;
   ADSR3_change_decay_curve(ADSR3DecayCurveVal);
 }
 static void SRAM_HOT(apply_param_adsr3_release_curve)(int16_t v) {
   ADSR3ReleaseCurveVal = (uint8_t)v;
   ADSR3_change_release_curve(ADSR3ReleaseCurveVal);
 }
 
 static void SRAM_HOT(apply_param_adsr3_restart)(int16_t v) { ADSR3Restart = (uint8_t)v; }
 static void SRAM_HOT(apply_param_adsr2_restart)(int16_t v) { ADSR2Restart = (uint8_t)v; }
 static void SRAM_HOT(apply_param_adsr1_restart)(int16_t v) { ADSR1Restart = (uint8_t)v; }

 // --- Shared VCF Trigger Mode ---
 static void SRAM_HOT(apply_param_vcf_trigger_mode)(int16_t v) {
   set_vcf_trigger_mode((uint8_t)v);
 }
 
 // =============================================================================
 // 5. CALIBRATION CONTROLS & STORAGE TRIMS
 // =============================================================================
 
 static void dco_send_all_calibration_data() {
   for (uint8_t i = 0; i < NUM_OSCILLATORS; i++) {
     uint32_t packed = ((uint32_t)i << 8) | (uint8_t)manualCalibrationOffset[i];
     serialSendParam32(PARAM_MANUAL_CALIBRATION_OFFSET_FROM_DCO, packed);
   }
   for (uint8_t i = 0; i < NUM_OSCILLATORS; i++) {
     uint32_t packed = ((uint32_t)i << 16) | (uint16_t)ampComp440[i];
     serialSendParam32(PARAM_AMP_COMP_440, packed);
   }
   for (uint8_t ch = 0; ch < 4; ch++) {
     uint32_t packed = ((uint32_t)ch << 16) | (uint16_t)PW_CENTER[ch];
     serialSendParam32(PARAM_CAL_PW_CENTER, packed);
   }
   for (uint8_t i = 0; i < NUM_OSCILLATORS; i++) {
     uint32_t packed = ((uint32_t)i << 16) | (uint16_t)ampCompDutyOffset[i];
     serialSendParam32(PARAM_AMP_COMP_DUTY_OFFSET, packed);
   }
 }
 
 static void SRAM_HOT(apply_param_manual_calibration_flag)(int16_t v) {
   manualCalibrationFlag = (v != 0);
   calibrationFlag = (v != 0);
   if (v != 0) {
     dco_send_all_calibration_data();
   }
 }
 
 static void SRAM_HOT(apply_param_calibration_flag)(int16_t v) {
  if (v == 0) {
    calibrationCancelRequested = true;
    calibrationFlag = false;
    calibrationVerifyRequested = false; // <-- ADD THIS to fully cancel everything
    return;
  }

  uint8_t scopeVal = (uint8_t)v;
  if (scopeVal >= 8) {
    calibrationPrecision = CAL_PRECISION_FAST;
    scopeVal -= 8;
  } else if (scopeVal >= 4) {
    calibrationPrecision = CAL_PRECISION_FINE;
    scopeVal -= 4;
  } else {
    calibrationPrecision = CAL_PRECISION_NORMAL;
  }

  if (scopeVal == 1) {
    calibrationScope = CAL_SCOPE_AMP;
  } else if (scopeVal == 2) {
    calibrationScope = CAL_SCOPE_PW;
  } else {
    calibrationScope = CAL_SCOPE_FULL;
  }

  calibrationCancelRequested = false;
  calibrationFlag = true;
}
 
 static void SRAM_HOT(apply_param_manual_calibration_stage)(int16_t v) {
   manualCalibrationStage = (uint8_t)v;
   manualCalibrationStep =
       cal_stage_is_440_n(manualCalibrationStage, NUM_OSCILLATORS) ? 1 : 0;
 }
 
 static void SRAM_HOT(apply_param_manual_calibration_offset)(int16_t v) {
   uint8_t osc = cal_stage_to_osc_n(manualCalibrationStage, NUM_OSCILLATORS);
   if (osc < NUM_OSCILLATORS) {
     manualCalibrationOffset[osc] = (int8_t)v;
   }
 }
 
 static void SRAM_HOT(apply_param_manual_calibration_step)(int16_t v) {
   manualCalibrationStep = (uint8_t)v;
 }
 
 static void SRAM_HOT(apply_param_amp_comp_440)(int16_t v) {
   uint8_t osc = cal_stage_to_osc_n(manualCalibrationStage, NUM_OSCILLATORS);
   if (osc < NUM_OSCILLATORS) {
     ampComp440[osc] =
         (uint16_t)constrain(v, AMP_COMP_440_MIN, AMP_COMP_440_MAX);
   }
 }
 
 static void SRAM_HOT(apply_param_cal_pw_center)(int16_t v) {
   uint8_t osc = cal_stage_to_osc_n(manualCalibrationStage, NUM_OSCILLATORS);
   uint8_t ch = osc / 2;
   if (ch < NUM_PW_CHANNELS) {
     PW_CENTER[ch] = (uint16_t)constrain(v, 0, CAL_PW_CENTER_MAX);
   }
 }
 
 static void SRAM_HOT(apply_param_amp_comp_duty_offset)(int16_t v) {
   uint8_t osc = cal_stage_to_osc_n(manualCalibrationStage, NUM_OSCILLATORS);
   if (osc < NUM_OSCILLATORS) {
     ampCompDutyOffset[osc] = (int16_t)constrain(v, -500, 500);
   }
 }
 
 static void SRAM_HOT(apply_param_manual_calibration_store)(int16_t /*v*/) {
   for (uint8_t i = 0; i < NUM_OSCILLATORS; i++) {
     update_FS_ManualCalibrationOffset(i, manualCalibrationOffset[i]);
     update_FS_AmpComp440(i, ampComp440[i]);
     update_FS_AmpCompDutyOffset(i, ampCompDutyOffset[i]);
   }
   for (uint8_t ch = 0; ch < 4; ch++) {
     update_FS_PWCenter(ch, PW_CENTER[ch]);
   }
 }
 
 // =============================================================================
 // 6. DIAGNOSTIC & DEBUG COMMANDS (PARAM_DEBUG_COMMAND = 160)
 // =============================================================================
 
 static void SRAM_HOT(apply_param_debug_command)(int16_t v) {
  const uint16_t u = (uint16_t)v;
  const uint8_t hi = (uint8_t)(u >> 8);
  const uint8_t lo = (uint8_t)(u & 0xFF);

  // 1. Packed Hex Jitter Commands (0xC8xx, 0xCAxx, 0xCBxx)
  if (hi == 0xC8) {
    ampCompJitter = (lo > 128) ? 128 : lo;
    character_recompute_scales();
    return;
  }
  if (hi == 0xCA) {
    pitchJitter = (lo > 128) ? 128 : lo;
    character_recompute_scales();
    return;
  }
  if (hi == 0xCB) {
    pulsewidthJitter = (lo > 128) ? 128 : lo;
    character_recompute_scales();
    return;
  }

  // 2. Hardware Reset Pulse Length (200 .. 50000 cycles)
  // Check that hi is not one of our reserved command prefixes
  if (hi < 0xC0 && u >= 200 && u <= 50000) {
    pioPulseLength = u;
    pio_defer_request_reset_pulse_all();
    return;
  }

  // 3. Discrete Command Dispatch
  switch (v) {
    case 1:
      pio_topology_report();
      break;
    case 2:
      pio_period_probe(0, 100);
      break;
    case 3:
      pio_period_probe(0, 50000);
      break;
    case 5:
      print_dma_pwm_report();
      break;
    case 6:
      print_mcu_dma_map();
      break;

#ifdef RUNNING_AVERAGE
    case 10:
      bench_dump_request = true;
      break;
    case 11:
      bench_reset_all();
      break;
    case 12:
      bench_periodic = !bench_periodic;
      break;
#endif

#ifdef ENABLE_MEM_DIAG
    case 13:
      mem_diag_request();
      break;
    case 14:
      mem_diag_runtime_enabled = false;
      break;
    case 15:
      mem_diag_runtime_enabled = true;
      break;
#endif

#if defined(USE_FLOAT_AMP_COMP)
    case 20:
      amp_comp_set_method(AMP_COMP_FLOAT_QUAD);
      break;
    case 21:
      amp_comp_set_method(AMP_COMP_LUT);
      break;
    case 22:
      amp_comp_set_method(AMP_COMP_FIXED);
      break;
#endif

#if defined(AMP_COMP_BENCHMARK)
    case 24:
      amp_comp_bench_speed_pending = true;
      break;
    case 25:
      amp_comp_bench_accuracy_pending = true;
      break;
#endif
    case 28:
      pitch_interp_bench_speed_pending = true;
      break;
    case 29:
      pitch_interp_bench_accuracy_pending = true;
      break;
    case 30:
      seed_fake_calibration_tables(true);
      break;
    case 34:
      autotuneAmpMethod = 0;
      break;
    case 35:
      autotuneAmpMethod = 1;
      break;
    case 36:
      calibrationVerifyRequested = true;
      break;
    case 37:
      autotuneSearchMode = 0;
      break;
    case 38:
      autotuneSearchMode = 1;
      break;
    case 39:
      autotuneSearchMode = 2;
      break;
    case 40:
      autotuneAmp0Mode = 0;
      break;
    case 41:
      autotuneAmp0Mode = 1;
      break;
    case 46:
      pwCvProbeRequested = true;
      break;

    case 42:
    case 43:
    case 44:
    case 45:
      serialSendParam16(PARAM_DEBUG_COMMAND, v);
      break;

    case 90:
      Serial.println("[mcu] Rebooting system...");
      Serial.flush();
      delay(30);
#if defined(PICO_MULTICORE) || defined(ARDUINO_ARCH_RP2040)
      multicore_reset_core1();
#endif
#if defined(ARDUINO_ARCH_RP2040)
      rp2040.reboot();
#else
      watchdog_reboot(0, 0, 0);
#endif
      break;

    case 91:
      Serial.println("[mcu] Entering BOOTSEL mode...");
      Serial.flush();
      delay(30);
#if defined(PICO_MULTICORE) || defined(ARDUINO_ARCH_RP2040)
      multicore_reset_core1();
#endif
#if defined(ARDUINO_ARCH_RP2040)
      rp2040.rebootToBootloader();
#else
      reset_usb_boot(0, 0);
#endif
      break;

    default:
      break;
  }
}
 
 // =============================================================================
 // 7. PRESET STORE, CALIBRATION DUMP & RECALL
 // =============================================================================
 
 static void apply_param_preset_save(int16_t v) { preset_store_save((uint8_t)v); }
 static void apply_param_preset_load(int16_t v) { preset_store_load((uint8_t)v); }
 static void apply_param_preset_dump(int16_t v) { preset_store_dump((int16_t)v); }
 static void apply_param_cal_dump(int16_t v)    { preset_store_cal_dump(v); }
 static void SRAM_HOT(apply_param_ui_preset_scroll)(int16_t v) {
   const uint8_t slot = (uint8_t)v;
   currentPresetSlot = slot;
   serial_send_preset_scroll_to_mb(slot);
   serial_send_preset_loaded_to_mb(slot);
 }
 
 // =============================================================================
 // MODULATION MATRIX SLOT APPLIERS (MUST BE ABOVE paramTable[])
 // =============================================================================
 // They now live in mod_matrix_engine.h


 // =============================================================================
 // 8. ROUTER TABLE & DISPATCH JUMP SETUP
 // =============================================================================
 
 /** @brief Master descriptor table binding Parameter IDs to applier functions. */
 static const ParamDescriptorT<int16_t> paramTable[] = {
     {PARAM_OSC1_PULSE_ENABLE, apply_param_osc1_pulse_enable},
     {PARAM_OSC1_INTERVAL, apply_param_osc1_interval},
     {PARAM_OSC2_INTERVAL, apply_param_osc2_interval},
     {PARAM_OSC3_INTERVAL, apply_param_osc3_interval},
     {PARAM_OSC2_DETUNE_VAL, apply_param_osc2_detune},
     {PARAM_OSC3_DETUNE_VAL, apply_param_osc3_detune},
     {PARAM_UNISON_DETUNE, apply_param_unison_detune},
     {PARAM_PORTAMENTO_TIME, apply_param_portamento_time},
     {PARAM_PORTAMENTO_MODE, apply_param_portamento_mode},
     {PARAM_VOICE_MODE, apply_param_voice_mode},
     {PARAM_VOICE_ALLOC_MODE, apply_param_voice_alloc_mode},
     {PARAM_OSC_PHASE_SYNC, apply_param_phase_align},
     {PARAM_SYNC_MODE, apply_param_sync_mode},
     {PARAM_SOFT_SYNC, apply_param_soft_sync},
     {PARAM_SUBOSC_DIVIDE, apply_param_subosc_divide},
     // --- Digital Modulation ---
     {PARAM_CROSSMOD_DEPTH, apply_param_crossmod_depth},
     {PARAM_CROSSMOD_MODE, apply_param_crossmod_mode},
     // --- LFOs ---
     {PARAM_LFO1_WAVEFORM, apply_param_lfo1_waveform},
     {PARAM_LFO2_WAVEFORM, apply_param_lfo2_waveform},
     {PARAM_LFO1_SPEED, apply_param_lfo1_speed},
     {PARAM_LFO2_SPEED, apply_param_lfo2_speed},
     {PARAM_LFO1_TO_DCO, apply_param_lfo1_to_dco},
     {PARAM_LFO1_TO_OSC1, apply_param_lfo1_to_osc1},
     {PARAM_LFO1_TO_OSC2, apply_param_lfo1_to_osc2},
     {PARAM_LFO1_TO_OSC3, apply_param_lfo1_to_osc3},
     {PARAM_LFO2_TO_OSC2, apply_param_lfo2_to_osc2},
     {PARAM_LFO2_TO_OSC3, apply_param_lfo2_to_osc3},
     {PARAM_LFO2_TO_OSC2_COARSE, apply_param_lfo2_to_osc2_coarse},
     {PARAM_LFO2_TO_OSC3_COARSE, apply_param_lfo2_to_osc3_coarse},
     {PARAM_LFO2_TO_PW, apply_param_lfo2_to_pw},
 
     {PARAM_ANALOG_DRIFT_AMOUNT, apply_param_analog_drift_amount},
     {PARAM_ANALOG_DRIFT_SPEED, apply_param_analog_drift_speed},
     {PARAM_ANALOG_DRIFT_SPREAD, apply_param_analog_drift_spread},
     // --- Character ---
     {PARAM_CHARACTER, apply_param_character},
     // --- Pulse Width ---
     {PARAM_PW_VALUE, apply_param_pw_value},

     // --- Envelope Modulations ---
     {PARAM_ADSR3_TO_OSC_SELECT, apply_param_adsr3_to_osc_select},
     {PARAM_ADSR3_TO_PWM, apply_param_adsr3_to_pwm},
     {PARAM_ADSR3_TO_DETUNE1, apply_param_adsr3_to_detune1},

     // --- Envelope Curve Presets & Trigger Mode ---
     {PARAM_ADSR1_ATTACK_CURVE, apply_param_adsr1_attack_curve},
     {PARAM_ADSR1_DECAY_CURVE, apply_param_adsr1_decay_curve},
     {PARAM_ADSR1_RELEASE_CURVE, apply_param_adsr1_release_curve},
     {PARAM_ADSR2_ATTACK_CURVE, apply_param_adsr2_attack_curve},
     {PARAM_ADSR2_DECAY_CURVE, apply_param_adsr2_decay_curve},
     {PARAM_ADSR2_RELEASE_CURVE, apply_param_adsr2_release_curve},
     {PARAM_ADSR3_ATTACK_CURVE, apply_param_adsr3_attack_curve},
     {PARAM_ADSR3_DECAY_CURVE, apply_param_adsr3_decay_curve},
     {PARAM_ADSR3_RELEASE_CURVE, apply_param_adsr3_release_curve},
     {PARAM_VCF_TRIGGER_MODE, apply_param_vcf_trigger_mode},

     // --- Envelope Restart ---
     {PARAM_ADSR3_RESTART, apply_param_adsr3_restart},
     {PARAM_ADSR2_RESTART, apply_param_adsr2_restart},
     {PARAM_ADSR1_RESTART, apply_param_adsr1_restart},

     // --- Envelope Mode ---
     {PARAM_ADSR3_MODE, apply_param_adsr3_mode},
     {PARAM_ADSR2_MODE, apply_param_adsr2_mode},
     {PARAM_ADSR1_MODE, apply_param_adsr1_mode},

     // --- Modulation Matrix (Slots 0..7) ---
    {PARAM_MOD_SLOT0_SOURCE, apply_param_mod_slot0_source},
    {PARAM_MOD_SLOT0_DEST,   apply_param_mod_slot0_dest},
    {PARAM_MOD_SLOT0_DEPTH,  apply_param_mod_slot0_depth},
    {PARAM_MOD_SLOT1_SOURCE, apply_param_mod_slot1_source},
    {PARAM_MOD_SLOT1_DEST,   apply_param_mod_slot1_dest},
    {PARAM_MOD_SLOT1_DEPTH,  apply_param_mod_slot1_depth},
    {PARAM_MOD_SLOT2_SOURCE, apply_param_mod_slot2_source},
    {PARAM_MOD_SLOT2_DEST,   apply_param_mod_slot2_dest},
    {PARAM_MOD_SLOT2_DEPTH,  apply_param_mod_slot2_depth},
    {PARAM_MOD_SLOT3_SOURCE, apply_param_mod_slot3_source},
    {PARAM_MOD_SLOT3_DEST,   apply_param_mod_slot3_dest},
    {PARAM_MOD_SLOT3_DEPTH,  apply_param_mod_slot3_depth},
    {PARAM_MOD_SLOT4_SOURCE, apply_param_mod_slot4_source},
    {PARAM_MOD_SLOT4_DEST,   apply_param_mod_slot4_dest},
    {PARAM_MOD_SLOT4_DEPTH,  apply_param_mod_slot4_depth},
    {PARAM_MOD_SLOT5_SOURCE, apply_param_mod_slot5_source},
    {PARAM_MOD_SLOT5_DEST,   apply_param_mod_slot5_dest},
    {PARAM_MOD_SLOT5_DEPTH,  apply_param_mod_slot5_depth},
    {PARAM_MOD_SLOT6_SOURCE, apply_param_mod_slot6_source},
    {PARAM_MOD_SLOT6_DEST,   apply_param_mod_slot6_dest},
    {PARAM_MOD_SLOT6_DEPTH,  apply_param_mod_slot6_depth},
    {PARAM_MOD_SLOT7_SOURCE, apply_param_mod_slot7_source},
    {PARAM_MOD_SLOT7_DEST,   apply_param_mod_slot7_dest},
    {PARAM_MOD_SLOT7_DEPTH,  apply_param_mod_slot7_depth},
 
     // --- Calibration Controls ---
     {PARAM_CALIBRATION_FLAG, apply_param_calibration_flag},
     {PARAM_MANUAL_CALIBRATION_FLAG, apply_param_manual_calibration_flag},
     {PARAM_MANUAL_CALIBRATION_STAGE, apply_param_manual_calibration_stage},
     {PARAM_MANUAL_CALIBRATION_OFFSET, apply_param_manual_calibration_offset},
     {PARAM_MANUAL_CALIBRATION_STEP, apply_param_manual_calibration_step},
     {PARAM_AMP_COMP_440, apply_param_amp_comp_440},
     {PARAM_CAL_PW_CENTER, apply_param_cal_pw_center},
     {PARAM_AMP_COMP_DUTY_OFFSET, apply_param_amp_comp_duty_offset},
     {PARAM_MANUAL_CALIBRATION_STORE, apply_param_manual_calibration_store},
 
     // --- Presets & Diagnostics ---
     {PARAM_PRESET_SAVE, apply_param_preset_save},
     {PARAM_PRESET_LOAD, apply_param_preset_load},
     {PARAM_PRESET_DUMP, apply_param_preset_dump},
     {PARAM_CAL_DUMP, apply_param_cal_dump},
     {PARAM_UI_PRESET_SCROLL, apply_param_ui_preset_scroll},
     {PARAM_DEBUG_COMMAND, apply_param_debug_command},
 };
 
 // =============================================================================
 // 9. CALIBRATION PARAMETER GATING & ROUTER ENTRY POINT
 // =============================================================================
 
 /** @brief Checks if a parameter ID belongs strictly to the calibration UI suite. */
 static inline bool is_calibration_parameter(uint8_t id) {
   switch (id) {
     case PARAM_CALIBRATION_FLAG:
     case PARAM_MANUAL_CALIBRATION_FLAG:
     case PARAM_MANUAL_CALIBRATION_STAGE:
     case PARAM_MANUAL_CALIBRATION_OFFSET:
     case PARAM_MANUAL_CALIBRATION_STORE:
     case PARAM_MANUAL_CALIBRATION_STEP:
     case PARAM_AMP_COMP_440:
     case PARAM_AMP_COMP_DUTY_OFFSET:
     case PARAM_CAL_PW_CENTER:
     case PARAM_DEBUG_COMMAND:
       return true;
     default:
       return false;
   }
 }
 
 /** @brief O(1) jump table array instantiated for 256 parameter IDs. */
 static void (*dcoParamJump[PARAM_ROUTER_JUMP_SIZE])(int16_t) = {nullptr};
 
 /**
  * @brief Builds the O(1) jump table from the static descriptor array at system startup.
  */
 void init_param_router() {
   param_router_build_jump(dcoParamJump, paramTable,
                           sizeof(paramTable) / sizeof(paramTable[0]));
 }
 
 /**
  * @brief Main dispatch entry point for parameter updates arriving from UART or USB CDC.
  * 
  * @param id    16-bit parameter ID.
  * @param value Parameter payload value.
  */
 void SRAM_HOT(update_parameters)(uint8_t id, int16_t value) {
   // Gating during calibration to prevent knob noise leaks
   if (__builtin_expect(calibrationFlag || calibrationVerifyRequested, 0)) {
     if (!manualCalibrationFlag) {
       if (id == PARAM_CALIBRATION_FLAG || id == PARAM_DEBUG_COMMAND) {
         param_router_apply(dcoParamJump, id, value);
       }
       return;
     }
     if (!is_calibration_parameter(id)) {
       return;
     }
   }
 
   // Normal parameter execution
   param_router_apply(dcoParamJump, id, value);
   preset_shadow_capture(id, value);
 }


  // =============================================================================
 // PRESET SHADOW APPLIER (Recall State from RAM Buffer)
 // =============================================================================
 
 /**
  * @brief Applies an entire preset from the RAM shadow array to the synth engine.
  */
  void SRAM_HOT(dco_apply_preset_shadow)() {
    // =========================================================================
    // 1. Oscillator & Voice Configuration
    // =========================================================================
    
    pulseWaveOn       = (presetParamShadow[PARAM_OSC1_PULSE_ENABLE] != 0);
    octave_shift      = (int8_t)presetParamShadow[PARAM_OSC1_INTERVAL];
    OSC2_interval     = (int8_t)presetParamShadow[PARAM_OSC2_INTERVAL];
    OSC3_interval     = (int8_t)presetParamShadow[PARAM_OSC3_INTERVAL];
    OSC2_detune       = (uint16_t)presetParamShadow[PARAM_OSC2_DETUNE_VAL];
    unisonDetune      = presetParamShadow[PARAM_UNISON_DETUNE];
  
    apply_param_portamento_time((uint16_t)presetParamShadow[PARAM_PORTAMENTO_TIME]);
    portamento_mode   = (uint8_t)presetParamShadow[PARAM_PORTAMENTO_MODE];
  
    voiceMode         = (uint8_t)presetParamShadow[PARAM_VOICE_MODE];
    setVoiceMode(voiceMode);
    voiceAlloc.setMode((uint8_t)presetParamShadow[PARAM_VOICE_ALLOC_MODE]);
    
    softSyncChunks    = (uint8_t)presetParamShadow[PARAM_SOFT_SYNC];
    syncMode          = (uint8_t)presetParamShadow[PARAM_SYNC_MODE];
    setSyncMode();
    apply_param_phase_align((int16_t)presetParamShadow[PARAM_OSC_PHASE_SYNC]);
    subOscDivide      = (uint8_t)presetParamShadow[PARAM_SUBOSC_DIVIDE];
    crossmod_depth    = (int16_t)presetParamShadow[PARAM_CROSSMOD_DEPTH];
    crossmod_mode      = (uint8_t)presetParamShadow[PARAM_CROSSMOD_MODE];
    update_crossmod_prebake(crossmod_depth);
  
    // =========================================================================
    // 2. LFO Speeds, Waveforms & Frequencies
    // =========================================================================
    LFO1Waveform      = (uint8_t)presetParamShadow[PARAM_LFO1_WAVEFORM];
    LFO1_class.setWaveForm(LFO1Waveform);
  
    LFO2Waveform      = (uint8_t)presetParamShadow[PARAM_LFO2_WAVEFORM];
    LFO2_class.setWaveForm(LFO2Waveform);
  
    const uint32_t now_us = micros();
    LFO1SpeedVal      = (uint16_t)presetParamShadow[PARAM_LFO1_SPEED];
    LFO1Speed         = fast_exp_speed_5000(LFO1SpeedVal);
    LFO1_class.setMode0Freq(LFO1Speed, now_us);
  
    LFO2SpeedVal      = (uint16_t)presetParamShadow[PARAM_LFO2_SPEED];
    LFO2Speed         = fast_exp_speed_5000(LFO2SpeedVal);
    LFO2_class.setMode0Freq(LFO2Speed, now_us);
  
// =========================================================================
    // 3. LFO Pitch Depths (Q24 Math & Float)
    // =========================================================================
    LFO1toDCOVal = (uint16_t)constrain(presetParamShadow[PARAM_LFO1_TO_DCO], 0, 511);
#if defined(USE_FLOAT_VOICE_TASK)
    LFO1toDCO_f           = lfo_pitch_depth_f(fast_lfo_depth_norm<511>(LFO1toDCOVal), LFO_4_OCTAVES);
    LFO1toOSC1_f          = lfo_pitch_depth_f(fast_lfo_depth_norm<255>((uint16_t)constrain(presetParamShadow[PARAM_LFO1_TO_OSC1], 0, 255)), LFO_4_OCTAVES);
    LFO1toOSC2_f          = lfo_pitch_depth_f(fast_lfo_depth_norm<255>((uint16_t)constrain(presetParamShadow[PARAM_LFO1_TO_OSC2], 0, 255)), LFO_4_OCTAVES);
    LFO1toOSC3_f          = lfo_pitch_depth_f(fast_lfo_depth_norm<255>((uint16_t)constrain(presetParamShadow[PARAM_LFO1_TO_OSC3], 0, 255)), LFO_4_OCTAVES);
    LFO2toOSC2_f          = lfo_pitch_depth_f(fast_lfo_depth_norm<255>((uint16_t)constrain(presetParamShadow[PARAM_LFO2_TO_OSC2], 0, 255)), LFO_VIBRATO_2_SEMITONES);
    LFO2toOSC3_f          = lfo_pitch_depth_f(fast_lfo_depth_norm<255>((uint16_t)constrain(presetParamShadow[PARAM_LFO2_TO_OSC3], 0, 255)), LFO_VIBRATO_2_SEMITONES);
    LFO2toOSC2_coarse_f   = lfo_pitch_depth_f(fast_lfo_depth_norm<511>((uint16_t)constrain(presetParamShadow[PARAM_LFO2_TO_OSC2_COARSE], 0, 511)), LFO_4_OCTAVES);
    LFO2toOSC3_coarse_f   = lfo_pitch_depth_f(fast_lfo_depth_norm<511>((uint16_t)constrain(presetParamShadow[PARAM_LFO2_TO_OSC3_COARSE], 0, 511)), LFO_4_OCTAVES);
#else
    LFO1toDCO_q24         = lfo_pitch_depth_q24(fast_lfo_depth_norm<511>(LFO1toDCOVal), LFO_COARSE_2_OCTAVES_Q24);
    LFO1toOSC1_q24        = lfo_pitch_depth_q24(fast_lfo_depth_norm<255>((uint16_t)constrain(presetParamShadow[PARAM_LFO1_TO_OSC1], 0, 255)), LFO_COARSE_2_OCTAVES_Q24);
    LFO1toOSC2_q24        = lfo_pitch_depth_q24(fast_lfo_depth_norm<255>((uint16_t)constrain(presetParamShadow[PARAM_LFO1_TO_OSC2], 0, 255)), LFO_COARSE_2_OCTAVES_Q24);
    LFO1toOSC3_q24        = lfo_pitch_depth_q24(fast_lfo_depth_norm<255>((uint16_t)constrain(presetParamShadow[PARAM_LFO1_TO_OSC3], 0, 255)), LFO_COARSE_2_OCTAVES_Q24);
    LFO2toOSC2_q24        = lfo_pitch_depth_q24(fast_lfo_depth_norm<255>((uint16_t)constrain(presetParamShadow[PARAM_LFO2_TO_OSC2], 0, 255)), LFO_VIBRATO_2_SEMITONES_Q24);
    LFO2toOSC3_q24        = lfo_pitch_depth_q24(fast_lfo_depth_norm<255>((uint16_t)constrain(presetParamShadow[PARAM_LFO2_TO_OSC3], 0, 255)), LFO_VIBRATO_2_SEMITONES_Q24);
    LFO2toOSC2_coarse_q24 = lfo_pitch_depth_q24(fast_lfo_depth_norm<511>((uint16_t)constrain(presetParamShadow[PARAM_LFO2_TO_OSC2_COARSE], 0, 511)), LFO_COARSE_2_OCTAVES_Q24);
    LFO2toOSC3_coarse_q24 = lfo_pitch_depth_q24(fast_lfo_depth_norm<511>((uint16_t)constrain(presetParamShadow[PARAM_LFO2_TO_OSC3_COARSE], 0, 511)), LFO_COARSE_2_OCTAVES_Q24);
#endif
  
    // ADSR3 Detune
    ADSR3toDETUNE1           = presetParamShadow[PARAM_ADSR3_TO_DETUNE1];
  #if defined(USE_FLOAT_VOICE_TASK)
    ADSR3toDETUNE1_scale_f   = (float)ADSR3toDETUNE1 * (2.0f / (511.0f * 32768.0f));
  #else
    ADSR3toDETUNE1_scale_q24 = (int32_t)ADSR3toDETUNE1 * 65664;
  #endif
  
    LFO2toPW          = (uint16_t)(((uint32_t)presetParamShadow[PARAM_LFO2_TO_PW] * DIV_COUNTER_PW) / 1024);

    // =========================================================================
    // 4. Analog Drift, Pulse Width & Character
    // =========================================================================
    apply_param_analog_drift_amount(presetParamShadow[PARAM_ANALOG_DRIFT_AMOUNT]);
    apply_param_analog_drift_speed(presetParamShadow[PARAM_ANALOG_DRIFT_SPEED]);
    apply_param_analog_drift_spread(presetParamShadow[PARAM_ANALOG_DRIFT_SPREAD]);
    init_DRIFT_LFOs();
  
    PW[0] = presetParamShadow[PARAM_PW_VALUE] >> 1;
  
   // =========================================================================
   // 5. Envelope Modulations, Curves & Filter Trigger Mode
   // =========================================================================
   ADSR3ToOscSelect         = (int8_t)presetParamShadow[PARAM_ADSR3_TO_OSC_SELECT];
   ADSR3toPWM = (int16_t)(((int32_t)(presetParamShadow[PARAM_ADSR3_TO_PWM] - 512) * (int32_t)DIV_COUNTER_PW) / 1024);
   ADSR3toPWM_scale = ADSR3toPWM;
   ADSR3toDETUNE1           = presetParamShadow[PARAM_ADSR3_TO_DETUNE1];
   ADSR3toDETUNE1_scale_q24 = (int32_t)presetParamShadow[PARAM_ADSR3_TO_DETUNE1] * 65664;
   
   ADSR3Mode                = (uint8_t)presetParamShadow[PARAM_ADSR3_MODE];
   ADSR2Mode                = (uint8_t)presetParamShadow[PARAM_ADSR2_MODE];
   ADSR1Mode                = (uint8_t)presetParamShadow[PARAM_ADSR1_MODE];
 
   ADSR3Restart = (uint8_t)presetParamShadow[PARAM_ADSR3_RESTART];
   ADSR2Restart = (uint8_t)presetParamShadow[PARAM_ADSR2_RESTART];
   ADSR1Restart = (uint8_t)presetParamShadow[PARAM_ADSR1_RESTART];
 
   // 1. Apply VCA Curves (ADSR1)
   ADSR1AttackCurveVal  = (uint8_t)presetParamShadow[PARAM_ADSR1_ATTACK_CURVE];
   ADSR1DecayCurveVal   = (uint8_t)presetParamShadow[PARAM_ADSR1_DECAY_CURVE];
   ADSR1ReleaseCurveVal = (uint8_t)presetParamShadow[PARAM_ADSR1_RELEASE_CURVE];
   ADSR_VCA_change_attack_curve(ADSR1AttackCurveVal);
   ADSR_VCA_change_decay_curve(ADSR1DecayCurveVal);
   ADSR_VCA_change_release_curve(ADSR1ReleaseCurveVal);
 
   // 2. Apply VCF Curves (ADSR2 -> VCF1 & VCF2)
   ADSR2AttackCurveVal  = (uint8_t)presetParamShadow[PARAM_ADSR2_ATTACK_CURVE];
   ADSR2DecayCurveVal   = (uint8_t)presetParamShadow[PARAM_ADSR2_DECAY_CURVE];
   ADSR2ReleaseCurveVal = (uint8_t)presetParamShadow[PARAM_ADSR2_RELEASE_CURVE];
   ADSR_VCF_change_attack_curve(ADSR2AttackCurveVal);
   ADSR_VCF_change_decay_curve(ADSR2DecayCurveVal);
   ADSR_VCF_change_release_curve(ADSR2ReleaseCurveVal);
   ADSR_VCF2_change_attack_curve(ADSR2AttackCurveVal);
   ADSR_VCF2_change_decay_curve(ADSR2DecayCurveVal);
   ADSR_VCF2_change_release_curve(ADSR2ReleaseCurveVal);
 
   // 3. Apply DCO Curves (ADSR3)
   ADSR3AttackCurveVal  = (uint8_t)presetParamShadow[PARAM_ADSR3_ATTACK_CURVE];
   ADSR3DecayCurveVal   = (uint8_t)presetParamShadow[PARAM_ADSR3_DECAY_CURVE];
   ADSR3ReleaseCurveVal = (uint8_t)presetParamShadow[PARAM_ADSR3_RELEASE_CURVE];
   ADSR3_change_attack_curve(ADSR3AttackCurveVal);
   ADSR3_change_decay_curve(ADSR3DecayCurveVal);
   ADSR3_change_release_curve(ADSR3ReleaseCurveVal);
 
   // 4. Apply Shared Filter Trigger Mode (Legato / Multi / Direct)
   set_vcf_trigger_mode((uint8_t)presetParamShadow[PARAM_VCF_TRIGGER_MODE]);
 
  }