#ifndef __AUTOTUNE_CONSTANTS_H__
#define __AUTOTUNE_CONSTANTS_H__

#include <stdint.h>

// Common constants used by the DCO/VCO autotune and measurement code.
// Kept here to avoid magic numbers scattered across the implementation.

// Sentinel value returned by gap-measurement routines to indicate
// a timeout or invalid measurement.
constexpr float kGapTimeoutSentinel = 1.16999f;

// PARAM_GAP_FROM_DCO payload (duty error [%] * 100) when find_gap times out.
// Distinct from a real near-zero trim so USB/Screen never look "perfect".
constexpr int32_t kManualGapTimeoutDutyErrTimes100 = 99999;

// Time without seeing an edge before a measurement is considered timed out, and
// (being the same thing from the other side) the longest segment find_gap() will
// accept as part of a waveform.
//
// 100 ms is generous for most of the range and not generous at all at the bottom
// of it: one period at 12 Hz is 83 ms, and amp comp 0 - the one operating point
// where the pulse is deliberately as lopsided as the oscillator can make it -
// can easily put a single segment past 100 ms there. So the deadline is a floor
// rather than a constant: kGapTimeoutPeriods periods of whatever is being probed
// when that is longer, up to kGapTimeoutMaxUs. The cap matters because this is
// also what a dead oscillator costs per probe.
constexpr unsigned long kGapTimeoutUs     = 100000UL;
constexpr unsigned long kGapTimeoutMaxUs  = 400000UL;
constexpr double        kGapTimeoutPeriods = 2.5;

// Minimum time (in microseconds) between detected edges to treat
// them as valid (simple debounce).
constexpr unsigned long kEdgeDebounceMinUs = 20UL;

// Accepted low/high segments averaged per find_gap() measurement in the
// classic path (specialMode 0). The hi-res modes (2 = PW search, 3 = frequency
// probe) take their segment counts from the precision profile below.
constexpr uint16_t kGapSamplesDefault = 6;
constexpr uint16_t kGapSamplesHiRes   = 12;

// Segment floor for hi-res readings below kSearchStepVeryLowHz (30 Hz). Down
// there the profile's measurement window fits nothing at a 50+ ms period, so a
// reading otherwise sits on the profile's 6-segment floor - and pair 1 of the
// table was noise-limited to ~0.2% duty by exactly that. Twice the segments
// costs ~150 ms per reading at 20 Hz and drops the swing by ~1/sqrt(2). The
// gapMaxWindowMs cap still bounds it, which is what keeps the amp-0 scan at
// 8 Hz from paying for this.
constexpr uint16_t kGapSamplesVeryLowMin = 12;

// --- Calibration precision profiles ----------------------------------------
//
// Everything that trades run time against measurement quality lives here, in
// three sets: NORMAL builds a table from scratch quickly, FINE re-measures an
// existing one as accurately as the hardware allows, FAST is NORMAL cut down
// to produce a testing table as quickly as possible. The value of
// PARAM_CALIBRATION_FLAG picks the set (1/2/3 normal, 5/6/7 fine, 9/10/11
// fast); see calibrationPrecision in autotune.h and docs/AUTOTUNE.md.
struct CalPrecisionProfile {
  uint16_t gapSamplesMin;     // find_gap modes 2/3: floor on averaged segments
  uint16_t gapSamplesMax;     // ... and ceiling
  uint32_t gapWindowMs;       // measurement window the segment count aims at
  uint32_t gapMaxWindowMs;    // ... and the longest one reading may ever take
  float    settlePeriods;     // wait after the last frequency write, in periods
  uint16_t settleMinMs;       // ... floored at this many milliseconds
  double   bisectDutyTol;     // search acceptance, as a fraction of duty
  double   bisectGapFloorUs;  // ... floored at this many microseconds of gap
  int      bisectIters;       // probes allowed per search
  int      bisectWindows;     // travel allowance, in caller windows
  int      confirmReads;      // readings averaged at the converged frequency
  int      confirmRounds;     // corrections allowed if that average misses
  int      anchorTries;       // FREQ_TRACE 440 Hz anchor corrections
  int      rungRetries;       // FREQ_TRACE per-rung corrections
  uint8_t  settleMaxChecks;   // re-readings allowed after a large move
  float    settleStableMult;  // x acceptance tolerance = "reading has settled"
};

