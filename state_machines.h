#ifndef __STATE_MACHINES_H__
#define __STATE_MACHINES_H__

#include <stdint.h>
#include "hardware/pio.h"
#include "hardware/dma.h"

// --- DMA WAVESHAPING ARCHITECTURE ---
// 64-word list, two halves. DMA rings 512 bytes (A then B). CPU writes the idle half.
#define DCO_WAVE_SLOTS 64
#define DCO_WAVE_NATIVE 32768u
extern uint32_t dco_wave_buffer[NUM_VOICES_TOTAL][2][DCO_WAVE_SLOTS];

// Declared here so the sketch prototype pass can see it. The .ino helpers
// that take this type are prototyped before voices.ino is parsed.
struct WaveSeg {
    uint32_t cycles;
    uint8_t dir;
};
extern int dco_dma_chan[NUM_VOICES_TOTAL];

// DMA list holds one native-length period. Pitch is the SM clkdiv, updated
// every voice_task frame so the open ramp time-warps with the note.
int dco_wave_commit(uint8_t voice, const uint32_t* baked, uint16_t n);
void dco_wave_set_clkdiv(uint8_t voice, uint32_t total_cycles);
void dco_wave_invalidate_bake(uint8_t voice);

void init_pio();
void start_voice_sms();
void assign_sm_mapping();
void set_subosc_divide(uint8_t divide);

// NEW: Phase resync for the DMA streamer (used during Retriggers)
void reset_dco_dma_phase(uint8_t voice, PIO pio, uint sm, uint pio_block_idx);

// Load a reset pulse width (Y) and clk_div into an oscillator whose SM is already
// stopped. The caller is responsible for stopping and re-enabling, so that paired
// oscillators can be restarted on the same cycle.
//
// Y only reaches the SM through the OSR (put -> pull -> out y), and the OSR also holds
// clk_div for the four `mov x, OSR` chunk reads. Writing Y on a *running* SM therefore
// leaves a window in which a chunk can latch the pulse width as its ramp count, which
// is very audible. Hence the stopped-SM requirement.
//
// Inline + direct TXF/instr MMIO: note-on EXACT_Y calls the fused noclear variant;
// boot/topology paths use osc_load_period_stopped() which still FJOIN-clears TX.

static inline pio_hw_t *osc_pio_hw(uint8_t osc) {
  // Zero branches. Evaluates to PIO0_BASE (0x50200000) or PIO1_BASE (0x50300000).
  // Compiles to a single shift-left (LSL) and add instruction.
  return (pio_hw_t *)(PIO0_BASE + (VOICE_TO_PIO[osc] << 20));
}

static inline void osc_load_period_stopped_noclear(uint8_t osc, uint32_t y, uint32_t clk_div) {
  const uint sm = VOICE_TO_SM[osc];
  pio_hw_t *const hw = osc_pio_hw(osc);

  const uint pull_instr = pio_encode_pull(false, false);
  const uint out_y_instr = pio_encode_out(pio_y, 31);

  hw->txf[sm] = y;
  hw->sm[sm].instr = pull_instr;
  hw->sm[sm].instr = out_y_instr;
  osc_last_y[osc] = y;

  hw->txf[sm] = clk_div;
  hw->sm[sm].instr = pull_instr;
  osc_last_clk_div[osc] = clk_div;
}

