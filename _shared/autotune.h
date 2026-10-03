#ifndef __AUTOTUNE_H__
#define __AUTOTUNE_H__

#include "../include_all.h"
#include "autotune_constants.h"
#include "autotune_measurement.h"
#include "autotune_context.h"

// Global flags controlling calibration routines.
//  - calibrationFlag: a calibration process is currently running.
//  - manualCalibrationFlag: manual calibration mode is active.
//  - firstTuneFlag: true on the very first calibration run after boot/flash.
bool calibrationFlag = false;
bool manualCalibrationFlag = false;
bool firstTuneFlag = false;

// Cancel request for a running auto-calibration. Set on core 0 by
// apply_param_calibration_flag(0) (PARAM_CALIBRATION_FLAG = 0) while
// DCO_calibration() blocks core 1; the calibration loops poll it and unwind,
// skipping the FS persist of whatever stage was interrupted. Cleared by
// DCO_calibration() on entry.
volatile bool calibrationCancelRequested = false;

// Which stage(s) the next DCO_calibration() run performs; carried by the value
// of PARAM_CALIBRATION_FLAG and mirroring the Screen's calibration menu tabs
// (AUTO / PW / FULL). PW and amp-comp are independent: an amp-only run drives
// the pulse from the PW center already stored in the filesystem.
enum CalibrationScope : uint8_t {
  CAL_SCOPE_AMP  = 1,  // amp-comp tables only
  CAL_SCOPE_PW   = 2,  // PW center + low/high limits only
  CAL_SCOPE_FULL = 3,  // PW stage, then amp-comp
};
uint8_t calibrationScope = CAL_SCOPE_FULL;

static inline const char *calibration_scope_name(uint8_t s) {
  if (s == CAL_SCOPE_AMP) return "AMP";
  if (s == CAL_SCOPE_PW)  return "PW";
  return "FULL";
}

static inline bool calibration_scope_runs_pw(uint8_t s) {
  return s == CAL_SCOPE_PW || s == CAL_SCOPE_FULL;
}

static inline bool calibration_scope_runs_amp(uint8_t s) {
  return s == CAL_SCOPE_AMP || s == CAL_SCOPE_FULL;
}

// How carefully the next run measures, also carried by the value of
// PARAM_CALIBRATION_FLAG: 1/2/3 run a scope at NORMAL, 5/6/7 the same scope at
// FINE, 9/10/11 at FAST. NORMAL builds a table from scratch as fast as the
// hardware allows; FINE skips the model-building entirely and re-measures the
// frequency of every pair already in the stored table (refine_DCO_amp_table);
// FAST is NORMAL's build with cheaper readings and the structural shortcuts
// gated on it in calibrate_DCO_freq_trace() - a table for testing, quickly.
enum CalPrecision : uint8_t {
  CAL_PRECISION_NORMAL = 0,
  CAL_PRECISION_FINE   = 1,
  CAL_PRECISION_FAST   = 2,
};
uint8_t calibrationPrecision = CAL_PRECISION_NORMAL;

// The measurement settings the calibration code should use right now. Every
// speed/quality knob is read through this, so nothing has to know which mode
// is active.
static inline const CalPrecisionProfile &cal_precision() {
  if (calibrationPrecision == CAL_PRECISION_FINE) return kCalPrecisionFine;
  if (calibrationPrecision == CAL_PRECISION_FAST) return kCalPrecisionFast;
  return kCalPrecisionNormal;
}

static inline const char *calibration_precision_name(uint8_t p) {
  if (p == CAL_PRECISION_FINE) return "FINE";
  if (p == CAL_PRECISION_FAST) return "FAST";
  return "NORMAL";
}

// How the amp-comp-0 endpoint (the lowest reachable frequency, pair 0 of the
// table) is obtained. MEASURE runs the live hunt (band scan + bounded search);
// CALC skips the hunt entirely and stores the least-squares fit through the
// lowest measured rungs (amp0_fit_freq()). CALC exists because on hardware
// whose pulse dies before the duty reaches 50% the hunt can never accept a
// measurement anyway, and the fit is what the rejection branch would store -
// minus the probe timeouts spent proving it. Runtime-only A/B via
// PARAM_DEBUG_COMMAND 40/41. Lives here with the other calibration-run flags
// (scope, precision, method, search).
#ifndef AUTOTUNE_AMP0_MODE_DEFAULT
#define AUTOTUNE_AMP0_MODE_DEFAULT 0
#endif
enum AutotuneAmp0Mode : uint8_t {
  AMP0_MODE_MEASURE = 0,
  AMP0_MODE_CALC    = 1,
};
uint8_t autotuneAmp0Mode = (uint8_t)AUTOTUNE_AMP0_MODE_DEFAULT;

