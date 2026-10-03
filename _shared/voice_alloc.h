//----------------------------------//
// Voice allocation / note priority for the DCO boards
// by felipegaspari
// version 1.0
//----------------------------------//
//
// One allocator covering every policy in VoiceAllocMode, plus the mono held-key
// stack that the same policy value drives. Header-only, no dependency on the
// sketch: the caller keeps its own gate flags and note table and tells the
// allocator what happened through markOn() / markOff().
//
// See README.md for the modes and the wiring contract.

#ifndef DCO_VOICE_ALLOC_H
#define DCO_VOICE_ALLOC_H

#include <stdint.h>

#if defined(ARDUINO)
#include "Arduino.h"
#endif

// 1 = RP2040 __not_in_flash_func on the note-path methods (define before include).
// 0 = portable / flash (library default). No-op if the attribute is missing (AVR).
// Allocation runs at MIDI rate, so this buys jitter consistency rather than
// throughput: it keeps a note-on off the XIP cache while Core1 is mid-frame.
#ifndef VOICE_ALLOC_SRAM_HOT
#define VOICE_ALLOC_SRAM_HOT 0
#endif
#if VOICE_ALLOC_SRAM_HOT
#ifndef __not_in_flash_func
#define __not_in_flash_func(fn) fn
#endif
#define VOICE_ALLOC_HOT(fn) __not_in_flash_func(fn)
#else
#define VOICE_ALLOC_HOT(fn) fn
#endif

// Emit active config once per translation unit (visible in the compile log).
#ifndef VOICE_ALLOC_CONFIG_REPORTED
#define VOICE_ALLOC_CONFIG_REPORTED
#if VOICE_ALLOC_SRAM_HOT
#pragma message("voice_alloc: SRAM hot path ON (VOICE_ALLOC_SRAM_HOT=1) — alloc/markOn/markOff .time_critical")
#else
#pragma message("voice_alloc: SRAM hot path OFF (VOICE_ALLOC_SRAM_HOT=0) — library default")
#endif
#endif

// Voice allocation policy (PARAM_VOICE_ALLOC_MODE). One value drives two things:
// in poly it is the steal policy, in mono it is the held-stack note priority.
//   0 ROUND_ROBIN        poly least-recently-used   mono last-note
//   1 OLDEST             poly oldest trigger        mono first-note
//   2 QUIETEST           poly lowest EnvVCA level   mono last-note
//   3 QUIETEST_KEEP_LOW  as 2, spares lowest held   mono low-note
//   4 QUIETEST_KEEP_HIGH as 2, spares highest held  mono high-note
//   5 NO_STEAL           poly drops the note-on     mono first-note, later keys never sound
enum VoiceAllocMode : uint8_t {
  VOICE_ALLOC_ROUND_ROBIN = 0,
  VOICE_ALLOC_OLDEST = 1,
  VOICE_ALLOC_QUIETEST = 2,
  VOICE_ALLOC_QUIETEST_KEEP_LOW = 3,
  VOICE_ALLOC_QUIETEST_KEEP_HIGH = 4,
  VOICE_ALLOC_NO_STEAL = 5,
  VOICE_ALLOC_MODE_COUNT = 6
};

// alloc() returns this when VOICE_ALLOC_NO_STEAL refuses the note; also the
// "no note" marker for MonoNoteStack.
static constexpr uint8_t VOICE_ALLOC_NONE = 0xFF;

// Envelope full scale, and the level below which a release tail counts as
// silence (~ -54 dB) and its slot may be reused without an audible cut.
static constexpr int16_t VOICE_ALLOC_Q15_ONE = 32767;
static constexpr int16_t VOICE_ALLOC_SILENT_Q15 = 64;

enum VoiceState : uint8_t {
  VOICE_IDLE = 0,      // no note, envelope finished
  VOICE_HELD = 1,      // key down, gate high
  VOICE_RELEASING = 2  // note-off sent, envelope tail still sounding
};

// Allocation bookkeeping for up to MaxVoices slots.
//
// Owns state / trigger stamp / release stamp / LRU order and its own copy of the
// sounding note. The caller keeps its gate flags, note table and ADSR edge flags
// and mirrors them here: markOn() on every note-on path, markOff() on every
// note-off path. RELEASING is promoted to IDLE lazily inside alloc(), derived
// from the envelope level, so no second core ever has to free a slot.
template <uint8_t MaxVoices>
class VoiceAllocator {
  static_assert(MaxVoices >= 1 && MaxVoices <= 8,
                "VoiceAllocator supports 1..8 voices (the candidate mask is a uint8_t)");

public:
  VoiceAllocator() { begin(); }

