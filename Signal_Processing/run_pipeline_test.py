"""
run_pipeline_test.py : whole signal chain on a PC, before touching the board.

    source (bit-exact test pattern / modelled XADC input)
      -> decimator model (bit-exact with decim_avg.v)
      -> the board's C code  (Vitis_Code/common/lfsp_dsp.c, Q15 and float paths)
      -> compared with the float64 Python reference (lfsp_reference.py)

Checks per scenario:
    * detection label (C Q15, C float and Python) equals the expected label
      once the hold time has passed
    * C Q15 band energy within 0.5 dB of the Python reference whenever the
      block carries a real signal (RMS >= DSP_MIN_RMS, i.e. not "quiet") and
      that band holds >= 1 % of the power. Below that, energies are rounding
      noise or leakage and a dB error is meaningless; the label is still checked.
    * labels that do not belong to a scenario never appear in any block
      (e.g. a 0.5 s burst must not be reported: the hold rule needs ~1 s)
    * detection latency in blocks for the switching sequence

Usage (from the repo root):
    python3 Signal_Processing/run_pipeline_test.py
Needs numpy and a C compiler (cc).
"""

import csv
import io
import math
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "python"))
import lfsp_reference as L  # noqa: E402

REPO = L.REPO
HOLD = int(L.C["DSP_HOLD_BLOCKS"])
DB_LIMIT = 0.5
SIGNIFICANT_PM = 10.0            # band must hold >= 1 % of power for the dB check


# =============================================================================
# Build and run the C code
# =============================================================================
def build_host(tmp):
    exe = Path(tmp) / "host_dsp"
    cmd = ["cc", "-O2", "-Wall", "-Wextra", f"-I{REPO / 'Vitis_Code' / 'common'}", "-o", str(exe),
           str(HERE / "host_test" / "host_dsp.c"), str(REPO / "Vitis_Code" / "common" / "lfsp_dsp.c"), "-lm"]
    subprocess.run(cmd, check=True)
    return exe


def run_c(exe, blocks, tmp):
    path = Path(tmp) / "blocks.txt"
    path.write_text("\n".join(str(int(v)) for v in np.concatenate(blocks)) + "\n")
    out = subprocess.run([str(exe), str(path)], check=True, capture_output=True, text=True).stdout
    rows = {"q15": [], "float": []}
    for r in csv.DictReader(io.StringIO(out)):
        rows[r["path"]].append(r)
    return rows


# =============================================================================
# Scenarios: list of segments, each (number of blocks, expected label, codes function)
# =============================================================================
def pattern(mode, freq=None):
    return lambda src, n: src["pat"].codes(n, mode, L.phase_inc_for(freq) if freq else 0)


def xadc(**kw):
    return lambda src, n: src["xadc"].codes(n, **kw)


TRI, SQR, CONST = L.PatternSource.TRIANGLE, L.PatternSource.SQUARE, L.PatternSource.CONST

SCENARIOS = [
    ("pattern triangle 20 Hz",            [(8, "RUMBLE",    pattern(TRI, 20))]),
    ("pattern triangle 50 Hz",            [(8, "MAINS HUM", pattern(TRI, 50))]),
    ("pattern triangle 100 Hz",           [(8, "nothing",   pattern(TRI, 100))]),
    ("pattern triangle 10 Hz",            [(8, "RUMBLE",    pattern(TRI, 10))]),
    ("pattern triangle 35 Hz",            [(8, "RUMBLE",    pattern(TRI, 35))]),
    ("pattern triangle 40 Hz",            [(8, "RUMBLE",    pattern(TRI, 40))]),
    ("pattern triangle 45 Hz",            [(8, "nothing",   pattern(TRI, 45))]),
    ("pattern constant (silence)",        [(6, "quiet",     pattern(CONST))]),
    ("pattern square 20 Hz, full scale",  [(8, "RUMBLE",    pattern(SQR, 20))]),
    ("XADC sine 20 Hz, Day-1 level",      [(8, "RUMBLE",    xadc(freq_hz=20))]),
    ("XADC sine 50 Hz, Day-1 level",      [(8, "MAINS HUM", xadc(freq_hz=50))]),
    ("XADC sine 100 Hz, Day-1 level",     [(8, "nothing",   xadc(freq_hz=100))]),
    ("XADC sine 20 Hz, 1/10 level",       [(8, "RUMBLE",    xadc(freq_hz=20, ampl=44))]),
    ("XADC open input (noise only)",      [(6, "quiet",     xadc(centre=8))]),
    ("XADC elephant-like 15 Hz + harmonics",
     [(8, "RUMBLE", xadc(tones=[(15, 300), (30, 150), (45, 90), (60, 60)], noise=4))]),
    ("short 20 Hz burst (1 block, ~0.5 s) in silence: ignored",
     [(4, "quiet", pattern(CONST)), (1, "quiet", pattern(TRI, 20)), (4, "quiet", pattern(CONST))]),
    ("sequence 20 -> 50 -> 100 -> silence -> 20 Hz (latency)",
     [(6, "RUMBLE", pattern(TRI, 20)), (6, "MAINS HUM", pattern(TRI, 50)),
      (6, "nothing", pattern(TRI, 100)), (6, "quiet", pattern(CONST)),
      (6, "RUMBLE", pattern(TRI, 20))]),
]