static inline const char *autotune_amp0_mode_name(uint8_t m) {
  return (m == AMP0_MODE_CALC) ? "CALC" : "MEASURE";
}

// Amp-comp calibration method (A/B via cmds 34/35; see docs/AUTOTUNE.md).
// 0 CLASSIC: per-note range-PWM search (calibrate_DCO).
// 1 FREQ_TRACE: fixed-PWM frequency bisection from the manual 440 Hz anchor
//   (calibrate_DCO_freq_trace). Boot default from AUTOTUNE_AMP_METHOD_DEFAULT
//   in DCO.ino; fallback here if that is unset.
#ifndef AUTOTUNE_AMP_METHOD_DEFAULT
#define AUTOTUNE_AMP_METHOD_DEFAULT 0
#endif
enum AutotuneAmpMethod : uint8_t {
  AMP_METHOD_CLASSIC    = 0,
  AMP_METHOD_FREQ_TRACE = 1,
};
uint8_t autotuneAmpMethod = (uint8_t)AUTOTUNE_AMP_METHOD_DEFAULT;

static inline const char *autotune_amp_method_name(uint8_t m) {
  return (m == AMP_METHOD_FREQ_TRACE) ? "FREQ_TRACE" : "CLASSIC";
}

// How the frequency search closes in once it has the answer bracketed
// (A/B via cmds 37/38/39; see docs/AUTOTUNE.md).
// 0 BISECT: geometric midpoint every time. Only the sign of a reading matters,
//   so a noisy magnitude cannot move the probe. Slowest, most robust.
// 1 INTERP: Illinois secant in log-frequency. Two or three probes instead of
//   six, at the cost of believing the size of a reading.
// 2 GATED: INTERP where both bracket readings are clearly above the
//   measurement's own noise, BISECT where they are not.
#ifndef AUTOTUNE_SEARCH_MODE_DEFAULT
#define AUTOTUNE_SEARCH_MODE_DEFAULT 1
#endif
enum AutotuneSearchMode : uint8_t {
  SEARCH_BISECT = 0,
  SEARCH_INTERP = 1,
  SEARCH_GATED  = 2,
};
uint8_t autotuneSearchMode = (uint8_t)AUTOTUNE_SEARCH_MODE_DEFAULT;

static inline const char *autotune_search_mode_name(uint8_t m) {
  switch (m) {
    case SEARCH_BISECT: return "BISECT";
    case SEARCH_GATED:  return "GATED";
    default:            return "INTERP";
  }
}

// Measure at FINE quality for the rest of the enclosing scope, whatever the run
// asked for, and put the run's own precision back on the way out (including when
// the calibration is cancelled mid-probe).
//
// For the top of the range: that pair is where every note above the last rung is
// played from, and it sits right below the frequency at which the pulse
// collapses, so it is worth more readings than a coarse run would give it - and
// up there a reading is a couple of milliseconds, so they are nearly free.
struct CalPrecisionOverride {
  uint8_t saved;
  explicit CalPrecisionOverride(uint8_t p = CAL_PRECISION_FINE)
      : saved(calibrationPrecision) {
    calibrationPrecision = p;
  }
  ~CalPrecisionOverride() { calibrationPrecision = saved; }
};

#ifndef NUM_PW_CHANNELS
#define NUM_PW_CHANNELS NUM_OSCILLATORS
#endif

// PW channel that belongs to oscillator `osc`.
// DCO3: PW arrays are NUM_OSCILLATORS and only [0] is wired, so osc 1/2
// hit PW_PIN_UNASSIGNED and are skipped. DCO4: two oscillators share one
// PW channel (NUM_PW_CHANNELS == NUM_VOICES_TOTAL), so this is osc / 2.
static inline uint8_t cal_pw_channel(uint8_t osc) {
  if (NUM_PW_CHANNELS == NUM_OSCILLATORS) return osc;
  return (uint8_t)(osc / (NUM_OSCILLATORS / NUM_PW_CHANNELS));
}

