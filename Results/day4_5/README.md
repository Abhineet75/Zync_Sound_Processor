# Days 4–5 results — detection working on hardware (2026-11-06)

Program: [`Vitis_Code/day45_detect/main.c`](../../Vitis_Code/day45_detect/main.c) with [`Vitis_Code/common/lfsp_dsp.c`](../../Vitis_Code/common/lfsp_dsp.c) — the Day 3 data path plus, per 512-sample block: DC removal, 21 Goertzel filters (rumble bins k = 5…22 ≈ 9–41 Hz, mains bins k = 26…28 ≈ 47–52 Hz), band share of total power, and detection with a 2-block hold.

## 1. Pre-hardware verification (on a PC)

[`Signal_Processing/run_pipeline_test.py`](../../Signal_Processing/run_pipeline_test.py) runs the board's own C code on a PC, fed by Python models that are **bit-exact with the Verilog** (test pattern + decimator: 2048 outputs, 0 mismatches), and compares it with an independent float64 DFT reference.

| Check | Result |
|---|---|
| Scenarios (tones 10–100 Hz, silence, full-scale square, modelled XADC input incl. noise-only, elephant-like harmonics, 0.5 s burst, switching sequence) | **17 / 17 PASS** |
| Q15 band energy vs float64 reference (signal blocks, bands ≥ 1 % of power) | **≤ 0.05 dB** (target 0.5 dB) |
| Planted DSP bugs caught (wrong shift, no DC removal, wrong coefficient, wrong share formula, wrong power sign, hold rule ignored) | **6 / 6** |

## 2. Self-test on the board (SW0 = 0, built-in test pattern)

The test pattern plays triangle tones and silence; the program checks each verdict and measures the latency from the tone change to the confirmed verdict.

| Step | Band shares (‰, rumble / mains) | Verdict | Expected | Latency |
|---|---|---|---|---|
| triangle 20 Hz | 985 / 0 | RUMBLE | RUMBLE | 1085 ms (2 blocks) |
| silence | 0 / 0, rms 0 | quiet | quiet | 1630 ms (3 blocks) |
| triangle 50 Hz | ~10 / 915 | MAINS HUM | MAINS HUM | 1085 ms |
| triangle 100 Hz | ~1 / ~0 | nothing | nothing | 1085 ms |
| triangle 10 Hz | ~900–940 / ~3 | RUMBLE | RUMBLE | 1085 ms |
| triangle 45 Hz | ~50 / ~68 | nothing | nothing | 1085 ms |
| triangle 35 Hz | 986 / 0 | RUMBLE | RUMBLE | 1085 ms |
| silence | 0 / 0 | quiet | quiet | 1630 ms |

**8 / 8 PASS in every round**, for both processing paths and both build configurations (5+ consecutive rounds observed). Band shares on hardware match the PC simulation to within a few per mille (e.g. 20 Hz: 986 / 0, rms 18975 in both). Silence takes one block longer because the block in which the pattern switches still contains the end of the previous tone.

## 3. Real analog input (SW0 = 1, function generator → 1.2 kΩ → A0)

Sine, 1.65 V offset, High Z; frequency changed by hand with the knob.

| Input | Band shares (‰, rumble / mains) | RMS | Verdict |
|---|---|---|---|
| Generator off | ~60 / ~600 | **2** | quiet |
| 20 Hz | **999** / 0 | ≈ 3640 | RUMBLE (2 blocks after switch-on) |
| Swept up through the rumble band | 950–1000 / ≤ 5 | ≈ 3630 | RUMBLE |
| ≈ 50 Hz | 6–14 / **925–928** | ≈ 3625 | MAINS HUM |
| Above 52 Hz | ≤ 7 / ≤ 7 | ≈ 3610 | nothing |
| Swept back down through 50 Hz, into the rumble band | → mains 926 → rumble 950–1000 | | MAINS HUM → RUMBLE |
| Off again | | 2 | quiet (2 blocks) |

Overruns 0, DMA errors 0, drops 0 throughout.

Observations:

- **Real mains pickup.** With the generator off, ~60 % of the tiny residual power (rms 2) sits in the 50 Hz band: genuine mains interference picked up by the wiring. The minimum-RMS rule (300) keeps the verdict at "quiet"; without it the detector would report mains hum with no input.
- **Switching transients.** The block in which the generator output is switched on or off has a large RMS (5801–7970) from the DC-offset step; the 2-block hold stops it from producing a false verdict.
- RMS ≈ 3640 is about 26 % below the Day 1 level (≈ 4930 predicted); the generator amplitude was likely set slightly lower. Band shares do not depend on level.

## 4. Fixed point vs float: processing time per block

Measured with the ARM global timer around the processing (busy pin high → low), same input, SW1 switching the path at run time.

| Build | Q15 (fixed point) | float (VFPv3) | Faster |
|---|---|---|---|
| Debug, `-O0` | 761 µs | 589 µs | float by 23 % |
| Optimised, `-O2` | **190 µs** | **271 µs** | **Q15 by 30 %** |

CPU utilization at `-O2`: Q15 190 µs / 545 ms = **0.035 %**; float **0.05 %**. Both paths give the same verdicts and band shares within 1–2 ‰.

Why the order flips: the Q15 Goertzel needs a 32 × 32 → 64-bit multiply per sample. Unoptimised, each 64-bit operation becomes many instructions with memory round trips; at `-O2` it is a single `SMULL` with the filter state in registers, which beats the dependent multiply–add chain on the Cortex-A9's VFP. Fixed point is not automatically faster on a CPU with an FPU; its real advantage is on FPU-less microcontrollers and in FPGA fabric — and Q15 reaches the same accuracy (≤ 0.05 dB) with integer hardware only.

## Still to capture

- Busy-pin scope captures (JA2) for Q15 and float at `-O2`, probe ground on the Arduino GND
- Cache-bug build (`USE_CACHE_INVALIDATE 0`) output
- Vivado utilization report screenshot
- Full terminal logs as text files