  // Reset every slot to idle, rebuild the LRU order and register the level
  // source in one go. Call before setMode() / setVoiceCount(); it restores the
  // defaults for both.
  void begin(const int16_t* levels_q15 = nullptr) {
    _levels = levels_q15;
    _mode = VOICE_ALLOC_ROUND_ROBIN;
    _count = MaxVoices;
    _releaseMs = 0;
    _silentQ15 = VOICE_ALLOC_SILENT_Q15;
    for (uint8_t i = 0; i < MaxVoices; i++) {
      _state[i] = VOICE_IDLE;
      _startMs[i] = 0;
      _releaseAtMs[i] = 0;
      _note[i] = 0;
      _lru[i] = i;
    }
  }

  // Out-of-range values are ignored, so a stale preset cannot land the allocator
  // on a policy this build does not have.
  void setMode(uint8_t mode) {
    if (mode < VOICE_ALLOC_MODE_COUNT) _mode = mode;
  }
  uint8_t mode() const { return _mode; }

  // Slots the caller is actually running (its NUM_VOICES), clamped to MaxVoices.
  void setVoiceCount(uint8_t n) {
    if (n < 1) n = 1;
    if (n > MaxVoices) n = MaxVoices;
    _count = n;
  }
  uint8_t voiceCount() const { return _count; }

  // Per-voice EnvVCA level, 0..VOICE_ALLOC_Q15_ONE, indexed by voice. Used to
  // rank release tails by loudness and to tell a finished tail from a live one.
  // nullptr (the default) falls back to estimating the tail from setReleaseMs(),
  // for builds where nothing refreshes the levels.
  void setLevelSource(const int16_t* levels_q15) { _levels = levels_q15; }
  bool hasLevelSource() const { return _levels != nullptr; }

  // Envelope release time in ms. Only read when there is no level source.
  void setReleaseMs(uint16_t ms) { _releaseMs = ms; }

  void setSilenceThresholdQ15(int16_t q15) { _silentQ15 = q15; }

  uint8_t stateOf(uint8_t voice) const { return _state[voice]; }
  uint8_t noteOf(uint8_t voice) const { return _note[voice]; }

  // Voice already carrying this note, or VOICE_ALLOC_NONE. A release tail still
  // counts, so a fast retrigger reuses its own slot instead of stealing another.
  uint8_t findNote(uint8_t note) const {
    for (uint8_t i = 0; i < _count; i++) {
      if (_state[i] != VOICE_IDLE && _note[i] == note) return i;
    }
    return VOICE_ALLOC_NONE;
  }

  // Choose a voice for an incoming note. Prefers idle slots, then release tails,
  // and only steals a held note when nothing else is available.
  // Returns VOICE_ALLOC_NONE when the mode refuses to steal.
  uint8_t VOICE_ALLOC_HOT(alloc)(uint32_t now_ms) {
    uint8_t idle_mask = 0;
    uint8_t releasing_mask = 0;
    uint8_t held_mask = 0;

    for (uint8_t i = 0; i < _count; i++) {
      switch (effectiveState(i, now_ms)) {
        case VOICE_IDLE:      idle_mask |= (uint8_t)(1u << i); break;
        case VOICE_RELEASING: releasing_mask |= (uint8_t)(1u << i); break;
        default:              held_mask |= (uint8_t)(1u << i); break;
      }
    }

    uint8_t mask = idle_mask ? idle_mask : releasing_mask;
    if (!mask) {
      if (_mode == VOICE_ALLOC_NO_STEAL) return VOICE_ALLOC_NONE;
      mask = held_mask;

      // Spare the bass line (or the melody) unless it is the only thing left.
      if (_mode == VOICE_ALLOC_QUIETEST_KEEP_LOW
          || _mode == VOICE_ALLOC_QUIETEST_KEEP_HIGH) {
        const bool keep_low = (_mode == VOICE_ALLOC_QUIETEST_KEEP_LOW);
        uint8_t protect = VOICE_ALLOC_NONE;
        for (uint8_t i = 0; i < _count; i++) {
          if (!(held_mask & (1u << i))) continue;
          if (protect == VOICE_ALLOC_NONE
              || (keep_low ? (_note[i] < _note[protect])
                           : (_note[i] > _note[protect]))) {
            protect = i;
          }
        }
        if (protect != VOICE_ALLOC_NONE) {
          const uint8_t spared = (uint8_t)(held_mask & ~(1u << protect));
          if (spared != 0) mask = spared;
        }
      }
    }

    if (!mask) return VOICE_ALLOC_NONE;

    // The LRU order is updated by markOn(), which every caller reaches.
    return pick(mask, now_ms);
  }