// Manual DCO calibration workflow state and per-oscillator manual offsets
// that are added on top of automatic amp compensation.
uint8_t manualCalibrationStage;
int8_t manualCalibrationOffset[NUM_OSCILLATORS] = { 0, 0, 0 };

// Oscillator under trim. Packed DCO4 walk is not stage/3.
static inline uint8_t cal_stage_to_osc(uint8_t stage) {
  return cal_stage_to_osc_n(stage, NUM_OSCILLATORS);
}
static inline CalStageKind cal_stage_kind(uint8_t stage) {
  return cal_stage_kind_n(stage, NUM_OSCILLATORS);
}
static inline bool cal_stage_is_440(uint8_t stage) {
  return cal_stage_is_440_n(stage, NUM_OSCILLATORS);
}
static inline bool cal_stage_is_saw(uint8_t stage) {
  return cal_stage_is_saw_n(stage, NUM_OSCILLATORS);
}
static inline bool cal_stage_is_tri(uint8_t stage) {
  return cal_stage_is_tri_n(stage, NUM_OSCILLATORS);
}
static inline bool cal_stage_is_pw_edit(uint8_t stage) {
  return cal_stage_is_pw_edit_n(stage, NUM_OSCILLATORS);
}
static inline bool cal_stage_is_square(uint8_t stage) {
  return cal_stage_is_square_n(stage, NUM_OSCILLATORS);
}
static inline uint8_t cal_manual_osc() {
  uint8_t osc = cal_stage_to_osc(manualCalibrationStage);
  if (osc >= NUM_OSCILLATORS) osc = NUM_OSCILLATORS - 1;
  return osc;
}

static inline uint8_t cal_stage_max() {
  return cal_stage_max_n(NUM_OSCILLATORS);
}

// Manual calibration step (PARAM_MANUAL_CALIBRATION_STEP): 0 = trimpot stage
// at the low starting note, 1 = 440 Hz amp-set stage (adjust ampComp440 until
// duty = 50%). Reset to 0 on every manual-cal entry.
uint8_t manualCalibrationStep = 0;

// Per-oscillator amp-comp (range PWM) value at 440 Hz, set during manual
// calibration step 1 and persisted in LittleFS ("AmpComp440"). 0 = never set;
// FREQ_TRACE refuses to run without it (it is the curve anchor).
uint16_t ampComp440[NUM_OSCILLATORS] = { 0, 0, 0 };

// Per-oscillator duty target trim (PARAM_AMP_COMP_DUTY_OFFSET), in hundredths
// of a percent of duty, persisted in LittleFS ("AmpCompDutyOffset").
// The calibration sense pin is a digital input with its own thresholds, so the
// duty it calls 50% can be a fixed offset away from the 50% a scope sees on
// the analog pulse output. Both amp-comp methods aim at 50% + this trim, so
// dialling it once per oscillator (scope on the pulse output) makes every
// stored point land at a true 50%. 0 = untrimmed, the historical behaviour.
int16_t ampCompDutyOffset[NUM_OSCILLATORS] = { 0, 0, 0 };

// Gap (in microseconds) that corresponds to the trimmed duty target at freqHz.
// From duty - 0.5 = gap / (2T): gapTarget = 2T * offsetFraction. Searches
// compare their measured gap against this instead of against zero.
static inline float duty_trim_gap_us(uint8_t osc, float freqHz) {
  if (osc >= NUM_OSCILLATORS || freqHz <= 0.0f || ampCompDutyOffset[osc] == 0) {
    return 0.0f;
  }
  const float offsetFraction = (float)ampCompDutyOffset[osc] / 10000.0f;
  return 2.0f * (1.0e6f / freqHz) * offsetFraction;
}

/************************************************/
/****************** DCO calibration ******************/

// Temporary buffer used during calibration to build [frequency, range-PWM]
// pairs for a single DCO. Persisted via update_FS_voice() when an osc is done.
uint32_t calibrationData[chanLevelVoiceDataSize];

