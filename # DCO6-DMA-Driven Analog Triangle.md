# DCO4-REBORN: DMA-Driven Analog Triangle Waveshaping Architecture
**Subsystem Technical Specification**
**Platform:** RP2040 / RP2350 (DCO Board) ↔ Custom Analog Core
**Date:** September 2026

## 1. Abstract
This document details the architecture of the **Triangle / Waveshaper Core** for the DCO4-REBORN polyphonic synthesizer. It relies on a hybrid topology: a purely analog "2I - I" current-steering integrator controlled cycle-by-cycle by an RP2040 microcontroller. 

By leveraging the RP2040's Programmable I/O (PIO) and Direct Memory Access (DMA) hardware, the system drives the analog integrator through a 16-segment high-speed sequence. This turns a traditional single-slope triangle core into a polyphonic analog vector synthesizer capable of drawing half-wave symmetrical M/W-waves, staircases, and complex phase distortions with zero CPU overhead, zero digital audio aliasing, and atomic pitch stability.

---

## 2. Analog Hardware Architecture
The core is built per-voice using one **TL074 Quad Op-Amp** and one channel of a **DG411 Quad Analog Switch**. It operates on bipolar $\pm 12\text{V}$ power rails but is driven by 3.3V logic.

### 2.1 The Amplitude Compensation (Amp-Comp) Path
Because a triangle wave's amplitude naturally drops as frequency increases (due to less integration time), the amplitude is dynamically scaled by the RP2040:
1. **PWM Generation:** RP2040 generates a baseline PWM signal mapped to pitch.
2. **Sallen-Key LPF (Op-Amp 1):** Smooths the 3.3V PWM into a stiff, positive DC control voltage ($+V_{comp}$).
3. **Inverter (Op-Amp 2):** Inverts $+V_{comp}$ to create an exact negative mirror ($-V_{comp}$).

### 2.2 The "2I - I" Current Steering Integrator
The actual audio generation occurs in **Op-Amp 3** (The Integrator) using a math trick that requires only one digital logic pin:
*   **The Constant Pull ($-I$):** A $20\text{k}\Omega$ resistor connects $-V_{comp}$ directly to the Op-Amp's summing node (Inverting input). This constantly attempts to pull the integration capacitor up.
*   **The Switched Push ($+2I$):** A $10\text{k}\Omega$ resistor connects $+V_{comp}$ to the summing node, *but it is gated by the DG411 Analog Switch*.

**Hardware Logic Truth Table:**
*   **RP2040 Pin HIGH (Logic 1):** DG411 Switch is OPEN. Only the $-I$ current flows. The integrator ramps **UP** at rate $S$.
*   **RP2040 Pin LOW (Logic 0):** DG411 Switch is CLOSED. The $+2I$ current overpowers the $-I$ current (Net $= +I$). The integrator ramps **DOWN** at rate $-S$.

*Note: A $10\text{M}\Omega$ resistor is placed in parallel with the $2.7\text{nF}$ integration capacitor to softly center the DC offset over time.*

---

## 3. Digital Control Architecture (Firmware)
To generate shapes beyond a simple Triangle, the RP2040 rapidly toggles the DG411 switch at varying intervals within a single audio period. This requires nanosecond precision without blocking the CPU.

### 3.1 PIO State Machine (The Segment Player)
A dedicated PIO program continuously reads 32-bit segment instructions. 
*   **Bit 0:** Determines the switch state (`1` = UP, `0` = DOWN).
*   **Bits 1-31:** Dictates the duration to hold that state (in clock cycles minus instruction overhead).

```pasm
; dco_waveshaper.pio (Length: 4 instructions)
.wrap_target
    pull block          ; [1] Wait for 32-bit word from DMA
    out pins, 1         ; [1] Pop Bit 0 directly to the DG411 switch pin
    out y, 31           ; [1] Pop Bits 1-31 into the Y delay register
segment_loop:
    jmp y-- segment_loop; [1] Wait for segment duration
.wrap
```
**Total Instruction Overhead:** 4 clock cycles (`PIO_OVERHEAD`).

### 3.2 The DMA Ring Buffer
The CPU does not manually feed the PIO. A DMA channel is linked to the PIO's TX FIFO.
*   **Data Structure:** A 16-element array of 32-bit integers: `uint32_t dco_wave_buffer[16]`.
*   **Ring Mode:** The DMA channel is configured with `channel_config_set_ring(&c, false, 6)`. Because $2^6 = 64$ bytes (16 words $\times$ 4 bytes), the DMA pointer automatically wraps back to index `0` infinitely in hardware. 
*   **Result:** The CPU "bakes" 16 segments of a waveform into RAM once. The DMA/PIO combination plays it forever at the targeted frequency with 0% CPU load.