// A frequency move smaller than this needs no stability check: the late
// iterations of a bisection move by well under a cent, and the analog core has
// nothing to follow.
constexpr float kSettleSkipCents = 5.0f;

// A move of this size or more gets the profile's full stability budget; smaller
// ones (but above kSettleSkipCents) get a single confirming reading.
constexpr float kSettleBigMoveCents = 100.0f;

// Window ratios for the fixed-amp probes that are not on the derived ladder: how
// far from its seed each one expects the answer to be. The manual trim point is a
// known note, and both endpoints are seeded by a model built from ~20 measured
// points, so all of them can search tightly.
//
// windowRatio also sizes the search's first step (a quarter of it), which is why
// the two endpoints differ. The top one is seeded by a power law anchored on the
// nearest measured point and lands within ~10 cents. The bottom one extrapolates
// to amp comp 0, where the log-log form has no anchor at all, so its seed can be
// most of an octave out.
constexpr float kManualNoteWindowRatio     = 1.15f;
constexpr float kTopEndpointWindowRatio    = 1.05f;
constexpr float kBottomEndpointWindowRatio = 1.25f;
constexpr float kAnchorWindowRatio         = 1.15f;

// The exception: the very first anchor probe, which is the one search in a run
// with no model behind it at all. Its amp comp comes from the panel slider, and
// a hand-dialled value can be a long way from a 440 Hz operating point - after
// the reference note moved from 220 Hz to 440 Hz, a value stored by an earlier
// firmware is a whole octave off. An octave either way is wide enough to find
// where that amp really sits; the re-anchor loop then walks the amp to 440 Hz
// and persists it, and every later probe uses the tight window above.
constexpr float kAnchorAcquireWindowRatio  = 2.0f;

// Where the bootstrap probes sit relative to the 440 Hz anchor, in semitones of
// amp comp: amp = anchorAmp * 2^(n/12), which lands the frequencies near
// 440 * 2^(n/12) - about 311, 370, 523 and 622 Hz. These four points are what
// the model has to work with before the ladder starts, so they have to straddle
// the anchor (one above, one below, twice over) and be far enough apart to say
// something about the curve's shape. The inner pair comes first, so that a run
// that loses the outer probes still has a straddle. They only ever feed the
// model - none of them is written to the table.
constexpr int kBootstrapSemitones[4] = { 3, -3, 6, -6 };

// The power-law and quadratic seeds for the full-amp endpoint disagreeing by more
// than this means the curve is bending near the ceiling and the quadratic is
// extrapolating through the bend. Take the lower of the two when that happens:
// overshooting lands in the collapse, and a timeout carries no information about
// where the answer is.
constexpr float kEndpointSeedAgreeCents = 50.0f;

// Largest single step the frequency search may take, by range, in cents. It
// steps out from its seed until it brackets the answer and then interpolates,
// so this only bounds one probe's move - but that bound is what keeps the
// bottom of the range readable, where a probe is a handful of periods long and
// a jump of several hundred cents lands on a waveform that is still moving.
// The very-low range exists for the amp-comp-0 hunt: down there a 50-cent
// step barely moves the duty, so the search walks without closing. A semitone
// (100 cents) is enough to change the reading and still half of the 200-cent
// mid-range cap; the denser pre-scan is what finds the sign change, not creeps.
constexpr float kSearchStepVeryLowHz     = 30.0f;
constexpr float kSearchStepLowHz         = 100.0f;
constexpr float kSearchStepHighHz        = 440.0f;
constexpr float kSearchStepCentsVeryLow  = 100.0f;  // below kSearchStepVeryLowHz
constexpr float kSearchStepCentsLow      = 100.0f;  // kSearchStepVeryLowHz .. LowHz
constexpr float kSearchStepCentsMid      = 200.0f;  // kSearchStepLowHz .. HighHz
constexpr float kSearchStepCentsHigh     = 400.0f;  // above kSearchStepHighHz