// --- Per-run calibration report -------------------------------------------
// Where each table pair came from and which duty error was achieved when it
// was measured. Filled by whichever amp-comp method built the table and
// printed by print_calibration_report() once the oscillator is done, so a bad
// point is visible without re-measuring anything.
enum CalPointSource : uint8_t {
  CAL_SRC_NONE = 0,       // never written (should not survive a complete run)
  CAL_SRC_RUNG,           // ladder rung (FREQ_TRACE) / per-note search (classic)
  CAL_SRC_ANCHOR,         // the 440 Hz manual operating point
  CAL_SRC_ENDPOINT_FULL,  // top endpoint at full amp comp
  CAL_SRC_ENDPOINT_AMP0,  // bottom endpoint at amp comp 0
  CAL_SRC_MANUAL,         // trimpot header pair, written without measuring
  CAL_SRC_FILLED,         // interpolated or estimated, never measured
  CAL_SRC_SENTINEL,       // 20000000 padding above the top endpoint
  CAL_SRC_REFINED,        // stored pair re-measured by the fine pass
};

// Number of [freq, amp comp] pairs in a calibration table.
static constexpr int kCalReportPairs = (int)(chanLevelVoiceDataSize / 2);

// Marks a pair with no measurement behind it in calPointDutyErrPct[].
static constexpr float kCalDutyErrUnknown = 1e9f;

// Signed duty error per pair, in percentage points. Sign follows the search
// convention (measure_gap_for_amp / measure_duty_at_freq): + = amplitude too
// low, i.e. the pulse spends less than half the period high.
float   calPointDutyErrPct[kCalReportPairs];
uint8_t calPointSource[kCalReportPairs];

// FREQ_TRACE ladder shape of the last run, for the report header
// (0 / -1 = not applicable, e.g. after a classic run).
int calReportLadderInterval = 0;
int calReportAnchorPair     = -1;
// True when the last FREQ_TRACE build used the geometric amp-grid builder
// (not the obsolete freq-spaced ladder). Drives the CAL_REPORT "amp-grid" label.
bool calReportAmpGrid       = false;

// What the current oscillator's amp-comp stage has cost so far: duty
// measurements taken and wall-clock time since cal_report_reset(). These are the
// two numbers the search-mode A/B (autotuneSearchMode, cmds 37-39) is judged on,
// so they are printed on the report footer. The PW stage is not counted; it does
// not go through the frequency search.
uint32_t      calRunProbes  = 0;
unsigned long calRunStartMs = 0;

// Duty error in percentage points from a measured gap in microseconds, using
// |duty - 0.5| = |gap| / (2 * period). Keeps the caller's sign.
static inline float duty_err_pct_from_gap(float gapUs, float freqHz) {
  if (gapUs == kGapTimeoutSentinel || freqHz <= 0.0f) {
    return kCalDutyErrUnknown;
  }
  return 100.0f * gapUs * freqHz / 2.0e6f;
}

// Calibration logs: 3 decimal Hz. The stored table stays freq × 100 integers.
static inline String fmt_freq(float hz) {
  return String(hz, 3);
}

// Verification-sweep request (PARAM_DEBUG_COMMAND 36). Set on core 0; loop1
// runs the sweep because every probe blocks on a duty measurement.
volatile bool calibrationVerifyRequested = false;

// PW CV probe request (PARAM_DEBUG_COMMAND 46). Set on core 0 while manual
// calibration is running; loop1 runs it between two manual passes because each
// step blocks on a duty measurement.
volatile bool pwCvProbeRequested = false;

// Manual calibration solos one oscillator by stopping every other state machine,
// which a synced pair cannot survive: under hard sync the master's sideset owns
// the slave's RESET pin, under soft sync the slave polls the master's pin, so a
// stopped partner leaves the soloed oscillator unable to reset itself and it goes
// silent. Manual cal walks with a neutral topology and puts the operator's choice
// back on exit; these hold it meanwhile, and also absorb a sync change arriving
// mid-walk (a preset load) so it cannot re-arm sync under the solo.
uint8_t manualCalSavedSyncMode = 0;
uint8_t manualCalSavedSoftSyncChunks = 0;
// Rebuilding the topology touches PIO, so core 0 only asks: loop1's manual-cal
// branch runs it, since that branch never reaches pio_defer_service().
volatile bool calSyncNeutralRequested = false;

