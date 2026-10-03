#ifndef __BENCH_H__
#define __BENCH_H__

// Realtime profiler for the DCO hot path.
//
// Enabled by RUNNING_AVERAGE in DCO.ino; every macro below compiles to nothing when it is
// off, so the shipping build carries no cost. RUNNING_AVERAGE_FINE additionally enables the
// probes around the smallest stages (a few multiplies each) — useful for A/B-ing how much
// the instrumentation itself distorts the totals, since every probe is an optimisation
// barrier the compiler cannot move work across. RUNNING_AVERAGE_PERIOD keeps only
// BENCH_PERIOD (loop / loop1); all BENCH_BEGIN/END stage probes and path counters compile
// out — use that for a true loop baseline without intermediate probe tax. PERIOD overrides FINE.
// BENCH_PATH_STATS (DCO.ino): all path bumps (amp/ratio/porta + walk-step sums) and the
// dump `-- Path counters --` block. Off = no-ops even with RUNNING_AVERAGE. PERIOD still wins.
// BENCH_STAGE_STRIDE (default 9): MAIN/FINE stage probes every Nth loop; BENCH_PERIOD always.
// Note-on family (BENCH_T_RARE) always records. 1 = every-iter MAIN (old tax).
// BENCH_USE_SYSTICK (DCO.ino, default 1): SysTick for PERIOD + stages; 0 = 1 us timer for all.
// BENCH_PERIOD_MAX_US (DCO.ino, default 2000): discard PERIOD samples longer than this
// (autotune / wrap-looking stalls). Dump window (1 s gate) always uses bench_us_now().
//
// Time source: SysTick, read as a free-running 24-bit down-counter clocked from clk_sys.
// RP2040's Cortex-M0+ has no DWT cycle counter, so SysTick is the only single-cycle source
// available on both chips. Resolution is one clk_sys cycle (4.4 ns at 225 MHz).
//
// Range: 24 bits wraps after 2^24 cycles — ~74.5 ms at 225 MHz, ~126 ms at 133 MHz.
// BENCH_PERIOD() uses the same SysTick clock as stages when BENCH_USE_SYSTICK; samples
// longer than BENCH_PERIOD_MAX_US are discarded. The 1 us timer remains for the dump
// window gate only. The report flags any probe whose max lands near the wrap.

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "pico/platform.h"
#include "hardware/clocks.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/timer.h"

// Defined in amp_comp_bench.ino; prints AMP_COMP_BENCHMARK speed/accuracy when pending.
void print_amp_comp_bench();
// Defined in pitch_interp_bench.ino; pitch interp speed/accuracy when RUNNING_AVERAGE.
extern volatile bool pitch_interp_bench_speed_pending;
extern volatile bool pitch_interp_bench_accuracy_pending;
void print_pitch_interp_bench();
// Defined in clkdiv_bench.ino; six methods × both voices; pctVsGOLD_REF.
extern volatile bool clkdiv_hp_bench_speed_pending;
extern volatile bool clkdiv_hp_bench_accuracy_pending;
void print_clkdiv_hp_bench();

extern volatile bool pio_probe_report_pending;
void pio_probe_report_flush();

static inline const char *bench_pitch_interp_mode_name() {
#if PITCH_INTERP_MODE == PITCH_INTERP_FLOAT
  return "FLOAT";
#elif PITCH_INTERP_MODE == PITCH_INTERP_FLOAT_FAST
  return "FLOAT_FAST";
#elif PITCH_INTERP_MODE == PITCH_INTERP_RATIO_Q16
  return "RATIO_Q16";
#elif PITCH_INTERP_MODE == PITCH_INTERP_Q12
  return "Q12";
#else
  return "?";
#endif
}

// Set to 0 to fall back to the 1 us timer for every probe, e.g. if SysTick turns out to be
// claimed by the core (a FreeRTOS build uses it; plain arduino-pico drives millis()/micros()
// from time_us_64() and leaves it alone). Sketch (DCO.ino) wins when set before include.
#ifndef BENCH_USE_SYSTICK
#define BENCH_USE_SYSTICK 1
#endif
#ifndef BENCH_PERIOD_MAX_US
#define BENCH_PERIOD_MAX_US 2000
#endif
#if BENCH_PERIOD_MAX_US < 1
#undef BENCH_PERIOD_MAX_US
#define BENCH_PERIOD_MAX_US 1
#endif

// ---------------------------------------------------------------------------
// Probe table — the single source of truth.
//
// Storage, enum ids, core ownership, parent/child nesting and the printed labels are all
// generated from this list, so a probe cannot exist in one place and be missing in another.
// Adding a probe means adding one line here plus the BENCH_BEGIN/BENCH_END pair at the site.
//
//   X(id, core, kind, tier, parent, label)
// ---------------------------------------------------------------------------
#if PROJECT_INSTRUMENT == 3
// Sub-osc segment words (ENABLE_SUBOSC_ENGINE2); multiplies only, DMA does the pushing.
#define BENCH_X_SUBOSC(X) \
  X(vt_subosc,         1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "subosc segments")
