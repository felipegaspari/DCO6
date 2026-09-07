# MIDI CC implementation chart

Generated from `DCO-CONTROL-PANEL/params.py` by `DCO-CONTROL-PANEL/gen_midi_map.py`. Do not edit by hand.

Every control the bench app exposes is reachable from a 7-bit CC on any channel (the DCO listens omni), over USB MIDI or the DIN input. The board does not send anything back, so a panel should push its state after connecting.

A controller value scales into the parameter's native range as

```
value = lo + ((hi - lo) * cc + 63) / 127
```

so CC 0 lands on `CC 0` below and CC 127 on `CC 127`. Envelope attack, decay and release then go through `linearToExponential(value, 50, 25000)`, the same curve the Input board applies to its faders, because the `'a'`-`'c'` block frames carry those values already exp-mapped.

## Map

| CC | Control | Group | Target | CC 0 | CC 127 | Curve |
| --- | --- | --- | --- | --- | --- | --- |
| 2 | Octave shift | Oscillators | `PARAM_OSC1_INTERVAL` | 0 | 127 | linear |
| 3 | OSC B interval (semitones) | Oscillators | `PARAM_OSC2_INTERVAL` | 0 | 60 | linear |
| 4 | OSC3 interval (semitones) | Oscillators | `PARAM_OSC3_INTERVAL` | 0 | 60 | linear |
| 5 | OSC B detune | Oscillators | `PARAM_OSC2_DETUNE_VAL` | 0 | 512 | linear |
| 8 | OSC3 detune | Oscillators | `PARAM_OSC3_DETUNE_VAL` | 0 | 512 | linear |
| 20 | Hard sync topology | Oscillators | `PARAM_SYNC_MODE` | 0 | 127 | linear |
| 21 | Soft sync | Oscillators | `PARAM_SOFT_SYNC` | 0 | 127 | linear |
| 22 | Sub-oscillator divide | Oscillators | `PARAM_SUBOSC_DIVIDE` | 0 | 127 | linear |
| 23 | Osc sync / phase align OSC B | Oscillators | `PARAM_OSC_PHASE_SYNC` | 0 | 127 | linear |
| 69 | Voice mode | Oscillators | `PARAM_VOICE_MODE` | 0 | 127 | linear |
| 78 | Voice alloc / note priority | Oscillators | `PARAM_VOICE_ALLOC_MODE` | 0 | 127 | linear |
| 70 | Unison detune | Oscillators | `PARAM_UNISON_DETUNE` | 0 | 127 | linear |
| 71 | Portamento time | Oscillators | `PARAM_PORTAMENTO_TIME` | 0 | 255 | linear |
| 72 | Portamento mode | Oscillators | `PARAM_PORTAMENTO_MODE` | 0 | 127 | linear |
| 73 | Analog drift amount | Oscillators | `PARAM_ANALOG_DRIFT_AMOUNT` | 0 | 127 | linear |
| 74 | Analog drift speed | Oscillators | `PARAM_ANALOG_DRIFT_SPEED` | 1 | 255 | linear |
| 75 | Analog drift spread | Oscillators | `PARAM_ANALOG_DRIFT_SPREAD` | 1 | 127 | linear |
| 76 | VCA level | Oscillators | `PARAM_VCA_LEVEL` | 0 | 128 | linear |
| 77 | Velocity to VCA | Oscillators | `PARAM_VELOCITY_TO_VCA` | 0 | 20 | linear |
| 9 | OSC A level | Oscillators | `PARAM_OSC1_LEVEL` | 0 | 127 | linear |
| 12 | OSC B level | Oscillators | `PARAM_OSC2_LEVEL` | 0 | 127 | linear |
| 83 | OSC3 level | Oscillators | `PARAM_OSC3_LEVEL` | 0 | 127 | linear |
| 13 | Sub level | Oscillators | `PARAM_SUB_LEVEL` | 0 | 127 | linear |
| 16 | OSC A Saw enable | Oscillators | `PARAM_OSC1_SAW_ENABLE` | 0 | 1 | linear |
| 17 | OSC A Pulse enable | Oscillators | `PARAM_OSC1_PULSE_ENABLE` | 0 | 1 | linear |
| 18 | OSC A Tri enable | Oscillators | `PARAM_OSC1_TRI_ENABLE` | 0 | 1 | linear |
| 112 | OSC B Saw enable | Oscillators | `PARAM_OSC2_SAW_ENABLE` | 0 | 1 | linear |
| 113 | OSC B Pulse enable | Oscillators | `PARAM_OSC2_PULSE_ENABLE` | 0 | 1 | linear |
| 115 | OSC3 Saw enable | Oscillators | `PARAM_OSC3_SAW_ENABLE` | 0 | 1 | linear |
| 116 | OSC3 Pulse enable | Oscillators | `PARAM_OSC3_PULSE_ENABLE` | 0 | 1 | linear |
| 117 | OSC3 Tri enable | Oscillators | `PARAM_OSC3_TRI_ENABLE` | 0 | 1 | linear |
| 48 | ADSR1 to VCA | Envelopes | `PARAM_ADSR1_TO_VCA` | 0 | 512 | linear |
| 24 | EnvDCO (ADSR3) enabled | Envelopes | `PARAM_ADSR3_ENABLED` | 0 | 1 | linear |
| 25 | ADSR3 to osc select | Envelopes | `PARAM_ADSR3_TO_OSC_SELECT` | 0 | 127 | linear |
| 26 | ADSR3 to OSC A detune | Envelopes | `PARAM_ADSR3_TO_DETUNE1` | -511 | 511 | linear |
| 27 | ADSR1 attack curve | Envelopes | `PARAM_ADSR1_ATTACK_CURVE` | 0 | 127 | linear |
| 28 | ADSR1 decay curve | Envelopes | `PARAM_ADSR1_DECAY_CURVE` | 0 | 127 | linear |
| 29 | ADSR2 attack curve | Envelopes | `PARAM_ADSR2_ATTACK_CURVE` | 0 | 127 | linear |
| 30 | ADSR2 decay curve | Envelopes | `PARAM_ADSR2_DECAY_CURVE` | 0 | 127 | linear |
| 31 | VCA ADSR restart | Envelopes | `PARAM_ADSR1_RESTART` | 0 | 1 | linear |
| 33 | VCF ADSR restart | Envelopes | `PARAM_ADSR2_RESTART` | 0 | 1 | linear |
| 34 | EnvVCA times: Attack | Envelopes | `CC_LOCAL_ADSR_VCA_ATTACK` | 0 | 25000 | exp |
| 35 | EnvVCA times: Decay | Envelopes | `CC_LOCAL_ADSR_VCA_DECAY` | 0 | 25000 | exp |
| 36 | EnvVCA times: Sustain | Envelopes | `CC_LOCAL_ADSR_VCA_SUSTAIN` | 0 | 4095 | linear |
| 37 | EnvVCA times: Release | Envelopes | `CC_LOCAL_ADSR_VCA_RELEASE` | 0 | 25000 | exp |
| 39 | EnvVCF times: Attack | Envelopes | `CC_LOCAL_ADSR_VCF_ATTACK` | 0 | 25000 | exp |
| 40 | EnvVCF times: Decay | Envelopes | `CC_LOCAL_ADSR_VCF_DECAY` | 0 | 25000 | exp |
| 41 | EnvVCF times: Sustain | Envelopes | `CC_LOCAL_ADSR_VCF_SUSTAIN` | 0 | 4095 | linear |
| 43 | EnvVCF times: Release | Envelopes | `CC_LOCAL_ADSR_VCF_RELEASE` | 0 | 25000 | exp |
| 44 | EnvDCO times (pitch and PW): Attack | Envelopes | `CC_LOCAL_ADSR_DCO_ATTACK` | 0 | 25000 | exp |
| 45 | EnvDCO times (pitch and PW): Decay | Envelopes | `CC_LOCAL_ADSR_DCO_DECAY` | 0 | 25000 | exp |
| 46 | EnvDCO times (pitch and PW): Sustain | Envelopes | `CC_LOCAL_ADSR_DCO_SUSTAIN` | 0 | 4095 | linear |
| 47 | EnvDCO times (pitch and PW): Release | Envelopes | `CC_LOCAL_ADSR_DCO_RELEASE` | 0 | 25000 | exp |
| 49 | VCF keytrack | Filter | `PARAM_VCF_KEYTRACK` | -256 | 255 | linear |
| 50 | Velocity to VCF | Filter | `PARAM_VELOCITY_TO_VCF` | 0 | 20 | linear |
| 51 | Resonance amp compensation | Filter | `PARAM_RESONANCE_COMPENSATION` | 0 | 1 | linear |
| 118 | Filter mode | Filter | `PARAM_FILTER_MODE` | 0 | 127 | linear |
| 81 | Distortion drive | Filter | `PARAM_DIST_DRIVE` | 0 | 4095 | linear |
| 82 | Distortion mix | Filter | `PARAM_DIST_MIX` | 0 | 4095 | linear |
| 52 | Filter block: Cutoff | Filter | `CC_LOCAL_FILTER_CUTOFF` | 0 | 4095 | linear |
| 53 | Filter block: Resonance | Filter | `CC_LOCAL_FILTER_RESONANCE` | 0 | 4095 | linear |
| 54 | Filter block: EnvVCF to cutoff | Filter | `CC_LOCAL_FILTER_ADSR2_TO_VCF` | 0 | 512 | linear |
| 55 | Filter block: LFO2 to cutoff | Filter | `CC_LOCAL_FILTER_LFO2_TO_VCF` | 0 | 512 | linear |
| 59 | Pulse width | PWM | `PARAM_PW_VALUE` | 0 | 4095 | linear |
| 56 | LFO2 to PW | PWM | `PARAM_LFO2_TO_PW` | 0 | 511 | linear |
| 57 | ADSR3 to PWM | PWM | `PARAM_ADSR3_TO_PWM` | 0 | 1023 | linear |
| 60 | LFO1 waveform | LFOs | `PARAM_LFO1_WAVEFORM` | 0 | 127 | linear |
| 61 | LFO2 waveform | LFOs | `PARAM_LFO2_WAVEFORM` | 0 | 127 | linear |
| 62 | LFO1 speed | LFOs | `PARAM_LFO1_SPEED` | 0 | 4095 | linear |
| 63 | LFO2 speed | LFOs | `PARAM_LFO2_SPEED` | 0 | 4095 | linear |
| 65 | LFO1 to DCO | LFOs | `PARAM_LFO1_TO_DCO` | 0 | 511 | linear |
| 14 | LFO1 to OSC A extra | LFOs | `PARAM_LFO1_TO_OSC1` | 0 | 255 | linear |
| 15 | LFO1 to OSC B extra | LFOs | `PARAM_LFO1_TO_OSC2` | 0 | 255 | linear |
| 19 | LFO1 to OSC3 extra | LFOs | `PARAM_LFO1_TO_OSC3` | 0 | 255 | linear |
| 66 | LFO1 to VCA | LFOs | `PARAM_LFO1_TO_VCA` | 0 | 1023 | linear |
| 67 | LFO2 to OSC B detune | LFOs | `PARAM_LFO2_TO_OSC2` | 0 | 255 | linear |
| 68 | LFO2 to OSC3 detune | LFOs | `PARAM_LFO2_TO_OSC3` | 0 | 255 | linear |
| 119 | LFO2 to OSC B coarse | LFOs | `PARAM_LFO2_TO_OSC2_COARSE` | 0 | 511 | linear |
| 84 | Mod slot 0 source | Mod matrix | `PARAM_MOD_SLOT0_SOURCE` | 0 | 127 | linear |
| 85 | Mod slot 0 dest | Mod matrix | `PARAM_MOD_SLOT0_DEST` | 0 | 127 | linear |
| 86 | Mod slot 0 depth | Mod matrix | `PARAM_MOD_SLOT0_DEPTH` | -4095 | 4095 | linear |
| 87 | Mod slot 1 source | Mod matrix | `PARAM_MOD_SLOT1_SOURCE` | 0 | 127 | linear |
| 88 | Mod slot 1 dest | Mod matrix | `PARAM_MOD_SLOT1_DEST` | 0 | 127 | linear |
| 89 | Mod slot 1 depth | Mod matrix | `PARAM_MOD_SLOT1_DEPTH` | -4095 | 4095 | linear |
| 90 | Mod slot 2 source | Mod matrix | `PARAM_MOD_SLOT2_SOURCE` | 0 | 127 | linear |
| 91 | Mod slot 2 dest | Mod matrix | `PARAM_MOD_SLOT2_DEST` | 0 | 127 | linear |
| 92 | Mod slot 2 depth | Mod matrix | `PARAM_MOD_SLOT2_DEPTH` | -4095 | 4095 | linear |
| 93 | Mod slot 3 source | Mod matrix | `PARAM_MOD_SLOT3_SOURCE` | 0 | 127 | linear |
| 94 | Mod slot 3 dest | Mod matrix | `PARAM_MOD_SLOT3_DEST` | 0 | 127 | linear |
| 95 | Mod slot 3 depth | Mod matrix | `PARAM_MOD_SLOT3_DEPTH` | -4095 | 4095 | linear |
| 96 | Mod slot 4 source | Mod matrix | `PARAM_MOD_SLOT4_SOURCE` | 0 | 127 | linear |
| 97 | Mod slot 4 dest | Mod matrix | `PARAM_MOD_SLOT4_DEST` | 0 | 127 | linear |
| 102 | Mod slot 4 depth | Mod matrix | `PARAM_MOD_SLOT4_DEPTH` | -4095 | 4095 | linear |
| 103 | Mod slot 5 source | Mod matrix | `PARAM_MOD_SLOT5_SOURCE` | 0 | 127 | linear |
| 104 | Mod slot 5 dest | Mod matrix | `PARAM_MOD_SLOT5_DEST` | 0 | 127 | linear |
| 105 | Mod slot 5 depth | Mod matrix | `PARAM_MOD_SLOT5_DEPTH` | -4095 | 4095 | linear |
| 106 | Mod slot 6 source | Mod matrix | `PARAM_MOD_SLOT6_SOURCE` | 0 | 127 | linear |
| 107 | Mod slot 6 dest | Mod matrix | `PARAM_MOD_SLOT6_DEST` | 0 | 127 | linear |
| 108 | Mod slot 6 depth | Mod matrix | `PARAM_MOD_SLOT6_DEPTH` | -4095 | 4095 | linear |
| 109 | Mod slot 7 source | Mod matrix | `PARAM_MOD_SLOT7_SOURCE` | 0 | 127 | linear |
| 110 | Mod slot 7 dest | Mod matrix | `PARAM_MOD_SLOT7_DEST` | 0 | 127 | linear |
| 111 | Mod slot 7 depth | Mod matrix | `PARAM_MOD_SLOT7_DEPTH` | -4095 | 4095 | linear |

