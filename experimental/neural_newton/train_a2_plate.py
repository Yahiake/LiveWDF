#!/usr/bin/env python3
"""Fit the stock 1,871-value NAM A2-Lite model to the Rectifier plate differential."""
import argparse
import hashlib
import json
import math
import random
import sys
import time
from pathlib import Path

import numpy as np

NAM_V3_SHA256 = "70f8ec7f25686a1bd77f25973de8e51a6721e957e81eec121822e5e53366bc41"
RATE = 48000


def load_pair(folder, track):
    x = np.fromfile(folder / f"{track}.x.f32", dtype="<f4")
    y = np.fromfile(folder / f"{track}.y.f32", dtype="<f4")
    if len(x) != len(y) or not len(x) or not np.all(np.isfinite(x)) or not np.all(np.isfinite(y)):
        raise ValueError(f"{track}: expected finite, nonempty, sample-aligned float32 input and target")
    return x, y


def evaluate(net, render, pairs, scale):
    info = {}
    for track, (x, y) in pairs.items():
        prediction = render(net, x, block=32768).astype(np.float64) * scale
        rmse = float(np.sqrt(np.mean(np.square(prediction - y))))
        power = float(np.mean(np.square(y, dtype=np.float64)))
        if not math.isfinite(rmse) or not (power > 0):
            raise ValueError(f"{track}: invalid held-out score")
        info[track] = {
            "samples": len(x),
            "rmse_volts": rmse,
            "nmse_db": 10 * math.log10(max(rmse**2 / power, 1e-30)),
        }
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rig-converter", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--captures", type=Path, required=True)
    parser.add_argument("--official-wave", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--epochs", type=int, default=20)
    parser.add_argument("--threads", type=int, default=6)
    parser.add_argument("--chunk", type=int, default=8192)
    parser.add_argument("--learning-rate", type=float)
    parser.add_argument("--recipe", choices=("mse", "nam"), default="mse")
    args = parser.parse_args()
    nam_recipe = args.recipe == "nam"
    learning_rate = args.learning_rate if args.learning_rate is not None else (.004 if nam_recipe else .002)
    weight_decay = 3.17e-7 if nam_recipe else 0.
    if min(args.epochs, args.threads, args.chunk) <= 0 or learning_rate <= 0:
        parser.error("epochs, threads, chunk size and learning rate must be positive")
    if hashlib.sha256(args.official_wave.read_bytes()).hexdigest() != NAM_V3_SHA256:
        raise ValueError("Training signal is not the verified official NAM v3.0.0 WAV")
    amp_fit = args.rig_converter.resolve() / "amp-fit"
    sys.path.insert(0, str(amp_fit))
    from a2lite import HISTORY, RECEPTIVE_FIELD, TRAINING_LEVEL, WEIGHT_COUNT, KernelNet
    from a2lite import epoch_batches, identity_weights, loss as nam_loss, render
    from nam_export import _model_json, standard_a2lite

    x, physical_y = load_pair(args.captures, "train")
    validation = {track: load_pair(args.captures, track) for track in ("sire", "strat")}
    expected = 190 * RATE
    if len(x) != expected:
        raise ValueError(f"Expected every sample of NAM v3 ({expected}), got {len(x)}")
    from scipy.io import wavfile
    wave_rate, wave_integer = wavfile.read(args.official_wave)
    if wave_rate != RATE or wave_integer.ndim != 1 or len(wave_integer) != expected:
        raise ValueError("Expected the full mono NAM v3 waveform at 48 kHz")
    official = np.asarray(wave_integer, np.float32) / np.float32(2**31)
    input_gain = float(np.max(np.abs(x)) / np.max(np.abs(official)))
    input_match = float(np.max(np.abs(x - official * np.float32(input_gain))))
    if not (input_gain > 0 and input_match < 2e-7):
        raise ValueError(f"Training audio differs from the official signal with uniform gain: {input_match}")
    del wave_integer, official
    y_scale = float(np.sqrt(np.mean(np.square(physical_y, dtype=np.float64))) / .125)
    if not math.isfinite(y_scale) or y_scale <= 0:
        raise ValueError("Training target must have finite nonzero power")
    y = np.asarray(physical_y / y_scale, dtype=np.float32)
    del physical_y
    args.output.mkdir(parents=True, exist_ok=True)
    net = KernelNet(library=args.kernel.resolve(), threads=args.threads)
    net.weights = identity_weights()
    if WEIGHT_COUNT != 1871 or RECEPTIVE_FIELD != 6347 or net.weight_count != WEIGHT_COUNT:
        raise ValueError("Expected the literal stock 48 kHz A2 Lite architecture")

    rng = random.Random(473)
    rows = [(None, None, x, None, y)]
    updates_per_epoch = math.ceil(math.ceil(len(x) / args.chunk) / args.threads)
    total = updates_per_epoch * args.epochs
    completed = 0
    summaries = []
    template = standard_a2lite(json.loads((amp_fit / "data/preview/amps/jc.nam").read_text()))
    history_file = args.output / "history.json"

    def save():
        weights = net.weights
        weights[-1] *= y_scale
        if len(weights) != 1871 or not np.all(np.isfinite(weights)):
            raise ValueError("Invalid physical output weights")
        model = _model_json(template, weights, "Rectifier half-differential plate voltage guess")
        model["metadata"] = {"name": "Rectifier half-differential plate voltage guess"}
        standard_a2lite(model)
        for filename, data in (
            ("weights.f32", weights.astype("<f4").tobytes()),
            ("a2-guess.nam", (json.dumps(model, allow_nan=False, separators=(",", ":")) + "\n").encode()),
            ("history.json", (json.dumps(summaries, allow_nan=False, indent=2) + "\n").encode()),
        ):
            path = args.output / filename
            temp = path.with_suffix(path.suffix + ".tmp")
            temp.write_bytes(data)
            temp.replace(path)

    report = {
        "sample_rate": RATE, "weights": net.weight_count, "receptive_field": RECEPTIVE_FIELD,
        "samples_per_epoch": len(x), "training_signal_sha256": NAM_V3_SHA256,
        "official_input_gain": input_gain, "official_input_max_error": input_match,
        "target_normalization_volts": y_scale, "epochs_requested": args.epochs,
        "chunk": args.chunk, "threads": args.threads, "updates_per_epoch": updates_per_epoch,
        "learning_rate": learning_rate, "recipe": args.recipe,
        "loss": "MSE + 0.0005 MRSTFT" if nam_recipe else "MSE",
        "weight_decay": weight_decay,
        "heldout_before_training": evaluate(net, render, validation, y_scale),
    }
    (args.output / "training.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(json.dumps(report, allow_nan=False), flush=True)
    for epoch in range(1, args.epochs + 1):
        started = time.perf_counter()
        count = 0
        square_error = 0.
        objective = 0.
        for windows, wanted, mask in epoch_batches(rows, args.chunk, HISTORY, args.threads, rng):
            samples = int(mask.sum())
            result = net.forward(windows)
            error = (result - wanted) * mask
            square = float(np.sum(np.square(error), dtype=np.float64))
            if not math.isfinite(square):
                raise ValueError(f"Nonfinite training loss in epoch {epoch} after {count} samples")
            if nam_recipe:
                batch_loss, gradient = nam_loss(result, wanted, mask, TRAINING_LEVEL / .125)
                rate = learning_rate * .994 ** (epoch - 1)
            else:
                batch_loss = square / samples
                gradient = error * np.float32(2. / samples)
                rate = learning_rate * .5 * (1. + math.cos(math.pi * completed / total))
            if not math.isfinite(batch_loss):
                raise ValueError(f"Nonfinite objective in epoch {epoch} after {count} samples")
            grads, _ = net.backward(gradient)
            if weight_decay:
                grads += np.float32(weight_decay) * net.weights
            norm = net.adam(grads, rate)
            if not math.isfinite(norm):
                raise ValueError(f"Nonfinite gradient in epoch {epoch} after {count} samples")
            count += samples
            completed += 1
            square_error += square
            objective += batch_loss * samples
        if count != expected or completed != epoch * updates_per_epoch:
            raise ValueError(f"Coverage mismatch, epoch {epoch}: {count} samples and {completed} total updates")
        elapsed = time.perf_counter() - started
        score = {
            "epoch": epoch, "samples": count, "optimizer_updates": completed,
            "train_mse": square_error / count, "train_objective": objective / count, "train_seconds": elapsed,
            "heldout": evaluate(net, render, validation, y_scale),
        }
        summaries.append(score)
        save()
        print(json.dumps(score, allow_nan=False), flush=True)
    history = json.loads(history_file.read_text())
    if len(history) != args.epochs or sum(item["samples"] for item in history) != expected * args.epochs:
        raise ValueError("Saved training history does not cover all epochs")


if __name__ == "__main__":
    main()