---

## 4. The Bake-On-Write Mathematics
Whenever pitch, waveform shape, or manual symmetry corrections change, the CPU recalculates the 16 segments. This is handled by `bake_waveform(voice, shape, period_cycles)`.

### 4.1 The Law of DC Balance (Half-Wave Symmetry)
To prevent the analog capacitor from drifting into the power rails, the waveform must be perfectly DC balanced. 
*   The sum of all `UP` segment cycles **must exactly equal** the sum of all `DOWN` segment cycles over the course of the period (adjusted only by intentional `symmetry_trim` to counter hardware tolerance).
*   $T_{up\_total} = (Period / 2) + trim$
*   $T_{down\_total} = (Period / 2) - trim$

### 4.2 The Accumulator Pattern (Zero Pitch Drift)
When dividing cycles to calculate complex segment timings (e.g., $15\%$ of $T_{up}$), integer truncation drops fractions of a clock cycle. Across 16 segments, this causes the fundamental frequency to artificially shorten and drift sharp.
**Solution:** The algorithm accumulates every cycle committed to the buffer. The final segment of the array (`buf[14]` for UP, `buf[15]` for DOWN) does no math—it simply subtracts the accumulator from the total budget, natively absorbing any rounding remainders.

### 4.3 Seamless Frequency Modulation
Because the DMA operates as a geometric ring, instantaneous frequency changes (like vibrato or pitch bend) do not require stopping the PIO or realigning phase. 
Overwriting `dco_wave_buffer` in RAM causes the DMA to seamlessly scale the remainder of the wave on its next segment read, preserving structural phase automatically with zero audio clicks.

---

## 5. Waveform Implementation Examples

### Example 1: Pure Triangle (Shape 0)
To prevent the analog core from acting as a frequency multiplier, the 16 segments are split into two contiguous halves: 8 continuous `UP` segments, followed by 8 continuous `DOWN` segments.

```cpp
uint32_t up_val = t_up_total / 8; 
uint32_t dn_val = t_down_total / 8;

for (int j = 0; j < 7; j++) {
    buf[j] = pack_segment(STATE_UP, up_val); 
    up_accum += up_val;
}
buf[7] = pack_segment(STATE_UP, t_up_total - up_accum);

// ... (Repeat for DOWN phase in buf[8] to buf[15])
```

### Example 2: Symmetrical M/W Folded Wave (Shape 1)
This shape draws a perfect 'M' on the positive excursion and a perfect 'W' on the negative excursion. By dividing the budget into 6 horizontal time units, the wave maintains pristine X/Y symmetry and returns exactly to 0V at the half-period array wrap.

```cpp
uint32_t min_t = PIO_OVERHEAD; // Safe 4-cycle padding
uint32_t u_unit = (t_up_total - (3 * min_t)) / 6;
uint32_t d_unit = (t_down_total - (2 * min_t)) / 6;

// Positive Half (The 'M')
buf[0] = pack_segment(STATE_UP,   u_unit); up_accum += u_unit;
buf[1] = pack_segment(STATE_DOWN, min_t);  dn_accum += min_t; // Peak 1
buf[2] = pack_segment(STATE_UP,   u_unit); up_accum += u_unit;
buf[3] = pack_segment(STATE_DOWN, d_unit); dn_accum += d_unit; // Center Fold
buf[4] = pack_segment(STATE_UP,   u_unit); up_accum += u_unit;
buf[5] = pack_segment(STATE_DOWN, d_unit); dn_accum += d_unit; // Peak 2
buf[6] = pack_segment(STATE_UP,   min_t);  up_accum += min_t;
buf[7] = pack_segment(STATE_DOWN, d_unit); dn_accum += d_unit; // Return to 0V

// ... (Negative Half mirrors this logic and absorbs remainders in buf[14]/buf[15])
```

---
**End of Specification.**
*To integrate new waveforms, ensure segments sum strictly to `t_up_total` and `t_down_total`, and assign at least `PIO_OVERHEAD` (4 cycles) to every active index to prevent hardware underflow.*


Here is **Part II** of the technical document. This section dives significantly deeper into the electrical physics, the closed-loop autotuning mechanism, phase synchronization, and the advanced geometric breakdowns of the complex waveshapes. 

