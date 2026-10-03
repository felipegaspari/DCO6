#ifndef __AUTOTUNE_IMPL_H__
#define __AUTOTUNE_IMPL_H__

#include "../include_all.h"

// =============================================================================
// autotune_impl.h — DCO calibration orchestration, PW center/limit searches and
// the edge-timing duty measurement core (find_gap).
//
// Definitions, not declarations: include this exactly once per sketch, from a
// .ino shim so the merge order of the sketch's translation unit is unchanged
// (DCO/autotune.ino). The declarations are in autotune.h.
//
// The per-note amplitude-compensation search (calibrate_DCO) and its helpers
// live in autotune_search_impl.h.
// =============================================================================

// File-scope helpers called before the line that defines them. While this code
// lived in a .ino the Arduino builder generated these prototypes; a header gets
// none, so they are written out here.
static void reset_pw_to_DIV_COUNTER_PW();
static inline void apply_pw_center(uint8_t ch) {
  if (ch >= NUM_PW_CHANNELS || PW_PINS[ch] == PW_PIN_UNASSIGNED) return;
  pwm_set_chan_level(PW_PWM_SLICES[ch], pwm_gpio_to_channel(PW_PINS[ch]), PW_CENTER[ch]);
  PW[ch] = PW_CENTER[ch];
}

// Stored center on the measured channel, 0 on all the others — the same rule
// the manual walk follows (voice_task_autotune), and for the same reason: the
// pulse is the one wave with no analog switch, so a channel left anywhere but 0
// is an open pulse on a voice nothing is measuring. Applies only; no search can
// move a stored center from here.
//
// Only the soloed channel is tracked in PW[]: the muted ones would write PW[0],
// which the play path reads as the panel's pulse width (apply_param_pw_value).
static void apply_pw_center_solo(uint8_t soloCh) {
  for (uint8_t ch = 0; ch < NUM_PW_CHANNELS; ++ch) {
    if (PW_PINS[ch] == PW_PIN_UNASSIGNED) continue;
    if (ch == soloCh) {
      apply_pw_center(ch);
      continue;
    }
    pwm_set_chan_level(PW_PWM_SLICES[ch], pwm_gpio_to_channel(PW_PINS[ch]), 0);
  }
}

// What a PW channel is actually driving, read back from the slice's compare
// register. The diagnostics want this rather than a tracked copy: PW[] is not a
// hardware mirror (the play path's voice_write_pw() does not update it, and
// PW[0] doubles as the panel's pulse width), and the tracker this replaces was
// only touched by the PW search — it reported 0 through whole amp runs that were
// in fact sitting at max wrap.
static uint16_t pw_level_readback(uint8_t ch) {
  if (ch >= NUM_PW_CHANNELS || PW_PINS[ch] == PW_PIN_UNASSIGNED) return 0;
  const uint32_t cc = pwm_hw->slice[PW_PWM_SLICES[ch]].cc;
  return (pwm_gpio_to_channel(PW_PINS[ch]) == PWM_CHAN_A) ? (uint16_t)(cc & 0xFFFFu)
                                                          : (uint16_t)(cc >> 16);
}

// For debug logging and duty computation in gap measurement: the duty
// target/period assumed by the current PW search routine, and the most
// recently measured period from find_gap(). The PW in force is not tracked
// here — pw_level_readback() asks the hardware instead.
static double   g_gapLogCurrentPeriodUs = 0.0;
static double   g_gapLogTargetDutyFraction = 0.5;  // default 50%

// Helper: turn off all oscillators and set their RANGE outputs to a known
// state, while charging their timing capacitors using the original
// PIO+GPIO sequence. This preserves the analogue behaviour you rely on.
static void disable_all_oscillators_and_range_pwm() {
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    PIO     pioN      = pio[VOICE_TO_PIO[i]];
    uint8_t smN = VOICE_TO_SM[i];

    // Original "park" frequency used to pre-charge the caps.
    uint32_t clk_div1 = 200;

    // Run the DCO SM at a known slow rate while driving the RANGE PWM.
    pio_sm_set_enabled(pioN, smN, true);
    pio_sm_put(pioN, smN, clk_div1);
    pio_sm_exec(pioN, smN, pio_encode_pull(false, false));

    delay(200);

    // Stop the SM and hold the RANGE pin high as a plain GPIO output.
    pio_sm_set_enabled(pioN, smN, false);
#ifdef RANGE0_PIO_DITHER_TEST
    range_pio_set_level((uint8_t)i, DIV_COUNTER);  // full-on via PIO; do not steal RANGE pin
    continue;
#endif
    gpio_init(RANGE_PINS[i]);
    gpio_set_dir(RANGE_PINS[i], GPIO_OUT);
    gpio_put(RANGE_PINS[i], 1);
  }

  // After all RANGE caps are charged, park every PW PWM at max wrap so the
  // centre search can start from a known state. (Matches original behaviour.)
  //
  // Max wrap is a rail, not a usable operating point: there the comparator has
  // no crossing to make and the sense pin never toggles. Only the PW searches
  // may leave it here, since they program PW on every probe. Any stage that
  // measures without writing PW has to put the stored centre back first —
  // restart_DCO_calibration() does that for the amp-comp stage.
  reset_pw_to_DIV_COUNTER_PW();

  // Nothing is oscillating any more, so the next probe is a cold start.
  g_lastDrivenFreqHz = 0.0f;
}



// Helper: park every assigned PW PWM at max wrap (DIV_COUNTER_PW). Called from
// disable_all_oscillators_and_range_pwm(). Unassigned pins are skipped.
static void reset_pw_to_DIV_COUNTER_PW() {
  for (int i = 0; i < NUM_PW_CHANNELS; i++) {
    if (PW_PINS[i] == PW_PIN_UNASSIGNED) continue;
    pwm_set_chan_level(PW_PWM_SLICES[i], pwm_gpio_to_channel(PW_PINS[i]), DIV_COUNTER_PW);
  }
}

// After auto-cal / verify: the disable helper left every voice SM stopped
// (except the last probe, parked at an inaudible amp-0 frequency). Playback
// only writes dividers into those SMs, so there is no sound until something
// like Send all hits PARAM_SYNC_MODE and calls start_voice_sms(). Restore
// that here — already on core 1.
static void restore_voice_engine_after_calibration() {
  for (uint8_t ch = 0; ch < NUM_PW_CHANNELS; ++ch) {
    apply_pw_center(ch);
  }
  start_voice_sms();
  for (int i = 0; i < NUM_VOICES_TOTAL; i++) {
    note_on_flag[i] = 1;
  }
}

/*************************************************************************************/
/*************************************************************************************/
/*************************************************************************************/
// Main DCO auto-calibration entry point; the stage(s) it runs come from
// calibrationScope (value of PARAM_CALIBRATION_FLAG: 1 amp, 2 PW, 3 full).
// PW stage: once per assigned PW channel (center + low/high limits).
// Amp stage: per oscillator, calibrate_DCO()/calibrate_DCO_freq_trace() build a
// [freq -> range PWM] table persisted via update_FS_voice().
void DCO_calibration() {
  autotune_fill_init_manual_amp();

  // A fresh run always starts un-cancelled; PARAM_CALIBRATION_FLAG = 0 (core 0)
  // raises the request while this function blocks core 1.
  calibrationCancelRequested = false;

  // Stage selection comes from the value of PARAM_CALIBRATION_FLAG.
  const uint8_t scope = calibrationScope;
  const bool runPW  = calibration_scope_runs_pw(scope);
  const bool runAmp = calibration_scope_runs_amp(scope);
  const bool fine = (calibrationPrecision == CAL_PRECISION_FINE);
  Serial.println((String)"[DCO_CAL] scope: " + calibration_scope_name(scope) +
                 " precision: " + calibration_precision_name(calibrationPrecision) +
                 " (param 150 value: 1=amp-comp, 2=PW, 3=full;" +
                 " 5/6/7 = the same in fine mode, 9/10/11 in fast mode)");

  // Make the active amp-comp method visible up front: a stored 440 Hz anchor
  // is only used when FREQ_TRACE is selected (panel buttons / debug cmds 34-35,
  // boot default from AUTOTUNE_AMP_METHOD_DEFAULT). The fine pass measures the
  // stored table whatever built it, so the method does not apply there.
  if (runAmp && fine) {
    Serial.println("[DCO_CAL] amp-comp stage: refining the stored tables "
                   "(amp comp values kept, frequencies re-measured)");
  } else if (runAmp) {
    Serial.println((String)"[DCO_CAL] amp-comp method: " +
                   autotune_amp_method_name(autotuneAmpMethod) +
                   " (select via PARAM_DEBUG_COMMAND 34=CLASSIC / 35=FREQ_TRACE)");
  }
  if (runAmp) {
    Serial.println((String)"[DCO_CAL] freq search: " +
                   autotune_search_mode_name(autotuneSearchMode) +
                   " (37=BISECT / 38=INTERP / 39=GATED; compare probes= and"
                   " elapsed= on the report footer)");
    Serial.println((String)"[DCO_CAL] amp-0 endpoint: " +
                   autotune_amp0_mode_name(autotuneAmp0Mode) +
                   " (40=MEASURE live hunt / 41=CALC bottom-rung fit)");
  }

  // TURN OFF ALL OSCILLATORS and park PW channels.
  disable_all_oscillators_and_range_pwm();

  // PW is per channel (DCO3: one wired pin; DCO4: one per voice, two oscs
  // sharing it) and independent of the amp-comp stage. An amp-only run reuses
  // the PW centers already stored in the FS, applied per oscillator by
  // restart_DCO_calibration(). Each distinct assigned channel is calibrated
  // once, driving the first oscillator that maps to it.
  if (runPW) {
    uint8_t lastCh = 0xFF;
    for (uint8_t osc = 0; osc < NUM_OSCILLATORS && !calibrationCancelRequested; ++osc) {
      const uint8_t ch = cal_pw_channel(osc);
      if (ch == lastCh) continue;
      if (PW_PINS[ch] == PW_PIN_UNASSIGNED) continue;
      lastCh = ch;
      currentDCO = osc;
      restart_DCO_calibration();
      DCO_calibration_current_note = manual_DCO_calibration_start_note;
      VOICE_NOTES[0] = DCO_calibration_current_note;
      find_PW_center(0);
      if (!calibrationCancelRequested) find_PW_limit_v2(PW_LIMIT_LOW);
      if (!calibrationCancelRequested) find_PW_limit_v2(PW_LIMIT_HIGH);
    }
  }

  for (int i = 0; runAmp && i < NUM_OSCILLATORS && !calibrationCancelRequested; i++) {
    currentDCO = i;

    restart_DCO_calibration();

    ampCompCalibrationVal = initManualAmpCompCalibrationVal[currentDCO] + manualCalibrationOffset[currentDCO];
    write_range_pwm(currentDCO, ampCompCalibrationVal);

    DCO_calibration_current_note = DCO_calibration_start_note;
    VOICE_NOTES[0] = DCO_calibration_current_note;

    // Build a small context for this DCO and run the calibration routine.
    DCOCalibrationContext ctx(
      currentDCO,
      DCO_calibration_current_note,
      calibrationData,
      manualCalibrationOffset,
      initManualAmpCompCalibrationVal
    );
    // Amp-comp stage: method A (classic per-note PWM search) or method B
    // (fixed-PWM frequency tracing), selected via PARAM_DEBUG_COMMAND 34/35.
    bool tableOk = true;
    cal_report_reset();
    if (fine) {
      tableOk = refine_DCO_amp_table(ctx);
    } else if (autotuneAmpMethod == AMP_METHOD_FREQ_TRACE) {
      tableOk = calibrate_DCO_freq_trace(ctx);
    } else {
      // The classic path inherits the two header pairs written by
      // restart_DCO_calibration(): the amp-comp-0 placeholder (measured later
      // by apply_measured_lowest_freq) and the trimpot operating point.
      cal_report_set_pair(0, kCalDutyErrUnknown, CAL_SRC_FILLED);
      cal_report_set_pair(1, kCalDutyErrUnknown, CAL_SRC_MANUAL);
      // Desired duty-cycle error tolerance as a fraction (e.g. 0.005 = 0.5%).
      double dutyErrorFraction = 0.001;
      calibrate_DCO(ctx, dutyErrorFraction);
    }

    // The classic method leaves an extrapolated (or placeholder) amp-comp-0
    // anchor in entry [0..1]; measure it for real before the table is
    // printed/persisted. FREQ_TRACE measures its own bottom endpoint, and the
    // fine pass re-measures whatever pair 0 already holds. In CALC mode the
    // table keeps what find_lowest_freq() computed - which is already the
    // least-squares fit through the bottom of the table - with no live hunt.
    if (!fine && autotuneAmpMethod != AMP_METHOD_FREQ_TRACE &&
        !calibrationCancelRequested && tableOk) {
      // FAST runs always skip the hunt, like the FREQ_TRACE bottom endpoint.
      if (autotuneAmp0Mode == AMP0_MODE_CALC ||
          calibrationPrecision == CAL_PRECISION_FAST) {
        Serial.println((String)"[LOWEST_FREQ] DCO=" + currentDCO +
                       " amp-0 hunt skipped (" +
                       ((calibrationPrecision == CAL_PRECISION_FAST)
                          ? "FAST run" : "CALC") +
                       "); keeping the calculated " +
                       ((float)calibrationData[0] / 100.0f) + " Hz");
      } else {
        apply_measured_lowest_freq(ctx);
      }
    }

    for (int j = 0; j < chanLevelVoiceDataSize; j++) {
      Serial.println(calibrationData[j]);
    }

    if (!calibrationCancelRequested) {
      print_calibration_report(currentDCO, calibrationData);
    }

    if (calibrationCancelRequested) {
      // Interrupted mid-osc: discard this table; previously finished
      // oscillators keep the tables already persisted.
      Serial.println((String)"[DCO_CAL] DCO=" + currentDCO +
                     " interrupted; keeping previous calibration");
    } else if (tableOk) {
      update_FS_voice(currentDCO);
    } else {
      Serial.println((String)"[FREQ_TRACE_ERROR] DCO=" + currentDCO +
                     " table rejected; keeping previous calibration");
    }

    Serial.println((String) "DCO " + currentDCO + (String) " calibration finished.");
  }
  if (calibrationCancelRequested) {
    Serial.println("[DCO_CAL] cancelled by user");
    calibrationCancelRequested = false;
  } else {
    Serial.println((String)"[DCO_CAL] " + calibration_scope_name(scope) + " calibration done");
  }
  calibrationFlag = false;
  init_FS();

  // Rebuild amp-comp tables for the active engine.
  precompute_amp_comp_for_engine();
  restore_voice_engine_after_calibration();
}
/*************************************************************************************/
/*************************************************************************************/
/*************************************************************************************/