// Note-on EXACT_Y after the frame's pio put+pull: TX is empty, so FJOIN clear is skipped.
// Hoists pull/out encodings once and loads OSC A then B back-to-back.
static inline void SRAM_HOT(osc_load_periods_stopped_noclear)(
  uint8_t osc_a, uint32_t y_a, uint32_t clk_div_a,
  uint8_t osc_b, uint32_t y_b, uint32_t clk_div_b) 
{
  // Caching instruction constants in CPU registers (r4-r11) prior to memory writes
  register const uint pull_instr = pio_encode_pull(false, false);
  register const uint out_y_instr = pio_encode_out(pio_y, 31);
  
  pio_hw_t *const hw = osc_pio_hw(osc_a); // Assumes osc_b is on the same PIO block!
  const uint sm_a = VOICE_TO_SM[osc_a];
  const uint sm_b = VOICE_TO_SM[osc_b];
  
  // OSC A
  hw->txf[sm_a] = y_a;
  hw->sm[sm_a].instr = pull_instr;
  hw->sm[sm_a].instr = out_y_instr;
  
  hw->txf[sm_a] = clk_div_a;
  hw->sm[sm_a].instr = pull_instr;
  
  // OSC B
  hw->txf[sm_b] = y_b;
  hw->sm[sm_b].instr = pull_instr;
  hw->sm[sm_b].instr = out_y_instr;
  
  hw->txf[sm_b] = clk_div_b;
  hw->sm[sm_b].instr = pull_instr;
  
  // Update tracking at the end to allow consecutive volatile writes 
  // to pipeline over the RP2350 memory bus without stalls.
  osc_last_y[osc_a] = y_a;
  osc_last_clk_div[osc_a] = clk_div_a;
  osc_last_y[osc_b] = y_b;
  osc_last_clk_div[osc_b] = clk_div_b;
}

// Note-on phase align after osc_load_periods_stopped_noclear (OSR holds clk_div, Y is
// the real pulse). Preload X with a one-shot countdown, restore clk_div, release reset,
// jmp loop_final. First flyback is delayed; later cycles use Y unchanged.
// SM must already be stopped. Old 8-chunk recipe: jmp 10; out x.
static inline void osc_phase_align_hold_stopped(uint8_t osc, uint32_t x_count) {
  const uint sm = VOICE_TO_SM[osc];
  pio_hw_t *const hw = osc_pio_hw(osc);
  const uint32_t clk_div = osc_last_clk_div[osc];

  const uint pull_instr = pio_encode_pull(false, false);
  const uint out_x_instr = pio_encode_out(pio_x, 31);
  const uint set0_instr = pio_encode_set(pio_pins, 0);
  const uint jmp_instr = pio_encode_jmp(osc_phase_hold_target(osc));

  hw->txf[sm] = x_count;
  hw->sm[sm].instr = pull_instr;
  hw->sm[sm].instr = out_x_instr;
  hw->txf[sm] = clk_div;
  hw->sm[sm].instr = pull_instr;
  hw->sm[sm].instr = set0_instr;
  hw->sm[sm].instr = jmp_instr;
}

// Boot / topology paths: disable does not empty TX — clear before Y/OSR reload.
static inline void osc_load_period_stopped(uint8_t osc, uint32_t y, uint32_t clk_div) {
  const uint sm = VOICE_TO_SM[osc];
  pio_hw_t *const hw = osc_pio_hw(osc);

  // Same FJOIN-RX trick as pio_sm_clear_fifos(), without the call.
  hw_set_bits(&hw->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
  hw_clear_bits(&hw->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);

  osc_load_period_stopped_noclear(osc, y, clk_div);
}

void osc_reload_reset_pulse_all(uint32_t y);
void pio_topology_report();
void pio_period_probe(uint8_t osc, uint32_t clk_div);
void pio_solve_period_model(uint32_t clk_div_a, double measured_hz_a,
                            uint32_t clk_div_b, double measured_hz_b, uint32_t y);

// Rebuild the sync topology (voices.ino) — core 1 only. Declared here because the
// deferred queue and the manual-cal branch of loop1() both call it.
void setSyncMode();

// PIO mutations requested from core 0 (serial/MIDI) and applied on core 1 before voice_task.
void pio_defer_request_sync_mode();
void pio_defer_request_cal_restore();
void pio_defer_request_reset_pulse_all();
void pio_defer_request_subosc(uint8_t divide);
void pio_defer_request_period_probe(uint8_t osc, uint32_t clk_div);
void pio_defer_service();

extern volatile bool pio_probe_report_pending;
void pio_probe_report_flush();

// The period model itself (PioPeriod, pio_period_split, pio_clk_div_for_y) and the PIO
// jump-target helpers live in globals.h, next to the state they read.

#endif
