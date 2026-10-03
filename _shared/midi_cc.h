#ifndef __MIDI_CC_H__
#define __MIDI_CC_H__

#include <stdint.h>
#include <stddef.h>

// 7-bit MIDI CC control surface.
//
// Every control the bench app exposes is reachable from a CC, so a generic panel app
// (Open Stage Control, a DAW's MIDI rack, a hardware controller) can drive the board
// with no Input board attached. Receive only: nothing is echoed back, so a panel pushes
// its state with a "send all" after connecting.
//
// The table lives in the generated midi_cc_map.h, which DCO.ino includes right after this
// file because it defines the array. That header, the chart in docs/MIDI_CC_MAP.md and the
// panel session in tools/panels all come out of tools/dco_control/params.py via
// gen_midi_map.py, so none of them can drift apart: edit params.py and re-run it.
//
// The two functions below are implemented in midi.ino.

// CC 42 keeps its historical meaning (pitch-bend range in semitones) and is left out of
// the map, along with the other reserved controllers.
#define MIDI_CC_PITCH_BEND_RANGE 42

// Curve applied on the way from a CC to the native value.
enum : uint8_t {
  MIDI_CC_LINEAR = 0,
  // Envelope attack/decay/release. The 'a'–'c' block frames carry these already exp-mapped,
  // because on hardware the Input board applies the curve before transmitting, so a CC
  // has to do the same to feel like the fader.
  MIDI_CC_EXP_TIME = 1,
};

// Envelope time curve, matching linearToExponential(v, 50, 25000) on the Input board.
#define MIDI_CC_EXP_BASE 50.0f
#define MIDI_CC_EXP_MAX 25000

// Targets for the values that reach the DCO as 1 ms block frames ('a'–'d') rather than
// as 'p' parameter frames, and so have no ParamId. Numbered above every ParamId in
// params_def.h (wire id is uint8; 222 is the highest today) so one uint8_t target can
// mean either kind and the dispatch stays a single switch.
//
// Names are what gen_midi_map.py derives from the Block and BlockField keys in
// params.py, as CC_LOCAL_<block>_<field> uppercased; the generator checks that each one
// it emits is both declared here and handled in midi_cc_apply().
enum : uint8_t {
  CC_LOCAL_FIRST = 224,

  CC_LOCAL_ADSR_VCA_ATTACK = CC_LOCAL_FIRST,
  CC_LOCAL_ADSR_VCA_DECAY,
  CC_LOCAL_ADSR_VCA_SUSTAIN,
  CC_LOCAL_ADSR_VCA_RELEASE,

  CC_LOCAL_ADSR_VCF_ATTACK,
  CC_LOCAL_ADSR_VCF_DECAY,
  CC_LOCAL_ADSR_VCF_SUSTAIN,
  CC_LOCAL_ADSR_VCF_RELEASE,

  CC_LOCAL_ADSR_DCO_ATTACK,
  CC_LOCAL_ADSR_DCO_DECAY,
  CC_LOCAL_ADSR_DCO_SUSTAIN,
  CC_LOCAL_ADSR_DCO_RELEASE,

  CC_LOCAL_FILTER_CUTOFF,
  CC_LOCAL_FILTER_RESONANCE,
  CC_LOCAL_FILTER_ADSR2_TO_VCF,
  CC_LOCAL_FILTER_LFO2_TO_VCF,
};

struct MidiCcEntry {
  uint8_t cc;      // controller number
  uint8_t target;  // a ParamId, or one of the CC_LOCAL_* codes above
  int16_t lo;      // CC 0 lands here
  int16_t hi;      // CC 127 lands here
  uint8_t curve;   // MIDI_CC_LINEAR or MIDI_CC_EXP_TIME
};

void midi_cc_handle(uint8_t number, uint8_t value);
void midi_cc_apply(uint8_t target, int16_t value);

#endif