// --- Calibration report ----------------------------------------------------

// Clear the per-pair provenance/duty bookkeeping before an oscillator runs.
void cal_report_reset() {
  for (int p = 0; p < kCalReportPairs; ++p) {
    calPointDutyErrPct[p] = kCalDutyErrUnknown;
    calPointSource[p]     = CAL_SRC_NONE;
  }
  calReportLadderInterval = 0;
  calReportAnchorPair     = -1;
  calRunProbes            = 0;
  calRunStartMs           = millis();
}

// Record what a table pair is and how well it landed. dutyErrPct is signed
// (+ = duty above 50%); pass kCalDutyErrUnknown when there is no measurement.
void cal_report_set_pair(int pair, float dutyErrPct, uint8_t src) {
  if (pair < 0 || pair >= kCalReportPairs) {
    return;
  }
  calPointDutyErrPct[pair] = dutyErrPct;
  calPointSource[pair]     = src;
}

// Same, converting a measured gap in microseconds at freqHz into duty error.
void cal_report_set_pair_from_gap(int pair, float gapUs, float freqHz, uint8_t src) {
  cal_report_set_pair(pair, duty_err_pct_from_gap(gapUs, freqHz), src);
}

// Right-align a field so the [CAL_REPORT] table stays readable in a terminal.
static String cal_pad_left(const String& s, int width) {
  String out = s;
  while ((int)out.length() < width) {
    out = " " + out;
  }
  return out;
}

static const char *cal_point_source_name(uint8_t src) {
  switch (src) {
    case CAL_SRC_RUNG:          return "rung";
    case CAL_SRC_ANCHOR:        return "anchor";
    case CAL_SRC_ENDPOINT_FULL: return "endpoint-full";
    case CAL_SRC_ENDPOINT_AMP0: return "endpoint-amp0";
    case CAL_SRC_MANUAL:        return "manual";
    case CAL_SRC_FILLED:        return "filled";
    case CAL_SRC_SENTINEL:      return "sentinel";
    case CAL_SRC_REFINED:       return "refined";
    default:                    return "-";
  }
}

// Print the finished table for one oscillator: every pair with the duty error
// it was measured at, plus the endpoints, the span and the worst point.
// data is the [freq*100, amp comp] table that is about to be persisted.
void print_calibration_report(uint8_t dcoIndex, const uint32_t *data) {
  if (autotuneDebug < 1) {
    return;
  }

  String header = (String)"[CAL_REPORT] DCO=" + dcoIndex +
                  " method=" +
                  ((calibrationPrecision == CAL_PRECISION_FINE)
                     ? "REFINE"
                     : autotune_amp_method_name(autotuneAmpMethod)) +
                  " precision=" + calibration_precision_name(calibrationPrecision) +
                  " search=" + autotune_search_mode_name(autotuneSearchMode);
  if (calReportLadderInterval > 0) {
    header += (String)" ladder=" + calReportLadderInterval + " semitones";
  }
  if (calReportAnchorPair >= 0) {
    header += (String)" anchorPair=" + calReportAnchorPair;
  }
  Serial.println(header);
  Serial.println("[CAL_REPORT] pair    freqHz  ampComp  dutyErr%     gapUs    1cnt%  src");

  float    errSum      = 0.0f;
  int      errCount    = 0;
  float    worstErr    = -1.0f;
  int      worstPair   = -1;
  int      measured    = 0;
  int      highestPair = -1;

  for (int p = 0; p < kCalReportPairs; ++p) {
    const float    freqHz = (float)data[2 * p] / 100.0f;
    const uint32_t amp    = data[2 * p + 1];
    const uint8_t  src    = calPointSource[p];
    const bool     isSent = (src == CAL_SRC_SENTINEL);
    const float    err    = calPointDutyErrPct[p];
    const bool     hasErr = (fabsf(err) < 1e8f);

    if (!isSent) {
      highestPair = p;
    }
    if (src == CAL_SRC_RUNG || src == CAL_SRC_ANCHOR ||
        src == CAL_SRC_ENDPOINT_FULL || src == CAL_SRC_ENDPOINT_AMP0 ||
        src == CAL_SRC_REFINED) {
      ++measured;
    }

    String line = "[CAL_REPORT] " + cal_pad_left(String(p), 4);
    line += cal_pad_left(isSent ? String("-") : fmt_freq(freqHz), 10);
    line += cal_pad_left(String(amp), 9);

    if (hasErr) {
      const float absErr = fabsf(err);
      errSum += absErr;
      ++errCount;
      if (absErr > worstErr) {
        worstErr  = absErr;
        worstPair = p;
      }
      // gapUs is the inverse of duty_err_pct_from_gap() at this frequency.
      const float gapUs = (freqHz > 0.0f) ? (err * 20000.0f / freqHz) : 0.0f;
      line += cal_pad_left(String(err, 3), 10);
      line += cal_pad_left(String(gapUs, 2), 10);
    } else {
      line += cal_pad_left("-", 10);
      line += cal_pad_left("-", 10);
    }

    // Duty change caused by one count of amp comp, to first order
    // (duty - 0.5 scales with the relative amplitude error, so one count of
    // 'amp' is 50/amp percentage points). This is the floor for that point:
    // a dutyErr% already below it cannot be improved by more averaging.
    if (!isSent && amp > 0) {
      line += cal_pad_left(String(50.0f / (float)amp, 3), 9);
    } else {
      line += cal_pad_left("-", 9);
    }
    line += (String)"  " + cal_point_source_name(src);
    Serial.println(line);
  }

  const float lowestHz  = (float)data[0] / 100.0f;
  const float highestHz = (highestPair >= 0) ? ((float)data[2 * highestPair] / 100.0f) : 0.0f;
  String span = "-";
  if (lowestHz > 0.0f && highestHz > lowestHz) {
    span = String(log2f(highestHz / lowestHz), 2);
  }
  Serial.println((String)"[CAL_REPORT] DCO=" + dcoIndex +
                 " lowest=" + lowestHz + " Hz highest=" + highestHz +
                 " Hz span=" + span + " octaves measured=" + measured +
                 "/" + kCalReportPairs);

  if (errCount > 0 && worstPair >= 0) {
    Serial.println((String)"[CAL_REPORT] DCO=" + dcoIndex +
                   " dutyErr avg=" + String(errSum / (float)errCount, 3) +
                   "% worst=" + String(worstErr, 3) +
                   "% at pair " + worstPair +
                   " (" + String((float)data[2 * worstPair] / 100.0f, 2) + " Hz)");
  } else {
    Serial.println((String)"[CAL_REPORT] DCO=" + dcoIndex +
                   " dutyErr: no measured points");
  }

  // What this oscillator cost. Together with the dutyErr line above, this is the
  // whole A/B: same table quality in fewer probes and less time is a better
  // search mode (cmds 37-39), and a probe count far below the number of pairs
  // means something converged on its first reading rather than measuring.
  const unsigned long elapsedMs = millis() - calRunStartMs;
  Serial.println((String)"[CAL_REPORT] DCO=" + dcoIndex +
                 " search=" + autotune_search_mode_name(autotuneSearchMode) +
                 " probes=" + calRunProbes +
                 " elapsed=" + String(elapsedMs / 1000.0f, 1) + " s" +
                 " (" + String((float)elapsedMs / (float)max(calRunProbes, 1u), 1) +
                 " ms/probe)");
}

// --- Verification sweep ----------------------------------------------------

// Semitones between probes of the verification sweep.
static constexpr uint8_t kCalVerifyNoteStep = 3;

