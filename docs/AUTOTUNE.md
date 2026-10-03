# Autotune (DCO6)

Canonical algorithms and file layout: [`../_shared/docs/AUTOTUNE.md`](../_shared/docs/AUTOTUNE.md).
Operator bring-up: [`CALIBRATION_PROCEDURE.md`](CALIBRATION_PROCEDURE.md).

This page carries only what is specific to the DCO6 board.

---

## Board topology

**4 voices × 2 oscillators = 8 oscillators**, **4 PW channels**, cal-sense **GP10**.

Because `NUM_PW_CHANNELS` (4) ≠ `NUM_OSCILLATORS` (8), the helpers in
`_shared/autotune.h` take their second branch:

```c
cal_pw_channel(osc) = osc / (NUM_OSCILLATORS / NUM_PW_CHANNELS)   // = osc / 2
osc_has_pw(osc)     = (osc % (NUM_OSCILLATORS / NUM_PW_CHANNELS)) == 0   // = even osc
```

So **only even 0-based oscillators own a PW channel** — those are the ones printed as
OSC1 / OSC3 / OSC5 / OSC7 by `pio_topology_report()`, which numbers oscillators **1-based**.
That is consistent, not a bug: PW belongs to the saw core of each voice pair.

| Voice | Osc A (saw, has PW) | Osc B (triangle, no PW) | PW channel |
|-------|--------------------|--------------------------|------------|
| V0 | osc 0 | osc 1 | 0 (GP3) |
| V1 | osc 2 | osc 3 | 1 (GP2) |
| V2 | osc 4 | osc 5 | 2 (GP4) |
| V3 | osc 6 | osc 7 | 3 (GP5) |

Debug cmd **42** is still forwarded to the Mainboard.

---

## Entry point and gating

Autotune runs on **Core 1**, entered from the calibration trap at the top of `loop1()`
([`DCO6.ino`](../DCO6.ino)):

```cpp
if (__builtin_expect(calibrationFlag || calibrationVerifyRequested, 0)) {
    autotune_loop_task();
    return; // EARLY EXIT: voice_task_main() is never reached while calibrating!
}
```

There is **one** entry point — `autotune_loop_task()` (`autotune_task.ino`). It dispatches
internally to `DCO_calibration()` for the automatic sweep and `DCO_calibration_debug()` for the
manual path. Note the second condition, **`calibrationVerifyRequested`**, which also suspends
the voice task.

While either flag is set, `update_parameters()` on Core 0 drops almost every incoming
ParamId — see the calibration gating table in
[`README_serial_and_params.md`](README_serial_and_params.md). Only ids 150–162 (minus 154/155,
which are telemetry **out**) and 160 get through, and only once `manualCalibrationFlag` is set.

---

## Gap measurement constants

From `_shared/autotune_constants.h`:

| Constant | Value | Role |
|---|---|---|
| `kGapTimeoutUs` | `100000` (100 ms) | Baseline timeout |
| `kGapTimeoutMaxUs` | `500000` (500 ms) | Ceiling at ultra-low frequency |
| `kGapTimeoutPeriods` | `2.5` | Period multiple for the timeout deadline |
| `kGapTimeoutSentinel` | `1.16999f` | Value reported when the gap measurement times out |
| `kGapSamplesDefault` | `6` | Samples per gap measurement |
| `kGapSamplesHiRes` | `12` | Hi-res sample count |
| `kGapSamplesVeryLowMin` | `4` | Floor below 30 Hz |
| `kGapPeriodTolRatio` | `0.15f` | Period acceptance tolerance |
| `kGapPolarityInverted` | `false` | Set `true` if the cal pin is inverted vs the DCO output |

Gap telemetry leaves the DCO as `'x'` `PARAM_GAP_FROM_DCO` (**154**) and the stored calibration
offset echoes back as `PARAM_MANUAL_CALIBRATION_OFFSET_FROM_DCO` (**155**). Both travel
DCO → Mainboard → Input → Screen; 154 is the only one the Screen renders.