def db_err(a, b):
    return abs(10 * math.log10(a / b)) if a > 0 and b > 0 else float("inf")


def run_scenario(exe, tmp, name, segments, seed):
    src = {"pat": L.PatternSource(), "xadc": L.XadcSource(seed)}
    blocks, expected, seg_start = [], [], []
    # Start the decimator at an arbitrary point of the source, like the hardware
    for nblk, label, fn in segments:
        seg_start.append(len(blocks))
        codes = fn(src, nblk * L.N * L.R)
        dec = L.decimate(codes)
        blocks += [dec[i * L.N:(i + 1) * L.N] for i in range(nblk)]
        expected += [label] * nblk

    c = run_c(exe, blocks, tmp)
    det = L.Detector()
    ref = [L.analyse_block(b) for b in blocks]
    ref_labels = [det.update(r) for r in ref]

    fails, max_db, max_pm = [], 0.0, 0.0
    allowed = set(expected) | {"quiet"}          # start-up output is always "quiet"
    for i, r in enumerate(ref):
        for path in ("q15", "float"):
            if c[path][i]["detect"] not in allowed:
                fails.append(f"block {i} {path}: '{c[path][i]['detect']}' must never appear here")
        settled = all(i - s >= HOLD for s in seg_start if s <= i) and \
            not any(0 <= i - s < HOLD for s in seg_start)
        for path in ("q15", "float"):
            row = c[path][i]
            if settled and row["detect"] != expected[i]:
                fails.append(f"block {i} {path}: '{row['detect']}' expected '{expected[i]}'")
        if settled and ref_labels[i] != expected[i]:
            fails.append(f"block {i} python: '{ref_labels[i]}' expected '{expected[i]}'")
        q = c["q15"][i]
        for band in ("rumble", "mains"):
            max_pm = max(max_pm, abs(float(q[f"{band}_pm"]) - r[f"{band}_pm"]))
            if r["rms"] >= L.C["DSP_MIN_RMS"] and r[f"{band}_pm"] >= SIGNIFICANT_PM:
                max_db = max(max_db, db_err(float(q[f"{band}_e"]), r[f"{band}_e"]))
    if max_db > DB_LIMIT:
        fails.append(f"Q15 band energy error {max_db:.3f} dB > {DB_LIMIT} dB")

    # Latency: blocks from a segment start until the C Q15 output shows the new label
    lat = []
    for s, (nblk, label, _) in zip(seg_start, segments):
        hit = next((i - s for i in range(s, s + nblk) if c["q15"][i]["detect"] == label), None)
        lat.append(hit)

    return dict(fails=fails, max_db=max_db, max_pm=max_pm, latency=lat,
                shares=[(int(c["q15"][i]["rumble_pm"]), int(c["q15"][i]["mains_pm"]), int(c["q15"][i]["rms"]))
                        for i in range(len(blocks))],
                labels=[c["q15"][i]["detect"] for i in range(len(blocks))])


def main():
    print(f"fs = {L.FS:.3f} Hz, N = {L.N}, bin = {L.FS / L.N:.3f} Hz, "
          f"rumble bins {L._RUMBLE.start}..{L._RUMBLE.stop - 1}, mains bins {L._MAINS.start}..{L._MAINS.stop - 1}\n")
    total_fail = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_host(tmp)
        print(f"{'scenario':52} {'result':6} {'Q15 dB err':>10} {'share err':>9}  steady (rumble/mains per mille, rms)")
        for n, (name, segs) in enumerate(SCENARIOS):
            r = run_scenario(exe, tmp, name, segs, seed=n + 1)
            ok = not r["fails"]
            total_fail += 0 if ok else 1
            rp, mp, rms = r["shares"][-1]
            print(f"{name:52} {'PASS' if ok else 'FAIL':6} {r['max_db']:10.3f} {r['max_pm']:7.1f}pm  {rp:4d} / {mp:4d}, rms {rms}")
            if len(segs) > 1:
                lat = ", ".join(f"{lbl}: {b} blk ({b * L.N / L.FS:.2f} s)" if b is not None else f"{lbl}: never"
                                for (_, lbl, _), b in zip(segs, r["latency"]))
                print(f"{'':52}        latency -> {lat}")
                print(f"{'':52}        labels  -> {' | '.join(r['labels'])}")
            for f in r["fails"][:5]:
                print(f"{'':52}        ! {f}")
    print(f"\n{'ALL SCENARIOS PASS' if total_fail == 0 else f'{total_fail} SCENARIO(S) FAILED'}")
    return 1 if total_fail else 0


if __name__ == "__main__":
    sys.exit(main())
