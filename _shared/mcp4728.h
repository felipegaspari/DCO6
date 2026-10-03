#ifndef __MCP4728_H__
#define __MCP4728_H__

// MCP4728 I2C protocol shared by DCO4 Mainboard (STM32) and, later, DCO3 DCO
// (RP2040) behind ENABLE_MCP4728. Addresses, Fast Write encoding, attach,
// diagnostic probe / reattach. Live level writes are not gated by probe
// results — a failed 1 MHz ping used to mute analog for the whole run.
//
// Probe and reattach are blocking, on-demand operator tools: they run inline
// from the debug-command handler and nothing polls for them. What they must
// never do is start a transfer while the CV loop still has one in flight, so
// they quiesce the bus first with a real abort instead of forging the handle
// state — the forgery is what used to wedge the peripheral.
//
// Include from the board that owns the chips. Definitions live in
// mcp4728_impl.h (include once from the board shim, after the bus hooks).
//
// Board hooks the shim must define before including mcp4728_impl.h:
//   void mcp_i2c_bus_begin();           // pins, clock, optional DMA init
//   void mcp_i2c_recover();             // abort, PE cycle, re-link DMA
//   void mcp_i2c_abort();               // abort in-flight TX, bounded wait
//   bool mcp_i2c_idle();                // bus ready (real state, never forged)
//   bool mcp_i2c_tx(uint8_t addr7, const uint8_t* data, uint8_t n);
//   bool mcp_i2c_tx_blocking(uint8_t addr7, const uint8_t* data, uint8_t n);
//   void mcp_i2c_clear_error();         // error bits only; never State / Mode
//   uint32_t mcp_i2c_last_error();      // latched HAL error bits from last probe
//   void mcp_diag_print(const char* s); // USB Serial or slim 't' chunks
//   void mcp_after_reattach();          // rewrite live levels (mcpUpdate)

#ifdef ENABLE_MCP4728

#include <stdint.h>
#include "MCP4728_multiaddress.h"

static constexpr uint8_t MCP_CHIP_COUNT = 3;
static constexpr uint8_t MCP_ADDR7[3] = { 0x63, 0x64, 0x65 };
static constexpr uint8_t MCP_FAST_WRITE_BYTES = 8;
static constexpr uint32_t MCP_IDLE_WAIT_US = 2000;

extern MCP4728 mcp;
extern MCP4728 mcp2;
extern MCP4728 mcp3;

// Last diagnostic probe. Informational only — do not skip analogWrite.
extern bool mcp_present[3];

// DMA / IT TX buffer (32-byte aligned for STM32 D-cache clean).
extern uint8_t mcp_tx_buf[32];

void init_MCP4728();
void mcp_dac_probe();
void mcp_dac_reattach();
void mcp_i2c_wait_idle();
bool mcp_i2c_quiesce();
bool mcp_async_write(uint8_t addr7, uint16_t a, uint16_t b, uint16_t c, uint16_t d);

// Board hooks (defined in the sketch shim before including mcp4728_impl.h).
void mcp_i2c_bus_begin();
void mcp_i2c_recover();
void mcp_i2c_abort();
bool mcp_i2c_idle();
bool mcp_i2c_tx(uint8_t addr7, const uint8_t* data, uint8_t n);
bool mcp_i2c_tx_blocking(uint8_t addr7, const uint8_t* data, uint8_t n);
void mcp_i2c_clear_error();
uint32_t mcp_i2c_last_error();
void mcp_diag_print(const char* s);
void mcp_after_reattach();

#endif  // ENABLE_MCP4728

#endif
