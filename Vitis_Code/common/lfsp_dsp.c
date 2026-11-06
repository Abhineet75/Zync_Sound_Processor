// =============================================================================
// lfsp_dsp.c : block processing — see lfsp_dsp.h for the overview
// =============================================================================

#include "lfsp_dsp.h"

// No libm: Vitis does not link the maths library by default, and everything
// below needs only small positive numbers and angles.

// =============================================================================
// Setup: bin ranges and coefficient tables (runs once, float is fine here)
// =============================================================================
static int hz_to_bin_floor(float f, float fs) { return (int)(f * DSP_N / fs); }

static int hz_to_bin_ceil(float f, float fs)
{
    float v = f * DSP_N / fs;
    int   k = (int)v;
    return ((float)k < v) ? k + 1 : k;
}

// cos(x) by Taylor series; bins used here give x <= 0.35 rad, where 8 terms
// are exact to double precision
static double cos_small(double x)
{
    double term = 1.0, sum = 1.0, x2 = x * x;
    for (int n = 1; n <= 8; n++) {
        term *= -x2 / (double)((2 * n - 1) * (2 * n));
        sum  += term;
    }
    return sum;
}

static void add_bin(dsp_cfg_t *cfg, int k, int is_mains)
{
    if (cfg->nbins >= DSP_MAX_BINS)
        return;
    double c = 2.0 * cos_small(2.0 * 3.14159265358979323846 * (double)k / (double)DSP_N);
    cfg->k[cfg->nbins]         = k;
    cfg->is_mains[cfg->nbins]  = is_mains;
    cfg->coeff_f[cfg->nbins]   = (float)c;
    cfg->coeff_q14[cfg->nbins] = (int32_t)(c * 16384.0 + 0.5);    // round; c > 0
    cfg->nbins++;
}

void dsp_init(dsp_cfg_t *cfg, float fs)
{
    cfg->fs        = fs;
    cfg->rumble_lo = hz_to_bin_ceil(DSP_RUMBLE_LO_HZ, fs);
    cfg->rumble_hi = hz_to_bin_floor(DSP_RUMBLE_HI_HZ, fs);
    cfg->mains_lo  = hz_to_bin_ceil(DSP_MAINS_LO_HZ, fs);
    cfg->mains_hi  = hz_to_bin_floor(DSP_MAINS_HI_HZ, fs);
    cfg->nbins     = 0;
    for (int k = cfg->rumble_lo; k <= cfg->rumble_hi; k++) add_bin(cfg, k, 0);
    for (int k = cfg->mains_lo;  k <= cfg->mains_hi;  k++) add_bin(cfg, k, 1);
}

// =============================================================================
// Helpers
// =============================================================================
static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return (uint32_t)r;
}

// share = 2 * band / (N * total), in per mille
static uint32_t share_pm(double band, double total)
{
    if (total <= 0.0) return 0;
    double pm = 2000.0 * band / ((double)DSP_N * total);
    return pm > 1000.0 ? 1000u : (uint32_t)(pm + 0.5);
}

static uint32_t share_pm_int(uint64_t band, uint64_t total)
{
    // band <= N*total/2 by Parseval, so 2000*band < 2^64 for any 16-bit input
    uint64_t den = (uint64_t)DSP_N * total;
    if (den == 0) return 0;
    uint64_t pm = (2000ULL * band + den / 2) / den;
    return pm > 1000u ? 1000u : (uint32_t)pm;
}

// =============================================================================
// Fixed-point path
// =============================================================================

// Unsigned decimator output -> signed Q15, DC removed with the block mean.
// The mean is a shift because N = 512 is a power of two.
static void to_q15(const uint16_t *in, int16_t *out)
{
    int32_t sum = 0;                       // 512 * 65535 < 2^31
    for (int i = 0; i < DSP_N; i++) sum += in[i];
    int32_t mean = sum >> 9;               // / 512
    for (int i = 0; i < DSP_N; i++) {
        int32_t d = (int32_t)in[i] - mean;
        if (d >  32767) d =  32767;        // only reachable with a full-scale step
        if (d < -32768) d = -32768;
        out[i] = (int16_t)d;
    }
}

