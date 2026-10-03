# Modulation matrix — DCO6

Sparse, **polyphonic**, control-rate mod matrix. Panel bases stay on ParamIds / `'d'` blocks;
each tick sums the active slots **per voice** onto hardware CVs and pitch deltas.

> 🔴 **This page was rewritten on 2026-09-16.** The previous revision described the **DCO3**
> matrix: a different source enum, a different destination enum, a global (mono) sum, and
> function names that no longer exist. Every ID in the old tables was wrong for this board.
> A migration table is in [§8](#8-what-changed-from-the-dco3-matrix).

**Dual-bus policy:** LFO1 and LFO2 remain on their fixed depth params (`PARAM_LFO1_TO_DCO`,
`PARAM_LFO2_TO_PW`, …) *and* are available as matrix sources. Matrix routing is independent and
additive.

Related: [`CV_MOD_SCALES.md`](CV_MOD_SCALES.md),
[`UPDATE_CV_OUTS_HOT_PATH.md`](UPDATE_CV_OUTS_HOT_PATH.md), [`LFO.md`](LFO.md),
[`PINOUT.md`](PINOUT.md).

---

## 1. Model

```text
slot: source × dest × depth
contribution = (src_q15 * depth) >> 15      // src_q15: ±32767 ≈ ±1.0
dest_sum[voice][dest] = Σ contributions
hw[d] = clamp(panel_base[d] ± dest_sum[v][d])
```

Eight slots (`MOD_SLOT_COUNT`). A slot is inactive when its source is `SRC_OFF` (**0**).

Depth is bipolar `int16`. The accumulate core is integer **Q15 MAC**; only the pitch
destinations are converted afterwards, to float or Q24 depending on the compiled voice engine.

**The sum is per voice.** `mod_matrix_accumulate_all(&sources, NUM_VOICES_TOTAL)` evaluates all
eight slots for all four voices in one call, so a per-voice source (envelope, velocity,
keytrack, per-voice drift, S&H) produces a genuinely different contribution on each voice.

---

## 2. Sources (`enum ModSrc`, `params_def.h`)

| ID | Name | Range | Per-voice? | Notes |
|---:|------|-------|:---:|-------|
| 0 | `SRC_OFF` | — | — | Slot disabled |
| 1 | `SRC_LFO1` | ±32767 | no | `LFO1Level`, mo-lfo `getWaveQ15()` |
| 2 | `SRC_LFO2` | ±32767 | no | `LFO2Level` |
| 3 | `SRC_ENV_VCA` | 0..32767 | **yes** | `ADSR_VCA_Level_q15[v]` |
| 4 | `SRC_ENV_VCF` | 0..32767 | **yes** | `ADSR_VCF_Level_q15[v]` |
| 5 | `SRC_ENV_DCO` | 0..32767 | **yes** | `ADSR3Level_q15[v]` |
| 6 | `SRC_MODWHEEL` | 0..32767 | no | MIDI CC 1 |
| 7 | `SRC_AFTERTC` | 0..32767 | no | Channel aftertouch |
| 8 | `SRC_VELOCITY` | 0..32767 | **yes** | `velocity[v]` |
| 9 | `SRC_BEND` | ±32767 | no | `midi_pitch_bend − 8192` |
| 10 | `SRC_DRIFT` | ±32767 | no | Global drift tap, `LFO_DRIFT_LEVEL[0]` |
| 11 | `SRC_KEYTRACK` | ±32767 | **yes** | From note 60; idle voice falls back to 60 |
| 12 | `SRC_DRIFT_VOICE` | ±32767 | **yes** | Decorrelated per-voice drift, `LFO_DRIFT_LEVEL[v*2]` |
| 13 | `SRC_RANDOM_SH` | ±32767 | **yes** | Sample & hold on note strike |
| 14 | `SRC_VOICE_ID` | ±32767 | **yes** | Static voice spread offset |
| 15 | `SRC_NOISE` | ±32767 | no | Continuous noise generator |
| 16 | `SRC_LFO3` | ±32767 | no | LFO3 tap |
| 17 | `SRC_EXPRESSION` | 0..32767 | no | MIDI CC 11 |
| 18 | `SRC_BREATH` | 0..32767 | no | MIDI CC 2 |
| | `MOD_SRC_COUNT` | | | Sentinel |

Unipolar sources (envelopes, velocity, wheels, pedals) run `0..32767`; bipolar sources run
`±32767`. A negative depth inverts either.

---

## 3. Destinations (`enum ModDest`, `params_def.h`)

| ID | Name | Domain |
|---:|------|--------|
| 0 | `DEST_PITCH` | Master DCO pitch, **both** oscillators |
| 1 | `DEST_VCF_CUTOFF` | Cutoff CV offset (`VCF_PWM`) |
| 2 | `DEST_OSC1_LEVEL` | MCP4728 ch A |
| 3 | `DEST_OSC2_LEVEL` | MCP4728 ch B |
| 4 | `DEST_SUB_LEVEL` | MCP4728 ch C |
| 5 | `DEST_DIST_DRIVE` | Distortion drive CV |
| 6 | `DEST_DIST_MIX` | Distortion dry/wet CV |
| 7 | **`DEST_VCA_LEVEL`** | **Master VCA CV offset (`VCA_PWM`)** |
| 8 | `DEST_VCF_RESO` | Resonance CV (`RESONANCE_PWM`) |
| 9 | `DEST_ENV_TO_VCF` | Dynamic scaler for EnvVCF → cutoff depth |
| 10 | `DEST_ENV_TO_VCA` | Dynamic scaler for EnvVCA → volume depth |
| 11 | `DEST_LFO1_SPEED` | LFO1 rate offset |
| 12 | `DEST_LFO2_SPEED` | LFO2 rate offset |
| 13 | `DEST_LFO1_DEPTH` | Scaler for LFO1 intensity |
| 14 | `DEST_LFO2_DEPTH` | Scaler for LFO2 intensity |
| — | *(15 unused)* | Gap in the enum |
| 16 | `DEST_PW` | Pulse width offset |
| 17 | `DEST_ENV_VCF_ATTACK` | EnvVCF attack offset (ms) |
| 18 | `DEST_ENV_VCF_DECAY` | EnvVCF decay offset (ms) |
| 19 | `DEST_ENV_VCA_ATTACK` | EnvVCA attack offset (ms) |
| 20 | `DEST_ENV_VCA_DECAY` | EnvVCA decay offset (ms) |
| 21 | `DEST_ENV_ALL_TIME` | Master time scale, all envelope phases |
| 22–29 | `DEST_MOD_SLOT0..7_DEPTH` | **Matrix modulating its own slot depths** |
| 30 | `DEST_OSC1_PITCH` | Individual OSC1 pitch (Q24) |
| 31 | `DEST_OSC2_PITCH` | Individual OSC2 pitch (Q24) |
| 32 | `DEST_LFO3_SPEED` | LFO3 rate offset |
| 33 | `DEST_CROSSMOD_DEPTH` | Crossmod depth (0..32767) |
| | `MOD_DEST_COUNT` | Sentinel |

Three structural changes worth calling out:

- 🔴 **VCA is now a destination** (`DEST_VCA_LEVEL` = 7). The old rule *"never a matrix
  destination: main VCA"* **no longer holds**.
- **Slot depths are destinations** (22–29), so one slot can modulate another's depth.
  Evaluation is single-pass, so a slot's depth reflects the **previous** tick's sum — fine for
  control-rate movement, not a feedback loop.
- **Per-oscillator pitch** (30, 31) exists alongside master pitch (0). `DEST_PITCH` hits both
  oscillators; 30/31 offset one each, on top.

**ID 15 is a hole.** Do not reuse it without checking the host tooling.

---

## 4. ParamIds (63–86)

Per slot `i` (0..7):

| Field | ParamId |
|-------|---------|
| source | `63 + 3*i` |
| dest | `64 + 3*i` |
| depth | `65 + 3*i` |

So slot 0 is 63/64/65 and slot 7 is 84/85/86. Names `PARAM_MOD_SLOT0_SOURCE` …
`PARAM_MOD_SLOT7_DEPTH`.

> ⚠️ **Not 60–83.** The previous revision documented that range; **60 is `PARAM_FILTER_MODE`**
> and 61–62 are unused. Writing a slot source to id 60 changes the AS3320 pole configuration
> instead.

Appliers `mod_matrix_set_source()` / `_set_dest()` / `_set_depth()` are reached from
`paramTable[]` like any other parameter.

---

## 5. Runtime — where it actually runs

**On Core 0, inside `update_CV_outs()`** ([`cv_out.ino`](../cv_out.ino)), every `loop()`
iteration — *not* on the voice task, and not at "~10 kHz with ADSR".

The pass is staged, with a bench probe per stage:

| Stage | Probe | What happens |
|---|---|---|
| **B. Ingest** | `cv_ingest` | Fill a `sources` struct: global taps (LFO1/2, drift global, bend, expression, breath) once; then a 4-iteration unrolled loop for the per-voice taps (`drift_voice`, `env_vca`, `env_dco`, `env_vcf`, `velocity`, `keytrack_note`) |
| **C. Accumulate** | `cv_matrix` | `mod_matrix_accumulate_all(&sources, NUM_VOICES_TOTAL)` — the polyphonic MAC core |
| **D. Extract** | `cv_deltas` | Per voice, pull the destinations the voice task needs, then **`__dmb()`** |
| **E. LFO commit** | — | Compare `DEST_LFO1/2/3_SPEED` and the panel speed values against their last committed values; reprogram only on change |

### The float / fixed split

```cpp
#if defined(USE_FLOAT_VOICE_TASK)
  matrix_pitch_mod_f[i]      = mod_matrix_get_dest_float<DEST_PITCH>(i);
  matrix_osc1_pitch_mod_f[i] = mod_matrix_get_dest_float<DEST_OSC1_PITCH>(i);
  matrix_osc2_pitch_mod_f[i] = mod_matrix_get_dest_float<DEST_OSC2_PITCH>(i);
#else
  matrix_pitch_mod_q24[i]      = mod_matrix_get_dest_fast<DEST_PITCH>(i);
  matrix_osc1_pitch_mod_q24[i] = mod_matrix_get_dest_fast<DEST_OSC1_PITCH>(i);
  matrix_osc2_pitch_mod_q24[i] = mod_matrix_get_dest_fast<DEST_OSC2_PITCH>(i);
#endif
  matrix_pw_mod[i]   = mod_matrix_get_dest_fast<DEST_PW>(i);
  matrix_xmod_mod[i] = mod_matrix_get_dest_fast<DEST_CROSSMOD_DEPTH>(i);
```

The getters are **templates on the destination id**, so the index is a compile-time constant and
the compiler emits a direct offset — no runtime lookup on the hot path. `..._float` returns
octaves (`1.0f` = one octave); `..._fast` returns the Q24 bit-shift form.

PW and crossmod always use the integer getter regardless of voice engine.

### The memory barrier matters

`__dmb()` after the extract loop is what makes the hand-off to Core 1 safe: it guarantees the
voice task sees a fully written set of deltas rather than a half-updated mix. Do not remove it
when refactoring.

### Under `ENABLE_MB_MOD_STREAM`

The flag is **off** in this tree, so the matrix runs locally. When it is on, the Mainboard's
`'m'` frames overwrite `matrix_pitch_mod_q24[v]` in `Serial.ino` instead.

---

## 6. Engine API (`_shared/mod_matrix_engine.h`)

| Function | Role |
|---|---|
| `mod_matrix_init()` | Clear slots and sums; called from `setup1()` |
| `mod_matrix_set_source(slot, src)` | Applier for `PARAM_MOD_SLOTn_SOURCE` |
| `mod_matrix_set_dest(slot, dest)` | Applier for `PARAM_MOD_SLOTn_DEST` |
| `mod_matrix_set_depth(slot, depth)` | Applier for `PARAM_MOD_SLOTn_DEPTH` |
| `mod_matrix_accumulate_all(srcs, nVoices)` | Evaluate all slots × all voices |
| `mod_matrix_get_dest_fast<DEST>(voice)` | Integer / Q24 read, template on destination |
| `mod_matrix_get_dest_float<DEST>(voice)` | Float read (octaves for pitch) |

`USE_MOD_MATRIX_FLOAT_ENGINE` (board default **on** for RP2350, off for RP2040) selects the
float evaluation path inside the engine.

> ❌ `mod_matrix_accumulate()`, `mod_matrix_apply_cv()`, `mod_matrix_eval_pitch_q24()` and
> `mod_matrix_apply_dist()` **do not exist**. There is also **no `mod_matrix.ino`** in the
> sketch — only `mod_matrix.h`, which is board glue over the shared engine header.

---

## 7. Calibration gating

`update_CV_outs()` has a separate `update_CV_outs_manual_calibration()` path, and the matrix is
skipped while manual calibration is active — a moving CV would corrupt every gap measurement.

---

## 8. What changed from the DCO3 matrix

| Topic | DCO3 revision | DCO6 |
|---|---|---|
| Source numbering | ADSR3=0, ADSR4=1, LFO3=2, LFO4=3, Velocity=4, Keytrack=5, Random=6, AT=7, LFO1=8, LFO2=9, Bend=10, Wheel=11, Noise0..3=12..15 | **Completely renumbered** — see §2. `SRC_OFF`=0, LFO1=1, LFO2=2, envelopes 3–5, … |
| Empty slot marker | `0xFF` on source or dest | **`SRC_OFF` = 0** |
| Destination numbering | OSC1/2/3 level 0–2, Sub 3, Reso 4–5, Dist 6/8, Cutoff 7, Pitch 9 | **Completely renumbered** — Pitch=0, Cutoff=1, levels 2–4, … |
| VCA as destination | *"Never a matrix destination"* | **`DEST_VCA_LEVEL` = 7 exists** |
| Slot ParamIds | 60–83 | **63–86** |
| Sum scope | Global / mono | **Per voice** (`NUM_VOICES_TOTAL`) |
| Entry point | `mod_matrix_accumulate()` + `mod_matrix_apply_cv()` | `mod_matrix_accumulate_all()` + templated getters |
| Pitch latch | `dest_sums[9]` → `matrix_pitch_mod_q24` | `mod_matrix_get_dest_float/fast<DEST_PITCH>(v)` per voice |
| Per-osc pitch | not available | `DEST_OSC1_PITCH` 30 / `DEST_OSC2_PITCH` 31 |
| Depth-of-slot modulation | not available | `DEST_MOD_SLOT0..7_DEPTH` 22–29 |
| Envelope time destinations | not available | 17–21 |
| Implementation file | `mod_matrix.ino` | `_shared/mod_matrix_engine.h` + `mod_matrix.h` glue |
| Noise sources | Noise 0–3 (two stubs) | One `SRC_NOISE` (15) |
| OSC3 level dest | present | absent — this board has no OSC3 analog |

---

## 9. Code map

| File | Role |
|------|------|
| [`_shared/mod_matrix_engine.h`](../_shared/mod_matrix_engine.h) | Slots, MAC core, templated getters |
| [`mod_matrix.h`](../mod_matrix.h) | Board glue, `mod_matrix_init()` wiring |
| [`cv_out.ino`](../cv_out.ino) | Ingest → accumulate → extract → `__dmb()`; LFO speed commit |
| [`voices.ino`](../voices.ino) / [`voice_task_backup.ino`](../voice_task_backup.ino) | Consume `matrix_*_mod_f[]` / `_q24[]` in the pitch sum |
| [`params.ino`](../params.ino) | `mod_matrix_set_*` appliers in `paramTable[]` |
| [`PWM.ino`](../PWM.ino) | `write_level_pwm_raw`, `RESONANCE_PWM[]` |
| [`midi.ino`](../midi.ino) | CC 1 mod wheel, CC 2 breath, CC 11 expression, aftertouch, note-on S&H |
| [`_build_libs/DCO-PROTOCOL/params_def.h`](../_build_libs/DCO-PROTOCOL/params_def.h) | `ModSrc`, `ModDest`, slot ParamIds |
