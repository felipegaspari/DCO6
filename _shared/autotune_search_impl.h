#ifndef __AUTOTUNE_SEARCH_IMPL_H__
#define __AUTOTUNE_SEARCH_IMPL_H__

#include "../include_all.h"

// Canary: autotune_search.ino #errors if this macro is missing. A linker
// symbol below is also referenced from autotune_impl.h so a stale search_impl
// TU that still defines calibrate_DCO_freq_trace but lacks this symbol fails
// at link — the preprocessor canary alone can be bypassed by an old
// autotune_search.ino that never checks the macro.
#define FREQ_TRACE_AMP_GEOM_GRID 1
constexpr uint32_t kFreqTraceAmpGeomGridMagic = 0x414D4747u;  // 'AMGG'
const uint32_t kFreqTraceAmpGeomGridBuilder = kFreqTraceAmpGeomGridMagic;

// =============================================================================
// autotune_search_impl.h — search-based DCO amplitude-compensation calibration.
//
// This file holds the per-note search that builds each oscillator's
// [frequency -> range PWM] table (calibrate_DCO), the highest/lowest
// frequency estimators used when the table reaches the top of the PWM range,
// and the interpolation helpers shared by those routines.
//
// Definitions, not declarations: include this exactly once per sketch, from a
// .ino shim (DCO/autotune_search.ino), after autotune_impl.h — the file-scope
// statics of the two are visible to each other in that order.
//
// Orchestration (DCO_calibration) and the PW center/limit searches live in
// autotune_impl.h; the edge-timing measurement core (find_gap) lives there too.
// =============================================================================

// Signed duty error (in microseconds, + = amplitude too low) measured at the
// frequency find_freq_for_duty50() last returned; kGapTimeoutSentinel when the
// search never saw a usable signal. The FREQ_TRACE logs report it per stored
// pair and the calibration report converts it to a duty percentage, so the
// achieved precision is visible.
static float g_lastFreqBisectGapUs = kGapTimeoutSentinel;

// Duty probes spent by the last find_freq_for_duty50() call (bisection plus
// refinement). Printed as probes= so an implausibly fast run is visible.
static int g_lastFreqBisectProbes = 0;

// Extra readings the last search spent waiting for the waveform to stop moving
// after a frequency change (see measure_duty_at_freq). Printed as settle=, so
// an oscillator that needs a long time to follow a jump is visible in the logs
// rather than silently biasing the readings.
static int g_lastSettleChecks = 0;

// Last measure_duty_at_freq() at amp 0 never got two readings to agree.
// find_freq_for_duty50() then treats the probe as sign-only (no INTERP).
static bool g_lastDutyUnsettled = false;

// Secant seed from the last amp0_prescan() that found a sign change, or 0.
// FREQ_TRACE stores this when the endpoint search is rejected, instead of
// the model intercept that can sit below the pulse floor.
static float g_lastAmp0ScanSeedHz = 0.0f;

// Compute allowed |gap| (in microseconds) for a given frequency (Hz) and
// duty-cycle error fraction (e.g. 0.005 = 0.5% duty error).
// From duty_high - 0.5 = gap / (2*T): |gap|max = 2 * epsilon * T.
double compute_gap_tolerance_for_freq(double freqHz, double dutyErrorFraction) {
  if (freqHz <= 0.0) {
    return 1e6;  // Very loose tolerance if frequency is invalid.
  }
  double periodUs = 1e6 / freqHz;
  return 2.0 * dutyErrorFraction * periodUs;
}

// Return true if the two values have opposite signs (simple sign change test).
// Used by calibrate_DCO() to detect when the duty-cycle error has crossed
// through zero between successive measurements (indicating we've passed the
// ideal PWM point and should probe neighbours more carefully).
static bool did_sign_change(float previous, float current) {
  return (previous > 0.0f && current < 0.0f) ||
         (previous < 0.0f && current > 0.0f);
}

// Helper: set the current DCO amplitude, wait for the waveform to settle,
// and return the measured duty-cycle gap (or timeout sentinel value).
// IMPORTANT: We normalize the sign here so that a *positive* value means
// "amplitude too low" and a *negative* value means "amplitude too high".
static float measure_gap_for_amp(uint16_t ampPwm) {
  const float freqHz = note_to_freq(DCO_calibration_current_note);
  voice_task_autotune(0, ampPwm);
  settle_for_freq((double)freqHz);
  ++calRunProbes;
  GapMeasurement gm = measure_gap(0);

  // Preserve the timeout sentinel exactly so downstream code can reliably
  // detect "no signal" vs a real small error.
  if (gm.timedOut) {
    return kGapTimeoutSentinel;
  }

  // find_gap() returns avgHighUs - avgLowUs; flip the sign so the search
  // moves the PWM in the correct direction regardless of edge polarity, and
  // aim at the trimmed duty target instead of a bare 50%.
  return -(gm.value - duty_trim_gap_us(currentDCO, freqHz));
}

// Helper: evaluate neighbour measurements (lower/higher) around the current
// PWM and update closestToZero / bestAmpComp if any of them are better.
// The caller passes in the measurements taken one step below and above the
// current PWM value; this routine picks the best candidate among those and
// the current PWM, based purely on closeness of the duty error to zero.
static void update_best_from_neighbours(
  int rangeSamples,
  const float* lowerMeasurements,
  const uint16_t* lowerVoltages,
  const float* higherMeasurements,
  const uint16_t* higherVoltages,
  float avgValue,
  float& closestToZero,
  uint16_t& bestAmpComp,
  uint16_t currentAmpCompCalibrationVal
) {
  for (int i = 0; i < rangeSamples; i++) {
    if (abs(lowerMeasurements[i]) < abs(closestToZero)) {
      closestToZero = lowerMeasurements[i];
      bestAmpComp = lowerVoltages[i];
    }
    if (abs(higherMeasurements[i]) < abs(closestToZero)) {
      closestToZero = higherMeasurements[i];
      bestAmpComp = higherVoltages[i];
    }
  }

  // Check the current voltage again
  if (abs(avgValue) < abs(closestToZero)) {
    closestToZero = avgValue;
    bestAmpComp = currentAmpCompCalibrationVal;
  }
}

// Helper: PWM step for the next probe based on the current error.
// For large errors step by 2; once close to the target (within tolerance * 20)
// step by 1 to avoid overshooting. Sign follows the error direction.
static int step_amp_from_error(float avgValue, double tolerance) {
  int magnitude = (abs(avgValue) < tolerance * 20) ? 1 : 2;
  return (avgValue > 0) ? magnitude : -magnitude;
}

// Helper: compute the initial amplitude (range PWM) guess for a given table
// index j and note, using the same interpolation strategy as the original code:
//  - j == 4: manual preset scaled by 1.35,
//  - j == 6: logarithmic interpolation between the first two entries,
//  - else : quadratic interpolation based on the previous three calibration points.
static uint16_t compute_initial_amp_for_note(
  const DCOCalibrationContext& ctx,
  int j
) {
  if (j == 4) {
    return (ctx.initManualAmpByOsc[ctx.dcoIndex] + ctx.manualOffsetByOsc[ctx.dcoIndex]) * 1.35;
  } else if (j == 6) {
    return logarithmicInterpolation(
      ctx.calibrationData[2],
      ctx.calibrationData[3],
      ctx.calibrationData[4],
      ctx.calibrationData[5],
      note_to_freq(ctx.currentNote) * 100
    );
  } else {
    return quadraticInterpolation(
      ctx.calibrationData[j - 6],
      ctx.calibrationData[j - 5],
      ctx.calibrationData[j - 4],
      ctx.calibrationData[j - 3],
      ctx.calibrationData[j - 2],
      ctx.calibrationData[j - 1],
      note_to_freq(ctx.currentNote) * 100
    );
  }
}

// Helper: store the final calibration pair for the current note into the
// calibration table and print a short summary to Serial.
static void store_note_result(
  DCOCalibrationContext& ctx,
  int j,
  uint16_t bestAmpComp,
  float closestToZero
) {
  ctx.calibrationData[j]     = note_to_freq(ctx.currentNote) * 100;
  ctx.calibrationData[j + 1] = bestAmpComp;

  // closestToZero keeps its 50000 initializer when no probe ever succeeded.
  cal_report_set_pair_from_gap(
    j / 2,
    (fabsf(closestToZero) < 40000.0f) ? closestToZero : kGapTimeoutSentinel,
    note_to_freq(ctx.currentNote),
    CAL_SRC_RUNG);

  Serial.print("DCO_calibration_current_note ");
  Serial.println(ctx.currentNote);
  Serial.print("Best calibration voltage: ");
  Serial.println(bestAmpComp);
  Serial.print("Closest measurement to zero: ");
  Serial.println(closestToZero);
}

// Frequency ratio spanned by one calibration note interval (2^(n/12)).
static inline float calibration_interval_ratio() {
  return powf(2.0f, (float)calibration_note_interval / 12.0f);
}

// How far a frequency change moves the oscillator, in cents. Returns a huge
// value when there is nothing to compare against (cold start), which makes the
// callers treat it as the largest possible move.
static float freq_move_cents(float fromHz, float toHz) {
  if (fromHz <= 0.0f || toHz <= 0.0f) {
    return 1e9f;
  }
  return fabsf(1200.0f * log2f(toHz / fromHz));
}

// Wait a number of waveform periods, floored at a minimum. Used for the wait
// between writing a frequency and reading it; the values come from the
// precision profile, and they can be short because nothing here is trusted
// until measure_duty_at_freq() sees two readings agree.
static void wait_periods(float freqHz, float periods, uint32_t minUs) {
  uint32_t us = minUs;
  if (freqHz > 0.0f) {
    const uint32_t p = (uint32_t)(periods * 1000000.0f / freqHz + 0.999f);
    if (p > us) {
      us = p;
    }
  }
  if (us >= 1000u) {
    delay(us / 1000u);
  }
  const uint32_t rem = us % 1000u;
  if (rem) {
    delayMicroseconds(rem);
  }
}

// Write a probe frequency and remember it. The move is made in one go: walking
// there in small steps only means the divider changes again before a full
// waveform has come out at the previous one, which is not settling, it is a
// frequency ramp. One write followed by a wait long enough to produce whole
// periods is what the measurement actually needs.
static void drive_freq(float freqHz, uint16_t amp) {
  calibrationFreqHz  = freqHz;
  voice_task_autotune(4, amp);
  g_lastDrivenFreqHz = freqHz;
}

// Probe the duty error at an arbitrary frequency with a fixed range PWM. Sets
// the frequency (drive_freq), gates find_gap() against it and then keeps
// reading until two readings agree - the wait that actually matters is not a
// constant, it is however long this oscillator needs, so the fixed wait stays
// short and two readings that agree within the search's own acceptance prove
// nothing is moving any more. Sign convention matches measure_gap_for_amp():
// positive = amplitude too low (i.e. the frequency is too high for this PWM).
// hiRes averages the precision profile's window instead of kGapSamplesDefault.
// Returns kGapTimeoutSentinel on timeout.
float measure_duty_at_freq(float freqHz, uint16_t amp, bool hiRes) {
  const CalPrecisionProfile &prec = cal_precision();
  const float movedCents = freq_move_cents(g_lastDrivenFreqHz, freqHz);
  const bool  bigMove    = (movedCents >= kSettleBigMoveCents);

  gapGateFreqHz = freqHz;
  drive_freq(freqHz, amp);
  wait_periods(freqHz, prec.settlePeriods, prec.settleMinMs * 1000u);

  const uint8_t mode = hiRes ? 3 : 0;

  // First reading. A timeout normally means "frequency too high" (the pulse
  // collapsed), but right after a large move it can also be the core still
  // catching up, so that case is given one more chance before believing it.
  ++g_lastFreqBisectProbes;
  ++calRunProbes;
  GapMeasurement gm = measure_gap(mode);
  if (gm.timedOut && bigMove) {
    ++g_lastFreqBisectProbes;
    ++g_lastSettleChecks;
    ++calRunProbes;
    gm = measure_gap(mode);
  }
  if (gm.timedOut) {
    gapGateFreqHz = 0.0f;
    return kGapTimeoutSentinel;
  }

  // How much settling this probe may pay for, from how far it just moved: a
  // late bisection iteration moves by well under a cent and needs nothing, a
  // semitone or more gets the full budget.
  int checks = 0;
  if (movedCents >= kSettleSkipCents) {
    checks = bigMove ? (int)prec.settleMaxChecks : 1;
  }
  if (amp == 0 && checks < 3) {
    checks = 3;
  }

  // "Settled" = two readings that agree closely enough that more waiting could
  // not change what the search decides.
  double stableTol = compute_gap_tolerance_for_freq(freqHz, prec.bisectDutyTol);
  if (stableTol < prec.bisectGapFloorUs) {
    stableTol = prec.bisectGapFloorUs;
  }
  stableTol *= (double)prec.settleStableMult;

  float value    = gm.value;
  bool  settled  = (checks == 0);
  g_lastDutyUnsettled = false;
  for (int c = 0; c < checks; ++c) {
    if (calibrationCancelRequested) {
      break;
    }
    ++g_lastFreqBisectProbes;
    ++g_lastSettleChecks;
    ++calRunProbes;
    GapMeasurement again = measure_gap(mode);
    if (again.timedOut) {
      // The valid reading in hand already proved the waveform exists; a
      // re-read discarded by the gap gates (one-sided, off-period, a genuine
      // glitch) does not refute it. Marginal waveforms flicker between clean
      // and glitchy readings, and returning the sentinel here is how a probe
      // that had measured the far side of the crossing once became a "no
      // pulse" wall that stopped the pair-1 search 1% short of the answer.
      // The check is consumed; if none are left, the valid reading stands.
      if (autotuneDebug >= 2) {
        Serial.println((String)"[FREQ_SETTLE] f=" + fmt_freq(freqHz) + " amp=" + amp +
                       " re-read discarded; keeping the valid reading");
      }
      continue;
    }
    if (fabsf(again.value - value) <= (float)stableTol) {
      value   = 0.5f * (value + again.value);  // both are good; average them
      settled = true;
      break;
    }
    if (amp == 0) {
      // Keep the reading closer to 50% (smaller |gap|). The newer one used
      // to throw away a 49.90% settle in favour of a later 3% swing.
      if (fabsf(again.value) < fabsf(value)) {
        value = again.value;
      }
    } else {
      value = again.value;  // still moving: the newer reading is the better one
    }
  }

  if (!settled && autotuneDebug >= 2) {
    Serial.println((String)"[FREQ_SETTLE] f=" + fmt_freq(freqHz) + " amp=" + amp +
                   " moved=" + movedCents + " cents; no two readings within " +
                   stableTol + " us after " + checks + " checks");
  }
  if (!settled && amp == 0) {
    g_lastDutyUnsettled = true;
  }

  gapGateFreqHz = 0.0f;
  // Aim at 50% + the oscillator's duty trim (0 by default).
  return -(value - duty_trim_gap_us(currentDCO, freqHz));
}

// How fast the outward step grows while the search is still hunting for a
// bracket, and how far inside the bracket an interpolated candidate must stay
// (as a fraction of the bracket, in log-frequency) to be worth measuring.
static constexpr float  kSearchStepGrowth = 1.6f;
static constexpr double kBracketEdgeGuard = 0.05;
// Narrower than this and there is nothing left to resolve: the noise between
// two readings of the same point is bigger than what moving inside the bracket
// could change. Without a floor the search re-probes the same frequency until
// the budget runs out (seen at the amp-0 endpoint: ~20 probes at 6.29 Hz).
static constexpr double kBracketMinWidthCents = 3.0;