// --- Implemented in autotune_impl.h ---

// The calibration run itself, driven from core 1 while calibrationFlag is set.
void DCO_calibration();

void run_calibration_verify_sweep();
void run_pw_cv_probe();
void cal_report_reset();
void cal_report_set_pair(int pair, float dutyErrPct, uint8_t src);
void cal_report_set_pair_from_gap(int pair, float gapUs, float freqHz, uint8_t src);
void print_calibration_report(uint8_t dcoIndex, const uint32_t *data);

// Prepare the next oscillator's calibration run: seed its table header, reset
// the per-DCO state and drive the start note.
void restart_DCO_calibration();

// Undo what a calibration run did to the oscillators: PW centers back, every SM
// started same-cycle, voices retriggered. Manual cal reaches it from core 0
// through pio_defer_request_cal_restore(), so it needs a declaration this early.
static void restore_voice_engine_after_calibration();

// PW calibration stages, called from the param handlers and the manual
// calibration workflow. mode selects which note/voice the center search runs on.
void find_PW_center(uint8_t mode);

// One-shot diagnostics for the calibration sense path (PARAM_DEBUG_COMMAND).
void DCO_calibration_debug();

// Index of the DCO currently being calibrated.
uint8_t currentDCO;

// millis() timestamp when the current calibration pass started. Used by the
// PW search phases for their 60 s safety timeouts.
unsigned long DCOCalibrationStart;

// Current range-PWM value used during calibration for the active DCO.
volatile uint16_t ampCompCalibrationVal;

// Frequency override (Hz) consumed by voice_task_autotune() mode 4 during the
// highest-frequency search (replaces the old PID_v1 PIDOutput coupling).
float calibrationFreqHz = 0.0f;

// When > 0, find_gap() gates edge intervals against this frequency instead of
// note_to_freq(DCO_calibration_current_note). Set by measure_duty_at_freq()
// while probing arbitrary frequencies; 0 = fall back to the current note.
float gapGateFreqHz = 0.0f;

// Frequency (Hz) the oscillator is currently running at, so the next probe
// knows how far it has to move: measure_duty_at_freq() sizes its stability
// checks from that distance. 0 = nothing is running (a cold start), which
// counts as the largest possible move.
float g_lastDrivenFreqHz = 0.0f;

// Baseline manual amp-comp starting value (PWM counts). 35 was measured at
// wrap 14000; scale so analog duty stays the same if RANGE_PWM_WRAP changes.
static constexpr uint16_t initManualAmpCompCalibrationValPreset =
    (uint16_t)(35u * DIV_COUNTER / 14000u);
// Per-oscillator baseline manual amp-comp starting values. Filled from the
// preset on first use so the array size can follow NUM_OSCILLATORS (3 on
// DCO3, 8 on DCO4) without a brace list that only covers the first three.
uint16_t initManualAmpCompCalibrationVal[NUM_OSCILLATORS];

static inline void autotune_fill_init_manual_amp() {
  static bool filled = false;
  if (filled) return;
  for (int i = 0; i < NUM_OSCILLATORS; ++i) {
    initManualAmpCompCalibrationVal[i] = initManualAmpCompCalibrationValPreset;
  }
  filled = true;
}
// Range-PWM value stored as the "lowest frequency" anchor in the calibration
// table header (also persisted by FS.ino when seeding fake tables). 10 was
// measured at wrap 14000.
volatile uint16_t ampCompLowestFreqVal = (uint16_t)(10u * DIV_COUNTER / 14000u);

