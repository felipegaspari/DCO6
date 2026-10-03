/**
 * @file DCO-cv_out.ino
 * @brief DCO Control Voltage & Modulation Math Engine (RP2040/RP2350).
 */

 #include "include_all.h"
 #include <string.h>
 
 // =============================================================================
 // 1. DCO SHADOW STORAGE & HARDWARE DELTA BUFFERS
 // =============================================================================
 uint16_t ADSR_VCA_attack  = 0, ADSR_VCA_decay  = 0, ADSR_VCA_sustain  = 4095, ADSR_VCA_release  = 0;
 uint16_t ADSR_VCF_attack  = 0, ADSR_VCF_decay  = 0, ADSR_VCF_sustain  = 4095, ADSR_VCF_release  = 0;
 bool     ADSR1Restart     = true, ADSR2Restart = true, ADSR3Restart   = true;
 
 uint8_t  ADSR1AttackCurveVal = 1, ADSR1DecayCurveVal = 2, ADSR1ReleaseCurveVal = 1;
 uint8_t  ADSR2AttackCurveVal = 4, ADSR2DecayCurveVal = 6, ADSR2ReleaseCurveVal = 1;
 uint8_t  ADSR3AttackCurveVal = 7, ADSR3DecayCurveVal = 7, ADSR3ReleaseCurveVal = 7;
 
 volatile uint16_t CUTOFF = 1024, RESONANCE = 0, LFO2toVCF = 0, VCALevel = 4095, LFO1toVCA = 0;
 volatile int16_t  ADSR2toVCF = 0, ADSR1toVCA = 0;
 uint16_t DIST_DRIVE = 0, DIST_MIX = 0;
 uint8_t  FILTER_MODE = 0;
 
 volatile int32_t  ADSR2toVCF_scale_q15 = 0, LFO2toVCF_scale_q15 = 0, LFO1toVCA_scale_q15 = 0;
 volatile int32_t  velocityToVCF_q15 = 0, velocityToVCA_q15 = 0, vcf_drift_scale_q15 = 0;
 volatile int16_t  VCF_DRIFT[NUM_VOICES_TOTAL] = {0};
 
 // Hardware modulation delta buffers
 #if defined(USE_FLOAT_VOICE_TASK)
 volatile float matrix_pitch_mod_f[NUM_VOICES_TOTAL]           = {0};
 volatile float matrix_osc1_pitch_mod_f[NUM_VOICES_TOTAL]      = {0};
 volatile float matrix_osc2_pitch_mod_f[NUM_VOICES_TOTAL]      = {0};
 #endif
 
 volatile int32_t matrix_pitch_mod_q24[NUM_VOICES_TOTAL]       = {0};
 volatile int32_t matrix_osc1_pitch_mod_q24[NUM_VOICES_TOTAL]  = {0};
 volatile int32_t matrix_osc2_pitch_mod_q24[NUM_VOICES_TOTAL]  = {0};
 volatile int32_t matrix_pw_mod[NUM_VOICES_TOTAL]              = {0};
 volatile int32_t matrix_xmod_mod[NUM_VOICES_TOTAL]            = {0};
 
 bool     RESONANCEAmpCompensation = true;
 volatile int16_t  VCAResonanceCompensation = 100, VCFKeytrack = 0;
 int8_t   velocityToVCFVal = 0, velocityToVCAVal = 0;
 
 SRAM_DATA uint16_t VCA_PWM[NUM_VOICES_TOTAL]        = {0};
 SRAM_DATA uint16_t VCF_PWM[NUM_VOICES_TOTAL]        = {0};
 SRAM_DATA uint16_t RESONANCE_PWM[NUM_FILTERS]       = {0};
 SRAM_DATA uint16_t AS2164_VCA_linearize_table[4096] = {0};
 
 volatile int16_t  OSC1LevelVal = 0, OSC2LevelVal = 0, OSC3LevelVal = 0, SubLevelVal = 0;
 volatile uint16_t OSC1Level = 0, OSC2Level = 0, OSC3Level = 0, SubLevel = 0;
 bool     ADSR3Enabled = false;
 
 // =============================================================================
 // MODULATION MATRIX STORAGE DEFINITIONS (Instantiated for DCO)
 // =============================================================================
 SRAM_DATA ModSlot mod_slots[8];
 SRAM_DATA alignas(8) int32_t voice_mod_sums[MAX_SUPPORTED_VOICES][MOD_DEST_COUNT];
 SRAM_DATA int32_t prev_depth_mods[MAX_SUPPORTED_VOICES][8];
 
 SRAM_DATA int16_t aftertouch_q15 = 0;
 SRAM_DATA int16_t mod_wheel_q15  = 0;
 SRAM_DATA int16_t expression_q15 = 0;
 SRAM_DATA int16_t breath_q15     = 0;
 SRAM_DATA int16_t random_sh_q15[MAX_SUPPORTED_VOICES] = {0};
 
 // =============================================================================
 // 2. BOOT & SCALE INITIALIZATION
 // =============================================================================
 #ifdef ENABLE_CV_OUTS
 static constexpr int32_t CV_U12_MAX = 4095;
 static inline uint16_t cv_clamp_u12(int32_t v) {
   if (v < 0) return 0;
   if (v > CV_U12_MAX) return (uint16_t)CV_U12_MAX;
   return (uint16_t)v;
 }
 static inline uint16_t lerp_0_4095(uint16_t value, uint16_t y0, uint16_t y1) {
   return (uint16_t)((int32_t)y0 + ((((int32_t)y1 - (int32_t)y0) * (int32_t)value) >> 12));
 }
 static inline uint16_t cv_q15_to_u12(int16_t q15) {
   return cv_clamp_u12((int32_t)q15 >> 3);
 }
 #endif
 
 void init_cv_out() {
 #ifdef ENABLE_CV_OUTS
   generateBezierArray({ 0, 4095 }, { 4095, 0 }, { 150, 1420 }, { -235, 815 }, 4096, AS2164_VCA_linearize_table);
   cv_update_mod_scales();
 #endif
 
   for (uint8_t i = 0; i < NUM_VOICES_TOTAL; i++) {
 #if defined(USE_FLOAT_VOICE_TASK)
     matrix_pitch_mod_f[i]      = 0.0f;
     matrix_osc1_pitch_mod_f[i] = 0.0f;
     matrix_osc2_pitch_mod_f[i] = 0.0f;
 #else
     matrix_pitch_mod_q24[i]      = 0;
     matrix_osc1_pitch_mod_q24[i] = 0;
     matrix_osc2_pitch_mod_q24[i] = 0;
 #endif
     matrix_pw_mod[i]   = 0;
     matrix_xmod_mod[i] = 0;
   }
 }
 
 void cv_bake_adsr2_to_vcf_scale() { ADSR2toVCF_scale_q15 = (int32_t)ADSR2toVCF << 3; }
 void cv_bake_lfo2_to_vcf_scale()  { LFO2toVCF_scale_q15  = -((int32_t)LFO2toVCF << 2); }
 void cv_bake_lfo1_to_vca_scale()  { LFO1toVCA_scale_q15  = -((int32_t)LFO1toVCA << 2); }
 void cv_update_mod_scales() {
   cv_bake_adsr2_to_vcf_scale();
   cv_bake_lfo2_to_vcf_scale();
   cv_bake_lfo1_to_vca_scale();
 }

 static constexpr uint32_t LFO_DEST_SLEW_US = 201u;

 // Commit LFO Hz only when dest or SpeedVal changed. Dest-only changes are
 // rate-limited; SpeedVal changes commit immediately. last-* update after commit.
 static inline __attribute__((always_inline))
 void SRAM_HOT(lfo_commit_speed_if_changed)(
     lfo& obj, int32_t dest, uint32_t speed,
     int32_t& last_dest, uint32_t& last_speed,
     uint32_t& last_dest_flush_us) {
   if (__builtin_expect((dest != last_dest) | (speed != last_speed), 0)) {
     if (__builtin_expect(speed != last_speed, 0) ||
         (timer_hw->timerawl - last_dest_flush_us) >= LFO_DEST_SLEW_US) {
       last_dest = dest;
       last_speed = speed;
       last_dest_flush_us = timer_hw->timerawl;
       obj.setMode0Freq(fast_exp_speed_5000((uint32_t)((int32_t)speed + dest)));
     }
   }
 }
 
 // =============================================================================
 // 3. REALTIME CV & MODULATION MATRIX EXECUTION LOOP
 // =============================================================================
 void SRAM_HOT(update_CV_outs)() {
 
   // =========================================================================
   // A. Calibration Guard (Checked BEFORE benchmark macros to prevent corrupt spans)
   // =========================================================================
   if (__builtin_expect(manualCalibrationFlag || calibrationFlag, 0)) {
     for (uint8_t i = 0; i < NUM_VOICES_TOTAL; i++) {
       mod_matrix_clear_voice(i);
 #if defined(USE_FLOAT_VOICE_TASK)
       matrix_pitch_mod_f[i]      = 0.0f;
       matrix_osc1_pitch_mod_f[i] = 0.0f;
       matrix_osc2_pitch_mod_f[i] = 0.0f;
 #else
       matrix_pitch_mod_q24[i]      = 0;
       matrix_osc1_pitch_mod_q24[i] = 0;
       matrix_osc2_pitch_mod_q24[i] = 0;
 #endif
       matrix_pw_mod[i]   = 0;
       matrix_xmod_mod[i] = 0;
     }
     return;
   }
 
   // =========================================================================
   // B. Ingest All Polyphonic Voice Sources (100% Integer Q15)
   // =========================================================================
   ModSources sources;
   {
     BENCH_BEGIN(cv_ingest);
     sources.lfo1         = LFO1Level;
     sources.lfo2         = LFO2Level;
     sources.lfo3         = LFO3Level;             
     sources.noise        = (int16_t)noiseLevel[0];         
     sources.pitch_bend   = (int16_t)((int32_t)midi_pitch_bend - 8192);
     sources.drift_global = LFO_DRIFT_LEVEL[0];    
     sources.expression   = expression_q15;        
     sources.breath       = breath_q15;            
 
     _Pragma("GCC unroll 4")
     for (uint8_t i = 0; i < NUM_VOICES_TOTAL; i++) {
       const uint8_t oscA = (uint8_t)(i * 2);
       sources.drift_voice[i]   = LFO_DRIFT_LEVEL[oscA];
       sources.env_vca[i]       = ADSR_VCA_Level_q15[i];
       sources.env_dco[i]       = ADSR3Level_q15[i];
       sources.env_vcf[i]       = ADSR_VCF_Level_q15[i]; 
       sources.velocity[i]      = velocity[i];
       sources.keytrack_note[i] = VOICE_NOTES[i] ? VOICE_NOTES[i] : 60;
     }
     BENCH_END(cv_ingest);
   }
 
   // =========================================================================
   // C. Execute Matrix Engine (Polyphonic MAC Core)
   // =========================================================================
   {
     BENCH_BEGIN(cv_matrix);
     mod_matrix_accumulate_all(&sources, NUM_VOICES_TOTAL);
     BENCH_END(cv_matrix);
   }
 
   // =========================================================================
   // D. Extract Hardware Deltas (Branch between Float & Q24)
   // =========================================================================
   {
     BENCH_BEGIN(cv_deltas);
     _Pragma("GCC unroll 4")
     for (uint8_t i = 0; i < NUM_VOICES_TOTAL; i++) {
 #if defined(USE_FLOAT_VOICE_TASK)
       // Converts raw int32 matrix sum to Float Octaves (1.0f = 1 Octave)
       matrix_pitch_mod_f[i]      = mod_matrix_get_dest_float<DEST_PITCH>(i);
       matrix_osc1_pitch_mod_f[i] = mod_matrix_get_dest_float<DEST_OSC1_PITCH>(i);
       matrix_osc2_pitch_mod_f[i] = mod_matrix_get_dest_float<DEST_OSC2_PITCH>(i);
 #else
       // RP2040 Q24 bit-shift path
       matrix_pitch_mod_q24[i]      = mod_matrix_get_dest_fast<DEST_PITCH>(i);
       matrix_osc1_pitch_mod_q24[i] = mod_matrix_get_dest_fast<DEST_OSC1_PITCH>(i);
       matrix_osc2_pitch_mod_q24[i] = mod_matrix_get_dest_fast<DEST_OSC2_PITCH>(i);
 #endif
       matrix_pw_mod[i]   = mod_matrix_get_dest_fast<DEST_PW>(i);
       matrix_xmod_mod[i] = mod_matrix_get_dest_fast<DEST_CROSSMOD_DEPTH>(i);
     }
     __dmb(); // Memory barrier: Ensure Core 1 voice task reads fresh float deltas
     BENCH_END(cv_deltas);
   }
 
   // =========================================================================
   // E. LFO Frequency Commit (dest or SpeedVal change)
   // =========================================================================
   {
     static int32_t last_l1_dest = 0, last_l2_dest = 0, last_l3_dest = 0;
     static uint32_t last_l1_speed = 0, last_l2_speed = 0, last_l3_speed = 0;
     static uint32_t last_dest_flush_us = 0;

     const int32_t l1_dest = mod_matrix_get_dest_fast<DEST_LFO1_SPEED>(0);
     const uint32_t l1_speed = (uint32_t)LFO1SpeedVal;
     const int32_t l2_dest = mod_matrix_get_dest_fast<DEST_LFO2_SPEED>(0);
     const uint32_t l2_speed = (uint32_t)LFO2SpeedVal;
     const int32_t l3_dest = mod_matrix_get_dest_fast<DEST_LFO3_SPEED>(0);
     const uint32_t l3_speed = (uint32_t)LFO3SpeedVal;

     if (__builtin_expect(
           (l1_dest != last_l1_dest) | (l1_speed != last_l1_speed) |
           (l2_dest != last_l2_dest) | (l2_speed != last_l2_speed) |
           (l3_dest != last_l3_dest) | (l3_speed != last_l3_speed), 0)) {
       BENCH_BEGIN(cv_lfo_subloop);
       lfo_commit_speed_if_changed(LFO1_class, l1_dest, l1_speed,
                                   last_l1_dest, last_l1_speed, last_dest_flush_us);
       lfo_commit_speed_if_changed(LFO2_class, l2_dest, l2_speed,
                                   last_l2_dest, last_l2_speed, last_dest_flush_us);
       lfo_commit_speed_if_changed(LFO3_class, l3_dest, l3_speed,
                                   last_l3_dest, last_l3_speed, last_dest_flush_us);
       BENCH_END(cv_lfo_subloop);
     }
   }
 }
 
 void update_CV_outs_manual_calibration() {
 #ifndef ENABLE_CV_OUTS
   byte stage = (byte)manualCalibrationStage;
   waveSelector_manual_calibration(stage);
 #endif
 }