// Snap a candidate so the probe actually moves by at least kMinFreqStepHz.
static double snap_min_freq_step(double from, double to) {
  if (fabs(to - from) >= (double)kMinFreqStepHz) {
    return to;
  }
  return (to >= from) ? (from + (double)kMinFreqStepHz)
                      : (from - (double)kMinFreqStepHz);
}

// Largest step one probe of the frequency search may take at this frequency:
// 400 cents above 440 Hz, 200 from 100 Hz up, 100 below that (including the
// amp-0 hunt under 30 Hz). See kSearchStepCents* in autotune_constants.h.
static float search_step_cap_cents(float freqHz) {
  if (freqHz >= kSearchStepHighHz)    return kSearchStepCentsHigh;
  if (freqHz >= kSearchStepLowHz)     return kSearchStepCentsMid;
  if (freqHz >= kSearchStepVeryLowHz) return kSearchStepCentsLow;
  return kSearchStepCentsVeryLow;
}

// Next frequency to probe inside a bracket, per autotuneSearchMode (cmds 37-39).
//
// The bracket is [fLo, fHi] with gLo < 0 < gHi, so the answer is between them
// whatever this returns; the mode only decides how fast it closes and how much
// it is willing to believe.
//   BISECT: the geometric midpoint, which halves the bracket in cents. Only the
//     sign of a reading is used, so a magnitude thrown off by noise cannot move
//     the probe.
//   INTERP: an Illinois secant step in log-frequency. Duty error against
//     log-frequency is nearly straight over a small bracket, so this usually
//     lands inside the acceptance in one or two moves.
//   GATED: INTERP only where both readings are clearly bigger than the noise
//     the measurement admits to (noiseGapUs, the disagreement between two
//     readings of one point that measure_duty_at_freq() is willing to accept),
//     BISECT where they are not - fast up high, sign-only at the bottom.
// edgeFromTimeout means one of the edges is a probe that found no pulse and so
// has no magnitude to interpolate against, which forces the midpoint whatever
// the mode. So does an interpolated candidate that lands within
// kBracketEdgeGuard of an edge: it would measure a frequency we have
// effectively already measured.
static double next_probe_in_bracket(double fLo, double fHi, double gLo, double gHi,
                                    bool edgeFromTimeout, double noiseGapUs) {
  const double lLo = log(fLo);
  const double lHi = log(fHi);

  bool interpolate = (autotuneSearchMode != SEARCH_BISECT) &&
                     !edgeFromTimeout && gLo < 0.0 && gHi > 0.0;
  if (interpolate && autotuneSearchMode == SEARCH_GATED) {
    interpolate = (-gLo > noiseGapUs) && (gHi > noiseGapUs);
  }

  double next = 0.0;
  if (interpolate) {
    next = exp(lLo + (lHi - lLo) * (gLo / (gLo - gHi)));
    const double guardLo = exp(lLo + (lHi - lLo) * kBracketEdgeGuard);
    const double guardHi = exp(lHi - (lHi - lLo) * kBracketEdgeGuard);
    if (!(next > guardLo && next < guardHi)) {
      next = 0.0;
    }
  }
  return (next > 0.0) ? next : sqrt(fLo * fHi);
}

// Find the frequency at which a fixed range PWM produces ~zero duty error (the
// 50% duty point of the freq(PWM) calibration curve).
//
// At a fixed PWM, a positive duty error ("amplitude too low") means the
// frequency is too high for the oscillator to reach full amplitude, so the
// answer is below this probe; a negative error means headroom, so it is above.
//
// A timeout means there is no pulse to measure at all, and which way that points
// depends on where the probe is. Above the range the amplitude has collapsed
// below the comparator threshold, so the answer is lower; at the very bottom the
// duty goes so lopsided that a single segment outlasts the reading deadline, so
// it is higher. The search decides from evidence where it has any - a timeout
// below a frequency that did produce a signal can only be the bottom - and
// otherwise reads it as "too high", which is the collapse and by far the common
// case. The amp-comp-0 search, the one that lives at the bottom of the range, is
// handed a measured bracket by amp0_prescan() so that it starts with the evidence
// instead of a guess about it.
//
// freqGuess is measured first - every caller passes a modelled seed, so that is
// the probe most likely to be the answer - and then the search walks outward in
// the indicated direction until it has the answer bracketed, in steps of at most
// search_step_cap_cents() and starting at a quarter of the caller's window, so a
// good seed is probed finely. Once bracketed, next_probe_in_bracket() closes in.
// windowRatio is how far the caller expects the answer to be from the seed;
// travelling more than a few times that means the seed was wrong and the search
// gives up with its best reading. Without bounds, spending kMaxSearchTimeouts
// probes on frequencies with no pulse ends it the same way; with bounds, the
// edges of the band are the terminator instead - a search that has been told
// where the answer must be is allowed to spend its probes getting there.
// With refine = true (FREQ_TRACE, the fine pass
// and both endpoints) the probes average a longer window, the probe budget and
// the acceptance come from the precision profile, and the converged frequency is
// confirmed by averaging before it is returned.
// Returns the best frequency found in Hz, or 0 if no usable signal was seen.
float find_freq_for_duty50(uint16_t amp, float freqGuess, float windowRatio,
                           bool refine, const FreqSearchBounds *bounds) {
  if (windowRatio < 1.05f) {
    windowRatio = 1.05f;
  }
  if (freqGuess <= 0.0f) {
    return 0.0f;
  }
  const double boundLo = (bounds != nullptr) ? (double)bounds->loHz : 0.0;
  const double boundHi = (bounds != nullptr) ? (double)bounds->hiHz : 0.0;
  const bool   bounded = (boundLo > 0.0 && boundHi > boundLo);
  if (bounded) {
    if (freqGuess < (float)boundLo) freqGuess = (float)boundLo;
    if (freqGuess > (float)boundHi) freqGuess = (float)boundHi;
  }
  // Keep the global in sync so [GAP_TIMEOUT]/debug logs report the probed PWM.
  ampCompCalibrationVal  = amp;
  g_lastFreqBisectGapUs  = kGapTimeoutSentinel;
  g_lastFreqBisectProbes = 0;
  g_lastSettleChecks     = 0;

  const CalPrecisionProfile &prec = cal_precision();
  const int   maxProbes   = refine ? prec.bisectIters : 24;
  const float windowCents = 1200.0f * log2f(windowRatio);
  // Allowance for the hunt: the caller's window plus one per retry it would
  // have spent shifting that window, which is how far the search used to be
  // able to reach. Spending it means the seed was outside the range the caller
  // promised, so there is nothing to gain from walking further.
  const int   windows      = refine ? prec.bisectWindows : 2;
  const float travelBudget = windowCents * (float)(windows + 1);

  float bestFreq      = 0.0f;
  float bestAbsGap    = 1e9f;
  float bestSignedGap = 0.0f;  // same measurement, sign kept for the report

  // Bracket: the highest probe that read "too low" and the lowest that read
  // "too high". Duty error rises with frequency, so the answer lies between
  // them. 0 = that side has not been seen yet.
  double fLo = 0.0, fHi = 0.0;
  double gLo = 0.0, gHi = 0.0;    // their readings
  bool   hiFromTimeout = false;   // no usable gap on the high edge
  bool   loFromTimeout = false;   // ... or on the low one
  int    lastSide      = 0;       // which edge the previous probe replaced
  int    timeouts      = 0;       // probes in a row that found no pulse at all

  // Open at a quarter of the window: the seed is a model prediction, so the
  // first move should be sized to the error expected of it, not to the whole
  // range the caller allows. The 1.6x growth still reaches the window edge in
  // three steps, so nothing loses reach.
  double f         = (double)freqGuess;
  float  stepCents = fminf(search_step_cap_cents(freqGuess), 0.25f * windowCents);
  float  travelled = 0.0f;
  // The frequencies that did produce a signal bound the region worth probing:
  // the answer cannot be outside them by more than the collapse itself, and a
  // timeout beyond either one says which side the dead zone is on.
  double lowGoodFreq  = 0.0;
  double highGoodFreq = 0.0;
  double tol          = prec.bisectGapFloorUs;

  for (int probe = 0; probe < maxProbes; ++probe) {
    if (calibrationCancelRequested) {
      return bestFreq;  // best-so-far; callers poll the cancel flag themselves
    }

    const float diff      = measure_duty_at_freq((float)f, amp, refine);
    const bool  timedOut  = (diff == kGapTimeoutSentinel);
    const bool  signOnly  = timedOut || (amp == 0 && g_lastDutyUnsettled);

    if (!timedOut) {
      if (f > highGoodFreq) {
        highGoodFreq = f;
      }
      if (lowGoodFreq == 0.0 || f < lowGoodFreq) {
        lowGoodFreq = f;
      }
      if (fabsf(diff) < bestAbsGap) {
        bestAbsGap    = fabsf(diff);
        bestSignedGap = diff;
        bestFreq      = (float)f;
      }

      if (autotuneDebug >= 2) {
        Serial.println((String)"[FREQ_BISECT] amp=" + amp +
                       (String)" f=" + fmt_freq((float)f) + (String)" gap=" + diff +
                       (String)" dutyErr=" +
                       duty_err_pct_from_gap(diff, (float)f) + "%");
      }

      // Acceptance from the precision profile (NORMAL 0.05% / 0.5 us, FINE
      // 0.02% / 0.25 us). The tight one is only meaningful because the hi-res
      // probes average a long time window (find_gap).
      tol = compute_gap_tolerance_for_freq(f, prec.bisectDutyTol);
      if (tol < prec.bisectGapFloorUs) tol = prec.bisectGapFloorUs;
      if (fabsf(diff) <= tol) {
        break;
      }
    }

    // Which edge of the bracket this probe becomes. A reading says so itself; a
    // timeout has to be placed, and getting that wrong is what used to send the
    // bottom-endpoint search marching further down into silence.
    int side;
    if (!timedOut) {
      side     = (diff > 0.0f) ? +1 : -1;
      timeouts = 0;  // the allowance below is for being lost, not for bad luck
    } else {
      ++timeouts;
      if (lowGoodFreq > 0.0 && f < lowGoodFreq) {
        side = -1;  // below a frequency that worked: the dead zone is the bottom
      } else if (highGoodFreq > 0.0 && f > highGoodFreq) {
        side = +1;  // above one: the pulse has collapsed
      } else {
        side = +1;  // nothing measured yet: assume the collapse
      }
    }

    if (side > 0) {
      fHi           = f;
      gHi           = timedOut ? 0.0 : (double)diff;
      hiFromTimeout = signOnly;
    } else {
      fLo           = f;
      gLo           = timedOut ? 0.0 : (double)diff;
      loFromTimeout = signOnly;
    }
    // Illinois: when the same edge is replaced twice running, halve the stale
    // edge's error so the interpolation stops creeping in from one side.
    if (side == lastSide) {
      if (side > 0) gLo *= 0.5;
      else          gHi *= 0.5;
    }
    lastSide = side;

    // Probing a region the oscillator cannot produce is the most expensive way
    // to learn nothing: 100 ms per probe, more at the bottom of the range and
    // doubled after a large move. Spend a fixed allowance of those in a row and
    // then stop, bracketed or not - in a row, because a search that keeps
    // producing readings between the dead probes is converging, not lost. A
    // bounded search is exempt: it cannot wander, so let it walk the whole band
    // looking for the pulse and stop at the edge instead of at a probe count.
    if (!bounded && timeouts >= kMaxSearchTimeouts) {
      Serial.println((String)"[FREQ_BISECT] amp=" + amp +
                     (String)" gave up after " + timeouts +
                     " probes in a row with no pulse (last " + fmt_freq((float)f) +
                     " Hz, seed " + fmt_freq(freqGuess) + "); keeping best=" +
                     fmt_freq(bestFreq));
      break;
    }

    if (fLo > 0.0 && fHi > 0.0) {
      // A bracket narrower than the smallest probe move is exhausted. A
      // 3-cent floor used to stop with a 3% duty error still on the table;
      // only stop on cents when the best reading is already inside tolerance.
      const double widthHz    = fHi - fLo;
      const double widthCents = 1200.0 * log2(fHi / fLo);
      if (widthHz < (double)kMinFreqStepHz ||
          (widthCents < kBracketMinWidthCents && bestAbsGap <= tol)) {
        if (autotuneDebug >= 1) {
          Serial.println((String)"[FREQ_BISECT] amp=" + amp +
                         (String)" bracket exhausted at " + fmt_freq((float)fLo) + ".." +
                         fmt_freq((float)fHi) + " Hz (" + (float)widthCents +
                         " cents); keeping best=" + fmt_freq(bestFreq));
        }
        break;
      }
      const double prevF = f;
      f = next_probe_in_bracket(fLo, fHi, gLo, gHi,
                                hiFromTimeout || loFromTimeout,
                                tol * (double)prec.settleStableMult);
      f = snap_min_freq_step(prevF, f);
      if (f <= fLo) f = fLo + (double)kMinFreqStepHz;
      if (f >= fHi) f = fHi - (double)kMinFreqStepHz;
      if (f <= fLo || f >= fHi) {
        if (bestAbsGap <= tol) {
          break;
        }
        // Nowhere left to put a distinct probe; keep the best reading.
        break;
      }
      // An edge that timed out puts that end of the bracket inside a dead zone,
      // and the answer is at its border, not in its middle. Keep the next probe
      // within half a step of the nearest frequency that did produce a signal,
      // so the search closes on the border from the side that can be measured
      // instead of spending 100 ms at a time inside the silence.
      if (hiFromTimeout && highGoodFreq > 0.0) {
        const double ceilingHz =
          highGoodFreq * pow(2.0, 0.5 * (double)search_step_cap_cents((float)highGoodFreq) / 1200.0);
        if (f > ceilingHz && ceilingHz > fLo) {
          f = ceilingHz;
        }
      }
      if (loFromTimeout && lowGoodFreq > 0.0) {
        const double floorHz =
          lowGoodFreq * pow(2.0, -0.5 * (double)search_step_cap_cents((float)lowGoodFreq) / 1200.0);
        if (f < floorHz && floorHz < fHi) {
          f = floorHz;
        }
      }
      continue;
    }

    // Not bracketed yet: step outward, bounded, growing until the sign flips.
    if (travelled >= travelBudget) {
      if (autotuneDebug >= 1) {
        Serial.println((String)"[FREQ_BISECT] amp=" + amp +
                       (String)" no bracket within " + travelBudget +
                       " cents of " + fmt_freq(freqGuess) + " Hz; keeping best=" +
                       fmt_freq(bestFreq));
      }
      break;
    }
    // A real reading says how far the crossing is. Cap the step at that
    // distance (assuming a conservatively flat slope, so the cap overshoots
    // ~2x and still brackets in one hop) instead of jumping the full range cap
    // away from a seed that already read near zero. See
    // kSearchSlopeMinPctPer100Cents in autotune_constants.h.
    if (!timedOut) {
      const float propCents = fabsf(duty_err_pct_from_gap(diff, (float)f)) *
                              (100.0f / kSearchSlopeMinPctPer100Cents);
      stepCents = fminf(stepCents, fmaxf(propCents, kSearchStepFloorCents));
    }
    const double prev = f;
    f = f * pow(2.0, (side > 0 ? -1.0 : 1.0) * (double)stepCents / 1200.0);
    f = snap_min_freq_step(prev, f);
    if (bounded) {
      if (f < boundLo) f = boundLo;
      if (f > boundHi) f = boundHi;
      // Standing on an edge and being told to go further means the answer is
      // not inside the band: either nothing in it pulsed at all, or every
      // reading kept pointing past this edge. Say which - they mean different
      // things (a silent band has no signal; readings pointing past the edge
      // mean the 50% point sits outside what the caller allows).
      if (f == prev) {
        if (highGoodFreq > 0.0) {
          Serial.println((String)"[FREQ_BISECT] amp=" + amp +
                         (String)" readings keep pointing " +
                         ((prev <= boundLo) ? "below" : "above") +
                         " the band " + fmt_freq((float)boundLo) + ".." +
                         fmt_freq((float)boundHi) +
                         " Hz; keeping best=" + fmt_freq(bestFreq));
        } else {
          Serial.println((String)"[FREQ_BISECT] amp=" + amp +
                         (String)" no pulse anywhere in " + fmt_freq((float)boundLo) + ".." +
                         fmt_freq((float)boundHi) + " Hz (" + timeouts +
                         " timeouts); keeping best=" + fmt_freq(bestFreq));
        }
        break;
      }
    }
    travelled += stepCents;

    // How big the next step may be. The per-range cap is there so a reading is
    // not taken straight after a jump the waveform has not settled from - but a
    // probe that timed out produced no reading to protect, so a bounded search
    // that has yet to see a single pulse strides instead, and crosses its band in
    // a few probes rather than a dozen. kHuntStepMaxCents keeps even that from
    // jumping clean over a narrow band of usable frequencies. Only a bounded
    // search: it has been told where the answer is, so striding can only bring it
    // closer, whereas an unbounded hunt striding away from a seed it cannot check
    // is how the top endpoint used to overshoot.
    const bool nothingMeasuredYet = (highGoodFreq == 0.0);
    stepCents = fminf(stepCents * kSearchStepGrowth,
                      (bounded && nothingMeasuredYet)
                        ? kHuntStepMaxCents
                        : search_step_cap_cents((float)f));
    // Feeling for the border of a dead zone from a frequency that works: halve
    // the step so the last probe before the silence is a close one.
    if (timedOut && !nothingMeasuredYet) {
      stepCents *= 0.5f;
    }
  }

  if (bestAbsGap >= 1e9f) {
    Serial.println((String)"[FREQ_BISECT] amp=" + amp +
                   (String)" no valid signal around " + fmt_freq(freqGuess) + " Hz");
    return 0.0f;
  }

  if (!refine) {
    g_lastFreqBisectGapUs = bestSignedGap;
    return bestFreq;
  }

  // Confirm: the search stops on one probe, which noise can bias by a step, so
  // average confirmReads readings at the frequency it settled on. If that
  // average still misses the acceptance, the probe that ended the search was
  // lucky rather than right: feed the average back into the bracket, take one
  // more step and confirm again.
  //
  // This replaces measuring a grid of five candidates +/-0.05% and +/-0.1% away
  // (under 2 cents) and keeping whichever read smallest. That was a minimum over
  // five noisy readings, which reported its own luck as the achieved error;
  // averaging the same number of readings at one frequency cuts the noise by
  // sqrt(n) with no bias, and correcting through the bracket is what actually
  // moves the answer when the frequency really is off.
  const int confirmReads  = (prec.confirmReads  < 1) ? 1 : prec.confirmReads;
  const int confirmRounds = (prec.confirmRounds < 1) ? 1 : prec.confirmRounds;

  float bestConfirmedFreq = 0.0f;
  float bestConfirmedGap  = 1e9f;
  float bestConfirmedSign = 0.0f;
  f = (double)bestFreq;  // reuse the search's probe cursor

  for (int round = 0; round < confirmRounds; ++round) {
    if (calibrationCancelRequested) {
      break;
    }

    float sum = 0.0f;
    int   n   = 0;
    for (int read = 0; read < confirmReads; ++read) {
      const float d = measure_duty_at_freq((float)f, amp, true);
      if (d == kGapTimeoutSentinel) {
        n = 0;  // unusable point: nothing to average
        break;
      }
      sum += d;
      ++n;
    }
    if (n == 0) {
      break;
    }

    const float avg = sum / (float)n;
    if (fabsf(avg) < bestConfirmedGap) {
      bestConfirmedGap  = fabsf(avg);
      bestConfirmedSign = avg;
      bestConfirmedFreq = (float)f;
    }

    const double acceptFrac = (amp == 0)
      ? ((double)kEndpointAcceptDutyPct / 100.0)
      : prec.bisectDutyTol;
    double roundTol = compute_gap_tolerance_for_freq(f, acceptFrac);
    if (roundTol < prec.bisectGapFloorUs) roundTol = prec.bisectGapFloorUs;
    if (fabsf(avg) <= roundTol) {
      break;
    }

    // Correct through the bracket the search already built. Without one (the
    // seed was accepted on the first probe) there is nothing to interpolate
    // against, so the averaged reading stands — do not accept just because
    // confirmRounds is exhausted.
    if (avg > 0.0f) {
      fHi = f; gHi = (double)avg; hiFromTimeout = false;
    } else {
      fLo = f; gLo = (double)avg;
    }
    if (!(fLo > 0.0 && fHi > 0.0)) {
      break;
    }
    const double next = next_probe_in_bracket(fLo, fHi, gLo, gHi, hiFromTimeout,
                                              roundTol * (double)prec.settleStableMult);
    if (!(next > 0.0)) {
      break;
    }
    if (autotuneDebug >= 2) {
      Serial.println((String)"[FREQ_BISECT] amp=" + amp +
                     (String)" confirm " + fmt_freq((float)f) + " avg=" + avg +
                     (String)" over " + n + " reads; correcting to " +
                     fmt_freq((float)next));
    }
    f = snap_min_freq_step(f, next);
  }

  if (bestConfirmedGap >= 1e9f) {
    g_lastFreqBisectGapUs = bestSignedGap;
    return bestFreq;  // every confirmation reading timed out; keep the search result
  }

  const float searchDuty  = fabsf(duty_err_pct_from_gap(bestAbsGap, bestFreq));
  const float confirmDuty = fabsf(duty_err_pct_from_gap(bestConfirmedGap, bestConfirmedFreq));
  const float acceptPct   = (amp == 0)
    ? kEndpointAcceptDutyPct
    : (float)(prec.bisectDutyTol * 100.0);
  if (searchDuty <= acceptPct && confirmDuty > acceptPct) {
    if (autotuneDebug >= 2) {
      Serial.println((String)"[FREQ_BISECT] amp=" + amp +
                     (String)" confirm " + fmt_freq(bestConfirmedFreq) +
                     " dutyErr=" + confirmDuty +
                     "% worse than search " + fmt_freq(bestFreq) +
                     " dutyErr=" + searchDuty + "%; keeping search");
    }
    g_lastFreqBisectGapUs = bestSignedGap;
    return bestFreq;
  }

  if (autotuneDebug >= 2) {
    Serial.println((String)"[FREQ_BISECT] amp=" + amp +
                   (String)" confirmed " + fmt_freq(bestFreq) + " -> " +
                   fmt_freq(bestConfirmedFreq) +
                   (String)" gap=" + bestConfirmedGap + " (was " + bestAbsGap + ")" +
                   (String)" dutyErr=" +
                   duty_err_pct_from_gap(bestConfirmedGap, bestConfirmedFreq) + "%");
  }
  g_lastFreqBisectGapUs = bestConfirmedSign;
  return bestConfirmedFreq;
}

