"""
make_playback_clips.py : cut clips from a field recording for playback into the
board (laptop audio out -> bias network -> A0), and predict what the board will
report for each one.

Versions of every clip:
    raw      the recording as it is                                  (written)
    rc68     first-order low-pass at 68 Hz, applied digitally: the   (written)
             response of a 1.2 kOhm + 1.94 uF RC filter, i.e. the
             simplest anti-aliasing front end, without depending on
             the laptop's output impedance
    lp60     4th-order low-pass at 60 Hz                       (simulated only)
             -- shows that too narrow a front end makes background
             noise alone reach the band-share threshold

All clips of one version share ONE gain (the loudest clip peaks at -1 dBFS), so
background clips keep their natural, quieter level, as they would in the field.
WAV files are 16-bit at the recording's rate with 0.5 s fades.

Board prediction: each clip is reconstructed at the XADC rate (961,538 S/s,
linear interpolation as a stand-in for the laptop DAC), mapped to ADC codes
(bias ~650 codes; full scale = +/-400 codes; 2 codes of noise), decimated
bit-exactly like decim_avg.v, and analysed block by block with the reference
model of lfsp_dsp.c (shares, minimum RMS, 2-block hold). Each version is also
checked with a first-order 20 Hz high-pass in front ("+hp20"), the typical
low-frequency roll-off of a laptop audio output.

Usage (from the repo root):
    python3 Signal_Processing/field_recordings/make_playback_clips.py RECORDING.wav OUTDIR \
        --clip 10:42:50 30 --clip 10:30:00 20 ...
Needs numpy.
"""

import argparse
import sys
import wave
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "python"))
sys.path.insert(0, str(HERE))
import lfsp_reference as L  # noqa: E402
from find_rumbles import wav_pcm16  # noqa: E402

CODE_BIAS = 650          # A0 sits near 0.57 V: 1.65 V bias divider loaded by 1.2 k + board divider
CODE_FULL_SCALE = 400    # codes of swing for a full-scale (0 dBFS) clip at high laptop volume
CODE_NOISE = 2.0
FADE_S = 0.5
SHORT = {"RUMBLE": "R", "MAINS HUM": "M", "nothing": ".", "quiet": "q"}


def parse_hms(s):
    h, m, sec = (int(v) for v in s.split(":"))
    return h * 3600 + m * 60 + sec


def butter_lowpass(x, rate, fc, order):
    """Zero-phase Butterworth-magnitude low-pass via FFT (whole clip)."""
    f = np.fft.rfftfreq(len(x), 1 / rate)
    return np.fft.irfft(np.fft.rfft(x) / np.sqrt(1 + (f / fc) ** (2 * order)), len(x))


def rc_lowpass(x, rate, fc):
    """First-order RC low-pass (causal, like the real circuit)."""
    a = np.exp(-2 * np.pi * fc / rate)
    y = np.empty_like(x)
    acc = x[0]
    for i, v in enumerate(x):
        acc = a * acc + (1 - a) * v
        y[i] = acc
    return y


def highpass1(x, rate, fc):
    """First-order high-pass magnitude (laptop audio output roll-off)."""
    r = np.fft.rfftfreq(len(x), 1 / rate) / fc
    return np.fft.irfft(np.fft.rfft(x) * r / np.sqrt(1 + r ** 2), len(x))


def faded(x, rate):
    n = int(FADE_S * rate)
    env = np.ones(len(x))
    env[:n] = np.linspace(0, 1, n)
    env[-n:] = np.linspace(1, 0, n)
    return (x - x.mean()) * env


def board_prediction(x_fs, rate, seed=0):
    """Verdict per block for a clip given in full-scale units (|x| <= 1)."""
    t_out = np.arange(int(len(x_fs) / rate * L.FS_RAW)) / L.FS_RAW
    v = np.interp(t_out, np.arange(len(x_fs)) / rate, x_fs * CODE_FULL_SCALE)
    rng = np.random.default_rng(seed)
    codes = np.clip(np.rint(CODE_BIAS + v + rng.normal(0, CODE_NOISE, len(v))), 0, 4095).astype(np.int64)
    dec = L.decimate(codes)
    det = L.Detector()
    rows = []
    for b in range(len(dec) // L.N):
        r = L.analyse_block(dec[b * L.N:(b + 1) * L.N])
        rows.append((r["rumble_pm"], r["rms"], det.update(r)))
    return rows


def write_wav(path, x_fs, rate):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(np.rint(np.clip(x_fs, -1, 1) * 32767).astype("<i2").tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("outdir")
    ap.add_argument("--clip", nargs=2, action="append", metavar=("HH:MM:SS", "SECONDS"), required=True)
    args = ap.parse_args()

    data, rate = wav_pcm16(args.wav)
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)

    filters = {
        "raw": lambda x: x,
        "rc68": lambda x: rc_lowpass(x, rate, 68.0),
        "lp60": lambda x: butter_lowpass(x, rate, 60.0, 4),
    }
    names, clips = [], {k: [] for k in filters}
    for start, dur in args.clip:
        t0, d = parse_hms(start), float(dur)
        x = np.asarray(data[int(t0 * rate):int((t0 + d) * rate)], dtype=np.float64)
        x -= x.mean()
        names.append(f"elp_{start.replace(':', '')}_{int(d)}s")
        for k, flt in filters.items():
            clips[k].append(faded(flt(x), rate))

    print(f"{'clip':<16} {'version':<10} {'max rumble':>10} {'RUMBLE':>7} {'rms':>6}  verdicts (one per 0.545 s block)")
    for k in filters:
        gain = (10 ** (-1 / 20)) / max(np.max(np.abs(c)) for c in clips[k])   # one gain per version
        for name, c in zip(names, clips[k]):
            c_fs = c * gain
            if k in ("raw", "rc68"):
                write_wav(out / f"{name}_{k}.wav", c_fs, rate)
            for hp in ("", "+hp20"):
                rows = board_prediction(highpass1(c_fs, rate, 20.0) if hp else c_fs, rate)
                print(f"{name:<16} {k + hp:<10} {max(r[0] for r in rows):9.0f}‰ "
                      f"{sum(r[2] == 'RUMBLE' for r in rows):7d} {int(np.median([r[1] for r in rows])):6d}  "
                      f"{''.join(SHORT[r[2]] for r in rows)}")
        print()
    print("legend: R = RUMBLE, M = MAINS HUM, . = nothing, q = quiet; rms = median block RMS (board threshold 300)")


if __name__ == "__main__":
    main()
