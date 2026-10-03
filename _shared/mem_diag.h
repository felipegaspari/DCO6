#ifndef __MEM_DIAG_H__
#define __MEM_DIAG_H__

// SRAM / heap / stack snapshot for Diagnostics (PARAM_DEBUG_COMMAND 13).
// ENABLE_MEM_DIAG (DCO.ino, default on): polls compile in. Comment out for a
// zero-cost match to pre-mem_diag period-only benches. Runtime 14/15 disable
// polls without rebuild (dump 13 ignored while off). Core 1 never prints.

#ifdef ENABLE_MEM_DIAG

#include <stdbool.h>

extern volatile bool mem_diag_runtime_enabled;
extern volatile bool mem_diag_pending;
extern volatile bool mem_diag_core1_ready;

void mem_diag_request();
void mem_diag_poll_core0_work();
void mem_diag_poll_core1_work();

// Idle: one volatile load (+ pending check). Work stays in mem_diag.ino.
static inline void mem_diag_poll_core1() {
  if (!mem_diag_runtime_enabled) {
    return;
  }
  if (!mem_diag_pending || mem_diag_core1_ready) {
    return;
  }
  mem_diag_poll_core1_work();
}

static inline void mem_diag_poll_core0() {
  if (!mem_diag_runtime_enabled) {
    return;
  }
  if (!mem_diag_pending || !mem_diag_core1_ready) {
    return;
  }
  mem_diag_poll_core0_work();
}

#else  // !ENABLE_MEM_DIAG

static inline void mem_diag_request() {}
static inline void mem_diag_poll_core0() {}
static inline void mem_diag_poll_core1() {}

#endif

#endif
