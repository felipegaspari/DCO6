// ========================================================
// DMA WAVESHAPER PIO (OSC B / Triangle Core)
// ========================================================
uint pio_offset_triangle[2];

#define dco_waveshaper_wrap_target 0
#define dco_waveshaper_wrap 3

static const uint16_t dco_waveshaper_instructions[] = {
    // .wrap_target
    0x80a0, //  0: pull   block           ; Wait for next 32-bit DMA segment
    0x6001, //  1: out    pins, 1         ; Pop Bit 0 (Switch State) directly to pin!
    0x605f, //  2: out    y, 31           ; Pop Bits 1-31 (Duration) into Y
    0x0083, //  3: jmp    y--, 3          ; Wait for segment duration
    // .wrap
};

static const struct pio_program dco_waveshaper_program = {
    .instructions = dco_waveshaper_instructions,
    .length = 4,
    .origin = -1,
};

static inline pio_sm_config dco_waveshaper_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + dco_waveshaper_wrap_target, offset + dco_waveshaper_wrap);
    return c;
}

void dco_waveshaper_init(PIO pio, uint sm, uint offset, uint pin) {
    pio_sm_config c = dco_waveshaper_get_default_config(offset);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
    sm_config_set_out_pins(&c, pin, 1); 
    pio_gpio_init(pio, pin);
    pio_sm_init(pio, sm, offset, &c);
}

#include "hardware/structs/dma.h"

// --- DMA SETUP ---
// Two 64-word halves, 512-byte ring. Hardware wraps A then B. No period IRQ.
uint32_t dco_wave_buffer[NUM_VOICES_TOTAL][2][DCO_WAVE_SLOTS] __attribute__((aligned(512)));
int dco_dma_chan[NUM_VOICES_TOTAL] = {-1, -1, -1, -1};

static uint32_t dco_pack_seed(uint32_t state, uint32_t cycles) {
    if (cycles < 4) cycles = 4;
    return ((cycles - 4) << 1) | (state & 1u);
}

static void dco_wave_seed(uint voice) {
    const uint32_t slice = DCO_WAVE_NATIVE / DCO_WAVE_SLOTS;
    const uint32_t up = dco_pack_seed(1, slice);
    const uint32_t dn = dco_pack_seed(0, slice);
    for (int h = 0; h < 2; h++) {
        for (int i = 0; i < 32; i++) dco_wave_buffer[voice][h][i] = up;
        for (int i = 32; i < DCO_WAVE_SLOTS; i++) dco_wave_buffer[voice][h][i] = dn;
    }
}

void dco_wave_set_clkdiv(uint8_t voice, uint32_t total_cycles) {
    if (voice >= NUM_VOICES_TOTAL) return;
    const uint8_t osc = (uint8_t)(voice * 2u + 1u);
    PIO pio_hw = pio[VOICE_TO_PIO[osc]];
    const uint sm = VOICE_TO_SM[osc];
    uint32_t q8 = (uint32_t)(((uint64_t)total_cycles << 8) / DCO_WAVE_NATIVE);
    if (q8 < 256u) q8 = 256u;
    const uint16_t div_int = (uint16_t)(q8 >> 8);
    const uint8_t div_frac = (uint8_t)(q8 & 0xFFu);
    pio_sm_set_clkdiv_int_frac(pio_hw, sm, div_int, div_frac);
}

int dco_wave_commit(uint8_t voice, const uint32_t* baked, uint16_t n) {
    if (voice >= NUM_VOICES_TOTAL || baked == nullptr) return -1;
    const int chan = dco_dma_chan[voice];
    if (chan < 0) return -1;
    if (n > DCO_WAVE_SLOTS) n = DCO_WAVE_SLOTS;

    const uintptr_t base = (uintptr_t)&dco_wave_buffer[voice][0][0];
    const uintptr_t ra = (uintptr_t)dma_hw->ch[chan].read_addr;
    const uint32_t idx = ((uint32_t)(ra - base) / 4u) & 127u;
    const uint32_t pos = idx & 63u;
    if (pos >= 60u) return -1;

    const int idle = (int)(1u - (idx >> 6));
    uint32_t* dst = dco_wave_buffer[voice][idle];
    for (uint16_t i = 0; i < DCO_WAVE_SLOTS; i++) dst[i] = baked[i];
    return idle;
}