## Menu values

These parameters take discrete values; the CC number to send is the value itself.

- **CC 2, Octave shift**: -3 = 0, -2 = 12, -1 = 24, +0 = 36, +1 = 48, +2 = 60, +3 = 72
- **CC 20, Hard sync topology**: 0 - all free running = 0, 1 - OSC B masters OSC A = 1, 2 - OSC A masters OSC B = 2
  - which oscillator's sideset drives which reset pin; not the note-on phase reset, which is 'Osc sync / phase align OSC B' below
- **CC 21, Soft sync**: 0 - hard sync (cap only) = 0, 1 - soft ~40% window = 1, 2 - soft ~67% window = 2, 3 - soft ~86% window = 3
  - 0 = hard sync (sideset); 1..3 = soft sync trailing polled chunks
- **CC 22, Sub-oscillator divide**: Off = 0, Divide by 2 = 2, Divide by 4 = 4
  - output on GP8, needs a mixer input on the carrier to be audible
- **CC 23, Osc sync / phase align OSC B**: Off - free running (no note-on sync) = 0, Sync at note-on (0 deg) = 1, Sync + 30 deg = 15, Sync + 45 deg = 2, Sync + 60 deg = 30, Sync + 90 deg = 3, Sync + 120 deg = 60, Sync + 135 deg = 4, Sync + 150 deg = 75, Sync + 180 deg = 5, Sync + 210 deg = 105, Sync + 225 deg = 6, Sync + 240 deg = 120, Sync + 270 deg = 7, Sync + 315 deg = 8
  - out of 7-bit reach, use the serial bench app instead: Sync + 300 deg (150), Sync + 330 deg (165)