// Model helpers shared with the curve tracer, defined further down with the rest
// of the FREQ_TRACE model.
static float  freq_trace_power_seed(const float *freqs, const float *amps,
                                    int count, float ampTarget);
static String freq_trace_quality(float gapUs, float freqHz, int probes,
                                 int settleChecks);

// Measure frequency at full range PWM (amp = DIV_COUNTER). Returns Hz*100.
// The amp ceiling is fixed at DIV_COUNTER (not a searched "highest amp"); this
// only runs find_freq_for_duty50 at that amp. Called from calibrate_DCO() when
// the classic per-note search tops out. Never writes DIV_COUNTER+1.
float find_highest_freq(DCOCalibrationContext& ctx, int pairsFilled) {
  CalPrecisionOverride fineForEndpoint(
    (calibrationPrecision == CAL_PRECISION_FAST) ? calibrationPrecision
                                                 : CAL_PRECISION_FINE);

  float knownFreq[kCalReportPairs];
  float knownAmp[kCalReportPairs];
  int   knownCount = 0;
  if (pairsFilled > kCalReportPairs) {
    pairsFilled = kCalReportPairs;
  }
  for (int p = 1; p < pairsFilled; ++p) {
    const float f = (float)ctx.calibrationData[2 * p] / 100.0f;
    const float a = (float)ctx.calibrationData[2 * p + 1];
    if (f <= 0.0f || f >= 100000.0f || a <= 0.0f) {
      continue;
    }
    knownFreq[knownCount] = f;
    knownAmp[knownCount]  = a;
    ++knownCount;
  }

  float fSeed = freq_trace_power_seed(knownFreq, knownAmp, knownCount,
                                      (float)DIV_COUNTER);
  float windowRatio = kTopEndpointWindowRatio;
  if (fSeed <= 0.0f) {
    fSeed       = note_to_freq(DCO_calibration_current_note);
    windowRatio = calibration_interval_ratio();
  }

  float bestFreq = find_freq_for_duty50(DIV_COUNTER, fSeed, windowRatio, true);
  const float lastFreq = (knownCount > 0) ? knownFreq[knownCount - 1] : 0.0f;
  if (bestFreq <= lastFreq && knownCount > 0 && !calibrationCancelRequested) {
    const float fRetry = lastFreq * calibration_interval_ratio();
    Serial.println((String)"[HIGHEST_FREQ] retry from " + fRetry +
                   " Hz (seed " + fSeed + " gave " + bestFreq +
                   "); measuring at amp=DIV_COUNTER");
    bestFreq = find_freq_for_duty50(DIV_COUNTER, fRetry,
                                    calibration_interval_ratio(), true);
  }
  if (bestFreq <= 0.0f) {
    bestFreq = (lastFreq > 0.0f)
                 ? lastFreq * calibration_interval_ratio()
                 : note_to_freq(DCO_calibration_current_note);
    Serial.println((String)"[HIGHEST_FREQ] no valid signal at DIV_COUNTER; using " +
                   bestFreq);
  }

  Serial.println((String)"Highest freq at amp=DIV_COUNTER(" + DIV_COUNTER +
                 "): " + bestFreq +
                 freq_trace_quality(g_lastFreqBisectGapUs, bestFreq,
                                    g_lastFreqBisectProbes, g_lastSettleChecks) +
                 " (seed=" + fSeed + ")");

  return bestFreq * 100.0f;  // Hz * 100 for calibrationData
}

// Defined further down with the rest of the FREQ_TRACE model helpers, used
// here and by calibrate_DCO_freq_trace().
static float amp0_fit_freq(const float *amps, const float *freqs, int count);

// Estimate the lowest reachable frequency for the current DCO using the
// latest [freq -> PWM] calibration data, assuming an amp compensation
// (range PWM) of 0. This is conceptually symmetric to find_highest_freq(),
// but instead of a live search we derive the estimate from the stored table.
//
// The estimate is a least-squares line through the lowest table pairs
// (amp0_fit_freq(): the bottom of the curve is measured to be linear), with
// the historical 3-point quadratic kept only as the fallback when too few
// distinct pairs exist for a fit.
//
// Return value: estimated lowest frequency * 100 (same units as
// calibrationData[] entries and find_highest_freq()).
float find_lowest_freq() {
  // Use amp compensation (range PWM) = 0 as requested.
  ampCompCalibrationVal = 0;

  // We require at least three calibration points (six entries). The layout of
  // calibrationData is:
  //   [0]  reserved / lowestFreq placeholder
  //   [1]  reserved
  //   [2]  freq0 * 100
  //   [3]  pwm0
  //   [4]  freq1 * 100
  //   [5]  pwm1
  //   [6]  freq2 * 100
  //   [7]  pwm2
  //   ...
  //
  // If we don't have enough data, just return 0.
  if (chanLevelVoiceDataSize < 8) {
    return 0.0f;
  }

  // Collect the bottom pairs for the fit. The pairs are stored ascending, so
  // the first ones are the lowest; the helper picks the lowest-amp ones and
  // applies its own spread rule. The top sentinel (20 MHz) never enters
  // because collection stops well before it, and synthetic amp-0 entries are
  // skipped explicitly.
  float fitAmps[kAmp0FitPoints + 3];
  float fitFreqs[kAmp0FitPoints + 3];
  int   fitCount = 0;
  for (int j = 2;
       j + 1 < chanLevelVoiceDataSize &&
       fitCount < (int)(sizeof(fitAmps) / sizeof(fitAmps[0]));
       j += 2) {
    const float fHz = (float)calibrationData[j] / 100.0f;
    const float amp = (float)calibrationData[j + 1];
    if (!(fHz > 0.0f) || !(amp > 0.0f)) continue;
    fitAmps[fitCount]  = amp;
    fitFreqs[fitCount] = fHz;
    ++fitCount;
  }
  const float fitHz = amp0_fit_freq(fitAmps, fitFreqs, fitCount);
  if (fitHz > 0.0f) {
    Serial.println((String)"[LOWEST_FREQ_EST] DCO=" + currentDCO +
                   (String)" estFreq*100=" + (fitHz * 100.0f) +
                   (String)" from least-squares fit");
    return fitHz * 100.0f;
  }

  float f0 = (float)calibrationData[2];  // already freq * 100
  float p0 = (float)calibrationData[3];
  float f1 = (float)calibrationData[4];
  float p1 = (float)calibrationData[5];
  float f2 = (float)calibrationData[6];
  float p2 = (float)calibrationData[7];

  // Guard against degenerate cases where the PWMs are identical.
  if (p0 == p1 || p1 == p2 || p0 == p2) {
    // Fall back to a simple linear extrapolation using the first segment.
    float y = linearInterpolation(p0, f0, p1, f1, 0.0f);
    return y;
  }

  // No usable fit: fall back to the quadratic in the space PWM -> (freq * 100)
  // evaluated at PWM = 0.
  float estFreqTimes100 = quadraticInterpolation(
    p0, f0,
    p1, f1,
    p2, f2,
    0.0f
  );

  // Clamp to a sensible minimum to avoid negative or zero frequencies
  // from extreme extrapolation.
  if (estFreqTimes100 < 0.0f) {
    estFreqTimes100 = 0.0f;
  }

  Serial.println((String)"[LOWEST_FREQ_EST] DCO=" + currentDCO +
                 (String)" estFreq*100=" + estFreqTimes100 +
                 (String)" using PWM points {" + p0 + "," + p1 + "," + p2 + "}");

  return estFreqTimes100;
}

// Search window for the amp-0 hunt: the seed is an extrapolation rather than a
// measurement, so allow roughly an octave in either direction. The caller's
// bounds are what really contain the search; this only sizes its first step.
static constexpr float kLowestFreqWindowRatio = 2.0f;