  // Mark a voice as sounding a new note: held, freshly triggered, most recently
  // used. The caller raises its own gate and ADSR flags alongside this.
  void VOICE_ALLOC_HOT(markOn)(uint8_t voice, uint8_t note, uint32_t now_ms) {
    _state[voice] = VOICE_HELD;
    _startMs[voice] = now_ms;
    _note[voice] = note;
    lruTouch(voice);
  }

  // Gate a voice off and start tracking its release tail.
  void VOICE_ALLOC_HOT(markOff)(uint8_t voice, uint32_t now_ms) {
    _state[voice] = VOICE_RELEASING;
    _releaseAtMs[voice] = now_ms;
  }

  // Re-gate a slot that is already allocated: the pitch changes but the envelope
  // is not retriggered, so the trigger stamp and the LRU order stay put. This is
  // the mono fallback when a released key hands priority to another held one.
  void regate(uint8_t voice, uint8_t note) {
    _state[voice] = VOICE_HELD;
    _note[voice] = note;
  }

  // Realign the allocation state with the caller's gate flags. A slot that the
  // voice count dropped mid-note would otherwise come back HELD when the count
  // grows again.
  void resyncFromGates(const volatile uint32_t* gates) {
    for (uint8_t i = 0; i < MaxVoices; i++) {
      _state[i] = (gates[i] != 0) ? VOICE_HELD : VOICE_IDLE;
    }
  }

#if defined(ARDUINO)
  uint8_t alloc() { return alloc(millis()); }
  void markOn(uint8_t voice, uint8_t note) { markOn(voice, note, millis()); }
  void markOff(uint8_t voice) { markOff(voice, millis()); }
#endif

private:
  // Current EnvVCA level of a voice, used to rank release tails by loudness.
  // Without a level source, estimate the tail from the release time instead.
  int16_t ampQ15(uint8_t i, uint32_t now_ms) const {
    if (_levels) return _levels[i];
    if (_state[i] != VOICE_RELEASING) return VOICE_ALLOC_Q15_ONE;
    const uint32_t release_ms = _releaseMs;
    if (release_ms == 0) return 0;
    const uint32_t elapsed = now_ms - _releaseAtMs[i];
    if (elapsed >= release_ms) return 0;
    return (int16_t)(((uint32_t)VOICE_ALLOC_Q15_ONE * (release_ms - elapsed)) / release_ms);
  }

  // Effective state, promoting a release tail that has already faded out to idle.
  uint8_t effectiveState(uint8_t i, uint32_t now_ms) const {
    const uint8_t state = _state[i];
    if (state == VOICE_RELEASING && ampQ15(i, now_ms) <= _silentQ15) {
      return VOICE_IDLE;
    }
    return state;
  }

  // Move a voice to the front of the least-recently-used order. Every allocation
  // path calls this, so the round-robin order stays valid across a mode change.
  void lruTouch(uint8_t voice) {
    uint8_t pos = 0;
    while (pos < _count && _lru[pos] != voice) pos++;
    if (pos >= _count) pos = (uint8_t)(_count - 1);
    for (; pos > 0; pos--) {
      _lru[pos] = _lru[pos - 1];
    }
    _lru[0] = voice;
  }

  // Position in the LRU order; higher means longer since it was last used.
  uint8_t lruRank(uint8_t voice) const {
    for (uint8_t pos = 0; pos < _count; pos++) {
      if (_lru[pos] == voice) return pos;
    }
    return 0;
  }

  // Milliseconds since the voice was triggered. Subtracting before the
  // comparison keeps the ordering correct across the millis() rollover.
  uint32_t ageMs(uint8_t voice, uint32_t now_ms) const {
    return now_ms - _startMs[voice];
  }

