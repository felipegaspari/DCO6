// Calibration storage definitions: the init_FS() loader, the update_FS_*
// writers and the fake-calibration seed. Include exactly once, from the
// sketch's DCO/FS.ino shim. Declarations: FS.h. Format and the rules for
// changing any of it: docs/CALIBRATION_STORAGE.md.
//
// The on-flash byte layout is fixed — bank sizes are compile-time constants
// that preset bulk restore, the host cal dump and this loader each derive
// independently, so a size change silently breaks stored calibration.
#ifndef __FS_IMPL_H__
#define __FS_IMPL_H__

#include "../include_all.h"

#if PROJECT_INSTRUMENT == 4
static void ensure_pw_fs_banks();
#endif

// Mount LittleFS and load amp-comp / PW / offset calibration into runtime arrays (float or Q8).
// Called from setup1() and again at end of DCO_calibration().
void init_FS() {
  LittleFS.begin();

  if (!LittleFS.exists("voiceTables")) {
    fileVoiceTablesFS = LittleFS.open("voiceTables", "w+");
  } else {
    fileVoiceTablesFS = LittleFS.open("voiceTables", "r");
  }

#ifdef ENABLE_FS_CALIBRATION

  fileVoiceTablesFS.read(voiceTablesBankBuffer, FSBankSize);
  fileVoiceTablesFS.close();


    for (int i = 0; i < (chanLevelVoiceDataSize * NUM_OSCILLATORS); i++) {
     freq_to_amp_comp_array[i] = (int32_t(voiceTablesBankBuffer[i * 4 + 3]) << 24) |
                    (int32_t(voiceTablesBankBuffer[i * 4 + 2]) << 16) |
                    (int32_t(voiceTablesBankBuffer[i * 4 + 1]) << 8) |
                    int32_t(voiceTablesBankBuffer[i * 4 ]);
  }

    for (int datasetIndex = 0; datasetIndex < NUM_OSCILLATORS; ++datasetIndex) {
        for (int pairIndex = 0; pairIndex < chanLevelVoiceDataSize / 2; ++pairIndex) {
            int rawIndex = datasetIndex * chanLevelVoiceDataSize + pairIndex * 2;

        // Stored frequencies are in Hz*100.
            int32_t freq_x100 = freq_to_amp_comp_array[rawIndex];

        ampCompArray[datasetIndex][pairIndex] = freq_to_amp_comp_array[rawIndex + 1];

#ifdef USE_FLOAT_AMP_COMP
        // Float engine: Hz table for FLOAT_QUAD / LUT; Q8 also seeded at precompute for FIXED.
        float freqHz = (float)freq_x100 / 100.0f;
        ampCompFrequencyHz[datasetIndex][pairIndex] = freqHz;
#else
        // Fixed-point engine: convert to fixed-point Hz (Hz * 2^FREQ_FRAC_BITS).
        int64_t scaled = (int64_t)freq_x100 * (1LL << FREQ_FRAC_BITS);
        int32_t freq_fx = (scaled >= 0)
                        ? (int32_t)((scaled + 50LL) / 100LL)
                        : (int32_t)(-((( -scaled) + 50LL) / 100LL));
        ampCompFrequencyArray[datasetIndex][pairIndex] = freq_fx;
#endif
        }
    }


  uint8_t highestNoteFound = 255;
  // for (int i = 0; i < NUM_OSCILLATORS; i++) {
  //   highestOSCNote[i] =
  //     if (highestOSCNote[i] < highestNoteFound) {
  //     highestNoteFound = highestOSCNote[i];
  //   }
  // }

  // PW CALIBRATION VALUES FROM FS (one slot per PW channel).
#if PROJECT_INSTRUMENT == 4
  ensure_pw_fs_banks();
#endif

  // PW_CENTER
  if (!LittleFS.exists("PWCenter")) {
    filePWCenterFS = LittleFS.open("PWCenter", "w+");
  } else {
    filePWCenterFS = LittleFS.open("PWCenter", "r");
  }

  filePWCenterFS.read(PWCenterBankBuffer, FSPWBankSize);
  filePWCenterFS.close();

  for (int i = 0; i < NUM_PW_CHANNELS; i++) {
    uint16_t uint16Data;
    for (int j = 0; j < FSPWDataSize; j++) {
      ((uint8_t *)&uint16Data)[j] = PWCenterBankBuffer[i * 2 + j];
    }

    PW_CENTER[i] = (uint16_t)uint16Data;
  }
  // PW_HIGH_LIMIT
    if (!LittleFS.exists("PWHighLimit")) {
    filePWHighLimitFS = LittleFS.open("PWHighLimit", "w+");
  } else {
    filePWHighLimitFS = LittleFS.open("PWHighLimit", "r");
  }

  filePWHighLimitFS.read(PWHighLimitBankBuffer, FSPWBankSize);
  filePWHighLimitFS.close();

  for (int i = 0; i < NUM_PW_CHANNELS; i++) {
    uint16_t uint16Data;
    for (int j = 0; j < FSPWDataSize; j++) {
      ((uint8_t *)&uint16Data)[j] = PWHighLimitBankBuffer[i * 2 + j];
    }
    PW_HIGH_LIMIT[i] = (uint16_t)uint16Data;
  }
  // PW_LOW_LIMIT
  if (!LittleFS.exists("PWLowLimit")) {
    filePWLowLimitFS = LittleFS.open("PWLowLimit", "w+");
  } else {
    filePWLowLimitFS = LittleFS.open("PWLowLimit", "r");
  }

  filePWLowLimitFS.read(PWLowLimitBankBuffer, FSPWBankSize);
  filePWLowLimitFS.close();

  for (int i = 0; i < NUM_PW_CHANNELS; i++) {
    uint16_t uint16Data;
    for (int j = 0; j < FSPWDataSize; j++) {
      ((uint8_t *)&uint16Data)[j] = PWLowLimitBankBuffer[i * 2 + j];
    }
    PW_LOW_LIMIT[i] = (uint16_t)uint16Data;
  }

  // Manual calibration offsets (one signed byte per oscillator).
  if (!LittleFS.exists("ManualOffset")) {
    fileManualOffsetFS = LittleFS.open("ManualOffset", "w+");
    // Initialise FS with zeros so future reads are defined.
    for (int i = 0; i < FSManualOffsetBankSize; ++i) {
      ManualOffsetBankBuffer[i] = 0;
    }
    fileManualOffsetFS.write(ManualOffsetBankBuffer, FSManualOffsetBankSize);
  } else {
    fileManualOffsetFS = LittleFS.open("ManualOffset", "r");
    fileManualOffsetFS.read(ManualOffsetBankBuffer, FSManualOffsetBankSize);
  }
  fileManualOffsetFS.close();

  // Copy stored offsets into the runtime array.
  for (int osc = 0; osc < NUM_OSCILLATORS; ++osc) {
    manualCalibrationOffset[osc] = (int8_t)ManualOffsetBankBuffer[osc];
  }

  // 440 Hz manual anchor values (one uint16 per oscillator, little-endian).
  if (!LittleFS.exists("AmpComp440")) {
    fileAmpComp440FS = LittleFS.open("AmpComp440", "w+");
    // Initialise FS with zeros (0 = never set) so future reads are defined.
    for (int i = 0; i < FSAmpComp440BankSize; ++i) {
      AmpComp440BankBuffer[i] = 0;
    }
    fileAmpComp440FS.write(AmpComp440BankBuffer, FSAmpComp440BankSize);
  } else {
    fileAmpComp440FS = LittleFS.open("AmpComp440", "r");
    fileAmpComp440FS.read(AmpComp440BankBuffer, FSAmpComp440BankSize);
  }
  fileAmpComp440FS.close();

  for (int osc = 0; osc < NUM_OSCILLATORS; ++osc) {
    uint16_t v;
    ((uint8_t *)&v)[0] = AmpComp440BankBuffer[osc * 2];
    ((uint8_t *)&v)[1] = AmpComp440BankBuffer[osc * 2 + 1];
    ampComp440[osc] = v;
  }

  // Duty target trims (one int16 per oscillator, little-endian; 0 = none).
  if (!LittleFS.exists("AmpCompDutyOffset")) {
    fileAmpCompDutyOffsetFS = LittleFS.open("AmpCompDutyOffset", "w+");
    for (int i = 0; i < FSAmpCompDutyOffsetBankSize; ++i) {
      AmpCompDutyOffsetBankBuffer[i] = 0;
    }
    fileAmpCompDutyOffsetFS.write(AmpCompDutyOffsetBankBuffer, FSAmpCompDutyOffsetBankSize);
  } else {
    fileAmpCompDutyOffsetFS = LittleFS.open("AmpCompDutyOffset", "r");
    fileAmpCompDutyOffsetFS.read(AmpCompDutyOffsetBankBuffer, FSAmpCompDutyOffsetBankSize);
  }
  fileAmpCompDutyOffsetFS.close();

  for (int osc = 0; osc < NUM_OSCILLATORS; ++osc) {
    int16_t v;
    ((uint8_t *)&v)[0] = AmpCompDutyOffsetBankBuffer[osc * 2];
    ((uint8_t *)&v)[1] = AmpCompDutyOffsetBankBuffer[osc * 2 + 1];
    ampCompDutyOffset[osc] = v;
  }

#endif

  //singleFileDrive.begin("voiceTables", "voicetables.txt");
}