// Read-only pass over the finished tables, measuring what the engine would
// actually produce: for every oscillator walk the playable range and take the
// amp comp from the *runtime* lookup (interpolated), not from the table row,
// so interpolation error and table error are both visible.
// Reading the result: a constant error is a frame-of-reference offset between
// the sense pin and the real output (that is what the duty trim is for), error
// peaking between breakpoints is interpolation, error growing toward the low
// notes is the one-count floor printed as 1cnt=.
void run_calibration_verify_sweep() {
  Serial.println((String)"[CAL_VERIFY] start: step=" + kCalVerifyNoteStep +
                 " semitones, amp from the runtime lookup (" +
                 amp_comp_method_name(amp_comp_method) + ")");

  // One probe per note, so the fine profile's long averaging costs almost
  // nothing here and the numbers are worth trusting.
  const uint8_t precisionBefore = calibrationPrecision;
  calibrationPrecision = CAL_PRECISION_FINE;

  calibrationCancelRequested = false;
  calibrationFlag = true;  // keeps the main voice task off this core's oscillators

  disable_all_oscillators_and_range_pwm();

  for (uint8_t osc = 0; osc < NUM_OSCILLATORS && !calibrationCancelRequested; ++osc) {
    currentDCO = osc;
    restart_DCO_calibration();

    // Top of the useful range: the frequency where the table saturates at full
    // amp comp. Without a plateau, fall back to the amp-comp table's own limit.
    float topHz = (plateauStartFreqQ[osc] > 0)
                    ? ((float)plateauStartFreqQ[osc] / (float)(1 << FREQ_FRAC_BITS))
                    : (float)AMP_COMP_MAX_HZ;

    float errSum   = 0.0f;
    int   errCount = 0;
    float worstErr = -1.0f;
    float worstHz  = 0.0f;

    for (uint8_t note = manual_DCO_calibration_start_note;
         note < 120 && !calibrationCancelRequested;
         note += kCalVerifyNoteStep) {
      const float freqHz = note_to_freq(note);
      if (freqHz > topHz) {
        break;
      }
      const uint16_t amp = get_chan_level_for_engine(freqHz, osc);

      DCO_calibration_current_note = note;
      VOICE_NOTES[0] = note;
      const float gapUs = measure_duty_at_freq(freqHz, amp, true);

      String line = (String)"[CAL_VERIFY] DCO=" + osc + " note=" + note +
                    " freq=" + fmt_freq(freqHz) + " amp=" + amp;
      if (gapUs == kGapTimeoutSentinel) {
        Serial.println(line + " dutyErr=- gapUs=- (no signal)");
        continue;
      }
      const float errPct = duty_err_pct_from_gap(gapUs, freqHz);
      errSum += fabsf(errPct);
      ++errCount;
      if (fabsf(errPct) > worstErr) {
        worstErr = fabsf(errPct);
        worstHz  = freqHz;
      }
      line += " dutyErr=" + String(errPct, 3) + "% gapUs=" + String(gapUs, 2);
      if (amp > 0) {
        line += " 1cnt=" + String(50.0f / (float)amp, 3) + "%";
      }
      Serial.println(line);
    }

    if (errCount > 0) {
      Serial.println((String)"[CAL_VERIFY] DCO=" + osc +
                     " points=" + errCount +
                     " dutyErr avg=" + String(errSum / (float)errCount, 3) +
                     "% worst=" + String(worstErr, 3) +
                     "% at " + String(worstHz, 2) + " Hz");
    } else {
      Serial.println((String)"[CAL_VERIFY] DCO=" + osc + " no usable points");
    }
  }

  disable_all_oscillators_and_range_pwm();
  calibrationFlag = false;
  calibrationPrecision = precisionBefore;
  restore_voice_engine_after_calibration();
  if (calibrationCancelRequested) {
    Serial.println("[CAL_VERIFY] cancelled by user");
    calibrationCancelRequested = false;
  } else {
    Serial.println("[CAL_VERIFY] done");
  }
}

/*************************************************************************************/

// Reset per-DCO calibration state and header entries in calibrationData.
// This is called once for the PW pass and again before calibrating each DCO.
void restart_DCO_calibration() {
  autotune_fill_init_manual_amp();

  VOICE_NOTES[0] = DCO_calibration_start_note;
  DCO_calibration_current_note = DCO_calibration_start_note;

  // Table header:
  //  [0..1] "lowest frequency" anchor (freq placeholder 0, PWM ampCompLowestFreqVal)
  //  [2..3] manual starting point, one interval below the first calibrated note.
  calibrationData[0] = 0;
  calibrationData[1] = ampCompLowestFreqVal;
  calibrationData[2] = (uint32_t)(note_to_freq(DCO_calibration_current_note - calibration_note_interval) * 100);
  calibrationData[3] = initManualAmpCompCalibrationVal[currentDCO] + manualCalibrationOffset[currentDCO];

  // Reference for the 60 s safety timeouts used by the PW search phases.
  DCOCalibrationStart = millis();

  // TURN OFF ALL OSCILLATORS for a clean restart, and pre-charge the
  // RANGE capacitors using the legacy helper.
  disable_all_oscillators_and_range_pwm();

  // IMPORTANT: disable_all_oscillators_and_range_pwm() leaves RANGE_PINS[]
  // as plain GPIO outputs driven HIGH. Before starting calibration for the
  // currentDCO we must restore its RANGE pin back to PWM function so that
  // voice_task_autotune() and subsequent RANGE PWM writes actually appear
  // on the physical pin.
#ifndef RANGE0_PIO_DITHER_TEST
  gpio_set_function(RANGE_PINS[currentDCO], GPIO_FUNC_PWM);
#endif

  PIO pioN = pio[VOICE_TO_PIO[currentDCO]];
  uint8_t sm1N = VOICE_TO_SM[currentDCO];
  pio_sm_set_enabled(pioN, sm1N, true);

  // Undo the PW park above for this oscillator: the amp-comp stage never writes
  // PW itself (voice_task_autotune's auto branch only touches the divider and
  // RANGE), so whatever is left here is what it measures at — and the rail the
  // park leaves gives the comparator no crossing at all, hence a whole run of
  // [GAP_TIMEOUT] with edges=0. The PW searches overwrite this immediately.
  apply_pw_center_solo(cal_pw_channel(currentDCO));

  // This oscillator starts from nothing, so its first probe gets the full
  // settle budget instead of one sized against the previous one's frequency.
  g_lastDrivenFreqHz = 0.0f;

  delay(100);
}

/*************************************************************************************/
/*************************************************************************************/
/*************************************************************************************/
// PW search — shared low-level helpers
/*************************************************************************************/

// Helper: program a PW value on the given PW channel, keep PW[] and the debug
// tracker in sync, wait for the waveform to settle and measure the gap.
// This replaces the "set PWM, delay, measure" blocks that used to be
// copy-pasted throughout the PW search code.
//
// pwCh is a PW channel, `cal_pw_channel(osc)` — not an oscillator index. They
// coincide on DCO3 (one channel, index 0) and do not on DCO4 (two oscillators
// per channel), which is why the centre search's callers pass it explicitly
// instead of assuming 0: they used to sweep voice 0 whatever was calibrated.
static GapMeasurement set_pw_and_measure(uint8_t pwCh, uint16_t pw) {
  pwm_set_chan_level(PW_PWM_SLICES[pwCh],
                     pwm_gpio_to_channel(PW_PINS[pwCh]),
                     pw);
  PW[pwCh] = pw;
  delay(30);
  return measure_gap(2);
}

// PWSearchState / PWRecordMode are defined in autotune.h so the Arduino
// builder's auto-generated prototypes for these helpers can see the types.

static void pw_search_state_init(PWSearchState& st) {
  st.validCount       = 0;
  st.inToleranceCount = 0;
  st.haveBest         = false;
  st.bestGapAbs       = 1e12;
  st.bestPW           = 0;
  st.haveBracket      = false;
  st.pwLow            = 0;
  st.pwHigh           = 0;
  st.gapLow           = 0.0;
}

// Record one valid (non-timeout) measurement into the search state.
static void pw_record_sample(PWSearchState& st, uint16_t pw, double gapDiff,
                             double targetGap, PWRecordMode mode) {
  double absGapDiff = fabs(gapDiff);

  if (absGapDiff <= targetGap) {
    st.inToleranceCount++;
  }
  if (!st.haveBest || absGapDiff < st.bestGapAbs) {
    st.haveBest   = true;
    st.bestGapAbs = absGapDiff;
    st.bestPW     = pw;
  }

  if (mode == PW_RECORD_NO_TABLE) {
    return;
  }
  if (st.validCount < kPWMaxSamples) {
    st.validPW[st.validCount]      = pw;
    st.validGapDiff[st.validCount] = gapDiff;
    st.validCount++;
  } else if (mode == PW_RECORD_REPLACE_WORST) {
    int worstIdx = 0;
    double worstAbs = fabs(st.validGapDiff[0]);
    for (int vi = 1; vi < st.validCount; ++vi) {
      double curAbs = fabs(st.validGapDiff[vi]);
      if (curAbs > worstAbs) {
        worstAbs = curAbs;
        worstIdx = vi;
      }
    }
    if (absGapDiff < worstAbs) {
      st.validPW[worstIdx]      = pw;
      st.validGapDiff[worstIdx] = gapDiff;
    }
  }
}

// Phase 1: coarse scan over [pwMin, pwMax] looking for a sign-change bracket
// around the target duty. When a bracket is found, one extra sample at the
// linearly interpolated crossing point is measured and stored, then the scan
// stops.
static void pw_coarse_scan(PWSearchState& st,
                           double gapTarget, double targetGap,
                           uint16_t pwMin, uint16_t pwMax, uint16_t coarseStep,
                           double periodUs, double toleranceDutyPercent) {
  bool     havePrev    = false;
  double   prevGapDiff = 0.0;
  uint16_t prevPW      = 0;

  for (uint16_t pw = pwMin; pw <= pwMax; pw = (uint16_t)(pw + coarseStep)) {

    if (calibrationCancelRequested) break;
    if (millis() - DCOCalibrationStart > 60000) {
      Serial.println("PW coarse scan timeout (60s)");
      break;
    }

    GapMeasurement gm = set_pw_and_measure(cal_pw_channel(currentDCO), pw);
    if (gm.timedOut) {
      continue;  // no usable signal at this PW
    }

    double gap     = (double)gm.value;
    double gapDiff = gap - gapTarget;

    if (autotuneDebug >= 2 && periodUs > 0.0) {
      double dutyPercent = (0.5 + gap / (2.0 * periodUs)) * 100.0;
      Serial.println((String)"[PW_CENTER_COARSE] note=" + DCO_calibration_current_note +
                     (String)" DCO=" + currentDCO +
                     (String)" PW_raw=" + pw +
                     (String)" gap=" + gap +
                     (String)"us duty=" + dutyPercent +
                     (String)"% target=50% tol≈" + toleranceDutyPercent + "%");
    }

    pw_record_sample(st, pw, gapDiff, targetGap, PW_RECORD_REPLACE_WORST);

    if (havePrev &&
        ((gapDiff > 0.0 && prevGapDiff < 0.0) || (gapDiff < 0.0 && prevGapDiff > 0.0))) {
      st.haveBracket = true;
      st.pwLow  = prevPW;
      st.gapLow = prevGapDiff + gapTarget;  // raw gap at pwLow
      st.pwHigh = pw;

      // With two samples straddling the target, probe the crossing point
      // estimated by linear interpolation between them.
      double denom = fabs(prevGapDiff) + fabs(gapDiff);
      if (denom > 0.0) {
        double t = fabs(prevGapDiff) / denom;  // weight towards the closer side
        uint16_t pwEst = (uint16_t)((double)prevPW + ((double)(pw - prevPW) * t));
        if (pwEst >= pwMin && pwEst <= pwMax) {
          GapMeasurement gmEst = set_pw_and_measure(cal_pw_channel(currentDCO), pwEst);
          if (!gmEst.timedOut) {
            pw_record_sample(st, pwEst, (double)gmEst.value - gapTarget,
                             targetGap, PW_RECORD_APPEND);
          }
        }
      }
      break;
    }

    havePrev    = true;
    prevGapDiff = gapDiff;
    prevPW      = pw;
  }
}

