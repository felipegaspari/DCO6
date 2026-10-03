# DCO6 PIO Oscillators, Sync and Phase Align

How the eight oscillator cores (4 voices × 2 DCOs) are driven from PIO: the resident programs,
the state machine topology, the period arithmetic, the two sync flavours, phase align, and the
sub-oscillator.

**Live source of truth for the programs:** [`pico-dco.pio.h`](../pico-dco.pio.h) — *not* the
`.pio` source, see section 4. The triangle program and its init live in
[`state_machines.ino`](../state_machines.ino).
**Live source of truth for the wiring and roles:** [`state_machines.ino`](../state_machines.ino).
**Live source of truth for the timing constants:** the "PIO Program Timing Constants" block in
[`globals.h`](../globals.h).

Related: [`PINOUT.md`](PINOUT.md) (pin and block map),
[`../_shared/docs/AUTOTUNE.md`](../_shared/docs/AUTOTUNE.md) (how the reset pulse interacts with
gap measurement and amp-comp), [`ENGINE_OPTIONS.md`](ENGINE_OPTIONS.md) (which voice engine
computes the dividers).

---

## 1. What the PIO actually does

Each oscillator is a Juno-style DCO. A current source, set by that oscillator's RANGE PWM,
charges an integrator capacitor into a linear ramp. The PIO does exactly one job: it asserts a
**reset pulse** on the oscillator's RESET pin at precisely the right moment, which closes an
analog switch, dumps the capacitor, and starts the next ramp.

So the PIO is not synthesising a waveform. It is a very accurate one-shot timer, repeated
forever, and pitch is entirely determined by how many system clock cycles pass between reset
pulses.

```
        ramp (integrator charges)              reset
   /|                                          pulse
  / |                                         |‾‾‾|
 /  |                                         |   |
/   |_________________________________________|   |____
    <------------- one period ---------------->
```

Two consequences shape everything below:

- **Period accuracy is pitch accuracy.** A one-cycle error at 225 MHz is 4.4 ns; whether that
  matters depends entirely on how long the period is (section 5).
- **The reset pulse is dead time.** It is part of the period but contributes no ramp, so it
  affects waveform shape and amplitude, not just timing (section 2).

### 1.1 Two core types, two programs

**This board runs two different oscillator cores**, dispatched in `start_voice_sms()`:

```cpp
if (i % 2 != 0) {
  dco_triangle_init(...);            // OSC B — odd index — triangle core
} else {
  frequency_sync_poll_init(...)      // OSC A — even index — saw core (soft-sync slave)
  or frequency_sync_4_jumps(...);    //                      saw core (free / hard sync)
}
```

| | Osc A (even idx) | Osc B (odd idx) |
|---|---|---|
| Core | Sawtooth | Triangle |
| Program | `frequency_sync_4_jumps` / `frequency_sync_poll` | `dco_triangle` |
| Program length | 20 / 21–23 | **11** |
| Sideset config | `sm_config_set_sideset(&c, 2, true, false)` | `sm_config_set_sideset(&c, 1, false, false)` |
| **SET pins configured?** | **Yes** — `sm_config_set_set_pins(&c, pin, 1)` | **No** |
| Has a PW channel? | Yes (`cal_pw_channel` = `osc/2`) | No |
| `osc_uses_sync_program[]` | true only for a soft-sync slave | always false |

> 🔴 **The SET-pin asymmetry is a real trap.** `dco_triangle_init()` configures **only** the
> sideset pins. With the SET pin count left at 0, every `pio_encode_set(pio_pins, x)` aimed at
> an **odd** oscillator is a **silent no-op**. The same instruction works on even oscillators,
> because their init does call `sm_config_set_set_pins()`.
>
> The core-agnostic way to clamp a pad is `pio_sm_set_pins_with_mask()`, or
> `pio_sm_exec(pio, sm, pio_encode_nop() | pio_encode_sideset(1, level))` — raw `0xb042` for
> side 1, `0xa042` for side 0 — with the SM **disabled first**. `gpio_put()` is dead on a pad
> muxed to PIO.

---

## 2. The analog core and where `pioPulseLength` comes from

