// Calibration storage: bank sizes, RAM buffers, file handles, FS API.
// Shared by DCO3-MONOSYNTH and DCO4-REBORN through the DCO/FS.h shim.
// Definitions live in FS_impl.h. On-flash format, per-board sizing and the
// invariants that keep stored calibration readable: docs/CALIBRATION_STORAGE.md.
//
// Included FROM the sketch's include_all.h (and before amp_comp.h, which sizes
// arrays with chanLevelVoiceDataSize) — so it must not include include_all.h.
// NUM_PW_CHANNELS must already be defined by the sketch's globals.h.
#ifndef __FS_H__
#define __FS_H__

static constexpr uint16_t FSVoiceDataSize = 22 * 2 * 4;

static constexpr uint16_t FSPWDataSize = 2;

static constexpr uint16_t FSBankSize = FSVoiceDataSize * NUM_OSCILLATORS;
static constexpr uint16_t FSPWBankSize = FSPWDataSize * NUM_PW_CHANNELS;

// One signed byte per oscillator to store manualCalibrationOffset[].
static constexpr uint16_t FSManualOffsetDataSize = 1;
static constexpr uint16_t FSManualOffsetBankSize = FSManualOffsetDataSize * NUM_OSCILLATORS;

// One uint16 per oscillator to store ampComp440[] (440 Hz manual anchor).
static constexpr uint16_t FSAmpComp440DataSize = 2;
static constexpr uint16_t FSAmpComp440BankSize = FSAmpComp440DataSize * NUM_OSCILLATORS;

// One int16 per oscillator to store ampCompDutyOffset[] (duty target trim,
// hundredths of a percent of duty).
static constexpr uint16_t FSAmpCompDutyOffsetDataSize = 2;
static constexpr uint16_t FSAmpCompDutyOffsetBankSize =
  FSAmpCompDutyOffsetDataSize * NUM_OSCILLATORS;

static constexpr uint16_t chanLevelVoiceDataSize = FSVoiceDataSize / 4;

// Calibration buffers (FS-local)
uint8_t voiceTablesCalibrationBuffer[FSVoiceDataSize];
uint8_t PWCenterCalibrationBuffer[FSPWDataSize];
uint8_t PWHighLimitCalibrationBuffer[FSPWDataSize];
uint8_t PWLowLimitCalibrationBuffer[FSPWDataSize];
uint8_t ManualOffsetCalibrationBuffer[FSManualOffsetDataSize];

uint8_t voiceTablesBankBuffer[FSBankSize];
uint8_t PWCenterBankBuffer[FSPWBankSize];
uint8_t PWHighLimitBankBuffer[FSPWBankSize];
uint8_t PWLowLimitBankBuffer[FSPWBankSize];
uint8_t ManualOffsetBankBuffer[FSManualOffsetBankSize];
uint8_t AmpComp440BankBuffer[FSAmpComp440BankSize];
uint8_t AmpCompDutyOffsetBankBuffer[FSAmpCompDutyOffsetBankSize];

File fileVoiceTablesFS;
File filePWCenterFS;
File filePWHighLimitFS;
File filePWLowLimitFS;
File fileManualOffsetFS;
File fileAmpComp440FS;
File fileAmpCompDutyOffsetFS;

// Build one oscillator's 22 [freq_x100, RANGE PWM] pairs into out[chanLevelVoiceDataSize].
void generate_fake_calibration_data(uint8_t osc, uint32_t* out);
// Seed LittleFS with fake amp-comp + PW defaults + AmpComp440 = DIV_COUNTER/10.
// force=false: only if voiceTables file is missing. force=true: overwrite + precompute (cmd 30).
void seed_fake_calibration_tables(bool force = false);
// Truncate/create a LittleFS file and write a full bank in one shot.
// Shared with the bulk-restore path in preset_store.ino.
void write_fs_bank(const char* name, const uint8_t* data, size_t size);

// FS API prototypes. Explicit because Arduino's .ino prototype generation stops
// covering these once FS.ino becomes a one-line shim over the shared impl
// (same trap as the autotune extraction).
void init_FS();
void update_FS_voice(byte voiceN);
void update_FS_PWCenter(byte voiceN, uint16_t value);
void update_FS_PW_High_Limit(byte voiceN, uint16_t value);
void update_FS_PW_Low_Limit(byte voiceN, uint16_t value);
void update_FS_ManualCalibrationOffset(byte oscIndex, int8_t value);
void update_FS_AmpComp440(byte oscIndex, uint16_t value);
void update_FS_AmpCompDutyOffset(byte oscIndex, int16_t value);

#endif
