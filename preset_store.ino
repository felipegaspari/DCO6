#include "include_all.h"
#include <LittleFS.h>

static const char PRESET_LAST_FILE[] = "pstLast";

int16_t presetParamShadow[PRESET_PARAM_COUNT];
uint8_t presetParamSetBitmap[PRESET_PARAM_COUNT / 8];
bool presetBootPending = true;

uint8_t currentPresetSlot = 0;

// --- IN-RAM PRESET STORE ---
uint8_t presetStoreRAM[PRESET_RAM_SLOTS][PRESET_RECORD_SIZE];
static uint8_t presetBulkStaging[PRESET_BULK_STAGING_SIZE];

static void preset_chunk_filename(uint8_t chunkIndex, char* out, size_t cap) {
  snprintf(out, cap, "pb%02u", (unsigned)chunkIndex);
}

static inline uint8_t preset_chunk_index(uint8_t slot) {
  return (uint8_t)(slot >> 2);
}

static inline uint16_t preset_slot_offset(uint8_t slot) {
  return (uint16_t)(slot & 3u) * PRESET_RECORD_SIZE;
}

static const char* preset_bulk_target_name(uint8_t target) {
  switch (target) {
    case PRESET_BULK_PRESET:            return "preset";
    case PRESET_BULK_VOICE_TABLES:      return "voiceTables";
    case PRESET_BULK_PW_3PT:            return "PWCal3Pt";
    case PRESET_BULK_AMP_COMP_TOP_PAIR: return "AmpCompTopPair";
    case PRESET_BULK_MANUAL_OFFSET:     return "ManualOffset";
    case PRESET_BULK_AMP_COMP_440:      return "AmpComp440";
    case PRESET_BULK_AMP_COMP_DUTY:     return "AmpCompDutyOffset";
    default:                            return "unknown";
  }
}

// 256-bit (32 byte) bitmap tracking validated RAM slots
// Bitmap tracking validated RAM slots
static uint32_t presetSlotValidBitmap[PRESET_RAM_SLOTS / 32];

static inline bool preset_is_slot_valid(uint8_t slot) {
  if (slot >= PRESET_RAM_SLOTS) return false;
  return (presetSlotValidBitmap[slot >> 5] & (1u << (slot & 31u))) != 0;
}

static inline void preset_set_slot_valid(uint8_t slot, bool valid) {
  if (slot >= PRESET_RAM_SLOTS) return;
  if (valid) {
    presetSlotValidBitmap[slot >> 5] |= (1u << (slot & 31u));
  } else {
    presetSlotValidBitmap[slot >> 5] &= ~(1u << (slot & 31u));
  }
}

// Deferred last-slot save state
static int16_t g_pending_last_slot = -1;
static uint32_t g_pending_last_slot_time = 0;

void preset_store_schedule_last_write(uint8_t slot) {
  g_pending_last_slot = (int16_t)slot;
  g_pending_last_slot_time = millis();
}

void preset_store_deferred_task() {
  if (g_pending_last_slot >= 0 && (millis() - g_pending_last_slot_time > 500)) {
    uint8_t slot = (uint8_t)g_pending_last_slot;
    g_pending_last_slot = -1;
    preset_store_write_last(slot);
  }
}

// --- One-Time Boot Loader (Loads all 153 KB into RAM) ---
// --- One-Time Boot Loader ---
void preset_store_init_ram() {
  memset(presetStoreRAM, 0, sizeof(presetStoreRAM));
  memset(presetSlotValidBitmap, 0, sizeof(presetSlotValidBitmap));
  char fname[8];

  for (uint8_t chunk = 0; chunk < PRESET_RAM_CHUNK_COUNT; ++chunk) {
    preset_chunk_filename(chunk, fname, sizeof(fname));
    if (!LittleFS.exists(fname)) continue;
    File f = LittleFS.open(fname, "r");
    if (!f || f.size() != PRESET_CHUNK_SIZE) {
      if (f) f.close();
      continue;
    }
    
    const uint8_t startSlot = chunk * PRESET_RECORDS_PER_FILE;
    f.read(&presetStoreRAM[startSlot][0], PRESET_CHUNK_SIZE);
    f.close();

    for (uint8_t i = 0; i < PRESET_RECORDS_PER_FILE; ++i) {
      uint8_t s = startSlot + i;
      if (preset_record_validate(presetStoreRAM[s])) {
        preset_set_slot_valid(s, true);
      }
    }
  }
}