> ⚠️ **`kGapPolarityInverted = false` is worth re-checking.** The cal-sense front end inverts
> (see [`CALIBRATION_PROCEDURE.md`](CALIBRATION_PROCEDURE.md)), so `find_gap()` may be measuring
> the complement. That would explain why `AMP_TARGET_DUTY_OSC_B` needed hand-compensation
> rather than sitting at its geometric value. Change one thing at a time, and only after the
> cal bus reads clean.

---

## PW parking during calibration

`MUTE_PW_CHANNEL` is a **hardcoded literal `0`** in `_shared/autotune.h`:

```c
#define MUTE_PW_CHANNEL 0
```

It ignores both `pwSweepMode` and `PW_POLARITY_INVERTED`. Call sites
(`autotune_impl.h`) write it through `voice_write_pw(ch, MUTE_PW_CHANNEL)` with the comment
*"max voltage to drive integrator output low"*.

> ⚠️ **Structural limitation, not a tuning value.** With the PW CV clamped at one rail, the
> comparator side is fixed by its own sign, so "integrator parked low" and "pulse output parked
> low" cannot both be satisfied through the PW CV alone on an inverting comparator. Do not
> invent a second parking helper — if per-mode parking is needed again, it belongs in
> `MUTE_PW_CHANNEL` itself.

---

## Sweep and amp-comp flags that affect calibration

Set in the **CALIBRATION** block of [`settings.h`](../settings.h):

| Flag | Value in tree | Meaning |
|---|---|---|
| `PW_SWEEP_MODE_DEFAULT` | ⚠️ see below | `0` FULL (2 %…98 %), `1` HALF_HIGH (50 %…98 %), `2` HALF_LOW (2 %…50 %) |
| `PW_POLARITY_INVERTED` | `0` | PW CV polarity |
| `AMP_DUTY_INVERT_ALL` | `false` | — |
| `AMP_DUTY_INVERT_OSC_A` | `true` | Reverses the **search direction** of the duty bisection |
| `AMP_DUTY_INVERT_OSC_B` | `true` | Same, for the triangle core |
| `AMP_TARGET_DUTY_OSC_A` | `0.50f` | Saw core target |
| `AMP_TARGET_DUTY_OSC_B` | `0.62f` | Triangle core target — **not** 50 % |

The `AMP_DUTY_INVERT_*` flags reverse the direction the bisection hunts, because measured duty
moves the opposite way on this hardware. They are **not** an output inversion.

Sweep mode is selected at **compile time** through templates
(`get_PW_level_interpolated<Mode>`), never a runtime `if` on the voice hot path. In the HALF
modes the PW slider and its modulation must be **unipolar positive**, using the full sweep with
the slider at 0 (= 50 % duty).

> 🔴 **`PW_SWEEP_MODE_DEFAULT` is currently defined twice.** The
> `#ifndef` / `PROJECT_INSTRUMENT == 4` block sets it to `0` (FULL), then a later line
> redefines it to `1` (HALF_HIGH) with its `#undef` still commented out. That is a redefinition
> warning with an ambiguous final value. Decide which one is intended and either uncomment the
> `#undef` (for HALF_HIGH) or delete the trailing line (for FULL).

---

## Reset polarity

`ENABLE_PIO_RESET_INVERT` **is defined** in this tree. `pio_reset_pin_apply_polarity()`
therefore applies `GPIO_OVERRIDE_INVERT`, and PIO **logical 1 = pad low = DG411 on = cap
discharged**. Parking an inactive oscillator's reset uses logical level **1**.

Do not make that conditional on the flag — the DG411 is the discharge switch on this board
regardless of the stale comment next to the `#define` claiming DCO4 does not use it.

> **RESET pins are driven by side-set only.** `dco_triangle_init()` (Osc B / odd indices) and
> `frequency_sync_4_jumps()` (Osc A) both call `sm_config_set_sideset_pins()`;
> `sm_config_set_set_pins()` is called nowhere in the firmware. Every
> `pio_encode_set(pio_pins, x)` in the tree is therefore a **no-op** (SET pin count defaults to
> 0). To clamp a pad, use `pio_sm_set_pins_with_mask()` or
> `pio_sm_exec(pio, sm, pio_encode_nop() | pio_encode_sideset(1, level))` with the SM disabled
> first. `gpio_put()` is dead on a pad muxed to PIO.
