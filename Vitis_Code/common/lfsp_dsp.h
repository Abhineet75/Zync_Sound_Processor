// =============================================================================
// lfsp_dsp.h : block processing for the low-frequency signal processor
// =============================================================================
//
// Portable C (no Xilinx headers): the same file is compiled for the Cortex-A9
// in Vitis and for the host (Mac/PC) by Signal_Processing/host_test.
//
// Per 512-sample block from the decimator:
//   1. DC removal: subtract the block mean -> signed samples x[n]
//   2. Goertzel filter at every bin of two bands
//        rumble : 10 - 40 Hz   (elephant rumble fundamental)
//        mains  : 48 - 52 Hz   (50 Hz hum)
//      bin k sits at k * fs / N  (fs = 939.002 Hz, N = 512 -> 1.834 Hz per bin)
//   3. Band share of total power, in per mille:
//        share = 2 * sum(|X_k|^2 over band) / (N * sum(x[n]^2))
//      (Parseval: a pure tone exactly on one bin gives 1000)
//   4. Detection with a hold time (see dsp_detect)
//
// Two implementations of steps 1-3, selectable per block:
//   dsp_block_q15   : integer only. x in Q15 (int16), Goertzel coefficient
//                     2cos(w) in Q14 (int32), filter state int32, energies int64.
//   dsp_block_float : the same algorithm in single-precision float, for the
//                     float-vs-fixed timing and accuracy comparison.
// =============================================================================

#ifndef LFSP_DSP_H
#define LFSP_DSP_H

#include <stdint.h>

#define DSP_N              512
#define DSP_FS_HZ          939.002f     // measured on the board (Day 3)
#define DSP_MAX_BINS       48

// Band edges in Hz (converted to bin ranges in dsp_init)
#define DSP_RUMBLE_LO_HZ   9.0f
#define DSP_RUMBLE_HI_HZ   41.0f
#define DSP_MAINS_LO_HZ    47.0f
#define DSP_MAINS_HI_HZ    52.0f

// Detection rules
#define DSP_MIN_RMS        300u         // below this (decimated units) the block is "quiet"
#define DSP_RUMBLE_PM      600u         // rumble band must hold >= 60 % of the power
#define DSP_MAINS_PM       600u         // mains band must hold >= 60 % of the power
#define DSP_HOLD_BLOCKS    2            // same verdict on 2 consecutive blocks (~1.09 s)

typedef enum {
    DET_NONE   = 0,     // signal present, but neither band dominates
    DET_RUMBLE = 1,
    DET_MAINS  = 2,
    DET_QUIET  = 3      // too little signal to judge
} det_label_t;

typedef struct {
    float   fs;
    int     rumble_lo, rumble_hi;         // inclusive bin ranges
    int     mains_lo,  mains_hi;
    int     nbins;
    int     k[DSP_MAX_BINS];              // all bins: rumble first, then mains
    int     is_mains[DSP_MAX_BINS];
    int32_t coeff_q14[DSP_MAX_BINS];      // round(2cos(2 pi k / N) * 2^14)
    float   coeff_f[DSP_MAX_BINS];        // 2cos(2 pi k / N)
} dsp_cfg_t;

typedef struct {
    // Energies in (decimated units)^2, as double so both paths can be compared
    double   rumble_e;                    // sum |X_k|^2 over the rumble bins
    double   mains_e;                     // sum |X_k|^2 over the mains bins
    double   total_e;                     // sum x[n]^2  (time domain)
    uint32_t rumble_pm;                   // rumble share of total power, per mille
    uint32_t mains_pm;                    // mains share, per mille
    uint32_t rms;                         // RMS of x[n], decimated units
} dsp_result_t;

typedef struct {
    det_label_t candidate;                // verdict of the latest block alone
    int         count;                    // consecutive blocks with that verdict
    det_label_t output;                   // confirmed verdict
} det_state_t;

void        dsp_init(dsp_cfg_t *cfg, float fs);
void        dsp_block_q15(const dsp_cfg_t *cfg, const uint16_t *in, dsp_result_t *res);
void        dsp_block_float(const dsp_cfg_t *cfg, const uint16_t *in, dsp_result_t *res);
void        dsp_detect_init(det_state_t *st);
det_label_t dsp_classify(const dsp_result_t *res);          // one block, no hold
det_label_t dsp_detect(det_state_t *st, const dsp_result_t *res);
const char *dsp_label_name(det_label_t label);

#endif