```c
uint32_t pioPulseLength = 3000;  // cycles; runtime via PARAM_DEBUG_COMMAND 160 ∈ [200, 50000]
```

`pioPulseLength` is the reset pulse width in system clock cycles, held in the state machine's
**Y** register. At 225 MHz, 3000 cycles is **~13.3 µs**. It is not persisted; the Calibration
tab in `dco_control` sends unsigned 16-bit values 200–50000 on id 160 to change it live (small
opcodes 1–30 stay below that range). Setting Y via debug 160 also requests
`pio_defer_request_reset_pulse_all()` so running SMs reload (stop / load Y / restart) and voices
resplit the period on the next note.

> The system clock is **not** a compile-time constant. `sysClock_Hz` is a macro over
> `sysClock_Hz_cached`, filled by `sys_clock_hz_refresh()` from `clock_get_hz(clk_sys)` once per
> core at the top of `setup()` / `setup1()`. Arduino's *Tools → CPU Speed* sets the real value
> before either runs. Never call `clock_get_hz()` on the voice hot path.

### 2.1 Sizing the pulse

The pulse has to fully discharge the integrator capacitor, and no longer:

| Quantity | Value | Why |
|----------|-------|-----|
| Integrator cap | 4.7 nF | Chosen with the 20 k range resistor for the target pitch range |
| Range resistor | 20 k | Sets the charging current per RANGE PWM level |
| Discharge series R | ~180 R | Limits peak switch current inside the DG411's pulsed rating |
| Discharge time constant | ~846 ns | `180 R × 4.7 nF` |
| Pulse needed for full discharge | **~7.5 µs (~1700 cycles)** | About 9 time constants; residual charge negligible |

**RESET pad polarity.** PIO programs always use logical `1` = assert / discharge and `0` = ramp.
The analog switch is a **DG411** (IN low closes the discharge path), so
`ENABLE_PIO_RESET_INVERT` **is defined** in [`settings.h`](../settings.h).
`start_voice_sms()` then calls `pio_reset_pin_apply_polarity()` on every `RESET_PINS[]` entry,
applying GPIO `OUTOVER` + `INOVER` invert so the pad is active-low while soft-sync `jmp pin`,
hard-sync sideset and the sub-oscillator `wait` all keep the same logical sense. Parking an
inactive oscillator's reset therefore uses logical level **1**.

> ⚠️ A comment next to the `#define` still claims *"DCO3 (DG411) defines this; DCO4
> (active-high / FET) does not."* That comment is stale — the flag is correctly defined here.

> **Open item.** The constant is **3000** cycles, roughly 1.8× the ~1700 the RC analysis calls
> for. See section 13 before changing it: the amp-comp calibration is measured *with* the
> current pulse width.

### 2.2 Why the pulse is a fixed absolute time, not a fraction of the period

The engine computes the ramp as "period minus pulse minus overhead", so as pitch rises the ramp
shrinks while the pulse stays put. The pulse is therefore a **growing fraction** of the cycle:

| Note | Period @225 MHz | Pulse at 3000 | Pulse at 1700 |
|------|-----------------|---------------|---------------|
| 12 Hz | 18.75 M cycles | 0.016 % | 0.009 % |
| 1 kHz | 225 k cycles | 1.3 % | 0.76 % |
| 7 kHz | 32.1 k cycles | **9.3 %** | 5.3 % |

Making the pulse proportional to the period would hold duty constant, but it would require
rewriting Y on every control frame — unsafe on a running state machine (section 6) — and it
would invalidate the amp-comp tables. Fixed is the right call; the open question is the *value*.

The absolute ceiling is `pioPulseLength + overhead` = 3012 cycles = **74.7 kHz**, so the pulse
is nowhere near limiting playable pitch. What it costs at the top of the range is ramp
amplitude, which the amp-comp tables compensate.

---

## 3. Block and pin topology

### 3.1 Each voice pair must share one PIO block

Hard sync works by pointing the **master's sideset** at the **slave's RESET pin**. Two state
machines can only drive the same pin if they are in the same PIO block, because the pin's
function select belongs to whichever block called `pio_gpio_init()` last.

