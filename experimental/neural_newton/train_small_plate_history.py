#!/usr/bin/env python3
"""Fit a small next-sample plate-voltage predictor with the NAM training loss."""
import argparse
import json
import math
import random
import sys
import time
from pathlib import Path

import numpy as np
import torch
from scipy import fft
from torch import nn

from train_a2_plate import NAM_V3_SHA256, RATE, load_pair, verify_training_wave

PLATE_HISTORY = 8
HIDDEN_WIDTH = 14


def features(x, plate, guitar_scale):
    # Row t ends at the previous accepted solve. Each take starts from zero history.
    past = np.lib.stride_tricks.sliding_window_view(
        np.pad(plate, (PLATE_HISTORY, 0)), PLATE_HISTORY
    )[:-1, ::-1]
    return past.copy() if guitar_scale is None else np.column_stack((past, x / guitar_scale))


def epoch_batches(inputs, target, chunk, batch_size, rng):
    starts = list(range(0, len(target), chunk))
    rng.shuffle(starts)
    for offset in range(0, len(starts), batch_size):
        batch = starts[offset:offset + batch_size]
        x = np.zeros((len(batch), chunk, inputs.shape[1]), dtype=np.float32)
        y = np.zeros((len(batch), chunk), dtype=np.float32)
        mask = np.zeros_like(y)
        for index, start in enumerate(batch):
            count = min(chunk, len(target) - start)
            x[index, :count] = inputs[start:start + count]
            y[index, :count] = target[start:start + count]
            mask[index, :count] = 1
        yield x, y, mask


