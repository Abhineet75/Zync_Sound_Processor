"""
find_rumbles.py : scan a long field recording for rumble-like events using the
board's own band-share rule, and list candidates for playback into the board.

The board decimates to 939 S/s and analyses 512-sample blocks (0.545 s). Here
the recording (8 kHz) is cut into 4096-sample blocks (0.512 s) and each block is
analysed with an FFT restricted to what the board would see after decimation:

    rumble share = sum |X_k|^2 over 9-41 Hz   / sum |X_k|^2 over 1 Hz .. 469 Hz
    mains share  = sum |X_k|^2 over 47-52 Hz  / same total

(no window, DC removed, as on the board). A candidate event is a run of at
least DSP_HOLD_BLOCKS consecutive blocks with rumble share >= DSP_RUMBLE_PM
whose rumble-band power is at least SNR_DB above the local background (median
over the surrounding 10 minutes). The recording is not calibrated, so the
board's absolute minimum-RMS rule is replaced by this relative one.

Candidates still need a human check (spectrogram / listening): wind, thunder,
vehicles and aircraft also put energy below 41 Hz.

Usage (from the repo root):
    python3 Signal_Processing/field_recordings/find_rumbles.py RECORDING.wav [--out candidates.csv]
Needs numpy.
"""

import argparse
import csv
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "python"))
import lfsp_reference as L  # noqa: E402  (band edges and thresholds from lfsp_dsp.h)

BLOCK = 4096                 # samples per block at 8 kHz (0.512 s)
BOARD_NYQUIST_HZ = L.FS / 2  # 469.5 Hz: the board cannot see above this
SNR_DB = 10.0                # event must stand this far above the local background
BACKGROUND_S = 600           # window for the background median


