#ifndef USE_FLOAT_VOICE_TASK
// Fixed-point realtime voice engine (portamento, modifiers, clkdiv, amp,
// PIO/PWM/PW). Selected by voice_task_main() when USE_FLOAT_VOICE_TASK is not
// defined.
void SRAM_HOT(voice_task_fixed_point)() {
  static uint32_t last_portamento_time = 0;
  static uint8_t last_portamento_mode = PORTA_MODE_TIME;
  static uint8_t lastNote1[NUM_VOICES_TOTAL] = {};
  static uint8_t lastNote2[NUM_VOICES_TOTAL] = {};
  uint32_t portaTime = portamento_time;
  uint8_t portaMode = portamento_mode;
  bool portaTimeChanged = (portaTime != last_portamento_time);
  bool portaModeChanged = (portaMode != last_portamento_mode);

  int32_t calcPitchbend_q24;

  BENCH_BEGIN(vt_pitchbend);
  int32_t bend_normalized_q24 = ((int32_t)midi_pitch_bend << 11) - (1 << 24);
  calcPitchbend_q24 =
      (int32_t)(((int64_t)bend_normalized_q24 * pitchBendMultiplier_q24) >> 24);
  BENCH_END(vt_pitchbend);

  last_midi_pitch_bend = midi_pitch_bend;

  for (int i = 0; i < NUM_VOICES; i++) {

    if (note_on_flag[i] == 1) {
      note_on_flag_flag[i] = true;
      note_on_flag[i] = 0;
      __dmb(); // Memory barrier: Ensure flag is cleared before reading note indices
    }

#if DCO_DEBUG_REPORT
    float dbg_freq_base_Hz = 0.0f;
    float dbg_freq_after_mod_Hz = 0.0f;
#endif

    // =========================================================================
    // INSTANT ZERO-MATH FETCH (Replaces runtime midi_offset_to_table_index)
    // =========================================================================
    // Fetch the pre-baked pitch table indices directly from SRAM (1 clock cycle).
    // All octave shifting, interval math, and folding were completed at note_on.
    const uint8_t note1 = VOICE_NOTE_OSC1[i];
    const uint8_t note2 = VOICE_NOTE_OSC2[i];

    // Detect if the target pitch has changed for glide / portamento re-targeting
    const bool pitchTargetChanged =
        note1 != lastNote1[i] || note2 != lastNote2[i];
    lastNote1[i] = note1;
    lastNote2[i] = note2;

    int64_t freq_q24_A;
    int64_t freq_q24_B;
    const uint8_t DCO_A = (uint8_t)(i * 2);
    const uint8_t DCO_B = (uint8_t)(i * 2 + 1);

    // OSC2 Fine Detune
    BENCH_BEGIN(vt_osc_detune);
    static constexpr int32_t DETUNE_SCALE_Q24 = (int32_t)(0.0002f * (float)(1 << 24) + 0.5f);
    int32_t detune_steps = ((int32_t)OSC2_detune - 256);
    int32_t detune_q24 = (1 << 24) + (detune_steps * DETUNE_SCALE_Q24);
    BENCH_END(vt_osc_detune);


    BENCH_BEGIN(vt_portamento);
    if (portaTime > 0) {
      uint32_t now_us = micros();
      portamentoTimer[i] = now_us - portamentoStartMicros[i];

      if (note_on_flag_flag[i]) {
        portamentoStartMicros[i] = now_us;
        portamentoTimer[i] = 0;

        int32_t targetNoteA_q16 = ((int32_t)note1) << 16;
        int32_t targetNoteB_q16 = ((int32_t)note2) << 16;
        porta_setup_glide_q16(
            DCO_A, porta_resolve_start_note_q16(DCO_A, targetNoteA_q16),
            targetNoteA_q16, portaMode);
        porta_setup_glide_q16(
            DCO_B, porta_resolve_start_note_q16(DCO_B, targetNoteB_q16),
            targetNoteB_q16, portaMode);
      }

      const bool portaDoRetime =
          (portaTimeChanged || portaModeChanged || pitchTargetChanged) &&
          !note_on_flag_flag[i];

      int64_t curA;
      int64_t curB;

      if (portaDoRetime) {
        portamentoStartMicros[i] = now_us;
        portamentoTimer[i] = 0;
      
        // DIRECT note position (no table lookups or binary searches!)
        porta_setup_glide_q16(DCO_A, porta_note_cur_q16[DCO_A], (float)note1, portaMode);
        porta_setup_glide_q16(DCO_B, porta_note_cur_q16[DCO_B], (float)note2, portaMode);
        curA = portamento_cur_freq_q24[DCO_A];
        curB = portamento_cur_freq_q24[DCO_B];
      } else if (porta_note_cur_q16[DCO_A] == porta_note_stop_q16[DCO_A] &&
                 porta_note_cur_q16[DCO_B] == porta_note_stop_q16[DCO_B]) {
        curA = portamento_stop_q24[DCO_A];
        curB = portamento_stop_q24[DCO_B];
      } else {
        int32_t elapsed_u = (int32_t)((uint32_t)portamentoTimer[i] >> 8);

        // 100% 32-bit native MULS + ADDS
        int32_t curNoteA_q16 = porta_note_start_q16[DCO_A] + (porta_note_step_q16[DCO_A] * elapsed_u);
        int32_t curNoteB_q16 = porta_note_start_q16[DCO_B] + (porta_note_step_q16[DCO_B] * elapsed_u);

        int32_t stopA  = porta_note_stop_q16[DCO_A];
        int32_t stopB  = porta_note_stop_q16[DCO_B];
        int32_t startA = porta_note_start_q16[DCO_A];
        int32_t startB = porta_note_start_q16[DCO_B];

        if ((stopA >= startA && curNoteA_q16 >= stopA) ||
            (stopA < startA && curNoteA_q16 <= stopA)) {
          curNoteA_q16 = stopA;
        }
        if ((stopB >= startB && curNoteB_q16 >= stopB) ||
            (stopB < startB && curNoteB_q16 <= stopB)) {
          curNoteB_q16 = stopB;
        }

        porta_note_cur_q16[DCO_A] = curNoteA_q16;
        porta_note_cur_q16[DCO_B] = curNoteB_q16;

        curA = (curNoteA_q16 == stopA) ? portamento_stop_q24[DCO_A] : noteQ16_to_freqQ24(curNoteA_q16);
        curB = (curNoteB_q16 == stopB) ? portamento_stop_q24[DCO_B] : noteQ16_to_freqQ24(curNoteB_q16);
      }

      portamento_cur_freq_q24[DCO_A] = curA;
      portamento_cur_freq_q24[DCO_B] = curB;
    } else {
      // Instant lookup when portamento is off (ZERO math)
      portamento_cur_freq_q24[DCO_A] = sNotePitches_q24[note1];
      portamento_cur_freq_q24[DCO_B] = sNotePitches_q24[note2];
      portamento_stop_q24[DCO_A] = portamento_cur_freq_q24[DCO_A];
      portamento_stop_q24[DCO_B] = portamento_cur_freq_q24[DCO_B];
      porta_note_cur_q16[DCO_A] = ((int32_t)note1) << 16;
      porta_note_cur_q16[DCO_B] = ((int32_t)note2) << 16;
      porta_note_stop_q16[DCO_A] = porta_note_cur_q16[DCO_A];
      porta_note_stop_q16[DCO_B] = porta_note_cur_q16[DCO_B];
      porta_note_valid[DCO_A] = true;
      porta_note_valid[DCO_B] = true;
    }
    BENCH_END(vt_portamento);

    BENCH_BEGIN(vt_adsr_mod);
    int32_t ADSRModifier_q24 = 0;
    if (ADSR3toDETUNE1_scale_q24 != 0) {
      ADSRModifier_q24 = applyDepthQ24(ADSR3Level_q15[i], ADSR3toDETUNE1_scale_q24);
    }
    // ADSR3→pitch: 0=A, 1=B, 2=A+B (legacy), 3/4 ignored or map 4→A+B
    int32_t ADSRModifierOSC1_q24 =
        (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 ||
         ADSR3ToOscSelect == 4)
            ? ADSRModifier_q24
            : 0;
    int32_t ADSRModifierOSC2_q24 =
        (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 ||
         ADSR3ToOscSelect == 4)
            ? ADSRModifier_q24
            : 0;
    BENCH_END(vt_adsr_mod);

    BENCH_BEGIN(vt_unison_mod);
    static constexpr int32_t UNISON_SCALE_Q24 =
        (int32_t)(0.0001f * (float)(1 << 24) + 0.5f);
    const int32_t unisonBase = (int32_t)unisonDetune * UNISON_SCALE_Q24;
    int32_t voiceMag = (i >> 1) + 1;
    int32_t voiceSign = ((i & 0x01) == 0) ? 1 : -1;
    int32_t unisonMODIFIER_q24 = unisonBase * (voiceSign * voiceMag);
    BENCH_END(vt_unison_mod);

    BENCH_BEGIN(vt_drift_mod);
    const int32_t driftScale_q24 = drift_pitch_scale_q24;
    const int16_t driftA = LFO_DRIFT_LEVEL[DCO_A];
    const int16_t driftB = LFO_DRIFT_LEVEL[DCO_B];
    int32_t DETUNE_DRIFT_OSC1_q24 =
        (driftScale_q24 != 0) ? applyDepthQ24(driftA, driftScale_q24) : 0;
    int32_t DETUNE_DRIFT_OSC2_q24 =
        (driftScale_q24 != 0) ? applyDepthQ24(driftB, driftScale_q24) : 0;
    BENCH_END(vt_drift_mod);

    int32_t modifiersBase_q24;
    int32_t freqModifiers_q24;
    int32_t freq2Modifiers_q24;
    {
      const int32_t local_lfo1_osc1 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC1];
      const int32_t local_lfo1_osc2 = lfo1_pitch_mod_q24[LFO1_PITCH_OSC2];
      const int32_t local_lfo2_osc2 = lfo2_pitch_mod_q24[LFO2_PITCH_OSC2];
      BENCH_BEGIN(vt_modifiers);
      modifiersBase_q24 = calcPitchbend_q24 + Q24_ONE_EPS +
                          matrix_pitch_mod_q24[i] + unisonMODIFIER_q24;
      if (char_pitch_scale_q15) {
        modifiersBase_q24 += character_pitch_delta_q24();
      }
      freqModifiers_q24 = ADSRModifierOSC1_q24 + DETUNE_DRIFT_OSC1_q24 + modifiersBase_q24 + local_lfo1_osc1 + matrix_osc1_pitch_mod_q24[i];
      freq2Modifiers_q24 = ADSRModifierOSC2_q24 + DETUNE_DRIFT_OSC2_q24 + modifiersBase_q24 + local_lfo1_osc2 + local_lfo2_osc2 + matrix_osc2_pitch_mod_q24[i];
      BENCH_END(vt_modifiers);
    }

    BENCH_BEGIN(vt_freq_scale_x);
    int32_t xScaled1_Q16 = modifiers_q24_to_xQ16(freqModifiers_q24);
    int32_t xScaled2_Q16 = modifiers_q24_to_xQ16(freq2Modifiers_q24);
    BENCH_END(vt_freq_scale_x);

    BENCH_BEGIN(vt_ratio_interp);
    int32_t ratio1_Q16 = interpolate_live_ratio_q16(xScaled1_Q16, DCO_A);
    int32_t ratio2_Q16 = interpolate_live_ratio_q16(xScaled2_Q16, DCO_B);
    BENCH_END(vt_ratio_interp);

    BENCH_BEGIN(vt_freq_scale_post);
    freq_q24_A = (portamento_cur_freq_q24[DCO_A] * (int64_t)ratio1_Q16) >> 16;
    int32_t detune_Q16 = (int32_t)((((int64_t)detune_q24) + 128) >> 8);
    int32_t combined_Q16 =
        (int32_t)((((int64_t)ratio2_Q16 * (int64_t)detune_Q16) + (1LL << 15)) >>
                  16);
    freq_q24_B = (portamento_cur_freq_q24[DCO_B] * (int64_t)combined_Q16) >> 16;

        // Fast 1-cycle Q24 -> Q16 conversion with rounding (+128 >> 8)
    // Both fA_q16 and fB_q16 are now stored in fast 32-bit registers
    const uint32_t fA_q16 = (freq_q24_A <= 0) ? 0 : (uint32_t)((freq_q24_A + 128) >> 8);
    const uint32_t fB_q16 = (freq_q24_B <= 0) ? 0 : (uint32_t)((freq_q24_B + 128) >> 8);

#if DCO_DEBUG_REPORT
    dbg_freq_after_mod_Hz = (float)freq_q24_A / (float)(1 << 24);
#endif
    BENCH_END(vt_freq_scale_post);

   
    BENCH_BEGIN(vt_clk_div);
    PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
    PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
    uint8_t smAN = VOICE_TO_SM[DCO_A];
    uint8_t smBN = VOICE_TO_SM[DCO_B];

    uint32_t total_cycles1, total_cycles2;
    const uint32_t sys_hz = sysClock_Hz;
   
    total_cycles1 = clkdiv_q16_total_cycles(sys_hz, fA_q16);
    total_cycles2 = clkdiv_q16_total_cycles(sys_hz, fB_q16);
    
    // Fetch parameters using normal function calls (no angle brackets)
    uint32_t wA, kA, wB, kB;
    get_osc_params(DCO_A, wA, kA);
    get_osc_params(DCO_B, wB, kB);

    uint32_t clk_div1 = pio_clk_div_for_y(total_cycles1, osc_last_y[DCO_A], wA, kA);
    uint32_t clk_div2 = pio_clk_div_for_y(total_cycles2, osc_last_y[DCO_B], wB, kB);

    BENCH_END(vt_clk_div);

    uint32_t phaseHoldX = 0;
    PioPeriod retrig_p1{};
    PioPeriod retrig_p2{};
    if (note_on_flag_flag[i] && oscPhaseSync > 1) {
      BENCH_BEGIN(vt_phase_align);
      phaseHoldX = osc_phase_hold_x(total_cycles2, phaseAlignOSC2);
      BENCH_END(vt_phase_align);
    }
    if (note_on_flag_flag[i] && oscPhaseSync >= 1 &&
        note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
      BENCH_BEGIN(vt_retrig_split);
      retrig_p1 = pio_period_split(total_cycles1, wA, kA);
      retrig_p2 = pio_period_split(total_cycles2, wB, kB);
      BENCH_END(vt_retrig_split);
    }

    BENCH_BEGIN(vt_chan_level);

    // 2. Direct calls (Compiles straight to inlined register assembly)
    const uint16_t chanLevel  = get_chan_level_q16_fast(fA_q16, DCO_A);
    const uint16_t chanLevel2 = get_chan_level_q16_fast(fB_q16, DCO_B);
    BENCH_END(vt_chan_level);

    if (note_on_flag_flag[i]) {
      BENCH_BEGIN(vt_note_retrig);
#if DCO_DEBUG_REPORT
      uint32_t actual_total_osr_val = clk_div1 * wA;
      uint32_t actual_total_period =
          osc_last_y[DCO_A] + actual_total_osr_val + kA;
      float expected_freq = (double)sysClock_Hz / (double)actual_total_period;
      PioPeriod p1dbg = pio_period_split(total_cycles1, wA, kA);
      Serial.println("----------------[ DCO DEBUG REPORT ]----------------");
      Serial.printf("Target Freq In:   %.2f Hz\n",
                    (float)freq_q24_A / (float)(1 << 24));
      Serial.printf("Total Cycles Calc:  %lu (Target for the whole period)\n",
                    total_cycles1);
      Serial.printf("Reset pulse (Y):    %lu cycles (incl. period remainder)\n",
                    p1dbg.y);
      Serial.printf("Period Overhead:    %lu cycles (program constant)\n", kA);
      Serial.printf("Total OSR Delay:    %lu cycles (Remaining for loops)\n",
                    p1dbg.clk_div * wA);
      Serial.printf("clk_div (Exact):    %lu (Value sent to PIO)\n",
                    p1dbg.clk_div);
      Serial.println("---");
      Serial.printf(
          "Actual Period Gen:  %lu cycles (Y + (clk_div*%u) + overhead)\n",
          actual_total_period, (unsigned)wA);
      Serial.printf("==> Expected Freq Out: %.2f Hz\n", expected_freq);
      Serial.println("---");
      Serial.println("OSC1 Frequency Stages:");
      Serial.printf("  Base after portamento:     %.4f Hz\n", dbg_freq_base_Hz);
      Serial.printf("  After modifiers (Q24):     %.4f Hz\n",
                    dbg_freq_after_mod_Hz);
      Serial.printf("  Quantized by PIO (clkdiv): %.4f Hz\n", expected_freq);
      Serial.println("----------------------------------------------------\n");
#endif

      if (oscPhaseSync >= 1) {
        BENCH_BEGIN(vt_retrig_sm_apply);
        if (note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
          uint32_t maskAB = (1u << smAN) | (1u << smBN);
          pio_set_sm_mask_enabled(pioN_A, maskAB, false);

          osc_load_periods_stopped_noclear(DCO_A, retrig_p1.y,
                                           retrig_p1.clk_div, DCO_B,
                                           retrig_p2.y, retrig_p2.clk_div);

          pio_sm_exec(pioN_A, smAN, pio_encode_jmp(osc_restart_target(DCO_A)));

          if (phaseHoldX != 0) {
            osc_phase_align_hold_stopped(DCO_B, phaseHoldX);
          } else {
            pio_sm_exec(pioN_B, smBN,
                        pio_encode_jmp(osc_restart_target(DCO_B)));
          }

          pio_enable_sm_mask_in_sync(pioN_A, maskAB);
        } else {
          pio_sm_exec(pioN_A, smAN, pio_encode_jmp(osc_restart_target(DCO_A)));
          pio_sm_exec(pioN_B, smBN, pio_encode_jmp(osc_restart_target(DCO_B)));
        }
        BENCH_END(vt_retrig_sm_apply);
      }

      BENCH_END(vt_note_retrig);
    } else {
      BENCH_BEGIN(vt_pio_write);
      pio_sm_put(pioN_A, smAN, clk_div1);
      pio_sm_put(pioN_B, smBN, clk_div2);
      pio_sm_exec(pioN_A, smAN, pio_encode_pull(false, false));
      pio_sm_exec(pioN_B, smBN, pio_encode_pull(false, false));
      osc_last_clk_div[DCO_A] = clk_div1;
      osc_last_clk_div[DCO_B] = clk_div2;
      BENCH_END(vt_pio_write);
    }
    
      BENCH_BEGIN(vt_range_pwm);
      write_range_pwm(DCO_A, chanLevel);
      write_range_pwm(DCO_B, chanLevel2);
      BENCH_END(vt_range_pwm);

      if (timer99microsFlag2) {
      if (pulseWaveOn) {
        BENCH_FBEGIN(vt_pwm_calc);

        const int16_t local_LFO2Level = LFO2Level;
        const int16_t local_LFO2toPW = LFO2toPW;
        const int16_t local_ADSR3toPWM =
            ADSR3toPWM; // Note: ADSR3toPWM is signed (-512 .. +512)

        // 1. Calculate Q15 modulation deltas
        const int32_t adsr3_delta =
            ((int32_t)ADSR3Level_q15[i] * (int32_t)local_ADSR3toPWM) >> 15;
        const int32_t lfo2_delta =
            ((int32_t)local_LFO2Level * (int32_t)local_LFO2toPW) >> 15;

        // 2. ✅ Clean logical sum: Knob + LFO + Envelope + mod_matrix + Jitter 
        int32_t pw_calc = (int32_t)PW[0] + lfo2_delta + adsr3_delta + matrix_pw_mod[i] + (int32_t)character_pw_delta();
        pw_calc = (pw_calc < 0) ? 0 : ((pw_calc > max_pw) ? max_pw : pw_calc);

        PW_PWM[i] = (uint16_t)pw_calc;
        BENCH_FEND(vt_pwm_calc);
        BENCH_BEGIN(vt_pwm_write);
        // 4. Pass pitch (Q24) for 3-point key tracking
        voice_write_pw(i, get_PW_level_interpolated(PW_PWM[i], DCO_A, freq_q24_A));
        BENCH_END(vt_pwm_write);
      } else {
        BENCH_BEGIN(vt_pwm_write);
        voice_write_pw(i, 0);
        BENCH_END(vt_pwm_write);
      }
    }
  }

  for (int k = 0; k < NUM_VOICES_TOTAL; k++) {  // <-- Change to NUM_VOICES_TOTAL
    note_on_flag_flag[k] = false;
  }

  last_portamento_time = portaTime;
  last_portamento_mode = portaMode;
}