// Phase 2a (bracket found): bisection search within the sign-change bracket.
// Midpoint samples refine the best candidate but are not added to the valid
// table (same as the original implementation).
static void pw_bisect_bracket(PWSearchState& st,
                              double gapTarget, double targetGap,
                              double periodUs, double toleranceDutyPercent) {
  uint16_t pwLow  = st.pwLow;
  uint16_t pwHigh = st.pwHigh;
  double   gapLow = st.gapLow;

  for (int iter = 0; iter < 14; ++iter) {
    if (calibrationCancelRequested) break;
    if (millis() - DCOCalibrationStart > 60000) {
      Serial.println("PW bisection timeout (60s)");
      break;
    }

    uint16_t pwMid = (uint16_t)((pwLow + pwHigh) / 2);
    GapMeasurement gm = set_pw_and_measure(cal_pw_channel(currentDCO), pwMid);
    if (gm.timedOut) {
      // No valid data at this midpoint; try again on the next iteration.
      if (autotuneDebug >= 2) {
        Serial.println("PW center: timeout during bisection, skipping midpoint.");
      }
      continue;
    }

    double gapMid     = (double)gm.value;
    double gapDiffMid = gapMid - gapTarget;

    pw_record_sample(st, pwMid, gapDiffMid, targetGap, PW_RECORD_NO_TABLE);

    if (autotuneDebug >= 2 && periodUs > 0.0) {
      double dutyPercent = (0.5 + gapMid / (2.0 * periodUs)) * 100.0;
      Serial.println((String)"[PW_CENTER_BISECT] note=" + DCO_calibration_current_note +
                     (String)" DCO=" + currentDCO +
                     (String)" PW_raw=" + pwMid +
                     (String)" gap=" + gapMid +
                     (String)"us duty=" + dutyPercent +
                     (String)"% target=50% tol≈" + toleranceDutyPercent + "%");
    }

    // Maintain the sign-change bracket.
    if ((gapDiffMid > 0.0 && (gapLow - gapTarget) > 0.0) ||
        (gapDiffMid < 0.0 && (gapLow - gapTarget) < 0.0)) {
      pwLow  = pwMid;
      gapLow = gapMid;
    } else {
      pwHigh = pwMid;
    }

    if (pwHigh - pwLow <= 1) {
      break;  // can't refine further in integer PW space
    }
  }
}

// Phase 2b (no bracket): local fine scan around the best coarse candidate so
// that we still gather several near-target samples before deciding.
static void pw_fine_scan_around_best(PWSearchState& st,
                                     double gapTarget, double targetGap,
                                     uint16_t pwMin, uint16_t pwMax,
                                     uint16_t coarseStep) {
  if (autotuneDebug >= 1) {
    Serial.println("PW center: no sign-change bracket found, running local fine scan.");
  }

  uint16_t startPW = (st.haveBest && st.bestPW >= pwMin && st.bestPW <= pwMax)
                       ? st.bestPW
                       : (uint16_t)((pwMin + pwMax) / 2);
  uint16_t span = (coarseStep > 0) ? coarseStep * 2 : 4;
  uint16_t fineMin = (startPW > span) ? (startPW - span) : pwMin;
  uint16_t fineMax = (startPW + span < pwMax) ? (startPW + span) : pwMax;
  if (fineMax < fineMin) {
    uint16_t tmp = fineMin;
    fineMin = fineMax;
    fineMax = tmp;
  }
  uint16_t fineStep = (fineMax > fineMin) ? ((fineMax - fineMin) / 16) : 1;
  if (fineStep == 0) fineStep = 1;

  for (uint16_t pw = fineMin; pw <= fineMax; pw = (uint16_t)(pw + fineStep)) {
    if (calibrationCancelRequested) break;
    if (millis() - DCOCalibrationStart > 60000) {
      Serial.println("PW local fine scan timeout (60s)");
      break;
    }

    GapMeasurement gm = set_pw_and_measure(cal_pw_channel(currentDCO), pw);
    if (gm.timedOut) {
      continue;
    }
    pw_record_sample(st, pw, (double)gm.value - gapTarget, targetGap, PW_RECORD_APPEND);
  }
}

// Lock-in: demand 3 consecutive measurements within targetGap of gapTarget at
// the given PW (up to 8 tries). On success, writes the last locked gap to
// lockedGapOut and returns true.
static bool pw_lock_in(uint8_t voiceIdx, uint16_t pw,
                       double gapTarget, double targetGap,
                       double periodUs, double& lockedGapOut) {
  const int kMaxLockInTries = 8;
  int consecutiveOk = 0;

  for (int li = 0; li < kMaxLockInTries; ++li) {
    if (calibrationCancelRequested) return false;
    GapMeasurement gm = set_pw_and_measure(voiceIdx, pw);
    if (gm.timedOut || periodUs <= 0.0) {
      consecutiveOk = 0;
      continue;
    }

    double gap = (double)gm.value;
    if (fabs(gap - gapTarget) <= targetGap) {
      consecutiveOk++;
      if (consecutiveOk >= 3) {
        lockedGapOut = gap;
        return true;
      }
    } else {
      consecutiveOk = 0;
    }
  }
  return false;
}

// Phase 3: pick the best candidate from the valid-samples table (smallest gap
// to target first), demanding a lock-in at each candidate. A locked candidate
// is then refined locally (PW-2..PW+2, each with its own mini lock-in).
// Returns true and writes the final PW to chosenPWOut on success; false if
// every candidate failed lock-in or the best gap was hopelessly large.
static bool pw_select_and_lock(PWSearchState& st,
                               double gapTarget, double targetGap,
                               uint16_t pwMin, uint16_t pwMax,
                               double periodUs, uint16_t& chosenPWOut) {
  // Try candidates from best gap to worse. After each failed lock-in the
  // candidate's gap difference is inflated so it won't be chosen again.
  for (int attempt = 0; attempt < st.validCount; ++attempt) {
    if (calibrationCancelRequested) return false;
    int    bestIdx = -1;
    double bestAbs = 1e12;
    int    inTolForThisPass = 0;

    for (int vi = 0; vi < st.validCount; ++vi) {
      double curAbs = fabs(st.validGapDiff[vi]);
      if (curAbs <= targetGap) {
        inTolForThisPass++;
      }
      if (curAbs < bestAbs) {
        bestAbs = curAbs;
        bestIdx = vi;
      }
    }

    if (bestIdx < 0) {
      break;
    }

    // If the best gap is still extremely large compared to the allowed gap
    // (e.g. > 10x), abort early and keep the previous PW center.
    if (bestAbs > targetGap * 10.0) {
      if (autotuneDebug >= 1) {
        Serial.println((String)"[PW_CENTER_ABORT] note=" + DCO_calibration_current_note +
                       (String)" DCO=" + currentDCO +
                       (String)" bestGap=" + bestAbs +
                       (String)"us (> " + targetGap * 10.0 +
                       (String)"us); keeping PW_center=" + PW_CENTER[cal_pw_channel(currentDCO)]);
      }
      return false;
    }

    uint16_t chosenPW  = st.validPW[bestIdx];
    double   chosenGap = gapTarget + st.validGapDiff[bestIdx];

    double lockedGap = 0.0;
    if (pw_lock_in(cal_pw_channel(currentDCO), chosenPW, gapTarget, targetGap,
                   periodUs, lockedGap)) {
      chosenGap = lockedGap;

      // Local refinement: probe a small neighbourhood around the locked-in PW
      // (PW-2..PW+2). Each candidate must pass its own mini lock-in before it
      // can replace the current choice.
      uint16_t bestLocalPW     = chosenPW;
      double   bestLocalGapAbs = bestAbs;

      for (int16_t off = -2; off <= 2; ++off) {
        int32_t testPW32 = (int32_t)chosenPW + off;
        if (testPW32 < (int32_t)pwMin || testPW32 > (int32_t)pwMax) continue;
        uint16_t testPW = (uint16_t)testPW32;

        double gapLocal = 0.0;
        if (pw_lock_in(cal_pw_channel(currentDCO), testPW, gapTarget, targetGap,
                       periodUs, gapLocal)) {
          double absGapDiffLocal = fabs(gapLocal - gapTarget);
          if (absGapDiffLocal < bestLocalGapAbs) {
            bestLocalGapAbs = absGapDiffLocal;
            bestLocalPW     = testPW;
            chosenGap       = gapLocal;
          }
        }
      }

      chosenPW = bestLocalPW;
      double chosenDutyPercent = 0.0;
      if (periodUs > 0.0) {
        chosenDutyPercent = (0.5 + chosenGap / (2.0 * periodUs)) * 100.0;
      }

      if (autotuneDebug >= 1) {
        Serial.println((String)"[PW_CENTER_RESULT] note=" + DCO_calibration_current_note +
                       (String)" DCO=" + currentDCO +
                       (String)" PW_center=" + chosenPW +
                       (String)" duty≈" + chosenDutyPercent +
                       (String)"% bestGap=" + bestLocalGapAbs +
                       (String)"us inTolSamples=" + inTolForThisPass +
                       (String)" totalValid=" + st.validCount);
      }
      chosenPWOut = chosenPW;
      return true;
    }

    // This candidate failed lock-in; inflate its gap diff so we try the next
    // best one on the following attempt.
    st.validGapDiff[bestIdx] = targetGap * 20.0;
    if (autotuneDebug >= 1) {
      Serial.println((String)"[PW_CENTER_LOCKIN_REJECT] note=" + DCO_calibration_current_note +
                     (String)" DCO=" + currentDCO +
                     (String)" PW=" + chosenPW +
                     (String)" could not get 3 consecutive in-band readings; trying next candidate.");
    }
  }

  if (autotuneDebug >= 1) {
    Serial.println((String)"[PW_CENTER_ABORT] note=" + DCO_calibration_current_note +
                   (String)" DCO=" + currentDCO +
                   (String)" all candidates failed lock-in; keeping PW_center=" +
                   PW_CENTER[cal_pw_channel(currentDCO)]);
  }
  return false;
}

