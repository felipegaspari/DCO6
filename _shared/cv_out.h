#ifndef __CV_OUT_H__
#define __CV_OUT_H__

// Software CV math (Phase 2). Hardware PWM writers arrive in Phase 3.

#ifndef NUM_FILTERS
#define NUM_FILTERS 2
#endif

void init_cv_out();
void update_CV_outs();
void update_CV_outs_manual_calibration();

// Bake CV mod depth scales (call after the matching depth global changes).
void cv_bake_adsr2_to_vcf_scale();
void cv_bake_lfo2_to_vcf_scale();
void cv_bake_lfo1_to_vca_scale();
void cv_update_mod_scales();  // all three (boot)

#endif