`VOICE_TO_PIO[] = {0,0,0,0,1,1,1,1}` — voices 0–1 (osc 0–3) on **pio0**, voices 2–3 (osc 4–7) on
**pio1**. Each A+B pair is inside one block, which is the only property sync depends on.

> ⚠️ Earlier revisions of this page carried a DCO3-era invariant reading *"All oscillators stay
> on PIO0"* and a bench expectation of `reset pin ownership: OK (all PIO0)`. That is **wrong for
> this board** — with 8 oscillators the work is split across two blocks. The invariant that
> still holds is *"a voice pair never straddles two blocks."*

### 3.2 Block budget

| Block | Contents |
|-------|----------|
| pio0 | 4 freq SMs (osc 0–3) + resident free / poll / triangle program images |
| pio1 | 4 freq SMs (osc 4–7) + the same images |
| pio2 (RP2350 only) | `SUBOSC_PIO` — up to 4 sub-osc SMs, pins TBD |

Program offsets are tracked per block (`pio_offset_free[]`, `pio_offset_sync[]`,
`pio_offset_triangle[]`), which is why every address helper takes the oscillator index.

There are **no spare SMs** on pio0/pio1. That is why `RANGE0_PIO_DITHER_TEST` is off — dither
would need one SM per RANGE pin.

### 3.3 Pins

RESET and RANGE arrays are selected by `DCO_MCU_BOARD`; see [`PINOUT.md`](PINOUT.md).
`pio_gpio_init()` is called on each oscillator's own RESET pin, and for hard sync also on the
partner's pin being sideset. Reading a pin as input (soft sync `jmp pin`, sub-osc `wait`) needs
**no** function select change, which is what keeps soft sync from stealing pins.

---

## 4. Resident and legacy programs

| Program | Where | Loaded? | Role |
|---------|-------|:---:|------|
| `frequency_sync_4_jumps` | `pico-dco.pio.h` | ✅ | Production saw core, free-running or hard-sync |
| `frequency_sync_poll` / `_2` / `_3` | `pico-dco.pio.h` | ✅ one image | Soft-sync slave with 1/2/3 trailing polled chunks |
| `dco_triangle` | **`state_machines.ino`** | ✅ | Production triangle core (odd indices), 11 instructions |
| `subosc_div2` / `subosc_div4` | `pico-dco.pio.h` | RP2350 only | Sub-oscillator on pio2 |
| `frequency` | `pico-dco.pio.h` | ❌ | Legacy |
| `frequency_sync` | `pico-dco.pio.h` | ❌ | Legacy |
| `frequency_pulse1` | `pico-dco.pio.h` | ❌ | Legacy |
| `noise_lfsr` | `pico-dco.pio.h` | ❌ | Noise is CPU-side (`NOISE_ENGINE 2`); `dcoNoisePioBegin` is a no-op |

### 4.1 The `.pio.h` file is hand-maintained

Arduino never runs `pioasm`. The header is edited by hand and the `.pio` source has drifted —
duplicate `.program frequency`, two `init_sm_pin` signatures — so it would not assemble today.
**Keep the two in step manually**, and treat the header as authoritative.

Note that the triangle program lives in `state_machines.ino` rather than the generated header,
so a `pioasm` regeneration would not touch it.

### 4.2 `frequency_sync_4_jumps`, annotated

Offset-relative addresses, one full period:

```
  a11 set pins,1        1              ; assert reset (SET works here — see §1.1)
  a0  lp0               Y + 1          ; hold the pulse
  a1  mov x, OSR        1
  a2  set pins,0        1              ; release; ramp starts
  a3  lp1               clk_div + 1    ; chunk 1
  a4  mov x, OSR        1
  a5  lp2               clk_div + 1    ; chunk 2
  a6  mov x, OSR        1
  a7  lp3               clk_div + 1    ; chunk 3
  a8  mov x, OSR        1
  a9  loop_final        clk_div + 1    ; chunk 4  ← phase-hold target
  a10 mov x, y          1              ; reload pulse width
=> period = Y + 4*clk_div + 12
```

`jmp x-- lp` executes **X+1** times: it jumps while X is non-zero, then spends one more cycle
falling through at X == 0. All five delay loops therefore cost one cycle beyond their count —
the reason `T_LOW_OVERHEAD_CYCLES` is **10**, not 5.

