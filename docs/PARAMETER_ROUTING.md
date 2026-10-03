# Parameter Architecture & O(1) Routing Guide

This document describes how parameter IDs, data types, routing tables, and bidirectional state
synchronization are implemented across all hardware boards in the **DCO3** and **DCO4 / DCO6**
synthesizer ecosystems.

> **Naming note.** DCO6 is a revision of DCO4-REBORN on the same codebase. Its
> `PROJECT_INSTRUMENT` value is still `4`, and its sketch folder is `DCO6/`.

---

## 1. Core Architecture Principles

1. **Single Source of Truth:** Every logical synthesizer parameter has a unique, permanent
   numerical ID defined in `_build_libs/DCO-PROTOCOL/params_def.h` (`enum ParamId : uint16_t`).

2. **IDs must be ≤ 255 — this is structural, not stylistic.** The router is a 256-entry jump
   table (`PARAM_ROUTER_JUMP_SIZE = 256`), `param_router_build_jump()` only indexes
   `if (id < PARAM_ROUTER_JUMP_SIZE)`, `param_router_apply()` checks
   `rawId < PARAM_ROUTER_JUMP_SIZE`, and `update_parameters()` takes a **`uint8_t`** id.
   An id ≥ 256 is **dropped silently**: no compile error, no runtime error, the parameter
   simply never applies. The `uint16_t` on the enum is misleading — the routable space is
   8 bits.

3. **Zero-Allocation O(1) Jump Dispatch:** Linear scans and large `switch` statements are
   eliminated. Every MCU builds the 256-pointer lookup table on boot. Executing
   `update_parameters(id, value)` jumps directly to the applier in constant time, guaranteeing
   no audio/UI loop jitter during parameter bursts.

4. **Universal 16-bit Wire Format:** All parameter updates across all serial and USB links use
   the `'p'` frame: a 1-byte command followed by a **3-byte payload** `[id:u8][value:i16 LE]`,
   i.e. a 4-byte inner frame. The payload length constant is `SERIAL_LEN_PARAM_16 = 3`.
   Wider and narrower variants exist: `'w'` `CMD_PARAM_8` (2-byte payload) and `'x'`
   `CMD_PARAM_32` (5-byte payload).

5. **Clear Board Responsibilities:**
   * **DCO:** The DSP authority. Applies parameters to PIO clock dividers, PWM slices, LFO
     pitch scales, portamento, and LittleFS preset/calibration storage.
   * **Mainboard:** The analog authority and central router. Applies parameters to analog
     filter/VCA CVs and mixer DACs; forwards DCO-relevant parameters across UART.
   * **Input Controller:** The UI state authority. Maintains in-RAM mirrors of synthesizer
     state so physical faders, LEDs, and preset saves remain perfectly synchronized.
   * **Screen Controller:** The display authority. Formats parameters into names and values
     for LVGL widgets and pop-up toasts.

---

## 2. The O(1) Router Lifecycle (`param_router.h`)

Every board compiles the same templated router header:

```cpp
template<typename ValueT>
struct ParamDescriptorT {
  ParamId id;
  void (*apply)(ValueT value);
};
```

### Execution flow

```
1. Boot (setup()):
   paramTable[] (array of ID + function pointers)
        │
        ▼
   param_router_build_jump() ──► memsets and fills paramJump[256]

2. Runtime ('p' frame arrives):
   update_parameters(id, value)
        │
        ├─► calibration gating (see §6)
        ▼
   param_router_apply(paramJump, id, value) ──► paramJump[id](value)
        │
        ▼
   preset_shadow_capture(id, value)
```

On the DCO the jump array is `dcoParamJump[]` and the builder call lives in
`init_param_router()`, which `setup()` invokes before MIDI comes up. Unregistered ids leave a
`nullptr` slot and are skipped by the `&& jump[rawId]` guard in `param_router_apply()`.

---

## 3. Data Flow & Bidirectional Synchronization

A parameter change can originate from three places. Ingress tagging (`g_param_ingress`)
ensures that updates flow everywhere without creating infinite echo loops.

```cpp
enum ParamIngress : uint8_t {
  PARAM_SRC_MAINBOARD = 0, // Arrived from hardware UART (Serial2)
  PARAM_SRC_USB       = 1, // Arrived from Host PC CDC (Serial)
};
```

> There is **no `PARAM_SRC_INPUT`**. On classic DCO4 wiring the DCO's only peer UART is the
> Mainboard, and panel edits arrive relayed through it, so Mainboard-origin and panel-origin
> traffic share the `PARAM_SRC_MAINBOARD` tag.

```
                      ┌──────────────────────┐
                      │   Input Controller   │
                      │ (Knobs / Faders / UI)│
                      └──────────┬───────────┘
                                 │ Ingress 'p'
                                 ▼
┌─────────────┐   'p' Toasts   ┌──────────────────────┐    'p' Edit     ┌─────────────┐
│   Screen    │◄───────────────┤   STM32 Mainboard    ├────────────────►│  DCO Voice  │
│ Controller  │                │ (Analog CV / Router) │                 │   Engine    │
└─────────────┘                └──────────────────────┘                 └──────┬──────┘
                                         ▲                                     │
                                         │ Preset Load / MIDI CC / USB Mirror  │
                                         └─────────────────────────────────────┘
```