#else
#define BENCH_X_SUBOSC(X)
#endif
#define BENCH_PROBES(X)                                                                       \
  /* --- Core 0: loop() --- */                                                                \
  X(loop0_period,      0, BENCH_PERIOD_KIND, BENCH_T_MAIN, BENCH_NONE,  "loop period")         \
  X(loop0_microsTimer, 0, BENCH_CYC, BENCH_T_MAIN, BENCH_loop0_period,  "microsTimer")         \
  X(loop0_midi,        0, BENCH_CYC, BENCH_T_MAIN, BENCH_loop0_period,  "MIDI read")           \
  X(loop0_serial,      0, BENCH_CYC, BENCH_T_MAIN, BENCH_loop0_period,  "serial panel/USB")    \
  X(loop0_lfo1,        0, BENCH_CYC, BENCH_T_MAIN, BENCH_loop0_period,  "LFO1")                \
  X(loop0_lfo2,        0, BENCH_CYC, BENCH_T_MAIN, BENCH_loop0_period,  "LFO2")                \
  X(loop0_drift,       0, BENCH_CYC, BENCH_T_MAIN, BENCH_loop0_period,  "drift LFOs")          \
  /* --- Core 1: loop1() --- */                                                               \
  X(loop1_period,      1, BENCH_PERIOD_KIND, BENCH_T_MAIN, BENCH_NONE,  "loop1 period")        \
  X(loop1_microsTimer, 1, BENCH_CYC, BENCH_T_MAIN, BENCH_loop1_period,  "microsTimer2")        \
  X(loop1_noise,       1, BENCH_CYC, BENCH_T_MAIN, BENCH_loop1_period,  "noise_gens")          \
  X(loop1_noise_refill,1, BENCH_CYC, BENCH_T_MAIN, BENCH_loop1_noise,   "noise refill")        \
  X(loop1_adsr,        1, BENCH_CYC, BENCH_T_MAIN, BENCH_loop1_period,  "ADSR_update")         \
  X(loop1_cv_outs,     1, BENCH_CYC, BENCH_T_MAIN, BENCH_loop1_period,  "update_CV_outs")      \
  X(voice_task,        1, BENCH_CYC, BENCH_T_MAIN, BENCH_loop1_period,  "voice_task TOTAL")    \
  /* --- Core 1: inside voice_task --- */                                                     \
  X(vt_pitchbend,      1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "pitch bend")          \
  X(vt_osc_detune,     1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "OSC2/3 detune")       \
  X(vt_portamento,     1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "portamento")          \
  X(vt_adsr_mod,       1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "ADSR modifier")       \
  X(vt_unison_mod,     1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "unison modifier")     \
  X(vt_drift_mod,      1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "drift modifier")      \
  X(vt_modifiers,      1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "modifier sum")        \
  X(vt_freq_scale_x,   1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "table x scaling")     \
  X(vt_ratio_interp,   1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "ratio interpolate")   \
  X(vt_freq_scale_post,1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "apply ratio")         \
  X(vt_clk_div,        1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "clkdiv math")         \
  X(vt_phase_align,    1, BENCH_CYC, BENCH_T_RARE, BENCH_voice_task,    "phase align")         \
  /* Exact split on note-on frames next to hot phase_align (not inside note-on block). */ \
  X(vt_retrig_split,   1, BENCH_CYC, BENCH_T_RARE, BENCH_voice_task,    "retrig period split") \
  X(vt_chan_level,     1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "amp comp")            \
  X(vt_pio_write,      1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "PIO put/exec")        \
  BENCH_X_SUBOSC(X)                                                                           \
  X(vt_note_retrig,    1, BENCH_CYC, BENCH_T_RARE, BENCH_voice_task,    "note-on retrigger")   \
  /* One SM-apply probe: fine MAIN slices mis-attribute cold XIP across load/jmp/enable. */ \
  X(vt_retrig_sm_apply,1, BENCH_CYC, BENCH_T_RARE, BENCH_vt_note_retrig,"retrig SM apply")     \
  X(vt_retrig_pwm,     1, BENCH_CYC, BENCH_T_RARE, BENCH_vt_note_retrig,"retrig RANGE PWM")    \
  X(vt_range_pwm,      1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "RANGE PWM")           \
  X(vt_pwm_calc,       1, BENCH_CYC, BENCH_T_FINE, BENCH_voice_task,    "PW arithmetic")       \
  X(vt_pw_update,      1, BENCH_CYC, BENCH_T_MAIN, BENCH_voice_task,    "PW level + write")

enum BenchId {
#define BENCH_X(id, core, kind, tier, parent, label) BENCH_##id,
  BENCH_PROBES(BENCH_X)
#undef BENCH_X
  BENCH_COUNT
};
#define BENCH_NONE BENCH_COUNT

// Probe unit. BENCH_CYC values are clk_sys cycles, BENCH_US values are microseconds.
#define BENCH_CYC 0
#define BENCH_US  1
#if BENCH_USE_SYSTICK
#define BENCH_PERIOD_KIND BENCH_CYC
#else
#define BENCH_PERIOD_KIND BENCH_US
#endif

// Probe tier. FINE probes only collect when RUNNING_AVERAGE_FINE is defined.
// RARE (note-on family) always records even when MAIN stages are sampled 1/N.
#define BENCH_T_MAIN 0
#define BENCH_T_FINE 1
#define BENCH_T_RARE 2

#ifndef BENCH_STAGE_STRIDE
#define BENCH_STAGE_STRIDE 9
#endif
#if BENCH_STAGE_STRIDE < 1
#undef BENCH_STAGE_STRIDE
#define BENCH_STAGE_STRIDE 1
#endif

// ---------------------------------------------------------------------------
// Time source. Kept outside the RUNNING_AVERAGE guard so one-shot benches can use it
// on their own; with no probes compiled in, none of this is referenced and none of it
// costs anything at runtime.
// ---------------------------------------------------------------------------

// Cost of one back-to-back bench_now() pair, subtracted from every cycle sample.
uint32_t bench_overhead_cyc = 0;
uint32_t bench_cycles_per_us = 1;
// PERIOD discard cap in the same unit as BENCH_PERIOD samples (cyc or us).
uint32_t bench_period_max_raw = 0xFFFFFFFFu;

// Wall-clock span each core's current set of samples was collected over, used for the
// "share of the core's time" column.
uint32_t bench_window_start_us[2] = { 0, 0 };
uint32_t bench_window_us[2] = { 0, 0 };

#if BENCH_USE_SYSTICK
#define BENCH_TIMER_MASK 0x00FFFFFFu
// SysTick counts down, so the earlier reading is the larger one.
static inline uint32_t bench_now(void) {
  return systick_hw->cvr;
}
static inline uint32_t bench_span(uint32_t start, uint32_t end) {
  return (start - end) & BENCH_TIMER_MASK;
}
#else
#define BENCH_TIMER_MASK 0xFFFFFFFFu
static inline uint32_t bench_now(void) {
  return timer_hw->timerawl;
}
static inline uint32_t bench_span(uint32_t start, uint32_t end) {
  return end - start;
}
#endif

// Free-running 1 us reading for the dump window / 1 s gate (not loop-period probes).
static inline uint32_t bench_us_now(void) {
  return timer_hw->timerawl;
}

