#ifndef MCU_BOARD_H
#define MCU_BOARD_H

// MCU module GP23/24 helpers (Pico/Pico 2 SMPS PS, WeAct KEY + analog board-fix).
// Not included by the sketch. To enable: `#include "mcu_board.h"` and call
// mcu_board_pins_init() from setup() and user_key_task() from Core 0 loop().
// Pin constants live in globals.h (SMPS_PS_PIN / USER_KEY_PIN / BOARD_FIX_PIN).

static inline void mcu_board_pins_init() {
  if (SMPS_PS_PIN != MCU_PIN_UNASSIGNED) {
    pinMode(SMPS_PS_PIN, OUTPUT);
    digitalWrite(SMPS_PS_PIN, HIGH);  // Pico/Pico2: RT6150 PWM, less 3V3 ripple
  }
  if (BOARD_FIX_PIN != MCU_PIN_UNASSIGNED) {
    pinMode(BOARD_FIX_PIN, OUTPUT);
    digitalWrite(BOARD_FIX_PIN, HIGH);  // WeAct analog carrier rail
  }
  if (USER_KEY_PIN != MCU_PIN_UNASSIGNED) {
    pinMode(USER_KEY_PIN, INPUT_PULLUP);  // WeAct onboard KEY, active-low
  }
}

// WeAct KEY: hold plays MIDI 69 / A440. No-op on Pico (USER_KEY_PIN unassigned).
static inline void user_key_task() {
  if (USER_KEY_PIN == MCU_PIN_UNASSIGNED) return;
  static uint8_t stable = HIGH;
  static uint8_t raw_last = HIGH;
  static bool note_held = false;
  static uint32_t edge_ms = 0;

  uint8_t raw = (uint8_t)digitalRead(USER_KEY_PIN);
  uint32_t t = millis();
  if (raw != raw_last) {
    raw_last = raw;
    edge_ms = t;
  } else if ((uint32_t)(t - edge_ms) >= 20u && raw != stable) {
    stable = raw;
    if (!calibrationFlag) {
      if (stable == LOW) {
        note_on(69, 100);
        note_held = true;
      } else if (note_held) {
        note_off(69);
        note_held = false;
      }
    }
  }
  if (calibrationFlag && note_held) {
    note_off(69);
    note_held = false;
  }
}

#endif
