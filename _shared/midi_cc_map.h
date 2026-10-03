#ifndef __MIDI_CC_MAP_H__
#define __MIDI_CC_MAP_H__

// GENERATED FILE - do not edit.
// Emitted by DCO-CONTROL-PANEL/gen_midi_map.py from DCO-CONTROL-PANEL/params.py.
// Re-run that script after changing params.py; see docs/MIDI_CC_MAP.md for the chart.
//
// cc, target, lo, hi, curve. CC 0 lands on lo and CC 127 on hi; targets at or above
// CC_LOCAL_FIRST are block values that midi_cc_apply() writes directly.

#include <stddef.h>
#include "../params_def.h"
#include "midi_cc.h"

static const MidiCcEntry midiCcMap[] = {
  // --- Oscillators ---
  {   2, PARAM_OSC1_INTERVAL,               0,   127, MIDI_CC_LINEAR },
  {   3, PARAM_OSC2_INTERVAL,               0,    60, MIDI_CC_LINEAR },
  {   4, PARAM_OSC3_INTERVAL,               0,    60, MIDI_CC_LINEAR },
  {   5, PARAM_OSC2_DETUNE_VAL,             0,   512, MIDI_CC_LINEAR },
  {   8, PARAM_OSC3_DETUNE_VAL,             0,   512, MIDI_CC_LINEAR },
  {  20, PARAM_SYNC_MODE,                   0,   127, MIDI_CC_LINEAR },
  {  21, PARAM_SOFT_SYNC,                   0,   127, MIDI_CC_LINEAR },
  {  22, PARAM_SUBOSC_DIVIDE,               0,   127, MIDI_CC_LINEAR },
  {  23, PARAM_OSC_SYNC_MODE,               0,   127, MIDI_CC_LINEAR },
  {  69, PARAM_VOICE_MODE,                  0,   127, MIDI_CC_LINEAR },
  {  78, PARAM_VOICE_ALLOC_MODE,            0,   127, MIDI_CC_LINEAR },
  {  70, PARAM_UNISON_DETUNE,               0,   127, MIDI_CC_LINEAR },
  {  71, PARAM_PORTAMENTO_TIME,             0,   255, MIDI_CC_LINEAR },
  {  72, PARAM_PORTAMENTO_MODE,             0,   127, MIDI_CC_LINEAR },
  {  73, PARAM_ANALOG_DRIFT_AMOUNT,         0,   127, MIDI_CC_LINEAR },
  {  74, PARAM_ANALOG_DRIFT_SPEED,          1,   255, MIDI_CC_LINEAR },
  {  75, PARAM_ANALOG_DRIFT_SPREAD,         1,   127, MIDI_CC_LINEAR },
  {  76, PARAM_VCA_LEVEL,                   0,   128, MIDI_CC_LINEAR },
  {  77, PARAM_VELOCITY_TO_VCA,             0,    20, MIDI_CC_LINEAR },
  {   9, PARAM_OSC1_LEVEL,                  0,   127, MIDI_CC_LINEAR },
  {  12, PARAM_OSC2_LEVEL,                  0,   127, MIDI_CC_LINEAR },
  {  83, PARAM_OSC3_LEVEL,                  0,   127, MIDI_CC_LINEAR },
  {  13, PARAM_SUB_LEVEL,                   0,   127, MIDI_CC_LINEAR },
  {  16, PARAM_OSC1_SAW_ENABLE,             0,     1, MIDI_CC_LINEAR },
  {  17, PARAM_OSC1_PULSE_ENABLE,           0,     1, MIDI_CC_LINEAR },
  {  18, PARAM_OSC1_TRI_ENABLE,             0,     1, MIDI_CC_LINEAR },
  { 112, PARAM_OSC2_SAW_ENABLE,             0,     1, MIDI_CC_LINEAR },
  { 113, PARAM_OSC2_PULSE_ENABLE,           0,     1, MIDI_CC_LINEAR },
  { 114, PARAM_OSC2_TRI_ENABLE,             0,     1, MIDI_CC_LINEAR },
  { 115, PARAM_OSC3_SAW_ENABLE,             0,     1, MIDI_CC_LINEAR },
  { 116, PARAM_OSC3_PULSE_ENABLE,           0,     1, MIDI_CC_LINEAR },
  { 117, PARAM_OSC3_TRI_ENABLE,             0,     1, MIDI_CC_LINEAR },
  // --- Envelopes ---
  {  48, PARAM_ADSR1_TO_VCA,                0,   512, MIDI_CC_LINEAR },
  {  24, PARAM_ADSR3_ENABLED,               0,     1, MIDI_CC_LINEAR },
  {  25, PARAM_ADSR3_TO_OSC_SELECT,         0,   127, MIDI_CC_LINEAR },
  {  26, PARAM_ADSR3_TO_DETUNE1,         -511,   511, MIDI_CC_LINEAR },
  {  27, PARAM_ADSR1_ATTACK_CURVE,          0,   127, MIDI_CC_LINEAR },
  {  28, PARAM_ADSR1_DECAY_CURVE,           0,   127, MIDI_CC_LINEAR },
  {  29, PARAM_ADSR2_ATTACK_CURVE,          0,   127, MIDI_CC_LINEAR },
  {  30, PARAM_ADSR2_DECAY_CURVE,           0,   127, MIDI_CC_LINEAR },
  {  31, PARAM_VCA_ADSR_RESTART,            0,     1, MIDI_CC_LINEAR },
  {  33, PARAM_VCF_ADSR_RESTART,            0,     1, MIDI_CC_LINEAR },
  {  34, CC_LOCAL_ADSR_VCA_ATTACK,          0,  4095, MIDI_CC_EXP_TIME },
  {  35, CC_LOCAL_ADSR_VCA_DECAY,           0,  4095, MIDI_CC_EXP_TIME },
  {  36, CC_LOCAL_ADSR_VCA_SUSTAIN,         0,  4095, MIDI_CC_LINEAR },
  {  37, CC_LOCAL_ADSR_VCA_RELEASE,         0,  4095, MIDI_CC_EXP_TIME },
  {  39, CC_LOCAL_ADSR_VCF_ATTACK,          0,  4095, MIDI_CC_EXP_TIME },
  {  40, CC_LOCAL_ADSR_VCF_DECAY,           0,  4095, MIDI_CC_EXP_TIME },
  {  41, CC_LOCAL_ADSR_VCF_SUSTAIN,         0,  4095, MIDI_CC_LINEAR },
  {  43, CC_LOCAL_ADSR_VCF_RELEASE,         0,  4095, MIDI_CC_EXP_TIME },
  {  44, CC_LOCAL_ADSR_DCO_ATTACK,          0,  4095, MIDI_CC_EXP_TIME },
  {  45, CC_LOCAL_ADSR_DCO_DECAY,           0,  4095, MIDI_CC_EXP_TIME },
  {  46, CC_LOCAL_ADSR_DCO_SUSTAIN,         0,  4095, MIDI_CC_LINEAR },
  {  47, CC_LOCAL_ADSR_DCO_RELEASE,         0,  4095, MIDI_CC_EXP_TIME },
  // --- Filter ---
  {  49, PARAM_VCF_KEYTRACK,             -256,   255, MIDI_CC_LINEAR },
  {  50, PARAM_VELOCITY_TO_VCF,             0,    20, MIDI_CC_LINEAR },
  {  51, PARAM_RESONANCE_COMPENSATION,      0,     1, MIDI_CC_LINEAR },
  { 118, PARAM_FILTER_MODE,                 0,   127, MIDI_CC_LINEAR },
  {  81, PARAM_DIST_DRIVE,                  0,  4095, MIDI_CC_LINEAR },
  {  82, PARAM_DIST_MIX,                    0,  4095, MIDI_CC_LINEAR },
  {  52, CC_LOCAL_FILTER_CUTOFF,            0,  4095, MIDI_CC_LINEAR },
  {  53, CC_LOCAL_FILTER_RESONANCE,         0,  4095, MIDI_CC_LINEAR },
  {  54, CC_LOCAL_FILTER_ADSR2_TO_VCF,      0,   512, MIDI_CC_LINEAR },
  {  55, CC_LOCAL_FILTER_LFO2_TO_VCF,       0,   512, MIDI_CC_LINEAR },
  // --- PWM ---
  {  59, PARAM_PW_VALUE,                    0,  4095, MIDI_CC_LINEAR },
  {  56, PARAM_LFO2_TO_PW,                  0,   511, MIDI_CC_LINEAR },
  {  57, PARAM_ADSR3_TO_PWM,                0,  1023, MIDI_CC_LINEAR },
  {  58, PARAM_PWM_POTS_CONTROL_MANUAL,     0,     1, MIDI_CC_LINEAR },
  // --- LFOs ---
  {  60, PARAM_LFO1_WAVEFORM,               0,   127, MIDI_CC_LINEAR },
  {  61, PARAM_LFO2_WAVEFORM,               0,   127, MIDI_CC_LINEAR },
  {  62, PARAM_LFO1_SPEED,                  0,  4095, MIDI_CC_LINEAR },
  {  63, PARAM_LFO2_SPEED,                  0,  4095, MIDI_CC_LINEAR },
  {  65, PARAM_LFO1_TO_DCO,                 0,   511, MIDI_CC_LINEAR },
  {  14, PARAM_LFO1_TO_OSC1,                0,   255, MIDI_CC_LINEAR },
  {  15, PARAM_LFO1_TO_OSC2,                0,   255, MIDI_CC_LINEAR },
  {  19, PARAM_LFO1_TO_OSC3,                0,   255, MIDI_CC_LINEAR },
  {  66, PARAM_LFO1_TO_VCA,                 0,  1023, MIDI_CC_LINEAR },
  {  67, PARAM_LFO2_TO_OSC2,                0,   255, MIDI_CC_LINEAR },
  {  68, PARAM_LFO2_TO_OSC3,                0,   255, MIDI_CC_LINEAR },
  { 119, PARAM_LFO2_TO_OSC2_COARSE,         0,   511, MIDI_CC_LINEAR },
  // --- Mod matrix ---
  {  84, PARAM_MOD_SLOT0_SOURCE,            0,   127, MIDI_CC_LINEAR },
  {  85, PARAM_MOD_SLOT0_DEST,              0,   127, MIDI_CC_LINEAR },
  {  86, PARAM_MOD_SLOT0_DEPTH,         -4095,  4095, MIDI_CC_LINEAR },
  {  87, PARAM_MOD_SLOT1_SOURCE,            0,   127, MIDI_CC_LINEAR },
  {  88, PARAM_MOD_SLOT1_DEST,              0,   127, MIDI_CC_LINEAR },
  {  89, PARAM_MOD_SLOT1_DEPTH,         -4095,  4095, MIDI_CC_LINEAR },
  {  90, PARAM_MOD_SLOT2_SOURCE,            0,   127, MIDI_CC_LINEAR },
  {  91, PARAM_MOD_SLOT2_DEST,              0,   127, MIDI_CC_LINEAR },
  {  92, PARAM_MOD_SLOT2_DEPTH,         -4095,  4095, MIDI_CC_LINEAR },
  {  93, PARAM_MOD_SLOT3_SOURCE,            0,   127, MIDI_CC_LINEAR },
  {  94, PARAM_MOD_SLOT3_DEST,              0,   127, MIDI_CC_LINEAR },
  {  95, PARAM_MOD_SLOT3_DEPTH,         -4095,  4095, MIDI_CC_LINEAR },
  {  96, PARAM_MOD_SLOT4_SOURCE,            0,   127, MIDI_CC_LINEAR },
  {  97, PARAM_MOD_SLOT4_DEST,              0,   127, MIDI_CC_LINEAR },
  { 102, PARAM_MOD_SLOT4_DEPTH,         -4095,  4095, MIDI_CC_LINEAR },
  { 103, PARAM_MOD_SLOT5_SOURCE,            0,   127, MIDI_CC_LINEAR },
  { 104, PARAM_MOD_SLOT5_DEST,              0,   127, MIDI_CC_LINEAR },
  { 105, PARAM_MOD_SLOT5_DEPTH,         -4095,  4095, MIDI_CC_LINEAR },
  { 106, PARAM_MOD_SLOT6_SOURCE,            0,   127, MIDI_CC_LINEAR },
  { 107, PARAM_MOD_SLOT6_DEST,              0,   127, MIDI_CC_LINEAR },
  { 108, PARAM_MOD_SLOT6_DEPTH,         -4095,  4095, MIDI_CC_LINEAR },
  { 109, PARAM_MOD_SLOT7_SOURCE,            0,   127, MIDI_CC_LINEAR },
  { 110, PARAM_MOD_SLOT7_DEST,              0,   127, MIDI_CC_LINEAR },
  { 111, PARAM_MOD_SLOT7_DEPTH,         -4095,  4095, MIDI_CC_LINEAR },
};

static const size_t midiCcMapSize = sizeof(midiCcMap) / sizeof(midiCcMap[0]);

#endif