void init_dco_dma(uint voice, PIO pio, uint sm) {
    if (dco_dma_chan[voice] == -1) {
        dco_dma_chan[voice] = dma_claim_unused_channel(true);
    } else {
        dma_channel_abort((uint)dco_dma_chan[voice]);
        dma_channel_set_irq0_enabled((uint)dco_dma_chan[voice], false);
    }
    int chan = dco_dma_chan[voice];
    dma_channel_config c = dma_channel_get_default_config((uint)chan);

    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    channel_config_set_ring(&c, false, 9);

    dco_wave_seed(voice);
    dco_wave_invalidate_bake((uint8_t)voice);
    dma_channel_configure((uint)chan, &c,
        &pio->txf[sm],
        &dco_wave_buffer[voice][0][0],
        0xFFFFFFFFu,
        true
    );
}

void reset_dco_dma_phase(uint8_t voice, PIO pio, uint sm, uint pio_block_idx) {
    int chan = dco_dma_chan[voice];
    pio_sm_set_enabled(pio, sm, false);
    dma_channel_abort((uint)chan);
    pio_sm_clear_fifos(pio, sm);

    pio_sm_exec(pio, sm, pio_encode_jmp(pio_offset_triangle[pio_block_idx]));

    dma_channel_set_trans_count((uint)chan, 0xFFFFFFFFu, false);
    dma_channel_set_read_addr((uint)chan, &dco_wave_buffer[voice][0][0], true);
    pio_sm_set_enabled(pio, sm, true);
}


// Per-voice even/odd pairing (DCO_A = v*2, DCO_B = v*2+1).
// syncMode 1: B's sideset drives A's reset (A slave, B master).
// syncMode 2: A's sideset drives B's reset (B slave, A master).
static inline int pair_slave(int voice) {
  if (__builtin_expect(syncMode == 0, 0)) return -1;
  // syncMode 1 -> a + 0; syncMode 2 -> a + 1
  return (voice << 1) + (syncMode - 1); 
}

static inline int pair_master(int voice) {
  if (__builtin_expect(syncMode == 0, 0)) return -1;
  // syncMode 1 -> a + 1; syncMode 2 -> a + 0
  return (voice << 1) + (2 - syncMode); 
}

// Give the slave a lower state machine index than its master within the PIO block.
// When two SMs write the same pin on the same cycle the higher-numbered one wins.
void assign_sm_mapping() {
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    VOICE_TO_SM[i] = (uint8_t)(i & 3);
  }
  if (syncMode == 0) {
    return;
  }
  for (int v = 0; v < NUM_VOICES_TOTAL; v++) {
    const int slave = pair_slave(v);
    const int master = pair_master(v);
    if (slave < 0 || master < 0) continue;
    if (VOICE_TO_SM[slave] > VOICE_TO_SM[master]) {
      const uint8_t tmp = VOICE_TO_SM[slave];
      VOICE_TO_SM[slave] = VOICE_TO_SM[master];
      VOICE_TO_SM[master] = tmp;
    }
  }
}

// Pick the soft-sync poll program image for N trailing polled chunks (1..3).
static const pio_program_t *soft_sync_program_for_chunks(uint8_t chunks) {
  if (chunks >= 3) return &frequency_sync_poll_3_program;
  if (chunks == 2) return &frequency_sync_poll_2_program;
  return &frequency_sync_poll_program;
}

// Keep free-running + exactly one poll image on a freq PIO block. Swapping among
// N=1/2/3 removes the old poll program and loads the new one (12 + 13..15 <= 27/32).
static void ensure_soft_sync_program(uint8_t pio_idx, uint8_t chunks) {
  if (chunks < 1) chunks = 1;
  if (chunks > 3) chunks = 3;
  if (chunks == pio_loaded_sync_chunks[pio_idx]) return;

  if (pio_loaded_sync_chunks[pio_idx] != 0) {
    pio_remove_program(pio[pio_idx],
                       soft_sync_program_for_chunks(pio_loaded_sync_chunks[pio_idx]),
                       pio_offset_sync[pio_idx]);
  }
  pio_offset_sync[pio_idx] = pio_add_program(pio[pio_idx], soft_sync_program_for_chunks(chunks));
  pio_loaded_sync_chunks[pio_idx] = chunks;
}