// Shared search routine used by PW calibration (currently the center search).
// It looks for the PW value whose duty cycle is closest to targetDutyFraction
// at the current calibration note. targetGap is the allowed absolute gap (in
// microseconds) from the ideal duty at that note. On failure the caller's
// fallbackPW is returned unchanged.
//
// Phases: coarse scan → bisection (bracket) or local fine scan (no bracket)
// → candidate selection with lock-in and local refinement.
static uint16_t find_PW_for_target_duty(double targetDutyFraction,
                                        uint16_t targetGap,
                                        uint16_t pwMin,
                                        uint16_t pwMax,
                                        uint16_t fallbackPW) {

  double freqHz   = (double)note_to_freq(DCO_calibration_current_note);
  double periodUs = (freqHz > 0.0) ? (1000000.0 / freqHz) : 0.0;

  // Update global logging context for gap measurements during this search.
  g_gapLogCurrentPeriodUs    = periodUs;
  g_gapLogTargetDutyFraction = targetDutyFraction;

  double toleranceDutyPercent = 0.0;
  double gapTarget = 0.0;
  if (periodUs > 0.0) {
    // Ideal gap for a target HIGH-duty p: gap = avgHigh - avgLow = T*(2p - 1).
    // (Zero for the 50% center target, positive above, negative below.)
    gapTarget = periodUs * (2.0 * targetDutyFraction - 1.0);
    toleranceDutyPercent = ((double)targetGap / (2.0 * periodUs)) * 100.0;
  }

  // Coarse step: use smaller steps for low/high limit searches (target duty
  // far from 50%) and larger steps for the center search.
  uint16_t coarseDiv  = (fabs(targetDutyFraction - 0.5) < 0.05) ? 16 : 32;
  uint16_t coarseStep = (pwMax > pwMin) ? ((pwMax - pwMin) / coarseDiv) : 1;
  if (coarseStep == 0) coarseStep = 1;

  PWSearchState st;
  pw_search_state_init(st);

  pw_coarse_scan(st, gapTarget, (double)targetGap, pwMin, pwMax, coarseStep,
                 periodUs, toleranceDutyPercent);

  if (st.haveBracket) {
    pw_bisect_bracket(st, gapTarget, (double)targetGap, periodUs, toleranceDutyPercent);
  } else {
    pw_fine_scan_around_best(st, gapTarget, (double)targetGap, pwMin, pwMax, coarseStep);
  }

  if (calibrationCancelRequested) {
    return fallbackPW;
  }

  if (st.validCount == 0) {
    // No valid samples at all in the searched range: keep the caller's PW
    // and log the situation so the user can investigate.
    if (autotuneDebug >= 1) {
      Serial.println("PW search: no valid samples found; keeping current PW.");
    }
    return fallbackPW;
  }

  uint16_t chosenPW = fallbackPW;
  if (pw_select_and_lock(st, gapTarget, (double)targetGap, pwMin, pwMax,
                         periodUs, chosenPW)) {
    return chosenPW;
  }
  return fallbackPW;
}

// Locate PW center for the current DCO's voice by minimizing duty-cycle error
// at a reference note. Mode 0 = low note, mode 1 = higher note refinement.
void find_PW_center(uint8_t mode) {

  DCO_calibration_current_note = manual_DCO_calibration_start_note;
  VOICE_NOTES[0] = DCO_calibration_current_note;
  ampCompCalibrationVal = initManualAmpCompCalibrationVal[currentDCO] + manualCalibrationOffset[currentDCO];

  uint16_t targetGap;
  uint8_t voiceTaskMode;

  if (mode == 0) {
    targetGap = compute_gap_tolerance_for_freq(note_to_freq(DCO_calibration_current_note), 0.005);
    voiceTaskMode = 2;
  } else {
    DCO_calibration_current_note = 76;
    VOICE_NOTES[0] = DCO_calibration_current_note;
    targetGap = 5;
    voiceTaskMode = 3;
  }

  DCOCalibrationStart = millis();

  const uint8_t pwCh = cal_pw_channel(currentDCO);
  if (PW_PINS[pwCh] == PW_PIN_UNASSIGNED) {
    Serial.println((String)"[DCO_CAL] PW channel " + pwCh +
                   " unassigned; skipping center search for DCO=" + currentDCO);
    return;
  }

  // Starting PW: middle of the range on the very first tune, otherwise the
  // previously stored center.
  if (firstTuneFlag == true) {
    PW[pwCh] = DIV_COUNTER_PW / 2;
    PW_CENTER[pwCh] = DIV_COUNTER_PW / 2;
  } else {
    PW[pwCh] = PW_CENTER[pwCh];
  }
  uint16_t startPW = PW[pwCh];

  // Apply the starting PW to the PW PWM channel before configuring the DCO.
  pwm_set_chan_level(PW_PWM_SLICES[pwCh], pwm_gpio_to_channel(PW_PINS[pwCh]), startPW);

  voice_task_autotune(voiceTaskMode, ampCompCalibrationVal);

  uint16_t centerPW = find_PW_for_target_duty(
    kPWCenterDutyFraction,
    targetGap,
    0,
    DIV_COUNTER_PW,
    startPW
  );
  if (calibrationCancelRequested) {
    Serial.println("[DCO_CAL] PW center search interrupted; keeping previous center");
    return;
  }
  Serial.println("PW center found !!!");
  update_FS_PWCenter(pwCh, centerPW);
  PW_CENTER[pwCh] = centerPW;

  // Apply the newly found PW center immediately to the hardware so that the
  // effect is visible on the pulse waveform as soon as calibration finishes.
  pwm_set_chan_level(PW_PWM_SLICES[pwCh],
                     pwm_gpio_to_channel(PW_PINS[pwCh]),
                     centerPW);
  PW[pwCh] = centerPW;
}


// -----------------------------------------------------------------------------
// PW limit search
// -----------------------------------------------------------------------------

PWLimitSearchResult search_PW_limit_from_center(
  uint8_t     voiceIdx,
  uint16_t    centerPW,
  PWLimitDir  dir,
  double      periodUs,
  double      targetDuty
) {
  PWLimitSearchResult result;
  result.ok                  = false;
  result.limitPW             = centerPW;
  result.finalDutyPercent    = -1.0;

  if (periodUs <= 0.0) {
    return result;
  }

  // Hard bounds convention:
  //  - LOW  side scans from center down to 0
  //  - HIGH side scans from center up to DIV_COUNTER_PW
  uint16_t minPW = (dir == PW_LIMIT_LOW)  ? 0           : centerPW;
  uint16_t maxPW = (dir == PW_LIMIT_LOW)  ? centerPW    : DIV_COUNTER_PW;

  // Coarse step size for scanning from center toward the limit.
  uint16_t step = DIV_COUNTER_PW / 64;
  if (step == 0) step = 1;

  bool     haveBest   = false;
  uint16_t bestPW     = centerPW;
  double   bestDelta  = 1e12;
  double   bestDuty   = -1.0;   // duty (0..1) at bestPW when known

  unsigned long searchStartMs = millis();

  // Coarse scan: walk from center toward the requested side, tracking the
  // PW that gets closest to the target duty. We stop when we reach the
  // boundary, run out of time, or find a value within tolerance.
  for (uint16_t pw = centerPW; ; ) {
    if (calibrationCancelRequested) break;
    if (millis() - searchStartMs > 60000UL) {
      // Safety timeout.
      break;
    }

    if (pw < minPW) pw = minPW;
    if (pw > maxPW) pw = maxPW;

    GapMeasurement gm = set_pw_and_measure(voiceIdx, pw);
    if (!gm.timedOut) {
      double gap           = (double)gm.value;
      double dutyErrorFrac = gap / (2.0 * periodUs);
      double duty          = 0.5 + dutyErrorFrac;

      double delta = fabs(duty - targetDuty);
      if (!haveBest || delta < bestDelta) {
        haveBest  = true;
        bestDelta = delta;
        bestPW    = pw;
        bestDuty  = duty;
      }

      if (autotuneDebug >= 2) {
        const char *scanTag =
          (dir == PW_LIMIT_LOW) ? "[PW_LOW_SCAN_V2]" : "[PW_HIGH_SCAN_V2]";
        Serial.println((String)scanTag +
                       (String)" note=" + DCO_calibration_current_note +
                       (String)" DCO=" + currentDCO +
                       (String)" PW_raw=" + pw +
                       (String)" duty=" + (duty * 100.0) + "%" +
                       (String)" targetDuty=" + (targetDuty * 100.0) + "%");
      }

      // If we are already within tolerance, we can stop the coarse scan early.
      if (delta <= kPWLimitDutyTolerance) {
        break;
      }
    }

    // Step toward the boundary.
    if (dir == PW_LIMIT_LOW) {
      if (pw <= minPW + step) {
        break;
      }
      pw = (uint16_t)(pw - step);
    } else {  // PW_LIMIT_HIGH
      if (pw >= maxPW - step) {
        break;
      }
      pw = (uint16_t)(pw + step);
    }
  }

  if (!haveBest) {
    // Never saw a valid measurement; caller should keep previous limit.
    return result;
  }

  // Fine refinement around bestPW: search with step = 1 in a relatively
  // tight window around the best coarse candidate. This keeps the search
  // local so we do not wander too far from the best-known PW.
  uint16_t refineRadius = step / 2;
  if (refineRadius < 4)  refineRadius = 4;
  if (refineRadius > 32) refineRadius = 32;

  uint16_t startPW;
  if (bestPW > refineRadius) {
    startPW = bestPW - refineRadius;
  } else {
    startPW = minPW;
  }
  // Enforce the same [minPW, maxPW] bounds used in the coarse scan so that
  // the refinement phase never crosses to the other side of center.
  if (startPW < minPW) startPW = minPW;

  uint16_t endPW = bestPW + refineRadius;
  if (endPW > maxPW) {
    endPW = maxPW;
  }

  int consecutiveTimeouts = 0;
  for (uint16_t pw = startPW; pw <= endPW; ++pw) {
    if (calibrationCancelRequested) break;
    GapMeasurement gm = set_pw_and_measure(voiceIdx, pw);
    if (gm.timedOut) {
      // If we are stepping deeper into the "edge" side and accumulate several
      // consecutive timeouts, stop refining in that direction to avoid
      // spending a long time in a region with no measurable signal.
      ++consecutiveTimeouts;
      bool goingDeeperLow  = (dir == PW_LIMIT_LOW)  && (pw < bestPW);
      bool goingDeeperHigh = (dir == PW_LIMIT_HIGH) && (pw > bestPW);
      if ((goingDeeperLow || goingDeeperHigh) && consecutiveTimeouts >= 4) {
        break;
      }
      continue;
    }
    consecutiveTimeouts = 0;

    double gap           = (double)gm.value;
    double dutyErrorFrac = gap / (2.0 * periodUs);
    double duty          = 0.5 + dutyErrorFrac;

    double delta = fabs(duty - targetDuty);
    if (delta < bestDelta) {
      bestDelta = delta;
      bestPW    = pw;
      bestDuty  = duty;
    }
  }

  // Final result: start from the best sample seen during coarse+fine.
  result.ok      = true;
  result.limitPW = bestPW;

  if (bestDuty >= 0.0) {
    result.finalDutyPercent = bestDuty * 100.0;
  }

  // Check whether the target duty is actually reachable within tolerance.
  double currentDutyFrac = result.finalDutyPercent / 100.0;
  if (result.finalDutyPercent <= 0.0 ||
      fabs(currentDutyFrac - targetDuty) > kPWLimitDutyTolerance) {
    // Not within tolerance: push all the way to the hardware boundary for
    // this side and treat that as the "best possible" limit. This matches
    // the specification that the target is considered unreachable only after
    // trying the maximum/minimum PW value.
    uint16_t boundaryPW = (dir == PW_LIMIT_LOW) ? minPW : maxPW;

    GapMeasurement gmEdge = set_pw_and_measure(voiceIdx, boundaryPW);
    if (!gmEdge.timedOut) {
      double gap           = (double)gmEdge.value;
      double dutyErrorFrac = gap / (2.0 * periodUs);
      double duty          = 0.5 + dutyErrorFrac;
      result.limitPW        = boundaryPW;
      result.finalDutyPercent = duty * 100.0;
    } else {
      // If even the boundary cannot be measured reliably, we still honour the
      // boundary PW as the limit but leave finalDutyPercent as-is.
      result.limitPW = boundaryPW;
    }
  }

  return result;
}

