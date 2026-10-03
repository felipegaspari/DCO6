# voice_alloc.h

Voice allocation for a polyphonic or paraphonic board, and the mono held-key
stack. One policy value drives both halves of the same decision: in poly it
picks which voice gets stolen when all of them are busy, in mono it picks which
held key sounds. This is `PARAM_VOICE_ALLOC_MODE` (102) in
[DCO-PROTOCOL](https://github.com/felipegaspari/DCO-PROTOCOL).

Include it as `#include "_shared/voice_alloc.h"`; each sketch wraps it in a local
`voice_alloc_state.h` that fixes `MaxVoices` and instantiates the objects.

## Modes

| `VoiceAllocMode` | Poly | Mono |
|---|---|---|
| `0 VOICE_ALLOC_ROUND_ROBIN` | least recently used | last note |
| `1 VOICE_ALLOC_OLDEST` | oldest trigger | first note |
| `2 VOICE_ALLOC_QUIETEST` | lowest EnvVCA level | last note |
| `3 VOICE_ALLOC_QUIETEST_KEEP_LOW` | as 2, spares the lowest held note | low note |
| `4 VOICE_ALLOC_QUIETEST_KEEP_HIGH` | as 2, spares the highest held note | high note |
| `5 VOICE_ALLOC_NO_STEAL` | drops the note-on | first note, later keys never sound |

Every poly mode takes an idle slot first, then the best release tail, and only
steals a held note as a last resort. Mode `5` refuses that last step and returns
`VOICE_ALLOC_NONE`.

Mode `5` in mono is not the same as mode `1`. First-note priority keeps the
later keys on the stack, so they take over as earlier keys are released; mode
`5` refuses them at push time, so they never sound at all.

## State ownership

The library owns only the allocation bookkeeping: per-voice state, trigger
stamp, release stamp, LRU order and its own copy of the sounding note. The
sketch keeps its own gate flags, note table and ADSR edge flags, and mirrors
every change into the allocator through `markOn()` / `markOff()`. Nothing in a
sketch hot path has to change to adopt it.

Voices move `IDLE -> HELD -> RELEASING`. The step back to `IDLE` is not written
by anyone: `alloc()` derives it from the envelope level each time it runs, so
there is no window in which one core frees a slot the other just took.

## Level source

The quietest modes, and the "this release tail has finished" test, need a
per-voice EnvVCA level in Q15 (`0..32767`):

```cpp
voiceAlloc.begin(ADSR_VCA_Level_q15);   // or setLevelSource() later
```

If no level source is registered, the allocator estimates a tail from the
release time instead, which the sketch must keep current:

```cpp
voiceAlloc.setReleaseMs(ADSR_VCA_release);
```

That fallback exists for builds where nothing refreshes the level array, such as
DCO4 with `ENABLE_MB_MOD_STREAM`, where the envelopes are computed on the
Mainboard.

## API

```cpp
template <uint8_t MaxVoices> class VoiceAllocator;  // 1..8 slots
```

| Call | Notes |
|---|---|
| `begin(levels_q15 = nullptr)` | Reset all slots to idle, rebuild the LRU order, register the level source. Also restores the default mode and voice count, so call it first. |
| `setMode(m)` / `mode()` | Out-of-range values are ignored. |
| `setVoiceCount(n)` / `voiceCount()` | The runtime `NUM_VOICES`, clamped to `MaxVoices`. |
| `setLevelSource(p)` / `hasLevelSource()` | See above. |
| `setReleaseMs(ms)` | Only read when there is no level source. |
| `setSilenceThresholdQ15(q15)` | Default 64, about -54 dB. |
| `findNote(note)` | Voice already carrying this note, including a release tail, else `VOICE_ALLOC_NONE`. |
| `alloc(now_ms)` | The allocation. `VOICE_ALLOC_NONE` when mode 5 refuses. |
| `markOn(v, note, now_ms)` | Held, freshly triggered, most recently used. |
| `markOff(v, now_ms)` | Gate off, start tracking the tail. |
| `regate(v, note)` | Re-gate an already allocated slot without retriggering: new pitch, same trigger stamp. The mono priority fallback. |
| `resyncFromGates(gates)` | Realign with the sketch's gate flags after a voice count change. |
| `stateOf(v)` / `noteOf(v)` | Read-only. |

`alloc()`, `markOn()` and `markOff()` also have no-argument overloads that call
`millis()`. The explicit `now_ms` ones exist so a caller that already read the
clock does not read it twice, and so the allocator can be tested off-target.

```cpp
template <uint8_t Depth = 8> class MonoNoteStack;
```

| Call | Notes |
|---|---|
| `clear()` | Drop every held key, on a voice mode change. |
| `push(note, mode)` | Re-strike moves to the top; full drops the oldest. Returns `false` when mode 5 denies the key. |
| `remove(note)` | Returns `false` if the note was not held. |
| `pick(mode)` | The sounding key, or `VOICE_ALLOC_NONE` when nothing is held. |
| `count()` / `empty()` | |

The mode is passed in rather than shared with the allocator, so the two classes
stay independent.

## Build flags

| Flag | Default | Effect |
|---|---|---|
| `VOICE_ALLOC_SRAM_HOT` | `0` | `1` puts `alloc` / `markOn` / `markOff` / the two `pick`s and the stack edits in `.time_critical` (RP2040 `__not_in_flash_func`). No-op where the attribute does not exist. |

Define it before the include, the same way the DCO sketches do for
`ADSR_BEZIER_SRAM_HOT` and `MO_LFO_SRAM_HOT`:

```cpp
#ifndef VOICE_ALLOC_SRAM_HOT
#define VOICE_ALLOC_SRAM_HOT 1
#endif
#include "_shared/voice_alloc.h"
```

Allocation runs on the MIDI path, not in an audio loop, so this buys jitter
consistency rather than throughput: it keeps a note-on off the XIP cache while
the audio core is mid-frame. The active setting is reported once per translation
unit with `#pragma message`, visible in the `arduino-cli` log.

## Usage sketch

```cpp
// once
voiceAlloc.begin(ADSR_VCA_Level_q15);
voiceAlloc.setVoiceCount(NUM_VOICES);

// poly note-on
uint8_t v = voiceAlloc.findNote(note);
if (v == VOICE_ALLOC_NONE) v = voiceAlloc.alloc();
if (v == VOICE_ALLOC_NONE) return;          // mode 5 refused the note
VOICES[v] = 1; VOICE_NOTES[v] = note;       // the sketch's own bookkeeping
voiceAlloc.markOn(v, note);

// poly note-off
VOICES[v] = 0;
voiceAlloc.markOff(v);

// mono note-on
if (!monoStack.push(note, voiceAlloc.mode())) return;
const uint8_t winner = monoStack.pick(voiceAlloc.mode());
```
