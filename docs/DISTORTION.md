# Post-filter distortion (Drive + Mix)

**Status: hardware design locked for breadboard / PCB. Firmware is a reservation only** — the
two ParamIds exist and reach the board, but there is no applier on the DCO.

> 🔴 **Corrected 2026-09-16.** The ParamIds were wrong: Drive/Mix are **58 / 59**, not 52/53 —
> **52 and 53 are `PARAM_ADSR2_DECAY_CURVE` / `PARAM_ADSR2_RELEASE_CURVE`**. See
> [§9](#9-what-changed).

Related: [`PINOUT.md`](PINOUT.md), [`FILTER_ROUTING.md`](FILTER_ROUTING.md) (SSI2144 → dist →
AS3320 multimode), [`DUAL_MCU.md`](DUAL_MCU.md) (Drive/Mix → RP2040 aux in the dual-MCU build).

### Schematic (KiCad)

Open the KiCad **10** project:
[`schematics/distortion/distortion.kicad_pro`](schematics/distortion/distortion.kicad_pro)

Sheet: [`schematics/distortion/distortion.kicad_sch`](schematics/distortion/distortion.kicad_sch)
— fully wired left→right (CV RC / dry buf + AS2164 / drive process / mix summer / `DIST_OUT`).
Embedded symbols (`DCO:AS2164`, `DCO:TL074`, …); edge labels `LP_OUT`, `DRIVE_CV`, `MIX_CV`,
`DIST_OUT`, plus mid-chain `DRY`, `WET`, `MIX_FILT`, `MIX_CV_INV`. PDF:
[`schematics/distortion/distortion.pdf`](schematics/distortion/distortion.pdf). Regenerate with
`python3 schematics/distortion/generate_sch.py`.

---

## 1. Firmware status

| Item | State |
|---|---|
| `PARAM_DIST_DRIVE` | **58** — reserved, in `PERSISTABLE_PARAMS`, MIDI **CC 81** |
| `PARAM_DIST_MIX` | **59** — reserved, in `PERSISTABLE_PARAMS`, MIDI **CC 82** |
| DCO appliers | ❌ **none** — no `apply_param_dist_*` in the DCO `paramTable[]` |
| Matrix destinations | `DEST_DIST_DRIVE` **5**, `DEST_DIST_MIX` **6** ([`MOD_MATRIX.md`](MOD_MATRIX.md)) |
| PWM writers | Behind `ENABLE_CV_OUTS` (**off**) in [`PWM.ino`](../PWM.ino) |
| Pins | `DIST_DRIVE_PIN` **GP9**, `DIST_MIX_PIN` **GP26** in [`globals.h`](../globals.h) |

Both ids travel host → DCO end to end and **nothing on this board acts on them**. The matrix
destinations exist, so a slot can be pointed at Dist Drive today — the sum is computed and then
goes nowhere.

⚠️ **GP9 and GP26 are not free.** With 8 oscillators GP9 is **RANGE osc 6** and GP26 is **RANGE
osc 0** on Pico / Pico 2 / WeAct RP2350 ([`PINOUT.md`](PINOUT.md)). Driving distortion from the
DCO in a solo build needs a PCB remap or the aux MCU.

---

## 2. Signal chain

```text
Osc / sub mix (level VCAs — may run hot into the LP)
  → SSI2144 LOWPASS (Cut0 / Res0)
  → Distortion (Drive + Mix only)
  → AS3320 multimode (Cut1 / Res1; digital mode — see FILTER_ROUTING.md)
  → Main Amp VCA
  → FV-1 (later)
  → outs
```

Dry for Mix is taken at **post-SSI2144 / pre-dist** — the same node that feeds the Drive VCA. No
symmetry, tone, or pre/post filter-order switch in v1.

```text
LP out ──┬── dry ──────────────────────────────┐
         │                                     │
         ▼                                     │
      Drive VCA ◄── CV Drive (0..~3 V)         │
         │                                     │
         ▼                                     │
      Presence shelf (fixed RC, slight HF boost)
         │                                     │
         ▼                                     │
      Asymmetric soft clip (unequal diodes / LED)
         │                                     │
         ▼                                     │
      Light fold stage (mainly at high Drive)
         │                                     │
         ▼                                     │
      AC couple + mild LPF (kill DC / ultrasonic)
         │                                     │
         ▼                                     │
      Wet ──► Mix crossfade VCAs ◄── CV Mix ──► sum ──► HP in
              (dry rises as wet falls)
```

---

## 3. Detailed circuit

One audio input (`LP_OUT`), one audio output (`DIST_OUT` → HP). Two control voltages from the
MCU (PWM + RC or DAC): **Drive** and **Mix**, roughly 0…3 V into the **AS2164** Ec pins (expo
control; scale/trim to the chip's CV range).

### 3.1 Split dry / wet

`LP_OUT` feeds a short bus:

1. **Dry path** — unity op-amp follower so the Mix stage does not load the LP.
2. **Wet path** — into the Drive VCA.

Keep both taps AC-coupled from the LP if its output sits on a DC bias; otherwise one coupling
cap at `LP_OUT` is enough.

### 3.2 Drive VCA

**Job:** set how hard the nonlinear core is hit. At Drive = 0 the wet path should be quiet
enough that Mix ≈ dry.

**Part:** one channel of **AS2164** (Coolaudio SSM2164-compatible quad VCA). Current in /
current out — series `Rin` into `I_IN`, op-amp I–V converter on `I_OUT`.

- Audio in: post-LP, after the dry tap
- CV: **Drive**, after the RC filter / buffer
- Gain law: expo (2164) is fine; trim so mid-Drive is "warm" and max reaches the fold, not an
  instant brickwall

Optional: a small **fixed gain** after the VCA (×2…×4) so the clipper is easier to reach without
extreme CV.

### 3.3 Presence shelf (fixed)

A little more high-mid into the clipper so grit is brighter, without a Tone knob.

- Boost on the order of **+3…+6 dB** above roughly 1–2 kHz
- Fixed resistors / caps only, tuned on the bench

### 3.4 Asymmetric soft clip

First nonlinearity — warm saturation, not a hard square. Op-amp stage with diodes in the
feedback or to a bias point, **asymmetric**:

- One polarity: **1N4148** (or two in series)
- Other polarity: **LED** (red/amber) or a different diode stack

Different positive/negative thresholds mix even and odd harmonics; less "dead" than a matched
4148 pair.

Targets: soft onset (diodes in feedback, or a series resistor with them); low Drive barely
conducting; high Drive strongly soft-limiting **before** the fold still has headroom.

### 3.5 Light fold (high Drive)

Extra harmonic bloom at high Drive, near-transparent at low Drive. One **Lockhart-style** (or
diode-bridge / current) fold is enough — not a multi-fold Serge monster.

- Soft-clipped signal → series resistor → **fold cell** → buffer
- Set the fold threshold **above** soft-clip onset: clean → soft clip → fold

If the fold is too aggressive, pad with a series R or divider before the cell.

### 3.6 AC couple + mild LPF

Remove DC from the asymmetric clip/fold, and ultrasonic hash before the Mix VCAs and HP.

- Series **coupling cap** (e.g. 1 µF film)
- Mild 1-pole (or 2-pole) LPF around **15–25 kHz**

Output of this stage is **WET**.

### 3.7 Mix crossfade → HP

Preferred: **two more AS2164 channels** (same IC as Drive):

| Path | CV |
|------|-----|
| Dry VCA | ∝ (1 − Mix) |
| Wet VCA | ∝ Mix |

Generate the complementary CV in analog (inverter from Mix around 0…Vref). For v1, **one Mix CV
+ analog inverter** is enough.

Sum dry and wet in a **passive mix into an op-amp summer** (equal resistors), buffer →
`DIST_OUT` → HP filter input.

| Mix | Result |
|-----|--------|
| 0 | Dry only (distortion effectively out) |
| Mid | Parallel grit |
| Max | Full wet |

---

## 4. CV conditioning

Same pattern as cutoff / main VCA:

```text
GP9  PWM ── RC (e.g. 10k + 100n…1µ) ──► optional buffer ──► Drive VCA CV
GP26 PWM ── RC ──► buffer ──► Mix (+ inverter for dry CV)
```

Scale / offset so:

- Drive = 0 → wet path near mute, below diode conduction
- Drive = max → into the fold
- Mix = 0 → dry open, wet muted

> On the dual-MCU build these CVs move to the aux RP2040 (`ENABLE_VOICE_AUX`), which frees GP9
> and GP26 on the DCO — `globals.h` then reassigns them as `OSC3_LEVEL_PIN` and `SUB_LEVEL_PIN`.

Manual calibration must park both CVs at 0. With `ENABLE_CV_OUTS` off,
`update_CV_outs_manual_calibration()` only drives the wave selector, so there is nothing to park
today.

---

## 5. Parts budget (typical)

| Qty | Role |
|-----|------|
| 1× **AS2164** | Drive + dry Mix + wet Mix (3 of 4 sections; 4th spare) |
| 2× **TL074** | Dry buffer, I–V converters, clip, fold buffer, summer, MIX invert |
| Diodes + 1 LED | Asymmetric soft clip |
| Small fold network | Diodes + resistors |
| R's and C's | Presence, coupling, LPF, CV filters, `Rin` / `Rf` |

---

## 6. Not in v1

- No symmetry CV, tone pot, or LP↔HP **order** switch — the chain is fixed
  SSI2144 → dist → AS3320; AS3320 **mode** is separate
  ([`FILTER_ROUTING.md`](FILTER_ROUTING.md))
- No digital audio path — only two analog CVs

---

## 7. Bench tune order

1. Mix = 0 — confirm unity dry, silent wet.
2. Mix = max, raise Drive slowly — soft-clip character.
3. Drive to top — fold appears without exploding level.
4. Mix mid — parallel blend feels musical.
5. Sweep LP resonance into it — peaks should push the clipper; HP after cleans mud.

---

## 8. Digital control

| Param | ID | CC | Range | Default | Meaning |
|-------|---:|---:|------:|--------:|---------|
| Drive | `PARAM_DIST_DRIVE` | **58** | 81 | 0..4095 | Drive VCA CV (0 = no push into the clipper) |
| Mix | `PARAM_DIST_MIX` | **59** | 82 | 0..4095 | 0 = dry, 4095 = full wet |

Matrix destinations `DEST_DIST_DRIVE` (5) and `DEST_DIST_MIX` (6) add on top of the panel base,
per voice — though with no applier the sum is currently unused.

---

## 9. What changed

| Topic | Previous revision | Current |
|---|---|---|
| `PARAM_DIST_DRIVE` | 52 | **58** — 52 is `PARAM_ADSR2_DECAY_CURVE` |
| `PARAM_DIST_MIX` | 53 | **59** — 53 is `PARAM_ADSR2_RELEASE_CURVE` |
| MIDI CCs | not listed | **81 / 82** |
| DCO appliers | implied to exist | **None** — ids reserved but unrouted |
| Matrix dests | 6 / 8 | **5 / 6** (`ModDest` was renumbered) |
| GP9 / GP26 | free for dist | **RANGE pins** for osc 6 / osc 0 — remap or aux needed |
| `MAINBOARD_ABSORPTION.md` link | referenced | Removed — superseded |

---

## 10. Next steps

1. Breadboard the chain and tune per §7.
2. **Decide the owner** — aux RP2040 (`ENABLE_VOICE_AUX`) or a PCB remap freeing GP9/GP26.
3. Add `apply_param_dist_drive` / `apply_param_dist_mix` to the owning board's `paramTable[]`.
4. Wire the matrix destinations 5 / 6 into the same writers so modulation lands.
5. Park both CVs at 0 in the calibration path once writers exist.