// Persist one oscillator's calibrationData slice into voiceTables. Called from DCO_calibration().
void update_FS_voice(byte voiceN) {
  byte calibrationDataBytes[FSVoiceDataSize];

  // Serialize calibrationData (uint32_t pairs: [freq_x100, pwm]) for this voice
  // into a contiguous byte buffer. Each entry is written little-endian.

  for (int i = 0; i < chanLevelVoiceDataSize; i++) {
    // freq_to_amp_comp_array[i + (voiceN * chanLevelVoiceDataSize)] = calibrationData[i]; // can be used for in-RAM updates if desired
    byte *b = (byte *)&calibrationData[i];
    for (int j = 0; j < 4; j++) {
      calibrationDataBytes[i * 4 + j] = b[j];
    }
  }
  uint16_t startByteN = voiceN * FSVoiceDataSize;

  fileVoiceTablesFS = LittleFS.open("voiceTables", "r+");
  fileVoiceTablesFS.seek(startByteN);
  fileVoiceTablesFS.write(calibrationDataBytes, FSVoiceDataSize);
  fileVoiceTablesFS.close();
}


// Persist PW center for one PW channel. Called from find_PW_center().
void update_FS_PWCenter(byte voiceN, uint16_t value) {
  if (voiceN >= NUM_PW_CHANNELS) {
    return;
  }
  byte calibrationDataBytes[FSPWDataSize];
  byte *b = (byte *)&value;

  uint16_t startByteN = voiceN * FSPWDataSize;

  filePWCenterFS = LittleFS.open("PWCenter", "r+");
  filePWCenterFS.seek(startByteN);
  filePWCenterFS.write(b, FSPWDataSize);
  filePWCenterFS.close();
}