### Scenario A: User turns a physical pot/fader on the Panel

1. **Input Controller:** Encodes `'p'` and transmits to Mainboard on `Serial8`. (Also sends a
   direct UI toast to Screen if configured.)
2. **Mainboard:** `update_parameters()` applies local analog CV/DACs. If the parameter affects
   pitch or the DCO engine, `forward_dco()` transmits `'p'` to the DCO on `Serial2`.
3. **DCO:** `update_parameters()` applies changes to PIO/timers and registers state in
   `preset_shadow_capture()`. Because `g_param_ingress == PARAM_SRC_MAINBOARD`, it does
   **not** echo back to the Mainboard — the gate is `serial_forward_usb_edit_to_mb()`, which
   returns early unless the tag is `PARAM_SRC_USB`.

### Scenario B: Host sends a parameter edit over USB CDC or MIDI CC

1. **DCO:** Receives edit (`g_param_ingress == PARAM_SRC_USB`). Calls `update_parameters()` to
   update the audio engine and triggers `serial_echo_persistable_param16()`.
2. **Mainboard:** Receives the mirrored `'p'` from the DCO on `Serial2`. Applies local analog
   CVs, forwards to Input Controller (`relay_to_input()`), and forwards to Screen
   (`relay_to_screen()`).
3. **Input Controller:** `update_parameters()` updates local RAM variables so the next preset
   save captures the host edit.
4. **Screen Controller:** Displays the parameter name and value pop-up toast.

### Scenario C: Preset Recall

1. **DCO:** Loads the patch chunk from LittleFS. Restores all DSP globals and bursts the patch
   parameters as `'p'` (and blocks as `'a'`–`'d'`) over `Serial2`.
2. **Mainboard & Input & Screen:** Follow the same mirror path as Scenario B, synchronizing
   analog CVs, panel fader memories, and screen titles.

---

## 4. How to Add a New Parameter

### Step 1: Register the ID in `_build_libs/DCO-PROTOCOL/params_def.h`

```cpp
enum ParamId : uint16_t {
  // ...
  PARAM_NEW_FEATURE = 105, // Use next unused integer <= 255. Never renumber!
};
```

Free ranges as of this revision: **5–6, 61–62, 103–119, 138–149, 163–169, 175–189, 195–198,
202–209, 213, 238+**.

### Step 2: Implement on DCO (`DCO6/params.ino`)

```cpp
static void apply_param_new_feature(int16_t v) {
  myNewFeatureGlobal = v;
  // Recompute DSP / PIO if needed
}

// In paramTable[]:
{ PARAM_NEW_FEATURE, apply_param_new_feature },
```

Nothing else to register — `init_param_router()` rebuilds the jump table from
`sizeof(paramTable)`.

### Step 3: Implement on Mainboard if analog/relayed (`MAINBOARD-CONTROLLER/params.ino`)

```cpp
static void apply_param_new_feature(int16_t v) {
  // If Mainboard controls analog hardware for it:
  set_analog_hardware(v);
  // If DCO also needs it:
  forward_dco(PARAM_NEW_FEATURE, v);
}

// In paramTable[]:
{ PARAM_NEW_FEATURE, apply_param_new_feature },
```

**This step is what makes an id reachable from the panel.** Panel `'p'` is applied and
re-emitted per ParamId on the Mainboard, never relayed as raw bytes.

### Step 4: Implement on Input Controller if mirrored (`INPUT-CONTROLLER/params.ino`)

```cpp
static void apply_param_new_feature(int16_t v) {
  myNewFeatureMirror = v;
}

// In paramTable[]:
{ PARAM_NEW_FEATURE, apply_param_new_feature },
```

---

## 5. Parameter Map