### 4.3 Soft-sync poll programs

The slave's trailing chunks are replaced with a `jmp pin` + `jmp x--` pair, costing **2 cycles
per iteration** instead of 1. Each polled chunk therefore counts **twice** toward the ramp
weight. One resident image at a time; `ensure_soft_sync_program(n)` swaps it (remove + add) and
tracks which is loaded in `pio_loaded_sync_chunks`.

### 4.4 RANGE dither PWM (`range_pwm_dither`)

Off on this board (`RANGE0_PIO_DITHER_TEST` undefined). All eight RANGE pins use hardware PWM
slices with `wrap = DIV_COUNTER`. See [`PINOUT.md`](PINOUT.md) for the slice-sharing caveat on
Resonance 1.

---

## 5. The period model

```
period = Y + ramp_weight * clk_div + period_overhead
```

```c
static constexpr uint32_t T_HIGH_OVERHEAD_CYCLES = 2;
static constexpr uint32_t T_LOW_OVERHEAD_CYCLES  = 10;
static constexpr uint32_t NUM_OSR_CHUNKS         = 4;

static constexpr uint32_t PIO_RAMP_WEIGHT_BY_CHUNKS[4]     = { 4, 5, 6, 7 };
static constexpr uint32_t PIO_PERIOD_OVERHEAD_BY_CHUNKS[4] = { 12, 13, 14, 15 };
```

Index by `softSyncChunks`: `0` = free or hard sync, `1..3` = soft-sync trailing polled chunks.
`PIO_*_FREE` aliases index 0 and `PIO_*_SYNC` aliases index 1.

### 5.1 Where overhead = 12 comes from

Instruction counting on the annotated listing in §4.2: five `+1` loop fall-throughs, four
`mov x, OSR`, one `mov x, y`, two `set pins` = 12 cycles that are not `Y` and not
`4 × clk_div`. `T_HIGH_OVERHEAD_CYCLES` (2) and `T_LOW_OVERHEAD_CYCLES` (10) split the same 12
between the pulse and the ramp for the note-on splitter.

### 5.2 Two solvers, and when each applies

| Helper | Used | Behaviour |
|--------|------|-----------|
| `pio_period_split(total, w, k)` | **Note-on only** | Chooses both `clk_div` **and** `Y`; can rewrite the pulse width to absorb the remainder |
| `pio_clk_div_for_y(total, y, w, k)` | **Every control frame** | `Y` is fixed; solves the rounded `clk_div` only |

This split exists because Y cannot be written to a running SM (§6.2).

### 5.3 Resulting accuracy

`clk_div` is an integer, so the achievable period is quantised in steps of `ramp_weight` cycles
— 4 cycles free-running. At 225 MHz that is 17.8 ns, negligible at low pitch and a few cents at
the very top of the range, which the note-on splitter mops up by adjusting Y.

---

## 6. Chunk structure: update latency versus the OSR race

### 6.1 Why four chunks

The ramp is split into four equal `clk_div` counts, each preceded by `mov x, OSR`. A new
divider pushed mid-ramp is picked up at the **next chunk boundary**, so worst-case update
latency is a quarter period rather than a whole one — the difference between sluggish and
responsive modulation at low notes.

> **Do not add `pull noblock` to the chunk loop.** It would restore exactly the latency the
> chunks exist to remove.

### 6.2 The OSR race, and why Y is only written at note-on

The chunk loop re-reads the OSR every chunk. Writing Y (`out y, 31`) consumes the OSR, so a Y
write on a **running** SM races the chunk reads and can strand the oscillator with a garbage
divider for a period or more.

**Never write Y to a running state machine.** Stop it, load, restart —
`osc_load_period_stopped()` and `osc_reload_reset_pulse_all()` both do this.

### 6.3 Writing Y consumes the OSR

Every Y write must be followed by re-pushing `osc_last_clk_div[osc]`, which is why that array
exists. `start_voice_sms()` preloads Y and then re-pushes the divider for the same reason.

---

## 7. Sync modes

### 7.1 Roles