// Smallest frequency move the search will make, and the "same frequency" test.
// A 2-cent stop at 8 Hz is ~0.01 Hz — finer than the measurement — and was
// treating opposite-sign readings at one frequency as a finished bracket.
constexpr float kMinFreqStepHz = 0.1f;

// ... and, tighter than any of those, what the latest reading itself implies.
// The duty error moves ~3-4% per 100 cents across the whole range, so a seed
// that reads -0.07% is a few cents from the answer and a 100-cent jump away
// from it is pure waste (measured: 15 probes for a rung whose seed was already
// within noise). Dividing the error by a deliberately flat slope (about half
// the flattest measured) makes the step overshoot the true distance ~2x, so
// it still brackets in one hop; the floor guarantees progress when the error
// is within noise of zero. Timeouts have no magnitude and keep the range cap.
constexpr float kSearchSlopeMinPctPer100Cents = 1.5f;
constexpr float kSearchStepFloorCents         = 3.0f;

// ... except while a search with explicit bounds has yet to measure a single
// pulse, when the caps above protect nothing (there is no reading to spoil) and
// only slow the walk out of a region the oscillator cannot produce. Half an
// octave per step: enough to cross a dead zone in a few probes, small enough not
// to step over a narrow band of usable frequencies on the way. Bounded only,
// because striding away from a seed with no limit to stop at is how an unbounded
// search overshoots instead of arriving.
constexpr float kHuntStepMaxCents = 600.0f;

// Where the amp-comp-0 endpoint is allowed to be: from just under the first
// measured pair down to a fraction of it. Both amp-comp methods extrapolate the
// point before measuring it, and that extrapolation has no anchor under it, so
// the band has to be wide enough to hold the answer wherever it really is - a
// measured table puts it at pair 1 / 2.2, so a band of one ladder rung (which is
// what this used to be) looks for it about an octave above where it is.
//
// The floor is the frequency below which a reading stops meaning anything:
// kGapTimeoutPeriods periods of it are longer than the kGapTimeoutMaxUs deadline,
// so a lopsided pulse there cannot be told from silence, and each probe that
// tries costs the full cap.
constexpr float kAmp0BandRatio = 2.5f;
constexpr float kAmp0MinFreqHz = 5.0f;

// The freq(amp) curve is measured to be nearly linear at the bottom of the
// range (pair-to-pair slopes agree within ~1%), so the amp-0 intercept comes
// from a least-squares line through this many of the lowest-amp measured
// points. The 3-point quadratic that used to be extrapolated there amplified
// the noise of exactly the noisiest points and swung by whole octaves between
// runs (-2.87, 4.18, 4.68 Hz on the same hardware); the line is stable.
constexpr int kAmp0FitPoints = 5;

// When the amp-0 endpoint cannot be measured (on hardware whose pulse dies
// before the duty reaches 50%, i.e. practically always), the fit above is what
// gets stored as entry 0 - it is an interpolation anchor for the runtime
// lookup, not a producible frequency, so it is not clamped to the search band.
// Floor is intentional low precision for the runtime interpolator (0.1 Hz),
// not a "no signal" failure.
constexpr float kAmp0StoreFloorHz = 0.1f;

// The endpoint is scanned for before the search proper: this many frequencies,
// log-spaced across the band, one quick reading each after a wait of at least
// this long (or one period, whichever is longer - 20 ms at 8 Hz is a seventh of
// a period, and what comes back describes the previous frequency). A single
// modelled probe down here tells the search nothing except that it saw nothing,
// whereas a scan finds which frequencies pulse at all and, better, brackets the
// answer between two readings of opposite sign for the search to close on.
constexpr int      kAmp0ScanPoints   = 10;
constexpr uint32_t kAmp0ScanSettleMs = 20;