// --- Dump text helpers ---

static void dump_print_begin(const char* target, int slot, uint32_t size) {
  if (slot >= 0) {
    Serial.printf("[dump] begin target=%s slot=%d size=%lu\n", target, slot, (unsigned long)size);
  } else {
    Serial.printf("[dump] begin target=%s size=%lu\n", target, (unsigned long)size);
  }
}

static void dump_print_data_line(uint16_t offset, const uint8_t* data, uint16_t len) {
  char line[96];
  int n = snprintf(line, sizeof(line), "[dump] d %04X ", (unsigned)offset);
  for (uint16_t i = 0; i < len && n + 2 < (int)sizeof(line); ++i) {
    n += snprintf(line + n, sizeof(line) - n, "%02X", data[i]);
  }
  Serial.println(line);
}

static void dump_print_end(const char* target, uint32_t crc) {
  Serial.printf("[dump] end target=%s crc=%08lX\n", target, (unsigned long)crc);
}

static void dump_print_err(const char* target, const char* reason) {
  Serial.printf("[dump] err target=%s reason=%s\n", target, reason);
}

static void dump_buffer(const char* target, int slot, const uint8_t* data, uint16_t size) {
  dump_print_begin(target, slot, size);
  for (uint16_t off = 0; off < size; off += PRESET_BULK_CHUNK_DATA) {
    uint16_t n = size - off;
    if (n > PRESET_BULK_CHUNK_DATA) n = PRESET_BULK_CHUNK_DATA;
    dump_print_data_line(off, data + off, n);
  }
  dump_print_end(target, preset_crc32(data, size));
}

static void dump_fs_file(const char* target, const char* filename, uint32_t expectedSize) {
  if (!LittleFS.exists(filename)) {
    dump_print_err(target, "missing");
    return;
  }
  File f = LittleFS.open(filename, "r");
  if (!f) {
    dump_print_err(target, "open");
    return;
  }
  const uint32_t fileSize = f.size();
  if (fileSize < expectedSize) {
    f.close();
    dump_print_err(target, "short");
    return;
  }
  const uint32_t size = expectedSize;
  dump_print_begin(target, -1, size);
  uint8_t chunk[PRESET_BULK_CHUNK_DATA];
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t off = 0;
  while (off < size) {
    uint16_t n = (uint16_t)((size - off > PRESET_BULK_CHUNK_DATA) ? PRESET_BULK_CHUNK_DATA : (size - off));
    f.read(chunk, n);
    crc = preset_crc32_update(crc, chunk, n);
    dump_print_data_line((uint16_t)off, chunk, n);
    off += n;
  }
  f.close();
  dump_print_end(target, crc ^ 0xFFFFFFFFu);
}

// --- Record build / validate / apply ---

static void preset_record_build(uint8_t* buf) {
  memset(buf, 0, PRESET_RECORD_SIZE);
  buf[PRESET_OFF_MAGIC]   = PRESET_MAGIC;
  buf[PRESET_OFF_VERSION] = PRESET_VERSION;

  for (int i = 0; i < 16; ++i) {
    buf[PRESET_OFF_NAME + i] = presetName[i];
  }

  // 100% of the preset lives in the shadow array and bitmap
  memcpy(buf + PRESET_OFF_BITMAP, presetParamSetBitmap, sizeof(presetParamSetBitmap));
  for (uint16_t id = 0; id < PRESET_PARAM_COUNT; ++id) {
    encode_u16_le(buf + PRESET_OFF_PARAMS + id * 2, (uint16_t)presetParamShadow[id]);
  }

  // CRC32 over the entire record payload up to OFF_CRC
  encode_u32_le(buf + PRESET_OFF_CRC, preset_crc32(buf, PRESET_OFF_CRC));
}