#endif // !USE_FLOAT_VOICE_TASK


#ifdef USE_VOICE_TASK_Q24
// Float realtime voice engine (Optimized for Cortex-M33 / RP2350)
void SRAM_HOT(voice_task_Q24)() {
  static uint32_t last_portamento_time = 0;
  static uint8_t last_portamento_mode = PORTA_MODE_SLEW;
  static uint8_t lastNote1[NUM_VOICES_TOTAL] = {};
  static uint8_t lastNote2[NUM_VOICES_TOTAL] = {};
  uint32_t portaTime = portamento_time;
  uint8_t portaMode = portamento_mode;
  bool portaTimeChanged = (portaTime != last_portamento_time);
  bool portaModeChanged = (portaMode != last_portamento_mode);

  // --------------------------------------------------------------------------
  // PRE-LOOP SETUP: Keep Modifiers in Native Q24 Integer Math
  // --------------------------------------------------------------------------
  BENCH_BEGIN(vt_pitchbend);
  // Pure Q24 integer pitchbend calc (eliminates float conversions)
  int32_t bend_normalized_q24 = ((int32_t)midi_pitch_bend << 11) - (1 << 24);
  int32_t calcPitchbend_q24 = (int32_t)(((int64_t)bend_normalized_q24 * pitchBendMultiplier_q24) >> 24);
  BENCH_END(vt_pitchbend);

  last_midi_pitch_bend = midi_pitch_bend;

  // OSC Detune (kept as float because it is a direct Hz output scaler)
  const float osc1DetuneSteps = (float)((int32_t)OSC1_detune - 256);
  const float osc1DetuneRatio = 1.0f + 0.0002f * osc1DetuneSteps;

  const float osc2DetuneSteps = (float)((int32_t)OSC2_detune - 256);
  const float osc2DetuneRatio = 1.0f + 0.0002f * osc2DetuneSteps;

  // Unison Base - Native Q24
  static constexpr int32_t UNISON_SCALE_Q24 = (int32_t)(0.0001f * (float)(1 << 24) + 0.5f);
  const int32_t unisonBase_q24 = (int32_t)unisonDetune * UNISON_SCALE_Q24;

  // PWM LFO delta
  const int32_t lfo2_pw_delta = ((int32_t)LFO2Level * (int32_t)LFO2toPW) >> 15;

// --------------------------------------------------------------------------
  // PILLAR II: Apply const volatile __restrict to match global qualifiers
  // --------------------------------------------------------------------------
  const int16_t* __restrict p_adsr3 = ADSR3Level_q15;
  const volatile int16_t* __restrict p_drift = LFO_DRIFT_LEVEL;
  const volatile int32_t* __restrict p_matrix_pitch = matrix_pitch_mod_q24;
  const volatile int32_t* __restrict p_matrix_osc1 = matrix_osc1_pitch_mod_q24;
  const volatile int32_t* __restrict p_matrix_osc2 = matrix_osc2_pitch_mod_q24;
  const volatile uint8_t* __restrict p_note1 = VOICE_NOTE_OSC1;
  const volatile uint8_t* __restrict p_note2 = VOICE_NOTE_OSC2;

  for (int i = 0; i < NUM_VOICES; ++i) {

    if (note_on_flag[i] == 1) {
      note_on_flag_flag[i] = true;
      note_on_flag[i] = 0;
      __dmb(); // Memory barrier
    }

#if DCO_DEBUG_REPORT
    float dbg_freq_base_Hz = 0.0f;
    float dbg_freq_after_mod_Hz = 0.0f;
#endif

    const uint8_t note1 = p_note1[i];
    const uint8_t note2 = p_note2[i];
    const bool pitchTargetChanged = (note1 != lastNote1[i] || note2 != lastNote2[i]);
    lastNote1[i] = note1;
    lastNote2[i] = note2;

    float noteFreq1 = sNotePitches[note1];
    float noteFreq2 = sNotePitches[note2];

    float freqA, freqB;
    const uint8_t DCO_A = (uint8_t)(i << 1);
    const uint8_t DCO_B = (uint8_t)((i << 1) | 1);

    // =========================================================================
    // PORTAMENTO (Kept in Float due to sNotePitches interaction)
    // =========================================================================
    BENCH_BEGIN(vt_portamento);
    // [Portamento logic remains completely unchanged from original]
    if (portaTime > 0) {
      uint32_t now_us = micros();
      portamentoTimer[i] = now_us - portamentoStartMicros[i];

      if (note_on_flag_flag[i]) {
        portamentoStartMicros[i] = now_us;
        portamentoTimer[i] = 0;
        float targetNoteA = (float)note1;
        float targetNoteB = (float)note2;
        porta_setup_glide_f(DCO_A, porta_resolve_start_note_f(DCO_A, targetNoteA), targetNoteA, portaMode);
        porta_setup_glide_f(DCO_B, porta_resolve_start_note_f(DCO_B, targetNoteB), targetNoteB, portaMode);
      }

      const bool portaDoRetime = (portaTimeChanged || portaModeChanged || pitchTargetChanged) && !note_on_flag_flag[i];

      float curA, curB;
      if (portaDoRetime) {
        portamentoStartMicros[i] = now_us;
        portamentoTimer[i] = 0;
        porta_setup_glide_f(DCO_A, porta_note_cur_f[DCO_A], (float)note1, portaMode);
        porta_setup_glide_f(DCO_B, porta_note_cur_f[DCO_B], (float)note2, portaMode);
        curA = porta_freq_cur_f[DCO_A];
        curB = porta_freq_cur_f[DCO_B];
      } else if (porta_note_cur_f[DCO_A] == porta_note_stop_f[DCO_A] &&
                 porta_note_cur_f[DCO_B] == porta_note_stop_f[DCO_B]) {
        curA = porta_freq_stop_f[DCO_A];
        curB = porta_freq_stop_f[DCO_B];
      } else {
        int32_t elapsed = (int32_t)portamentoTimer[i];
        float startNoteA = porta_note_start_f[DCO_A];
        float startNoteB = porta_note_start_f[DCO_B];
        float stopNoteA = porta_note_stop_f[DCO_A];
        float stopNoteB = porta_note_stop_f[DCO_B];

        float dNoteA = stopNoteA - startNoteA;
        float dNoteB = stopNoteB - startNoteB;
        float curNoteA = startNoteA + porta_note_step_f[DCO_A] * (float)elapsed;
        float curNoteB = startNoteB + porta_note_step_f[DCO_B] * (float)elapsed;

        if ((dNoteA >= 0.0f && curNoteA >= stopNoteA) || (dNoteA < 0.0f && curNoteA <= stopNoteA)) curNoteA = stopNoteA;
        if ((dNoteB >= 0.0f && curNoteB >= stopNoteB) || (dNoteB < 0.0f && curNoteB <= stopNoteB)) curNoteB = stopNoteB;

        porta_note_cur_f[DCO_A] = curNoteA;
        porta_note_cur_f[DCO_B] = curNoteB;
        curA = (curNoteA == stopNoteA) ? porta_freq_stop_f[DCO_A] : noteIndex_to_freqFloat(curNoteA);
        curB = (curNoteB == stopNoteB) ? porta_freq_stop_f[DCO_B] : noteIndex_to_freqFloat(curNoteB);
        porta_freq_cur_f[DCO_A] = curA;
        porta_freq_cur_f[DCO_B] = curB;
      }
      freqA = curA;
      freqB = curB;
    } else {
      freqA = noteFreq1;
      freqB = noteFreq2;
      porta_freq_cur_f[DCO_A] = freqA;
      porta_freq_cur_f[DCO_B] = freqB;
      porta_freq_stop_f[DCO_A] = freqA;
      porta_freq_stop_f[DCO_B] = freqB;
      porta_note_cur_f[DCO_A] = (float)note1;
      porta_note_cur_f[DCO_B] = (float)note2;
      porta_note_stop_f[DCO_A] = (float)note1;
      porta_note_stop_f[DCO_B] = (float)note2;
      porta_note_valid[DCO_A] = true;
      porta_note_valid[DCO_B] = true;
    }

#if DCO_DEBUG_REPORT
    dbg_freq_base_Hz = freqA;
#endif
    BENCH_END(vt_portamento);

    // =========================================================================
    // MODULATORS: Native Q24 Integer Chain (Bypasses Float Conversions)
    // =========================================================================
    BENCH_BEGIN(vt_adsr_mod);
    int32_t ADSRModifier_q24 = 0;
    if (ADSR3toDETUNE1_scale_q24 != 0) {
      ADSRModifier_q24 = applyDepthQ24(p_adsr3[i], ADSR3toDETUNE1_scale_q24);
    }
    int32_t ADSRModifierOSC1_q24 = (ADSR3ToOscSelect == 0 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4) ? ADSRModifier_q24 : 0;
    int32_t ADSRModifierOSC2_q24 = (ADSR3ToOscSelect == 1 || ADSR3ToOscSelect == 2 || ADSR3ToOscSelect == 4) ? ADSRModifier_q24 : 0;
    BENCH_END(vt_adsr_mod);
    
    BENCH_BEGIN(vt_drift_mod);
    int32_t DETUNE_DRIFT_OSC1_q24 = (drift_pitch_scale_q24 != 0) ? applyDepthQ24(p_drift[DCO_A], drift_pitch_scale_q24) : 0;
    int32_t DETUNE_DRIFT_OSC2_q24 = (drift_pitch_scale_q24 != 0) ? applyDepthQ24(p_drift[DCO_B], drift_pitch_scale_q24) : 0;
    BENCH_END(vt_drift_mod);

    BENCH_BEGIN(vt_unison_mod);
    int32_t voiceMag = (i >> 1) + 1;
    // Pillar I: Branchless unison multiplier (turns odd/even to -1 or 1)
    int32_t voiceSign = 1 - ((i & 1) << 1); 
    int32_t unisonMODIFIER_q24 = unisonBase_q24 * (voiceSign * voiceMag);
    BENCH_END(vt_unison_mod);

    int32_t freqModifiers1_q24, freqModifiers2_q24;
    {
      BENCH_BEGIN(vt_modifiers);
      // All done dynamically in integer core pipeline
      int32_t modifiersBase_q24 = calcPitchbend_q24 + Q24_ONE_EPS + p_matrix_pitch[i] + unisonMODIFIER_q24;
      if (char_pitch_scale_q15) {
        modifiersBase_q24 += character_pitch_delta_q24();
      }
      
      freqModifiers1_q24 = ADSRModifierOSC1_q24 + DETUNE_DRIFT_OSC1_q24 + modifiersBase_q24 + 
                           lfo1_pitch_mod_q24[LFO1_PITCH_OSC1] + p_matrix_osc1[i];
                           
      freqModifiers2_q24 = ADSRModifierOSC2_q24 + DETUNE_DRIFT_OSC2_q24 + modifiersBase_q24 + 
                           lfo1_pitch_mod_q24[LFO1_PITCH_OSC2] + lfo2_pitch_mod_q24[LFO2_PITCH_OSC2] + p_matrix_osc2[i];
      BENCH_END(vt_modifiers);
    }

    BENCH_BEGIN(vt_ratio_interp);
    // Convert out to Float ONCE precisely at the step where we need it
    float ratio1 = interpolate_live_ratio_f(q24_to_float(freqModifiers1_q24), DCO_A);
    float ratio2 = interpolate_live_ratio_f(q24_to_float(freqModifiers2_q24), DCO_B);
    BENCH_END(vt_ratio_interp);

    BENCH_BEGIN(vt_freq_scale_post);
    float freqA_Hz = freqA * ratio1;
    float freqB_Hz = freqB * (ratio2 * osc2DetuneRatio);
#if DCO_DEBUG_REPORT
    dbg_freq_after_mod_Hz = freqA_Hz;
#endif
    BENCH_END(vt_freq_scale_post);

    // =========================================================================
    // PIO CLK_DIV & HARDWARE COMMIT (Unchanged float implementations)
    // =========================================================================
    BENCH_BEGIN(vt_clk_div);
    float correction = 0.0f;
    const uint32_t sys_hz = sysClock_Hz;
    uint32_t total_cycles1 = clkdiv_live_total_cycles(sys_hz, freqA_Hz) + (uint32_t)correction;
    uint32_t total_cycles2 = clkdiv_live_total_cycles(sys_hz, freqB_Hz) + (uint32_t)correction;

    const uint32_t wA = osc_ramp_weight(DCO_A), kA = osc_period_overhead(DCO_A);
    const uint32_t wB = osc_ramp_weight(DCO_B), kB = osc_period_overhead(DCO_B);

    uint32_t clk_div1 = pio_clk_div_for_y(total_cycles1, osc_last_y[DCO_A], wA, kA);
    uint32_t clk_div2 = pio_clk_div_for_y(total_cycles2, osc_last_y[DCO_B], wB, kB);
    BENCH_END(vt_clk_div);

    uint32_t phaseHoldX = 0;
    PioPeriod retrig_p1{};
    PioPeriod retrig_p2{};
    if (note_on_flag_flag[i] && oscPhaseSync > 1) {
      BENCH_BEGIN(vt_phase_align);
      phaseHoldX = osc_phase_hold_x(total_cycles2, phaseAlignOSC2);
      BENCH_END(vt_phase_align);
    }
    if (note_on_flag_flag[i] && oscPhaseSync >= 1 && note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
      BENCH_BEGIN(vt_retrig_split);
      retrig_p1 = pio_period_split(total_cycles1, wA, kA);
      retrig_p2 = pio_period_split(total_cycles2, wB, kB);
      BENCH_END(vt_retrig_split);
    }

    BENCH_BEGIN(vt_chan_level);
    uint16_t chanLevel, chanLevel2;
    switch (syncMode) {
    case 1: {
      float maxFreq = (freqA_Hz > freqB_Hz) ? freqA_Hz : freqB_Hz;
      chanLevel = get_chan_level_for_engine(maxFreq, DCO_A);
      chanLevel2 = get_chan_level_for_engine(freqB_Hz, DCO_B);
      break;
    }
    case 2: {
      float maxFreq = (freqA_Hz > freqB_Hz) ? freqA_Hz : freqB_Hz;
      chanLevel = get_chan_level_for_engine(freqA_Hz, DCO_A);
      chanLevel2 = get_chan_level_for_engine(maxFreq, DCO_B);
      break;
    }
    default:
      chanLevel = get_chan_level_for_engine(freqA_Hz, DCO_A);
      chanLevel2 = get_chan_level_for_engine(freqB_Hz, DCO_B);
      break;
    }
    BENCH_END(vt_chan_level);

    PIO pioN_A = pio[VOICE_TO_PIO[DCO_A]];
    PIO pioN_B = pio[VOICE_TO_PIO[DCO_B]];
    uint8_t sm1N = VOICE_TO_SM[DCO_A];
    uint8_t sm2N = VOICE_TO_SM[DCO_B];

    if (note_on_flag_flag[i]) {
      BENCH_BEGIN(vt_note_retrig);
      if (oscPhaseSync >= 1) {
        BENCH_BEGIN(vt_retrig_sm_apply);
        if (note_retrig_mode != NOTE_RETRIG_SYNC_JMP) {
          uint32_t maskAB = (1u << sm1N) | (1u << sm2N);
          pio_set_sm_mask_enabled(pioN_A, maskAB, false);
          osc_load_periods_stopped_noclear(DCO_A, retrig_p1.y, retrig_p1.clk_div, DCO_B, retrig_p2.y, retrig_p2.clk_div);
          pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(osc_restart_target(DCO_A)));

          if (phaseHoldX != 0) osc_phase_align_hold_stopped(DCO_B, phaseHoldX);
          else pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(osc_restart_target(DCO_B)));

          pio_enable_sm_mask_in_sync(pioN_A, maskAB);
        } else {
          pio_sm_exec(pioN_A, sm1N, pio_encode_jmp(osc_restart_target(DCO_A)));
          pio_sm_exec(pioN_B, sm2N, pio_encode_jmp(osc_restart_target(DCO_B)));
        }
        BENCH_END(vt_retrig_sm_apply);
      }
      BENCH_END(vt_note_retrig);
    } else {
      BENCH_BEGIN(vt_pio_write);
      pio_sm_put(pioN_A, sm1N, clk_div1);
      pio_sm_put(pioN_B, sm2N, clk_div2);
      pio_sm_exec(pioN_A, sm1N, pio_encode_pull(false, false));
      pio_sm_exec(pioN_B, sm2N, pio_encode_pull(false, false));
      osc_last_clk_div[DCO_A] = clk_div1;
      osc_last_clk_div[DCO_B] = clk_div2;
      BENCH_END(vt_pio_write);
    }

    write_range_pwm(DCO_A, chanLevel);
    write_range_pwm(DCO_B, chanLevel2);

    if (timer99microsFlag2) {
      if (pulseWaveOn) {
        BENCH_FBEGIN(vt_pwm_calc);

        const int16_t local_ADSR3toPWM = ADSR3toPWM;

        // Clean integer additions
        const int32_t adsr3_delta = ((int32_t)p_adsr3[i] * (int32_t)local_ADSR3toPWM) >> 15;
        int32_t pw_calc = (int32_t)PW[0] + lfo2_pw_delta + adsr3_delta + matrix_pw_mod[i] + (int32_t)character_pw_delta();

        // Pillar I: Branchless Ternary Clamping
        // Compiles down to an inline ARM CSEL Instruction (Conditional Select)
        pw_calc = pw_calc < 0 ? 0 : pw_calc;
        pw_calc = pw_calc > (int32_t)(DIV_COUNTER_PW - 1) ? (int32_t)(DIV_COUNTER_PW - 1) : pw_calc;

        PW_PWM[i] = (uint16_t)pw_calc;
        voice_write_pw(i, get_PW_level_interpolated(PW_PWM[i], DCO_A, freqA_Hz));

        BENCH_FEND(vt_pwm_calc);
      } else {
        voice_write_pw(i, 0);
      }
    }
  }

  // Clear flags at the end of the frame
  for (int k = 0; k < NUM_VOICES_TOTAL; k++) {
    note_on_flag_flag[k] = false;
  }

  last_portamento_time = portaTime;
  last_portamento_mode = portaMode;
}
#endif // USE_FLOAT_VOICE_TASK_Q24