// Persist PW high limit for one PW channel. Called from find_PW_limit_v2().
void update_FS_PW_High_Limit(byte voiceN, uint16_t value) {
  if (voiceN >= NUM_PW_CHANNELS) {
    return;
  }
  byte calibrationDataBytes[FSPWDataSize];
  byte *b = (byte *)&value;

  uint16_t startByteN = voiceN * FSPWDataSize;

  filePWHighLimitFS = LittleFS.open("PWHighLimit", "r+");
  filePWHighLimitFS.seek(startByteN);
  filePWHighLimitFS.write(b, FSPWDataSize);
  filePWHighLimitFS.close();
}

// Persist PW low limit for one PW channel. Called from find_PW_limit_v2().
void update_FS_PW_Low_Limit(byte voiceN, uint16_t value) {
  if (voiceN >= NUM_PW_CHANNELS) {
    return;
  }
  byte calibrationDataBytes[FSPWDataSize];
  byte *b = (byte *)&value;

  uint16_t startByteN = voiceN * FSPWDataSize;

  filePWLowLimitFS = LittleFS.open("PWLowLimit", "r+");
  filePWLowLimitFS.seek(startByteN);
  filePWLowLimitFS.write(b, FSPWDataSize);
  filePWLowLimitFS.close();
}

// Persist a single manualCalibrationOffset entry for the given oscillator index.
// Persist one oscillator's manual calibration offset. Called from apply_param_manual_calibration_store().
void update_FS_ManualCalibrationOffset(byte oscIndex, int8_t value) {
  if (oscIndex >= NUM_OSCILLATORS) {
    return;
  }

  uint8_t b = (uint8_t)value;  // store raw signed byte
  uint16_t startByteN = oscIndex * FSManualOffsetDataSize;

  fileManualOffsetFS = LittleFS.open("ManualOffset", "r+");
  fileManualOffsetFS.seek(startByteN);
  fileManualOffsetFS.write(&b, FSManualOffsetDataSize);
  fileManualOffsetFS.close();
}