You can append this directly to the first document.

***

# DCO4-REBORN: DMA-Driven Analog Triangle Waveshaping Architecture
**Part II: Advanced Systems, Calibration, and Geometric Analysis**

## 6. Closed-Loop Hardware Calibration (Autotune)
Because the analog integrator relies on physical resistors (10kΩ/20kΩ) and the internal ON-resistance of the DG411 silicon, environmental temperature changes and component tolerances will naturally introduce DC offset (symmetry error) and amplitude variance. The DCO board firmware actively measures and corrects this using a closed-loop hardware path.

### 6.1 The Calibration Comparator (Op-Amp 4)
The 4th op-amp in the voice's TL074 package is configured as a high-speed comparator.
*   **Input:** It taps the raw, DC-coupled output of the Integrator (Op-Amp 3, Pin 8) *before* the AC-coupling capacitor.
*   **Reference ($V_{ref}$):** The inverting input is tied to a fixed reference voltage (e.g., $+1.65\text{V}$, generated by a voltage divider from the 3.3V rail).
*   **Output Behavior:** Whenever the analog triangle wave rises above $+1.65\text{V}$, the op-amp output slams to the $+12\text{V}$ rail. When it falls below $+1.65\text{V}$, it slams to the $-12\text{V}$ rail.

### 6.2 Logic-Level Shifting & MCU Protection
To safely read the $\pm 12\text{V}$ comparator output without destroying the 3.3V RP2040, the signal drives the base of a **2N3904 NPN Transistor**.
*   The transistor's emitter is tied to Analog Ground.
*   The collector is tied to the RP2040 `CAL_MEASURE_PIN` (with an internal or external 10kΩ pull-up to 3.3V).
*   **Result:** The analog amplitude excursion above $V_{ref}$ is perfectly translated into a $0\text{V}$ to $3.3\text{V}$ digital LOW pulse.

### 6.3 The Two-Stage Digital Autotune Algorithm
By measuring the duty cycle of this calibration pin via RP2040 hardware timers, the firmware deduces both the symmetry and the amplitude.

1.  **Stage 1: DC Offset / Symmetry Correction**
    The firmware sets $V_{ref} = 0.0\text{V}$ (or measures between peaks). If the duty cycle is $>50.0\%$, the wave is drifting high. If it is $<50.0\%$, the wave is drifting low. 
    The CPU adjusts the `symmetry_trim` variable (adding/subtracting clock cycles to the DMA sequence) until the duty cycle is exactly $50.000\%$.
2.  **Stage 2: Amplitude Compensation**
    Once symmetry is locked, the wave is compared against $V_{ref} = +1.65\text{V}$. For a target amplitude of $5.0\text{V}_{pp}$ ($\pm 2.5\text{V}$), geometry dictates that the wave should spend exactly **17.0%** of its time above $+1.65\text{V}$.
    The firmware raises or lowers the Amp-Comp PWM duty cycle until the measurement hits $17.0\%$, then bakes that value into the voice's Q15 amplitude polynomial.

---

## 7. Deep Dive: Waveshape Geometries & Integrator Physics
When drawing complex shapes (Staircases, Notches), the PIO must command the DG411 switch to change direction *before* the wave reaches its natural peak. 

### 7.1 The Physics of Fixed Integration
The maximum slew rate ($\text{Volts per microsecond}$) is strictly fixed by the Amp-Comp CV. 
Therefore, **any waveform that includes a directional reversal (a notch or a step) inherently sacrifices vertical amplitude.** If $10\%$ of the cycle time is spent stepping backwards, the maximum peak voltage reached will be strictly $80\%$ of a pure triangle wave ($100\% - 10\% \text{ loss up} - 10\% \text{ loss down}$). 
*This prevents complex shapes from clipping the op-amps and naturally reduces their volume, which balances their increased harmonic loudness.*

### 7.2 Geometric Analysis: Symmetrical Staircase (Shape 3)
This wave produces a "bitcrushed" analog sawtooth effect. It steps up aggressively and falls back slightly, repeating 4 times on the positive half and 4 times on the negative half.
*   **Upward Step:** `up_big = 23%`, `dn_sml = 2%`. Net upward movement per step = $21\%$.
*   Across 4 steps, it uses $100\%$ of the $T_{up}$ time budget.
*   **Peak Amplitude Calculation:** $4 \times 21\% = \mathbf{84\%}$ of maximum Triangle height.
*   **Buffer Architecture:**
    ```cpp
    // Positive Excursion (4 jagged steps)
    buf[0] = STATE_UP (23%);   buf[1] = STATE_DOWN (2%);
    buf[2] = STATE_UP (23%);   buf[3] = STATE_DOWN (2%);
    buf[4] = STATE_UP (23%);   buf[5] = STATE_DOWN (2%);
    buf[6] = STATE_UP (23%);   buf[7] = STATE_DOWN (2%);
    ```