// Load oscillator programs into pio0+pio1 and start all voice SMs. Called from setup1().
// RP2350: also claim pio2 SM0–3 for per-voice sub-osc (no noise LFSR on either MCU).
void init_pio() {
  for (int sm = 0; sm < 4; sm++) {
    pio_sm_claim(pio[0], sm);
    pio_sm_claim(pio[1], sm);
  }

  const uint8_t syncChunks = softSyncChunks > 0 ? softSyncChunks : 1;
  for (int blk = 0; blk < 2; blk++) {
    pio_offset_free[blk] = pio_add_program(pio[blk], &frequency_sync_4_jumps_program);
    pio_offset_triangle[blk] = pio_add_program(pio[blk], &dco_waveshaper_program);
    pio_loaded_sync_chunks[blk] = 0;
    ensure_soft_sync_program((uint8_t)blk, syncChunks);
  }

#if defined(PICO_RP2350)
  for (int sm = 0; sm < NUM_VOICES_TOTAL; sm++) {
    pio_sm_claim(pio[SUBOSC_PIO], sm);
  }
  subosc_offset_div2 = pio_add_program(pio[SUBOSC_PIO], &subosc_div2_program);
  subosc_offset_div4 = pio_add_program(pio[SUBOSC_PIO], &subosc_div4_program);
#endif

  assign_sm_mapping();
  start_voice_sms();
  set_subosc_divide(subOscDivide);
}

// Match RESET pad polarity to the analog discharge switch. When ENABLE_PIO_RESET_INVERT
// is set, logical PIO "1 = assert" becomes pad low (DG411 on) while INOVER keeps jmp_pin
// / wait readers on the logical sense. Cleared explicitly when the flag is off so a
// rebuild without the define does not leave stale overrides after a soft reset.
static void pio_reset_pin_apply_polarity(uint pin) {
#ifdef ENABLE_PIO_RESET_INVERT
  gpio_set_outover(pin, GPIO_OVERRIDE_INVERT);
  gpio_set_inover(pin, GPIO_OVERRIDE_INVERT);
#else
  gpio_set_outover(pin, GPIO_OVERRIDE_NORMAL);
  gpio_set_inover(pin, GPIO_OVERRIDE_NORMAL);
#endif
}

// Configure every oscillator SM on its PIO block and start them same-cycle per block.
// Safe to call again whenever the sync topology changes.
void start_voice_sms() {
  const bool anySync = (syncMode == 1 || syncMode == 2);
  const bool softSync = (softSyncChunks > 0) && anySync;
  uint8_t chunks = soft_sync_chunks_clamped();
  if (chunks < 1) chunks = 1;

  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    pio_sm_set_enabled(pio[VOICE_TO_PIO[i]], VOICE_TO_SM[i], false);
  }
  if (softSync) {
    ensure_soft_sync_program(0, chunks);
    ensure_soft_sync_program(1, chunks);
  }

  uint32_t enableMask[2] = { 0, 0 };

  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    const uint8_t blk = VOICE_TO_PIO[i];
    const uint8_t sm = VOICE_TO_SM[i];
    const int voice = i / 2;
    const int slave = pair_slave(voice);
    const int master = pair_master(voice);

    // Hard sync: master's sideset drives the slave's RESET (same PIO block only).
    // Soft sync: slave polls master's pin; master keeps sideset on its own RESET.
    uint8_t sidesetPin = RESET_PINS[i];
    if (!softSync && master >= 0 && i == master) {
      sidesetPin = RESET_PINS[slave];
    }

    pio_sm_clear_fifos(pio[blk], sm);


    // ==========================================
    // NEW LOGIC: Odd indices (OSC B) get DMA Waveshaper
    // ==========================================
    if (i % 2 != 0) { 
      // It's OSC B (Triangle Core)
      dco_waveshaper_init(pio[blk], sm, pio_offset_triangle[blk], RESET_PINS[i]);
      osc_uses_sync_program[i] = false;
      
      // Initialize the DMA stream feeding this voice!
      init_dco_dma(voice, pio[blk], sm);
    } 
    else {
      // It's OSC A (Sawtooth Core)
      if (softSync && i == slave) {
        frequency_sync_poll_init(pio[blk], sm, pio_offset_sync[blk], RESET_PINS[i], sidesetPin,
                                 RESET_PINS[master], chunks);
        osc_uses_sync_program[i] = true;
      } else {
        frequency_sync_4_jumps(pio[blk], sm, pio_offset_free[blk], RESET_PINS[i], sidesetPin);
        osc_uses_sync_program[i] = false;
      }
      osc_load_period_stopped(i, pioPulseLength, osc_last_clk_div[i]);
    }
    enableMask[blk] |= (1u << sm);
  }

  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    pio_reset_pin_apply_polarity(RESET_PINS[i]);
  }

  pio_enable_sm_mask_in_sync(pio[0], enableMask[0]);
  pio_enable_sm_mask_in_sync(pio[1], enableMask[1]);
}

