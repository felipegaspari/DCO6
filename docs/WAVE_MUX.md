# Wave mux — per-osc Saw / Pulse / Tri

Analog waveform switching driven from two daisy-chained 74HC595s into three DG411 switch arrays.

> 🔴 **Corrected 2026-09-16.** The ParamId table was wrong: OSC2/OSC3 wave enables were listed as
> **84–89**, but **84–86 are mod-matrix slot 7** in the current enum. The real ids are
> **87–92**. Writing a wave enable to 84 rewrites a matrix slot instead. See
> [§7](#7-what-changed).

Not to be confused with the AS3320 filter-mode DG411 on VOICE-AUX
([`FILTER_ROUTING.md`](FILTER_ROUTING.md)).

**Status on this board: `ENABLE_WAVE_MUX` is OFF.** Every function compiles to an empty stub;
the soft `waveEnable[][]` state still updates so presets and the panel stay coherent. The draft
pins collide with the 8-oscillator RESET/RANGE map — see [`PINOUT.md`](PINOUT.md).

---

## 1. Hardware

```text
DCO GP12 DATA / GP13 LATCH / GP14 CLK
        ↓
  2× 74HC595 daisy-chained (16 bits)
        ↓  active-low IN
  3× DG411 (12 SPST; 9 used, 3 spare)
        ↓
  OSC1 / OSC2 / OSC3 mix buses
```

- **DG411 closes when IN is low** → 595 bit `0` = wave **on**, `1` = off.
- `waveMuxBits` is initialised to `0xFFFF` (everything off) and unused bits stay high.

### Bit map

```cpp
static const uint8_t WAVE_MUX_BIT[3][3] = {
  { 0, 1, 2 },  // OSC1 Saw, Pulse, Tri
  { 3, 4, 5 },  // OSC2
  { 6, 7, 8 },  // OSC3
};
```

| Bit | Enable |
|----:|--------|
| 0–2 | OSC1 Saw / Pulse / Tri |
| 3–5 | OSC2 Saw / Pulse / Tri |
| 6–8 | OSC3 Saw / Pulse / Tri |
| 9–15 | unused (held high) |

Shift is **MSB first** into the daisy-chain (chip 2 then chip 1), with a 1 µs `busy_wait_us_32`
on each clock edge and on the latch pulse. Remap `WAVE_MUX_BIT[3][3]` in
[`wave_mux.ino`](../wave_mux.ino) when the PCB is frozen.

---

## 2. ParamIds

| ID | Name |
|---:|------|
| 1 | `PARAM_OSC1_SAW_ENABLE` |
| 2 | `PARAM_OSC1_PULSE_ENABLE` |
| 3 | `PARAM_OSC1_TRI_ENABLE` |
| **87** | `PARAM_OSC2_SAW_ENABLE` |
| **88** | `PARAM_OSC2_PULSE_ENABLE` |
| **89** | `PARAM_OSC2_TRI_ENABLE` |
| **90** | `PARAM_OSC3_SAW_ENABLE` |
| **91** | `PARAM_OSC3_PULSE_ENABLE` |
| **92** | `PARAM_OSC3_TRI_ENABLE` |

- `PARAM_SINE_STATUS` (4): **deprecated**, retained for ID stability, no mux role.
- ParamIds **5–6**: unused (formerly digital-square enables) — reserved.
- **84–86 belong to mod-matrix slot 7** (`SOURCE` / `DEST` / `DEPTH`). Do not use them here.

MIDI CCs: OSC1 on 16/17/18, OSC2 Saw/Pulse on 112/113, OSC3 on 115/116/117.
⚠️ **`PARAM_OSC2_TRI_ENABLE` has no CC** — and on a 4×2 board OSC2 *is* the triangle core, so
that is the most useful of the three. See [`MIDI_CC_MAP.md`](MIDI_CC_MAP.md) §3 for free CC
numbers.

State: `bool waveEnable[osc 0..2][wave 0=Saw, 1=Pulse, 2=Tri]` in
[`cv_state.h`](../cv_state.h). All nine ids are in `PERSISTABLE_PARAMS`.

---

## 3. ⚠️ `update_waveSelector()` only refreshes two oscillators

```cpp
void update_waveSelector() {
  waveMuxBits = 0xFFFF;
  for (uint8_t osc = 0; osc < 2; osc++) {        // ← 2, not 3
    for (uint8_t wave = 0; wave < 3; wave++) {
      waveMuxWritePin(WAVE_MUX_BIT[osc][wave], waveEnable[osc][wave] ? 0 : 1);
    }
  }
  waveMuxShiftOut();
}
```

The outer loop stops at `osc < 2`, so **OSC3's three bits (6–8) are never written** and stay
high — permanently off. `WAVE_MUX_BIT` still declares row 2, and
`waveSelector_manual_calibration()` *can* reach OSC3, so the bit plan is intact; only the normal
refresh path skips it.

Two readings, and the code does not say which is intended:

- **Deliberate** — this is a 4×2 board with no third analog oscillator, so refreshing OSC3 is
  pointless. If so, the loop bound should be `NUM_OSCILLATORS / 2` or a named constant rather
  than a bare `2`, and `waveEnable[2][*]` should be documented as inert here.
- **A leftover** from the 3-oscillator monosynth that was shortened for this board and will be
  wrong again if OSC3 analog ever exists.

Since the whole file is behind `ENABLE_WAVE_MUX` (**off**), this has no audible effect today —
but it will the moment the flag is turned on. **Worth confirming before then.**

---

## 4. Pulse gating

`pulseWaveOn` (declared in [`globals.h`](../globals.h), set by
`apply_param_osc1_pulse_enable`) gates the shared PW PWM: when no oscillator has Pulse enabled
the PW channel is forced to 0.

> Note it is driven by **OSC1's** pulse enable alone, not by a reduction over
> `waveEnable[*][1]`. With `ENABLE_WAVE_MUX` off and a 4×2 analog board that is consistent —
> but it is a single-oscillator signal being used as a global gate.

---

## 5. Manual calibration

```cpp
void waveSelector_manual_calibration(byte stage) {
  waveMuxBits = 0xFFFF;
  uint8_t osc = cal_stage_to_osc(stage);
  if (osc > 2) osc = 2;
  uint8_t wave = 1;                              // default: Pulse
  if      (cal_stage_is_saw(stage)) wave = 0;
  else if (cal_stage_is_tri(stage)) wave = 2;
  waveMuxWritePin(WAVE_MUX_BIT[osc][wave], 0);
  waveMuxShiftOut();
}
```

All paths off, then exactly **one** enabled for the stage's oscillator:

| Stage kind | Wave selected |
|---|---|
| `cal_stage_is_saw(stage)` | 0 — Saw |
| `cal_stage_is_tri(stage)` | 2 — Tri |
| otherwise (pulse / 440 Hz sub-stages) | 1 — Pulse |

> The previous revision said *"enables OSC{stage} Saw only"* and the source comment still reads
> *"Never TRI"* — both are stale. Triangle **is** selectable, which matters on this board because
> the odd oscillators are triangle cores ([`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §1.1).

`cal_stage_to_osc(stage)` maps a stage to an oscillator index and is clamped to ≤ 2, so with 8
oscillators the mux can only ever address the first three. That is a mux-side limit, not a
calibration-side one.

This function is called from `update_CV_outs_manual_calibration()`, which — with
`ENABLE_CV_OUTS` off — does nothing else.

---

## 6. Compiled-out stubs

```cpp
#else  // !ENABLE_WAVE_MUX
void init_waveSelector() {}
void update_waveSelector() {}
void waveSelector_manual_calibration(byte) {}
```

So `init_waveSelector()` in `setup1()` and the calibration call are always safe to leave in
place. The three HC595 pins (GP12/13/14) collide with V3 A RESET, V2 B RESET and V2 A RANGE —
turning the flag on without a PCB remap breaks three oscillators.

---

## 7. What changed

| Topic | Previous revision | Current |
|---|---|---|
| OSC2 enables | 84 / 85 / 86 | **87 / 88 / 89** |
| OSC3 enables | 87 / 88 / 89 | **90 / 91 / 92** |
| 84–86 | wave enables | **Mod-matrix slot 7** |
| Flag location | `DCO.ino` | [`settings.h`](../settings.h) |
| Calibration wave | "Saw only" | Saw / **Tri** / Pulse by stage kind |
| PW gate source | `waveEnable[*][1]` | `pulseWaveOn`, from OSC1's enable |
| `update_waveSelector` scope | implied all three oscillators | **loops `osc < 2`** — OSC3 never refreshed |

---

## 8. Code

| File | Role |
|------|------|
| [`wave_mux.ino`](../wave_mux.ino) | Bit-bang dual 595, bit map, calibration select, stubs |
| [`wave_mux.h`](../wave_mux.h) | Declarations |
| [`cv_state.h`](../cv_state.h) | `waveEnable[3][3]` |
| [`globals.h`](../globals.h) | `HC595_DATA/LATCH/CLK_PIN`, `pulseWaveOn` |
| [`params.ino`](../params.ino) | Nine apply handlers |
| [`_shared/autotune.h`](../_shared/autotune.h) | `cal_stage_to_osc`, `cal_stage_is_saw`, `cal_stage_is_tri` |
| [`PINOUT.md`](PINOUT.md) | GPIO summary and the RESET/RANGE collisions |
