# Preset storage

The 256-slot preset store on the DCO board, and the host protocol that reads and
writes both presets and calibration. This is a **shared contract**, not shared
code: `preset_store.h` / `preset_store.ino` live in each sketch, but the record
format, the chunk addressing and every wire command below are the same on
DCO3-MONOSYNTH and DCO4-REBORN. Most functions in the two files are byte-identical.

Change anything here and both boards plus the host codec must change together.

Related pages:

- [`FILESYSTEM.md`](FILESYSTEM.md) — the LittleFS partition both halves share.
- [`CALIBRATION_STORAGE.md`](CALIBRATION_STORAGE.md) — the seven calibration banks.
- Per-board deltas: `DCO/docs/PRESET_STORE.md` in each project (see the last
  section here).

Host UI is `DCO-CONTROL-PANEL` (model-aware, shared by both projects).

---

## 1. What is in a preset

| Domain | How it is captured |
|---|---|
| Persistable `'p'` ParamIds | `update_parameters()` → `preset_shadow_capture()` keeps `presetParamShadow[]` + a set-bitmap |
| EnvVCA / EnvVCF / EnvDCO times (`'a'`/`'b'`/`'c'`) | Read from the ADSR globals at save time |
| Filter block (`'d'`) | `CUTOFF`, `RESONANCE`, `ADSR2toVCF`, `LFO2toVCF` |
| Name | Last `'q'` frame, 16 chars, copied verbatim into the record |

**Not in a preset:** calibration tables, autotune / store-cal pulses,
`PARAM_DEBUG_COMMAND`, PIO pulse length, Character diagnostic jitters, and the
other DCO-local bench ids.

`preset_param_is_persistable()` in `preset_store.h` is the single filter, used both
for shadow capture and for `serial_echo_persistable_param16()` (the USB/MIDI →
Serial2 mirror). It is deliberately a whitelist: command, calibration and UI ids
(150-160, 170-173, …) are excluded, so no host command can ever be captured into a
patch. It is also the one part of `preset_store.h` that differs per board — DCO3
adds the sub-oscillator range 90-99.

Values are captured, not read back from hardware, so a preset saves what was last
commanded even for a parameter whose effect is smoothed or deferred.

---

## 2. Slot addressing

256 slots are packed **4 records per chunk file**, `pb00` … `pb63`:

```
chunk  = slot >> 2
offset = (slot & 3) * 598
```

An all-zero record fails the `0xA5` magic check and reads back as an unused slot,
so a freshly created chunk needs no separate "empty" marker. Why four per file, and
how the in-place write stays inside one LittleFS block, is in
[`FILESYSTEM.md`](FILESYSTEM.md).

`pstLast` (1 byte) holds the last saved or loaded slot for boot recall.

---

## 3. The record: 598 bytes, little-endian

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | Magic `0xA5` |
| 1 | 1 | Version `1` |
| 2 | 16 | Name, ASCII, zero-padded |
| 18 | 32 | Param set-bitmap — bit *id* set means that ParamId was captured |
| 50 | 512 | 256 × `int16` ParamId values |
| 562 | 32 | 16 × `uint16` block fields |
| 594 | 4 | CRC32 over bytes `[0 .. 594)` |
| | **598** | `PRESET_RECORD_SIZE` |

The bitmap is what makes a record forward-compatible: a slot saved by an older
firmware simply has bits clear for ids it never knew, and on load those parameters
keep their current value instead of being zeroed. The params area is a flat
256-entry array indexed by ParamId, so ids must never be renumbered.

**Block field order** (wire / exp domain for A, D, R):

```
0-3    EnvVCA  A D S R          ('a')
4-7    EnvVCF  A D S R          ('b')
8-11   EnvDCO  A D S R          ('c' → ADSR1_*)
12-15  CUTOFF RESONANCE ADSR2toVCF LFO2toVCF   ('d')
```

On load, only bitmap-set persistable params are replayed through
`update_parameters()`; the blocks write globals plus dirty flags. Which blocks are
also mirrored to the Serial2 peer is board-specific.

CRC32 is the IEEE / zlib-compatible polynomial, computed by `preset_crc32()` from
a 16-entry nibble table, and it is streamable (seed `0xFFFFFFFF`, update per chunk,
final xor) so the same routine serves the record, dumps and bulk commits.

---

## 4. Host ↔ board protocol

### Commands (host or panel → DCO, slim inner frames)