// Reload every oscillator's reset pulse width (Y), preserving the running period.
//
// Period was Y_old + weight*clk_div + overhead. With a new Y the divider must be
// recomputed before re-enable; keeping the old clk_div stretches/shrinks pitch until
// the next voice_task frame. All SMs are stopped, loaded, then started in the same
// cycle so sync pairs do not tear.
void osc_reload_reset_pulse_all(uint32_t y) {
  uint32_t enableMask[2] = { 0, 0 };
  
  // Pillar II: Restrict pointers to guarantee no memory aliasing during hardware writes
  const uint32_t* __restrict p_last_y = osc_last_y;
  uint32_t* __restrict p_last_clk = osc_last_clk_div;
  
  // Pillar VI: Unroll PIO halt loop to avoid pipeline branch stalls
  _Pragma("GCC unroll 8")
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
      const uint8_t blk = VOICE_TO_PIO[i];
      const uint8_t sm = VOICE_TO_SM[i];
      pio_sm_set_enabled(pio[blk], sm, false);
      enableMask[blk] |= (1u << sm);
  }
  
  _Pragma("GCC unroll 8")
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    if (i % 2 != 0) continue; // SKIPPING OSC B (DMA handles itself)
    
      const uint32_t weight = osc_ramp_weight(i);
      const uint32_t overhead = osc_period_overhead(i);
      
      // Pillar III: Isolate addition to force a 1-cycle MLA (Multiply-Accumulate)
      // total = base + (weight * clk_div)
      const uint32_t base_cycles = p_last_y[i] + overhead;
      const uint32_t total = base_cycles + (weight * p_last_clk[i]); 
      
      const uint32_t clk_div = pio_clk_div_for_y(total, y, weight, overhead);
      osc_load_period_stopped(i, y, clk_div);
  }
  
  pio_enable_sm_mask_in_sync(pio[0], enableMask[0]);
  pio_enable_sm_mask_in_sync(pio[1], enableMask[1]);
}

// Report the sync topology and, importantly, which PIO block owns each reset pin.
//
// This is the check that catches the bug this rework fixes: a GPIO's function select can
// name only one block, so when the oscillators lived on pio0/pio1/pio2 the master's
// pio_gpio_init() silently re-pointed the slave's reset pin at the master's block and the
// slave stopped driving its own core. Every RESET pin must read back as PIO0 here.
void pio_topology_report() {
  bench_out_reset();
  bench_out_printf("[pio topology] syncMode=%u softSyncChunks=%u voices=%u oscs=%u\n",
                   syncMode, softSyncChunks, NUM_VOICES_TOTAL, NUM_OSCILLATORS);

  bool ownershipOk = true;
  for (int i = 0; i < NUM_OSCILLATORS; i++) {
    gpio_function_t fn = gpio_get_function(RESET_PINS[i]);
    const gpio_function_t expect =
        (VOICE_TO_PIO[i] == 0) ? GPIO_FUNC_PIO0 : GPIO_FUNC_PIO1;
    const bool onExpected = (fn == expect);
    if (!onExpected) ownershipOk = false;

    const char *prog = "free";
    if (osc_uses_sync_program[i]) {
      switch (soft_sync_chunks_clamped()) {
        case 2:  prog = "poll2"; break;
        case 3:  prog = "poll3"; break;
        default: prog = "poll1"; break;
      }
    }
    bench_out_printf("  OSC%d reset=GP%-2u pio=%u sm=%u program=%s funcsel=%d%s\n",
                     i + 1, RESET_PINS[i], VOICE_TO_PIO[i], VOICE_TO_SM[i], prog,
                     (int)fn, onExpected ? "" : "  <-- WRONG BLOCK");
  }

  bench_out_printf("  reset pin ownership: %s\n",
                   ownershipOk ? "OK (pair-local PIO)" : "BROKEN (a pin was stolen)");

  for (int v = 0; v < NUM_VOICES_TOTAL; v++) {
    const int slave = pair_slave(v);
    const int master = pair_master(v);
    if (master < 0 || slave < 0) continue;
    bench_out_printf("  V%d master OSC%d sm=%u, slave OSC%d sm=%u -> tie-break %s\n",
                     v, master + 1, VOICE_TO_SM[master], slave + 1, VOICE_TO_SM[slave],
                     VOICE_TO_SM[master] > VOICE_TO_SM[slave] ? "OK (master outranks slave)"
                                                              : "WRONG (master can lose edges)");
  }
  bench_out_active = true;
}