// How far the measured period (avgLow + avgHigh) may deviate from the ideal
// one before the reading is not describing the requested waveform at all. A
// healthy reading lands within ~0.5% of the ideal period; a degenerate one can
// be wildly off - at amp comp 0 near 6 Hz the pin was observed toggling at
// ~58% of the requested period (a double-trigger of the comparator), whose
// near-symmetric sub-segments read ~50% duty no matter what the frequency is.
// The 1%..99%-of-period segment gate cannot catch that; only the sum can.
constexpr float kGapPeriodTolRatio = 0.15f;

// How close to 50% the duty of the amp-comp-0 endpoint must come for it to count
// as measured, in percentage points. The rungs land within 0.05%, but this point
// is different in kind: at amp comp 0 the pulse may die before the duty ever
// reaches 50%, in which case the search converges on the border of the dead zone
// and returns a frequency whose duty is nowhere near the target. Storing that as
// pair 0 is worse than storing the extrapolation it was seeded with.
constexpr float kEndpointAcceptDutyPct = 0.5f;

// Timeouts in a row an unbounded search may spend before giving up with its best
// reading. A timeout costs at least kGapTimeoutUs (100 ms, more at the bottom of
// the range), doubled after a large move by the retry in measure_duty_at_freq(),
// so a search hunting inside a region the oscillator cannot produce at all is
// the most expensive way to learn nothing. Six is enough to walk out of a dead
// zone or converge onto its edge. In a row: a good reading resets the count,
// because a search that is still producing them is converging rather than lost.
constexpr int kMaxSearchTimeouts = 6;

// How long one reading takes is gapWindowMs by construction - the segment count
// is the window divided by a half period - so the two clamps around it are what
// actually decide the cost at the ends of the range. At the bottom the floor
// wins (at 16 Hz a segment is 30 ms, so 12 of them are 367 ms), which is what
// gapMaxWindowMs bounds. At the top the ceiling still wins, and how much
// averaging it allows is the accuracy/speed trade at that end: a fast build
// stops at 64 segments (16 ms at 2 kHz), a fine one lets the window govern.
//
// Fast build. The segment floor of 6 (instead of 12) is what speeds up the
// bottom of the range, where one segment is already tens of milliseconds, and
// the looser acceptance lets a good probe leave the search early. Only one
// stability check is allowed, so a from-scratch run does not pay for settling
// twenty times per pair.
constexpr CalPrecisionProfile kCalPrecisionNormal = {
  /* gapSamplesMin    */ 6,
  /* gapSamplesMax    */ 64,
  /* gapWindowMs      */ 25,
  // 300 rather than 200 so the cap admits kGapSamplesVeryLowMin (12) segments
  // at ~20 Hz, where the lowest measured pair sits; at 8 Hz it still holds a
  // reading to ~5 segments, so the amp-0 scan keeps its cost.
  /* gapMaxWindowMs   */ 300,
  /* settlePeriods    */ 1.0f,
  /* settleMinMs      */ 3,
  /* bisectDutyTol    */ 0.0005,
  /* bisectGapFloorUs */ 0.5,
  /* bisectIters      */ 24,
  /* bisectWindows    */ 2,
  /* confirmReads     */ 2,
  // Two rounds, not one: rungs can drift a few hundredths of a percent between
  // the search and the confirm (the DCO still creeping at a fresh operating
  // point), and with a single round the confirm can measure that miss but not
  // correct it. The second round runs only when the first average misses the
  // acceptance, so well-behaved rungs pay nothing.
  /* confirmRounds    */ 2,
  /* anchorTries      */ 2,
  /* rungRetries      */ 1,
  /* settleMaxChecks  */ 1,
  /* settleStableMult */ 3.0f,
};