| Path | Meaning |
|---|---|
| `'p'` `PARAM_PRESET_SAVE` (170) = slot | Snapshot live state → chunk file, update `pstLast` |
| `'p'` `PARAM_PRESET_LOAD` (171) = slot | Recall slot (also MIDI PC + Bank Select, §6) |
| `'p'` `PARAM_PRESET_DUMP` (172) = −1 | Directory listing (`[pdir]` lines) |
| `'p'` `PARAM_PRESET_DUMP` (172) = 0..255 | Hex dump of that slot's record |
| `'p'` `PARAM_CAL_DUMP` (173) = 0 or −1 | Dump **all seven** calibration banks |
| `'p'` `PARAM_CAL_DUMP` (173) = 1..7 | Dump one bank (`CAL_DUMP_*`) |
| `'q'` | 16-char name, sent before SAVE (board stores `presetName[16]`) |
| `'B'` | Bulk chunk: `[target][slot][offset:u16 LE][32 data]` → staging RAM |
| `'C'` | Bulk commit: `[target][slot][size:u16 LE][crc32 LE]` → verify + LittleFS write |

`SERIAL_INNER_MAX_PAYLOAD` is **36** on the DCO (`Serial.h`), which is exactly what
`'B'` needs: 1 + 1 + 2 + 32. A bulk preset commit still writes one 598-byte record —
the host protocol does not know about chunks; the board maps `slot` into the right
chunk and offset itself.

`PRESET_BULK_STAGING_SIZE` is per-board, sized to the largest bulk target, which is
the `voiceTables` bank rather than the preset record.

### Bulk targets

`PresetBulkTarget`, the first payload byte of `'B'` and `'C'`:

| | Target |
|---:|---|
| 0 | one preset slot record |
| 1 | `voiceTables` |
| 2 | `PWCenter` |
| 3 | `PWHighLimit` |
| 4 | `PWLowLimit` |
| 5 | `ManualOffset` |
| 6 | `AmpComp440` |
| 7 | `AmpCompDutyOffset` |

Targets 1-7 match `CAL_DUMP_*` 1-7, so a dump selector and a restore target use the
same numbering. A calibration commit calls the shared `write_fs_bank()` and then
`init_FS()` to reload, plus an amp-comp precompute for `voiceTables`.

**Calibration dump sizes are clamped, not raw file sizes.** `dump_fs_file()` always
sends the compile-time bank size (`FSBankSize` / `FSPWBankSize` /
`FSManualOffsetBankSize` / `FSAmpComp440BankSize` / `FSAmpCompDutyOffsetBankSize`,
per target) — the same leading bytes `init_FS()` reads at boot, never the LittleFS
file's actual length. A cal file on flash can be longer (a leftover from a build
with a different `NUM_OSCILLATORS`); the extra trailing bytes are unused and are not
sent. A file *shorter* than expected aborts the dump with `reason=short`. On the
host, `fileformats.decode_cal_table()` truncates an over-length payload with a
warning and raises on under-length.

### Directory sync (`'N'`/`'O'`/`'L'`)

The Input board has no filesystem. It keeps a RAM-only `presetDir[256][16]` name
cache and refills it from the DCO, whose slots are the system-wide source of truth:

| Path | Direction | Payload | Meaning |
|---|---|---|---|
| `'N'` | Input → DCO | 1 unused/padding byte | "Send me the whole directory" |
| `'O'` | DCO → Input | `[slot:u8][name:16]` | One entry; 256 per `'N'`, blank name = unused slot |
| `'L'` | DCO → Input | `[slot:u8]` | "I just finished loading this slot" |

Whether those frames travel directly over Serial2 or are relayed by the Mainboard is
board-specific.

`'N'`'s payload is 1 byte, not 0, even though the byte carries no information:
`serial_parser_dispatch()` / `serial_parser_process_byte()` treat `payload_len == 0`
as "unregistered command" in both RAW and COBS framing, so a true zero-length frame
can never dispatch.

`preset_store_send_directory_to_mb()` answers `'N'`. Either way the work is the
same: open each of the 64 chunk files once, read the four record heads for their
names, emit four `'O'` frames per chunk — 64 opens for 256 slots. A record head that
fails the magic check is sent as an all-zero name, and the function returns
immediately if Serial2 TX is not ready (a USB-only bench with no peer attached).
It runs once per Input boot or browse-mode entry, never per encoder tick.

Whether those 256 frames go out in one blocking burst or are paced from `loop()`
is board-specific, and depends on how much the link between the DCO and Input can
absorb at once.

`serial_send_preset_loaded_to_mb()` fires `'L'` once at the end of every successful
`preset_store_load()`, so the panel follows the DCO's actual current slot even for
loads it did not trigger — boot recall, MIDI program change, or a host command. It
is a notification, not an acknowledgement; there is no retry.

