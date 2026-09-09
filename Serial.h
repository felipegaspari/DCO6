#ifndef __SERIAL_H__
#define __SERIAL_H__


// Shared Protocol Includes
#include "_build_libs/DCO-PROTOCOL/serial_param_protocol.h"
#include "_build_libs/DCO-PROTOCOL/serial_input_protocol.h"
#include "_build_libs/DCO-PROTOCOL/serial_frame.h"
#include "_build_libs/DCO-PROTOCOL/serial_parser.h"

// NEW: Shared DMA Library replaces serial2_dma.h
#include "_build_libs/DCO-PROTOCOL/serial_dma_tx.h"


extern UartDmaTx Serial2Dma;

void init_serial();
void init_usb();
void serial_panel_task();

#ifdef ENABLE_USB_CONTROL
void SRAM_HOT(serial_usb_task)();
#endif

void SRAM_HOT(serialSendParam32)(byte paramNumber, uint32_t paramValue, bool force = false);
void SRAM_HOT(serialSendParam16)(byte paramNumber, int16_t paramValue, bool force = false);
void SRAM_HOT(serial_echo_usb_param16)(uint8_t id, int16_t value);

void SRAM_HOT(serial_send_adsr_vca_block_to_mb)();
void SRAM_HOT(serial_send_adsr_vcf_block_to_mb)();
void SRAM_HOT(serial_send_adsr_dco_block_to_mb)();
void SRAM_HOT(serial_send_filter_block_to_mb)();

void SRAM_HOT(serial_send_preset_loaded_to_mb)(uint8_t slot);
void SRAM_HOT(serial_send_preset_scroll_to_mb)(uint8_t slot);
void SRAM_HOT(serial_send_screen_signal_to_mb)(uint8_t signal);

void SRAM_HOT(serial_send_note_on)(uint8_t voice, uint8_t velocity, uint8_t note, uint8_t flags);
void SRAM_HOT(serial_send_note_off)(uint8_t voice);
void SRAM_HOT(serial_send_expression)();

void SRAM_HOT(serial_send_patch_osc_block_to_mb)();
void SRAM_HOT(serial_send_patch_lfo_block_to_mb)();
void SRAM_HOT(serial_send_patch_mod_block_to_mb)();
void SRAM_HOT(serial_send_patch_mix_block_to_mb)();

void SRAM_HOT(serial_send_preset_burst_to_mb)(); 

#endif