// Persist one oscillator's 440 Hz manual anchor value (ampComp440[]).
// Called from apply_param_manual_calibration_store().
void update_FS_AmpComp440(byte oscIndex, uint16_t value) {
  if (oscIndex >= NUM_OSCILLATORS) {
    return;
  }

  byte *b = (byte *)&value;
  uint16_t startByteN = oscIndex * FSAmpComp440DataSize;

  fileAmpComp440FS = LittleFS.open("AmpComp440", "r+");
  fileAmpComp440FS.seek(startByteN);
  fileAmpComp440FS.write(b, FSAmpComp440DataSize);
  fileAmpComp440FS.close();
}

// Persist one oscillator's duty target trim (ampCompDutyOffset[]).
// Called from apply_param_manual_calibration_store().
void update_FS_AmpCompDutyOffset(byte oscIndex, int16_t value) {
  if (oscIndex >= NUM_OSCILLATORS) {
    return;
  }

  byte *b = (byte *)&value;
  uint16_t startByteN = oscIndex * FSAmpCompDutyOffsetDataSize;

  fileAmpCompDutyOffsetFS = LittleFS.open("AmpCompDutyOffset", "r+");
  fileAmpCompDutyOffsetFS.seek(startByteN);
  fileAmpCompDutyOffsetFS.write(b, FSAmpCompDutyOffsetDataSize);
  fileAmpCompDutyOffsetFS.close();
}

// Archived amp-comp PWM curve (old wrap 10000), used as shape reference for fakes.
// See _removed/amp_comp.h — excludes the leading (0,0) and trailing sentinel pair.
static const uint16_t kFakeAmpPwmRef[] = {
  40, 50, 62, 79, 101, 130, 170, 222, 292, 386,
  511, 675, 924, 1252, 1688, 2231, 3034, 4132, 5632, 7676, 10000
};
static constexpr int kFakeAmpPwmRefCount =
  (int)(sizeof(kFakeAmpPwmRef) / sizeof(kFakeAmpPwmRef[0]));
static constexpr uint16_t kFakeAmpPwmRefWrap = 10000;
static constexpr uint32_t kFakeUnreachableFreqX100 = 20000000u;

// Truncate/create a LittleFS file and write a full bank in one shot.
// Non-static: also used by the bulk-restore path in preset_store.ino.
void write_fs_bank(const char* name, const uint8_t* data, size_t size) {
  File f = LittleFS.open(name, "w");
  if (!f) {
    return;
  }
  f.write(data, size);
  f.close();
}