// Fine tuning. Used by the refine pass over a stored table, by the verification
// sweep and by the top-of-range endpoint whatever the run's own precision; the
// anchor/rung fields are unused in the refine pass (no ladder is built) but stay
// filled in so a fine full build is still coherent. What a reading rests on
// after a frequency change is settlePeriods plus the stability checks: the
// frequency is written once and then left alone until whole periods have come
// out of it.
constexpr CalPrecisionProfile kCalPrecisionFine = {
  /* gapSamplesMin    */ 12,
  /* gapSamplesMax    */ 256,
  /* gapWindowMs      */ 60,
  // Matches NORMAL's 300: with 200 the cap clipped a 20 Hz fine reading to 8
  // segments, below what a normal reading now averages there.
  /* gapMaxWindowMs   */ 300,
  /* settlePeriods    */ 1.0f,
  /* settleMinMs      */ 4,
  /* bisectDutyTol    */ 0.0002,
  /* bisectGapFloorUs */ 0.25,
  /* bisectIters      */ 32,
  /* bisectWindows    */ 3,
  /* confirmReads     */ 5,
  /* confirmRounds    */ 2,
  /* anchorTries      */ 3,
  /* rungRetries      */ 1,
  /* settleMaxChecks  */ 3,
  /* settleStableMult */ 2.0f,
};

// Fastest usable build - a table for testing, not for keeps. NORMAL's
// structure with every knob turned toward speed: half the measurement window
// (a reading is noisier by ~sqrt(2), still well inside what the runtime
// interpolation smooths over), a 2x looser acceptance (0.1% duty, far below
// an audible amp error), a single confirm reading with a single round, no
// anchor or rung corrections. The gapMaxWindowMs cap of 200 also clips the
// kGapSamplesVeryLowMin floor at the very bottom, so pair 1 gets noisier -
// and the amp-0 fit through the lowest rungs absorbs it. The structural
// shortcuts (amp-0 fit instead of the live hunt, 2 bootstrap probes instead
// of 4, no forced-FINE top endpoint) are gated on CAL_PRECISION_FAST in
// autotune_search_impl.h, not expressed here.
constexpr CalPrecisionProfile kCalPrecisionFast = {
  /* gapSamplesMin    */ 4,
  /* gapSamplesMax    */ 32,
  /* gapWindowMs      */ 12,
  /* gapMaxWindowMs   */ 200,
  /* settlePeriods    */ 1.0f,
  /* settleMinMs      */ 2,
  /* bisectDutyTol    */ 0.0010,
  /* bisectGapFloorUs */ 1.0,
  /* bisectIters      */ 16,
  /* bisectWindows    */ 2,
  /* confirmReads     */ 1,
  /* confirmRounds    */ 1,
  /* anchorTries      */ 1,
  /* rungRetries      */ 0,
  /* settleMaxChecks  */ 1,
  /* settleStableMult */ 4.0f,
};

// Target duty fractions for PW calibration:
//  - Center:  50% duty
//  - Low:      2% duty (user-adjustable if desired)
//  - High:    98% duty (user-adjustable if desired)
constexpr double kPWCenterDutyFraction = 0.5;
constexpr double kPWLowDutyFraction    = 0.02;
constexpr double kPWHighDutyFraction   = 0.98;

// Polarity of the digital calibration signal on DCO_calibration_pin.
// If your hardware inverts the waveform (so the pin is high when the
// actual DCO output is low, and vice versa), set this to true. All duty
// measurements (find_gap / measure_gap) will automatically compensate.
constexpr bool kGapPolarityInverted    = false;  // true if cal pin is inverted vs DCO output

// Duty tolerance used when validating PW low/high limits and PW center lock-in.
// A sample whose duty is within ±kPWLimitDutyTolerance of the target
// low/center/high duty is considered "in tolerance".
constexpr double kPWLimitDutyTolerance = 0.01;  // ±1% duty

#endif  // __AUTOTUNE_CONSTANTS_H__