// Note from which DCO calibration starts, in the offset convention described at
// manual_cal_reference_note below (note 29 -> sNotePitches[17] = F0, 21.83 Hz).
static constexpr uint8_t DCO_calibration_start_note = 29;
// Interval in semitones between successive calibration notes.
static constexpr uint8_t calibration_note_interval = 5;
// Starting note used for the PW-centered calibration passes.
static constexpr uint8_t manual_DCO_calibration_start_note = DCO_calibration_start_note - 5;
// Reference note for the manual trim stage: A4, exactly 440 Hz. Duty feedback
// refreshes ~27x faster than at the low trim note, and this operating point
// becomes the anchor of the FREQ_TRACE curve.
// PW calibration deliberately stays at manual_DCO_calibration_start_note.
//
// 81, not the 69 an A4 usually is: note_to_freq() below reads
// sNotePitches[midiNote - 12], and that table starts at C-1 (8.18 Hz, standard
// MIDI 0), so every note number here names a pitch an octave below the MIDI
// note of the same number. 81 - 12 = 69 = NOTE_A4. The whole autotune path
// shares this convention (voice_task_autotune() looks up VOICE_NOTES[0] - 12
// the same way), so the numbers are consistent; only this one was chosen as if
// they were not, which had manual step 2 trimming at 220 Hz while the panel
// said 440.
static constexpr uint8_t manual_cal_reference_note = 81;

// Current note used during calibration.
uint8_t DCO_calibration_current_note;

// Global debug verbosity level for autotune routines.
byte autotuneDebug = 4;

// Convert a calibration note number to its frequency in Hz. sNotePitches[]
// starts at C-1 (standard MIDI 0), so the -12 makes every note number here name
// a pitch an octave below the MIDI note of the same number - see the convention
// note at manual_cal_reference_note above.
static inline float note_to_freq(uint8_t midiNote) {
  return sNotePitches[midiNote - 12];
}

// Period-proportional settle delay before a duty measurement: two waveform
// periods, floored at 4 ms (replaces the old fixed delay(10) which was too
// short at low frequencies and needlessly long at high ones).
static inline void settle_for_freq(double freqHz) {
  uint32_t settleMs = 4;
  if (freqHz > 0.0) {
    double twoPeriodsMs = 2000.0 / freqHz;
    if (twoPeriodsMs > (double)settleMs) {
      settleMs = (uint32_t)(twoPeriodsMs + 0.999);
    }
  }
  delay(settleMs);
}

// --- Implemented in autotune_search_impl.h ---

// Allowed |gap| in microseconds for a frequency and duty-error fraction.
double compute_gap_tolerance_for_freq(double freqHz, double dutyErrorFraction);

// Main DCO amp-comp calibration routine (search-based).
// dutyErrorFraction specifies the allowed duty-cycle error (e.g. 0.005 = 0.5%).
void calibrate_DCO(DCOCalibrationContext& ctx, double dutyErrorFraction);

// Curve-fit helpers shared by the calibration searches and the table builders.
float    quadraticInterpolation(float x0, float y0, float x1, float y1,
                                float x2, float y2, float x);
uint16_t logarithmicInterpolation(float x0, float y0, float x1, float y1, float x);
float    linearInterpolation(float x0, float y0, float x1, float y1, float x);
double   expInterpolationSolveY(double x, double x0, double x1,
                                double y0, double y1);

// Highest usable frequency at full range PWM (returns Hz * 100). Amp is fixed
// at DIV_COUNTER; this measures frequency there (no searched "highest amp").
// pairsFilled is how much of ctx's table already holds measured points to seed from.
float find_highest_freq(DCOCalibrationContext& ctx, int pairsFilled);

// Estimated lowest reachable frequency at amp comp = 0 (returns Hz * 100).
float find_lowest_freq();

// Hard frequency limits for one search. Outside them there is either nothing to
// find or nothing the caller may store, so a search that reaches an edge without
// a signal stops there instead of spending 100 ms timeouts past it. This type
// lives in the header for the same reason the PW ones below do: the Arduino
// builder's generated prototypes have to compile.
struct FreqSearchBounds {
  float loHz;
  float hiHz;
};

// Measured lowest usable frequency at amp comp = 0: scan bounds for a pair of
// readings that bracket 50% duty, then search between them with the amp fixed at
// 0. freqSeedHz is the fallback when nothing in the band pulses.
// Returns Hz, or 0 if no signal.
float measure_lowest_freq_at_amp0(float freqSeedHz, const FreqSearchBounds *bounds);

// Overwrite the table's amp-comp-0 anchor (entry [0..1]) with a measured point
// (classic method only). Keeps the previous estimate when there is no pulse at
// amp 0 anywhere in the band below the first real pair, or when the point found
// there is not close enough to 50% duty to be the one we were looking for.
void apply_measured_lowest_freq(DCOCalibrationContext& ctx);