static void __not_in_flash_func(preset_record_apply)(const uint8_t* buf) {
  // 1. Direct restore from flash into RAM shadow
  memcpy(presetParamShadow, buf + PRESET_OFF_PARAMS, sizeof(presetParamShadow));
  memcpy(presetParamSetBitmap, buf + PRESET_OFF_BITMAP, sizeof(presetParamSetBitmap));

  // 2. Transmit burst updates to Mainboard using the freshly unpacked shadow values
  serial_send_adsr_vca_block_to_mb();
  serial_send_adsr_vcf_block_to_mb();
  serial_send_adsr_dco_block_to_mb();
  serial_send_filter_block_to_mb();
  serial_dma_poll_one(0);
  serial_send_patch_osc_block_to_mb();
  serial_send_patch_lfo_block_to_mb();
  serial_dma_poll_one(0);
  serial_send_patch_mod_block_to_mb();
  serial_send_patch_mix_block_to_mb();


  // 3. Apply local DCO engine state and bake filter/envelope scales
  dco_apply_preset_shadow();
  mark_adsr_params_dirty(ADSR_DIRTY_VCA_ALL | ADSR_DIRTY_VCF_ALL | ADSR_DIRTY_DCO_ALL);
  cv_bake_adsr2_to_vcf_scale();
  cv_bake_lfo2_to_vcf_scale();
}

static bool preset_record_validate(const uint8_t* buf) {
  if (buf[PRESET_OFF_MAGIC] != PRESET_MAGIC) return false;
  if (buf[PRESET_OFF_VERSION] != PRESET_VERSION) return false;
  return decode_u32_le(buf + PRESET_OFF_CRC) == preset_crc32(buf, PRESET_OFF_CRC);
}

static void preset_store_write_last(uint8_t slot) {
  File f = LittleFS.open(PRESET_LAST_FILE, "w");
  if (!f) return;
  f.write(&slot, 1);
  f.close();
}

static bool preset_chunk_write_record(uint8_t slot, const uint8_t* record, const char* errTag) {
  const uint8_t chunk = preset_chunk_index(slot);
  const uint16_t offset = preset_slot_offset(slot);
  char fname[8];
  preset_chunk_filename(chunk, fname, sizeof(fname));

  const bool exists = LittleFS.exists(fname);
  bool needCreate = !exists;
  if (exists) {
    File check = LittleFS.open(fname, "r");
    if (!check || check.size() != PRESET_CHUNK_SIZE) needCreate = true;
    if (check) check.close();
  }

  if (needCreate) {
    File f = LittleFS.open(fname, "w");
    if (!f) {
      Serial.printf("[%s] err slot=%u reason=open\n", errTag, (unsigned)slot);
      return false;
    }
    static uint8_t emptyRecord[PRESET_RECORD_SIZE];
    for (uint8_t i = 0; i < PRESET_RECORDS_PER_FILE; ++i) {
      const uint8_t* src = ((slot & 3u) == i) ? record : emptyRecord;
      if (f.write(src, PRESET_RECORD_SIZE) != PRESET_RECORD_SIZE) {
        f.close();
        Serial.printf("[%s] err slot=%u reason=write\n", errTag, (unsigned)slot);
        return false;
      }
    }
    f.close();
    return true;
  }

  File f = LittleFS.open(fname, "r+");
  if (!f || !f.seek(offset) || f.write(record, PRESET_RECORD_SIZE) != PRESET_RECORD_SIZE) {
    if (f) f.close();
    Serial.printf("[%s] err slot=%u reason=write\n", errTag, (unsigned)slot);
    return false;
  }
  f.close();
  return true;
}

// --- Public API ---

void preset_store_save(uint8_t slot) {
  if (slot >= PRESET_RAM_SLOTS) return;
  currentPresetSlot = slot;
  preset_record_build(presetStoreRAM[slot]);
  if (!preset_chunk_write_record(slot, presetStoreRAM[slot], "preset")) return;
  preset_set_slot_valid(slot, true);
  preset_store_schedule_last_write(slot);
  Serial.printf("[preset] saved slot=%u name=\"%.16s\"\n", (unsigned)slot, (const char*)presetName);
}

