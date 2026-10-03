# Calibration procedure (DCO6)

Canonical operator workflow:
[`../_shared/docs/CALIBRATION_PROCEDURE.md`](../_shared/docs/CALIBRATION_PROCEDURE.md).
Algorithms: [`../_shared/docs/AUTOTUNE.md`](../_shared/docs/AUTOTUNE.md).
Board-specific autotune notes: [`AUTOTUNE.md`](AUTOTUNE.md).

This board: oscillators **0..7**, four PW channels (`cal_pw_channel` = `osc / 2`), cal-sense
**GP10**.

---

## The calibration sense path

No ADC is involved. The whole measurement is one shared digital line.

```
osc raw wave ──► comparator ──► NPN (~2N3904) ──┐
   (×8)          (Vref +1.65V)   10k base       │
                                                ├──► DCO_calibration_pin (GP10)
                       all 8 collectors tied ───┘         + pull-up to 3.3 V
```

1. Each oscillator's **raw** wave feeds a comparator whose inverting input sits at a fixed
   **Vref = +1.65 V**. Saw cores swing roughly 0 → +5 V; triangle cores roughly −2.5 → +2.5 V.
2. The comparator slams to ±12 V and drives an NPN base through 10 k, emitter to AGND.
3. **All eight collectors share `DCO_calibration_pin`**, which has a pull-up to +3.3 V — both
   the internal `INPUT_PULLUP` and a **physical resistor** on the board.

That makes the bus an **open-collector wired-NOR**: any single transistor conducting pulls the
whole line to GND and calibration fails. Seven oscillators must be parked below Vref before the
eighth can be measured.

Amplitude is recovered **geometrically from the duty cycle**, not from a voltage reading:

| Core | Peak | Fraction of period above Vref |
|------|------|-------------------------------|
| Saw (even index) | +5 V | ≈ **67 %** |
| Triangle (odd index) | +2.5 V | ≈ **17 %** |

The pin is read as a plain digital GPIO — `gpio_get()` / `digitalRead()` inside `find_gap()`
and `cal_sense_probe_log()` (`_shared/autotune_impl.h`). GP10 is funcsel 5 (SIO), direction IN,
and collides with nothing in the RESET / RANGE / PW arrays.

---

## Parking is asymmetric — one level is wrong

Because the two cores idle at different voltages, a uniform park level cannot work. Each core
must be driven **below +1.65 V**:

| Core | Park action | Result |
|------|-------------|--------|
| Saw (even idx) | Discharge switch **ON** (PIO logical 1) | Flatlines at 0 V |
| Triangle (odd idx) | Polarity switch **CLOSED** (logical 0) | Flatlines at −12 V |

Triangle parking was confirmed working. **The saw side stays stuck HIGH** — that is the
symptom to chase first when the bus will not go quiet.

> Reset polarity context: `ENABLE_PIO_RESET_INVERT` is defined, so PIO logical 1 = pad low =
> DG411 on = cap discharged. See [`AUTOTUNE.md`](AUTOTUNE.md).

---

## Known issue — Vref on the sense node

**Status as of 2026-09-16 — confirm before relying on this.**

Roughly **1.65 V (= VDD/2)** has been observed sitting on the circuit node that feeds
`DCO_calibration_pin`. That is squarely inside the RP2350 input's forbidden zone, and it
matches the reported symptoms:

- no waveshape visible on the scope at the pin
- most edges rejected by `find_gap()`
- `GAP_TIMEOUT` (sentinel `1.16999f`)

This is on the **cal sense node**, not on the PW CV — the two are separate problems.

Related constant: `kGapPolarityInverted = false` (`_shared/autotune_constants.h`). Since the
NPN stage **inverts**, that setting is suspect: it would make `find_gap()` measure the
complement, which is the most likely reason `AMP_TARGET_DUTY_OSC_B` ended up hand-compensated
instead of sitting near its geometric 0.17. **Fix the bus first, then change one variable at a
time.**

---

## Runtime controls

| ParamId | Name | Role |
|:---:|---|---|
| 150 | `PARAM_CALIBRATION_FLAG` | Auto-calibration trigger (0 = off, 1..3 = mode) |
| 151 | `PARAM_MANUAL_CALIBRATION_FLAG` | Manual calibration active state |
| 152 | `PARAM_MANUAL_CALIBRATION_STAGE` | Stage index |
| 153 | `PARAM_MANUAL_CALIBRATION_OFFSET` | Fine trim offset |
| 156 | `PARAM_MANUAL_CALIBRATION_STORE` | Commit calibration to LittleFS |
| 158 | `PARAM_MANUAL_CALIBRATION_STEP` | Step override (0 = low, 1 = 440 Hz) |
| 159 | `PARAM_AMP_COMP_440` | Amp-comp value at 440 Hz |
| 160 | `PARAM_DEBUG_COMMAND` | Low-level probe / bench trigger |
| 161 | `PARAM_AMP_COMP_DUTY_OFFSET` | Per-oscillator duty trim |
| 162 | `PARAM_CAL_PW_CENTER` | PW center trim point (0..1023) |

Telemetry **out** of the DCO: `'x'` id **154** (`PARAM_GAP_FROM_DCO`) and id **155**
(`PARAM_MANUAL_CALIBRATION_OFFSET_FROM_DCO`), routed DCO → Mainboard → Input → Screen.

**Only these ids reach the engine while calibrating.** `update_parameters()` drops everything
else — without `manualCalibrationFlag` set, only 150 and 160 pass at all. A panel control that
seems dead during calibration is being filtered here, not lost in a relay.

### Useful debug commands (`PARAM_DEBUG_COMMAND`, id 160)

| Value | Effect |
|:---:|---|
| **30** | Force-overwrite the calibration tables (`seed_fake_calibration_tables`) |
| **42** | Forwarded to the Mainboard |
| **160** | Set `pioPulseLength`, value in **[200, 50000]** clk_sys cycles (default 3000) |
| 20–22 | Select amp-comp method (FLOAT_QUAD / LUT / FIXED) |
| 24–25 | Amp-comp speed / accuracy one-shots (needs `AMP_COMP_BENCHMARK`) |

---

## Storage

`ENABLE_FS_CALIBRATION` is **on** (`globals.h`), so `init_FS()` loads the LittleFS
`voiceTables` and PW calibration into the amp-comp arrays at boot, on Core 1.

`seed_fake_calibration_tables(false)` runs **before** `init_FS()` in `setup1()` and only
creates tables if the file is missing. Pass `true` — or send debug command 30 — to force an
overwrite.

The DCO owns a **512 KB LittleFS partition** (`flash=4194304_524288` in the build FQBN) shared
between the 256-slot preset store and the calibration files. **Changing the FS size reformats
it** — back up calibration and presets with `tools/dco_control` first.

---

## Bring-up order

1. **Get the bus clean.** With all oscillators parked, GP10 must read HIGH. If it does not,
   stop — every gap measurement downstream is meaningless. Chase the saw-core parking first.
2. Confirm the scope shows a real waveshape at the sense node, not a static ~1.65 V.
3. Run automatic calibration (`PARAM_CALIBRATION_FLAG` = 1..3) and watch the 154 gap telemetry
   on the Screen.
4. Only once gaps are stable, revisit `kGapPolarityInverted` and the
   `AMP_TARGET_DUTY_OSC_B` value — and change **one** at a time.
5. Commit with `PARAM_MANUAL_CALIBRATION_STORE` (156).