// Probe the duty error at an arbitrary frequency with a fixed range PWM.
// Positive result = amplitude too low (frequency too high for this PWM);
// kGapTimeoutSentinel on timeout. hiRes averages twice as many segments.
float measure_duty_at_freq(float freqHz, uint16_t amp, bool hiRes = false);

// Search the frequency at a fixed range PWM at which duty = 50%. Measures
// freqGuess first and then steps outward until it brackets the answer, expecting
// it within freqGuess * [1/windowRatio, windowRatio]. refine adds hi-res probes,
// a larger probe budget and an averaged confirmation of the converged point.
// bounds, when given, is where the search may look at all - which also lets it
// spend its whole probe budget inside a band it has been told holds the answer,
// instead of stopping at the timeout allowance an unbounded search gets.
// Returns the best frequency in Hz, or 0 if no usable signal was seen.
float find_freq_for_duty50(uint16_t amp, float freqGuess, float windowRatio,
                           bool refine = false,
                           const FreqSearchBounds *bounds = nullptr);

// FREQ_TRACE amp-comp calibration: geometric amp-comp grid from the manual
// baseline through DIV_COUNTER (last rung), measuring frequency at duty 50% on
// each fixed amp. Returns false if the resulting table failed the monotonicity
// check (caller must then skip persisting it).
bool calibrate_DCO_freq_trace(DCOCalibrationContext& ctx);

// Fine pass (calibrationPrecision == CAL_PRECISION_FINE): keep every amp comp
// value of the stored table and re-measure the frequency each one really sits
// at. Returns false when there is no usable stored table or the result is not
// monotonic (caller must then skip persisting it).
bool refine_DCO_amp_table(DCOCalibrationContext& ctx);

// --- PW target-duty search (autotune_impl.h) ---
// These types live in this header (rather than beside the definitions) because
// the search-phase helpers are declared here.

// Maximum number of valid samples remembered by a PW search.
static constexpr int kPWMaxSamples = 40;

// State shared by the PW target-duty search phases (coarse scan, bisection,
// fine scan, candidate selection). All gap differences are relative to the
// target gap (gap - gapTarget), so "0" always means "exactly on target duty".
struct PWSearchState {
  uint16_t validPW[kPWMaxSamples];       // PW of each stored valid sample
  double   validGapDiff[kPWMaxSamples];  // gap - gapTarget for each sample
  int      validCount;
  int      inToleranceCount;  // valid samples measured within targetGap
  bool     haveBest;          // at least one valid sample was seen
  double   bestGapAbs;        // smallest |gap - gapTarget| seen so far
  uint16_t bestPW;            // PW that produced bestGapAbs
  bool     haveBracket;       // sign-change bracket found during coarse scan
  uint16_t pwLow, pwHigh;     // bracket bounds
  double   gapLow;            // raw gap measured at pwLow
};

// How a sample should enter the valid-samples table.
enum PWRecordMode {
  PW_RECORD_NO_TABLE,       // update best/in-tolerance counters only
  PW_RECORD_APPEND,         // append while there is room
  PW_RECORD_REPLACE_WORST,  // append, or replace the worst entry when full
};

// --- PW limit search (autotune_impl.h) ---

// Direction selector for the unified PW limit search.
enum PWLimitDir {
  PW_LIMIT_LOW,
  PW_LIMIT_HIGH
};

// Result structure used by the PW-limit search helpers.
struct PWLimitSearchResult {
  bool     ok;                  // true if at least one valid sample was found
  uint16_t limitPW;             // PW value chosen as limit
  double   finalDutyPercent;    // measured duty at limitPW in percent, or < 0 if unknown
};

// Low-level search routine that assumes the DCO is already configured for
// PW calibration on the desired note/voice. It scans from centerPW toward
// the requested direction and returns the PW that best matches targetDuty.
PWLimitSearchResult search_PW_limit_from_center(
  uint8_t     voiceIdx,
  uint16_t    centerPW,
  PWLimitDir  dir,
  double      periodUs,
  double      targetDuty
);

// High-level wrapper that configures the calibration context and commits the
// found limit (LOW or HIGH) to the filesystem and runtime tables.
void find_PW_limit_v2(PWLimitDir dir);


#endif
