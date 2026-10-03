#ifndef __MCP4728_IMPL_H__
#define __MCP4728_IMPL_H__

// Definitions for mcp4728.h. Include exactly once from the board shim
// (MAINBOARD-CONTROLLER/MCP4728.ino, later DCO/MCP4728.ino) after the bus
// hooks listed in mcp4728.h. Arduino's prototype generator cannot see through
// this include, so every cross-file function is declared in mcp4728.h.

#ifndef ENABLE_MCP4728
#error "mcp4728_impl.h requires ENABLE_MCP4728"
#endif

#include "mcp4728.h"
#include <stdio.h>
#include <string.h>
#include <Wire.h>

MCP4728 mcp;
MCP4728 mcp2;
MCP4728 mcp3;
bool mcp_present[3];
alignas(32) uint8_t mcp_tx_buf[32];

// MCP4728 Fast Write: (high nibble, low) × 4 channels, 12-bit values.
static void mcp_fill_tx(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
  const uint16_t ch[4] = { a, b, c, d };
  for (uint8_t i = 0; i < 4; i++) {
    const uint16_t v = (uint16_t)(ch[i] & 0x0FFFu);
    mcp_tx_buf[i * 2u]      = (uint8_t)(v >> 8);
    mcp_tx_buf[i * 2u + 1u] = (uint8_t)v;
  }
}

void mcp_i2c_wait_idle() {
  const uint32_t t0 = micros();
  while (!mcp_i2c_idle()) {
    if ((uint32_t)(micros() - t0) >= MCP_IDLE_WAIT_US) return;
  }
}

// Hand the bus over to a blocking transfer. The CV loop kicks an interrupt-
// driven Fast Write every ~120 us, so a probe arriving from the debug handler
// almost always lands on one in flight: wait it out, and abort it for real if
// it does not finish. Declaring the bus idle without that is what let a second
// transfer start on an armed peripheral and lock the board up.
bool mcp_i2c_quiesce() {
  mcp_i2c_wait_idle();
  if (mcp_i2c_idle()) return true;
  mcp_i2c_abort();
  return mcp_i2c_idle();
}

bool mcp_async_write(uint8_t addr7, uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
  if (!mcp_i2c_idle()) return false;
  mcp_fill_tx(a, b, c, d);
  return mcp_i2c_tx(addr7, mcp_tx_buf, MCP_FAST_WRITE_BYTES);
}

static void mcp_dac_attach_all() {
  mcp.attach(Wire, 255, MCP_ADDR7[0]);
  mcp2.attach(Wire, 255, MCP_ADDR7[1]);
  mcp3.attach(Wire, 255, MCP_ADDR7[2]);
}

// Presence = ACK of a full 8-byte Fast Write (not a truncated dummy byte, and
// not IsDeviceReady). A miss must not leave HAL in ERROR — the shim clears it.
static void mcp_dac_fill_present() {
  const bool bus = mcp_i2c_quiesce();
  mcp_i2c_clear_error();
  if (!bus) {
    // Say so rather than blaming the chips: nothing was sent, so every entry
    // below reads miss and the err field has nothing to report.
    mcp_diag_print("[mcp] bus never went idle, no ping sent\n");
  }
  mcp_fill_tx(4095, 4095, 4095, 4095);
  for (uint8_t i = 0; i < MCP_CHIP_COUNT; i++) {
    mcp_present[i] = bus &&
                     mcp_i2c_tx_blocking(MCP_ADDR7[i], mcp_tx_buf, MCP_FAST_WRITE_BYTES);
  }
}

static void mcp_dac_report(const char* tag) {
  char line[72];
  snprintf(line, sizeof(line), "[mcp] %s0x63=%s 0x64=%s 0x65=%s err=0x%lx\n",
           tag,
           mcp_present[0] ? "ok" : "miss",
           mcp_present[1] ? "ok" : "miss",
           mcp_present[2] ? "ok" : "miss",
           (unsigned long)mcp_i2c_last_error());
  mcp_diag_print(line);
}

void mcp_dac_probe() {
  mcp_dac_fill_present();
  mcp_after_reattach();
  mcp_dac_report("");
}

// Re-init the I2C peripheral, then attach + rewrite live levels. attach() only
// stores the 7-bit address; recover is what unsticks BUSY/ERROR.
void mcp_dac_reattach() {
  mcp_i2c_recover();
  mcp_dac_attach_all();
  mcp_after_reattach();
  mcp_dac_fill_present();
  mcp_after_reattach();
  mcp_dac_report("reattach ");
}

void init_MCP4728() {
  mcp_i2c_bus_begin();
  mcp_dac_attach_all();
  mcp.analogWrite(4095, 4095, 4095, 4095);
  mcp2.analogWrite(4095, 4095, 4095, 4095);
  mcp3.analogWrite(4095, 4095, 4095, 4095);
}

#endif