void find_PW_limit_v2(PWLimitDir dir) {
  uint8_t voiceTaskMode = 2;

  // Configure the calibration context the same way as the PW center search
  // so that both phases operate on the same note and amplitude.
  DCO_calibration_current_note = manual_DCO_calibration_start_note;
  VOICE_NOTES[0] = DCO_calibration_current_note;
  ampCompCalibrationVal =
    initManualAmpCompCalibrationVal[currentDCO] + manualCalibrationOffset[currentDCO];

  DCOCalibrationStart = millis();

  double freqHz   = (double)note_to_freq(DCO_calibration_current_note);
  double periodUs = (freqHz > 0.0) ? (1000000.0 / freqHz) : 0.0;

  uint8_t  voiceIdx = cal_pw_channel(currentDCO);
  uint16_t centerPW = PW_CENTER[voiceIdx];

  // Direction-dependent target HIGH-duty:
  //  - Low limit:  kPWLowDutyFraction  (≈ 2% HIGH)
  //  - High limit: kPWHighDutyFraction (≈98% HIGH)
  double targetDuty = (dir == PW_LIMIT_LOW)
                      ? kPWLowDutyFraction
                      : kPWHighDutyFraction;

  // Update global logging context for gap measurements during PW-limit search.
  g_gapLogCurrentPeriodUs    = periodUs;
  g_gapLogTargetDutyFraction = targetDuty;

  // Configure the DCO for PW calibration mode.
  voice_task_autotune(voiceTaskMode, ampCompCalibrationVal);
  delay(100);

  PWLimitSearchResult res =
    search_PW_limit_from_center(voiceIdx, centerPW, dir, periodUs, targetDuty);

  if (calibrationCancelRequested) {
    Serial.println((String)"[DCO_CAL] PW " +
                   ((dir == PW_LIMIT_LOW) ? "low" : "high") +
                   " limit search interrupted; keeping previous limit");
    return;
  }

  if (!res.ok) {
    if (autotuneDebug >= 1) {
      const char *abortTag =
        (dir == PW_LIMIT_LOW) ? "[PW_LOW_ABORT_NO_SIGNAL_V2]" : "[PW_HIGH_ABORT_NO_SIGNAL_V2]";
      uint16_t keepPW =
        (dir == PW_LIMIT_LOW) ? PW_LOW_LIMIT[voiceIdx] : PW_HIGH_LIMIT[voiceIdx];
      Serial.println((String)abortTag +
                     (String)" note=" + DCO_calibration_current_note +
                     (String)" DCO=" + currentDCO +
                     (String)" keeping_PW=" + keepPW);
    }
    return;
  }

  // Log result and commit it.
  double targetDutyPercent =
    (dir == PW_LIMIT_LOW)
      ? (kPWLowDutyFraction * 100.0)
      : ((1.0 - kPWHighDutyFraction) * 100.0);
  double targetHighDutyPercent = kPWHighDutyFraction * 100.0;

  if (autotuneDebug >= 1) {
    const char *resultTag =
      (dir == PW_LIMIT_LOW) ? "[PW_LOW_RESULT_V2]" : "[PW_HIGH_RESULT_V2]";
    Serial.println((String)resultTag +
                   (String)" note=" + DCO_calibration_current_note +
                   (String)" DCO=" + currentDCO +
                   (String)" PW_LIMIT=" + res.limitPW +
                   (String)" duty≈" + res.finalDutyPercent + "%" +
                   (String)" targetDuty=" + targetDutyPercent + "%" +
                   (dir == PW_LIMIT_LOW
                      ? (String)""
                      : (String)" targetHighDuty=" + targetHighDutyPercent + "%"));
  }

  if (dir == PW_LIMIT_LOW) {
    Serial.println("--------------------------------");
    Serial.println("PW low limit (v2) found !!!");
    Serial.println(
      (String)" PW_LIMIT=" + res.limitPW +
      (String)" duty≈" + res.finalDutyPercent + "%" +
      (String)" targetDuty=" + (kPWLowDutyFraction * 100.0) + "%");
    Serial.println("--------------------------------");
    update_FS_PW_Low_Limit(voiceIdx, res.limitPW);
    PW_LOW_LIMIT[voiceIdx] = res.limitPW;
  } else {
    Serial.println("--------------------------------");
    Serial.println("PW high limit (v2) found !!!");
    Serial.println(
      (String)" PW_LIMIT=" + res.limitPW +
      (String)" duty≈" + res.finalDutyPercent + "%" +
      (String)" targetDuty=" + ((1.0 - kPWHighDutyFraction) * 100.0) + "%" +
      (String)" targetHighDuty=" + (kPWHighDutyFraction * 100.0) + "%");
    Serial.println("--------------------------------");
    update_FS_PW_High_Limit(voiceIdx, res.limitPW);
    PW_HIGH_LIMIT[voiceIdx] = res.limitPW;
  }
}

//////////////////////////////////////////////////////////////////////////////
// Raw cal-sense probe (no period gate): sample digital level / edge rate so a
// TIMEOUT can be split into "pin stuck" vs "edges exist but find_gap rejects".
// Throttled to ~2 Hz. Called from DCO_calibration_debug on gap timeout.
static void cal_sense_probe_log() {
  static uint32_t lastPrintMs = 0;
  const uint32_t nowMs = millis();
  if ((nowMs - lastPrintMs) < 500u) {
    return;
  }
  lastPrintMs = nowMs;

  constexpr uint32_t kWindowUs = 40000u;  // 40 ms
  const uint32_t t0 = micros();
  bool lastRaw = digitalRead(DCO_calibration_pin);
  uint32_t edges = 0;
  uint32_t minDt = 0xFFFFFFFFu;
  uint32_t maxDt = 0;
  uint32_t lastEdgeUs = t0;
  bool haveEdge = false;

  while ((micros() - t0) < kWindowUs) {
    const bool raw = digitalRead(DCO_calibration_pin);
    if (raw != lastRaw) {
      const uint32_t nowUs = micros();
      const uint32_t dt = nowUs - lastEdgeUs;
      if (haveEdge) {
        if (dt < minDt) {
          minDt = dt;
        }
        if (dt > maxDt) {
          maxDt = dt;
        }
      }
      lastEdgeUs = nowUs;
      haveEdge = true;
      edges++;
      lastRaw = raw;
    }
  }

  const bool rawNow = digitalRead(DCO_calibration_pin);
  double expectHz = 0.0;
  if (DCO_calibration_current_note >= 12) {
    expectHz = (double)note_to_freq(DCO_calibration_current_note);
  }

  Serial.print((String)"[CAL_SENSE] pin=" + DCO_calibration_pin +
               (String)" raw=" + (int)rawNow +
               (String)" edges=" + edges);
  if (edges >= 2 && minDt != 0xFFFFFFFFu) {
    Serial.print((String)" minDt=" + minDt + (String)" maxDt=" + maxDt);
  } else {
    Serial.print(" minDt=- maxDt=-");
  }
  Serial.println((String)" pullup=1 invert=" + (int)kGapPolarityInverted +
                 (String)" note=" + DCO_calibration_current_note +
                 (String)" expectHz≈" + expectHz);
}