// Pack one uint16 little-endian into a PW bank buffer at voiceN * 2.
static void pack_pw_u16(uint8_t* bank, uint8_t voiceN, uint16_t value) {
  bank[voiceN * FSPWDataSize + 0] = (uint8_t)(value & 0xFF);
  bank[voiceN * FSPWDataSize + 1] = (uint8_t)((value >> 8) & 0xFF);
}

#if PROJECT_INSTRUMENT == 4
static bool fs_file_size_ok(const char* name, size_t expected) {
  File f = LittleFS.open(name, "r");
  if (!f) {
    return false;
  }
  const size_t sz = f.size();
  f.close();
  return sz == expected;
}

// Rewrite 4-voice PW banks if missing or still the old 8-slot (16 B) size.
// DCO4-only migration: must never run on DCO3, where it would overwrite a
// measured PW center with defaults.
static void ensure_pw_fs_banks() {
  if (fs_file_size_ok("PWCenter", FSPWBankSize) &&
      fs_file_size_ok("PWHighLimit", FSPWBankSize) &&
      fs_file_size_ok("PWLowLimit", FSPWBankSize)) {
    return;
  }
  for (uint8_t v = 0; v < NUM_PW_CHANNELS; ++v) {
    pack_pw_u16(PWCenterBankBuffer, v, kPwCenterDefault[v]);
    pack_pw_u16(PWLowLimitBankBuffer, v, 0);
    pack_pw_u16(PWHighLimitBankBuffer, v, DIV_COUNTER_PW);
  }
  write_fs_bank("PWCenter", PWCenterBankBuffer, FSPWBankSize);
  write_fs_bank("PWLowLimit", PWLowLimitBankBuffer, FSPWBankSize);
  write_fs_bank("PWHighLimit", PWHighLimitBankBuffer, FSPWBankSize);
}
#endif

// Build one oscillator's 22 [freq_x100, RANGE PWM] pairs matching real cal layout.
void generate_fake_calibration_data(uint8_t osc, uint32_t* out) {
  if (out == nullptr) {
    return;
  }
  if (osc >= NUM_OSCILLATORS) {
    osc = NUM_OSCILLATORS - 1;
  }

  // Small per-osc spread so tables are not identical. Sized for the largest
  // board (8 oscillators); smaller boards index only the first entries.
  static const float kOscScale[8] = {
    1.00f, 1.02f, 0.98f, 1.01f, 0.99f, 1.03f, 0.97f, 1.00f
  };
  const float oscScale = kOscScale[osc];
  const uint32_t pwmSat = (uint32_t)(0.98f * (float)DIV_COUNTER);

  // Pair 0: lowest-freq anchor (same header as restart_DCO_calibration).
  out[0] = 0;
  out[1] = (uint32_t)ampCompLowestFreqVal;

  // Pair 1: note (start - interval) with manual seed PWM.
  const uint8_t headerNote =
    (uint8_t)(DCO_calibration_start_note - calibration_note_interval);
  out[2] = (uint32_t)(sNotePitches[headerNote - 12] * 100.0f);
  out[3] = (uint32_t)(initManualAmpCompCalibrationVal[osc] +
                      manualCalibrationOffset[osc]);

  bool plateau = false;
  for (int pair = 2; pair < ampCompTableSize; ++pair) {
    const int i = pair * 2;

    if (plateau) {
      out[i]     = kFakeUnreachableFreqX100;
      out[i + 1] = DIV_COUNTER;
      continue;
    }

    const uint8_t note =
      (uint8_t)(DCO_calibration_start_note +
                calibration_note_interval * (pair - 2));
    const int pitchIdx = (int)note - 12;
    if (pitchIdx < 0 ||
        pitchIdx >= (int)(sizeof(sNotePitches) / sizeof(sNotePitches[0]))) {
      out[i]     = kFakeUnreachableFreqX100;
      out[i + 1] = DIV_COUNTER;
      plateau = true;
      continue;
    }

    // Map archived curve onto calibrated slots (pair 2 → ref[1], …).
    int refIdx = pair - 1;
    if (refIdx < 0) {
      refIdx = 0;
    }
    if (refIdx >= kFakeAmpPwmRefCount) {
      refIdx = kFakeAmpPwmRefCount - 1;
    }

    float pwmF = ((float)kFakeAmpPwmRef[refIdx] / (float)kFakeAmpPwmRefWrap) *
                 (float)DIV_COUNTER * oscScale;
    if (pwmF < 1.0f) {
      pwmF = 1.0f;
    }
    if (pwmF > (float)DIV_COUNTER) {
      pwmF = (float)DIV_COUNTER;
    }
    uint32_t pwm = (uint32_t)(pwmF + 0.5f);

    if (pwm >= pwmSat) {
      out[i]     = (uint32_t)(sNotePitches[pitchIdx] * 100.0f);
      out[i + 1] = DIV_COUNTER;
      plateau = true;
      continue;
    }

    out[i]     = (uint32_t)(sNotePitches[pitchIdx] * 100.0f);
    out[i + 1] = pwm;
  }
}

