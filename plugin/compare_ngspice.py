#!/usr/bin/env python3
"""Compare a LiveWDF preamp run against the ngspice export of the same document.

Inputs (all in <data-dir>, produced by `LiveWDF_PluginBench` and ngspice):

  engine_dc.csv    `--spice` writes this: net,volts from the engine's solveDc()
  dc.csv           ngspice `wrdata dc.csv` after `op`
  engine_tran.csv  `--probe` writes this: time + probe nets, 1 kHz / 1 mV
  tran.csv         ngspice transient, linearized to 48 kHz
  ac.csv           ngspice `ac` sweep, `mag(v(out))` / `ph(v(out))`
  plugin_tran.csv  `--plugin-probe` writes this: time,in,plugin_volts

The continuous ngspice AC result is the model reference. A 48 kHz transient
comparison also contains the engine's fixed-step trapezoidal discretization;
use `--probe-rate 96000` / `192000` and `--engine-tran engine_tran_192k.csv`
to separate the two. The transient comparison is skipped automatically when
the engine and ngspice time steps differ.

Usage:
  python compare_ngspice.py <data-dir> [--engine-tran FILE] [--ngspice-tran FILE]
                                    [--start S] [--stop S] [--freq HZ]
"""

import argparse
import os
import sys

import numpy as np


def load_engine_dc(path):
    values = {}
    with open(path, encoding="utf-8") as f:
        next(f)  # net,volts
        for line in f:
            line = line.strip()
            if not line:
                continue
            net, value = line.split(",")
            values[net] = float(value)
    return values


def load_engine_tran(path):
    with open(path, encoding="utf-8") as f:
        names = f.readline().strip().split(",")
    data = np.loadtxt(path, delimiter=",", skiprows=1)
    if data.ndim == 1:
        data = data[None, :]
    return names, data


def load_ngspice_table(path):
    """Whitespace table with a header. Column 0 is the scale, then the vectors."""
    with open(path, encoding="utf-8") as f:
        header = f.readline().split()
    data = np.loadtxt(path, skiprows=1)
    if data.ndim == 1:
        data = data[None, :]
    return header, data


def fit_fundamental(t, x, freq):
    w = 2.0 * np.pi * freq * t
    A = np.column_stack([np.cos(w), np.sin(w), np.ones_like(w)])
    c, _, _, _ = np.linalg.lstsq(A, x, rcond=None)
    amp = float(np.hypot(c[0], c[1]))
    phase = float(np.degrees(np.arctan2(-c[1], c[0])))
    return amp, phase