These three frames are the panel's path. The host never uses them: it asks with
`'p'` `PARAM_PRESET_DUMP` = −1 and reads the `[pdir]` text instead. Which command
table each frame is registered in — and therefore whether it is reachable over USB
at all — differs per board.

### Answers (DCO → host, plain USB CDC text)

```
[pdir] begin
[pdir] slot=005 name="BassPluck"
[pdir] end count=1

[dump] begin target=preset slot=5 size=598
[dump] d 0000 A501...
[dump] end target=preset crc=XXXXXXXX

[dump] begin target=voiceTables size=<FSBankSize>
...
[dump] err target=ManualOffset reason=missing|open|short

[preset] saved slot=3 name="MyName          "
[preset] loaded slot=3 name="MyName          "
[preset] err slot=4 reason=empty|corrupt|open|write|seek|nospace

[bulk] ok target=preset slot=9
[bulk] ok target=PWCenter
[bulk] err target=preset reason=crc|size|record|open|write|seek|nospace|target
```

Answers are structured text rather than binary so the same stream is both
host-parseable and readable in a serial monitor. `size=` on a `voiceTables` dump is
that board's `FSBankSize` (528 on DCO3, 1408 on DCO4).

---

## 5. Recall paths

| Trigger | Code path |
|---|---|
| MIDI Bank Select + Program Change | CC 0 or 32 latches `midiPresetBank` (0/1); PC → `preset_store_load(bank*128 + program)` |
| `'p'` 171 | `apply_param_preset_load()` → `preset_store_load()` |
| Boot | `preset_store_boot_task()` from `loop()` after `PRESET_BOOT_RECALL_MS` (~1.5 s), once, if `pstLast` exists and `!calibrationFlag` |

All of them end in `preset_store_load()`, so all of them emit `'L'` towards Input.

MIDI Program Change alone addresses slots 0..127 (bank 0); a nonzero CC 0 or CC 32
selects bank 1 (slots 128..255). Either bank CC is accepted for controller
compatibility — there are only two banks.

Boot recall is deferred ~1.5 s so both cores and the peer links are up before
parameters start moving, and it is skipped entirely while `calibrationFlag` is set
so a calibration run is never disturbed by a patch load. With no `pstLast`, firmware
defaults stay. Recall needs a successful 1-byte read: slot 255 is a valid value and
there is no `0xFF` "empty" sentinel.

Everything runs on **Core 0** (serial / MIDI / boot one-shot). Flash writes briefly
stall the other core, exactly as the calibration FS writers do.

---

## 6. Host file formats

`DCO-CONTROL-PANEL/fileformats.py` writes tagged JSON so the three kinds cannot be
mixed up. `<model>` is the active profile key from `models.py` — `dco3` or `dco4`:

| `"format"` | Contents |
|---|---|
| `<model>-patch` | One slot: `name`, `params`, `blocks` (linear ADSR fader domain) |
| `<model>-bank` | A whole 256-slot bank (`version`, `current`, `slots`) |
| `<model>-cal` | Decoded tables: `amp_comp`, `pw_center`, `pw_high_limit`, `pw_low_limit`, `manual_offset`, `amp_comp_440`, `amp_comp_duty` |

Patch and bank files share one param numbering across both synths, so either
model's file loads anywhere — parameters the other synth lacks fall back to defaults
through `presets.normalize_bank()`. **Calibration files are strictly per-model**,
because the table sizes follow the oscillator and PW channel counts.

The host ↔ MCU record codec converts ADSR A/D/R between the UI's linear 0..4095
fader domain and the exp wire domain 0..25000 (`lin_to_exp` / `exp_to_lin`).

---

## 7. Per-board deltas

| | DCO3-MONOSYNTH | DCO4-REBORN |
|---|---|---|
| `PRESET_BULK_STAGING_SIZE` | 640 (record 598 > `voiceTables` 528) | 1440 (`voiceTables` 1408 > record 598) |
| Extra persistable range | 90-99 sub-oscillator params | — |
| Serial2 peer | Input, directly | STM32 Mainboard, which relays to Input |
| Command tables | One LUT serves USB and Serial2 | Separate USB and Serial2 LUTs |
| `'B'`/`'C'` reachable on | Both links | USB only |
| Host format tag | `dco3-*` | `dco4-*` |

Full detail, including DCO4's relay topology and load-time block mirroring, is in
each project's `DCO/docs/PRESET_STORE.md`.