bool __not_in_flash_func(preset_store_load)(uint8_t slot) {
  if (slot >= PRESET_RAM_SLOTS) return false;
  currentPresetSlot = slot;
  const bool isValid = preset_is_slot_valid(slot);
  const uint8_t* record = presetStoreRAM[slot];
 
  if (isValid) {
    memcpy(presetName, record + PRESET_OFF_NAME, 16);
  } else {
    memset(presetName, ' ', 16);
  }

  serial_send_preset_scroll_to_mb(slot);
  serial_send_preset_loaded_to_mb(slot);
  serial_send_screen_signal_to_mb(SCREEN_SIGNAL_SILENT);

  if (isValid) {
    preset_record_apply(record);
  }

  serial_send_screen_signal_to_mb(1);
  return isValid;
}

static uint8_t presetDirPushChunk = PRESET_CHUNK_COUNT;

void preset_store_send_directory_to_mb() {
  presetDirPushChunk = 0;
}

void preset_store_dir_push_task() {
  if (presetDirPushChunk >= PRESET_CHUNK_COUNT) return;

  const uint8_t chunk = presetDirPushChunk++;
  uint8_t entry[1 + PRESET_NAME_LEN];

  for (uint8_t i = 0; i < PRESET_RECORDS_PER_FILE; ++i) {
    const uint8_t slot = (uint8_t)((chunk << 2) | i);
    entry[0] = slot;
    
    // Slots in RAM report their stored name; slots above PRESET_RAM_SLOTS report empty
    if (slot < PRESET_RAM_SLOTS && presetStoreRAM[slot][PRESET_OFF_MAGIC] == PRESET_MAGIC) {
      memcpy(entry + 1, &presetStoreRAM[slot][PRESET_OFF_NAME], PRESET_NAME_LEN);
    } else {
      memset(entry + 1, 0, PRESET_NAME_LEN);
    }

    serial_frame_write(Serial2Dma, CMD_PRESET_DIR_ENTRY, entry, sizeof(entry));
  }
}

void preset_store_dump(int16_t sel) {
  if (sel < 0) {
    Serial.println("[pdir] begin");
    uint16_t count = 0;
    for (uint16_t slot = 0; slot < PRESET_RAM_SLOTS; ++slot) {
      if (presetStoreRAM[slot][PRESET_OFF_MAGIC] == PRESET_MAGIC) {
        char name[PRESET_NAME_LEN + 1];
        memcpy(name, &presetStoreRAM[slot][PRESET_OFF_NAME], PRESET_NAME_LEN);
        name[PRESET_NAME_LEN] = 0;
        Serial.printf("[pdir] slot=%03u name=\"%s\"\n", (unsigned)slot, name);
        ++count;
      }
    }
    Serial.printf("[pdir] end count=%u\n", (unsigned)count);
    return;
  }

  if (sel >= (int16_t)PRESET_RAM_SLOTS) return;
  if (presetStoreRAM[sel][PRESET_OFF_MAGIC] != PRESET_MAGIC) {
    dump_print_err("preset", "empty");
    return;
  }
  dump_buffer("preset", sel, presetStoreRAM[sel], PRESET_RECORD_SIZE);
}

// ✅ UPGRADED: Dumps 3-Point PW and Top-Pair Data Banks
void preset_store_cal_dump(int16_t sel) {
  const bool all = (sel <= CAL_DUMP_ALL);
  if (all || sel == CAL_DUMP_VOICE_TABLES)      dump_fs_file("voiceTables", "voiceTables", FSBankSize);
  if (all || sel == CAL_DUMP_PW_3PT)            dump_fs_file("PWCal3Pt", "PWCal3Pt", FSPWBankSize);
  if (all || sel == CAL_DUMP_AMP_COMP_TOP_PAIR) dump_fs_file("AmpCompTopPair", "AmpCompTopPair", FSAmpCompTopPairBankSize);
  if (all || sel == CAL_DUMP_MANUAL_OFFSET)     dump_fs_file("ManualOffset", "ManualOffset", FSManualOffsetBankSize);
  if (all || sel == CAL_DUMP_AMP_COMP_440)      dump_fs_file("AmpComp440", "AmpComp440", FSAmpComp440BankSize);
  if (all || sel == CAL_DUMP_AMP_COMP_DUTY)     dump_fs_file("AmpCompDutyOffset", "AmpCompDutyOffset", FSAmpCompDutyOffsetBankSize);
}

