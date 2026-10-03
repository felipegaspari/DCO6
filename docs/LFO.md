# LFO (Q15 bus and depth scales)

The DCO is the sole LFO clock. Live waves are **full-scale bipolar Q15**; musical travel is set
by **depth constants expressed directly in octaves**, baked at param time into Q24 depths.

> 🔴 **Rewritten 2026-09-16.** The depth model changed completely: the old dimensionless
> scale factors (`LFO1_PITCH_DEPTH_SCALE` 1700, `LFO2_PITCH_DEPTH_SCALE` 512,
> `DRIFT_PITCH_DEPTH_SCALE` 1000) **no longer exist**. Depths are now named octave constants,
> and the panel curve moved from an exponential converter to a branchless quadratic.
> There is also a **third LFO**. See [§8](#8-what-changed).

Code: [`LFO.h`](../LFO.h), [`LFO.ino`](../LFO.ino), bakers in [`params.ino`](../params.ino).
Library: vendored [`_build_libs/mo-lfo`](../_build_libs/mo-lfo). CV LFO→VCA/VCF scales:
[`CV_MOD_SCALES.md`](CV_MOD_SCALES.md).

---

## 1. Depth model — octaves, not scale factors

Depth ceilings are declared as **musical travel** and converted to Q24 at compile time
(`1.0 octave = 1 << 24 = 16777216`):

```cpp
static constexpr float LFO_COARSE_2_OCTAVES     = 2.0f;        // ±2 octaves
static constexpr float LFO_4_OCTAVES            = 4.0f;        // ±4 octaves
static constexpr float LFO_VIBRATO_2_SEMITONES  = 2.0f / 12.0f; // ±2 semitones

static constexpr int32_t LFO_COARSE_2_OCTAVES_Q24    = LFO_COARSE_2_OCTAVES    * 16777216.0f + 0.5f;
static constexpr int32_t LFO_4_OCTAVES_Q24           = LFO_4_OCTAVES           * 16777216.0f + 0.5f;
static constexpr int32_t LFO_VIBRATO_2_SEMITONES_Q24 = LFO_VIBRATO_2_SEMITONES * 16777216.0f + 0.5f;
```

| Constant | Travel | Used for |
|---|---|---|
| `LFO_4_OCTAVES_Q24` | ±4 oct | `PARAM_LFO1_TO_DCO` — the global LFO1→pitch depth |
| `LFO_COARSE_2_OCTAVES_Q24` | ±2 oct | Per-oscillator LFO1→OSC1/2/3 depths |
| `LFO_VIBRATO_2_SEMITONES_Q24` | ±2 st | Vibrato-range destinations |

**To retune a depth range, edit the octave constant.** The value is self-documenting — no
mental arithmetic against a 1700-vs-512 ratio.

### The panel curve is a branchless quadratic

```cpp
template <uint32_t MaxVal>
static inline float fast_lfo_depth_norm(uint32_t v) {
    uint32_t sq = v * v;
    sq = (v > 1) ? sq : 0;                        // branchless IT-block override
    constexpr float scalar = 1.0f / (float)(MaxVal * MaxVal);
    return (float)sq * scalar;
}
```

Returns strictly `0.0f .. 1.0f`. `MaxVal` is a **template parameter**, so the reciprocal is a
compile-time constant — no divide at runtime. Panel full scale differs per parameter:

| Parameter | Template | Ceiling |
|---|---|---|
| `PARAM_LFO1_TO_DCO` | `fast_lfo_depth_norm<511>` | `LFO_4_OCTAVES_Q24` |
| `PARAM_LFO1_TO_OSC1/2/3` | `fast_lfo_depth_norm<255>` | `LFO_COARSE_2_OCTAVES_Q24` |

The quadratic replaces the old `expConverterFloat(panel) / 275000`. Mid-knob feel is similar —
gentle at the bottom, steep at the top — but it costs one multiply instead of a table walk.

### Bake and apply

```cpp
// bake (params.ino, on knob/CC write)
LFO1toDCO_q24  = lfo_pitch_depth_q24(fast_lfo_depth_norm<511>(LFO1toDCOVal), LFO_4_OCTAVES_Q24);
LFO1toOSC1_q24 = lfo_pitch_depth_q24(fast_lfo_depth_norm<255>(val), LFO_COARSE_2_OCTAVES_Q24);

// helpers (LFO.h)
int32_t lfo_pitch_depth_q24(float amt_norm, int32_t max_depth_q24) {
  return (int32_t)(amt_norm * (float)max_depth_q24 + 0.5f);
}

#if defined(USE_FLOAT_VOICE_TASK)
float lfo_pitch_depth_f(float amt_norm, float max_octaves) {
  return (amt_norm * max_octaves) * (1.0f / 32768.0f);
}
#endif
```

`applyDepthQ24()` is the hot multiply, split for 32-bit safety on M0+:

```cpp
static int32_t applyDepthQ24(int16_t wave_q15, int32_t depth_q24) {
  const int32_t w  = (int32_t)wave_q15;
  const int32_t hi = depth_q24 >> 15;
  const int32_t lo = depth_q24 - (hi << 15);
  return w * hi + ((w * lo) >> 15);
}
```

The hi/lo split avoids overflowing `int32` when a ±4-octave depth meets a full-scale Q15 wave —
a plain `(w * depth) >> 15` would wrap. Do not "simplify" it.

> There is also a float variant `lfo_pitch_depth_f()` compiled only under
> `USE_FLOAT_VOICE_TASK`, returning octaves directly for the float voice path.

---

## 2. Three LFOs

| Instance | Globals | Notes |
|---|---|---|
| `LFO1_class` | `LFO1Level`, `LFO1Waveform`, `LFO1Speed`, `LFO1SpeedVal` | Global + per-osc pitch, VCA |
| `LFO2_class` | `LFO2Level`, `LFO2Waveform`, `LFO2Speed`, `LFO2SpeedVal` | Fine pitch, PW, VCF |
| **`LFO3_class`** | **`LFO3Level`, `LFO3Waveform` (default 2), `LFO3Speed`, `LFO3SpeedVal`** | **Matrix source `SRC_LFO3` (16); `PARAM_LFO3_SPEED` 211 / `PARAM_LFO3_WAVEFORM` 212; `DEST_LFO3_SPEED` 32** |
| `LFO_DRIFT_CLASS[NUM_OSCILLATORS]` | `LFO_DRIFT_LEVEL[]` | Eight per-oscillator drift LFOs |

All are constructed with `lfo(LFO_DAC_SIZE_UNUSED)` — the ctor `dacSize` is ignored on the Q15
path (`getWaveQ15` / `setAmplQ15`). Changing it must not change audio; changing the octave
constants **does**.

`LFO3()` is declared in `LFO.h` alongside `LFO1()` / `LFO2()`.

---

## 3. Live bus

| Global | Source | Domain |
|--------|--------|--------|
| `LFO1Level` | `LFO1_class.getWaveQ15()` | ±32767 (`MO_LFO_Q15_ONE`) |
| `LFO2Level` | `LFO2_class.getWaveQ15()` | same |
| `LFO3Level` | `LFO3_class.getWaveQ15()` | same |
| `LFO_DRIFT_LEVEL[i]` | `−LFO_DRIFT_CLASS[i].getWaveQ15()` | same, **negated** for Mainboard polarity |

Init uses `setAmplQ15(MO_LFO_Q15_ONE)` only — never `setAmpl` / `getWave`.

---

## 4. Pitch mailboxes

```cpp
lfo1_pitch_mod_q24[LFO1_PITCH_OSC1] = applyDepthQ24(LFO1Level, LFO1toDCO_q24 + LFO1toOSC1_q24);
lfo1_pitch_mod_q24[LFO1_PITCH_OSC2] = applyDepthQ24(LFO1Level, LFO1toDCO_q24 + LFO1toOSC2_q24);
lfo1_pitch_mod_q24[LFO1_PITCH_OSC3] = applyDepthQ24(LFO1Level, LFO1toDCO_q24 + LFO1toOSC3_q24);

lfo2_pitch_mod_q24[LFO2_PITCH_OSC2] = applyDepthQ24(LFO2Level, LFO2toOSC2_q24 + LFO2toOSC2_coarse_q24);
lfo2_pitch_mod_q24[LFO2_PITCH_OSC3] = applyDepthQ24(LFO2Level, LFO2toOSC3_q24 + LFO2toOSC3_coarse_q24);
```

**The global and per-oscillator depths are summed before the multiply**, so one
`applyDepthQ24()` covers both — `PARAM_LFO1_TO_DCO` is folded into every slot rather than
applied separately.

LFO2 has **fine + coarse** pairs per destination (`PARAM_LFO2_TO_OSC2` 16 /
`PARAM_LFO2_TO_OSC2_COARSE` 219; `PARAM_LFO2_TO_OSC3` 36 / `..._OSC3_COARSE` 220), also summed
before the multiply.

Core 1's voice task **adds** these mailboxes into the pitch sum.

---

## 5. EnvDCO (ADSR3) → pitch

```text
wave = env_dco_pitch_wave_q15(ADSR3Level_q15)   // unipolar: env; centered: (env−16384)<<1
ADSRModifier_q24 = applyDepthQ24(wave, ADSR1toDETUNE1_scale_q24)
```

`PARAM_ADSR3_MODE` (**223**) selects how the Q15 tap becomes octaves; A/D/S/R always run.

| Mode | Behaviour |
|---|---|
| **0 Unipolar** (default) | `applyDepthQ24(env, depth)` — sustain > 0 holds a pitch offset; idle (`env == 0`) is the played note |
| **1 Centered** | `applyDepthQ24((env − 16384) << 1, depth)` — mid sustain ≈ the played note; higher S holds sharp, lower S flat; idle ≈ −full, peak ≈ +full |
| **2 Inverted** | Negated tap |

> The parameter is `PARAM_ADSR3_MODE`, not `PARAM_ADSR3_PITCH_MODE`. Companions
> `PARAM_ADSR1_MODE` (224) and `PARAM_ADSR2_MODE` (225) do the same for EnvVCA and EnvVCF.

- **Env is linear Q15** (no `linToLog`). Full env ≈ `depth_q24` of travel.
- Each voice uses `ADSR3Level_q15[i]` on A+B, gated by `ADSR3ToOscSelect` (0 = A, 1 = B,
  2/4 = A+B).
- PW stays unipolar regardless of mode.
- Pitch sums use Q24 where `1 << 24` ≈ **+1 octave** — the same unit as the mod matrix and
  Character.

---

## 6. Core 0 generate, Core 1 consume

| Core | When | LFO role |
|------|------|----------|
| **0** | `LFO1()` / `LFO2()` / `LFO3()` every `loop()` iteration; `DRIFT_LFOs()` on `timer51microsFlag` (~51 µs) | `getWaveQ15` → `LFO*Level`; bake pitch via `applyDepthQ24` into `*_pitch_mod_q24[]` |
| **0** | `update_CV_outs()` every iteration | Q15 levels feed the mod matrix ingest |
| **1** | Voice task | **Add** `*_pitch_mod_q24[]` into the pitch sum |

> ⚠️ **`update_CV_outs()` and the envelopes are on Core 0**, not Core 1. Older revisions of this
> page placed the LFO consumers on Core 1 at "~100 µs". The only Core 1 consumer is the voice
> task's pitch add.

Probes: `loop0_lfo1`, `loop0_lfo2`, `loop0_drift` — see [`BENCHMARKING.md`](BENCHMARKING.md).

### LFO rate commits are throttled

`update_CV_outs()` stage E calls `lfo_commit_speed_if_changed()` for all three LFOs. A **panel
speed change commits immediately**; a **matrix-destination-only change is rate-limited to one
commit per `LFO_DEST_SLEW_US` = 201 µs**, shared across the three. Frequency comes from
`fast_exp_speed_5000(speed + dest)`. See
[`UPDATE_CV_OUTS_HOT_PATH.md`](UPDATE_CV_OUTS_HOT_PATH.md) §2E.

---

## 7. Other sinks

| Sink | Format | Notes |
|------|--------|-------|
| LFO → VCA/VCF | `−(depth << 2)` baked scale | [`CV_MOD_SCALES.md`](CV_MOD_SCALES.md) — `ENABLE_CV_OUTS` only |
| LFO2 → PW | `(q15 * LFO2toPW) >> 15` | Full ±1.0 → `LFO2toPW` counts |
| Mod matrix | Q15 pass-through | `SRC_LFO1` 1, `SRC_LFO2` 2, `SRC_LFO3` 16 — [`MOD_MATRIX.md`](MOD_MATRIX.md) |
| Matrix → LFO rate | `DEST_LFO1/2/3_SPEED` 11 / 12 / 32 | Throttled commit, §6 |
| Drift → pitch | `applyDepthQ24` with the drift scale | ⚠️ see below |

> 🔴 **The drift→pitch bake is disabled in the source.** In `params.ino`
> (`apply_param_analog_drift_amount`) the line computing `drift_pitch_scale_q24` from
> `DRIFT_PITCH_UNIT_Q24 * DRIFT_PITCH_DEPTH_SCALE` is **commented out and flagged
> `//// NEEDS FIX !!!`**. Both constants are gone from `LFO.h`. Analog drift still modulates via
> the matrix (`SRC_DRIFT` 10, `SRC_DRIFT_VOICE` 12), but the direct drift→pitch depth path is
> not live. Confirm the intended behaviour before relying on the drift amount knob for pitch.

---

## 8. What changed

| Topic | Previous revision | Current |
|---|---|---|
| Depth model | Dimensionless scales: 1700 / 512 / 1000 | **Octave constants**: `LFO_4_OCTAVES`, `LFO_COARSE_2_OCTAVES`, `LFO_VIBRATO_2_SEMITONES` |
| `LFO1_PITCH_DEPTH_SCALE` etc. | In `LFO.h` | **Removed** |
| `ADSR_PITCH_MAX_OCTAVES` (2.0) | In `LFO.h` | **Removed** |
| `ADSR_PITCH_DEPTH_PANEL_FULL` (511) | In `LFO.h` | **Removed** — panel max is a template param |
| `DRIFT_PITCH_UNIT_Q24` / `DRIFT_PITCH_DEPTH_SCALE` | In `LFO.h` | **Removed**; the drift bake is commented out, `NEEDS FIX` |
| Panel curve | `expConverterFloat(panel) / 275000` | **`fast_lfo_depth_norm<MaxVal>()`** — branchless quadratic, compile-time reciprocal |
| `lfo_pitch_depth_q24(amt, SCALE)` | scale factor | **`(amt_norm, max_depth_q24)`** — takes a Q24 ceiling |
| Float path | not present | `lfo_pitch_depth_f()` under `USE_FLOAT_VOICE_TASK` |
| LFO count | 2 + drift | **3** + drift (`LFO3_class`, `LFO3Level`, `LFO3()`) |
| LFO consumers' core | Core 1 at "~100 µs" | **Core 0**, every `loop()` iteration |
| LFO1/2 rate | "~50 µs" | Every Core 0 iteration; drift on `timer51microsFlag` |
| EnvDCO mode param | `PARAM_ADSR3_PITCH_MODE` | **`PARAM_ADSR3_MODE`** (223), 3 modes incl. Inverted |
| EnvDCO tap global | `ADSR1Level_q15` | **`ADSR3Level_q15`** |
| Rate commit | not documented | Throttled, `LFO_DEST_SLEW_US` 201 µs |

---

## 9. Call sites

| Function | Core | Role |
|----------|------|------|
| `init_LFOs` / `init_LFO1` / `init_LFO2` | 0 boot | Waveform, full-scale Q15 amp, initial Hz |
| `init_DRIFT_LFOs` / `init_DRIFT_LFO` | 0 boot | Eight drift instances |
| `LFO1` / `LFO2` / `LFO3` | 0 | Every `loop()` — refresh level + pitch mailboxes |
| `DRIFT_LFOs` | 0 | `timer51microsFlag` |
| `apply_param_lfo*` | param table | Speeds, waveforms, `*_q24` depth bake |
| `lfo_commit_speed_if_changed` | 0 | `update_CV_outs` stage E |
| Voice task | 1 | Consume `*_pitch_mod_q24[]` |

---

## 10. Related

- [`CV_MOD_SCALES.md`](CV_MOD_SCALES.md) — LFO1→VCA / LFO2→VCF scale bake
- [`MOD_MATRIX.md`](MOD_MATRIX.md) — LFO1/2/3 as Q15 sources, LFO speeds as destinations
- [`UPDATE_CV_OUTS_HOT_PATH.md`](UPDATE_CV_OUTS_HOT_PATH.md) — ingest and the rate commit
- [`CHARACTER.md`](CHARACTER.md) — the same Q24-per-octave pitch unit
