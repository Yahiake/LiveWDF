"""Plot CSVs from the parts-validation tests. Requires matplotlib and numpy."""

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def plot(data_dir, output_dir, part, title, xlabel, ylabel, xscale, yscale,
         references, residual_unit, residual_scale=1.0):
    source = data_dir / f"parts_{part}.csv"
    if not source.exists():
        return
    data = np.genfromtxt(source, delimiter=",", names=True)
    x, native, *others = (data[key] for key in data.dtype.names[:2 + len(references)])
    fig, (main, residual) = plt.subplots(
        2, 1, sharex=True, figsize=(8.2, 5.8), layout="constrained",
        gridspec_kw={"height_ratios": [3, 2]},
    )
    fig.suptitle(title, fontsize=15, fontweight="semibold")
    main.plot(x, native, color="#174b8c", lw=2.4, label="LiveWDF")
    for values, (label, color) in zip(others, references, strict=True):
        main.plot(x, values, "--", color=color, lw=1.5, label=label)
        residual.plot(x, (native - values) * residual_scale, color=color, label=label)
    main.set(xscale=xscale, yscale=yscale, ylabel=ylabel)
    residual.axhline(0, lw=0.8, color="#8a929c")
    residual.set(xlabel=xlabel, ylabel=f"LiveWDF − reference\n({residual_unit})")
    for ax in (main, residual):
        ax.grid(alpha=0.22)
        ax.legend(loc="best", frameon=False)
    fig.savefig(output_dir / f"{part}.png", dpi=160)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_directory", type=Path)
    parser.add_argument("output_directory", type=Path)
    args = parser.parse_args()
    args.output_directory.mkdir(parents=True, exist_ok=True)
    orange = "#aa4228"
    green = "#26815c"
    spice = ("ngspice, exported equations", orange)
    cases = [
        ("that2180", "THAT2180 gain control", "Control voltage (V)", "Gain (dB)",
         "linear", "linear", [spice, ("THAT Eq. 2, typical", green)], "dB"),
        ("rms", "Log RMS detector, 1 V to 2 V step", "Time (s)", "Control output (V)",
         "linear", "linear", [spice, ("Nominal 16 ms response", green)], "nV", 1e9),
        ("mn3005", "MN3005 clock-to-delay response", "Clock frequency (Hz)", "Delay (s)",
         "log", "log", [("Panasonic, 4096 stages", green)], "µs", 1e6),
        ("ca3080", "CA3080 transconductance and bias current", "Bias current (A)",
         "Transconductance (S)", "log", "log",
         [spice, ("Intersil, typical at 500 µA", green)], "µS", 1e6),
        ("op275", "OP275 closed-loop gain of 100", "Frequency (Hz)", "Gain (dB)",
         "log", "linear", [("ngspice, corrected Rev. C deck", orange),
          ("Analog Devices TPC 5, approx.", green)], "dB"),
        ("op275_wave", "OP275 steady-state output, 20 kHz tone", "Time (s)", "Output (V)",
         "linear", "linear", [("ngspice, corrected Rev. C deck", orange)], "mV", 1e3),
    ]
    for case in cases:
        plot(args.csv_directory, args.output_directory, *case)


if __name__ == "__main__":
    main()