void preset_bulk_chunk(const uint8_t* payload, uint8_t len) {
  if (len != SERIAL_LEN_BULK_CHUNK) return;
  const uint16_t offset = decode_u16_le(payload + 2);
  if ((uint32_t)offset + PRESET_BULK_CHUNK_DATA > PRESET_BULK_STAGING_SIZE) return;
  memcpy(presetBulkStaging + offset, payload + 4, PRESET_BULK_CHUNK_DATA);
}

// ✅ UPGRADED: Commits 3-Point PW and Top-Pair Restores
void preset_bulk_commit(const uint8_t* payload, uint8_t len) {
  if (len != SERIAL_LEN_BULK_COMMIT) return;
  const uint8_t  target = payload[0];
  const uint8_t  slot   = payload[1];
  const uint16_t size   = decode_u16_le(payload + 2);
  const uint32_t crc    = decode_u32_le(payload + 4);
  const char* tname = preset_bulk_target_name(target);

  if (size == 0 || size > PRESET_BULK_STAGING_SIZE) return;
  if (preset_crc32(presetBulkStaging, size) != crc) return;

  if (target == PRESET_BULK_PRESET) {
    if (slot >= PRESET_RAM_SLOTS) return;
    if (!preset_record_validate(presetBulkStaging)) return;
    memcpy(presetStoreRAM[slot], presetBulkStaging, PRESET_RECORD_SIZE);
    if (!preset_chunk_write_record(slot, presetStoreRAM[slot], "bulk")) return;
    Serial.printf("[bulk] ok target=%s slot=%u\n", tname, (unsigned)slot);
    return;
  }

  uint16_t want = 0;
  const char* calFile = nullptr;
  switch (target) {
    case PRESET_BULK_VOICE_TABLES:      want = FSBankSize;                  calFile = "voiceTables"; break;
    case PRESET_BULK_PW_3PT:            want = FSPWBankSize;                calFile = "PWCal3Pt"; break;
    case PRESET_BULK_AMP_COMP_TOP_PAIR: want = FSAmpCompTopPairBankSize;    calFile = "AmpCompTopPair"; break;
    case PRESET_BULK_MANUAL_OFFSET:     want = FSManualOffsetBankSize;      calFile = "ManualOffset"; break;
    case PRESET_BULK_AMP_COMP_440:      want = FSAmpComp440BankSize;        calFile = "AmpComp440"; break;
    case PRESET_BULK_AMP_COMP_DUTY:     want = FSAmpCompDutyOffsetBankSize; calFile = "AmpCompDutyOffset"; break;
    default: return;
  }
  if (size != want) return;

  write_fs_bank(calFile, presetBulkStaging, size);
  init_FS();
  if (target == PRESET_BULK_VOICE_TABLES) precompute_amp_comp_for_engine();
  Serial.printf("[bulk] ok target=%s\n", tname);
}

void preset_store_boot_recall() {
  if (calibrationFlag) return;
  if (!LittleFS.exists(PRESET_LAST_FILE)) return;
  File f = LittleFS.open(PRESET_LAST_FILE, "r");
  if (!f) return;
  uint8_t slot = 0;
  const bool ok = (f.read(&slot, 1) == 1);
  f.close();
  if (!ok) return;
  preset_store_load(slot);
}


//// DEBUG - do  not delete this function
// void preset_debug_print_all(uint8_t slot) {
//   if (slot >= PRESET_NUM_SLOTS) return;
  
//   const uint8_t* record = presetStoreRAM[slot];
//   const bool isValid = preset_record_validate(record);

//   Serial.println(F("\n================================================================="));
//   Serial.printf(" PRESET DUMP - SLOT %03u [%s]\n", (unsigned)slot, isValid ? "VALID" : "INVALID / EMPTY");
//   Serial.println(F("================================================================="));

//   char nameBuf[17];
//   memcpy(nameBuf, record + PRESET_OFF_NAME, 16);
//   nameBuf[16] = '\0';
//   Serial.printf(" Name:       \"%s\"\n", nameBuf);
//   Serial.printf(" CRC32:      0x%08lX\n", (unsigned long)decode_u32_le(record + PRESET_OFF_CRC));