// Park one oscillator at clk_div (core 1 only — called from pio_defer_service).
static void pio_period_probe_run(uint8_t osc, uint32_t clk_div) {
  const uint8_t blk = VOICE_TO_PIO[osc];
  const uint8_t sm = VOICE_TO_SM[osc];

  pio_sm_set_enabled(pio[blk], sm, false);
  osc_load_period_stopped(osc, pioPulseLength, clk_div);
  pio_sm_set_enabled(pio[blk], sm, true);
}

void pio_period_probe(uint8_t osc, uint32_t clk_div) {
  pio_defer_request_period_probe(osc, clk_div);
}

// Solve period = Y + weight*clk_div + overhead from two frequency-counter readings taken
// with pio_period_probe() at the same Y. Prints the measured weight and overhead so they
// can be compared against PIO_RAMP_WEIGHT_* / PIO_PERIOD_OVERHEAD_*.
void pio_solve_period_model(uint32_t clk_div_a, double measured_hz_a,
                            uint32_t clk_div_b, double measured_hz_b,
                            uint32_t y) {
  if (clk_div_a == clk_div_b || measured_hz_a <= 0.0 || measured_hz_b <= 0.0) {
    Serial.println("[period solve] need two distinct clk_div values and non-zero readings");
    return;
  }

  double period_a = (double)sysClock_Hz / measured_hz_a;
  double period_b = (double)sysClock_Hz / measured_hz_b;

  double weight = (period_a - period_b) / ((double)clk_div_a - (double)clk_div_b);
  double overhead = period_a - (double)y - weight * (double)clk_div_a;

  Serial.printf("[period solve] weight = %.4f (expected %u/%u/%u/%u for chunks 0..3)\n",
                weight,
                (unsigned)PIO_RAMP_WEIGHT_BY_CHUNKS[0],
                (unsigned)PIO_RAMP_WEIGHT_BY_CHUNKS[1],
                (unsigned)PIO_RAMP_WEIGHT_BY_CHUNKS[2],
                (unsigned)PIO_RAMP_WEIGHT_BY_CHUNKS[3]);
  Serial.printf("[period solve] overhead = %.2f cycles (expected %u/%u/%u/%u)\n",
                overhead,
                (unsigned)PIO_PERIOD_OVERHEAD_BY_CHUNKS[0],
                (unsigned)PIO_PERIOD_OVERHEAD_BY_CHUNKS[1],
                (unsigned)PIO_PERIOD_OVERHEAD_BY_CHUNKS[2],
                (unsigned)PIO_PERIOD_OVERHEAD_BY_CHUNKS[3]);
}

// (Re)configure per-voice sub-oscs. divide 0 stops them; 2 / 4 = one / two octaves
// below that voice's OSC1 RESET. RP2040: no PIO (param stored only).
void set_subosc_divide(uint8_t divide) {
  subOscDivide = divide;
#if defined(PICO_RP2350)
  PIO p = pio[SUBOSC_PIO];
  for (int v = 0; v < NUM_VOICES_TOTAL; v++) {
    pio_sm_set_enabled(p, (uint)v, false);
  }
  if (divide == 0) {
    return;
  }
  const bool div4 = (divide >= 4);
  const uint offset = div4 ? subosc_offset_div4 : subosc_offset_div2;
  for (int v = 0; v < NUM_VOICES_TOTAL; v++) {
    if (SUBOSC_PINS[v] == SUBOSC_PIN_UNASSIGNED) continue;
    subosc_init(p, (uint)v, offset, RESET_PINS[v * 2], SUBOSC_PINS[v], div4);
    pio_sm_set_enabled(p, (uint)v, true);
  }
#else
  (void)divide;
#endif
}

// ---- Core-0 → core-1 deferred PIO requests -----------------------------------
// Serial/MIDI handlers on core 0 must not touch PIO while voice_task_main on core 1
// is driving the same state machines.

static volatile uint8_t pio_defer_pending = 0;
static volatile uint8_t pio_defer_subosc_value = 0;
static volatile uint8_t pio_defer_probe_osc = 0;
static volatile uint32_t pio_defer_probe_clk_div = 0;

