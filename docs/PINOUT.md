# DCO6 pinout — 4 voices × 2 oscillators

**Status:** Live RESET / RANGE selected by `DCO_MCU_BOARD` in
[`project_config.h`](../../project_config.h). **PW ×4** / cal match old DCO4. **Not frozen for
PCB fab.** Four SUB GPIOs are TBD (`SUBOSC_PINS[]` still `0xFF`). Cut/Res/VCA/dist/levels/
wave-mux **firmware is kept** behind `ENABLE_CV_OUTS` / `ENABLE_WAVE_MUX` (off) for later
expansion — not used on this 4×2 board (draft pins collide with RESET/RANGE).

**Platform:** RP2040 and RP2350 (WeAct / Pico / Pico 2 / WeAct RP2350). Dual-MCU:
[`DUAL_MCU.md`](DUAL_MCU.md). Live constants: [`globals.h`](../globals.h).

Related: [`MAINBOARD_ABSORPTION.md`](MAINBOARD_ABSORPTION.md),
[`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md).

---

## Live DCO outs (`DCO_MCU_BOARD`)

From [`globals.h`](../globals.h). Switch the MCU module in
[`project_config.h`](../../project_config.h). **Four** values are accepted:
`DCO_MCU_WEACT_RP2040` / `DCO_MCU_PICO` / `DCO_MCU_PICO2` / `DCO_MCU_WEACT_RP2350`.

Only osc 0/1 move between the two maps: WeAct RP2040 breaks out GPIO 29, while Pico, Pico 2 and
WeAct RP2350 use that pad as the VSYS ADC.

**WeAct RP2040** (`DCO_MCU_WEACT_RP2040`):

| Osc / voice | RESET | RANGE | PIO block / default SM | PW (per voice) |
|-------------|-------|-------|------------------------|----------------|
| V0 A (osc 0) | 29 | 28 | pio0 SM0 | GP3 |
| V0 B (osc 1) | 27 | 22 | pio0 SM1 | — |
| V1 A (osc 2) | 19 | 17 | pio0 SM2 | GP2 |
| V1 B (osc 3) | 18 | 16 | pio0 SM3 | — |
| V2 A (osc 4) | 15 | 14 | pio1 SM0 | GP4 |
| V2 B (osc 5) | 13 | 11 | pio1 SM1 | — |
| V3 A (osc 6) | 12 | 9 | pio1 SM2 | GP5 |
| V3 B (osc 7) | 8 | 7 | pio1 SM3 | — |

**Pico / Pico 2 / WeAct RP2350** (`DCO_MCU_PICO` / `DCO_MCU_PICO2` / `DCO_MCU_WEACT_RP2350`) —
osc 2–7 unchanged:

| Osc / voice | RESET | RANGE |
|-------------|-------|-------|
| V0 A (osc 0) | 28 | 26 |
| V0 B (osc 1) | 27 | 22 |
| V1–V3 (osc 2–7) | same as WeAct | same as WeAct |

`VOICE_TO_PIO[]` = `{0,0,0,0,1,1,1,1}`. A voice pair (A+B) **must share one PIO block** so
hard/soft sync can share a RESET pin. `VOICE_TO_SM[]` = `{0,1,2,3,0,1,2,3}` is mutable: within
a pair the slave takes the lower local SM so hard-sync sideset wins on a same-cycle tie.
Rewritten by `assign_sm_mapping()` whenever `syncMode` changes.

`PW_PINS[]` is length **4**: `{3, 2, 4, 5}` — one PW PWM per MIDI voice (shipping).
`PW_PIN_UNASSIGNED = 0xFF`.

| Function | GPIO | Notes |
|----------|------|-------|
| Cal sense | **10** | `DCO_calibration_pin` (old DCO4). GP6 is free. GP25 is the Pico LED — not cal. |
| WeAct KEY | **23** | `USER_KEY_PIN`: `INPUT_PULLUP`, hold = MIDI 69 / A440. WeAct RP2040 only. Not a header output. |
| Analog board-fix | **24** | `BOARD_FIX_PIN`, WeAct RP2040 only, OUT HIGH. Pico / Pico 2: GP24 is VBUS sense — not driven. |
| SMPS Power Save | **23** | `SMPS_PS_PIN` on Pico / Pico 2 / WeAct RP2350: OUT HIGH forces RT6150 PWM (less 3.3 V ripple). |
| SUB ×4 | **TBD** | RP2350 only: `SUBOSC_PIO = 2`, SM0–3, one per voice, waiting on that voice's OSC A RESET (`RESET_PINS[v*2]`). All `SUBOSC_PINS[]` = `0xFF` until assigned, and **the array only exists under `#if defined(PICO_RP2350)`**. **GP8 is OSC8 RESET — do not reuse DCO3's single SUB.** RP2040: no sub PIO. |
| Noise PIO LFSR | off | CPU `DCO_Noise` only (`NOISE_ENGINE 2` = PrimeHybridNoise). `NOISE_OUT_PIN` 2 unused; `NOISE_PIO`/`NOISE_SM` kept so `dcoNoisePioBegin` is a no-op. |
| RANGE PWM | HW slice | `RANGE0_PIO_DITHER_TEST` off (8 oscs — dither needs one SM per RANGE pin and there are none spare). |

`DCO6.ino` drives `pinMode(24, OUTPUT); digitalWrite(24, HIGH);` for every board **except**
`DCO_MCU_WEACT_RP2350`, which is guarded out.

**PIO reset pulse:** `pioPulseLength = 3000` clk_sys cycles, runtime-settable via
`PARAM_DEBUG_COMMAND` (160) with a value in **[200, 50000]** (dco_control Calibration tab).

Full programs / sync / phase align: [`PIO_OSCILLATORS.md`](PIO_OSCILLATORS.md).

---

## UART allocation (only 2 hardware UARTs)

| Role | Peripheral | Pins | Baud | Notes |
|------|------------|------|------|-------|
| **Mainboard** | HW `UART1` / `Serial2` | **GP20 TX / GP21 RX** | 2 500 000 | Classic PCB: DCO `Serial2` ↔ STM32 `Serial2`. FIFO 2048, polling off, DMA TX |
| **DIN MIDI** | HW `UART0`, raw SDK | **GP0 TX / GP1 RX** | 31 250 | 8-N-1, FIFO on, exclusive IRQ `on_midi_uart_rx`. USB MIDI stays too |
| ~~Screen (direct)~~ | *(removed)* | — | — | Gap 154 reaches Screen via Mainboard → Input → Screen. GP8 is OSC8 RESET |
| Host / bench | USB CDC | — | 2 000 000 | `ENABLE_USB_CONTROL` |

Soft bit-bang at 2.5 M is not acceptable.

DIN MIDI is initialised with the raw SDK (`uart_init`, `gpio_set_function`,
`irq_set_exclusive_handler`), **not** as an Arduino `Serial1` object. Input talks to the
Mainboard on its own Serial2 (GP4/5) and to Screen on Serial1 (GP0). See
[`MAINBOARD_REINTEGRATION.md`](MAINBOARD_REINTEGRATION.md).

> ⚠️ The comment block above the pin arrays in `globals.h` still labels GP20/21 as the
> *"Input UART"*. That is DCO3 leftover — on this wiring GP20/21 is the **Mainboard** link.

---

## Hub / CV / mux (code retained, unused on this board)

Cut/Res/VCA/dist/osc-sub levels/wave mux are **not wired** on this 4×2 project. Leave
`ENABLE_CV_OUTS` / `ENABLE_WAVE_MUX` off. The writers stay in-tree for later expansion; the
draft pins below still collide with 8-osc RESET/RANGE (not a near-term remap).

| Function | GPIO | PWM slice | Ch | Notes |
|----------|------|-----------|----|-------|
| Cutoff 0 | **15** | 7 | B | Collides with V2 A RESET |
| Cutoff 1 | **4** | 2 | A | Collides with PW V1 |
| Resonance 0 | **5** | 2 | B | Collides with PW V3 |
| Resonance 1 | **7** | 3 | B | Collides with V3 B RANGE. **Shares slice 3 with RANGE OSC2 (GP22)**, so this channel scales duty into `DIV_COUNTER` instead of using wrap 4095 — see `write_cv_pwm` |
| VCA | **11** | 5 | B | Collides with V2 B RANGE |
| Dist Drive | **9** | 4 | B | Collides with V3 A RANGE. Dual-MCU: `ENABLE_VOICE_AUX` |
| Dist Mix | **26** | 5 | A | Free on live 4×2 (WeAct); is RANGE osc 0 on Pico / Pico 2 / WeAct RP2350 |
| OSC1 level | **16** | 0 | A | Collides with V1 B RANGE |
| OSC2 level | **18** | 1 | A | Collides with V1 B RESET |
| OSC3 level | **9** / **32** | — | — | Dual-MCU (`ENABLE_VOICE_AUX`): Dist Drive **GP9**. Otherwise **GP32**, RP2350B provisional |
| Sub level | **26** / **33** | — | — | Dual-MCU: Dist Mix **GP26**. Otherwise **GP33**, RP2350B provisional |

`DIV_COUNTER_CV = 4095` is the CV PWM wrap for every channel except Resonance 1 as noted above.

| Function | GPIO | Notes |
|----------|------|-------|
| 74HC595 DATA | **12** | Collides with V3 A RESET — [`WAVE_MUX.md`](WAVE_MUX.md) |
| 74HC595 LATCH | **13** | Collides with V2 B RESET |
| 74HC595 CLK | **14** | Collides with V2 A RANGE |
| AS3320 mode | *TBD* | Dual-MCU → **RP2040**; solo-B → DCO spares — [`FILTER_ROUTING.md`](FILTER_ROUTING.md) |

**Wave mux:** 2× 74HC595 drive 3× DG411 (OSC1–3 × Saw/Pulse/Tri). Provisional bits 0–8; 9–15
unused. Active-low. Not used on this board.

**I2C level DAC dropped** — osc/sub levels would be PWM → analog level VCAs if CV is enabled
later. Soft bases still update with `ENABLE_CV_OUTS` off.

**Do not use for level PWM (when remapping):** GP2 (aliases GP18), GP6 (aliases RANGE OSC V0 B
GP22).

**GP2** — leftover `NOISE_OUT_PIN`; the noise PIO LFSR is off.

---

## Pin occupancy summary (live 4×2 only)

| GPIO | Role |
|------|------|
| 0, 1 | HW MIDI UART0 |
| 2, 3, 4, 5 | PW voices 0–3 (`PW_PINS[] = {3,2,4,5}`) |
| 6 | free (later SUB candidate) |
| 7, 9, 11, 14, 16, 17, 22, 28 | RANGE ×8 (WeAct RP2040). Pico / Pico 2 / WeAct RP2350: RANGE osc 0 is **GP26**, not GP28 |
| 8, 12, 13, 15, 18, 19, 27, 29 | RESET ×8 (WeAct RP2040). Pico / Pico 2 / WeAct RP2350: RESET osc 0 is **GP28**, not GP29 |
| 10 | Cal sense |
| 20, 21 | HW UART1 — **Mainboard** link |
| 23 | WeAct RP2040: onboard KEY (A440). Pico / Pico 2 / WeAct RP2350: SMPS PS, OUT HIGH |
| 24 | WeAct RP2040: analog board-fix OUT HIGH. Pico / Pico 2: VBUS sense, not driven. WeAct RP2350: skipped |
| 25 | Pico LED (not on header) |
| 26 | Pico / Pico 2 / WeAct RP2350: **RANGE osc 0**. WeAct RP2040: free (Dist Mix only if CV is enabled later) |
| SUB ×4 | TBD (RP2350 `SUBOSC_PIO = 2`, SM0–3) |

---

## PWM mux cautions

- Formula (SDK): slice = `(gpio >> 1) & 7` for gpio < 32; channel = `gpio & 1`.
- GPIOs that differ by **16** can alias the same slice/channel — do not use both as PWM.
- Channels on one slice share **wrap/clock**; duty is independent. This is why Resonance 1
  (GP7) has to scale into `DIV_COUNTER`: RANGE OSC2 (GP22) owns the wrap on slice 3.

---

## Feature flags (code)

Live states in [`settings.h`](../settings.h):

```text
ENABLE_CV_OUTS           // OFF — Cut/Res/VCA/dist/levels writers kept for later expansion
ENABLE_WAVE_MUX          // OFF — same; draft pins collide with 8-osc RESET/RANGE
ENABLE_VOICE_AUX         // OFF — Dist/mode on a second RP2040 when aux is used
ENABLE_PIO_RESET_INVERT  // ON  — RESET pad active-low (DG411 discharge); OUTOVER+INOVER
ENABLE_NOISE_OUT         // OFF — leftover; noise PIO LFSR is off on both MCUs
RANGE0_PIO_DITHER_TEST   // OFF — no spare SMs with 8 oscs
ENABLE_MAINBOARD_LINK    // ON  — Serial2 is the Mainboard link
ENABLE_MB_MOD_STREAM     // OFF — DCO runs LFO1/2 + envelopes + matrix locally
ENABLE_USB_CONTROL       // ON  — USB CDC bench frames
ENABLE_FS_CALIBRATION    // ON  (globals.h) — load LittleFS cal tables
```

> **`ENABLE_PIO_MIDI` does not exist.** Earlier revisions of this page listed it as a planned
> flag for DIN-on-PIO-UART; there is no such symbol anywhere in the tree. DIN MIDI is on raw
> hardware `uart0`.

`ENABLE_PIO_RESET_INVERT` **is defined** in this tree even though a nearby comment says
*"DCO3 (DG411) defines this; DCO4 (active-high / FET) does not"*. The comment is stale — the
discharge switch is a DG411, so the flag is correct as-is and PIO logical 1 = pad low = DG411
on = cap discharged.

PCM5102 I2S noise listen lives on **VOICE-AUX** (see
[`../../VOICE-AUX/docs/I2S_NOISE.md`](../../VOICE-AUX/docs/I2S_NOISE.md)).