//   // =========================================================================
//   // 1. Envelopes & Filter (Unpacked from PRESET_OFF_BLOCKS)
//   // =========================================================================
//   const uint8_t* b = record + PRESET_OFF_BLOCKS;
//   Serial.println(F("\n--- [ ENVELOPES & FILTER ] ---"));
//   Serial.printf(" EnvVCA (ADSR): A=%-5u D=%-5u S=%-5u R=%-5u\n",
//                 decode_u16_le(b + 0), decode_u16_le(b + 2), decode_u16_le(b + 4), decode_u16_le(b + 6));
//   Serial.printf(" EnvVCF (ADSR): A=%-5u D=%-5u S=%-5u R=%-5u\n",
//                 decode_u16_le(b + 8), decode_u16_le(b + 10), decode_u16_le(b + 12), decode_u16_le(b + 14));
//   Serial.printf(" EnvDCO (ADSR): A=%-5u D=%-5u S=%-5u R=%-5u\n",
//                 decode_u16_le(b + 16), decode_u16_le(b + 18), decode_u16_le(b + 20), decode_u16_le(b + 22));
//   Serial.printf(" Filter:        Cutoff=%-5u Reso=%-5u Env2Depth=%-5d LFO2Depth=%-5u\n",
//                 decode_u16_le(b + 24), decode_u16_le(b + 26), (int16_t)decode_u16_le(b + 28), decode_u16_le(b + 30));

//   // =========================================================================
//   // 2. Oscillators, Pitch & Modes (from PRESET_OFF_PARAMS)
//   // =========================================================================
//   const int16_t* p = (const int16_t*)(record + PRESET_OFF_PARAMS);
//   Serial.println(F("\n--- [ OSCILLATORS & VOICE ] ---"));
//   Serial.printf(" OSC1: Saw=%d Pulse=%d Tri=%d | Octave=%d\n",
//                 p[PARAM_OSC1_SAW_ENABLE], p[PARAM_OSC1_PULSE_ENABLE], p[PARAM_OSC1_TRI_ENABLE], p[PARAM_OSC1_INTERVAL]);
//   Serial.printf(" OSC2: Saw=%d Pulse=%d Tri=%d | Interval=%d Detune=%u\n",
//                 p[PARAM_OSC2_SAW_ENABLE], p[PARAM_OSC2_PULSE_ENABLE], p[PARAM_OSC2_TRI_ENABLE], p[PARAM_OSC2_INTERVAL], (unsigned)p[PARAM_OSC2_DETUNE_VAL]);
//   Serial.printf(" OSC3: Saw=%d Pulse=%d Tri=%d | Interval=%d\n",
//                 p[PARAM_OSC3_SAW_ENABLE], p[PARAM_OSC3_PULSE_ENABLE], p[PARAM_OSC3_TRI_ENABLE], p[PARAM_OSC3_INTERVAL]);
//   Serial.printf(" Voice: Mode=%u Alloc=%u UnisonDetune=%d Sync=%u SoftSync=%u SubDiv=%u\n",
//                 (unsigned)p[PARAM_VOICE_MODE], (unsigned)p[PARAM_VOICE_ALLOC_MODE], p[PARAM_UNISON_DETUNE],
//                 (unsigned)p[PARAM_SYNC_MODE], (unsigned)p[PARAM_SOFT_SYNC], (unsigned)p[PARAM_SUBOSC_DIVIDE]);
//   Serial.printf(" Portamento: Time=%u Mode=%u | PW=%u\n",
//                 (unsigned)p[PARAM_PORTAMENTO_TIME], (unsigned)p[PARAM_PORTAMENTO_MODE], (unsigned)p[PARAM_PW_VALUE]);
//   Serial.printf(" Drift/Char: Amount=%d Speed=%d Spread=%d | Character=%u\n",
//                 p[PARAM_ANALOG_DRIFT_AMOUNT], p[PARAM_ANALOG_DRIFT_SPEED], p[PARAM_ANALOG_DRIFT_SPREAD], (unsigned)p[PARAM_CHARACTER]);