// Where the amp-comp-0 point may be: under the first measured pair, down to
// kAmp0BandRatio below it, and never under kAmp0MinFreqHz. Both amp-comp methods
// bound the scan, the search and what they are willing to store to this.
static FreqSearchBounds amp0_search_band(float firstPairHz) {
  const float hi = firstPairHz * 0.99f;
  float       lo = firstPairHz / kAmp0BandRatio;
  if (lo < kAmp0MinFreqHz) {
    lo = kAmp0MinFreqHz;
  }
  if (!(lo < hi)) {
    // A first pair this low is already at the edge of what a duty reading can
    // resolve; leave a sliver of band rather than an empty one.
    lo = hi / 1.05f;
  }
  return { lo, hi };
}

// One quick duty reading at freqHz: write the frequency, wait, take a single
// averaged reading. This is measure_duty_at_freq() without the adaptive settle -
// deliberately, because a scan wants one number per point, not the two or three
// re-readings that function spends proving a point has stopped moving. The wait
// is still at least a whole period: down here kAmp0ScanSettleMs alone is a
// fraction of one, and what comes back then describes the previous frequency.
// Same sign and duty-trim convention as measure_duty_at_freq().
// Returns kGapTimeoutSentinel when nothing pulsed.
static float scan_duty_at_freq(float freqHz, uint16_t amp) {
  // Keep the logging global in sync, as find_freq_for_duty50() does: without
  // this the [GAP_MEASURE]/[GAP_TIMEOUT] lines of the scan report whatever amp
  // the previous stage drove (the top endpoint's 14000), not the scan's own.
  ampCompCalibrationVal = amp;
  gapGateFreqHz = freqHz;
  drive_freq(freqHz, amp);
  uint32_t settleMs = kAmp0ScanSettleMs;
  if (freqHz > 0.0f) {
    const uint32_t onePeriodMs = (uint32_t)(1000.0f / freqHz) + 1;
    if (onePeriodMs > settleMs) {
      settleMs = onePeriodMs;
    }
  }
  delay(settleMs);
  ++calRunProbes;
  const GapMeasurement gm = measure_gap(3);
  gapGateFreqHz = 0.0f;
  if (gm.timedOut) {
    return kGapTimeoutSentinel;
  }
  return -(gm.value - duty_trim_gap_us(currentDCO, freqHz));
}

// Walk kAmp0ScanPoints frequencies down through the band at amp comp 0, looking
// for two readings of opposite sign: that pair brackets the answer, and a
// bracket is worth far more to the search than any single seed - it can only
// interpolate inward from there, and no probe can wander into a region already
// known to be silent. Returns the bracket (the whole band when only one sign
// showed up) and, through seedOut, where the search should start: the secant
// crossing of the bracket when there is one, else the point that read closest to
// 50%, else the caller's fallback.
//
// Descending, because the top of the band is the most likely to pulse and
// because the first point that reads "too low" is the lower edge - the duty only
// grows further down, so nothing under it can close the bracket. A silent point,
// on the other hand, is no evidence about what is under it and does not stop the
// descent: the pulse can be lost at either end (above by the amplitude collapsing
// under the comparator threshold, below by the duty going so lopsided that a
// segment outlasts the reading deadline).
static FreqSearchBounds amp0_prescan(FreqSearchBounds band, float fallbackHz,
                                     float *seedOut) {
  *seedOut = fallbackHz;
  g_lastAmp0ScanSeedHz = 0.0f;
  if (!(band.loHz > 0.0f && band.hiHz > band.loHz) || kAmp0ScanPoints < 2) {
    return band;
  }

  const double ratio = pow((double)band.loHz / (double)band.hiHz,
                           1.0 / (double)(kAmp0ScanPoints - 1));
  float  bestFreq = 0.0f;
  float  bestGap  = 0.0f;
  bool   found    = false;
  // Tightest bracket seen so far: aboveFreq read "frequency too high" (gap > 0),
  // belowFreq read "too low", so the answer is between them.
  float  aboveFreq = 0.0f, aboveGap = 0.0f;
  float  belowFreq = 0.0f, belowGap = 0.0f;
  double f         = (double)band.hiHz;

  for (int i = 0; i < kAmp0ScanPoints; ++i, f *= ratio) {
    if (calibrationCancelRequested) {
      break;
    }
    const float gap = scan_duty_at_freq((float)f, 0);
    if (gap == kGapTimeoutSentinel) {
      // The sentinel covers a real timeout but also a reading the gap gates
      // discarded (one-sided, off-period) - the pin may well have pulsed.
      Serial.println((String)"[AMP0_SCAN] f=" + fmt_freq((float)f) + " no usable reading");
      continue;
    }
    Serial.println((String)"[AMP0_SCAN] f=" + fmt_freq((float)f) + " dutyErr=" +
                   String(duty_err_pct_from_gap(gap, (float)f), 2) + "%");
    if (!found || fabsf(gap) < fabsf(bestGap)) {
      bestFreq = (float)f;
      bestGap  = gap;
      found    = true;
    }
    if (gap > 0.0f) {
      // Too high: the tightest such point is the bracket's upper edge so far.
      aboveFreq = (float)f;
      aboveGap  = gap;
      continue;
    }
    // Too low, and the scan is descending, so every point under this one reads
    // the same way: this is the lower edge and there is no reason to go on.
    belowFreq = (float)f;
    belowGap  = gap;
    break;
  }

  if (belowFreq > 0.0f && aboveFreq > belowFreq && aboveGap > belowGap) {
    // Secant crossing in log frequency: duty error against log f is close enough
    // to a straight line over a bracket this small that this is usually within a
    // few cents of the answer - and unlike bestFreq it is a point the search has
    // not already measured.
    const double t = (double)(-belowGap) / (double)(aboveGap - belowGap);
    *seedOut = (float)((double)belowFreq *
                       pow((double)aboveFreq / (double)belowFreq, t));
    g_lastAmp0ScanSeedHz = *seedOut;
    Serial.println((String)"[AMP0_SCAN] bracketed " + fmt_freq(belowFreq) + ".." +
                   fmt_freq(aboveFreq) + " Hz; seeding the search at " +
                   fmt_freq(*seedOut) + " Hz");
    return { belowFreq, aboveFreq };
  }

  if (!found) {
    Serial.println((String)"[AMP0_SCAN] no usable reading anywhere in " +
                   fmt_freq(band.loHz) + ".." + fmt_freq(band.hiHz) +
                   " Hz; seeding the search at " + fmt_freq(fallbackHz));
    return band;
  }
  *seedOut = bestFreq;
  Serial.println((String)"[AMP0_SCAN] no sign change in " + fmt_freq(band.loHz) + ".." +
                 fmt_freq(band.hiHz) + " Hz; seeding the search at " + fmt_freq(bestFreq) +
                 " Hz (dutyErr=" +
                 String(duty_err_pct_from_gap(bestGap, bestFreq), 2) + "%)");
  return band;
}

// Measure (instead of extrapolating) the lowest usable frequency: fix the
// amp comp at 0 and search the frequency at which the duty is 50%, within
// bounds. Every amp-comp-0 point in the firmware comes through here, so the scan
// that finds where the oscillator pulses at all happens once, here: it hands the
// search a bracket to work inside and a seed on the line between its two edges,
// which is also why the search needs no hint about what a timeout means down
// here. freqSeedHz is only what is left if the scan finds no pulse at all.
// Returns the frequency in Hz, or 0 when amp 0 gives no usable signal at all.
float measure_lowest_freq_at_amp0(float freqSeedHz, const FreqSearchBounds *bounds) {
  if (freqSeedHz <= 0.0f) {
    return 0.0f;
  }
  if (bounds == nullptr) {
    return find_freq_for_duty50(0, freqSeedHz, kLowestFreqWindowRatio, true);
  }
  float                  seedHz  = freqSeedHz;
  const FreqSearchBounds bracket = amp0_prescan(*bounds, freqSeedHz, &seedHz);
  return find_freq_for_duty50(0, seedHz, kLowestFreqWindowRatio, true, &bracket);
}

// Replace the table's amp-comp-0 anchor (entry [0..1]) with a measured point.
// Classic method only: FREQ_TRACE measures its own bottom endpoint. The
// classic top-out path leaves an extrapolated frequency there, and a classic
// run that never reaches full amp comp leaves the restart_DCO_calibration()
// placeholder (freq 0, amp comp ampCompLowestFreqVal).
// Seed and result are both held inside amp0_search_band() below the first real
// pair, for the reasons given at the FREQ_TRACE bottom endpoint: below the
// oscillator's pulse floor there is nothing to measure at any frequency, and an
// extrapolation is a better entry 0 than a frequency that cannot be produced.
// That also keeps the table monotonic.
void apply_measured_lowest_freq(DCOCalibrationContext& ctx) {
  const float prevHz = (float)ctx.calibrationData[0] / 100.0f;
  if (ctx.calibrationData[2] == 0) {
    Serial.println((String)"[LOWEST_FREQ] DCO=" + ctx.dcoIndex +
                   " no first pair to bound the search; keeping entry 0 as is");
    return;
  }

  const float            firstPairHz = (float)ctx.calibrationData[2] / 100.0f;
  const FreqSearchBounds bounds      = amp0_search_band(firstPairHz);
  const float            floorHz     = bounds.loHz;
  const float            ceilHz      = bounds.hiHz;

  // Seed with the estimate the method left behind when it is inside the band,
  // otherwise one interval below the first pair. find_lowest_freq()'s quadratic
  // extrapolation to PWM 0 is not used as a seed: it is fitted to the three
  // lowest points and, aimed past all of them, is exactly where it is least
  // reliable.
  float seedHz = prevHz;
  if (!(seedHz >= floorHz && seedHz <= ceilHz)) {
    const float clamped = (seedHz < floorHz) ? floorHz : ceilHz;
    Serial.println((String)"[LOWEST_FREQ] DCO=" + ctx.dcoIndex +
                   " seed " + seedHz + " -> " + clamped +
                   " Hz (band " + floorHz + ".." + ceilHz + ")");
    seedHz = clamped;
  }

  const float foundHz = measure_lowest_freq_at_amp0(seedHz, &bounds);
  if (foundHz <= 0.0f) {
    Serial.println((String)"[LOWEST_FREQ] DCO=" + ctx.dcoIndex +
                   " no pulse at amp 0 anywhere in " + fmt_freq(floorHz) + ".." +
                   fmt_freq(ceilHz) +
                   " Hz (seed=" + fmt_freq(seedHz) + "); keeping estimate " +
                   fmt_freq(prevHz));
    return;
  }

  const float    foundErr      = duty_err_pct_from_gap(g_lastFreqBisectGapUs, foundHz);
  const uint32_t foundTimes100 = (uint32_t)(foundHz * 100.0f);
  if (!(foundHz >= floorHz && foundHz <= ceilHz) || foundTimes100 == 0 ||
      fabsf(foundErr) > kEndpointAcceptDutyPct) {
    Serial.println((String)"[LOWEST_FREQ] DCO=" + ctx.dcoIndex +
                   " rejected: best " + fmt_freq(foundHz) + " Hz dutyErr=" +
                   String(foundErr, 2) + "% (band " + fmt_freq(floorHz) + ".." +
                   fmt_freq(ceilHz) +
                   ", accept " + kEndpointAcceptDutyPct +
                   "%); keeping estimate " + fmt_freq(prevHz));
    return;
  }

  ctx.calibrationData[0] = foundTimes100;
  ctx.calibrationData[1] = 0;
  cal_report_set_pair_from_gap(0, g_lastFreqBisectGapUs, foundHz,
                               CAL_SRC_ENDPOINT_AMP0);
  Serial.println((String)"[LOWEST_FREQ] DCO=" + ctx.dcoIndex +
                 " amp=0 freq=" + fmt_freq(foundHz) +
                 " gapUs=" + g_lastFreqBisectGapUs +
                 " probes=" + g_lastFreqBisectProbes +
                 " settle=" + g_lastSettleChecks +
                 " (seed=" + fmt_freq(seedHz) + ", was " + fmt_freq(prevHz) + ")");
}

// Build the [frequency -> amplitude PWM] calibration table for the DCO in ctx.
// For each calibration note it:
//  - Picks an initial PWM guess (via interpolation),
//  - Searches locally for the PWM that makes the duty error closest to zero,
//  - Stores the best PWM together with the note frequency in ctx.calibrationData.
// dutyErrorFraction controls how much duty-cycle error (e.g. 0.005 = 0.5%)
// is tolerated before the search stops for each note.
// Classic per-note path only: when the initial amp guess exceeds this fraction
// of DIV_COUNTER, stop the note loop and measure frequency at amp=DIV_COUNTER.
// FREQ_TRACE no longer uses this — its geometric grid ends at DIV_COUNTER.
static constexpr float kAmpSaturationFraction = 0.98f;

