# Serial & Parameter Protocol — DCO (DCO6)

The wire format, the command table, the parser and the `ParamId` enum are shared by every
board and documented once in [`DCO-PROTOCOL/README.md`](../../DCO-PROTOCOL/README.md) and
[`DCO-PROTOCOL/docs/PARAMETER_ROUTING.md`](../../DCO-PROTOCOL/docs/PARAMETER_ROUTING.md).
The headers come from that library through `_build_libs/DCO-PROTOCOL`; there is no copy in
this sketch folder any more.

This page covers only what is specific to the DCO6 DCO. Topology:
[`SYSTEM_OVERVIEW.md`](SYSTEM_OVERVIEW.md). Preset protocol:
[`PRESET_STORE.md`](PRESET_STORE.md).

---

## Links

The only peer UART is `Serial2` ↔ **STM32 Mainboard**, which relays panel traffic to and from
the Input board. USB CDC (`ENABLE_USB_CONTROL`) speaks the same inner frames for
[`DCO-CONTROL-PANEL`](../../DCO-CONTROL-PANEL/README.md).

Configured in `init_serial()` ([`Serial.ino`](../Serial.ino)):

| Link | Port | Pins | Baud | Notes |
|------|------|------|------|-------|
| DIN MIDI | raw `uart0` | GP0 TX / GP1 RX | 31 250 | 8-N-1, FIFO on, exclusive IRQ `on_midi_uart_rx` |
| Mainboard | `Serial2` | GP20 TX / GP21 RX | 2 500 000 | FIFO 2048, polling off, DMA TX |
| Host / bench | USB CDC `Serial` | — | 2 000 000 | Only while `ENABLE_USB_CONTROL` |

DIN MIDI is driven as a **raw SDK `uart0`** with `uart_init()` + `irq_set_exclusive_handler()`,
not as an Arduino `Serial1` object. DMA TX is armed once with
`serial_dma_init_rp2040(0, uart1)` and every outbound frame goes through the `Serial2Dma`
`UartDmaTx` handle.

---

## Where the protocol actually lives

There is **no board-local protocol header**. [`Serial.h`](../Serial.h) pulls in five shared
headers from `_build_libs/DCO-PROTOCOL/` and then declares only this board's TX helpers:

| Shared header | Provides |
|---|---|
| `serial_input_protocol.h` | `enum SharedSerialCmd` (the **command bytes**), `SERIAL_LEN_*` payload sizes, the packed domain structs, screen-signal constants |
| `serial_param_protocol.h` | The **param-frame codec**: `ParamFrame`, `encode_param_p`/`decode_param_p` (`'p'`, 16-bit), `encode_param_w`/`decode_param_w` (`'w'`, 8-bit), `encode_param32`/`decode_param_x` (`'x'`, 32-bit), plus `encode_u16_le` / `decode_u32_le` primitives |
| `serial_frame.h` | Framing, stuffing, `SERIAL_INNER_MAX_PAYLOAD` |
| `serial_parser.h` | `SerialParserContext`, `serial_parser_drain()`, command LUTs |
| `serial_dma_tx.h` | `UartDmaTx`, `serial_dma_init_rp2040()` |

`serial_param_protocol.h` is **part of the shared library**, not board-local, and it does
**not** define the command set — it includes `serial_input_protocol.h` for the `SERIAL_LEN_*`
constants. The Mainboard ↔ DCO bytes `'n'`/`'o'`/`'e'` (DCO→MB) and `'m'`/`'t'` (MB→DCO) are
ordinary members of the shared `SharedSerialCmd` enum.