// Per core, at boot. SysTick is a core-local peripheral: core 0 and core 1 each have their
// own, and arming one does nothing for the other.
inline void bench_init_core() {
  bench_cycles_per_us = clock_get_hz(clk_sys) / 1000000u;
  if (bench_cycles_per_us == 0u) bench_cycles_per_us = 1u;
#if BENCH_USE_SYSTICK
  bench_period_max_raw = bench_cycles_per_us * (uint32_t)BENCH_PERIOD_MAX_US;
#else
  bench_period_max_raw = (uint32_t)BENCH_PERIOD_MAX_US;
#endif
  if (bench_period_max_raw == 0u) bench_period_max_raw = 1u;

#if BENCH_USE_SYSTICK
  systick_hw->csr = 0u;
  systick_hw->rvr = BENCH_TIMER_MASK;
  systick_hw->cvr = 0u;
  systick_hw->csr = 0x5u;  // ENABLE | CLKSOURCE=processor, interrupt off
#endif

  // Minimum of a run of back-to-back reads: the cleanest estimate of the probe's own cost,
  // since any larger sample picked up an interrupt. Both cores compute the same value.
  uint32_t best = 0xFFFFFFFFu;
  for (int i = 0; i < 32; ++i) {
    const uint32_t a = bench_now();
    const uint32_t b = bench_now();
    const uint32_t d = bench_span(a, b);
    if (d < best) best = d;
  }
  bench_overhead_cyc = best;

#ifdef RUNNING_AVERAGE
  bench_window_start_us[get_core_num() & 1u] = bench_us_now();
#endif
}

// ---- Paced USB CDC output (core 0, non-blocking) ---------------------------
// Shared by the profiler and PIO debug reports. Never blocks on Serial.write.

volatile bool bench_out_active = false;

#define BENCH_OUT_CAP   6144
#define BENCH_OUT_CHUNK 64

char bench_out_buf[BENCH_OUT_CAP];
uint16_t bench_out_len = 0;
uint16_t bench_out_pos = 0;

inline void bench_out_reset() {
  bench_out_len = 0;
  bench_out_pos = 0;
}

inline void bench_out_puts(const char *s) {
  while (*s != '\0' && bench_out_len + 1u < BENCH_OUT_CAP) {
    bench_out_buf[bench_out_len++] = *s++;
  }
}

inline void bench_out_printf(const char *fmt, ...) {
  char tmp[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  bench_out_puts(tmp);
}

inline void bench_out_println(const char *s) {
  bench_out_puts(s);
  bench_out_puts("\n");
}

// True when the host is still sending USB CDC frames (slider drag, etc.).
inline bool bench_cdc_rx_pending() {
#ifdef ENABLE_USB_CONTROL
  return Serial.available() > 0;
#else
  return false;
#endif
}

// Drain up to BENCH_OUT_CHUNK bytes without blocking. Returns true while more remains.
inline bool bench_out_drain_chunk() {
  if (!bench_out_active) return false;
  if (bench_cdc_rx_pending()) return true;

  const uint16_t remain = (uint16_t)(bench_out_len - bench_out_pos);
  if (remain == 0u) {
    bench_out_active = false;
    bench_out_reset();
    return false;
  }

  int avail = Serial.availableForWrite();
  if (avail <= 0) return true;

  uint16_t n = remain;
  if (n > BENCH_OUT_CHUNK) n = (uint16_t)BENCH_OUT_CHUNK;
  if ((uint16_t)avail < n) n = (uint16_t)avail;

  Serial.write(reinterpret_cast<const uint8_t *>(bench_out_buf + bench_out_pos), n);
  bench_out_pos = (uint16_t)(bench_out_pos + n);

  if (bench_out_pos >= bench_out_len) {
    bench_out_active = false;
    bench_out_reset();
    return false;
  }
  return true;
}

#ifdef RUNNING_AVERAGE

struct BenchDesc {
  uint8_t core;
  uint8_t kind;
  uint8_t tier;
  uint8_t parent;
  const char *label;
};

static const BenchDesc bench_desc[BENCH_COUNT] = {
#define BENCH_X(id, core, kind, tier, parent, label) { core, kind, tier, parent, label },
  BENCH_PROBES(BENCH_X)
#undef BENCH_X
};

// 20 bytes per probe. The whole table costs less than a tenth of what one
// RunningAverage(2000) buffer did, and the add is integer-only — which matters on the
// RP2040, where every float operation is an __aeabi_* library call.
struct BenchStat {
  uint32_t n;
  uint32_t min;
  uint32_t max;
  uint64_t sum;
};

BenchStat bench_stats[BENCH_COUNT];
BenchStat bench_snap[BENCH_COUNT];

// Path counters (core 1 voice hot path). Cheap integer bumps — not time probes — so they
// do not add SysTick barriers. Snapshotted with core 1's profiler window to attribute
// ratio/portamento max spikes (algorithmic miss vs IRQ).
struct BenchPathStat {
  uint32_t ratio_hit;
  uint32_t ratio_miss_direct;   // O(1) direct index + tiny fixup
  uint32_t ratio_miss_bsearch;
  uint32_t ratio_clamp;
  uint32_t ratio_walk_steps_sum;
  uint32_t ratio_walk_steps_max;
  uint32_t amp_hit;
  uint32_t amp_miss_walk;
  uint32_t amp_miss_scan;
  uint32_t amp_clamp;
  uint32_t amp_walk_steps_sum;
  uint32_t amp_walk_steps_max;
  uint32_t porta_off;
  uint32_t porta_note_on;
  uint32_t porta_retime;
  uint32_t porta_steady_time;
  uint32_t porta_steady_slew;
};
BenchPathStat bench_path_live;
BenchPathStat bench_path_snap;

#if defined(BENCH_PATH_STATS) && !defined(RUNNING_AVERAGE_PERIOD)
// Same gate as BENCH_PERIOD: do not bump while paced dump TX is active.
#define BENCH_PATH_INC(field) do {                                        \
    if (!bench_out_active) bench_path_live.field++;                       \
  } while (0)