void calibrate_DCO(DCOCalibrationContext& ctx, double dutyErrorFraction) {

  const int rangeSamples = 2;  // Number of neighbour voltages to probe around a sign change.
  const int numPresetVoltages = chanLevelVoiceDataSize;  // Size of the [freq, pwm] table.

  // Per-note search guards: a dead oscillator or an unreachable tolerance
  // must not hang the whole calibration run.
  const int           kMaxSearchIterations   = 300;
  const unsigned long kMaxNoteSearchMs       = 30000;
  const int           kMaxConsecutiveTimeouts = 20;

  for (int j = 4; j < numPresetVoltages; j += 2) {  // Start from the 3rd preset voltage

    if (calibrationCancelRequested) {
      return;  // caller (DCO_calibration) discards the partial table
    }

    ctx.currentNote = DCO_calibration_start_note + (calibration_note_interval * (j - 4) / 2);
    VOICE_NOTES[0] = ctx.currentNote;
    uint16_t currentAmpCompCalibrationVal = compute_initial_amp_for_note(ctx, j);

    if (currentAmpCompCalibrationVal >
        (uint16_t)(DIV_COUNTER * kAmpSaturationFraction)) {
      // Top-of-range for CLASSIC: stop the note loop and measure frequency at
      // fixed amp = DIV_COUNTER (no searched "highest amp", never +1).
      // j/2 pairs already filled seed the model for that measurement.
      float highestFreqFound = find_highest_freq(ctx, j / 2);  // Hz * 100
      float lowestFreqCalc   = find_lowest_freq();   // Hz * 100, at PWM=0

      // Store the highest reachable point at this index.
      ctx.calibrationData[j]     = (uint32_t)highestFreqFound;
      ctx.calibrationData[j + 1] = DIV_COUNTER;
      cal_report_set_pair_from_gap(j / 2, g_lastFreqBisectGapUs,
                                   highestFreqFound / 100.0f,
                                   CAL_SRC_ENDPOINT_FULL);

      // Ensure entry 0 continues to represent the lowest frequency at PWM=0.
      ctx.calibrationData[0] = (uint32_t)lowestFreqCalc;
      ctx.calibrationData[1] = 0;

      for (int i = j + 2; i < numPresetVoltages; i += 2) {
        ctx.calibrationData[i] = 20000000;
        ctx.calibrationData[i + 1] = DIV_COUNTER;
        cal_report_set_pair(i / 2, kCalDutyErrUnknown, CAL_SRC_SENTINEL);
      }
      break;
    }

    const uint16_t minAmpComp = currentAmpCompCalibrationVal * 0.8;  // Lower limit for this note.
    const uint16_t maxAmpComp = currentAmpCompCalibrationVal * 1.3;  // Upper limit for this note.

    const double freqHz = note_to_freq(VOICE_NOTES[0]);
    double tolerance = compute_gap_tolerance_for_freq(freqHz, dutyErrorFraction);

    // For debugging, report the effective duty-cycle tolerance in percent.
    const double periodUs = (freqHz > 0.0) ? (1000000.0 / freqHz) : 0.0;
    double toleranceDutyPercent = 0.0;
    if (periodUs > 0.0) {
      toleranceDutyPercent = (tolerance / (2.0 * periodUs)) * 100.0;
    }

    Serial.println((String) "Current DCO: " + ctx.dcoIndex);
    Serial.println((String) "Calibration note: " + VOICE_NOTES[0]);
    Serial.println((String) "Calibration note freq: " + freqHz);
    Serial.println((String) "Calibration note amplitude: " + currentAmpCompCalibrationVal);
    Serial.println((String) "Tolerance (us): " + tolerance);
    Serial.println((String) "Tolerance duty approx (%): " + toleranceDutyPercent);
    Serial.println((String) "MinAmpComp: " + minAmpComp);
    Serial.println((String) "MaxAmpComp: " + maxAmpComp);

    voice_task_autotune(0, currentAmpCompCalibrationVal);  // Send the preset voltage
    delay(10);

    uint16_t bestAmpComp = currentAmpCompCalibrationVal;  // Best PWM found so far for this note.
    float closestToZero = 50000;   // Smallest absolute duty error seen so far.
    float previousAvgValue = 0.0;  // Duty error from the previous iteration (for sign-change detection).

    float lowerMeasurements[rangeSamples];   // Duty errors measured at lower neighbour PWMs.
    float higherMeasurements[rangeSamples];  // Duty errors measured at higher neighbour PWMs.
    uint16_t lowerVoltages[rangeSamples];    // PWM values used for lowerMeasurements[].
    uint16_t higherVoltages[rangeSamples];   // PWM values used for higherMeasurements[].

    int flipCounter = 0;  // Count of successive sign changes; used to relax tolerance if the search oscillates.
    int consecutiveTimeouts = 0;
    unsigned long noteSearchStartMs = millis();

    for (int iteration = 0;; ++iteration) {
      if (calibrationCancelRequested) {
        break;  // note loop head returns to the caller
      }
      if (iteration >= kMaxSearchIterations ||
          (millis() - noteSearchStartMs) > kMaxNoteSearchMs) {
        Serial.println((String)"[DCO_AMP_GUARD] note=" + ctx.currentNote +
                       (String)" DCO=" + ctx.dcoIndex +
                       (String)" search guard tripped after " + iteration +
                       (String)" iterations; keeping best AMP=" + bestAmpComp);
        break;
      }

      float avgValue = measure_gap_for_amp(currentAmpCompCalibrationVal);

      // Optional debug: report current duty and tolerance when enabled.
      // Treat timeout sentinel specially so we don't fake a 50% duty reading.
      if (autotuneDebug >= 2 && periodUs > 0.0) {
        if (avgValue == kGapTimeoutSentinel) {
          Serial.println((String)"[DCO_AMP_SCAN] note=" + ctx.currentNote +
                         (String)" DCO=" + ctx.dcoIndex +
                         (String)" AMP=" + currentAmpCompCalibrationVal +
                         (String)" gap=TIMEOUT" +
                         (String)" duty=NA target=50% tol≈" + toleranceDutyPercent + "%");
        } else {
          // avgValue sign convention: positive => amplitude too low.
          double dutyErrorFrac = (double)avgValue / (2.0 * periodUs);
          double dutyPercent   = (0.5 + dutyErrorFrac) * 100.0;
          Serial.println((String)"[DCO_AMP_SCAN] note=" + ctx.currentNote +
                         (String)" DCO=" + ctx.dcoIndex +
                         (String)" AMP=" + currentAmpCompCalibrationVal +
                         (String)" gap=" + avgValue +
                         (String)"us duty=" + dutyPercent +
                         (String)"% target=50% tol≈" + toleranceDutyPercent + "%");
        }
      }

      // Timeout: no usable signal at this PWM. The most common cause is an
      // amplitude too low for the calibration comparator, so nudge the PWM up
      // one step and measure again. previousAvgValue is deliberately left
      // untouched so the sentinel cannot fake a sign change, and the sentinel
      // is never allowed into the best-candidate tracking below.
      if (avgValue == kGapTimeoutSentinel) {
        ++consecutiveTimeouts;
        if (consecutiveTimeouts >= kMaxConsecutiveTimeouts) {
          Serial.println((String)"[DCO_AMP_GUARD] note=" + ctx.currentNote +
                         (String)" DCO=" + ctx.dcoIndex +
                         (String)" too many consecutive timeouts; keeping best AMP=" + bestAmpComp);
          break;
        }
        if (currentAmpCompCalibrationVal < maxAmpComp) {
          currentAmpCompCalibrationVal += 1;
        }
        continue;
      }
      consecutiveTimeouts = 0;

      // Update best candidate if this measurement is closer to zero.
      if (abs(avgValue) < abs(closestToZero)) {
        closestToZero = avgValue;
        bestAmpComp = currentAmpCompCalibrationVal;
      }

      // Detect sign change
      if (did_sign_change(previousAvgValue, avgValue)) {
        // Store measurements around the current voltage
        for (int i = 0; i < rangeSamples; i++) {
          uint16_t lowerVoltage = currentAmpCompCalibrationVal - (i + 1);
          uint16_t higherVoltage = currentAmpCompCalibrationVal + (i + 1);

          lowerMeasurements[i] = measure_gap_for_amp(lowerVoltage);
          lowerVoltages[i] = lowerVoltage;

          higherMeasurements[i] = measure_gap_for_amp(higherVoltage);
          higherVoltages[i] = higherVoltage;
        }

        update_best_from_neighbours(
          rangeSamples,
          lowerMeasurements,
          lowerVoltages,
          higherMeasurements,
          higherVoltages,
          avgValue,
          closestToZero,
          bestAmpComp,
          currentAmpCompCalibrationVal
        );

        // Break the loop if the closest value is within tolerance
        if (abs(closestToZero) <= tolerance) {
          break;
        } else {
          tolerance = tolerance * 1.2;
        }
        flipCounter++;
        if (flipCounter >= 3 && abs(closestToZero) <= tolerance * 2) {
          break;
        } else {
          tolerance = tolerance * 1.5;
        }
      }

      // Step the PWM toward the target and enforce the allowed search window.
      // Stepping is done in int32 so it cannot wrap below zero.
      int32_t nextAmp = (int32_t)currentAmpCompCalibrationVal + step_amp_from_error(avgValue, tolerance);
      if (nextAmp < (int32_t)minAmpComp) nextAmp = (int32_t)minAmpComp;
      if (nextAmp > (int32_t)maxAmpComp) nextAmp = (int32_t)maxAmpComp;

      if ((uint16_t)nextAmp == currentAmpCompCalibrationVal &&
          (nextAmp == (int32_t)minAmpComp || nextAmp == (int32_t)maxAmpComp)) {
        // Stuck at a search bound with the error still pushing outward:
        // the target is not reachable inside the window; keep the best found.
        Serial.println((String)"[DCO_AMP_GUARD] note=" + ctx.currentNote +
                       (String)" DCO=" + ctx.dcoIndex +
                       (String)" stuck at bound AMP=" + currentAmpCompCalibrationVal +
                       (String)"; keeping best AMP=" + bestAmpComp);
        break;
      }
      currentAmpCompCalibrationVal = (uint16_t)nextAmp;

      previousAvgValue = avgValue;
    }

    store_note_result(ctx, j, bestAmpComp, closestToZero);
  }
}

// =============================================================================
// FREQ_TRACE amp-comp calibration (method B, PARAM_DEBUG_COMMAND 35).
//
// The calibration table is one monotonic curve: freq(amp comp) at 50% duty.
// Instead of fixing a note frequency and hunting the integer amp comp (classic
// method above), fix the amp comp and bisect the frequency — the PIO divider gives
// near-continuous frequency resolution, so every stored pair is exact.
// =============================================================================

// Extrapolate y at targetX from up to three known curve points, ordered
// nearest-first (x1/y1 is the closest to the target). Mirrors the classic
// initial-guess strategy: 1 point -> proportional scaling (freq and amp are
// roughly proportional), 2 -> logarithmic, 3 -> quadratic. Works in either
// direction of the freq(amp) curve, so x/y can be freq/amp or amp/freq.
static float extrapolate_amp_for_freq(
  int nPoints,
  float x1, float y1,
  float x2, float y2,
  float x3, float y3,
  float targetX
) {
  if (nPoints >= 3) {
    return quadraticInterpolation(x3, y3, x2, y2, x1, y1, targetX);
  }
  if (nPoints == 2) {
    return (float)logarithmicInterpolation(x2, y2, x1, y1, targetX);
  }
  return (x1 > 0.0f) ? y1 * (targetX / x1) : y1;
}

// Minimum relative separation (in x) between the points chosen by
// freq_trace_guess(); closer candidates are treated as one point.
static constexpr float kGuessMinSpread = 0.10f;

// Least-squares quadratic through 4+ points, evaluated at targetX. An exact
// fit through 4 points would be a cubic - worse behaved than the quadratic it
// replaces when extrapolating - so the 4th point is used as redundancy
// instead: the fit averages the noise of any single measurement rather than
// reproducing it exactly in the coefficients, the way the exact 3-point
// quadratic does.
//
// Fitted in u = x - targetX so the intercept c0 is directly y(targetX), and in
// double throughout: the normal equations carry sums up to u^4, and with amps
// reaching 14000 those overflow what a float sum can resolve. Returns NAN on a
// degenerate system (x values nearly collinear after centering); the caller
// falls back to the exact 3-point path.
static float lsq_quadratic(const float *xs, const float *ys,
                           const int *idx, int n, float targetX) {
  double s0 = (double)n;
  double s1 = 0.0, s2 = 0.0, s3 = 0.0, s4 = 0.0;
  double t0 = 0.0, t1 = 0.0, t2 = 0.0;
  for (int k = 0; k < n; ++k) {
    const double u = (double)xs[idx[k]] - (double)targetX;
    const double y = (double)ys[idx[k]];
    const double u2 = u * u;
    s1 += u;
    s2 += u2;
    s3 += u2 * u;
    s4 += u2 * u2;
    t0 += y;
    t1 += u * y;
    t2 += u2 * y;
  }

  // Normal equations for y = c0 + c1*u + c2*u^2, solved by Cramer's rule:
  //   | s0 s1 s2 | | c0 |   | t0 |
  //   | s1 s2 s3 | | c1 | = | t1 |
  //   | s2 s3 s4 | | c2 |   | t2 |
  const double det = s0 * (s2 * s4 - s3 * s3)
                   - s1 * (s1 * s4 - s3 * s2)
                   + s2 * (s1 * s3 - s2 * s2);
  // Scale-aware degeneracy test: det has units of u^6, so compare it against
  // the point spread at the same power rather than a fixed epsilon.
  const double scale = s2 / (double)n;  // ~ mean squared spread of u
  if (!(det > 1e-9 * scale * scale * scale)) {
    return NAN;
  }

  const double c0 = (t0 * (s2 * s4 - s3 * s3)
                   - s1 * (t1 * s4 - s3 * t2)
                   + s2 * (t1 * s3 - s2 * t2)) / det;

  if (autotuneDebug >= 2) {
    Serial.println((String)"[GUESS_LSQ] x=" + targetX + " y=" + (float)c0 +
                   " points=" + n);
  }
  return (float)c0;
}

// Interpolate/extrapolate y(x) through the up-to-4 known points nearest to x.
// xs/ys hold every point measured so far in the current FREQ_TRACE run; the
// helper is used in both directions: amp-for-freq when targeting a ladder
// frequency, and freq-for-amp when seeding a fixed-amp bisection (bootstrap
// probes, downward trace). Points closer than kGuessMinSpread to an already
// chosen one are skipped, which also covers duplicate x values from integer
// PWM quantization (a quadratic fit would divide by zero on those).
// With 4 qualifying points the answer is a least-squares quadratic
// (lsq_quadratic() above); with 3 or fewer the historical exact paths apply
// (quadratic / log / proportional).
static float freq_trace_guess(const float *xs, const float *ys, int count, float x) {
  if (count <= 0) {
    return 0.0f;
  }

  int idx[4];
  int n = 0;

  // A quadratic through three nearly coincident points is meaningless outside
  // the cluster: right after the bootstrap the 4 probes sit within +/-6% of the
  // anchor, and fitting them alone sent the first ladder guesses ~25% off.
  // Points must therefore be spread by at least kGuessMinSpread in x.
  auto far_enough = [&](int cand) {
    for (int k = 0; k < n; ++k) {
      const float a = xs[cand], b = xs[idx[k]];
      const float ref = fmaxf(fabsf(a), fabsf(b));
      if (ref <= 0.0f || fabsf(a - b) < kGuessMinSpread * ref) {
        return false;
      }
    }
    return true;
  };

  // side: -1 = only points below x, +1 = only above, 0 = either. Nearest wins.
  auto pick = [&](int side) {
    int best = -1;
    for (int i = 0; i < count; ++i) {
      bool used = false;
      for (int k = 0; k < n; ++k) {
        if (idx[k] == i) used = true;
      }
      if (used) continue;
      if (side < 0 && xs[i] > x) continue;
      if (side > 0 && xs[i] < x) continue;
      if (!far_enough(i)) continue;
      if (best < 0 || fabsf(xs[i] - x) < fabsf(xs[best] - x)) {
        best = i;
      }
    }
    return best;
  };

  // Bracket the target first (interpolating beats extrapolating), then fill up
  // to four points with the nearest qualifying ones.
  int cand = pick(-1);
  if (cand >= 0) idx[n++] = cand;
  cand = pick(+1);
  if (cand >= 0) idx[n++] = cand;
  while (n < 4) {
    cand = pick(0);
    if (cand < 0) break;
    idx[n++] = cand;
  }
  if (n == 0) {
    return 0.0f;
  }

  // Nearest-first order: the exact paths below take the closest points, and
  // it matters for the single-point proportional case.
  for (int i = 1; i < n; ++i) {
    for (int j = i; j > 0 && fabsf(xs[idx[j]] - x) < fabsf(xs[idx[j - 1]] - x); --j) {
      const int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
    }
  }

  // Four points: least-squares quadratic. On a degenerate fit fall through to
  // the exact quadratic over the three nearest, exactly as with 3 points.
  if (n >= 4) {
    const float fit = lsq_quadratic(xs, ys, idx, n, x);
    if (!isnan(fit)) {
      return fit;
    }
    n = 3;
  }

  return extrapolate_amp_for_freq(
    n,
    xs[idx[0]], ys[idx[0]],
    (n > 1) ? xs[idx[1]] : 0.0f, (n > 1) ? ys[idx[1]] : 0.0f,
    (n > 2) ? xs[idx[2]] : 0.0f, (n > 2) ? ys[idx[2]] : 0.0f,
    x);
}