| Range / IDs | Domain | Function |
|:---:|:---:|---|
| **1 – 4** | OSC1 wave enables | Saw, Pulse, Tri; 4 = deprecated Sine (kept for ID stability) |
| **7 – 9** | Mainboard VCF / VCA | Resonance compensation, EnvVCA/EnvVCF retrigger modes |
| **10 – 38** | Oscillator / Voice | EnvDCO routing, LFO1/2 waveforms, intervals, detune, phase sync, portamento, keytrack, velocity depths, mixer levels, voice/alloc mode, unison, analog drift, sync modes, sub divide |
| **34 – 36, 39, 90 – 92** | OSC3 (monosynth) | Interval, detune, LFO2 depth, legacy level alias, wave enables |
| **40 – 47** | LFO / VCA / Env→PW | LFO1→DCO, LFO1/2 speeds, `VCA_LEVEL`, LFO1→VCA, LFO2→PW, EnvDCO→PWM, EnvDCO→OSC1 pitch |
| **48 – 56** | Envelope curves | `ADSR1/2/3_{ATTACK,DECAY,RELEASE}_CURVE` |
| **57 – 59** | Filter gate / Distortion | VCF trigger mode, `DIST_DRIVE`, `DIST_MIX` |
| **60** | Filter mode | AS3320 pole configuration (24 dB, 12 dB, BP, HP) |
| **63 – 86** | Mod Matrix | 8 slots × (Source, Dest, Depth) |
| **87 – 89** | **OSC2** wave enables | Saw, Pulse, Tri |
| **93 – 101** | Sub oscillators | Sub1/Sub2 divide, master lock, phase, width, logic combinator |
| **102** | Calibration | Calibration sub-mode UI flag |
| **120 – 129** | Panel takeover | Fader/pot manual-control flags, EnvDCO enable, Shift key |
| **130 – 137** | Crossmod + tuning | XMOD depth/mode/shape/ratio/detune/symmetry; **132 = `OSC1_DETUNE_VAL`, 133 = `MASTER_TUNING`** (declared out of numeric order inside this block) |
| **150 – 162** | Calibration & telemetry | Auto-tune trigger (150), manual stages (151–153), **gap 154**, **cal offset echo 155**, store (156), voice topology UI (157), step (158), 440 Hz trim (159), **debug/bench command (160)**, duty trim (161), PW center (162) |
| **170 – 174** | Presets & dumps | Save, load, preset dump, cal table dump, preset name scroll |
| **190 – 201** | UI / Screen | Menu position (190), **toasts 191–194** (Cutoff, Res, Env→VCF, LFO2→VCF), calibration dismiss (199), calibration menu mode (200), menu mode (201) |
| **210 – 225** | Voice extras | PW (210), LFO3 speed/waveform (211–212), EnvDCO restart (214), `VCA_LEVEL_ALT` (215), LFO1→OSC1/2/3 (216–218), LFO2 coarse (219–220), **Character (221)**, **EnvVCA→VCA (222)**, envelope modes (223–225) |
| **226 – 237** | Envelope times & levels | ADSR1 (226–229), ADSR2 (230–233), ADSR3 (234–237), each × Attack/Decay/Sustain/Release |

---

## 6. Calibration Gating (DCO)

`update_parameters()` on the DCO filters aggressively while
`calibrationFlag || calibrationVerifyRequested` is set:

- **Without** `manualCalibrationFlag`: only `PARAM_CALIBRATION_FLAG` (150) and
  `PARAM_DEBUG_COMMAND` (160) are applied. Everything else returns **before** the shadow
  capture.
- **With** `manualCalibrationFlag`: only the ids in `is_calibration_parameter()` pass — 150,
  151, 152, 153, 156, 158, 159, 160, 161, 162.

A panel control that appears dead during calibration is being dropped here, not in a relay.

---

## 7. Known Issues & Open Questions

### 7.1 Two paths carry the same ADSR data

Envelope times exist **both** as blocks and as discrete ParamIds:

| Path | Wire | Goes through `update_parameters()`? | Shadow-captured? |
|---|---|---|---|
| Block | `'a'`/`'b'`/`'c'`, 8 B `AdsrBlock{a,d,s,r}` | ❌ writes globals directly | ❌ **no** |
| Discrete | `'p'` ids 226–237 | ✅ | ✅ |

Because the block path skips `preset_shadow_capture()`, a preset save can capture a different
value than the one currently sounding if the two disagree. This looks like an in-progress
migration from blocks to discrete ids. **Unresolved:** which path is authoritative, and whether
the panel should emit one or both.

### 7.2 Possible historical ID reuse

Older documentation described **OSC3 as ids 33–35, 38, 87–89** and **Dist as 52–53**. Against
the current enum, all of those now hold different parameters:

| ID | Old doc claimed | Current |
|---|---|---|
| 33 | OSC3 | `PARAM_PORTAMENTO_MODE` |
| 38 | OSC3 | `PARAM_SUBOSC_DIVIDE` |
| 87–89 | OSC3 | **OSC2** wave enables (OSC3's are 90–92) |
| 52–53 | Dist | `ADSR2_DECAY_CURVE` / `ADSR2_RELEASE_CURVE` (Dist is now 58–59) |

Either the old docs were always wrong, or ids were renumbered in violation of the cardinal
rule. The exact 87–89 → 90–92 shift suggests three ids were inserted. **If ids really were
renumbered, presets written under the old map load values into the wrong parameter.**
Verify with `gen_midi_map.py --check` before trusting archived records.

### 7.3 Numbering gaps

Ids **5–6, 61–62, 103–119, 138–149, 163–169, 175–189, 195–198, 202–209, 213** are unused.
`PARAM_SINE_STATUS` (4) is retained deprecated for ID stability. Ids 132–133 sit out of
numeric order inside the 130–137 crossmod block — harmless for the jump table, confusing to
read.

---

## Related documents

- `DCO-PROTOCOL/README.md` — protocol, DMA, framing and command bytes
- `DCO6/docs/README_serial_and_params.md` — DCO-side topology, command tables and routing
- `DCO6/docs/SYSTEM_OVERVIEW.md` — canonical board ownership and UART graph