- **CC 69, Voice mode**: 0 - MONO = 0, 1 - POLY = 1, 2 - UNISON = 2
- **CC 78, Voice alloc / note priority**: 0 - ROUND ROBIN = 0, 1 - OLDEST = 1, 2 - QUIETEST = 2, 3 - QUIETEST LOW = 3, 4 - QUIETEST HIGH = 4, 5 - NO STEAL = 5, 6 - FIRST FREE = 6, 7 - LOWEST PITCH = 7, 8 - HIGHEST PITCH = 8
- **CC 72, Portamento mode**: 0 - fixed time = 0, 1 - slew rate = 1
- **CC 25, ADSR3 to osc select**: 0 - OSC A = 0, 1 - OSC B = 1, 2 - OSC A+B = 2, 3 - OSC3 = 3, 4 - all = 4
- **CC 27, ADSR1 attack curve**: 0 - EXP = 0, 1 - SOFT = 1, 2 - STEEP = 2, 3 - CONCAVE = 3, 4 - FAST S = 4, 5 - SLOW THEN LIN = 5, 6 - ALMOST LIN = 6, 7 - LINEAR = 7
- **CC 28, ADSR1 decay curve**: 0 - EXP = 0, 1 - SOFT = 1, 2 - STEEP = 2, 3 - CONCAVE = 3, 4 - FAST S = 4, 5 - SLOW THEN LIN = 5, 6 - ALMOST LIN = 6, 7 - LINEAR = 7
- **CC 29, ADSR2 attack curve**: 0 - EXP = 0, 1 - SOFT = 1, 2 - STEEP = 2, 3 - CONCAVE = 3, 4 - FAST S = 4, 5 - SLOW THEN LIN = 5, 6 - ALMOST LIN = 6, 7 - LINEAR = 7
- **CC 30, ADSR2 decay curve**: 0 - EXP = 0, 1 - SOFT = 1, 2 - STEEP = 2, 3 - CONCAVE = 3, 4 - FAST S = 4, 5 - SLOW THEN LIN = 5, 6 - ALMOST LIN = 6, 7 - LINEAR = 7
- **CC 118, Filter mode**: 0 - LP24 = 0, 1 - BP12 = 1, 2 - HP6/LP18 = 2, 3 - alt = 3
- **CC 60, LFO1 waveform**: 0 - Off = 0, 1 - Saw = 1, 2 - Analog Sine = 2, 3 - Sine = 3, 4 - Square = 4, 5 - Sharktooth = 5, 6 - Trapezoid = 6, 7 - Linear Tri = 7, 8 - Staircase = 8, 9 - Folded Sine = 9, 10 - Analog Tape = 10, 11 - Analog Tube = 11, 12 - Analog Broken = 12
- **CC 61, LFO2 waveform**: 0 - Off = 0, 1 - Saw = 1, 2 - Analog Sine = 2, 3 - Sine = 3, 4 - Square = 4, 5 - Sharktooth = 5, 6 - Trapezoid = 6, 7 - Linear Tri = 7, 8 - Staircase = 8, 9 - Folded Sine = 9, 10 - Analog Tape = 10, 11 - Analog Tube = 11, 12 - Analog Broken = 12
- **CC 84, Mod slot 0 source**: 0 - Off = 0, 1 - LFO 1 = 1, 2 - LFO 2 = 2, 3 - Env VCA = 3, 4 - Env VCF = 4, 5 - Env DCO = 5, 6 - Mod Wheel = 6, 7 - Aftertouch = 7, 8 - Velocity = 8, 9 - Pitch Bend = 9, 10 - Drift Global = 10, 11 - Keytrack = 11, 12 - Drift Voice = 12, 13 - Random S&H = 13, 14 - Voice ID = 14, 15 - Noise = 15, 16 - LFO 3 = 16, 17 - Expression = 17, 18 - Breath = 18
- **CC 85, Mod slot 0 dest**: 0 - Master Pitch = 0, 1 - Cutoff = 1, 2 - OSC1 Level = 2, 3 - OSC2 Level = 3, 4 - Sub Level = 4, 5 - Dist Drive = 5, 6 - Dist Mix = 6, 7 - VCA Level = 7, 8 - Resonance = 8, 9 - Env -> VCF = 9, 10 - Env -> VCA = 10, 11 - LFO 1 Speed = 11, 12 - LFO 2 Speed = 12, 13 - LFO 1 Depth = 13, 14 - LFO 2 Depth = 14, 16 - Pulse Width = 16, 17 - EnvVCF Attack = 17, 18 - EnvVCF Decay = 18, 19 - EnvVCA Attack = 19, 20 - EnvVCA Decay = 20, 21 - ALL Env Time = 21, 22 - Mod Slot 0 = 22, 23 - Mod Slot 1 = 23, 24 - Mod Slot 2 = 24, 25 - Mod Slot 3 = 25, 26 - Mod Slot 4 = 26, 27 - Mod Slot 5 = 27, 28 - Mod Slot 6 = 28, 29 - Mod Slot 7 = 29, 30 - OSC1 Pitch = 30, 31 - OSC2 Pitch = 31, 32 - LFO 3 Speed = 32, 33 - Crossmod Depth = 33
- **CC 87, Mod slot 1 source**: 0 - Off = 0, 1 - LFO 1 = 1, 2 - LFO 2 = 2, 3 - Env VCA = 3, 4 - Env VCF = 4, 5 - Env DCO = 5, 6 - Mod Wheel = 6, 7 - Aftertouch = 7, 8 - Velocity = 8, 9 - Pitch Bend = 9, 10 - Drift Global = 10, 11 - Keytrack = 11, 12 - Drift Voice = 12, 13 - Random S&H = 13, 14 - Voice ID = 14, 15 - Noise = 15, 16 - LFO 3 = 16, 17 - Expression = 17, 18 - Breath = 18
- **CC 88, Mod slot 1 dest**: 0 - Master Pitch = 0, 1 - Cutoff = 1, 2 - OSC1 Level = 2, 3 - OSC2 Level = 3, 4 - Sub Level = 4, 5 - Dist Drive = 5, 6 - Dist Mix = 6, 7 - VCA Level = 7, 8 - Resonance = 8, 9 - Env -> VCF = 9, 10 - Env -> VCA = 10, 11 - LFO 1 Speed = 11, 12 - LFO 2 Speed = 12, 13 - LFO 1 Depth = 13, 14 - LFO 2 Depth = 14, 16 - Pulse Width = 16, 17 - EnvVCF Attack = 17, 18 - EnvVCF Decay = 18, 19 - EnvVCA Attack = 19, 20 - EnvVCA Decay = 20, 21 - ALL Env Time = 21, 22 - Mod Slot 0 = 22, 23 - Mod Slot 1 = 23, 24 - Mod Slot 2 = 24, 25 - Mod Slot 3 = 25, 26 - Mod Slot 4 = 26, 27 - Mod Slot 5 = 27, 28 - Mod Slot 6 = 28, 29 - Mod Slot 7 = 29, 30 - OSC1 Pitch = 30, 31 - OSC2 Pitch = 31, 32 - LFO 3 Speed = 32, 33 - Crossmod Depth = 33
- **CC 90, Mod slot 2 source**: 0 - Off = 0, 1 - LFO 1 = 1, 2 - LFO 2 = 2, 3 - Env VCA = 3, 4 - Env VCF = 4, 5 - Env DCO = 5, 6 - Mod Wheel = 6, 7 - Aftertouch = 7, 8 - Velocity = 8, 9 - Pitch Bend = 9, 10 - Drift Global = 10, 11 - Keytrack = 11, 12 - Drift Voice = 12, 13 - Random S&H = 13, 14 - Voice ID = 14, 15 - Noise = 15, 16 - LFO 3 = 16, 17 - Expression = 17, 18 - Breath = 18
- **CC 91, Mod slot 2 dest**: 0 - Master Pitch = 0, 1 - Cutoff = 1, 2 - OSC1 Level = 2, 3 - OSC2 Level = 3, 4 - Sub Level = 4, 5 - Dist Drive = 5, 6 - Dist Mix = 6, 7 - VCA Level = 7, 8 - Resonance = 8, 9 - Env -> VCF = 9, 10 - Env -> VCA = 10, 11 - LFO 1 Speed = 11, 12 - LFO 2 Speed = 12, 13 - LFO 1 Depth = 13, 14 - LFO 2 Depth = 14, 16 - Pulse Width = 16, 17 - EnvVCF Attack = 17, 18 - EnvVCF Decay = 18, 19 - EnvVCA Attack = 19, 20 - EnvVCA Decay = 20, 21 - ALL Env Time = 21, 22 - Mod Slot 0 = 22, 23 - Mod Slot 1 = 23, 24 - Mod Slot 2 = 24, 25 - Mod Slot 3 = 25, 26 - Mod Slot 4 = 26, 27 - Mod Slot 5 = 27, 28 - Mod Slot 6 = 28, 29 - Mod Slot 7 = 29, 30 - OSC1 Pitch = 30, 31 - OSC2 Pitch = 31, 32 - LFO 3 Speed = 32, 33 - Crossmod Depth = 33
- **CC 93, Mod slot 3 source**: 0 - Off = 0, 1 - LFO 1 = 1, 2 - LFO 2 = 2, 3 - Env VCA = 3, 4 - Env VCF = 4, 5 - Env DCO = 5, 6 - Mod Wheel = 6, 7 - Aftertouch = 7, 8 - Velocity = 8, 9 - Pitch Bend = 9, 10 - Drift Global = 10, 11 - Keytrack = 11, 12 - Drift Voice = 12, 13 - Random S&H = 13, 14 - Voice ID = 14, 15 - Noise = 15, 16 - LFO 3 = 16, 17 - Expression = 17, 18 - Breath = 18
- **CC 94, Mod slot 3 dest**: 0 - Master Pitch = 0, 1 - Cutoff = 1, 2 - OSC1 Level = 2, 3 - OSC2 Level = 3, 4 - Sub Level = 4, 5 - Dist Drive = 5, 6 - Dist Mix = 6, 7 - VCA Level = 7, 8 - Resonance = 8, 9 - Env -> VCF = 9, 10 - Env -> VCA = 10, 11 - LFO 1 Speed = 11, 12 - LFO 2 Speed = 12, 13 - LFO 1 Depth = 13, 14 - LFO 2 Depth = 14, 16 - Pulse Width = 16, 17 - EnvVCF Attack = 17, 18 - EnvVCF Decay = 18, 19 - EnvVCA Attack = 19, 20 - EnvVCA Decay = 20, 21 - ALL Env Time = 21, 22 - Mod Slot 0 = 22, 23 - Mod Slot 1 = 23, 24 - Mod Slot 2 = 24, 25 - Mod Slot 3 = 25, 26 - Mod Slot 4 = 26, 27 - Mod Slot 5 = 27, 28 - Mod Slot 6 = 28, 29 - Mod Slot 7 = 29, 30 - OSC1 Pitch = 30, 31 - OSC2 Pitch = 31, 32 - LFO 3 Speed = 32, 33 - Crossmod Depth = 33
- **CC 96, Mod slot 4 source**: 0 - Off = 0, 1 - LFO 1 = 1, 2 - LFO 2 = 2, 3 - Env VCA = 3, 4 - Env VCF = 4, 5 - Env DCO = 5, 6 - Mod Wheel = 6, 7 - Aftertouch = 7, 8 - Velocity = 8, 9 - Pitch Bend = 9, 10 - Drift Global = 10, 11 - Keytrack = 11, 12 - Drift Voice = 12, 13 - Random S&H = 13, 14 - Voice ID = 14, 15 - Noise = 15, 16 - LFO 3 = 16, 17 - Expression = 17, 18 - Breath = 18
- **CC 97, Mod slot 4 dest**: 0 - Master Pitch = 0, 1 - Cutoff = 1, 2 - OSC1 Level = 2, 3 - OSC2 Level = 3, 4 - Sub Level = 4, 5 - Dist Drive = 5, 6 - Dist Mix = 6, 7 - VCA Level = 7, 8 - Resonance = 8, 9 - Env -> VCF = 9, 10 - Env -> VCA = 10, 11 - LFO 1 Speed = 11, 12 - LFO 2 Speed = 12, 13 - LFO 1 Depth = 13, 14 - LFO 2 Depth = 14, 16 - Pulse Width = 16, 17 - EnvVCF Attack = 17, 18 - EnvVCF Decay = 18, 19 - EnvVCA Attack = 19, 20 - EnvVCA Decay = 20, 21 - ALL Env Time = 21, 22 - Mod Slot 0 = 22, 23 - Mod Slot 1 = 23, 24 - Mod Slot 2 = 24, 25 - Mod Slot 3 = 25, 26 - Mod Slot 4 = 26, 27 - Mod Slot 5 = 27, 28 - Mod Slot 6 = 28, 29 - Mod Slot 7 = 29, 30 - OSC1 Pitch = 30, 31 - OSC2 Pitch = 31, 32 - LFO 3 Speed = 32, 33 - Crossmod Depth = 33
- **CC 103, Mod slot 5 source**: 0 - Off = 0, 1 - LFO 1 = 1, 2 - LFO 2 = 2, 3 - Env VCA = 3, 4 - Env VCF = 4, 5 - Env DCO = 5, 6 - Mod Wheel = 6, 7 - Aftertouch = 7, 8 - Velocity = 8, 9 - Pitch Bend = 9, 10 - Drift Global = 10, 11 - Keytrack = 11, 12 - Drift Voice = 12, 13 - Random S&H = 13, 14 - Voice ID = 14, 15 - Noise = 15, 16 - LFO 3 = 16, 17 - Expression = 17, 18 - Breath = 18
- **CC 104, Mod slot 5 dest**: 0 - Master Pitch = 0, 1 - Cutoff = 1, 2 - OSC1 Level = 2, 3 - OSC2 Level = 3, 4 - Sub Level = 4, 5 - Dist Drive = 5, 6 - Dist Mix = 6, 7 - VCA Level = 7, 8 - Resonance = 8, 9 - Env -> VCF = 9, 10 - Env -> VCA = 10, 11 - LFO 1 Speed = 11, 12 - LFO 2 Speed = 12, 13 - LFO 1 Depth = 13, 14 - LFO 2 Depth = 14, 16 - Pulse Width = 16, 17 - EnvVCF Attack = 17, 18 - EnvVCF Decay = 18, 19 - EnvVCA Attack = 19, 20 - EnvVCA Decay = 20, 21 - ALL Env Time = 21, 22 - Mod Slot 0 = 22, 23 - Mod Slot 1 = 23, 24 - Mod Slot 2 = 24, 25 - Mod Slot 3 = 25, 26 - Mod Slot 4 = 26, 27 - Mod Slot 5 = 27, 28 - Mod Slot 6 = 28, 29 - Mod Slot 7 = 29, 30 - OSC1 Pitch = 30, 31 - OSC2 Pitch = 31, 32 - LFO 3 Speed = 32, 33 - Crossmod Depth = 33
- **CC 106, Mod slot 6 source**: 0 - Off = 0, 1 - LFO 1 = 1, 2 - LFO 2 = 2, 3 - Env VCA = 3, 4 - Env VCF = 4, 5 - Env DCO = 5, 6 - Mod Wheel = 6, 7 - Aftertouch = 7, 8 - Velocity = 8, 9 - Pitch Bend = 9, 10 - Drift Global = 10, 11 - Keytrack = 11, 12 - Drift Voice = 12, 13 - Random S&H = 13, 14 - Voice ID = 14, 15 - Noise = 15, 16 - LFO 3 = 16, 17 - Expression = 17, 18 - Breath = 18
- **CC 107, Mod slot 6 dest**: 0 - Master Pitch = 0, 1 - Cutoff = 1, 2 - OSC1 Level = 2, 3 - OSC2 Level = 3, 4 - Sub Level = 4, 5 - Dist Drive = 5, 6 - Dist Mix = 6, 7 - VCA Level = 7, 8 - Resonance = 8, 9 - Env -> VCF = 9, 10 - Env -> VCA = 10, 11 - LFO 1 Speed = 11, 12 - LFO 2 Speed = 12, 13 - LFO 1 Depth = 13, 14 - LFO 2 Depth = 14, 16 - Pulse Width = 16, 17 - EnvVCF Attack = 17, 18 - EnvVCF Decay = 18, 19 - EnvVCA Attack = 19, 20 - EnvVCA Decay = 20, 21 - ALL Env Time = 21, 22 - Mod Slot 0 = 22, 23 - Mod Slot 1 = 23, 24 - Mod Slot 2 = 24, 25 - Mod Slot 3 = 25, 26 - Mod Slot 4 = 26, 27 - Mod Slot 5 = 27, 28 - Mod Slot 6 = 28, 29 - Mod Slot 7 = 29, 30 - OSC1 Pitch = 30, 31 - OSC2 Pitch = 31, 32 - LFO 3 Speed = 32, 33 - Crossmod Depth = 33
- **CC 109, Mod slot 7 source**: 0 - Off = 0, 1 - LFO 1 = 1, 2 - LFO 2 = 2, 3 - Env VCA = 3, 4 - Env VCF = 4, 5 - Env DCO = 5, 6 - Mod Wheel = 6, 7 - Aftertouch = 7, 8 - Velocity = 8, 9 - Pitch Bend = 9, 10 - Drift Global = 10, 11 - Keytrack = 11, 12 - Drift Voice = 12, 13 - Random S&H = 13, 14 - Voice ID = 14, 15 - Noise = 15, 16 - LFO 3 = 16, 17 - Expression = 17, 18 - Breath = 18
- **CC 110, Mod slot 7 dest**: 0 - Master Pitch = 0, 1 - Cutoff = 1, 2 - OSC1 Level = 2, 3 - OSC2 Level = 3, 4 - Sub Level = 4, 5 - Dist Drive = 5, 6 - Dist Mix = 6, 7 - VCA Level = 7, 8 - Resonance = 8, 9 - Env -> VCF = 9, 10 - Env -> VCA = 10, 11 - LFO 1 Speed = 11, 12 - LFO 2 Speed = 12, 13 - LFO 1 Depth = 13, 14 - LFO 2 Depth = 14, 16 - Pulse Width = 16, 17 - EnvVCF Attack = 17, 18 - EnvVCF Decay = 18, 19 - EnvVCA Attack = 19, 20 - EnvVCA Decay = 20, 21 - ALL Env Time = 21, 22 - Mod Slot 0 = 22, 23 - Mod Slot 1 = 23, 24 - Mod Slot 2 = 24, 25 - Mod Slot 3 = 25, 26 - Mod Slot 4 = 26, 27 - Mod Slot 5 = 27, 28 - Mod Slot 6 = 28, 29 - Mod Slot 7 = 29, 30 - OSC1 Pitch = 30, 31 - OSC2 Pitch = 31, 32 - LFO 3 Speed = 32, 33 - Crossmod Depth = 33