// Frequency at amp comp 0, from a least-squares line through the lowest-amp
// measured points. The bottom of the freq(amp) curve is measured to be linear
// (pair-to-pair slopes agree within ~1%), and a line through up to
// kAmp0FitPoints of it makes the intercept stable run to run - the 3-point
// quadratic that freq_trace_guess() extrapolates there amplified the noise of
// exactly the noisiest points and swung by whole octaves between runs. The
// intercept also matters more than it looks: entry 0 anchors the runtime
// interpolation for every note below pair 1, and only the fitted intercept
// reproduces the measured slope of that segment.
//
// amps/freqs hold measured points in any order; points closer in amp than
// kGuessMinSpread to one already chosen are skipped (same rule as
// freq_trace_guess()). Returns 0 when no trustworthy fit exists - fewer than
// 2 usable points, non-positive slope, or an intercept at or above the lowest
// fitted frequency - and the caller falls back to the old model.
static float amp0_fit_freq(const float *amps, const float *freqs, int count) {
  int idx[kAmp0FitPoints];
  int n = 0;

  // Pick the lowest-amp points, spread apart: the fit describes the bottom of
  // the curve, and the slight upward bend further up would tilt the intercept.
  while (n < kAmp0FitPoints) {
    int best = -1;
    for (int i = 0; i < count; ++i) {
      bool usable = (amps[i] > 0.0f && freqs[i] > 0.0f);
      for (int k = 0; k < n && usable; ++k) {
        if (idx[k] == i) {
          usable = false;
        } else {
          const float ref = fmaxf(amps[i], amps[idx[k]]);
          if (fabsf(amps[i] - amps[idx[k]]) < kGuessMinSpread * ref) {
            usable = false;
          }
        }
      }
      if (!usable) continue;
      if (best < 0 || amps[i] < amps[best]) {
        best = i;
      }
    }
    if (best < 0) break;
    idx[n++] = best;
  }
  if (n < 2) {
    return 0.0f;
  }

  // Ordinary least squares of freq against amp; the intercept is f(amp = 0).
  double sumA = 0.0, sumF = 0.0, sumAA = 0.0, sumAF = 0.0;
  float  lowestFreq = freqs[idx[0]];
  for (int k = 0; k < n; ++k) {
    const double a = (double)amps[idx[k]];
    const double f = (double)freqs[idx[k]];
    sumA  += a;
    sumF  += f;
    sumAA += a * a;
    sumAF += a * f;
    if (freqs[idx[k]] < lowestFreq) lowestFreq = freqs[idx[k]];
  }
  const double det = (double)n * sumAA - sumA * sumA;
  if (det <= 0.0) {
    return 0.0f;
  }
  const double slope     = ((double)n * sumAF - sumA * sumF) / det;
  const double intercept = (sumF - slope * sumA) / (double)n;
  if (slope <= 0.0 || !(intercept > 0.0) || intercept >= (double)lowestFreq) {
    return 0.0f;
  }

  Serial.println((String)"[AMP0_FIT] DCO=" + currentDCO +
                 (String)" f0=" + fmt_freq((float)intercept) +
                 (String)" Hz slope=" + (float)(1.0 / slope) +
                 (String)" cnt/Hz points=" + n);
  return (float)intercept;
}

// Anchor refinement: the stored ampComp440 is a seed, not the truth. Correct
// it until the 50% duty frequency is within kAnchorToleranceHz of 440 Hz.
// Always try at least one amp correction after acquire when it is off by more
// than that. How many corrections are allowed comes from the precision profile.
static constexpr float kAnchorToleranceHz = 0.1f;

// Bounds (in semitones) for the ladder spacing derived at runtime.
static constexpr int kLadderIntervalMin = 3;
static constexpr int kLadderIntervalMax = 12;

// A rung is stored where it was measured, but a rung that lands far from its
// target frequency makes the ladder uneven. Correct the amp (as many times as
// the precision profile allows) and keep the closest measurement.
static constexpr float kRungToleranceCents = 25.0f;

// How the bisection behind a stored pair went, kept next to the pair so the
// logs and the report describe the measurement that was actually kept.
struct FreqTraceProbeInfo {
  float gapUs;
  int   probes;
  int   settleChecks;
};

// Common tail of the trace log lines: the achieved error in microseconds and
// in duty percent (the unit the scope reads), the probes it took, and how many
// of those went into waiting for the waveform to settle.
static String freq_trace_quality(float gapUs, float freqHz, int probes,
                                 int settleChecks) {
  return (String)" gapUs=" + gapUs +
         " dutyErr=" + duty_err_pct_from_gap(gapUs, freqHz) + "%" +
         " probes=" + probes +
         " settle=" + settleChecks;
}

// Local log-log slope d(log freq) / d(log amp comp) near freqRef, from the two
// nearest known points. Charge current (hence frequency) is roughly
// proportional to amp comp, so 1.0 is the expected value; the clamp keeps a
// noisy pair of points from producing a wild correction.
static float freq_trace_local_slope(const float *freqs, const float *amps,
                                    int count, float freqRef) {
  if (freqRef <= 0.0f) {
    return 1.0f;
  }
  int i1 = -1, i2 = -1;
  for (int i = 0; i < count; ++i) {
    if (freqs[i] <= 0.0f || amps[i] <= 0.0f) continue;
    const float d = fabsf(logf(freqs[i] / freqRef));
    if (i1 < 0 || d < fabsf(logf(freqs[i1] / freqRef))) {
      i2 = i1; i1 = i;
    } else if (i2 < 0 || d < fabsf(logf(freqs[i2] / freqRef))) {
      i2 = i;
    }
  }
  if (i1 < 0 || i2 < 0 || amps[i1] == amps[i2] || freqs[i1] == freqs[i2]) {
    return 1.0f;
  }
  const float s = logf(freqs[i2] / freqs[i1]) / logf(amps[i2] / amps[i1]);
  if (!(s > 0.5f)) return 0.5f;  // also catches NaN
  if (s > 2.0f)    return 2.0f;
  return s;
}

// Frequency a given amp comp should land at, extrapolated as a power law from
// the top of the measured curve (freq ~ amp^s, s from the two highest points).
//
// Used to seed the full-amp endpoint, which is the one probe that sits outside
// the measured range. freq_trace_guess() fits a quadratic in linear (amp, freq)
// space, and evaluated outside its data that fit is ill-conditioned: on a real
// table its three terms came to 4489 - 13593 + 13004 for a 17-cent error, where
// this power law was 6 cents off and stable. The physical curve is much closer
// to a power law than to a parabola.
// Returns 0 when there is nothing to extrapolate from.
static float freq_trace_power_seed(const float *freqs, const float *amps,
                                   int count, float ampTarget) {
  if (ampTarget <= 0.0f) {
    return 0.0f;
  }
  // Anchor on the highest measured amp comp: the endpoint is above all of them,
  // so that point is the nearest one and its slope is the relevant one.
  int top = -1;
  for (int i = 0; i < count; ++i) {
    if (freqs[i] <= 0.0f || amps[i] <= 0.0f) continue;
    if (top < 0 || amps[i] > amps[top]) top = i;
  }
  if (top < 0) {
    return 0.0f;
  }
  const float s = freq_trace_local_slope(freqs, amps, count, freqs[top]);
  return freqs[top] * powf(ampTarget / amps[top], s);
}

// A table the runtime can interpolate has to rise in both columns. Whoever
// built it reports under its own tag and the caller skips the FS write, so a
// bad pass never replaces a good table.
static bool cal_table_is_monotonic(const uint32_t *data, int numPairs,
                                   uint8_t dcoIndex, const char *tag) {
  bool ok = true;
  uint32_t prevFreq = data[0];
  uint32_t prevAmp  = data[1];
  for (int p = 1; p < numPairs; ++p) {
    const uint32_t f = data[2 * p];
    const uint32_t a = data[2 * p + 1];
    if (f < prevFreq || a < prevAmp) {
      Serial.println((String)"[" + tag + "] DCO=" + dcoIndex +
                     " non-monotonic at pair " + p +
                     " (freq " + prevFreq + "->" + f +
                     ", amp " + prevAmp + "->" + a + ")");
      ok = false;
    }
    prevFreq = f;
    prevAmp  = a;
  }
  return ok;
}