// Seed LittleFS with fake amp-comp tables + PW defaults + AmpComp440, then reload.
// force=false: only if voiceTables is missing. force=true: overwrite + precompute (cmd 30).
// Silent: no Serial (Core1 / TinyUSB race). Call before init_FS() at boot so stubs
// do not mask a missing file.
void seed_fake_calibration_tables(bool force) {
  LittleFS.begin();
  if (!force && LittleFS.exists("voiceTables")) {
    return;
  }

  for (uint8_t osc = 0; osc < NUM_OSCILLATORS; ++osc) {
    generate_fake_calibration_data(osc, calibrationData);

    // Pack LE uint32 pairs into the full voiceTables bank (same as update_FS_voice).
    const uint16_t startByteN = osc * FSVoiceDataSize;
    for (int i = 0; i < chanLevelVoiceDataSize; ++i) {
      const byte* b = (const byte*)&calibrationData[i];
      for (int j = 0; j < 4; ++j) {
        voiceTablesBankBuffer[startByteN + i * 4 + j] = b[j];
      }
    }

  }

  // Sane PW defaults (empty FS would otherwise load zeros over RAM defaults).
  for (uint8_t v = 0; v < NUM_PW_CHANNELS; ++v) {
    pack_pw_u16(PWCenterBankBuffer, v, kPwCenterDefault[v]);
    pack_pw_u16(PWLowLimitBankBuffer, v, 0);
    pack_pw_u16(PWHighLimitBankBuffer, v, DIV_COUNTER_PW);
  }

  static constexpr uint16_t kFakeAmpComp440 = DIV_COUNTER / 10;
  for (uint8_t osc = 0; osc < NUM_OSCILLATORS; ++osc) {
    AmpComp440BankBuffer[osc * FSAmpComp440DataSize + 0] =
      (uint8_t)(kFakeAmpComp440 & 0xFF);
    AmpComp440BankBuffer[osc * FSAmpComp440DataSize + 1] =
      (uint8_t)((kFakeAmpComp440 >> 8) & 0xFF);
  }

  write_fs_bank("voiceTables", voiceTablesBankBuffer, FSBankSize);
  write_fs_bank("PWCenter", PWCenterBankBuffer, FSPWBankSize);
  write_fs_bank("PWLowLimit", PWLowLimitBankBuffer, FSPWBankSize);
  write_fs_bank("PWHighLimit", PWHighLimitBankBuffer, FSPWBankSize);
  write_fs_bank("AmpComp440", AmpComp440BankBuffer, FSAmpComp440BankSize);

  init_FS();
  if (force) {
    precompute_amp_comp_for_engine();
  }
}

#endif  // __FS_IMPL_H__