```cpp
static inline int pair_slave(int voice) {
  if (syncMode == 0) return -1;
  return (voice << 1) + (syncMode - 1);   // mode 1 → A, mode 2 → B
}
static inline int pair_master(int voice) {
  if (syncMode == 0) return -1;
  return (voice << 1) + (2 - syncMode);   // mode 1 → B, mode 2 → A
}
```

`syncMode` 0 = off, 1 = B drives A, 2 = A drives B. Both return −1 when sync is off, which is
the "no role" sentinel every caller checks.

> These were called `sync_slave_osc()` / `sync_master_osc()` in earlier revisions. The live
> names are **`pair_slave()` / `pair_master()`**, and they take a **voice** index, not an
> oscillator index.

### 7.2 The state machine index invariant

When two SMs write the same pin on the same cycle, **the higher-numbered SM wins**.
`assign_sm_mapping()` therefore permutes `VOICE_TO_SM[]` so the **slave sits below its master**
inside the block. Call it *before* `start_voice_sms()`.

### 7.3 Hard sync versus soft sync

| | Hard sync | Soft sync |
|---|---|---|
| Mechanism | Master's sideset points at the **slave's** RESET pin | Slave polls the master's pin with `jmp pin` |
| Ramp weight | 4 (free) | 5 / 6 / 7 for 1 / 2 / 3 polled chunks |
| Effect | Discharges the slave's cap; its counter keeps running | Restarts the slave's own count |
| Cost | None | Polled chunks run at half speed |

Hard sync is analog-cap-only: it dumps the integrator without restarting the slave's schedule.

**Manual calibration forces `syncMode` to 0.** Cal solo stops the partner of every pair, and a
synced slave cannot reset itself without a running master.

### 7.4 Soft-sync thresholds

Receptive window ≈ **40 % / 67 % / 86 %** of the ramp for N = 1 / 2 / 3, because polled chunks
run at half speed. Swap images with `ensure_soft_sync_program(n)` while the SMs are stopped.

---

## 8. Phase align

`oscPhaseSync` holds the slave at `loop_final` for a computed number of cycles, then releases it
so the two cores start their ramps at a chosen offset.

### 8.1 Why 0 / 90 / 180 used to be the only working offsets

The old recipe entered the ramp at a **chunk boundary**, so only quarter-period offsets were
reachable.

### 8.2 One-shot hold on `loop_final` (current)

`osc_phase_align_hold_stopped(osc, x)` preloads X, restores `clk_div`, issues `set pins, 0` and
jumps to `osc_phase_hold_target(osc)` — the address of `loop_final`, the last `jmp x--` before
flyback (9 free; 10/11/12 for poll N = 1/2/3). Y stays the real pulse width, so later cycles are
undistorted. `osc_phase_hold_x(total, deg)` computes the X preload with a Q24 multiply; a result
of 0 means "just restart".

`NOTE_RETRIG_MODE_DEFAULT` selects the note-on behaviour: **`0` EXACT_Y** (Y load + phase hold,
the default) or `1` SYNC_JMP (restart jmp only). **SYNC_JMP is 0° only** — a degree offset needs
the stopped-SM X preload that only EXACT_Y performs. Runtime cmds 26/27.

> 🔴 **The `set pins, 0` in this path is a no-op on odd oscillators.** `set0_instr` is built with
> `pio_encode_set(pio_pins, 0)`, and triangle-core SMs have no SET pins configured (§1.1). On
> even (saw) oscillators it works. If phase align is ever needed on an Osc B, that clamp has to
> become a sideset or `pio_sm_set_pins_with_mask()` write.

---

## 9. Sub-oscillator

RP2350 only. `SUBOSC_PIO = 2`, one SM per voice (SM 0–3), each waiting on that voice's **OSC A**
RESET pin (`RESET_PINS[v*2]`). `set_subosc_divide(divide)` reconfigures them; `0` stops them,
`2` and `4` select `subosc_div2` / `subosc_div4`.

The array is compiled **only** under `#if defined(PICO_RP2350)` and every entry is still
`SUBOSC_PIN_UNASSIGNED` (`0xFF`), which the init skips. **GP8 is OSC8 RESET — do not reuse
DCO3's single SUB pin.**