def ac_error(engine, ngspice):
    e = engine - engine.mean()
    n = ngspice - ngspice.mean()
    sig = np.sqrt(np.mean(n * n))
    if sig == 0.0:
        return 0.0, 0.0
    diff = e - n
    return float(np.sqrt(np.mean(diff * diff)) / sig), float(np.max(np.abs(diff)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_dir")
    ap.add_argument("--engine-tran", default="engine_tran.csv")
    ap.add_argument("--ngspice-tran", default="tran.csv")
    ap.add_argument("--start", type=float, default=0.15)
    ap.add_argument("--stop", type=float, default=0.20)
    ap.add_argument("--freq", type=float, default=1000.0)
    args = ap.parse_args()

    d = args.data_dir
    eng_names, eng = load_engine_tran(os.path.join(d, args.engine_tran))
    ng_header, ng = load_ngspice_table(os.path.join(d, args.ngspice_tran))
    t = eng[:, 0]
    window = (t >= args.start) & (t < args.stop)
    if not np.any(window):
        sys.exit("empty comparison window")

    eng_dt = float(t[1] - t[0]) if len(t) > 1 else 0.0
    ng_dt = float(ng[1, 0] - ng[0, 0]) if len(ng) > 1 else 0.0
    same_rate = ng_dt > 0.0 and abs(eng_dt - ng_dt) <= 1e-9
    eng_fs = 1.0 / eng_dt if eng_dt > 0.0 else 0.0
    ng_fs = 1.0 / ng_dt if ng_dt > 0.0 else 0.0

    print(f"LiveWDF preamp vs ngspice: {os.path.abspath(d)}")
    print(f"  engine transient: {args.engine_tran} (fs={eng_fs:.0f} Hz)")
    print(f"  ngspice transient: {args.ngspice_tran} (fs={ng_fs:.0f} Hz)")
    print(f"  window {args.start:.3f}-{args.stop:.3f} s, {int(np.sum(window))} samples, "
          f"{args.freq:.0f} Hz")
    if not same_rate:
        print("  NOTE: engine and ngspice time steps differ; transient and plugin "
              "comparisons skipped.")

    # --- DC operating point -------------------------------------------------
    try:
        eng_dc = load_engine_dc(os.path.join(d, "engine_dc.csv"))
        with open(os.path.join(d, "dc.csv"), encoding="utf-8") as f:
            f.readline()
        dc = np.loadtxt(os.path.join(d, "dc.csv"), skiprows=1)
        # Column 0 is the scale; the rest are the probe vectors in order.
        dc = np.atleast_2d(dc)[0, 1:]
        probes = eng_names[1:]
        worst = (0.0, "")
        print("\nDC operating point:")
        print(f"  {'net':8} {'engine (V)':>16} {'ngspice (V)':>16} {'diff (V)':>12}")
        for j, net in enumerate(probes):
            if j >= len(dc):
                break
            e = eng_dc.get(net)
            if e is None:
                continue
            diff = abs(e - float(dc[j]))
            if diff > worst[0]:
                worst = (diff, net)
            print(f"  {net:8} {e:16.9g} {float(dc[j]):16.9g} {e - float(dc[j]):12.4g}")
        print(f"  worst DC |diff| = {worst[0]:.3g} V on {worst[1]}")
    except FileNotFoundError as exc:
        print(f"\nDC comparison skipped: {exc}")

    # --- output fundamental ------------------------------------------------
    out = eng_names.index("out")
    ae, pe = fit_fundamental(t[window], eng[window, out], args.freq)

    # --- continuous small-signal AC ----------------------------------------
    ac_path = os.path.join(d, "ac.csv")
    if os.path.exists(ac_path):
        with open(ac_path, encoding="utf-8") as f:
            ach = f.readline().split()
        ac = np.atleast_2d(np.loadtxt(ac_path, skiprows=1))
        idx = int(np.argmin(np.abs(ac[:, 0] - args.freq)))
        mi = ach.index("mag(v(out))")
        pi = ach.index("ph(v(out))")
        aac = float(ac[idx, mi])
        pac = float(np.degrees(ac[idx, pi]) - 90.0)  # ph() is radians; AC source is cosine
        print("\nContinuous AC cross-check (model reference):")
        print(f"  ngspice AC {aac:.7g} V/V @ {pac:+.5f} deg (sine reference)")
        print(f"  engine     {ae / 0.001:.7g} V/V @ {pe:+.5f} deg "
              f"(1 mV drive, fs={eng_fs:.0f} Hz)")
        print(f"  amplitude ratio {ae / (aac * 0.001):.7f}  "
              f"phase diff {pe - pac:+.5f} deg")
        if eng_fs < 90000.0:
            print("  NOTE: at 48 kHz the engine's fixed-step discretization is part of "
                  "this ratio; re-run with --probe-rate 96000/192000 to check convergence.")

    if not same_rate:
        return

    # --- matched-rate transient comparison ----------------------------------
    n = min(len(eng), len(ng))
    eng_cmp = eng[:n]
    ng_cmp = ng[:n]
    t_cmp = eng_cmp[:, 0]
    window_cmp = (t_cmp >= args.start) & (t_cmp < args.stop)
    if not np.any(window_cmp):
        print("\nTransient comparison skipped: no samples in window at matched rate.")
        return

    e_out = eng_cmp[window_cmp, out]
    n_out = ng_cmp[window_cmp, out]
    an, pn = fit_fundamental(t_cmp[window_cmp], n_out, args.freq)
    rel, maxabs = ac_error(e_out, n_out)
    print("\nOutput (out), matched-rate transient:")
    print(f"  engine  {ae:.7g} V @ {pe:+.5f} deg")
    print(f"  ngspice {an:.7g} V @ {pn:+.5f} deg")
    print(f"  amplitude ratio {ae / an:.7f}  phase diff {pe - pn:+.5f} deg")
    print(f"  AC RMS error {rel * 100:.4f}%  max |error| {maxabs:.3g} V")

    print("\nAC-only per node (mean removed; engine vs matched-rate ngspice):")
    print(f"  {'net':8} {'rel RMS':>10} {'max abs V':>12}")
    for j, net in enumerate(eng_names[1:], start=1):
        rel_j, max_j = ac_error(eng_cmp[window_cmp, j], ng_cmp[window_cmp, j])
        print(f"  {net:8} {rel_j * 100:9.3f}% {max_j:12.4g}")

    # --- plugin path --------------------------------------------------------
    plugin_path = os.path.join(d, "plugin_tran.csv")
    if os.path.exists(plugin_path):
        with open(plugin_path, encoding="utf-8") as f:
            pnames = f.readline().strip().split(",")
        plugin = np.loadtxt(plugin_path, delimiter=",", skiprows=1)[:n]
        p_out = plugin[window_cmp, pnames.index("plugin_volts")]
        ap, pp = fit_fundamental(t_cmp[window_cmp], p_out, args.freq)
        rel_pe, max_pe = ac_error(e_out, p_out)
        rel_pn, max_pn = ac_error(p_out, n_out)
        print("\nPlugin path (matched rate):")
        print(f"  plugin/engine  AC RMS {rel_pe * 100:.3g}%  max {max_pe:.3g} V  "
              f"({ap / ae:.7f}, {pp - pe:+.5f} deg)")
        print(f"  plugin/ngspice AC RMS {rel_pn * 100:.4f}%  max {max_pn:.3g} V  "
              f"({ap / an:.7f}, {pp - pn:+.5f} deg)")
        print(f"  plugin fundamental {ap:.7g} V @ {pp:+.5f} deg")


if __name__ == "__main__":
    main()
