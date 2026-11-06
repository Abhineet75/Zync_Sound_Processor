"""
lfsp_reference.py : Python models of the hardware and the float reference.

Models (bit-exact with the Verilog):
    pattern_codes()  axis_test_pattern.v  (ramp / triangle / square / constant)
    xadc_codes()     the real input path: sine at A0 -> 12-bit XADC codes
    decimate()       decim_avg.v          (sum of 1024 codes, keep bits 21..6)

Reference (independent of the C code):
    analyse_block()  direct DFT in float64 at every band bin, DC removed with the
                     exact mean, band share of total power in per mille
    Detector         same rules as dsp_detect() in lfsp_dsp.c

Thresholds and band edges are read from Vitis_Code/common/lfsp_dsp.h so the
reference and the C code can never drift apart.
"""

import math
import re
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "Vitis_Code" / "common" / "lfsp_dsp.h"

# ---- Hardware constants ------------------------------------------------------
FS_RAW = 100e6 / 4 / 26          # XADC: 100 MHz DCLK / 4 -> 25 MHz ADCCLK, 26 cycles per conversion
LOG2R = 10
R = 1 << LOG2R                   # decimation ratio 1024
N = 512                          # samples per block
FS = FS_RAW / R                  # 939.002 Hz

# Day 1 measurement: 1 Vpp sine at the generator -> codes 867..1743 at the XADC
DAY1_CODE_CENTRE = 1305
DAY1_CODE_AMPL = 438
DAY1_NOISE_CODES = 2.0           # single-sample noise, A0 open: 3..14 codes


def read_header_constants(path=HEADER):
    """#define DSP_xxx values from lfsp_dsp.h as a dict of floats."""
    consts = {}
    for line in path.read_text().splitlines():
        m = re.match(r"\s*#define\s+(DSP_\w+)\s+([0-9.]+)[fu]?\b", line)
        if m:
            consts[m.group(1)] = float(m.group(2))
    return consts


C = read_header_constants()


def phase_inc_for(freq_hz):
    """axis_test_pattern phase increment for a waveform frequency."""
    return int(round(freq_hz * 2**32 / FS_RAW))


# =============================================================================
# Signal sources (12-bit codes at the decimator input)
# =============================================================================
class PatternSource:
    """axis_test_pattern.v: 32-bit phase accumulator, code from phase_next."""

    RAMP, TRIANGLE, SQUARE, CONST = 0, 1, 2, 3

    def __init__(self):
        self.phase = 0

    def codes(self, n, mode, phase_inc):
        steps = np.arange(1, n + 1, dtype=np.uint64)
        ph = (np.uint64(self.phase) + steps * np.uint64(phase_inc)) & np.uint64(0xFFFFFFFF)
        self.phase = int(ph[-1])
        if mode == self.RAMP:
            code = ph >> np.uint64(20)
        elif mode == self.TRIANGLE:
            low = (ph >> np.uint64(19)) & np.uint64(0xFFF)
            code = np.where((ph >> np.uint64(31)) == 1, low ^ np.uint64(0xFFF), low)
        elif mode == self.SQUARE:
            code = np.where((ph >> np.uint64(31)) == 1, 4095, 0)
        else:
            code = np.full(n, 2048)
        return code.astype(np.int64)


class XadcSource:
    """Generator sine through the 1.2 kOhm resistor into A0, as measured on Day 1."""

    def __init__(self, seed=1):
        self.t0 = 0
        self.rng = np.random.default_rng(seed)

    def codes(self, n, freq_hz=None, ampl=DAY1_CODE_AMPL, centre=DAY1_CODE_CENTRE,
              tones=None, noise=DAY1_NOISE_CODES):
        """tones: list of (freq_hz, amplitude_codes); default one tone at freq_hz."""
        t = (self.t0 + np.arange(n)) / FS_RAW
        self.t0 += n
        v = np.full(n, float(centre))
        if tones is None:
            tones = [] if freq_hz is None else [(freq_hz, ampl)]
        for f, a in tones:
            v += a * np.sin(2 * np.pi * f * t)
        v += self.rng.normal(0.0, noise, n)
        return np.clip(np.rint(v), 0, 4095).astype(np.int64)


# =============================================================================
# Decimator (decim_avg.v)
# =============================================================================
def decimate(codes):
    """Sum each group of R codes and keep the top 16 of 22 bits (>> 6)."""
    usable = len(codes) // R * R
    sums = codes[:usable].reshape(-1, R).sum(axis=1)
    return (sums >> (LOG2R + 12 - 16)).astype(np.uint16)


# =============================================================================
# Float reference
# =============================================================================
def band_bins(fs=FS):
    k = lambda f, rnd: int(rnd(f * N / fs))
    rumble = range(k(C["DSP_RUMBLE_LO_HZ"], math.ceil), k(C["DSP_RUMBLE_HI_HZ"], math.floor) + 1)
    mains = range(k(C["DSP_MAINS_LO_HZ"], math.ceil), k(C["DSP_MAINS_HI_HZ"], math.floor) + 1)
    return rumble, mains


_RUMBLE, _MAINS = band_bins()
_n = np.arange(N)


def _band_energy(x, bins):
    ks = np.array(list(bins))
    basis = np.exp(-2j * np.pi * np.outer(ks, _n) / N)
    return float(np.sum(np.abs(basis @ x) ** 2))


def analyse_block(block):
    """Band energies, shares (per mille) and RMS of one block, in float64."""
    x = block.astype(np.float64)
    x -= x.mean()
    total = float(np.sum(x * x))
    rumble = _band_energy(x, _RUMBLE)
    mains = _band_energy(x, _MAINS)
    share = lambda e: 0.0 if total == 0 else min(1000.0, 2000.0 * e / (N * total))
    return dict(rumble_e=rumble, mains_e=mains, total_e=total,
                rumble_pm=share(rumble), mains_pm=share(mains),
                rms=math.sqrt(total / N))


def classify(res):
    if res["rms"] < C["DSP_MIN_RMS"]:
        return "quiet"
    if res["rumble_pm"] >= C["DSP_RUMBLE_PM"]:
        return "RUMBLE"
    if res["mains_pm"] >= C["DSP_MAINS_PM"]:
        return "MAINS HUM"
    return "nothing"


class Detector:
    """Same hold rule as dsp_detect(): report after HOLD consecutive agreeing blocks."""

    def __init__(self):
        self.candidate, self.count, self.output = "quiet", 0, "quiet"

    def update(self, res):
        c = classify(res)
        if c == self.candidate:
            self.count = min(self.count + 1, int(C["DSP_HOLD_BLOCKS"]))
        else:
            self.candidate, self.count = c, 1
        if self.count >= C["DSP_HOLD_BLOCKS"]:
            self.output = c
        return self.output