volatile bool pio_probe_report_pending = false;
static volatile uint8_t pio_probe_report_osc = 0;
static volatile uint8_t pio_probe_report_sm = 0;
static volatile uint32_t pio_probe_report_clk_div = 0;
static volatile uint32_t pio_probe_report_weight = 0;
static volatile uint32_t pio_probe_report_overhead = 0;
static volatile uint32_t pio_probe_report_predicted = 0;

static constexpr uint8_t PIO_DEFER_SYNC   = 1u << 0;
static constexpr uint8_t PIO_DEFER_RESET  = 1u << 1;
static constexpr uint8_t PIO_DEFER_SUBOSC = 1u << 2;
static constexpr uint8_t PIO_DEFER_PROBE  = 1u << 3;
// Bit 7: bits 4-6 are the DCO3 tree's per-sub requests, kept free here so both
// trees can carry the same value.
static constexpr uint8_t PIO_DEFER_CAL_RESTORE = 1u << 7;

void pio_defer_request_sync_mode() {
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_SYNC, __ATOMIC_SEQ_CST);
}

void pio_defer_request_cal_restore() {
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_CAL_RESTORE, __ATOMIC_SEQ_CST);
}

void pio_defer_request_reset_pulse_all() {
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_RESET, __ATOMIC_SEQ_CST);
}

void pio_defer_request_subosc(uint8_t divide) {
  pio_defer_subosc_value = divide;
  __dmb();
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_SUBOSC, __ATOMIC_SEQ_CST);
}

void pio_defer_request_period_probe(uint8_t osc, uint32_t clk_div) {
  pio_defer_probe_osc = osc;
  pio_defer_probe_clk_div = clk_div;
  __dmb();
  __atomic_fetch_or(&pio_defer_pending, PIO_DEFER_PROBE, __ATOMIC_SEQ_CST);
}

void pio_probe_report_flush() {
  if (!pio_probe_report_pending) {
    return;
  }
  pio_probe_report_pending = false;

  const uint8_t osc = pio_probe_report_osc;
  bench_out_reset();
  bench_out_printf("[period probe] osc=%u sm=%u pin=%u\n",
                   (unsigned)osc, (unsigned)pio_probe_report_sm,
                   (unsigned)RESET_PINS[osc]);
  bench_out_printf("  Y=%lu clk_div=%lu weight=%lu overhead=%lu\n",
                   (unsigned long)pioPulseLength,
                   (unsigned long)pio_probe_report_clk_div,
                   (unsigned long)pio_probe_report_weight,
                   (unsigned long)pio_probe_report_overhead);
  bench_out_printf("  predicted period = %lu cycles = %.4f Hz\n",
                   (unsigned long)pio_probe_report_predicted,
                   (double)sysClock_Hz / (double)pio_probe_report_predicted);
  bench_out_active = true;
}

void pio_defer_service() {
  const uint8_t pending =
      __atomic_exchange_n(&pio_defer_pending, 0, __ATOMIC_SEQ_CST);
  if (pending == 0) {
    return;
  }

  if (pending & PIO_DEFER_CAL_RESTORE) {
    // Manual cal stops every oscillator SM but the soloed one and zeroes the PW
    // channels; nothing in the play path ever starts them again.
    restore_voice_engine_after_calibration();
  }
  if (pending & PIO_DEFER_SYNC) {
    setSyncMode();
  }
  if (pending & PIO_DEFER_RESET) {
    // Y only; do not force note_on_flag — that retriggered every slider step (~50 Hz)
    // and was the main source of mid-drag pitch collapse / sync tear.
    osc_reload_reset_pulse_all(pioPulseLength);
  }
  if (pending & PIO_DEFER_SUBOSC) {
    set_subosc_divide(pio_defer_subosc_value);
  }
  if (pending & PIO_DEFER_PROBE) {
    const uint8_t osc = pio_defer_probe_osc;
    const uint32_t clk_div = pio_defer_probe_clk_div;
    pio_period_probe_run(osc, clk_div);
    pio_probe_report_osc = osc;
    pio_probe_report_sm = VOICE_TO_SM[osc];
    pio_probe_report_clk_div = clk_div;
    pio_probe_report_weight = osc_ramp_weight(osc);
    pio_probe_report_overhead = osc_period_overhead(osc);
    pio_probe_report_predicted =
        pioPulseLength + pio_probe_report_weight * clk_div + pio_probe_report_overhead;
    pio_probe_report_pending = true;
  }
}