Waiting on a pin needs no function select change, so the sub-osc SMs do not steal RESET pins
from pio0/pio1.

---

## 10. Function reference

### 10.1 [`state_machines.ino`](../state_machines.ino)

| Function | Purpose | Called from | Preconditions |
|----------|---------|-------------|---------------|
| `pair_slave(voice)` / `pair_master(voice)` | Resolve `syncMode` into oscillator indices, or −1 | `assign_sm_mapping()`, `start_voice_sms()`, `pio_topology_report()` | none |
| `assign_sm_mapping()` | Rewrite `VOICE_TO_SM` so the slave sits below its master | `init_pio()`, `setSyncMode()` | Call **before** `start_voice_sms()` |
| `init_pio()` | Load free + poll + triangle images into pio0 and pio1 (8 freq SMs); RP2350: sub-osc programs into pio2 | `setup1()` | Boot only |
| `dco_triangle_init(pio, sm, offset, pin)` | Configure a triangle-core SM (sideset only, **no SET pins**) | `start_voice_sms()` | Odd oscillator indices |
| `ensure_soft_sync_program(n)` | Swap the resident poll image to N trailing chunks (1..3) | `init_pio()`, `start_voice_sms()` | SMs must be stopped |
| `start_voice_sms()` | Choose each SM's program and pins; apply RESET polarity; preload Y; start all eight with `pio_enable_sm_mask_in_sync()` per block | `init_pio()`, `setSyncMode()` | Safe to re-call whenever topology changes |
| `pio_reset_pin_apply_polarity(pin)` | OUTOVER+INOVER invert for `ENABLE_PIO_RESET_INVERT` | `start_voice_sms()` | RESET pins only |
| `osc_load_period_stopped(osc, y, clk_div)` | Push Y then `clk_div` (with FJOIN clear) | `start_voice_sms()`, deferred reset-pulse work | **SM must already be stopped** |
| `osc_load_periods_stopped_noclear(...)` | Dual-osc Y + clk_div, no FJOIN | Both engines' note-on EXACT_Y paths | After frame put+pull; caller disables/enables |
| `osc_phase_align_hold_stopped(osc, x)` | Preload X, restore clk_div, `set pins, 0`, jmp `loop_final` | Note-on EXACT_Y when deg ≠ 0 | **SM stopped**; see the SET caveat in §8.2 |
| `osc_reload_reset_pulse_all()` | Re-arm every oscillator's Y, preserving the running period | Deferred queue (debug 160) | Stops and restarts each SM itself |
| `pio_defer_request_reset_pulse_all()` | Queue the above from Core 0 | `apply_param_debug_command()` | — |
| `pio_defer_request_sync_mode()` | Queue a sync-topology rebuild | `apply_param_sync_mode()`, `apply_param_soft_sync()` | — |
| `pio_defer_request_subosc()` | Queue a sub-osc reconfigure | `apply_param_subosc_divide()` | — |
| `pio_defer_request_period_probe()` | Queue a bench period probe | Debug cmds 2/3 | — |
| `pio_defer_request_cal_restore()` | Queue a post-calibration restore | Autotune | — |
| **`pio_defer_service()`** | **Drain the deferred queue** | `loop1()`, before `voice_task_main()` | Core 1 only |
| `pio_topology_report()` | Print sync roles and verify each RESET pin's function select matches `VOICE_TO_PIO[]` | Debug cmd 1 | Serial up |
| `pio_period_probe(osc, clk_div)` / `_run()` | Park an oscillator at a fixed divider and print the predicted period | Debug cmds 2/3 | Disturbs the oscillator |
| `pio_probe_report_flush()` | Emit a queued probe report through the chunked output | `bench_poll_core0()` | — |
| `pio_solve_period_model(...)` | Back-solve weight and overhead from two frequency readings | Bench | Two distinct dividers, same Y |
| `set_subosc_divide(divide)` | (Re)configure per-voice sub-oscs on pio2; skip `0xFF`; 0 stops | `init_pio()`, deferred | RP2350 |
| `start_voice_sms()` helpers `pair_master`/`pair_slave` | see above | | |

