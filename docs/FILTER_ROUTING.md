# Filter routing and AS3320 multimode (concept)

**Status: hardware concept for breadboard / PCB — not a finished schematic, and not implemented
in firmware.** This page is design intent; the only firmware that exists today is the ParamId
reservation and a GPIO stub on [`VOICE-AUX/`](../../VOICE-AUX/).

> 🔴 **Corrected 2026-09-16.** `PARAM_FILTER_MODE` is **60**, not 54 — **54 is
> `PARAM_ADSR3_ATTACK_CURVE`**. The suggested spare pins were also wrong: **GP2 is `PW_PINS[1]`**,
> not free. See [§9](#9-what-changed).

Related: [`DISTORTION.md`](DISTORTION.md) (Drive/Mix stage), [`PINOUT.md`](PINOUT.md) (Cut0/1,
Res0/1 PWM), [`DUAL_MCU.md`](DUAL_MCU.md) (AS3320 mode GPIOs → RP2040 aux in the dual-MCU build).

References:

- [AS3320 datasheet](https://cabintechglobal.com/pdf/ALFA_RPAR_AS3320.pdf) (Figs. 1–3: LP / HP / BP)
- [Electric Druid — CEM3320 filter designs](https://electricdruid.net/cem3320-filter-designs/)
- [Electric Druid — Multimode filters, Part 1 (reconfigurable)](https://electricdruid.net/multimode-filters-part-1-reconfigurable-filters/)
- Elka Synthex / Craig Anderton "Multiple Identity" / hermflink 3320VCF patterns

---

## 1. Firmware status

| Item | State |
|---|---|
| `PARAM_FILTER_MODE` | **60** — reserved in `params_def.h`, listed in `PERSISTABLE_PARAMS` |
| MIDI CC | **118** ([`MIDI_CC_MAP.md`](MIDI_CC_MAP.md)) |
| DCO applier | ❌ **none** — no `apply_param_filter_mode` in the DCO `paramTable[]` |
| Mode GPIOs | Not assigned in `globals.h` |
| Break-before-make timing | Not implemented |
| `PARAM_VCF_TRIGGER_MODE` (57) | Separate parameter — envelope gating, **not** pole configuration |

So the id and CC exist end-to-end from the host, and **nothing on the DCO acts on them**. The
Mainboard owns filter analog on this instrument; whether the applier lands there or on VOICE-AUX
is part of what this concept has to settle.

Neighbouring reserved ids: `PARAM_DIST_DRIVE` **58**, `PARAM_DIST_MIX` **59** — the distortion
stage in the same chain ([`DISTORTION.md`](DISTORTION.md)).

---

## 2. Intended voice chain

```text
Osc / sub mix (level VCAs)
  → SSI2144          fixed 24 dB/oct lowpass     Cut0 / Res0
  → Distortion       AS2164 Drive + Mix          dry tap = SSI2144 out
  → AS3320           digitally switched multimode Cut1 / Res1
  → Main Amp VCA
  → FV-1 (later)
  → outs
```

| IC | Role |
|----|------|
| **SSI2144** | Ladder-family 4-pole **LP only** — grit and character into distortion |
| **AS2164** | Distortion Drive + Mix VCAs |
| **AS3320** | CEM3320-class **reconfigurable** 4-pole — LP / BP / hybrid after grit |
| **DG411** (or 4066-class) | Analog switches that rewire AS3320 stages 1–2 |

One AS3320 **is** multimode; it does not need a second one. Multimode here means **rewiring the
four OTA stages**, not picking simultaneous SVF taps.

`NUM_FILTERS` is **2** in [`globals.h`](../globals.h), and the CV arrays are sized accordingly:
`CUTOFF_PINS[2] = {15, 4}`, `RESO_PINS[2] = {5, 7}` — filter 0 = SSI2144, filter 1 = AS3320.

---

## 3. Why not "swap LP and HP around distortion" with one DG411?

A clean **LP → dist → HP** vs **HP → dist → LP** order swap needs **eight** SPST paths (four
closed per mode). One DG411 has only four. **Out of scope.**

The DG411 (or a cheaper analog quad) is used only to **digitally select AS3320 mode**.

---

## 4. How AS3320 multimode works

### Inside the chip

Four independent filter cells + a shared expo frequency CV (`VCFI`) + an on-chip resonance VCA
(`Vres` / `Ires`).

Each cell, conceptually:

```text
          ┌─ OTA (gm set by freq CV) ─┐
IN_n ──►──┤                           ├──► buffer ──► OUT_n
          └─ external C on Cap_n ─────┘
```

Cascade externally: `OUT1 → IN2 → OUT2 → IN3 → OUT3 → IN4`, with the datasheet input
attenuators. PDIP-18 pins: `INn`, `Capn`, `OUTn` for n = 1…4.

**Multimode is not a post-mux of three live outs.** It is switching each pole between the
datasheet **lowpass** and **highpass** stage circuits.

### One pole: LP vs HP

**Lowpass** (Fig. 1) — "cap to ground":

```text
          Rin                 ┌── OTA ──┐
VIN ──►──/\/\/──●─────────────┤         ├──► buffer ──► VOUT
                │             └────┬────┘
               Rf                  C → GND
            (feedback)
```

**Highpass** (Fig. 2) — R and C roles swapped:

```text
                    C
VIN ──►─────────────┤├────────●──► buffer ──► VOUT
                              │
                         OTA as "R to gnd" (gm ← freq CV)
                              │
                             GND
```

**Bandpass** (Fig. 3) is not a third cell type: cascade **HP poles then LP poles** (HP first so
the LP kills HF noise). Stages 1–2 HP + 3–4 LP → ~12 dB/oct BP.

### Per-stage SPDT

| State | Cap / network | Stage |
|-------|---------------|--------|
| LP | `Cap` node → **GND** (LP `Rin` / `Rf`) | Lowpass |
| HP | `Cap` **in series** with audio into the cell | Highpass |

Each stage needs an **SPDT** (two complementary SPST). One quad SPST package = two SPDT →
**two stages** switchable.

```text
                    SW_A (on for LP)
 Cap_pin ──────────────●──────────────── GND
                       │
                    SW_B (on for HP)
                       │
                       └── HP series path / stage input
```

Copy datasheet Fig. 1 vs Fig. 2 around the same `INn` / `Capn` / `OUTn`, then replace the
hardwired C with that SPDT. Drive complementary controls; **break-before-make** (~50–200 µs)
when changing mode.

---

## 5. Switches are in the audio path

The mode switches sit on **signal nodes**, not digital-only lines.

- **LP:** the throw ties the integrator / `Cap` node to GND — that node carries filtered audio AC.
- **HP:** the throw puts C in series with the signal — audio current goes **through** the switch.

| Part | OK? | Notes |
|------|-----|--------|
| **DG411 / DG412** | Yes | Best audio: low Ron, ±15 V, low charge injection |
| **CD4066 / 74HC4066** | Yes | Cheaper analog switch; common in CEM3320 multimode builds. Watch Ron, supply range, distortion |
| Other bilateral quads (DG212, …) | Yes | Same role |
| Logic mux / bare GPIO (`74HC157`, …) | **No** | Wrong domain; not for audio nodes |

"Cheaper" means a cheaper **analog** switch (4066-class), not a digital logic IC.

> The DG411 is already the discharge switch on the oscillator cores, which is why
> `ENABLE_PIO_RESET_INVERT` is defined ([`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md) §2.1). Same
> part, same active-low sense — worth keeping consistent if the filter section reuses it.

---

## 6. v1 switching scope (one quad SPST)

- Stages **1–2**: switched LP ↔ HP
- Stages **3–4**: hardwired lowpass

```text
dist out → [S1 sw] → [S2 sw] → [S3 LP fixed] → [S4 LP fixed] → buffer → VCA
                ↑ resonance VCA (may need invert in BP — see below)
```

| Mode | S1 | S2 | S3 | S4 | Response |
|------|----|----|----|----|----------|
| `LP24` | LP | LP | LP | LP | 24 dB/oct lowpass |
| `BP12` | HP | HP | LP | LP | 12 dB/oct bandpass |
| `HP6_LP18` | HP | LP | LP | LP | 6 dB HP + 18 dB LP |
| *(optional)* | LP | HP | LP | LP | Extra hybrid colour |

Example 2-bit GPIO encode (each bit steers one stage's SPDT pair via `ctrl` / `!ctrl`):

| Code | Stage1 | Stage2 | Mode |
|------|--------|--------|------|
| 00 | LP | LP | `LP24` |
| 01 | HP | LP | `HP6_LP18` |
| 10 | LP | HP | optional |
| 11 | HP | HP | `BP12` |

That encode matches the `PARAM_FILTER_MODE` comment in `params_def.h`
(*"24dB, 12dB, BP, HP"*), so the enum ordering is already implied — pin it down when the applier
is written.

### Where the mode GPIOs go

**Dual MCU:** mode GPIOs on the **RP2040 aux** ([`DUAL_MCU.md`](DUAL_MCU.md)); cutoff/reso PWM
stay on the main MCU.

**Solo build:** two spare DCO GPIOs are needed, and the previously suggested trio was wrong:

| Pin | Previously suggested | Reality |
|---|---|---|
| **GP2** | "spare" | ❌ **`PW_PINS[1]`** — pulse width for voice 1 |
| **GP6** | "spare" | ✅ Free — the only genuinely unassigned GPIO, and already earmarked as a SUB candidate |
| **GP25** | "spare" | ⚠️ Pico onboard LED, **not on the header** |

With 8 oscillators the DCO has essentially **no spare GPIOs**
([`PINOUT.md`](PINOUT.md) occupancy table). A solo build needs either the aux MCU, a PCB remap,
or the mode bits folded into the existing 74HC595 chain — bits **9–15 are unused and held high**
([`WAVE_MUX.md`](WAVE_MUX.md)), which is the cheapest path if `ENABLE_WAVE_MUX` is ever enabled.

Match logic sense to the part: DG411 is normally closed when IN is low; DG412 is NO — firmware
must match.

---

## 7. Resonance

- The loop uses the chip resonance VCA from the cascade output back toward the input (datasheet
  LP arrangement as a starting point).
- In **BP** (HP then LP), the feedback path often needs an **op-amp invert** or Q collapses /
  misbehaves.
- High Q in BP can get loud; optional later: duck the input with a spare AS2164 section as
  resonance rises. v1: invert if needed, trim levels.

Taking the output after stage 2 vs stage 4 (full Synthex) needs **extra** switches — not in the
one-quad budget. Park for v2.

> `PARAM_RESONANCE_COMPENSATION` (**7**, CC 51) already exists for the resonance→amplitude
> compensation curve, applied on the Mainboard.

---

## 8. Parts (mode section only)

| Qty | Part | Role |
|----:|------|------|
| 1 | AS3320 | Four poles + resonance |
| 1 | DG411 (or 4066-class quad) | Two SPDT for stages 1–2 |
| 2 | Inverters (or 2 extra GPIO) | Complementary switch drive |
| 1 | Op-amp section | Output buffer; optional reso invert |
| — | Passives | Datasheet Rin / Rf / C; `Ree` on VEE; matched Caps (≤ 1 %) |
| 2 | GPIO | Mode select — **see §6, they are not free on the DCO** |

SSI2144 + distortion passives/ICs are separate ([`DISTORTION.md`](DISTORTION.md)).

---

## 9. What changed

| Topic | Previous revision | Current |
|---|---|---|
| `PARAM_FILTER_MODE` | 54 | **60** — 54 is `PARAM_ADSR3_ATTACK_CURVE` |
| MIDI CC | not listed | **118** |
| DCO applier | implied | **None** — id is reserved but unrouted on this board |
| Spare pins for solo build | "GP2 / GP6 / GP25" | **GP2 is `PW_PINS[1]`**; GP25 is the onboard LED; only GP6 is free |
| Dist params | not cross-referenced | `PARAM_DIST_DRIVE` 58 / `PARAM_DIST_MIX` 59 |
| Filter count | implicit | `NUM_FILTERS = 2`, `CUTOFF_PINS[2]`, `RESO_PINS[2]` |

---

## 10. Explicitly out of scope

- Pole-mixing multimode (Xpander-style math on fixed taps)
- State-variable simultaneous LP + BP + HP from one core used as two series stages around dist
- An audio matrix to swap SSI2144 and AS3320 order around distortion

---

## 11. Next steps

1. Pick the switch IC (DG411 vs 4066) for the ±12 V rail and the level into AS3320.
2. Draw the AS3320 from datasheet Figs. 1/2/3 with SPDTs on the Cap1 / Cap2 networks.
3. Breadboard `LP24` / `BP12`; confirm the resonance invert for BP.
4. **Decide which board owns the mode GPIOs** — aux RP2040, the 595 chain, or a PCB remap. The
   DCO has no spare pins.
5. Freeze the mode enum against the `PARAM_FILTER_MODE` comment ordering, add the applier on the
   owning board, and implement break-before-make.
6. Update [`PINOUT.md`](PINOUT.md) and add the id to the owning board's `paramTable[]`.