@torch.inference_mode()
def evaluate(model, pairs, plate_scale):
    result = {}
    for track, (inputs, target) in pairs.items():
        prediction = model(torch.from_numpy(inputs)).ravel().numpy().astype(np.float64) * plate_scale
        error = prediction - target
        rmse = np.sqrt(np.mean(error**2))
        power = np.mean(np.square(target, dtype=np.float64))
        if not math.isfinite(rmse) or not power > 0:
            raise ValueError(f"{track}: invalid held-out score")
        result[track] = {
            "samples": len(target),
            "mae_volts": float(np.mean(np.abs(error))),
            "rmse_volts": float(rmse),
            "persistence_rmse_volts": float(np.sqrt(np.mean((inputs[:, 0] * plate_scale - target)**2))),
            "lag_two_rmse_volts": float(np.sqrt(np.mean((inputs[:, 1] * plate_scale - target)**2))),
            "nmse_db": 10 * math.log10(max(rmse**2 / power, 1e-30)),
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rig-converter", type=Path, required=True)
    parser.add_argument("--captures", type=Path, required=True)
    parser.add_argument("--official-wave", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--guitar", action="store_true")
    parser.add_argument("--epochs", type=int, default=20)
    parser.add_argument("--chunk", type=int, default=32768)
    parser.add_argument("--batch-size", type=int, default=6)
    parser.add_argument("--threads", type=int, default=2)
    args = parser.parse_args()
    if min(args.epochs, args.batch_size, args.threads) <= 0 or args.chunk <= 1024:
        parser.error("epochs, threads and batch size must be positive, and a chunk must exceed 1024 samples")
    sys.path.insert(0, str(args.rig_converter.resolve() / "amp-fit"))
    from a2lite import TRAINING_LEVEL, loss as nam_loss

    guitar, plate = load_pair(args.captures, "train")
    input_gain, input_match = verify_training_wave(guitar, args.official_wave)
    plate_scale = float(np.sqrt(np.mean(np.square(plate, dtype=np.float64))) / TRAINING_LEVEL)
    if not math.isfinite(plate_scale) or plate_scale <= 0:
        raise ValueError("Training target must have finite nonzero power")
    guitar_scale = float(np.sqrt(np.mean(np.square(guitar, dtype=np.float64))) / TRAINING_LEVEL) if args.guitar else None
    target = plate / plate_scale
    inputs = features(guitar, target, guitar_scale)
    del guitar, plate
    pairs = {}
    for track in ("sire", "strat"):
        guitar, plate = load_pair(args.captures, track)
        pairs[track] = (features(guitar, plate / plate_scale, guitar_scale), plate)

    torch.set_num_threads(args.threads)
    torch.manual_seed(473)
    input_layer = nn.Linear(PLATE_HISTORY, HIDDEN_WIDTH)
    output_layer = nn.Linear(HIDDEN_WIDTH, 1)
    if args.guitar:
        conditioned = nn.Linear(PLATE_HISTORY + 1, HIDDEN_WIDTH)
        with torch.no_grad():
            conditioned.weight[:, :PLATE_HISTORY].copy_(input_layer.weight)
            conditioned.weight[:, PLATE_HISTORY].zero_()
            conditioned.bias.copy_(input_layer.bias)
        input_layer = conditioned
    model = nn.Sequential(input_layer, nn.LeakyReLU(.01), output_layer)
    optimizer = torch.optim.Adam(model.parameters(), lr=.004, weight_decay=3.17e-7)
    schedule = torch.optim.lr_scheduler.ExponentialLR(optimizer, gamma=.994)
    rng = random.Random(473)
    updates_per_epoch = math.ceil(math.ceil(len(target) / args.chunk) / args.batch_size)
    args.output.mkdir(parents=True, exist_ok=True)
    report = {
        "sample_rate": RATE,
        "weights": sum(p.numel() for p in model.parameters()),
        "plate_history": PLATE_HISTORY,
        "plate_scale_volts": plate_scale,
        "guitar_scale": guitar_scale,
        "samples_per_epoch": len(target),
        "training_signal_sha256": NAM_V3_SHA256,
        "official_input_gain": input_gain,
        "official_input_max_error": input_match,
        "epochs_requested": args.epochs,
        "updates_per_epoch": updates_per_epoch,
        "loss": "MSE + 0.0005 MRSTFT",
        "heldout_before_training": evaluate(model, pairs, plate_scale),
    }
    (args.output / "training.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(json.dumps(report, allow_nan=False), flush=True)
    scores = []
    updates = 0
    with fft.set_workers(args.threads):
        for epoch in range(1, args.epochs + 1):
            start = time.perf_counter()
            covered = 0
            objective = 0.
            squared = 0.
            for x, y, mask in epoch_batches(inputs, target, args.chunk, args.batch_size, rng):
                count = int(mask.sum())
                prediction = model(torch.from_numpy(x)).squeeze(-1)
                values = prediction.detach().numpy()
                loss, gradient = nam_loss(values, y, mask, 1.)
                if not math.isfinite(loss):
                    raise ValueError(f"Nonfinite loss in epoch {epoch}")
                optimizer.zero_grad()
                prediction.backward(torch.from_numpy(gradient))
                optimizer.step()
                updates += 1
                objective += loss * count
                squared += np.sum(((values - y) * mask)**2, dtype=np.float64)
                covered += count
            schedule.step()
            if covered != len(target) or updates != epoch * updates_per_epoch:
                raise ValueError(f"Coverage mismatch, epoch {epoch}: {covered} samples and {updates} total updates")
            score = {
                "epoch": epoch,
                "samples": covered,
                "optimizer_updates": updates,
                "train_mse": float(squared / covered),
                "train_objective": objective / covered,
                "train_seconds": time.perf_counter() - start,
                "heldout": evaluate(model, pairs, plate_scale),
            }
            scores.append(score)
            (args.output / "history.json").write_text(json.dumps(scores, indent=2, allow_nan=False) + "\n")
            print(json.dumps(score, allow_nan=False), flush=True)

    state = {
        "plate_history": PLATE_HISTORY,
        "plate_scale_volts": plate_scale,
        "guitar_scale": guitar_scale,
        "input_weights": model[0].weight.tolist(),
        "input_bias": model[0].bias.tolist(),
        "output_weights": model[2].weight[0].tolist(),
        "output_bias": model[2].bias.item(),
    }
    (args.output / "guess.json").write_text(json.dumps(state, indent=2, allow_nan=False) + "\n")


if __name__ == "__main__":
    main()