`SERIAL_INNER_MAX_PAYLOAD` is **40**, set in `serial_frame.h` (*"sized with headroom for all
domain blocks (<= 40B)"*), and it derives `SERIAL_STUFFED_MAX = 1 + 40 + 2`. The largest
payload this board actually moves is the `'B'` bulk chunk at **36** bytes.

---

## Two command tables

Both are registered at the end of `init_serial()` with `serial_command_table_init()`.

| Cmd | Constant | Len | `mainboardSerialCommands[]` (Serial2) | `usbSerialCommands[]` (USB CDC) |
|:---:|---|:---:|:---:|:---:|
| `'a'` | `CMD_ADSR1_BLOCK` | 8 | ✅ | ✅ |
| `'b'` | `CMD_ADSR2_BLOCK` | 8 | ✅ | ✅ |
| `'c'` | `CMD_ADSR3_BLOCK` | 8 | ✅ | ✅ |
| `'d'` | `CMD_FILTER_BLOCK` | 8 | ✅ | ✅ |
| `'p'` | `CMD_PARAM_16` | 3 | ✅ | ✅ |
| `'q'` | `CMD_PRESET_NAME` | 16 | ✅ | ✅ |
| `'m'` | `CMD_MOD_STREAM` | 16 | ✅ | — |
| `'t'` | `CMD_BENCH_TEXT` | 16 | ✅ | — |
| `'N'` | `CMD_PRESET_DIR_REQUEST` | 1 | ✅ | — |
| `'s'` | `CMD_SCREEN_SIGNAL` | 1 | — | ✅ |
| `'B'` | `CMD_BULK_CHUNK` | 36 | — | ✅ |
| `'C'` | `CMD_BULK_COMMIT` | 8 | — | ✅ |

They overlap but are **not** the same table: a command registered in only one is unreachable
from the other transport. Nine rows each.

`serial_usb_task()` uses a second `SerialParserContext`. Only host → DCO is framed; DCO → host
is plain debug text. `Serial2` and USB CDC both drain on Core 0 `timer1msFlag` (~1 ms), while
USB/DIN MIDI runs every `loop()`. The CDC drain is skipped when the host has not opened
`Serial`.

Screen-only commands (`'w'` `CMD_PARAM_8`, `'y'` `CMD_PARAM_NAV_BYTE`, `'k'` `CMD_CHAR_SELECT`)
never reach this board. Note that `'s'` **is** handled here, on the USB table, and is also
emitted DCO→MB by `serial_send_screen_signal_to_mb()`. The former `'e'`/`'f'` commands are now
`'p'` ids 222 / 210.

---

## Outbound frames

All TX goes through `serial_frame_write(Serial2Dma, …)`. Declared in [`Serial.h`](../Serial.h):

| Helper | Emits |
|---|---|
| `serial_send_note_on()` / `serial_send_note_off()` | `'n'` (4 B) / `'o'` (1 B) |
| `serial_send_expression()` | `'e'` (4 B) |
| `serialSendParam16()` / `serialSendParam32()` | `'p'` (3 B) / `'x'` (5 B) |
| `serial_send_adsr_vca/vcf/dco_block_to_mb()` | `'a'` / `'b'` / `'c'` (8 B, `AdsrBlock{a,d,s,r}`) |
| `serial_send_filter_block_to_mb()` | `'d'` (8 B, `FilterBlock{CUTOFF, RESONANCE, ADSR2toVCF, LFO2toVCF}`) |
| `serial_send_screen_signal_to_mb()` | `'s'` (1 B) |
| `serial_send_preset_loaded_to_mb()` / `..._scroll_to_mb()` | `'L'` (1 B) / preset scroll |
| `serial_send_patch_osc/lfo/mod/mix_block_to_mb()` | `'v'` / `'l'` / `'M'` / `'Q'` |
| `serial_send_preset_burst_to_mb()` | Full patch burst |

`'O'` `CMD_PRESET_DIR_ENTRY` (17 B) is emitted by the directory-push task. `'O'` and `'L'` are
**TX-only** on this board.

---

## Ingress tagging and echo prevention

```cpp
// Ingress tag to prevent bouncing Mainboard frames back onto Serial2
enum ParamIngress : uint8_t {
  PARAM_SRC_MAINBOARD = 0, // Arrived from hardware UART (Serial2)
  PARAM_SRC_USB       = 1, // Arrived from Host PC CDC (Serial)
};
static ParamIngress g_param_ingress = PARAM_SRC_MAINBOARD;
```

There is **no `PARAM_SRC_INPUT`**. On classic DCO4 wiring the DCO's only peer UART is the
Mainboard; panel edits arrive relayed through it, so Mainboard-origin and panel-origin traffic
share one tag.

`serial_panel_task()` sets `PARAM_SRC_MAINBOARD` before draining `Serial2`; `serial_usb_task()`
sets `PARAM_SRC_USB`. The single forwarding gate is:

```cpp
// Forward to Mainboard ONLY if the command originated from USB Host
static void serial_forward_usb_edit_to_mb(char cmd, const uint8_t* payload, uint8_t len) {
  if (g_param_ingress != PARAM_SRC_USB) return;
  serial_frame_write(Serial2Dma, (uint8_t)cmd, payload, len);
}
```

Called from the four block handlers (`'a'`/`'b'`/`'c'`/`'d'`), so a host edit reaches the
Mainboard — which applies the analog ones and relays all of them to Input — while a
Mainboard-origin block is never echoed back.

---

## Board-specific frame handling

- `'a'`/`'b'`/`'c'`/`'d'` write the ADSR and filter block globals **directly** and set dirty
  flags. They do **not** go through `update_parameters()`, and therefore do **not** hit
  `preset_shadow_capture()`.
- `'p'` is both ingress and the outbound persistable mirror (the mirror covers USB/MIDI edits
  only, never Serial2 ingress).
- `'q'` stages the name for the next preset save.
- `'O'`/`'L'` are TX-only.
- Patch bursts `'v'`/`'l'`/`'Q'`/`'M'` are TX-only.

> ⚠️ **Open design question — two paths for the same ADSR data.**
> `params_def.h` defines discrete ParamIds **226–237**
> (`PARAM_ADSR1/2/3_{ATTACK,DECAY,SUSTAIN,RELEASE}`), and the DCO routes all twelve with real
> appliers in `paramTable[]`. So an envelope time can arrive either as an `'a'`/`'b'`/`'c'`
> block *or* as a `'p'` frame. The block path skips the shadow capture, the ParamId path does
> not — which means a preset save can capture a different value than the one you hear if the
> two disagree. This looks like an in-progress migration from blocks to discrete ids; it is
> **not yet settled** which path is authoritative or whether the panel should emit both.

---

## MIDI CC

`midi_cc_apply()` writes the ADSR/filter block globals directly (`CC_LOCAL_*`). Everything
else, including `PARAM_PW_VALUE` (210) and `PARAM_ADSR1_TO_VCA` (222), goes through
`update_parameters()`. Persistable ParamId CCs also call
`serial_echo_persistable_param16()` so the panel display, and the next preset save, match what
you hear. The `'a'`–`'d'` domains are mirrored by their own block helpers instead — including
`'c'`, whose engine is DCO-local but whose faders live on the panel.

Full map: [`MIDI_CC_MAP.md`](MIDI_CC_MAP.md).

---

## Parameter routing

`update_parameters(uint8_t id, int16_t value)` does **not** scan `paramTable[]`. The table is
a descriptor array compiled into an **O(1) jump table at boot**:

```
paramTable[]  →  param_router_build_jump()   [in init_param_router(), called from setup()]
              →  dcoParamJump[PARAM_ROUTER_JUMP_SIZE]      // 256 entries
              →  param_router_apply()        [from update_parameters()]
              →  apply_param_*()
```

Every accepted write then calls `preset_shadow_capture(id, value)`.

**IDs must be ≤ 255.** `param_router_apply()` checks `rawId < PARAM_ROUTER_JUMP_SIZE` and the
function signature truncates to `uint8_t`, so an id ≥ 256 is dropped silently — no compile
error, no runtime error. `ParamId` being `uint16_t` is misleading.

### Calibration gating

While `calibrationFlag || calibrationVerifyRequested` is set, `update_parameters()` filters
hard ([`params.ino`](../params.ino)):

- **Without** `manualCalibrationFlag`: only `PARAM_CALIBRATION_FLAG` and
  `PARAM_DEBUG_COMMAND` (160) are applied; everything else returns early **before** the shadow
  capture.
- **With** `manualCalibrationFlag`: only the ids in `is_calibration_parameter()` pass —
  `PARAM_CALIBRATION_FLAG` (150), `PARAM_MANUAL_CALIBRATION_FLAG` (151),
  `PARAM_MANUAL_CALIBRATION_STAGE` (152), `PARAM_MANUAL_CALIBRATION_OFFSET` (153),
  `PARAM_MANUAL_CALIBRATION_STORE` (156), `PARAM_MANUAL_CALIBRATION_STEP` (158),
  `PARAM_AMP_COMP_440` (159), `PARAM_AMP_COMP_DUTY_OFFSET` (161),
  `PARAM_CAL_PW_CENTER` (162), `PARAM_DEBUG_COMMAND` (160).

If a panel control appears dead during calibration, this is why — not a relay problem.

---

## Adding a parameter here

Generic steps: [shared guide](../../DCO-PROTOCOL/docs/PARAMETER_ROUTING.md). Two extra steps
apply on this instrument:

1. Implement `apply_param_*`, add a row `{ PARAM_<NAME>, apply_param_<name> }` to
   `paramTable[]` in [`params.ino`](../params.ino), and rely on `init_param_router()` already
   being called from `setup()`. The jump table is rebuilt from `sizeof(paramTable)`, so there
   is nothing else to register.
2. **For the id to be reachable from the panel, add an applier row to the Mainboard's
   `paramTable[]` that calls `forward_dco()`.** Panel `'p'` is applied and re-emitted per
   ParamId there, not relayed as raw bytes.

If the parameter should survive a preset save, confirm it is reachable through
`update_parameters()` — values written directly by a block handler are not shadow-captured.

## Adding a serial command here

- Add the byte to `enum SharedSerialCmd` and its `SERIAL_LEN_*` constant in the shared
  `serial_input_protocol.h`. Keep the payload ≤ `SERIAL_INNER_MAX_PAYLOAD` (40).
- Implement the handler and add the `SerialCommandDef` row to the right table in
  [`Serial.ino`](../Serial.ino): `mainboardSerialCommands[]` for Serial2,
  **`usbSerialCommands[]`** for USB CDC, or both.
- **Register it in the Mainboard relay too** — `inputSerial8Commands[]` for Input→DCO and
  `mainSerial2Commands[]` for DCO→Input, in
  [`../../MAINBOARD-CONTROLLER/Serial.ino`](../../MAINBOARD-CONTROLLER/Serial.ino). Nothing
  crosses that board implicitly; an unregistered byte is dropped in transit with no error at
  either end.