### 7.3 Geometric Analysis: Symmetrical Notched Triangle (Shape 4)
This wave cuts a sharp V-shaped notch into the rising and falling slopes, generating strong vocal formants and Casio CZ-style phase distortion.
*   **The Positive Profile:** Rises $35\%$, drops sharply for $9\%$, then resumes rising.
*   **Peak Amplitude Calculation:** Initial Rise ($+35\%$) + Notch Drop ($-9\%$) + Secondary Rise ($+15\%$) + Peak segment ($+37\%$) ... the math limits the final peak to roughly **$70\%$** of maximum amplitude.
*   **Symmetry Constraint:** To preserve 0V DC balance, the exact inverse must be mapped to indices 8 through 15 (e.g., Fall $35\%$, Rise sharply $9\%$, Fall $15\%$, etc.).

---

## 8. Hard Sync and Retrigger Phase Management
While the DMA natively handles seamless, click-free pitch transitions via geometric array locking, **Note Retriggering** and **Oscillator Hard Sync** require aggressive phase resets to align the starting voltage of the waveform.

### 8.1 The DMA/PIO Phase Dissonance
If a MIDI `Note_On` command occurs and `oscPhaseSync >= 1`, the firmware cannot simply update the array. The integrator might be physically sitting at $+2.0\text{V}$ mid-cycle. Updating the array would cause the new wave to start from $+2.0\text{V}$, ruining the transient attack.

### 8.2 The `reset_dco_dma_phase` Sequence
To force the wave back to $0\text{V}$ and restart the phase, the CPU executes a tightly coupled hardware reset sequence on the DMA controller and the PIO block:
1.  **Halt & Abort:** `pio_sm_set_enabled(false)` and `dma_channel_abort()`.
2.  **FIFO Flush:** Clears any lingering segments in the PIO TX FIFO.
3.  **PIO Program Counter Reset:** Executes `jmp pio_offset_triangle` via `pio_sm_exec`, forcing the PIO back to instruction `0` (`pull block`).
4.  **DMA Pointer Reset:** The DMA read address is forced back to `&dco_wave_buffer[voice][0]`.
5.  **DMA Transfer Count Reset:** *Crucial step.* The RP2040 DMA `trans_count` decrements constantly. If it hits 0, the channel dies. The firmware forcefully resets it to `0xFFFFFFFF` (4.2 billion) during every note-on to ensure it runs infinitely.
6.  **Resume:** The PIO and DMA are re-enabled synchronously. The analog integrator begins drawing from `buf[0]` (the start of the positive phase) immediately.

---

## 9. Mathematical Limits and Bounding
The bake-on-write math is constrained by strict hardware timing limits that must be safeguarded in firmware.

### 9.1 Minimum Segment Length (`PIO_OVERHEAD`)
The PIO state machine requires exactly 4 clock cycles to execute its loop (`pull`, `out pins`, `out y`, `jmp`). 
*   If a segment calculation yields a duration of 1, 2, or 3 cycles, the PIO will mathematically underflow the `Y` register (`cycles - PIO_OVERHEAD`), resulting in a ~8.5-second stall (an unsigned 32-bit wrap to `0xFFFFFFFD`).
*   **Firmware Safeguard:** The `pack_segment()` macro enforces a hard minimum bound: `if (cycles < 4) cycles = 4;`. This ensures structural stability at the slight cost of geometry distortion at extreme high frequencies ($>8\text{ kHz}$).

### 9.2 The Sallen-Key Slew Limit
Because the analog integrator uses $+V_{comp}$ as its speed reference, digital waveshaping is strictly confined to the time domain (modulating the switch). The CPU cannot perform instantaneous voltage jumps on $+V_{comp}$ to reshape the wave, because the Sallen-Key low-pass filter possesses a $\sim 2\text{ms}$ step response. Any audio-rate modulation of the Amp-Comp PWM will be physically integrated into a static DC average by the filter. All audio-rate waveshaping *must* be performed via the DMA time-domain sequencer.