> **The deferred-work layer is the safe way to touch PIO from Core 0.** Anything that must stop
> an SM is queued with a `pio_defer_request_*()` and executed by `pio_defer_service()` at the top
> of `loop1()`, before the voice task. Never stop an SM directly from Core 0.

### 10.2 [`globals.h`](../globals.h) inlines

| Inline | Returns |
|--------|---------|
| `osc_program_base(osc)` | Load offset of the program that oscillator is running |
| `osc_restart_target(osc)` | Absolute address of `mov x, y` — jump here to retrigger (10 free; 11/12/13 poll N=1/2/3) |
| `osc_phase_hold_target(osc)` | Absolute address of `loop_final` (9 free; 10/11/12 poll) |
| `osc_phase_hold_x(total, deg)` | X preload for that hold (Q24 mul/shift), or 0 → restart |
| `osc_ramp_entry_target(osc, quarters)` | **Unused** by live phase align (leftover 90° chunk entries) |
| `osc_ramp_weight(osc)` | 4, or 5/6/7 from `softSyncChunks` when the slave runs a poll program |
| `osc_period_overhead(osc)` | 12, or 13/14/15 likewise |
| `pio_period_split(total, w, k)` | Exact `{clk_div, y}` split; **note-on only** |
| `pio_clk_div_for_y(total, y, w, k)` | Rounded `clk_div` for a fixed Y; **every frame** |
| `sys_clock_hz_refresh()` | Cache `clock_get_hz(clk_sys)`; once per core at boot |

### 10.3 Per-oscillator state

| Variable | Meaning |
|----------|---------|
| `VOICE_TO_PIO[]` | `{0,0,0,0,1,1,1,1}`. Never split a voice pair across blocks (§3.1) |
| `VOICE_TO_SM[]` | `{0,1,2,3,0,1,2,3}`; permuted by `assign_sm_mapping()` |
| `osc_uses_sync_program[]` | Which resident program each SM runs; drives the weight/address helpers. Always false for odd (triangle) oscillators |
| `osc_last_y[]` | Y currently loaded; `pio_clk_div_for_y()` solves against it |
| `osc_last_clk_div[]` | Last divider pushed, so a Y write can restore it (§6.3) |
| `softSyncChunks` | 0 = hard/free; 1..3 = soft-sync trailing polled chunks |
| `pio_loaded_sync_chunks` | Which poll image is resident (1..3) |
| `subOscDivide` | 0 / 2 / 4 |
| `pioPulseLength` | Reset pulse in cycles, default 3000 |

---

## 11. Invariants

Everything here is a trap that has already bitten, or would bite the next change.

1. **A voice pair never straddles two PIO blocks.** Hard sync needs two SMs sharing one pin's
   function select. Osc 0–3 are on pio0 and osc 4–7 on pio1 — that is fine; splitting a *pair*
   is not (§3.1).
2. **Never `pio_gpio_init` an oscillator pin from another block.** It steals the pin. Reading a
   pin as input needs no function select change (§3.1, §9).
3. **Never write Y to a running state machine.** The OSR is shared with the chunk reads (§6.2).
4. **Always re-push `clk_div` after writing Y.** `out y, 31` consumes the OSR (§6.3).
5. **The slave's SM index stays below its master's.** The higher SM wins a same-cycle pin write
   (§7.2).
6. **`pio_encode_set` only works on even (saw) oscillators.** Triangle-core SMs have no SET pins
   configured — the instruction is a silent no-op there (§1.1).
7. **Touch PIO from Core 0 only through `pio_defer_request_*()`.** `pio_defer_service()` runs the
   work on Core 1 before the voice task (§10.1).
8. **Keep `.pio` and `.pio.h` in step by hand.** Arduino never runs `pioasm`, and the `.pio`
   source would not assemble today (§4.1).
9. **Do not add `pull noblock` to the chunk loop.** It would restore the update latency the
   chunks exist to eliminate (§6.1).
10. **Jumping to `loop_final` requires an explicit pad clamp first** — and on odd oscillators
    that clamp must not be `set pins, 0` (§8.2).
11. **Changing `pioPulseLength` invalidates amp-comp calibration.** It is baked into the measured
    gap tables (§2.1, §13).

---

## 12. Bench and verification

### 12.0 How to invoke these