## Deliberately not mapped

These parameters stay panel/serial only:

- **Crossmod depth** (parameter 130)
- **Crossmod mode** (parameter 131)
- **EnvVCA mode** (parameter 224)
- **EnvVCF mode** (parameter 225)
- **EnvDCO mode** (parameter 223)
- **ADSR1 release curve** (parameter 50)
- **ADSR2 release curve** (parameter 53)
- **ADSR3 attack curve** (parameter 54)
- **ADSR3 decay curve** (parameter 55)
- **ADSR3 release curve** (parameter 56)
- **ADSR3 restart** (parameter 214)
- **PWM pots manual** (parameter 124)
- **LFO2 to OSC3 coarse** (parameter 220)
- **Character** (parameter 221)
- **Run calibration** (parameter 150)
- **Manual calibration mode** (parameter 151)
- **Manual cal stage** (parameter 152)
- **Manual cal offset** (parameter 153)
- **Amp comp @ 440 Hz** (parameter 159)
- **PW center (cal)** (parameter 162)
- **Duty trim (0.01%)** (parameter 161)
- **Store manual cal offsets** (parameter 156)

Autotune takes the board over for about a minute and the store writes the filesystem, so neither should be one stray controller away. Both are still available from the serial bench app in `DCO-CONTROL-PANEL`.

Reserved controllers left untouched: 0, 1, 6, 7, 10, 11, 32, 38, 42, 64, 98, 99, 100, 101, 120, 121, 122, 123, 124, 125, 126, 127. CC 0 / CC 32 are Bank Select: nonzero latches bank 1 so the next Program Change recalls slots 128..255 (`midi.ino`). CC 42 keeps its historical meaning here, pitch-bend range in semitones. 98-101 stay free so a later NRPN upgrade needs no reshuffling. CC 120 (All Sound Off) is reserved and is why LFO2→OSC3 coarse has no assignment.