//////////////////////////////////////////////////////////////////////////////
// Measure duty-cycle error on DCO_calibration_pin by timing rising/falling
// edges. Returns avgHighUs - avgLowUs (0 when duty is ≈50%), or
// kGapTimeoutSentinel on timeout. All measurement state is local; callers
// normally use the measure_gap() wrapper from autotune_measurement.h.
float find_gap(byte specialMode) {
  // Estimate the ideal period so we can reject obviously invalid edge
  // intervals (e.g. very short glitches) that do not match the DCO's actual
  // frequency. When gapGateFreqHz is set (arbitrary-frequency probes), gate
  // against the probe frequency instead of the current calibration note.
  double freqHz = (gapGateFreqHz > 0.0f)
                    ? (double)gapGateFreqHz
                    : (double)note_to_freq(DCO_calibration_current_note);
  double idealPeriodUs = (freqHz > 0.0) ? (1000000.0 / freqHz) : 0.0;

  // Number of accepted low/high segments per measurement. Mode 2 (PW search)
  // and mode 3 (FREQ_TRACE frequency probe) average over the precision
  // profile's time window instead, so the segment count scales with the
  // frequency and with how careful this run is meant to be.
  uint16_t samplesTarget = kGapSamplesDefault;
  if (specialMode == 2 || specialMode == 3) {
    const CalPrecisionProfile &prec = cal_precision();
    samplesTarget = prec.gapSamplesMin;
    const double halfPeriodMs = idealPeriodUs / 2000.0;  // one segment
    if (halfPeriodMs > 0.0) {
      long n = lround((double)prec.gapWindowMs / halfPeriodMs);
      if (n > (long)prec.gapSamplesMax) n = (long)prec.gapSamplesMax;
      if (n > (long)samplesTarget)      samplesTarget = (uint16_t)n;
      // Below ~30 Hz the window fits nothing, so without a higher floor a
      // reading down there averages only the profile's minimum segments and
      // its noise (~±0.2% duty at 20 Hz) becomes the accuracy limit of the
      // lowest measured pair. Raise the floor; the gapMaxWindowMs cap below
      // still bounds it, so the amp-0 scan around 8 Hz does not slow down.
      if (freqHz < (double)kSearchStepVeryLowHz &&
          samplesTarget < kGapSamplesVeryLowMin) {
        samplesTarget = kGapSamplesVeryLowMin;
      }
      // Bound how long one reading may take. Without this the segment floor
      // governs at the bottom of the range, where a segment is tens of
      // milliseconds, and every reading below ~27 Hz costs more than the
      // window asked for - a fine run spends most of its time down there.
      // Four segments is the least that still averages a rising and a falling
      // one of each polarity.
      long cap = lround((double)prec.gapMaxWindowMs / halfPeriodMs);
      if (cap < 4) cap = 4;
      if ((long)samplesTarget > cap) samplesTarget = (uint16_t)cap;
    }
  }
  // The deadline for one edge, and with it the longest segment that can be part
  // of a waveform rather than evidence of a dead one. See kGapTimeoutUs: at the
  // bottom of the range a single period outlasts the fixed 100 ms.
  unsigned long timeoutUs = kGapTimeoutUs;
  if (idealPeriodUs > 0.0) {
    const double scaled = idealPeriodUs * kGapTimeoutPeriods;
    if (scaled > (double)timeoutUs) {
      timeoutUs = (scaled > (double)kGapTimeoutMaxUs) ? kGapTimeoutMaxUs
                                                      : (unsigned long)scaled;
    }
  }

  double dtMinUs = 0.0;
  double dtMaxUs = 0.0;
  if (idealPeriodUs > 0.0) {
    // Accept any segment between ~1% and ~99% of the ideal period. This covers
    // extreme duty cycles (2%/98%) while rejecting very short/high-frequency
    // glitches that are clearly not the fundamental.
    dtMinUs = idealPeriodUs * 0.01;
    dtMaxUs = idealPeriodUs * 0.99;
    if (dtMinUs < (double)kEdgeDebounceMinUs) {
      dtMinUs = (double)kEdgeDebounceMinUs;
    }
    if (dtMaxUs > (double)timeoutUs) {
      dtMaxUs = (double)timeoutUs;
    }
  }

  // Segments are timed in CPU cycles, not microseconds. What this function
  // measures is the difference between two segment lengths, and micros() (a 1 us
  // timer) quantises each edge to 1 us: at 4 kHz a period is 250 us, so a single
  // count of quantisation is already 0.2% of duty - ten times the tolerance a
  // fine run asks for, and irreducible by averaging since the timer is what is
  // coarse, not the waveform. The cycle counter is the SysTick counter the core
  // already runs, extended to 32 bits (rp2040.getCycleCount()); at 125 MHz that
  // is 8 ns, so one reading resolves what micros() could not reach at all.
  // The timeout stays on micros(): 100 ms does not need 8 ns. The extension is
  // not trusted for segments longer than one 24-bit wrap - see the wrap
  // reconstruction at the edge handler below.
  const uint32_t cyclesPerUs   = (uint32_t)(rp2040.f_cpu() / 1000000);
  const double   usPerCycle    = 1.0 / (double)cyclesPerUs;
  const uint32_t debounceCycles = (uint32_t)kEdgeDebounceMinUs * cyclesPerUs;
  const uint32_t dtMinCycles    = (uint32_t)(dtMinUs * (double)cyclesPerUs);
  const uint32_t dtMaxCycles    = (uint32_t)(dtMaxUs * (double)cyclesPerUs);

  // Local edge-timing state (was global before the cleanup).
  int      pulseCount        = 0;
  uint16_t acceptedSamples   = 0;
  uint64_t risingSumCycles   = 0;
  uint64_t fallingSumCycles  = 0;
  bool     lastVal           = 0;
  uint16_t risingCount       = 0;
  uint16_t fallingCount      = 0;
  // Diagnostics: debounced edges vs period-gate rejects (TIMEOUT localization).
  uint16_t edgesSeen     = 0;
  uint16_t edgesRejected = 0;

  unsigned long lastEdgeTime   = micros();
  uint32_t      lastEdgeCycles = rp2040.getCycleCount();
  // How late an edge is noticed is one pass of this loop, so the loop carries as
  // little as possible: a register read of the pin and nothing else. The timeout
  // is checked once every 64 passes instead of on every one, which is still far
  // more often than a 100 ms deadline needs.
  uint8_t pollTick = 0;

  while (acceptedSamples < samplesTarget) {

    const bool rawVal = (bool)gpio_get(DCO_calibration_pin);
    // Compensate for hardware polarity if needed so that 'val == 1' always
    // represents the same logical DCO level for duty measurements.
    const bool val = kGapPolarityInverted ? !rawVal : rawVal;

    if (val == lastVal && ((++pollTick & 0x3F) != 0)) {
      continue;  // nothing happened; not time to look at the clock either
    }

    const uint32_t nowCycles = rp2040.getCycleCount();
    const unsigned long nowUs = micros();

    if ((nowUs - lastEdgeTime) > timeoutUs) {
      const bool rawAtTimeout = digitalRead(DCO_calibration_pin);

      // Manual cal: log at debug >= 1. Auto-cal keeps the quieter >= 3 threshold.
      if (autotuneDebug >= 3 || (manualCalibrationFlag && autotuneDebug >= 1)) {
        Serial.println((String)"[GAP_TIMEOUT] note=" + DCO_calibration_current_note +
                       (String)" freq=" + fmt_freq((float)freqHz) +
                       (String)" DCO=" + currentDCO +
                       (String)" raw=" + (int)rawAtTimeout +
                       (String)" edges=" + edgesSeen +
                       (String)" rejected=" + edgesRejected +
                       (String)" accepted=" + acceptedSamples +
                       (String)" TidealUs≈" + (uint32_t)idealPeriodUs +
                       (String)" timeoutUs=" + (uint32_t)timeoutUs +
                       (String)" PW_raw=" + pw_level_readback(cal_pw_channel(currentDCO)) +
                       (String)" ampComp=" + ampCompCalibrationVal);
      }

      return kGapTimeoutSentinel;
    }

    if (val != lastVal) {
      // The cycle counter is the 24-bit SysTick, and on this core its 32-bit
      // software extension does not advance: a segment longer than 2^24 cycles
      // (67 ms at 250 MHz) comes back short by whole wraps. That is how the
      // amp-0 hunt once rejected every reading of a waveform the scope showed
      // was clean - below ~7.5 Hz both halves outlast a wrap, and the mangled
      // period failed the off-period gate at every probe. The wall clock
      // (micros(), already read for the timeout) recovers the lost wraps: it
      // picks the multiple of 2^24, the counter keeps the fine 4 ns bits. The
      // low 24 bits of the delta are wrap-proof by construction, and micros()
      // jitter is four orders of magnitude below half a wrap, so the rounded
      // wrap count cannot come out wrong. The longest correctable segment is
      // the 400 ms gap deadline = 100 M cycles, comfortably inside uint32.
      const uint32_t fine24     = (nowCycles - lastEdgeCycles) & 0x00FFFFFFu;
      const uint64_t wallCycles = (uint64_t)(nowUs - lastEdgeTime) * cyclesPerUs;
      const int64_t  lostWraps  =
        ((int64_t)wallCycles - (int64_t)fine24 + (int64_t)(1u << 23)) >> 24;
      const uint32_t dtCycles =
        (lostWraps > 0) ? (fine24 + (uint32_t)((uint64_t)lostWraps << 24))
                        : fine24;

      if (dtCycles >= debounceCycles) {

        lastVal = val;
        edgesSeen++;

        // Re-align so counting starts on a rising edge.
        if (pulseCount == 1 && val == 0) {
          pulseCount = 0;
        }
        if (pulseCount > 2) {
          const uint32_t dt = dtCycles;  // cycles
          bool intervalOk = true;
          if (idealPeriodUs > 0.0) {
            // Reject intervals that are incompatible with the ideal period.
            // This prevents very short spurious edges from corrupting the
            // duty measurement at low frequencies.
            if (dt < dtMinCycles || dt > dtMaxCycles) {
              intervalOk = false;
            }
          }

          // NOTE: segment attribution follows the legacy convention (the
          // segment ending on a falling edge goes into the "falling" sum).
          // The overall sign chain (kGapPolarityInverted here plus the flip
          // in measure_gap_for_amp) is field-validated; keep them in sync if
          // this is ever changed.
          if (intervalOk) {
            if (val == 0) {
              fallingSumCycles += dt;
              fallingCount++;
            } else {
              risingSumCycles += dt;
              risingCount++;
            }
            acceptedSamples++;
          } else {
            edgesRejected++;
          }
        }
        lastEdgeTime   = nowUs;
        lastEdgeCycles = nowCycles;
        pulseCount++;
      }
    }
  }

  // A reading where only one polarity of segment survived the gate is not a
  // duty measurement: the duty is pegged at 0% or 100% (the other side's blips
  // were shorter than the segment floor) and avgHigh - avgLow then measures the
  // blips, not the waveform - at the bottom of the range that number is tiny
  // against the ideal period and converts to a duty error near zero, which is
  // how a pegged waveform once scored -0.72% and closed a fake amp-0 bracket.
  // Only the frequency-search probes (mode 3) get the sentinel: the classic
  // search and the PW limit search read extreme duties on purpose.
  if (specialMode == 3 && (risingCount == 0 || fallingCount == 0)) {
    if (autotuneDebug >= 2) {
      Serial.println((String)"[GAP_ONESIDED] freq=" + fmt_freq((float)freqHz) +
                     (String)" DCO=" + currentDCO +
                     (String)" highs=" + risingCount +
                     (String)" lows=" + fallingCount +
                     (String)" duty pegged; reading discarded");
    }
    return kGapTimeoutSentinel;
  }

  // Compute average low and high segment durations directly from the number
  // of segments we actually accumulated. Averaging in cycles and converting
  // once keeps the 8 ns resolution all the way to the result.
  float avgLowUs  = (fallingCount > 0)
                      ? (float)((double)fallingSumCycles * usPerCycle / (double)fallingCount)
                      : 0.0f;
  float avgHighUs = (risingCount > 0)
                      ? (float)((double)risingSumCycles * usPerCycle / (double)risingCount)
                      : 0.0f;

  // Derived period and direct HIGH-duty estimate based purely on measured
  // low/high portions (duty cycle = fraction of the period spent HIGH).
  float measuredPeriodUs = avgLowUs + avgHighUs;
  float dutyMeasuredFrac = (measuredPeriodUs > 0.0f) ? (avgHighUs / measuredPeriodUs) : 0.0f;

  // A reading whose segments do not sum to the requested period is not a duty
  // measurement of the requested waveform: near the bottom of the range at
  // amp comp 0 the pin can toggle roughly twice per requested cycle, and those
  // near-symmetric sub-segments read ~50% duty at any frequency - which is how
  // the amp-0 search once chased a fake 50% crossing the scope could not see.
  // Mode 3 only, like the one-sided rule above and for the same reason.
  if (specialMode == 3 && idealPeriodUs > 0.0 &&
      fabsf(measuredPeriodUs - (float)idealPeriodUs) >
        kGapPeriodTolRatio * (float)idealPeriodUs) {
    if (autotuneDebug >= 2) {
      Serial.println((String)"[GAP_OFFPERIOD] freq=" + fmt_freq((float)freqHz) +
                     (String)" DCO=" + currentDCO +
                     (String)" Tmeas=" + measuredPeriodUs +
                     (String)" Tideal=" + (float)idealPeriodUs +
                     (String)" not the requested waveform; reading discarded");
    }
    return kGapTimeoutSentinel;
  }

  // Positive result means the HIGH segment is longer than LOW (duty > 50%);
  // negative means LOW is longer (duty < 50%). This keeps the relation:
  //   duty_high - 0.5 = diff / (2 * periodUs)
  float diffUs = avgHighUs - avgLowUs;

  if (autotuneDebug >= 2) {
    // Log raw gap measurement with context: which mode, note/DCO, the
    // current amplitude compensation value, the last PW we explicitly set,
    // and the inferred duty/target duty if a period is available. freq= is the
    // frequency actually being driven, which during an arbitrary-frequency
    // probe has nothing to do with note= (that one is left at whatever note
    // the calibration last set).

    // Duty estimate using the same "diff vs ideal period" method used by
    // the PW search code.
    double dutyPercentIdeal = 0.0;
    double targetDutyPercent = g_gapLogTargetDutyFraction * 100.0;
    if (g_gapLogCurrentPeriodUs > 0.0) {
      double dutyErrorFrac = (double)diffUs / (2.0 * g_gapLogCurrentPeriodUs);
      dutyPercentIdeal = (0.5 + dutyErrorFrac) * 100.0;
    }

    // Direct duty estimate based only on measured low/high times.
    double dutyPercentMeasured = dutyMeasuredFrac * 100.0;

    Serial.println((String)"[GAP_MEASURE] mode=" + specialMode +
                   (String)" note=" + DCO_calibration_current_note +
                   (String)" freq=" + fmt_freq((float)freqHz) +
                   (String)" DCO=" + currentDCO +
                   (String)" AMP=" + ampCompCalibrationVal +
                   (String)" PW_raw=" + pw_level_readback(cal_pw_channel(currentDCO)) +
                   (String)" diff=" + diffUs +
                   (String)" avgLowUs=" + avgLowUs +
                   (String)" avgHighUs=" + avgHighUs +
                   (String)" T_meas=" + measuredPeriodUs +
                   (String)" duty_meas≈" + dutyPercentMeasured + "%" +
                   (String)" duty_ideal≈" + dutyPercentIdeal + "%" +
                   (String)" targetDuty=" + targetDutyPercent + "%");
  }

  return diffUs;
}