def wav_pcm16(path):
    """Memory-map the PCM data of a mono 16-bit WAV file; return (samples, rate)."""
    with open(path, "rb") as f:
        if f.read(4) != b"RIFF" or (f.read(4) and f.read(4) != b"WAVE"):
            raise ValueError("not a RIFF/WAVE file")
        rate = None
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                raise ValueError("no data chunk")
            cid, size = hdr[:4], struct.unpack("<I", hdr[4:])[0]
            if cid == b"fmt ":
                fmt, ch, rate, _, _, bits = struct.unpack("<HHIIHH", f.read(16))
                f.seek(size - 16 + (size & 1), 1)
                if fmt != 1 or ch != 1 or bits != 16:
                    raise ValueError(f"need mono 16-bit PCM, got format={fmt} channels={ch} bits={bits}")
            elif cid == b"data":
                offset = f.tell()
                break
            else:
                f.seek(size + (size & 1), 1)
    data = np.memmap(path, dtype="<i2", mode="r", offset=offset, shape=(size // 2,))
    return data, rate


def band_bins(rate):
    df = rate / BLOCK
    k = lambda f, up: int(np.ceil(f / df)) if up else int(np.floor(f / df))
    return (slice(k(L.C["DSP_RUMBLE_LO_HZ"], True), k(L.C["DSP_RUMBLE_HI_HZ"], False) + 1),
            slice(k(L.C["DSP_MAINS_LO_HZ"], True), k(L.C["DSP_MAINS_HI_HZ"], False) + 1),
            slice(1, k(BOARD_NYQUIST_HZ, False) + 1),
            df)


def analyse(data, rate, chunk_blocks=4096):
    """Per-block rumble power, total power (to 469 Hz), shares and peak frequency."""
    rumble_b, mains_b, total_b, df = band_bins(rate)
    nblk = len(data) // BLOCK
    out = {k: np.empty(nblk) for k in ("p_rumble", "p_total", "rumble_pm", "mains_pm", "peak_hz")}
    for start in range(0, nblk, chunk_blocks):
        stop = min(nblk, start + chunk_blocks)
        x = np.asarray(data[start * BLOCK:stop * BLOCK], dtype=np.float64).reshape(-1, BLOCK)
        x -= x.mean(axis=1, keepdims=True)
        p = np.abs(np.fft.rfft(x, axis=1)) ** 2
        pr, pm, pt = p[:, rumble_b].sum(1), p[:, mains_b].sum(1), p[:, total_b].sum(1)
        pt = np.maximum(pt, 1e-12)
        out["p_rumble"][start:stop] = pr
        out["p_total"][start:stop] = pt
        out["rumble_pm"][start:stop] = 1000 * pr / pt
        out["mains_pm"][start:stop] = 1000 * pm / pt
        out["peak_hz"][start:stop] = (np.argmax(p[:, rumble_b], axis=1) + rumble_b.start) * df
        print(f"\r  analysed {stop / nblk:6.1%}", end="", file=sys.stderr)
    print(file=sys.stderr)
    return out


def local_background(p, rate):
    """Median of the rumble-band power over a sliding window, computed in coarse steps."""
    win = max(1, int(BACKGROUND_S * rate / BLOCK))
    step = max(1, win // 8)
    centres = np.arange(0, len(p), step)
    med = np.array([np.median(p[max(0, c - win // 2):c + win // 2 + 1]) for c in centres])
    return np.interp(np.arange(len(p)), centres, med)


def find_events(a, rate):
    hold = int(L.C["DSP_HOLD_BLOCKS"])
    bg = local_background(a["p_rumble"], rate)
    snr = 10 * np.log10(np.maximum(a["p_rumble"], 1e-12) / np.maximum(bg, 1e-12))
    hit = (a["rumble_pm"] >= L.C["DSP_RUMBLE_PM"]) & (snr >= SNR_DB)
    events, i = [], 0
    while i < len(hit):
        if hit[i]:
            j = i
            while j < len(hit) and hit[j]:
                j += 1
            if j - i >= hold:
                sl = slice(i, j)
                events.append(dict(
                    start_s=i * BLOCK / rate, dur_s=(j - i) * BLOCK / rate, blocks=j - i,
                    peak_snr_db=float(snr[sl].max()),
                    mean_rumble_pm=float(a["rumble_pm"][sl].mean()),
                    mean_mains_pm=float(a["mains_pm"][sl].mean()),
                    peak_hz=float(np.median(a["peak_hz"][sl]))))
            i = j
        else:
            i += 1
    return events, snr


def hms(t):
    t = int(round(t))
    return f"{t // 3600:02d}:{t // 60 % 60:02d}:{t % 60:02d}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--out", default="rumble_candidates.csv")
    args = ap.parse_args()

    data, rate = wav_pcm16(args.wav)
    print(f"{args.wav}: {len(data)} samples at {rate} Hz = {len(data) / rate / 3600:.2f} h", file=sys.stderr)
    a = analyse(data, rate)
    events, _ = find_events(a, rate)
    events.sort(key=lambda e: (e["peak_snr_db"] * min(e["dur_s"], 5.0)), reverse=True)

    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["rank", "start_hms", "start_s", "dur_s", "blocks", "peak_snr_db",
                                          "mean_rumble_pm", "mean_mains_pm", "peak_hz"])
        w.writeheader()
        for r, e in enumerate(events, 1):
            w.writerow(dict(rank=r, start_hms=hms(e["start_s"]), **{k: round(v, 2) if isinstance(v, float) else v
                                                                     for k, v in e.items()}))

    blocks = len(a["rumble_pm"])
    print(f"blocks analysed: {blocks}; blocks with rumble share >= {L.C['DSP_RUMBLE_PM']:.0f}: "
          f"{int(np.sum(a['rumble_pm'] >= L.C['DSP_RUMBLE_PM']))}; events: {len(events)} -> {args.out}")
    print(f"{'rank':>4} {'start':>8} {'dur':>6} {'SNR':>6} {'rumble':>7} {'mains':>6} {'peak':>6}")
    for r, e in enumerate(events[:25], 1):
        print(f"{r:4d} {hms(e['start_s']):>8} {e['dur_s']:5.1f}s {e['peak_snr_db']:5.1f}dB "
              f"{e['mean_rumble_pm']:6.0f}‰ {e['mean_mains_pm']:5.0f}‰ {e['peak_hz']:5.1f}Hz")


if __name__ == "__main__":
    main()