  // Pick the best victim among the candidates flagged in the mask, applying the
  // mode's priority function. The mask always has at least one bit set.
  uint8_t VOICE_ALLOC_HOT(pick)(uint8_t mask, uint32_t now_ms) const {
    uint8_t best = VOICE_ALLOC_NONE;
    uint32_t best_key = 0;

    for (uint8_t i = 0; i < _count; i++) {
      if (!(mask & (1u << i))) continue;

      // Larger key wins, so every term is expressed as "how stealable is this".
      uint32_t key;
      switch (_mode) {
        case VOICE_ALLOC_OLDEST:
          key = ageMs(i, now_ms);
          break;
        case VOICE_ALLOC_QUIETEST:
        case VOICE_ALLOC_QUIETEST_KEEP_LOW:
        case VOICE_ALLOC_QUIETEST_KEEP_HIGH: {
          // Quietest first; age breaks ties, which a held chord sitting at the same
          // sustain level produces constantly. Clamped rather than masked so a voice
          // older than the tiebreak range does not wrap back to looking young.
          const uint32_t age = ageMs(i, now_ms);
          key = ((uint32_t)(VOICE_ALLOC_Q15_ONE - ampQ15(i, now_ms)) << 8)
              | (age > 255u ? 255u : age);
          break;
        }
        case VOICE_ALLOC_ROUND_ROBIN:
        case VOICE_ALLOC_NO_STEAL:
        default:
          key = lruRank(i);
          break;
      }

      if (best == VOICE_ALLOC_NONE || key > best_key) {
        best = i;
        best_key = key;
      }
    }
    return best;
  }

  const int16_t* _levels;
  volatile uint8_t _mode;
  volatile uint8_t _count;
  uint16_t _releaseMs;
  int16_t _silentQ15;

  volatile uint8_t _state[MaxVoices];
  volatile uint8_t _note[MaxVoices];
  volatile uint8_t _lru[MaxVoices];  // front = most recently used
  volatile uint32_t _startMs[MaxVoices];
  volatile uint32_t _releaseAtMs[MaxVoices];
};

// Mono held-key stack. Kept in strike order, so the ends of it give last-note
// and first-note; the same VoiceAllocMode value that picks a steal victim in
// poly picks the sounding key here.
template <uint8_t Depth = 8>
class MonoNoteStack {
  static_assert(Depth >= 1, "MonoNoteStack needs at least one slot");

public:
  MonoNoteStack() { clear(); }

  // Empty the stack, so notes do not leak across a voice mode change.
  void clear() { _count = 0; }

  uint8_t count() const { return _count; }
  bool empty() const { return _count == 0; }

  // Re-strike moves to the top; if full, drop the oldest so the newest note
  // still wins. Returns false when VOICE_ALLOC_NO_STEAL denies the key: while
  // one is down the rest are ignored outright, so they cannot take over later
  // either, which is what separates mode 5 from plain first-note priority.
  bool VOICE_ALLOC_HOT(push)(uint8_t note, uint8_t mode) {
    if (mode == VOICE_ALLOC_NO_STEAL && _count > 0) return false;

    remove(note);
    if (_count >= Depth) {
      for (uint8_t i = 0; i + 1u < Depth; i++) {
        _notes[i] = _notes[i + 1u];
      }
      _count = Depth - 1u;
    }
    _notes[_count++] = note;
    return true;
  }

  // Remove the first match. Returns false if the note was not held.
  bool VOICE_ALLOC_HOT(remove)(uint8_t note) {
    for (uint8_t i = 0; i < _count; i++) {
      if (_notes[i] != note) continue;
      for (uint8_t j = i; j + 1u < _count; j++) {
        _notes[j] = _notes[j + 1u];
      }
      _count--;
      return true;
    }
    return false;
  }

  // Which held key sounds, per the note priority in the mode.
  // VOICE_ALLOC_NONE when nothing is held.
  uint8_t VOICE_ALLOC_HOT(pick)(uint8_t mode) const {
    if (_count == 0) return VOICE_ALLOC_NONE;

    switch (mode) {
      case VOICE_ALLOC_OLDEST:
      case VOICE_ALLOC_NO_STEAL:
        return _notes[0];

      case VOICE_ALLOC_QUIETEST_KEEP_LOW:
      case VOICE_ALLOC_QUIETEST_KEEP_HIGH: {
        const bool lowest = (mode == VOICE_ALLOC_QUIETEST_KEEP_LOW);
        uint8_t best = _notes[0];
        for (uint8_t i = 1; i < _count; i++) {
          const uint8_t n = _notes[i];
          if (lowest ? (n < best) : (n > best)) best = n;
        }
        return best;
      }

      default:
        return _notes[_count - 1u];
    }
  }

private:
  uint8_t _notes[Depth];
  uint8_t _count;
};

#endif  // DCO_VOICE_ALLOC_H
