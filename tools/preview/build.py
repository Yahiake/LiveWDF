#!/usr/bin/env python3
"""Build a qualified modal Preview sidecar for one circuit, control snapshot and sample rate."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile

# Small LAPACK matrices fit on one core. Allow an explicit thread-count override.
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
import numpy as np
from modal_fit import modes_from_impulse, render


def errors(reference, candidate):
    difference = reference - candidate
    norm = np.linalg.norm(reference)
    peak = np.max(np.abs(reference))
    if not np.isfinite(difference).all() or norm <= 1e-10 or peak <= 1e-9:
        raise ValueError("qualification signal is silent or non-finite")
    return {
        "relative_rms_db": 20 * math.log10(max(1e-18, np.linalg.norm(difference) / norm)),
        "relative_peak_db": 20 * math.log10(max(1e-18, np.max(np.abs(difference)) / peak)),
        "peak_error_v": float(np.max(np.abs(difference))),
    }


def qualify(path, sample_rate, revision):
    def read(name):
        return np.fromfile(path / (name + ".f64"), dtype=np.float64)

    small, large, silence = map(read, ("small", "large", "zero"))
    bias = float(np.mean(silence))
    metrics = {"impulse_level_disagreement": errors(large, small)}
    disagreement = metrics["impulse_level_disagreement"]["relative_rms_db"]
    if disagreement > -45:
        raise ValueError(f"impulse depends on input level ({disagreement:.1f} dB relative RMS)")
    test_levels = (35, 360, 1200)
    probes = [(read(f"{v}.input"), read(f"{v}.wet") - bias) for v in test_levels]
    for direct, modes, impulse in modes_from_impulse(small, (4, 6, 8, 12, 16, 20, 24, 32)):
        metrics["impulse_fit"] = errors(small, impulse)
        if metrics["impulse_fit"]["relative_rms_db"] > -60:
            continue
        scores = [errors(wet, render(drive, direct, modes)) for drive, wet in probes]
        metrics["multilevel_probes"] = {f"{v}mV": s for v, s in zip(test_levels, scores)}
        if any(s["relative_rms_db"] > -55 or s["relative_peak_db"] > -35 for s in scores):
            continue
        return {
            "schema": "livewdf.modal-preview.v1",
            "document_sha256": hashlib.sha256((path / "document.json").read_bytes()).hexdigest(),
            "engine_revision": revision,
            "engine_sha256": (path / "engine.sha256").read_text().strip(),
            "sample_rate": sample_rate,
            "input_peak_v": 1.2,
            "output_bias_v": bias,
            "direct": direct,
            "modes": modes,
            "qualification": {"decision": "accepted", "metrics": metrics},
        }
    raise ValueError("no stable model met the impulse and multilevel waveform limits")


def build_probe(root, directory):
    if not (directory / "CMakeCache.txt").is_file():
        subprocess.run(["cmake", "-S", str(root), "-B", str(directory), "-DCMAKE_BUILD_TYPE=Release",
                        "-DLIVEWDF_BUILD_TESTS=OFF", "-DLIVEWDF_BUILD_EDITOR=OFF",
                        "-DLIVEWDF_BUILD_PLUGIN=OFF"], check=True)
    subprocess.run(["cmake", "--build", str(directory), "--config", "Release",
                    "--target", "livewdf_preview_probe", "--parallel", "2"], check=True)
    name = "livewdf_preview_probe" + (".exe" if os.name == "nt" else "")
    for parent in (directory, directory / "Release"):
        if (parent / name).is_file():
            return parent / name
    raise RuntimeError(f"cannot find {name} in {directory}")


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("sample_rate", nargs="?", type=int, default=48000)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build-dir", type=Path, default=root / "build-preview")
    parser.add_argument("--probe", type=Path, help="use an existing livewdf_preview_probe executable")
    parser.add_argument("--engine-revision", help="revision used to compile an external probe")
    args = parser.parse_args()
    output = args.output or args.file.with_name(f"{args.file.name}.{args.sample_rate}.preview.json")
    if not args.file.is_file():
        parser.error(f"missing circuit: {args.file}")
    if output.resolve() == args.file.resolve():
        parser.error("choose a sidecar path separate from the source circuit")
    if args.probe and not args.engine_revision:
        parser.error("--probe requires --engine-revision")
    probe = (args.probe or build_probe(root, args.build_dir)).resolve()
    revision = args.engine_revision or subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    with tempfile.TemporaryDirectory() as directory:
        subprocess.run([str(probe), str(args.file), str(args.sample_rate), directory], check=True)
        model = qualify(Path(directory), args.sample_rate, revision)
    serialized = json.dumps(model, indent=2, allow_nan=False) + "\n"
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=output.parent, delete=False) as tmp:
        tmp.write(serialized)
    try:
        os.replace(tmp.name, output)
    finally:
        Path(tmp.name).unlink(missing_ok=True)
    print(f"{output} ({len(model['modes'])} modes)")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