// Goertzel power |X_k|^2. c = 2cos(w) in Q14. Filter state stays below
// |X_k| / sin(w) ~ 1.4e8 for the lowest bin used (k = 5), inside int32.
static uint64_t goertzel_q14(const int16_t *x, int32_t c)
{
    int32_t s1 = 0, s2 = 0;
    for (int i = 0; i < DSP_N; i++) {
        int32_t s = x[i] + (int32_t)(((int64_t)c * s1) >> 14) - s2;
        s2 = s1;
        s1 = s;
    }
    int64_t cs1 = ((int64_t)c * s1) >> 14;
    int64_t p   = (int64_t)s1 * s1 + (int64_t)s2 * s2 - cs1 * s2;
    return p < 0 ? 0u : (uint64_t)p;
}

void dsp_block_q15(const dsp_cfg_t *cfg, const uint16_t *in, dsp_result_t *res)
{
    int16_t x[DSP_N];
    to_q15(in, x);

    uint64_t total = 0;
    for (int i = 0; i < DSP_N; i++) total += (uint64_t)((int32_t)x[i] * x[i]);

    uint64_t rumble = 0, mains = 0;
    for (int b = 0; b < cfg->nbins; b++) {
        uint64_t p = goertzel_q14(x, cfg->coeff_q14[b]);
        if (cfg->is_mains[b]) mains += p; else rumble += p;
    }

    res->rumble_e  = (double)rumble;
    res->mains_e   = (double)mains;
    res->total_e   = (double)total;
    res->rumble_pm = share_pm_int(rumble, total);
    res->mains_pm  = share_pm_int(mains, total);
    res->rms       = isqrt64(total / DSP_N);
}

// =============================================================================
// Float path (same algorithm, single precision)
// =============================================================================
void dsp_block_float(const dsp_cfg_t *cfg, const uint16_t *in, dsp_result_t *res)
{
    float x[DSP_N];
    float sum = 0.0f;
    for (int i = 0; i < DSP_N; i++) sum += (float)in[i];
    float mean = sum / (float)DSP_N;
    float total = 0.0f;
    for (int i = 0; i < DSP_N; i++) {
        x[i] = (float)in[i] - mean;
        total += x[i] * x[i];
    }

    float rumble = 0.0f, mains = 0.0f;
    for (int b = 0; b < cfg->nbins; b++) {
        float c = cfg->coeff_f[b], s1 = 0.0f, s2 = 0.0f;
        for (int i = 0; i < DSP_N; i++) {
            float s = x[i] + c * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        float p = s1 * s1 + s2 * s2 - c * s1 * s2;
        if (p < 0.0f) p = 0.0f;
        if (cfg->is_mains[b]) mains += p; else rumble += p;
    }

    res->rumble_e  = rumble;
    res->mains_e   = mains;
    res->total_e   = total;
    res->rumble_pm = share_pm(rumble, total);
    res->mains_pm  = share_pm(mains, total);
    res->rms       = isqrt64((uint64_t)(total / (float)DSP_N));
}

// =============================================================================
// Detection
// =============================================================================
det_label_t dsp_classify(const dsp_result_t *res)
{
    if (res->rms < DSP_MIN_RMS)          return DET_QUIET;
    if (res->rumble_pm >= DSP_RUMBLE_PM) return DET_RUMBLE;
    if (res->mains_pm  >= DSP_MAINS_PM)  return DET_MAINS;
    return DET_NONE;
}

void dsp_detect_init(det_state_t *st)
{
    st->candidate = DET_QUIET;
    st->count     = 0;
    st->output    = DET_QUIET;
}

// A verdict is reported only after DSP_HOLD_BLOCKS consecutive blocks agree,
// so a single odd block (e.g. one that straddles a change) does not flicker.
det_label_t dsp_detect(det_state_t *st, const dsp_result_t *res)
{
    det_label_t c = dsp_classify(res);
    if (c == st->candidate) {
        if (st->count < DSP_HOLD_BLOCKS) st->count++;
    } else {
        st->candidate = c;
        st->count     = 1;
    }
    if (st->count >= DSP_HOLD_BLOCKS)
        st->output = c;
    return st->output;
}

const char *dsp_label_name(det_label_t label)
{
    switch (label) {
        case DET_RUMBLE: return "RUMBLE";
        case DET_MAINS:  return "MAINS HUM";
        case DET_NONE:   return "nothing";
        default:         return "quiet";
    }
}