static inline void bench_path_walk_steps(uint32_t steps) {
  if (bench_out_active) return;
  bench_path_live.ratio_walk_steps_sum += steps;
  if (steps > bench_path_live.ratio_walk_steps_max) {
    bench_path_live.ratio_walk_steps_max = steps;
  }
}
static inline void bench_path_amp_walk_steps(uint32_t steps) {
  if (bench_out_active) return;
  bench_path_live.amp_walk_steps_sum += steps;
  if (steps > bench_path_live.amp_walk_steps_max) {
    bench_path_live.amp_walk_steps_max = steps;
  }
}
#else
// No path bumps without BENCH_PATH_STATS (or under PERIOD-only).
#define BENCH_PATH_INC(field) ((void)0)
static inline void bench_path_walk_steps(uint32_t) {}
static inline void bench_path_amp_walk_steps(uint32_t) {}
#endif

// Cross-core report handshake. Each core snapshots and clears its own probes, so no lock is
// needed and neither core ever reads a counter the other is mid-update on.
volatile bool bench_dump_request = false;
volatile bool bench_core_ready[2] = { false, false };
volatile bool bench_periodic = false;
// Bumped on snapshot/reset so BENCH_PERIOD drops its static prev (no straddling old window).
volatile uint8_t bench_period_gen = 0;

// Stage-probe sampling: BENCH_PERIOD every iter; MAIN/FINE every BENCH_STAGE_STRIDE loops.
uint32_t bench_sample_ctr[2] = { 0, 0 };
bool bench_stage_on[2] = { false, false };

static inline void bench_stat_add(BenchStat *s, uint32_t d) {
  s->sum += d;
  if (s->n == 0u || d < s->min) s->min = d;
  if (d > s->max) s->max = d;
  s->n++;
}

// Close a cycle probe. Reads the counter first so none of the bookkeeping below lands
// inside the measured span. Discard while dump TX is active (same gate as BENCH_PERIOD).
static inline void bench_add_cyc(uint8_t id, uint32_t start) {
  const uint32_t d = bench_span(start, bench_now());
  if (bench_out_active) return;
  bench_stat_add(&bench_stats[id], (d > bench_overhead_cyc) ? (d - bench_overhead_cyc) : 0u);
}

// Sampled MAIN/FINE: scale sum/n by STRIDE so mean/%win match a full-rate dump; min/max stay d.
static inline void bench_add_cyc_sampled(uint8_t id, uint32_t start) {
  const uint32_t d0 = bench_span(start, bench_now());
  if (bench_out_active) return;
  const uint32_t d = (d0 > bench_overhead_cyc) ? (d0 - bench_overhead_cyc) : 0u;
  BenchStat *s = &bench_stats[id];
  const uint32_t stride = (uint32_t)BENCH_STAGE_STRIDE;
  s->sum += d * stride;
  if (s->n == 0u || d < s->min) s->min = d;
  if (d > s->max) s->max = d;
  s->n += stride;
}

static inline uint32_t bench_stage_begin(uint8_t id) {
  if (bench_desc[id].tier == BENCH_T_RARE) {
    return bench_now();
  }
  if (!bench_stage_on[get_core_num() & 1u]) {
    return 0u;
  }
  return bench_now();
}

static inline void bench_stage_end(uint8_t id, uint32_t start) {
  if (bench_desc[id].tier == BENCH_T_RARE) {
    bench_add_cyc(id, start);
    return;
  }
  if (!bench_stage_on[get_core_num() & 1u]) {
    return;
  }
  bench_add_cyc_sampled(id, start);
}

static inline void bench_add_raw(uint8_t id, uint32_t d) {
  bench_stat_add(&bench_stats[id], d);
}