/*************************************************************************************/
/*************************************************************************************/
/*************************************************************************************/

// --- PW CV probe ------------------------------------------------------------

// PW raw levels walked by the probe: both rails plus three points across the
// span, so a comparator that only reacts near its center still shows movement.
static const uint16_t kPWProbeLevels[] = {
  0, DIV_COUNTER_PW / 4, DIV_COUNTER_PW / 2,
  (DIV_COUNTER_PW * 3) / 4, DIV_COUNTER_PW - 1
};

// Duty span (percentage points) above which a channel counts as having moved
// the pulse. Measurement noise on a good board is a fraction of a point.
static constexpr float kPWProbeMovedPct = 5.0f;

// Prove whether a PW CV write reaches the pulse comparator at all, and whether
// it reaches the voice the firmware believes it does (PARAM_DEBUG_COMMAND 46,
// run from loop1 while manual calibration is active).
//
// Manual cal is required because only then is a single oscillator soloed onto
// the cal-sense pin: the duty measured there is the only witness that the CV
// arrived. Every PW channel is walked, not just the calibrated one, so a duty
// that follows some other channel means PW_PINS does not match the wiring,
// while a duty that follows nothing means there is no CV path to this
// oscillator's pulse and no firmware change can mute it.
//
// Leaves PW clobbered on purpose: the next manual-cal pass in loop1 rewrites
// every channel from the current substage.
void run_pw_cv_probe() {
  const uint8_t osc       = cal_manual_osc();
  const uint8_t expectCh  = cal_pw_channel(osc);
  const double  freqHz    = (double)note_to_freq(DCO_calibration_current_note);
  const double  periodUs  = (freqHz > 0.0) ? (1000000.0 / freqHz) : 0.0;

  Serial.println((String)"[PW_PROBE] start: osc=" + osc +
                 " expected ch=" + expectCh +
                 " (pin GP" + PW_PINS[expectCh] + ")" +
                 " stage=" + manualCalibrationStage +
                 " note=" + DCO_calibration_current_note +
                 " freq=" + fmt_freq((float)freqHz));

  if (periodUs <= 0.0) {
    Serial.println("[PW_PROBE] no note driven; nothing to measure");
    return;
  }

  uint8_t bestCh    = 0;
  float   bestSpan  = -1.0f;
  float   expectSpan = 0.0f;
  bool    anyRead   = false;

  for (uint8_t ch = 0; ch < NUM_PW_CHANNELS; ++ch) {
    if (PW_PINS[ch] == PW_PIN_UNASSIGNED) {
      Serial.println((String)"[PW_PROBE] ch=" + ch + " not wired, skipped");
      continue;
    }

    // Only the channel under test carries a CV, so a duty that moves anyway
    // belongs to whatever channel is actually feeding this oscillator.
    for (uint8_t z = 0; z < NUM_PW_CHANNELS; ++z) {
      if (z != ch && PW_PINS[z] != PW_PIN_UNASSIGNED) {
        pwm_set_chan_level(PW_PWM_SLICES[z], pwm_gpio_to_channel(PW_PINS[z]), 0);
        PW[z] = 0;
      }
    }

    float dutyMin = 0.0f, dutyMax = 0.0f;
    uint8_t reads = 0;
    const uint8_t levels = (uint8_t)(sizeof(kPWProbeLevels) / sizeof(kPWProbeLevels[0]));

    for (uint8_t li = 0; li < levels && !calibrationCancelRequested; ++li) {
      const uint16_t pw = kPWProbeLevels[li];
      GapMeasurement gm = set_pw_and_measure(ch, pw);

      String line = (String)"[PW_PROBE] ch=" + ch + " pin=GP" + PW_PINS[ch] +
                    " PW_raw=" + pw;
      if (gm.timedOut) {
        Serial.println(line + " TIMEOUT");
        continue;
      }
      const float dutyPct = (float)((0.5 + (double)gm.value / (2.0 * periodUs)) * 100.0);
      Serial.println(line + " gapUs=" + gm.value +
                     " duty≈" + String(dutyPct, 2) + "%");

      if (reads == 0 || dutyPct < dutyMin) dutyMin = dutyPct;
      if (reads == 0 || dutyPct > dutyMax) dutyMax = dutyPct;
      ++reads;
      anyRead = true;
    }

    const float span = (reads > 0) ? (dutyMax - dutyMin) : 0.0f;
    Serial.println((String)"[PW_PROBE] ch=" + ch + " pin=GP" + PW_PINS[ch] +
                   " reads=" + reads + "/" + levels +
                   " span≈" + String(span, 2) + "pp" +
                   (ch == expectCh ? "  <-- expected for this oscillator" : ""));

    if (ch == expectCh) expectSpan = span;
    if (span > bestSpan) {
      bestSpan = span;
      bestCh   = ch;
    }
    if (calibrationCancelRequested) break;
  }

  apply_pw_center(expectCh);

  if (calibrationCancelRequested) {
    Serial.println("[PW_PROBE] cancelled by user");
    calibrationCancelRequested = false;
    return;
  }

  if (!anyRead) {
    Serial.println("[PW_PROBE] every read timed out: the cal-sense pin sees no pulse "
                   "at all, so this says nothing about the PW CV");
    cal_sense_probe_log();
    return;
  }
  if (expectSpan >= kPWProbeMovedPct) {
    Serial.println((String)"[PW_PROBE] done: expected ch=" + expectCh +
                   " moves the duty by " + String(expectSpan, 2) +
                   "pp, so the CV is live; look downstream in the mix");
    return;
  }
  if (bestSpan >= kPWProbeMovedPct) {
    Serial.println((String)"[PW_PROBE] done: the duty follows ch=" + bestCh +
                   " (GP" + PW_PINS[bestCh] + ", span " + String(bestSpan, 2) +
                   "pp) instead of the expected ch=" + expectCh +
                   ": PW_PINS does not match the wiring");
    return;
  }
  Serial.println((String)"[PW_PROBE] done: no channel moves the duty (widest " +
                 String(bestSpan, 2) + "pp on ch=" + bestCh +
                 "): the PW CV does not reach this oscillator's pulse, "
                 "so it cannot be muted from firmware");
}

// Debug helper used during manual calibration: measure and report the
// duty-cycle difference from the target duty (normally 50%) for the
// current note/DCO. The result is sent to the Input board as a 32-bit
// PARAM_GAP_FROM_DCO value, which it relays to the screen as "GAP".
void DCO_calibration_debug() {
  // Reuse the main gap-measurement path (which already handles polarity,
  // debouncing, and timeouts) so manual calibration sees the same notion
  // of "gap" as the automatic routines.
  GapMeasurement gm = measure_gap(0);  // target is 50% duty

  // Osc under trim comes from the stage walk (not currentDCO, which only moves
  // during auto-cal). The stage counts substages, so it is not the osc index.
  uint8_t reportDCO = cal_manual_osc();
  if (reportDCO >= NUM_OSCILLATORS) {
    reportDCO = NUM_OSCILLATORS - 1;
  }

  // Compute duty error relative to the center target (0.5) using the
  // *ideal* period for the current note. For manual trimming this is
  // sufficient and keeps the math simple.
  int32_t dutyErrorPercentTimes100 = 0;  // duty error [%] * 100

  if (!gm.timedOut) {
    double freqHz = (double)note_to_freq(DCO_calibration_current_note);
    if (freqHz > 0.0) {
      double periodUs = 1000000.0 / freqHz;
      // gm.value is avgHighUs - avgLowUs (same sign as find_gap).
      // For a perfect 50% duty, high and low are equal, so gm.value == 0.
      // Duty error fraction from 50% is:
      //   duty_high - 0.5 = (avgHighUs - avgLowUs) / (2 * periodUs)
      // The oscillator's duty trim is subtracted so that manual trimming and
      // auto-cal aim at the same target: "0" on screen means the same duty the
      // searches converge to.
      double gapUs = (double)gm.value - (double)duty_trim_gap_us(reportDCO, (float)freqHz);
      double dutyErrorFrac = gapUs / (2.0 * periodUs);
      double dutyErrorPercent = dutyErrorFrac * 100.0;
      // Scale by 100 for two decimal digits of resolution on the screen.
      dutyErrorPercentTimes100 = (int32_t)(dutyErrorPercent * 100.0);
    }
  } else {
    // On timeout, propagate a large sentinel so the UI/Serial never look
    // like a near-perfect 50% trim.
    dutyErrorPercentTimes100 = kManualGapTimeoutDutyErrTimes100;
  }

  if (autotuneDebug >= 1) {
    if (gm.timedOut) {
      Serial.println((String)"[MANUAL_GAP] note=" + DCO_calibration_current_note +
                     (String)" DCO=" + reportDCO +
                     (String)" AMP=" + ampCompCalibrationVal +
                     (String)" TIMEOUT");
      // Raw cal-sense window (no period gate) — separates stuck pin from rejected freq.
      cal_sense_probe_log();
    } else {
      Serial.println((String)"[MANUAL_GAP] note=" + DCO_calibration_current_note +
                     (String)" DCO=" + reportDCO +
                     (String)" AMP=" + ampCompCalibrationVal +
                     (String)" gapUs=" + gm.value +
                     (String)" dutyTrim=" + (ampCompDutyOffset[reportDCO] / 100.0) + "%" +
                     (String)" dutyErr(%)≈" + (dutyErrorPercentTimes100 / 100.0));
    }
  }

  // Send as a 32-bit PARAM_GAP_FROM_DCO value through the standard
  // param protocol: Serial2 → Mainboard, which relays it to the Screen.
  // force: do not drop when Serial2 DMA is busy — this is the live GAP UI.
  serialSendParam32(PARAM_GAP_FROM_DCO, (uint32_t)dutyErrorPercentTimes100, true);
}

#endif  // __AUTOTUNE_IMPL_H__