// Trace the freq(amp comp) curve by fixing a geometric amp-comp grid and
// measuring frequency at duty = 50% on each rung (amp → freq). Pair 0 is amp
// comp 0; pairs 1..(numPairs-1) are the measured grid with the **last pair**
// at DIV_COUNTER (real measured freq — no fake 200 kHz sentinel). No
// find_highest_freq; never DIV_COUNTER+1. Returns false when the emitted table
// fails monotonicity.
bool calibrate_DCO_freq_trace(DCOCalibrationContext& ctx) {
  constexpr int numPairs   = (int)(chanLevelVoiceDataSize / 2);
  constexpr int firstRung  = 1;              // pair 0 = amp-comp-0 endpoint
  constexpr int lastRung   = numPairs - 1;   // last table pair = measured DIV_COUNTER
  constexpr int nRungs     = lastRung - firstRung + 1;  // 21 with 22-pair tables
  static_assert(nRungs >= 4, "table needs room for a geometric amp grid");

  // Unmistakable on serial: if this line is missing, the board is not running
  // the amp-geom builder (stale binary / old shared symlink).
  Serial.println((String)"[FREQ_TRACE] DCO=" + ctx.dcoIndex +
                 " builder=AMP_GEOM_GRID A_H=DIV_COUNTER(" + DIV_COUNTER +
                 ") pairs=" + numPairs +
                 " (no sentinel, no freq-ladder, never DIV_COUNTER+1)");
  calReportAmpGrid = true;
  calReportLadderInterval = 0;  // obsolete freq-ladder label — do not print

  // Every point measured in this run feeds freq_trace_guess() seeds.
  constexpr int kMaxKnown = numPairs + 8;
  float knownFreq[kMaxKnown];
  float knownAmp[kMaxKnown];
  int   knownCount = 0;
  auto add_known = [&](float f, float a) {
    if (knownCount < kMaxKnown) {
      knownFreq[knownCount] = f;
      knownAmp[knownCount]  = a;
      ++knownCount;
    }
  };

  float    freqByPair[numPairs];
  uint16_t ampByPair[numPairs];
  for (int p = 0; p < numPairs; ++p) {
    freqByPair[p] = 0.0f;
    ampByPair[p]  = 0;
  }

  // --- Anchor at the manual 440 Hz operating point --------------------------
  uint16_t anchorAmp = ampComp440[ctx.dcoIndex];
  if (anchorAmp == 0) {
    Serial.println((String)"[FREQ_TRACE_GUARD] DCO=" + ctx.dcoIndex +
                   " 440 Hz anchor not set - run manual calibration step 2" +
                   " (PARAM_AMP_COMP_440) and store it; aborting (previous table kept)");
    return false;
  }
  float anchorFreq = find_freq_for_duty50(
    anchorAmp, note_to_freq(manual_cal_reference_note),
    kAnchorAcquireWindowRatio, true);
  if (calibrationCancelRequested) {
    return false;
  }
  if (anchorFreq <= 0.0f) {
    const uint8_t anchorPwCh = cal_pw_channel(ctx.dcoIndex);
    Serial.println((String)"[FREQ_TRACE_GUARD] DCO=" + ctx.dcoIndex +
                   " no signal at manual anchor amp=" + anchorAmp +
                   " PW_raw=" + pw_level_readback(anchorPwCh) +
                   " (centre " + PW_CENTER[anchorPwCh] + ")" +
                   "; aborting (previous table kept)");
    return false;
  }
  add_known(anchorFreq, (float)anchorAmp);
  float anchorGapUs = g_lastFreqBisectGapUs;
  Serial.println((String)"[FREQ_TRACE] DCO=" + ctx.dcoIndex +
                 " anchor amp=" + anchorAmp + " freq=" + fmt_freq(anchorFreq) +
                 freq_trace_quality(anchorGapUs, anchorFreq,
                                    g_lastFreqBisectProbes, g_lastSettleChecks));

  // --- Manual trim seed (model + A_L for the geometric grid) ---------------
  int32_t manualAmp = (int32_t)ctx.initManualAmpByOsc[ctx.dcoIndex] +
                      (int32_t)ctx.manualOffsetByOsc[ctx.dcoIndex];
  if (manualAmp < 1) manualAmp = 1;
  if (manualAmp > (int32_t)DIV_COUNTER) manualAmp = (int32_t)DIV_COUNTER;
  {
    const float nominalHz = note_to_freq(manual_DCO_calibration_start_note);
    float found = find_freq_for_duty50(
      (uint16_t)manualAmp, nominalHz, kManualNoteWindowRatio, true);
    if (calibrationCancelRequested) {
      return false;
    }
    if (found > 0.0f) {
      add_known(found, (float)manualAmp);
      const float cents = 1200.0f * log2f(found / nominalHz);
      Serial.println((String)"[FREQ_TRACE_MANUAL] DCO=" + ctx.dcoIndex +
                     " amp=" + manualAmp + " freq=" + fmt_freq(found) +
                     " nominal=" + nominalHz + " dev=" + cents + " cents" +
                     freq_trace_quality(g_lastFreqBisectGapUs, found,
                                        g_lastFreqBisectProbes,
                                        g_lastSettleChecks));
    } else {
      Serial.println((String)"[FREQ_TRACE_MANUAL] DCO=" + ctx.dcoIndex +
                     " no signal at manual trim amp=" + manualAmp +
                     " (nominal=" + nominalHz + "); continuing without it");
    }
  }

  // --- Re-anchor at 440 Hz --------------------------------------------------
  {
    const float target440 = note_to_freq(manual_cal_reference_note);
    const uint16_t storedAmp = anchorAmp;
    float cents = 1200.0f * log2f(anchorFreq / target440);
    int tries = cal_precision().anchorTries;
    if (tries < 1) tries = 1;

    for (int attempt = 0;
         attempt < tries &&
         fabsf(anchorFreq - target440) > kAnchorToleranceHz;
         ++attempt) {
      if (calibrationCancelRequested) {
        return false;
      }
      float ampGuess = freq_trace_guess(knownFreq, knownAmp, knownCount, target440);
      int32_t ampNext = (int32_t)lroundf(ampGuess);
      if (ampNext < 1) ampNext = 1;
      if (ampNext > (int32_t)DIV_COUNTER) ampNext = (int32_t)DIV_COUNTER;
      if (ampNext == (int32_t)anchorAmp) {
        ampNext += (anchorFreq < target440) ? 1 : -1;
        if (ampNext < 1 || ampNext > (int32_t)DIV_COUNTER) {
          break;
        }
      }

      float found = find_freq_for_duty50(
        (uint16_t)ampNext, target440, kAnchorWindowRatio, true);
      if (found <= 0.0f) {
        Serial.println((String)"[FREQ_TRACE_ANCHOR] DCO=" + ctx.dcoIndex +
                       " no signal at amp=" + ampNext + "; keeping amp=" + anchorAmp);
        break;
      }
      add_known(found, (float)ampNext);

      const float newCents = 1200.0f * log2f(found / target440);
      if (fabsf(newCents) < fabsf(cents)) {
        anchorAmp   = (uint16_t)ampNext;
        anchorFreq  = found;
        anchorGapUs = g_lastFreqBisectGapUs;
        cents       = newCents;
      }
    }

    cents = 1200.0f * log2f(anchorFreq / target440);
    Serial.println((String)"[FREQ_TRACE_ANCHOR] DCO=" + ctx.dcoIndex +
                   " stored=" + storedAmp + " refined=" + anchorAmp +
                   " freq=" + fmt_freq(anchorFreq) + " dev=" + cents + " cents" +
                   " (tol=" + kAnchorToleranceHz + " Hz)" +
                   " gapUs=" + anchorGapUs +
                   " dutyErr=" + duty_err_pct_from_gap(anchorGapUs, anchorFreq) + "%");

    if (anchorAmp != storedAmp) {
      ampComp440[ctx.dcoIndex] = anchorAmp;
      update_FS_AmpComp440(ctx.dcoIndex, anchorAmp);
      Serial.println((String)"[FREQ_TRACE_ANCHOR] DCO=" + ctx.dcoIndex +
                     " manual 440 Hz value corrected " + storedAmp + " -> " +
                     anchorAmp + " and persisted");
    }
  }

  // --- Bootstrap cluster (model only) ---------------------------------------
  {
    const int nBootstrap =
      (calibrationPrecision == CAL_PRECISION_FAST) ? 2 : 4;
    for (int b = 0; b < nBootstrap; ++b) {
      if (calibrationCancelRequested) {
        return false;
      }
      int32_t amp = lroundf((float)anchorAmp *
                            exp2f((float)kBootstrapSemitones[b] / 12.0f));
      if (amp < 1) amp = 1;
      if (amp > (int32_t)DIV_COUNTER) amp = (int32_t)DIV_COUNTER;
      if (amp == (int32_t)anchorAmp) {
        continue;
      }

      float fSeed = freq_trace_guess(knownAmp, knownFreq, knownCount, (float)amp);
      if (fSeed <= 0.0f) fSeed = anchorFreq;
      // Window ≈ one geometric amp step once the grid exists; until then use
      // the classic note interval ratio.
      float found = find_freq_for_duty50((uint16_t)amp, fSeed,
                                         calibration_interval_ratio(), true);
      if (found <= 0.0f) {
        Serial.println((String)"[FREQ_TRACE_BOOT] DCO=" + ctx.dcoIndex +
                       " no signal at amp=" + amp + "; probe skipped");
        continue;
      }
      add_known(found, (float)amp);
      Serial.println((String)"[FREQ_TRACE_BOOT] DCO=" + ctx.dcoIndex +
                     " amp=" + amp + " freq=" + fmt_freq(found) +
                     freq_trace_quality(g_lastFreqBisectGapUs, found,
                                        g_lastFreqBisectProbes,
                                        g_lastSettleChecks));
    }
  }

  if (calibrationCancelRequested) {
    return false;
  }

  // --- Geometric amp grid: A_L … DIV_COUNTER (last rung = full wrap) --------
  const int32_t A_L = manualAmp;  // seed / lower bound (measured above when possible)
  const int32_t A_H = (int32_t)DIV_COUNTER;
  uint16_t gridAmp[nRungs];

  if (A_L >= A_H) {
    // Degenerate: only the wrap is usable.
    for (int i = 0; i < nRungs - 1; ++i) {
      int32_t fill = A_H - (nRungs - 1 - i);
      gridAmp[i] = (uint16_t)((fill > 1) ? fill : 1);
    }
    gridAmp[nRungs - 1] = DIV_COUNTER;
  } else {
    const float ratio = powf((float)A_H / (float)A_L, 1.0f / (float)(nRungs - 1));
    for (int k = 0; k < nRungs; ++k) {
      int32_t a = (int32_t)lroundf((float)A_L * powf(ratio, (float)k));
      if (a < 1) a = 1;
      if (a > (int32_t)DIV_COUNTER) a = (int32_t)DIV_COUNTER;
      gridAmp[k] = (uint16_t)a;
    }
    gridAmp[nRungs - 1] = DIV_COUNTER;  // force last rung = wrap (never +1)
  }

  // Strictly increasing below the last rung; last stays DIV_COUNTER.
  for (int k = 1; k < nRungs - 1; ++k) {
    if (gridAmp[k] <= gridAmp[k - 1]) {
      uint32_t next = (uint32_t)gridAmp[k - 1] + 1u;
      if (next >= (uint32_t)DIV_COUNTER) {
        next = (uint32_t)DIV_COUNTER - 1u;
      }
      gridAmp[k] = (uint16_t)next;
    }
    if (gridAmp[k] >= DIV_COUNTER) {
      gridAmp[k] = (uint16_t)(DIV_COUNTER - 1u);
    }
  }
  // Re-walk if clamping broke mono near the top.
  for (int k = nRungs - 2; k >= 1; --k) {
    if (gridAmp[k] >= gridAmp[k + 1]) {
      uint32_t prev = (uint32_t)gridAmp[k + 1] - 1u;
      if (prev < 1u) prev = 1u;
      if (prev <= gridAmp[k - 1]) {
        prev = (uint32_t)gridAmp[k - 1] + 1u;
      }
      if (prev >= (uint32_t)DIV_COUNTER) {
        prev = (uint32_t)DIV_COUNTER - 1u;
      }
      gridAmp[k] = (uint16_t)prev;
    }
  }
  gridAmp[nRungs - 1] = DIV_COUNTER;

  // Nearest-replace ampComp440 into the open rungs (not the last / wrap rung),
  // unless the anchor itself is DIV_COUNTER.
  {
    const uint16_t a440 = anchorAmp;
    int best = 0;
    uint16_t bestDist = 0xFFFFu;
    const int replaceLast = (a440 == DIV_COUNTER) ? (nRungs - 1) : (nRungs - 2);
    for (int k = 0; k <= replaceLast; ++k) {
      const uint16_t d = (gridAmp[k] > a440) ? (gridAmp[k] - a440)
                                             : (a440 - gridAmp[k]);
      if (d < bestDist) {
        bestDist = d;
        best = k;
      }
    }
    if (a440 >= 1 && a440 <= DIV_COUNTER) {
      gridAmp[best] = a440;
      // Restore mono around the replaced slot.
      for (int k = best + 1; k < nRungs - 1; ++k) {
        if (gridAmp[k] <= gridAmp[k - 1]) {
          uint32_t next = (uint32_t)gridAmp[k - 1] + 1u;
          if (next >= (uint32_t)DIV_COUNTER) next = (uint32_t)DIV_COUNTER - 1u;
          gridAmp[k] = (uint16_t)next;
        }
      }
      for (int k = best - 1; k >= 0; --k) {
        if (gridAmp[k] >= gridAmp[k + 1]) {
          uint32_t prev = (uint32_t)gridAmp[k + 1] - 1u;
          if (prev < 1u) prev = 1u;
          gridAmp[k] = (uint16_t)prev;
        }
      }
      gridAmp[nRungs - 1] = DIV_COUNTER;
    }
  }

  const float ampStepRatio = (A_L > 0 && A_H > A_L)
    ? powf((float)A_H / (float)A_L, 1.0f / (float)(nRungs - 1))
    : calibration_interval_ratio();
  // Search window: one geometric step, floored at the manual-note window.
  float gridWindow = ampStepRatio;
  if (gridWindow < kManualNoteWindowRatio) {
    gridWindow = kManualNoteWindowRatio;
  }

  // Approximate cents/step (f∝A) — logged only; CAL_REPORT uses amp-grid label.
  const float stepCents = 1200.0f * log2f(fmaxf(ampStepRatio, 1.0001f));

  // Index of the grid amp nearest the 440 Hz anchor — measure ups from here.
  int anchorIdx = 0;
  {
    uint16_t bestDist = 0xFFFFu;
    for (int k = 0; k < nRungs; ++k) {
      const uint16_t d = (gridAmp[k] > anchorAmp) ? (gridAmp[k] - anchorAmp)
                                                  : (anchorAmp - gridAmp[k]);
      if (d < bestDist) {
        bestDist = d;
        anchorIdx = k;
      }
    }
  }
  calReportAnchorPair = firstRung + anchorIdx;

  Serial.println((String)"[FREQ_TRACE] DCO=" + ctx.dcoIndex +
                 " amp-grid A_L=" + A_L + " A_H=" + A_H +
                 " rungs=" + nRungs + " stepRatio=" + ampStepRatio +
                 " ~" + stepCents + " cents anchorIdx=" + anchorIdx +
                 " ampComp440=" + anchorAmp +
                 " lastRungAmp=" + gridAmp[nRungs - 1]);

  for (int k = 0; k < nRungs; ++k) {
    ampByPair[firstRung + k] = gridAmp[k];
  }

  // Measure one grid amp: seed freq-for-amp, then duty50 search. Amp is never
  // changed (no retry_rung). Last rung uses the full-amp seed helpers + FINE.
  auto measure_grid_amp = [&](int k) -> bool {
    if (calibrationCancelRequested) {
      return false;
    }
    const int pair = firstRung + k;
    const uint16_t amp = gridAmp[k];
    float fSeed = freq_trace_guess(knownAmp, knownFreq, knownCount, (float)amp);
    float window = gridWindow;
    bool useFine = false;
    if (amp == DIV_COUNTER) {
      useFine = (calibrationPrecision != CAL_PRECISION_FAST);
      const float fPower = freq_trace_power_seed(knownFreq, knownAmp, knownCount,
                                                 (float)DIV_COUNTER);
      const float fQuad  = freq_trace_guess(knownAmp, knownFreq, knownCount,
                                            (float)DIV_COUNTER);
      fSeed = (fPower > 0.0f) ? fPower : fQuad;
      if (fPower > 0.0f && fQuad > 0.0f) {
        const float disagreeCents = fabsf(1200.0f * log2f(fQuad / fPower));
        if (disagreeCents > kEndpointSeedAgreeCents) {
          fSeed = fminf(fPower, fQuad);
        }
      }
      window = kTopEndpointWindowRatio;
      if (fSeed <= 0.0f && knownCount > 0) {
        // Fall back: scale from highest known amp.
        int iMax = 0;
        for (int i = 1; i < knownCount; ++i) {
          if (knownAmp[i] > knownAmp[iMax]) iMax = i;
        }
        if (knownAmp[iMax] > 0.0f) {
          fSeed = knownFreq[iMax] * ((float)DIV_COUNTER / knownAmp[iMax]);
        }
      }
    }
    if (fSeed <= 0.0f) {
      fSeed = note_to_freq(manual_cal_reference_note) *
              ((float)amp / fmaxf((float)anchorAmp, 1.0f));
    }

    float found;
    {
      CalPrecisionOverride fineForTop(
        useFine ? CAL_PRECISION_FINE : calibrationPrecision);
      found = find_freq_for_duty50(amp, fSeed, window, true);
      if (!(found > 0.0f) && amp == DIV_COUNTER && !calibrationCancelRequested) {
        // One retry with a wider window from the previous rung's freq.
        float fRetry = fSeed;
        if (k > 0 && freqByPair[firstRung + k - 1] > 0.0f) {
          fRetry = freqByPair[firstRung + k - 1] * ampStepRatio;
        }
        found = find_freq_for_duty50(amp, fRetry, kBottomEndpointWindowRatio, true);
      }
    }
    if (calibrationCancelRequested) {
      return false;
    }
    if (!(found > 0.0f)) {
      Serial.println((String)"[FREQ_TRACE_GUARD] DCO=" + ctx.dcoIndex +
                     " no signal at grid amp=" + amp + " pair=" + pair +
                     "; aborting (previous table kept)");
      return false;
    }
    // Keep table mono in frequency if a noisy top reading undershoots.
    if (k > 0 && found <= freqByPair[firstRung + k - 1]) {
      found = freqByPair[firstRung + k - 1] * fmaxf(ampStepRatio, 1.001f);
      Serial.println((String)"[FREQ_TRACE_GUARD] DCO=" + ctx.dcoIndex +
                     " pair=" + pair + " freq clamped above previous for mono");
    }

    freqByPair[pair] = found;
    ampByPair[pair]  = amp;  // exactly the grid amp (DIV_COUNTER on last)
    add_known(found, (float)amp);

    const uint8_t src = (amp == DIV_COUNTER) ? (uint8_t)CAL_SRC_ENDPOINT_FULL
                      : (amp == anchorAmp)   ? (uint8_t)CAL_SRC_ANCHOR
                                             : (uint8_t)CAL_SRC_RUNG;
    cal_report_set_pair_from_gap(pair, g_lastFreqBisectGapUs, found, src);
    Serial.println((String)"[FREQ_TRACE] DCO=" + ctx.dcoIndex +
                   " pair=" + pair + " amp=" + amp +
                   " freq=" + fmt_freq(found) +
                   freq_trace_quality(g_lastFreqBisectGapUs, found,
                                      g_lastFreqBisectProbes,
                                      g_lastSettleChecks));
    return true;
  };

  // Ups from near-440 toward DIV_COUNTER, then lows toward A_L.
  for (int k = anchorIdx; k < nRungs; ++k) {
    if (!measure_grid_amp(k)) {
      return false;
    }
  }
  for (int k = anchorIdx - 1; k >= 0; --k) {
    if (!measure_grid_amp(k)) {
      return false;
    }
  }

  // No sentinel: lastRung already holds the measured DIV_COUNTER point.
  ampByPair[lastRung] = DIV_COUNTER;

  if (calibrationCancelRequested) {
    return false;
  }

  // --- Pair 0: amp comp 0 (store floor 0.1 Hz) ------------------------------
  const float firstPairHz = freqByPair[firstRung];
  const FreqSearchBounds f0Bounds = amp0_search_band(firstPairHz);
  const float            f0FloorHz = f0Bounds.loHz;
  const float            f0CeilHz  = f0Bounds.hiHz;

  float f0Model = amp0_fit_freq(knownAmp, knownFreq, knownCount);
  if (!(f0Model > 0.0f)) {
    f0Model = freq_trace_guess(knownAmp, knownFreq, knownCount, 0.0f);
  }

  float f0Est = f0Model;
  const bool amp0Calc = (autotuneAmp0Mode == AMP0_MODE_CALC) ||
                        (calibrationPrecision == CAL_PRECISION_FAST);
  if (amp0Calc) {
    if (!(f0Est > 0.0f)) f0Est = sqrtf(f0FloorHz * f0CeilHz);
    if (f0Est < kAmp0StoreFloorHz) f0Est = kAmp0StoreFloorHz;
    if (f0Est > f0CeilHz) f0Est = f0CeilHz;
    cal_report_set_pair(0, kCalDutyErrUnknown, CAL_SRC_FILLED);
    Serial.println((String)"[FREQ_TRACE] DCO=" + ctx.dcoIndex +
                   " bottom endpoint calculated: " + fmt_freq(f0Est) +
                   " Hz (amp-0 hunt skipped, " +
                   ((calibrationPrecision == CAL_PRECISION_FAST)
                      ? "FAST run"
                      : "CALC mode; cmd 40 to measure") + ")");
  } else {
    {
      const float raw = f0Est;
      if (!(f0Est > 0.0f)) {
        f0Est = sqrtf(f0FloorHz * f0CeilHz);
      } else if (f0Est < f0FloorHz) {
        f0Est = f0FloorHz;
      } else if (f0Est > f0CeilHz) {
        f0Est = f0CeilHz;
      }
      if (f0Est != raw) {
        Serial.println((String)"[FREQ_TRACE] DCO=" + ctx.dcoIndex +
                       " bottom endpoint seed " + fmt_freq(raw) + " -> " +
                       fmt_freq(f0Est) + " Hz (band " + f0FloorHz + ".." +
                       f0CeilHz + ")");
      }
    }
    {
      float found = measure_lowest_freq_at_amp0(f0Est, &f0Bounds);
      const float foundErr = duty_err_pct_from_gap(g_lastFreqBisectGapUs, found);
      if (found >= f0FloorHz && found <= f0CeilHz &&
          fabsf(foundErr) <= kEndpointAcceptDutyPct) {
        cal_report_set_pair_from_gap(0, g_lastFreqBisectGapUs, found,
                                     CAL_SRC_ENDPOINT_AMP0);
        Serial.println((String)"[FREQ_TRACE] DCO=" + ctx.dcoIndex +
                       " bottom endpoint amp=0 freq=" + fmt_freq(found) +
                       freq_trace_quality(g_lastFreqBisectGapUs, found,
                                          g_lastFreqBisectProbes,
                                          g_lastSettleChecks) +
                       " (seed=" + f0Est + ")");
        f0Est = found;
      } else {
        float stored = g_lastAmp0ScanSeedHz;
        const char *srcName = "scan secant";
        if (!(stored > 0.0f)) {
          stored = f0Model;
          srcName = "model estimate";
        }
        if (!(stored > 0.0f)) stored = f0Est;
        if (stored < kAmp0StoreFloorHz) stored = kAmp0StoreFloorHz;
        if (stored > f0CeilHz) stored = f0CeilHz;
        f0Est = stored;
        cal_report_set_pair(0, kCalDutyErrUnknown, CAL_SRC_FILLED);
        Serial.println((String)"[FREQ_TRACE_GUARD] DCO=" + ctx.dcoIndex +
                       " bottom endpoint rejected: best " +
                       ((found > 0.0f) ? fmt_freq(found) : String("n/a")) +
                       " Hz; storing the " + srcName + " " + fmt_freq(f0Est));
      }
    }
  }
  if (f0Est < kAmp0StoreFloorHz) {
    f0Est = kAmp0StoreFloorHz;
  }
  freqByPair[0] = f0Est;
  ampByPair[0]  = 0;

  // --- Emit (hard clamp: never DIV_COUNTER+1; last pair owns full scale) ----
  ampByPair[lastRung] = DIV_COUNTER;
  for (int p = 1; p < lastRung; ++p) {
    if (ampByPair[p] >= DIV_COUNTER) {
      ampByPair[p] = (uint16_t)(DIV_COUNTER - 1u);
    }
  }
  for (int p = 0; p < numPairs; ++p) {
    uint32_t a = ampByPair[p];
    if (a > (uint32_t)DIV_COUNTER) {
      a = (uint32_t)DIV_COUNTER;
    }
    ctx.calibrationData[2 * p]     = (uint32_t)(freqByPair[p] * 100.0f);
    ctx.calibrationData[2 * p + 1] = a;
  }

  return cal_table_is_monotonic(ctx.calibrationData, numPairs, ctx.dcoIndex,
                                "FREQ_TRACE_ERROR");
}