// Interval between successive arrivals. While a report is draining over USB, only refresh
// the previous timestamp — never record — so dump TX cannot inflate period max on either core.
// Generation check invalidates prev after snapshot/reset so the first delta cannot include
// time from the previous collection window. SysTick CVR can be 0, so a separate ok flag
// is the valid-prev sentinel (not prev==0). Spans above bench_period_max_raw are dropped.
#if BENCH_USE_SYSTICK
#define BENCH_PERIOD(id)                                                  \
  do {                                                                    \
    static uint32_t bench_prev_##id = 0u;                                 \
    static uint8_t bench_gen_##id = 0u;                                   \
    static uint8_t bench_ok_##id = 0u;                                    \
    const uint32_t bench_t_##id = bench_now();                            \
    const uint8_t bench_g_##id = bench_period_gen;                        \
    if (bench_gen_##id != bench_g_##id) {                                 \
      bench_gen_##id = bench_g_##id;                                      \
      bench_ok_##id = 0u;                                                 \
    }                                                                     \
    if (bench_ok_##id && !bench_out_active) {                             \
      const uint32_t d = bench_span(bench_prev_##id, bench_t_##id);       \
      if (d > 0u && d <= bench_period_max_raw) {                          \
        bench_add_raw(BENCH_##id, d);                                     \
      }                                                                   \
    }                                                                     \
    bench_prev_##id = bench_t_##id;                                       \
    bench_ok_##id = 1u;                                                   \
  } while (0)
#else
#define BENCH_PERIOD(id)                                                  \
  do {                                                                    \
    static uint32_t bench_prev_##id = 0u;                                 \
    static uint8_t bench_gen_##id = 0u;                                   \
    static uint8_t bench_ok_##id = 0u;                                    \
    const uint32_t bench_us_##id = bench_us_now();                        \
    const uint8_t bench_g_##id = bench_period_gen;                        \
    if (bench_gen_##id != bench_g_##id) {                                 \
      bench_gen_##id = bench_g_##id;                                      \
      bench_ok_##id = 0u;                                                 \
    }                                                                     \
    if (bench_ok_##id && !bench_out_active) {                             \
      const uint32_t d = bench_us_##id - bench_prev_##id;                  \
      if (d > 0u && d <= bench_period_max_raw) {                          \
        bench_add_raw(BENCH_##id, d);                                     \
      }                                                                   \
    }                                                                     \
    bench_prev_##id = bench_us_##id;                                      \
    bench_ok_##id = 1u;                                                   \
  } while (0)
#endif

#ifdef RUNNING_AVERAGE_PERIOD
#define BENCH_SAMPLE_TICK() ((void)0)
#define BENCH_BEGIN(id)  ((void)0)
#define BENCH_END(id)    ((void)0)
#define BENCH_FBEGIN(id) ((void)0)
#define BENCH_FEND(id)   ((void)0)
#else
#define BENCH_SAMPLE_TICK()                                               \
  do {                                                                    \
    const uint8_t bench_c_ = get_core_num() & 1u;                         \
    uint32_t bench_n_ = ++bench_sample_ctr[bench_c_];                     \
    if (bench_n_ >= (uint32_t)BENCH_STAGE_STRIDE) {                       \
      bench_sample_ctr[bench_c_] = 0u;                                    \
      bench_stage_on[bench_c_] = true;                                    \
    } else {                                                              \
      bench_stage_on[bench_c_] = false;                                   \
    }                                                                     \
  } while (0)
// volatile start: force a real store/load so back-to-back probes cannot CSE timestamps.
// Do NOT wrap in do/while — voice_task declares locals inside a probe and uses them after END.
#define BENCH_BEGIN(id) volatile uint32_t bench_t_##id = bench_stage_begin(BENCH_##id)
#define BENCH_END(id)   bench_stage_end(BENCH_##id, bench_t_##id)
#ifdef RUNNING_AVERAGE_FINE
#define BENCH_FBEGIN(id) BENCH_BEGIN(id)
#define BENCH_FEND(id)   BENCH_END(id)
#else
#define BENCH_FBEGIN(id) ((void)0)
#define BENCH_FEND(id)   ((void)0)
#endif
#endif

// Minimum collection time before a dump snapshot (after reset or previous dump).
static constexpr uint32_t BENCH_MIN_WINDOW_US = 1000000u; // 1 s

// Snapshot and clear this core's probes, then hand off to the printer on core 0.
inline void bench_service(uint8_t core) {
  if (!bench_dump_request || bench_core_ready[core]) return;

  const uint32_t now_us = bench_us_now();
  const uint32_t elapsed = now_us - bench_window_start_us[core];
  if (elapsed < BENCH_MIN_WINDOW_US) return;  // keep collecting

  bench_window_us[core] = elapsed;
  bench_window_start_us[core] = now_us;
  bench_period_gen++;  // invalidate BENCH_PERIOD static prev on both cores

  for (uint8_t i = 0; i < BENCH_COUNT; ++i) {
    if (bench_desc[i].core != core) continue;
    bench_snap[i] = bench_stats[i];
    bench_stats[i] = BenchStat{};
  }
  // Path counters live on core 1 only (voice_task).
  if (core == 1u) {
    bench_path_snap = bench_path_live;
    bench_path_live = BenchPathStat{};
  }
  bench_core_ready[core] = true;
}

inline void bench_reset_all() {
  for (uint8_t i = 0; i < BENCH_COUNT; ++i) {
    bench_stats[i] = BenchStat{};
    bench_snap[i] = BenchStat{};
  }
  bench_path_live = BenchPathStat{};
  bench_path_snap = BenchPathStat{};
  bench_window_start_us[0] = bench_window_start_us[1] = bench_us_now();
  bench_period_gen++;
}

// Raw probe value -> microseconds, as hundredths, without touching the FPU. Keeping this
// integer matters on RP2040 where float is emulated in software.
inline uint64_t bench_to_us100(uint64_t raw, uint8_t kind) {
  if (kind == BENCH_US) return raw * 100u;
  return ((raw * 100u) + (bench_cycles_per_us / 2u)) / bench_cycles_per_us;
}

inline void bench_fmt_us(char *out, size_t n, uint64_t raw, uint8_t kind) {
  const uint64_t h = bench_to_us100(raw, kind);
  snprintf(out, n, "%lu.%02lu", (unsigned long)(h / 100u), (unsigned long)(h % 100u));
}

inline void bench_fmt_us100(char *out, size_t n, uint64_t us100) {
  snprintf(out, n, "%lu.%02lu", (unsigned long)(us100 / 100u), (unsigned long)(us100 % 100u));
}

inline void bench_fmt_cyc(char *out, size_t n, uint64_t raw) {
  snprintf(out, n, "%luc", (unsigned long)raw);
}

// Fixed columns so nested indents and large totals stay under the header.
// name(28) count(8) mean(9) min(9) max(9) total(14) win(6) — win holds "100.00" or "<0.01"
#define BENCH_ROW_FMT "%-28s %8s %9s %9s %9s %14s %6s"

// %win as percent with two decimals from us100 share of window_us.
// pct_x100 = hundredths of a percent: (us100 * 100) / window_us.
inline void bench_fmt_win_us100(char *out, size_t n, uint64_t us100, uint32_t window_us) {
  if (window_us == 0u) {
    snprintf(out, n, "0.00");
    return;
  }
  const uint64_t pct_x100 = (us100 * 100u) / window_us;
  if (us100 > 0u && pct_x100 == 0u) {
    snprintf(out, n, "<0.01");
    return;
  }
  snprintf(out, n, "%lu.%02lu",
           (unsigned long)(pct_x100 / 100u), (unsigned long)(pct_x100 % 100u));
}

inline void bench_print_row(uint8_t id, const char *indent) {
  const BenchDesc &d = bench_desc[id];
  const BenchStat &s = bench_snap[id];
  if (s.n == 0u) return;

  char name[32], mean[16], mn[16], mx[16], tot[20], cnt[12], win[8], line[160];
  snprintf(name, sizeof(name), "%s%s", indent, d.label);
  snprintf(cnt, sizeof(cnt), "%lu", (unsigned long)s.n);

  // Mean from full sum (rounded), not floor(sum/n), so mean×count ≈ total.
  const uint64_t sum_us100 = bench_to_us100(s.sum, d.kind);
  const uint64_t mean_us100 = (sum_us100 + (s.n / 2u)) / s.n;
  // Sub-microsecond cycle probes print as 0.00 us forever at 225 MHz hundredths; show
  // mean/min/max in cycles instead. total and %win stay in us so the budget still adds.
  const bool as_cyc = (d.kind == BENCH_CYC) && (mean_us100 < 100u);
  if (as_cyc) {
    const uint64_t mean_cyc = (s.sum + (s.n / 2u)) / s.n;
    bench_fmt_cyc(mean, sizeof(mean), mean_cyc);
    bench_fmt_cyc(mn, sizeof(mn), s.min);
    bench_fmt_cyc(mx, sizeof(mx), s.max);
  } else {
    bench_fmt_us100(mean, sizeof(mean), mean_us100);
    bench_fmt_us(mn, sizeof(mn), s.min, d.kind);
    bench_fmt_us(mx, sizeof(mx), s.max, d.kind);
  }
  bench_fmt_us100(tot, sizeof(tot), sum_us100);

  const uint32_t window = bench_window_us[d.core];
  bench_fmt_win_us100(win, sizeof(win), sum_us100, window);

  snprintf(line, sizeof(line), BENCH_ROW_FMT, name, cnt, mean, mn, mx, tot, win);
  bench_out_println(line);

  // ~70% of the 24-bit range (~52 ms at 225 MHz). Past that the span is almost certainly
  // a wrap, not a real stage duration — say so rather than printing a plausible lie.
  if (d.kind == BENCH_CYC && s.max > ((BENCH_TIMER_MASK * 7u) / 10u)) {
    bench_out_println("                             ^^ max near 24-bit wrap (stall or overflow)");
  }
}

// Whatever the parent measured that none of its children claimed. Makes gaps in coverage
// visible instead of quietly absorbing them. Over-attribution (children > parent) is printed
// as a signed residual instead of clamping to zero.
inline void bench_print_unattributed(uint8_t parent, const char *indent) {
  const BenchStat &p = bench_snap[parent];
  if (p.n == 0u) return;

  uint64_t children_us100 = 0;
  bool any = false;
  for (uint8_t i = 0; i < BENCH_COUNT; ++i) {
    if (bench_desc[i].parent != parent || bench_snap[i].n == 0u) continue;
    children_us100 += bench_to_us100(bench_snap[i].sum, bench_desc[i].kind);
    any = true;
  }
  if (!any) return;

  const uint64_t parent_us100 = bench_to_us100(p.sum, bench_desc[parent].kind);
  const uint32_t window = bench_window_us[bench_desc[parent].core];

  char name[32], tot[20], win[8], line[160];
  if (parent_us100 >= children_us100) {
    const uint64_t rest = parent_us100 - children_us100;
    snprintf(name, sizeof(name), "%s%s", indent, "(unattributed)");
    bench_fmt_us100(tot, sizeof(tot), rest);
    bench_fmt_win_us100(win, sizeof(win), rest, window);
  } else {
    const uint64_t rest = children_us100 - parent_us100;
    snprintf(name, sizeof(name), "%s%s", indent, "(over-attributed)");
    snprintf(tot, sizeof(tot), "-%lu.%02lu",
             (unsigned long)(rest / 100u), (unsigned long)(rest % 100u));
    bench_fmt_win_us100(win, sizeof(win), rest, window);
  }
  snprintf(line, sizeof(line), BENCH_ROW_FMT, name, "-", "-", "-", "-", tot, win);
  bench_out_println(line);
}

// Recurse so FINE children of note-on (etc.) appear; fixed 3-level walk stopped at voice_task.
// Indent capped so the 28-char name column stays usable (~8 levels).
inline void bench_print_subtree(uint8_t id, const char *indent) {
  bench_print_row(id, indent);

  char child_indent[16];
  const size_t len = strlen(indent);
  if (len + 2u >= sizeof(child_indent)) {
    return; // do not nest further
  }
  memcpy(child_indent, indent, len);
  child_indent[len] = ' ';
  child_indent[len + 1u] = ' ';
  child_indent[len + 2u] = '\0';

  bool any_child = false;
  for (uint8_t c = 0; c < BENCH_COUNT; ++c) {
    if (bench_desc[c].parent != id) continue;
    any_child = true;
    bench_print_subtree(c, child_indent);
  }
  if (any_child) {
    bench_print_unattributed(id, child_indent);
  }
}

inline void bench_print_core(uint8_t core) {
  char line[160];
  snprintf(line, sizeof(line), "-- Core %u  (window %lu.%03lu ms) --", (unsigned)core,
           (unsigned long)(bench_window_us[core] / 1000u),
           (unsigned long)(bench_window_us[core] % 1000u));
  bench_out_println(line);
  snprintf(line, sizeof(line), BENCH_ROW_FMT,
           "probe", "count", "mean", "min", "max", "total", "%win");
  bench_out_println(line);

  for (uint8_t i = 0; i < BENCH_COUNT; ++i) {
    if (bench_desc[i].core != core || bench_desc[i].parent != BENCH_NONE) continue;
    bench_print_subtree(i, "");
  }
}

// Ratio / amp-comp / portamento path breakdown for the same window as core 1 probes.
inline void bench_print_path_counters() {
  const BenchPathStat &p = bench_path_snap;
  const uint32_t ratio_seg =
      p.ratio_hit + p.ratio_miss_direct + p.ratio_miss_bsearch;
  const uint32_t amp_seg = p.amp_hit + p.amp_miss_walk + p.amp_miss_scan;
  const uint32_t porta_n = p.porta_off + p.porta_note_on + p.porta_retime +
                           p.porta_steady_time + p.porta_steady_slew;
  const bool ratio_any = (ratio_seg != 0u) || (p.ratio_clamp != 0u);
  const bool amp_any = (amp_seg != 0u) || (p.amp_clamp != 0u);
  if (!ratio_any && !amp_any && porta_n == 0u) {
    return;
  }

  bench_out_puts("\n");
  bench_out_println("-- Path counters (core 1, same window) --");
  if (ratio_any) {
    bench_out_printf(
        "ratio: hit=%lu miss_direct=%lu miss_bsearch=%lu clamp=%lu  "
        "walk_steps max=%lu sum=%lu\n",
        (unsigned long)p.ratio_hit, (unsigned long)p.ratio_miss_direct,
        (unsigned long)p.ratio_miss_bsearch, (unsigned long)p.ratio_clamp,
        (unsigned long)p.ratio_walk_steps_max, (unsigned long)p.ratio_walk_steps_sum);
    if (ratio_seg > 0u) {
      const uint32_t miss = p.ratio_miss_direct + p.ratio_miss_bsearch;
      const uint32_t miss_pm = (miss * 1000u) / ratio_seg;
      bench_out_printf("ratio miss rate: %lu.%lu%% of in-table calls\n",
                       (unsigned long)(miss_pm / 10u), (unsigned long)(miss_pm % 10u));
    }
  } else {
    bench_out_println("ratio: (not instrumented for this build/path)");
  }
  if (amp_any) {
    bench_out_printf(
        "amp: hit=%lu miss_walk=%lu miss_scan=%lu clamp=%lu  "
        "find_steps max=%lu sum=%lu\n",
        (unsigned long)p.amp_hit, (unsigned long)p.amp_miss_walk,
        (unsigned long)p.amp_miss_scan, (unsigned long)p.amp_clamp,
        (unsigned long)p.amp_walk_steps_max, (unsigned long)p.amp_walk_steps_sum);
    if (amp_seg > 0u) {
      const uint32_t miss = p.amp_miss_walk + p.amp_miss_scan;
      const uint32_t miss_pm = (miss * 1000u) / amp_seg;
      bench_out_printf("amp miss rate: %lu.%lu%% of in-band calls\n",
                       (unsigned long)(miss_pm / 10u), (unsigned long)(miss_pm % 10u));
    }
  } else {
    bench_out_println("amp: (not instrumented for this build/path)");
  }
  if (porta_n != 0u) {
    bench_out_printf(
        "porta: off=%lu note_on=%lu retime=%lu steady_time=%lu steady_slew=%lu\n",
        (unsigned long)p.porta_off, (unsigned long)p.porta_note_on,
        (unsigned long)p.porta_retime, (unsigned long)p.porta_steady_time,
        (unsigned long)p.porta_steady_slew);
  }
}

// Format the report into bench_out_buf (no Serial I/O).
inline void bench_print_report() {
  char line[192];
  bench_out_puts("\n");
  bench_out_println("=================== DCO BENCH ===================");
#ifdef RUNNING_AVERAGE_PERIOD
  snprintf(line, sizeof(line), "clk_sys %lu MHz   probe overhead %lu cyc   period only",
           (unsigned long)bench_cycles_per_us, (unsigned long)bench_overhead_cyc);
#elif defined(RUNNING_AVERAGE_FINE)
#if BENCH_STAGE_STRIDE > 1
  snprintf(line, sizeof(line),
           "clk_sys %lu MHz   probe overhead %lu cyc   fine probes on, stages every %u",
           (unsigned long)bench_cycles_per_us, (unsigned long)bench_overhead_cyc,
           (unsigned)BENCH_STAGE_STRIDE);
#else
  snprintf(line, sizeof(line), "clk_sys %lu MHz   probe overhead %lu cyc   fine probes on",
           (unsigned long)bench_cycles_per_us, (unsigned long)bench_overhead_cyc);
#endif
#elif BENCH_STAGE_STRIDE > 1
  snprintf(line, sizeof(line),
           "clk_sys %lu MHz   probe overhead %lu cyc   stages every %u",
           (unsigned long)bench_cycles_per_us, (unsigned long)bench_overhead_cyc,
           (unsigned)BENCH_STAGE_STRIDE);
#else
  snprintf(line, sizeof(line), "clk_sys %lu MHz   probe overhead %lu cyc   fine probes off",
           (unsigned long)bench_cycles_per_us, (unsigned long)bench_overhead_cyc);
#endif
  bench_out_println(line);

  // Compile-time engine flags + live runtime selectors (amp method / note retrig).
#if defined(PICO_RP2350)
  const char *mcu = "RP2350";
#elif defined(PICO_RP2040)
  const char *mcu = "RP2040";
#else
  const char *mcu = "?";
#endif
#ifdef USE_FLOAT_VOICE_TASK
  const char *voice = "FLOAT";
#else
  const char *voice = "FIXED";
#endif
  // Compile-time CLKDIV_MODE (both voice engines).
#if CLKDIV_MODE == CLKDIV_GOLD
  const char *clkdiv = "GOLD";
#elif CLKDIV_MODE == CLKDIV_FLOAT
  const char *clkdiv = "FLOAT";
#elif CLKDIV_MODE == CLKDIV_Q16
  const char *clkdiv = "Q16";
#elif CLKDIV_MODE == CLKDIV_Q8
  const char *clkdiv = "Q8";
#else
  const char *clkdiv = "FAST_Q4";
#endif
#ifdef USE_FLOAT_AMP_COMP
  const char *amp = "FLOAT";
#else
  const char *amp = "FIXED";
#endif
#ifdef USE_FLOAT_CV_OUTS
  const char *cv = "FLOAT";
#else
  const char *cv = "FIXED";
#endif
  // Grouped flag lines (aligned labels) so A/B rebuilds are obvious in the Board pane.
  snprintf(line, sizeof(line),
           "engine: mcu=%s voice=%s pitch=%s amp=%s cv=%s amp_method=%s clkdiv=%s note_retrig=%s "
           "amp_cal=%s",
           mcu, voice, bench_pitch_interp_mode_name(), amp, cv,
           amp_comp_method_name(amp_comp_method), clkdiv,
           note_retrig_mode_name(note_retrig_mode),
           autotune_amp_method_name(autotuneAmpMethod));
  bench_out_println(line);

  snprintf(line, sizeof(line),
           "adsr:   phase=%d float=%d micros=%d native_q15=%d dyadic=%d q15_cache=%d sram_hot=%d",
           (int)ADSR_BEZIER_PHASE_SHIFT,
           (int)ADSR_BEZIER_USE_FLOAT,
           (int)ADSR_BEZIER_USE_MICROS,
           (int)ADSR_BEZIER_NATIVE_Q15,
           (int)ADSR_BEZIER_Q15_DYADIC,
           (int)ADSR_BEZIER_UPDATE_Q15_CACHE,
           (int)ADSR_BEZIER_SRAM_HOT);
  bench_out_println(line);

  snprintf(line, sizeof(line), "lfo:    sram_hot=%d",
           (int)MO_LFO_SRAM_HOT);
  bench_out_println(line);

  snprintf(line, sizeof(line), "noise:  engine=%d out=%d",
           (int)NOISE_ENGINE,
#ifdef ENABLE_NOISE_OUT
           1
#else
           0
#endif
  );
  bench_out_println(line);

  snprintf(line, sizeof(line),
           "board:  cv_outs=%d wave_mux=%d voice_aux=%d pio_rst_inv=%d fs_cal=%d",
#ifdef ENABLE_CV_OUTS
           1,
#else
           0,
#endif
#ifdef ENABLE_WAVE_MUX
           1,
#else
           0,
#endif
#ifdef ENABLE_VOICE_AUX
           1,
#else
           0,
#endif
#ifdef ENABLE_PIO_RESET_INVERT
           1,
#else
           0,
#endif
#ifdef ENABLE_FS_CALIBRATION
           1
#else
           0
#endif
  );
  bench_out_println(line);

  snprintf(line, sizeof(line), "bench:  amp_comp=%d path_stats=%d",
#ifdef AMP_COMP_BENCHMARK
           1,
#else
           0,
#endif
#ifdef BENCH_PATH_STATS
           1
#else
           0
#endif
  );
  bench_out_println(line);

  bench_out_println("Times in us (mean/min/max as cycles when mean < 1 us). %win = share of wall clock (two decimals).");
  bench_out_puts("\n");
  bench_print_core(0);
  bench_out_puts("\n");
  bench_print_core(1);
#if !defined(RUNNING_AVERAGE_PERIOD) && (BENCH_STAGE_STRIDE > 1)
  snprintf(line, sizeof(line),
           "Stage probes every %u loops (means/%%win scaled; period every iter; note-on always).",
           (unsigned)BENCH_STAGE_STRIDE);
  bench_out_println(line);
#endif
#if defined(BENCH_PATH_STATS) && !defined(RUNNING_AVERAGE_PERIOD)
  bench_print_path_counters();
#endif
  bench_out_println("=================================================");
  bench_out_puts("\n");
}

// Drain handled by shared bench_out_drain_chunk() above.

// Forward declaration: period-probe report queued from core 1.
extern volatile bool pio_probe_report_pending;

// Core 0 side of the handshake: format into RAM when both cores are ready, then pace
// Serial TX across loop iterations. Printing never happens on core 1.
inline void bench_poll_core0() {
  if (bench_cdc_rx_pending()) {
    return;
  }

  if (pio_probe_report_pending && !bench_out_active) {
    pio_probe_report_flush();
  }

  if (bench_out_drain_chunk()) {
    return;
  }

  if (bench_periodic && !bench_dump_request) {
    static uint32_t last_ms = 0;
    const uint32_t now_ms = millis();
    if (now_ms - last_ms >= 1000u) {
      last_ms = now_ms;
      bench_dump_request = true;
    }
  }

  bench_service(0);

  if (bench_dump_request && bench_core_ready[0] && bench_core_ready[1] &&
      !bench_cdc_rx_pending()) {
    bench_out_reset();
    bench_print_report();
    // Engine flags already on the build: line in bench_print_report().
    bench_core_ready[0] = false;
    bench_core_ready[1] = false;
    bench_dump_request = false;
    bench_out_active = (bench_out_len > 0u);
    bench_out_drain_chunk();
  }

  // Amp-comp method ack (PARAM_DEBUG_COMMAND 20–22). Wait until paced TX idle.
  if (!bench_out_active && amp_comp_method_ack_pending) {
    amp_comp_method_ack_pending = false;
    bench_out_reset();
    bench_out_printf("amp_comp method=%s (profiler reset)\n",
                     amp_comp_method_name(amp_comp_method));
    bench_out_active = (bench_out_len > 0u);
    bench_out_drain_chunk();
  }

  if (!bench_out_active && note_retrig_mode_ack_pending) {
    note_retrig_mode_ack_pending = false;
    bench_out_reset();
    bench_out_printf("note_retrig=%s\n", note_retrig_mode_name(note_retrig_mode));
    bench_out_active = (bench_out_len > 0u);
    bench_out_drain_chunk();
  }

  // One-shot amp-comp speed/accuracy (PARAM_DEBUG_COMMAND 24/25). Wait until paced TX idle.
  if (!bench_out_active &&
      (amp_comp_bench_speed_pending || amp_comp_bench_accuracy_pending)) {
    bench_out_reset();
    print_amp_comp_bench();
    bench_out_active = (bench_out_len > 0u);
    bench_out_drain_chunk();
  }

  // One-shot pitch-interp speed/accuracy (PARAM_DEBUG_COMMAND 28/29).
  if (!bench_out_active &&
      (pitch_interp_bench_speed_pending || pitch_interp_bench_accuracy_pending)) {
    bench_out_reset();
    print_pitch_interp_bench();
    bench_out_active = (bench_out_len > 0u);
    bench_out_drain_chunk();
  }

  // One-shot six-method clkdiv speed/accuracy vs GOLD_REF (32/33).
  if (!bench_out_active &&
      (clkdiv_hp_bench_speed_pending || clkdiv_hp_bench_accuracy_pending)) {
    bench_out_reset();
    print_clkdiv_hp_bench();
    bench_out_active = (bench_out_len > 0u);
    bench_out_drain_chunk();
  }
}

#else  // !RUNNING_AVERAGE

#define BENCH_BEGIN(id)  ((void)0)
#define BENCH_END(id)    ((void)0)
#define BENCH_PERIOD(id) ((void)0)
#define BENCH_SAMPLE_TICK() ((void)0)
#define BENCH_FBEGIN(id) ((void)0)
#define BENCH_FEND(id)   ((void)0)
#define BENCH_PATH_INC(field) ((void)0)
static inline void bench_path_walk_steps(uint32_t) {}
static inline void bench_path_amp_walk_steps(uint32_t) {}

inline void bench_service(uint8_t) {}
inline void bench_reset_all() {}

inline void bench_poll_core0() {
  if (bench_cdc_rx_pending()) {
    return;
  }
  if (pio_probe_report_pending && !bench_out_active) {
    pio_probe_report_flush();
  }
  bench_out_drain_chunk();
}

#endif  // RUNNING_AVERAGE

#endif  // __BENCH_H__