//   // =========================================================================
//   // 3. LFOs & Pitch Routing
//   // =========================================================================
//   Serial.println(F("\n--- [ LFOS ] ---"));
//   Serial.printf(" LFO1: Wave=%u Speed=%-5u -> DCO=%-5u OSC1=%-3u OSC2=%-3u OSC3=%-3u VCA=%-5u\n",
//                 (unsigned)p[PARAM_LFO1_WAVEFORM], (unsigned)p[PARAM_LFO1_SPEED], (unsigned)p[PARAM_LFO1_TO_DCO],
//                 (unsigned)p[PARAM_LFO1_TO_OSC1], (unsigned)p[PARAM_LFO1_TO_OSC2], (unsigned)p[PARAM_LFO1_TO_OSC3], (unsigned)p[PARAM_LFO1_TO_VCA]);
//   Serial.printf(" LFO2: Wave=%u Speed=%-5u -> OSC2=%-5u OSC3=%-5u Coarse2=%-3u Coarse3=%-3u PW=%-5u\n",
//                 (unsigned)p[PARAM_LFO2_WAVEFORM], (unsigned)p[PARAM_LFO2_SPEED], (unsigned)p[PARAM_LFO2_TO_OSC2],
//                 (unsigned)p[PARAM_LFO2_TO_OSC3], (unsigned)p[PARAM_LFO2_TO_OSC2_COARSE], (unsigned)p[PARAM_LFO2_TO_OSC3_COARSE], (unsigned)p[PARAM_LFO2_TO_PW]);

//   // =========================================================================
//   // 4. Mixer Levels, Analog CVs & Curves
//   // =========================================================================
//   Serial.println(F("\n--- [ MIXER & ANALOG CV ] ---"));
//   Serial.printf(" Levels: OSC1=%-3u OSC2=%-3u OSC3=%-3u SUB=%-3u VCA=%-3u\n",
//                 (unsigned)p[PARAM_OSC1_LEVEL], (unsigned)p[PARAM_OSC2_LEVEL], (unsigned)p[PARAM_OSC3_LEVEL], (unsigned)p[PARAM_SUB_LEVEL], (unsigned)p[PARAM_VCA_LEVEL]);
//   Serial.printf(" Routing: FilterMode=%u Keytrack=%-5d VelToVCF=%-3d VelToVCA=%-3d EnvToVCA=%-5d\n",
//                 (unsigned)p[PARAM_FILTER_MODE], p[PARAM_VCF_KEYTRACK], p[PARAM_VELOCITY_TO_VCF], p[PARAM_VELOCITY_TO_VCA], p[PARAM_ADSR1_TO_VCA]);
//   Serial.printf(" Curves: VCA_Atk=%u VCA_Dec=%u VCF_Atk=%u VCF_Dec=%u\n",
//                 (unsigned)p[PARAM_ADSR1_ATTACK_CURVE], (unsigned)p[PARAM_ADSR1_DECAY_CURVE], (unsigned)p[PARAM_ADSR2_ATTACK_CURVE], (unsigned)p[PARAM_ADSR2_DECAY_CURVE]);
//   Serial.printf(" Distortion: Drive=%-5u Mix=%-5u | Switches: ResComp=%d VCA_Rst=%d VCF_Rst=%d\n",
//                 (unsigned)p[PARAM_DIST_DRIVE], (unsigned)p[PARAM_DIST_MIX],
//                 p[PARAM_RESONANCE_COMPENSATION], p[PARAM_VCA_ADSR_RESTART], p[PARAM_VCF_ADSR_RESTART]);

//   // =========================================================================
//   // 5. Modulation Matrix (Slots 0..7)
//   // =========================================================================
//   Serial.println(F("\n--- [ MODULATION MATRIX ] ---"));
//   for (uint8_t i = 0; i < 8; ++i) {
//     uint8_t src = (uint8_t)p[PARAM_MOD_SLOT0_SOURCE + i * 3];
//     uint8_t dst = (uint8_t)p[PARAM_MOD_SLOT0_DEST   + i * 3];
//     int16_t dep = p[PARAM_MOD_SLOT0_DEPTH  + i * 3];
//     if (src != 0xFF && src != 0 && dep != 0) {
//       Serial.printf("  Slot %u: Source=%-3u -> Dest=%-3u [Depth=%-5d]\n", i, src, dst, dep);
//     } else {
//       Serial.printf("  Slot %u: [OFF] (Src=%u Dest=%u Depth=%d)\n", i, src, dst, dep);
//     }
//   }

//   Serial.println(F("=================================================================\n"));
// }