Nothing in the firmware calls the helpers below, so on a running board they are reached through
`PARAM_DEBUG_COMMAND` (id **160**): `1` runs the topology report, `2` and `3` run period probes
at a low and a high divider. Values **200–50000** set `pioPulseLength` instead of running an
opcode. See `apply_param_debug_command()` in [`params.ino`](../params.ino).

These requests are **queued**, not executed inline — they go through `pio_defer_request_*()` and
run on Core 1 in `pio_defer_service()`.

The easiest way to send them is [`tools/dco_control`](../tools/dco_control/README.md), which has
a button for each on its Oscillators tab. It also drives `PARAM_SOFT_SYNC` and
`PARAM_SUBOSC_DIVIDE`, which have no Input-board UI, so it is the only way to exercise soft sync
and the sub-oscillator.

### 12.1 Confirm the sync topology

```c
pio_topology_report();     // debug 160 value 1
```

Expect each RESET pin to report the block from `VOICE_TO_PIO[]` — **pio0 for osc 0–3, pio1 for
osc 4–7** — and the master/slave SM ordering from §7.2 to hold. Oscillators are printed
**1-based** (OSC1..OSC8), which is why the PW-owning ones read as OSC1/3/5/7.

> Do **not** expect "all PIO0". That expectation is left over from the 3-oscillator monosynth.

Then listen: enable sync and sweep OSC1's detune. A **timbral formant sweep** means hard sync is
working. If the pitch simply tracks with no change in character, the slave is being cloned
rather than synced. Scoping the shared reset pin should show reset edges at both rates.

### 12.2 Confirm the period model

`overhead = 12` is derived by instruction counting (§5.1). To confirm on hardware, probe at two
widely separated dividers with the same Y and back-solve.

> **Run this with no note playing.** `pio_period_probe()` parks the oscillator at a fixed
> `clk_div`, but `voice_task_main()` pushes a fresh divider every frame for a held note. With all
> voices released the voice task skips the oscillator and the probe holds.

```c
pio_period_probe(0, 2000);    // read the frequency counter on the RESET pin
pio_period_probe(0, 20000);   // read it again
pio_solve_period_model(2000, hz_a, 20000, hz_b, pioPulseLength);
```

Weight should come out very close to an integer; a fractional result means the two readings used
different Y values or different programs.

### 12.3 Confirm tuning

With `DCO_DEBUG_REPORT 1` in [`voices.ino`](../voices.ino), note-on prints the target period, the
Y actually used including remainder, the divider, and the resulting frequency. Target and
generated frequency should agree to the displayed precision.

---

## 13. Open items

| Item | Detail |
|------|--------|
| **`pioPulseLength` = 3000 vs ~1700** | The RC analysis (§2.1) calls for ~7.5 µs; the constant is 13.3 µs. Reducing it would recover ramp amplitude at the top of the range, but the amp-comp tables and `find_gap` calibration were measured with 3000 — it needs a full recalibration pass, not a constant edit. |
| **SET pins on the triangle core** | `dco_triangle_init()` never calls `sm_config_set_set_pins()`, so `pio_encode_set` is dead on odd oscillators (§1.1). Either configure SET there or migrate the affected call sites to sideset / `pio_sm_set_pins_with_mask()`. |
| **`.pio` source drift** | Duplicate `.program frequency`, two `init_sm_pin` signatures (§4.1). Worth cleaning so the file could assemble again as a cross-check. |
| **Triangle program location** | `dco_triangle` lives in `state_machines.ino`, not the generated header — easy to miss when regenerating. |
| **Hard-sync listening check** | Static and runtime checks pass; the detune-sweep listening test in §12.1 has not been done on hardware. |
| **Sub-oscillator** | RP2350 pio2 SMs ready; `SUBOSC_PINS[]` still `0xFF`. Assign 4 GPIOs + mixer inputs before it is audible. |
| **Soft-sync thresholds** | N = 1/2/3 implemented via poll-program swap (§7.4). Listening comparison still open. |
| **Legacy programs** | `frequency`, `frequency_sync`, `frequency_pulse1`, `noise_lfsr` are still in the header but never loaded. Removing them frees nothing at runtime but reduces confusion. |