// --- Fine pass: refine the stored table ------------------------------------

// Bracket for a stored pair's re-measurement. The stored frequency is the
// previous answer for that exact amp comp, so the window only has to cover
// drift plus the error of the run that produced it - and it should not cover
// much more: the opening step is a quarter of the window, so a wide window is
// what let one noisy first reading send a pair hunting 20 cents away from a
// value that was already right. +/-34 cents (about 8.5-cent opening steps)
// still covers real drift; a pair that moved further shows up as the search
// giving up at the window edge and a large moved= in the [CAL_REFINE] report,
// which is worth seeing rather than silently chasing.
static constexpr float kRefineWindowRatio = 1.02f;

// Re-measure the table this oscillator already has instead of building a new
// one: every amp-comp value is kept exactly as stored and only the frequency
// it sits at is measured again, at the FINE precision profile. There is no
// anchor, no bootstrap cluster and no ladder derivation, so nothing is guessed
// and nothing moves except the numbers the hardware disagrees with.
// Method-agnostic: fixing an amp and finding its 50%-duty frequency is valid
// for a classic table too.
// Returns false when there is no usable stored table (previous data kept) or
// the refined table fails the monotonicity check.
bool refine_DCO_amp_table(DCOCalibrationContext& ctx) {
  constexpr int numPairs = (int)(chanLevelVoiceDataSize / 2);
  const int base = (int)ctx.dcoIndex * (int)chanLevelVoiceDataSize;

  float    storedFreq[numPairs];
  uint16_t storedAmp[numPairs];
  for (int p = 0; p < numPairs; ++p) {
    const int32_t fx100 = freq_to_amp_comp_array[base + 2 * p];
    int32_t a           = freq_to_amp_comp_array[base + 2 * p + 1];
    if (a < 0) a = 0;
    if (a > (int32_t)DIV_COUNTER) a = (int32_t)DIV_COUNTER;
    storedFreq[p] = (fx100 > 0) ? ((float)fx100 / 100.0f) : 0.0f;
    storedAmp[p]  = (uint16_t)a;
  }

  // The last pair worth measuring is the first one at full amp comp. CLASSIC
  // tables may still pad sentinel rows above it; FREQ_TRACE amp-grid tables end
  // on the measured DIV_COUNTER pair with no fake 200 kHz sentinel.
  int topPair = -1;
  for (int p = 1; p < numPairs; ++p) {
    if (storedAmp[p] >= DIV_COUNTER) {
      topPair = p;
      break;
    }
  }

  const char *reject = nullptr;
  if (topPair < 4) {
    reject = "no full-amp endpoint (or too few pairs below it)";
  } else {
    int distinctAmps = 1;
    for (int p = 1; p <= topPair && reject == nullptr; ++p) {
      if (storedFreq[p] <= storedFreq[p - 1]) {
        reject = "frequencies are not increasing";
      } else if (storedAmp[p] < storedAmp[p - 1]) {
        reject = "amp comp values are not increasing";
      } else if (storedAmp[p] != storedAmp[p - 1]) {
        ++distinctAmps;
      }
    }
    if (reject == nullptr && distinctAmps < 4) {
      reject = "table is flat (looks seeded, not calibrated)";
    }
  }
  if (reject != nullptr) {
    Serial.println((String)"[CAL_REFINE_GUARD] DCO=" + ctx.dcoIndex +
                   " no usable stored table (" + reject +
                   "); run a normal calibration first");
    return false;
  }

  Serial.println((String)"[CAL_REFINE] DCO=" + ctx.dcoIndex +
                 " refining " + (topPair + 1) + " stored pairs" +
                 " (amp comp values kept, frequencies re-measured)");

  float refinedFreq[numPairs];
  for (int p = 0; p < numPairs; ++p) {
    refinedFreq[p] = storedFreq[p];
  }

  int   measured  = 0;
  float errSum    = 0.0f;
  float worstMove = 0.0f;
  int   worstPair = -1;

  for (int p = 0; p <= topPair; ++p) {
    if (calibrationCancelRequested) {
      return false;
    }
    // Pair 0 at amp comp 0 is not a rung, it is the bottom endpoint: the point
    // where the oscillator runs out of range, which may not have a 50% duty at
    // all. It gets the lowest-note treatment - a scan of the whole band for a
    // bracket, a band it may not leave, and a result that has to be near 50% to
    // be believed - instead of being re-measured in a +/-5% window around a
    // stored frequency the last run may itself have only extrapolated.
    const bool isBottomEndpoint = (p == 0 && storedAmp[0] == 0);
    if (isBottomEndpoint && autotuneAmp0Mode == AMP0_MODE_CALC) {
      // CALC mode: no live hunt. The endpoint is recomputed after this loop,
      // from the rungs the pass is about to re-measure - at this point none of
      // them have been refined yet, so a fit here would describe the old run.
      continue;
    }
    float found;
    if (isBottomEndpoint) {
      const FreqSearchBounds bounds = amp0_search_band(storedFreq[1]);
      found = measure_lowest_freq_at_amp0(storedFreq[p], &bounds);
      const float err = duty_err_pct_from_gap(g_lastFreqBisectGapUs, found);
      if (!(found >= bounds.loHz && found <= bounds.hiHz) ||
          fabsf(err) > kEndpointAcceptDutyPct) {
        cal_report_set_pair(p, kCalDutyErrUnknown, CAL_SRC_FILLED);
        Serial.println((String)"[CAL_REFINE_GUARD] DCO=" + ctx.dcoIndex +
                       " pair=0 amp=0 rejected: best " + found + " Hz dutyErr=" +
                       ((found > 0.0f) ? String(err, 2) : String("n/a")) +
                       "% (band " + bounds.loHz + ".." + bounds.hiHz +
                       ", accept " + kEndpointAcceptDutyPct +
                       "%); keeping the stored " + storedFreq[p]);
        continue;
      }
    } else {
      found = find_freq_for_duty50(storedAmp[p], storedFreq[p],
                                   kRefineWindowRatio, true);
    }
    if (found <= 0.0f) {
      cal_report_set_pair(p, kCalDutyErrUnknown, CAL_SRC_FILLED);
      Serial.println((String)"[CAL_REFINE_GUARD] DCO=" + ctx.dcoIndex +
                     " pair=" + p + " amp=" + storedAmp[p] +
                     " no signal near " + storedFreq[p] +
                     " Hz; keeping the stored frequency");
      continue;
    }

    refinedFreq[p] = found;
    cal_report_set_pair_from_gap(p, g_lastFreqBisectGapUs, found,
                                 CAL_SRC_REFINED);
    ++measured;

    const float moveCents = 1200.0f * log2f(found / storedFreq[p]);
    const float errPct    = duty_err_pct_from_gap(g_lastFreqBisectGapUs, found);
    if (errPct != kCalDutyErrUnknown) {
      errSum += fabsf(errPct);
    }
    if (fabsf(moveCents) > fabsf(worstMove)) {
      worstMove = moveCents;
      worstPair = p;
    }

    Serial.println((String)"[CAL_REFINE] DCO=" + ctx.dcoIndex +
                   " pair=" + p + " amp=" + storedAmp[p] +
                   " stored=" + storedFreq[p] + " -> found=" + found +
                   " moved=" + moveCents + " cents" +
                   freq_trace_quality(g_lastFreqBisectGapUs, found,
                                      g_lastFreqBisectProbes,
                                      g_lastSettleChecks));
  }

  for (int p = topPair + 1; p < numPairs; ++p) {
    cal_report_set_pair(p, kCalDutyErrUnknown, CAL_SRC_SENTINEL);
  }
  calReportLadderInterval = 0;   // the stored ladder is whatever built it
  calReportAnchorPair     = -1;

  // CALC mode: the amp-0 endpoint skipped in the loop above is recomputed here,
  // now that the rungs it extrapolates from are freshly measured. Clamped like
  // the FREQ_TRACE fallback: a sanity floor, and under pair 1 for monotonicity.
  if (storedAmp[0] == 0 && autotuneAmp0Mode == AMP0_MODE_CALC) {
    float fitAmps[kAmp0FitPoints + 3];
    float fitFreqs[kAmp0FitPoints + 3];
    int   fitCount = 0;
    for (int p = 1;
         p <= topPair && fitCount < (int)(sizeof(fitAmps) / sizeof(fitAmps[0]));
         ++p) {
      if (!(refinedFreq[p] > 0.0f) || storedAmp[p] == 0) continue;
      fitAmps[fitCount]  = (float)storedAmp[p];
      fitFreqs[fitCount] = refinedFreq[p];
      ++fitCount;
    }
    float f0 = amp0_fit_freq(fitAmps, fitFreqs, fitCount);
    if (!(f0 > 0.0f)) f0 = refinedFreq[0];  // no usable fit: keep the stored value
    const float f0CeilHz = refinedFreq[1] * 0.99f;
    if (f0 < kAmp0StoreFloorHz) f0 = kAmp0StoreFloorHz;
    if (f0 > f0CeilHz) f0 = f0CeilHz;
    refinedFreq[0] = f0;
    cal_report_set_pair(0, kCalDutyErrUnknown, CAL_SRC_FILLED);
    Serial.println((String)"[CAL_REFINE] DCO=" + ctx.dcoIndex +
                   " pair=0 amp=0 calculated: " + f0 +
                   " Hz (amp-0 hunt skipped, CALC mode; cmd 40 to measure)");
  }

  // Emit: refined frequencies, stored amp comp values, sentinels untouched.
  for (int p = 0; p < numPairs; ++p) {
    if (p > topPair) {
      ctx.calibrationData[2 * p]     = (uint32_t)freq_to_amp_comp_array[base + 2 * p];
      ctx.calibrationData[2 * p + 1] = (uint32_t)freq_to_amp_comp_array[base + 2 * p + 1];
      continue;
    }
    ctx.calibrationData[2 * p]     = (uint32_t)(refinedFreq[p] * 100.0f);
    ctx.calibrationData[2 * p + 1] = storedAmp[p];
  }

  String summary = (String)"[CAL_REFINE] DCO=" + ctx.dcoIndex +
                   " measured=" + measured + "/" + (topPair + 1);
  if (measured > 0) {
    summary += (String)" dutyErr avg=" + (errSum / (float)measured) + "%";
    summary += (String)" largest move=" + worstMove + " cents at pair " + worstPair;
  }
  Serial.println(summary);

  return cal_table_is_monotonic(ctx.calibrationData, numPairs, ctx.dcoIndex,
                                "CAL_REFINE_ERROR");
}


// 3-point quadratic interpolate y at x. Used by calibrate_DCO helpers / find_lowest_freq.
float quadraticInterpolation(float x0, float y0, float x1, float y1, float x2, float y2, float x) {
  // Calculate the coefficients of the quadratic polynomial
  float a = ((y2 - (x2 * (y1 - y0) + x1 * y0 - x0 * y1) / (x1 - x0)) / (x2 * (x2 - x0 - x1) + x0 * x1));
  float b = ((y1 - y0) / (x1 - x0) - a * (x0 + x1));
  float c = y0 - x0 * (b + a * x0);

  // Use the polynomial to estimate the next value
  return a * x * x + b * x + c;
}

// Log interpolate between two points → uint16. Used by compute_initial_amp_for_note().
uint16_t logarithmicInterpolation(float x0, float y0, float x1, float y1, float x) {
  // Ensure x0 and x1 are not zero or negative to avoid log(0) or log of negative number
  if (x0 <= 0 || x1 <= 0) {
    return 0;  // or handle the error as needed
  }

  // Calculate the constants a and b
  float a = (y1 - y0) / (log(x1) - log(x0));
  float b = y0 - a * log(x0);

  // Calculate the y value at the given x
  float y = a * log(x) + b;

  return (uint16_t)round(y);
}

// Linear interpolate between two points. Used by find_lowest_freq().
float linearInterpolation(float x0, float y0, float x1, float y1, float x) {
  // Ensure x0 and x1 are not the same to avoid division by zero
  if (x0 == x1) {
    return 0;  // or handle the error as needed
  }

  // Calculate the slope (m) of the line
  float m = (y1 - y0) / (x1 - x0);

  // Calculate the y-intercept (b) of the line
  float b = y0 - m * x0;

  // Calculate the y value at the given x
  float y = m * x + b;

  return y;
}

// Solve exponential interpolation for y at x (log-space lerp). Used by initMultiplierTables().
double expInterpolationSolveY(double x, double x0, double x1, double y0, double y1) {
  if (x0 <= 0 || x1 <= 0) {
    // Handle error: x0 and x1 must be greater than 0 for exponential interpolation
    return NAN;
  }

  double log_y0 = log(y0);
  double log_y1 = log(y1);

  double log_y = log_y0 + (log_y1 - log_y0) * (x - x0) / (x1 - x0);

  return exp(log_y);
}

#endif  // __AUTOTUNE_SEARCH_IMPL_H__
