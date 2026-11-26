#!/usr/bin/env python3
"""Fit a joint next-sample Newton seed from accepted port voltages and dry input."""
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

from train_a2_plate import NAM_V3_SHA256, RATE, verify_training_wave

PORTS = 33
VOLTAGE_FLOOR = .003


def load_capture(folder, track):
    def read(suffix):
        return np.fromfile(folder / f"{track}.{suffix}.f32", dtype="<f4")

    guitar = read("x")
    accepted = read("y").reshape(-1, PORTS)
    seed = read("seed").reshape(-1, PORTS)
    initial = read("initial").reshape(2, PORTS)
    rejected = np.fromfile(folder / f"{track}.rejected.u8", dtype=np.uint8)
    circuit_id = int((folder / f"{track}.id").read_text())
    if not 0 <= circuit_id < 2**64 or not len(guitar) or not len(guitar) == len(accepted) == len(seed) == len(rejected):
        raise ValueError(f"{track}: invalid or misaligned capture")
    states = np.empty((len(guitar) + 2, PORTS), dtype=np.float32)
    states[:2] = initial[::-1]
    states[2:] = accepted
    if not np.isfinite(guitar).all() or not np.isfinite(states).all() or not np.isfinite(seed).all() or (rejected > 1).any():
        raise ValueError(f"{track}: invalid capture values")
    past_rejected = np.empty(len(rejected), dtype=bool)
    past_rejected[0] = False
    past_rejected[1:] = rejected[:-1]
    # A run of eight rejects rebuilds both history slots at the operating point.
    recovered = np.zeros(len(rejected) + 1, dtype=bool)
    for start, end in np.flatnonzero(np.diff(np.r_[0, rejected, 0])).reshape(-1, 2):
        recovered[start + 9:end + 2:8] = True
    return guitar, states, seed, past_rejected, recovered, circuit_id


def raw_inputs(capture, start, end, include_stock):
    guitar, states, stock, past_rejected, recovered, _ = capture
    previous = states[start + 1:end + 1]
    older = states[start:end]
    width = (2 + include_stock) * PORTS + 1
    x = np.empty((end - start, width), dtype=np.float32)
    x[:, :PORTS] = previous
    delta = x[:, PORTS:2 * PORTS]
    np.subtract(previous, older, out=delta)
    after_recovery = np.flatnonzero(recovered[start:end])
    delta[after_recovery] = previous[after_recovery] - stock[start + after_recovery - 1]
    recovering = recovered[start + 1:end + 1]
    x[recovering, :PORTS] = stock[start:end][recovering]
    delta[past_rejected[start:end] | recovering] = 0.
    if include_stock:
        x[:, 2 * PORTS:3 * PORTS] = stock[start:end]
    x[:, -1] = guitar[start:end]
    return x


def stats(capture, include_stock, nam_level):
    guitar, states = capture[:2]
    # A voltage below the Preview tolerance gets no more weight than that tolerance.
    center_port = states[2:].mean(axis=0, dtype=np.float64).astype(np.float32)
    scale_port = (np.maximum(states[2:].std(axis=0, dtype=np.float64), VOLTAGE_FLOOR) / nam_level).astype(np.float32)
    width = (2 + include_stock) * PORTS + 1
    total, square = np.zeros(width), np.zeros(width)
    minimum, maximum = np.full(width, np.inf), np.full(width, -np.inf)
    for start in range(0, len(guitar), 65536):
        end = min(start + 65536, len(guitar))
        x = raw_inputs(capture, start, end, include_stock)
        total += x.sum(axis=0, dtype=np.float64)
        square += np.square(x, dtype=np.float64).sum(axis=0)
        minimum = np.minimum(minimum, x.min(axis=0))
        maximum = np.maximum(maximum, x.max(axis=0))
    center = total / len(guitar)
    sigma = np.sqrt(np.maximum(square / len(guitar) - center**2, 0.))
    # Keep the full training range inside the native model's 12-unit input guard.
    floor = np.full(width, VOLTAGE_FLOOR)
    floor[-1] = 1e-6
    scale = np.maximum.reduce((sigma, floor, (maximum - center) / 8, (center - minimum) / 8))
    return center.astype(np.float32), (1. / scale).astype(np.float32), center_port, scale_port


def epoch_batches(capture, include_stock, settings, chunk, batch_size, rng):
    guitar, states, stock = capture[:3]
    center, inv_scale, center_port, scale_port = settings
    starts = list(range(0, len(guitar), chunk))
    rng.shuffle(starts)
    for offset in range(0, len(starts), batch_size):
        batch = starts[offset:offset + batch_size]
        inputs = np.zeros((len(batch), chunk, len(center)), dtype=np.float32)
        guess = np.zeros((len(batch), chunk, PORTS), dtype=np.float32)
        target = np.zeros_like(guess)
        mask = np.zeros((len(batch), chunk), dtype=np.float32)
        for index, start in enumerate(batch):
            end = min(start + chunk, len(guitar))
            n = end - start
            raw = raw_inputs(capture, start, end, include_stock)
            inputs[index, :n] = (raw - center) * inv_scale
            guess[index, :n] = (stock[start:end] - center_port) / scale_port
            target[index, :n] = (states[start + 2:end + 2] - center_port) / scale_port
            mask[index, :n] = 1
        yield inputs, guess, target, mask


@torch.inference_mode()
def evaluate(model, captures, settings, include_stock):
    center, inv_scale, _, scale_port = settings
    scores = {}
    for track, capture in captures.items():
        guitar, states, stock = capture[:3]
        squared, native, lag1, lag2, extrapolated = [np.zeros(PORTS) for _ in range(5)]
        guard_hits = 0
        for start in range(0, len(guitar), 32768):
            end = min(start + 32768, len(guitar))
            x = (raw_inputs(capture, start, end, include_stock) - center) * inv_scale
            correction = model(torch.from_numpy(x)).numpy()
            guard_hits += int(((np.abs(x) > 12).any(axis=1) | (np.abs(correction) > 12).any(axis=1)).sum())
            previous = states[start + 1:end + 1]
            older = states[start:end]
            target = states[start + 2:end + 2]
            squared += np.square(stock[start:end] + correction * scale_port - target, dtype=np.float64).sum(axis=0)
            native += np.square(stock[start:end] - target, dtype=np.float64).sum(axis=0)
            lag1 += np.square(previous - target, dtype=np.float64).sum(axis=0)
            lag2 += np.square(older - target, dtype=np.float64).sum(axis=0)
            extrapolated += np.square(2 * previous - older - target, dtype=np.float64).sum(axis=0)
        total = len(guitar)
        scores[track] = {
            "samples": total,
            "normalized_rmse": math.sqrt(np.mean(squared / total / scale_port**2)),
            "stock_normalized_rmse": math.sqrt(np.mean(native / total / scale_port**2)),
            "lag1_normalized_rmse": math.sqrt(np.mean(lag1 / total / scale_port**2)),
            "lag2_normalized_rmse": math.sqrt(np.mean(lag2 / total / scale_port**2)),
            "extrapolated_normalized_rmse": math.sqrt(np.mean(extrapolated / total / scale_port**2)),
            "per_port_rmse_volts": np.sqrt(squared / total).tolist(),
            "per_port_stock_rmse_volts": np.sqrt(native / total).tolist(),
            "per_port_lag1_rmse_volts": np.sqrt(lag1 / total).tolist(),
            "per_port_lag2_rmse_volts": np.sqrt(lag2 / total).tolist(),
            "standardization_guard_samples": guard_hits,
        }
    return scores


def save(model, settings, circuit_id, path):
    center, inv_scale, _, scale_port = settings
    temporary = path.with_suffix(".tmp")
    with temporary.open("w") as stream:
        stream.write(f"{circuit_id} 2 {len(center)}\n")
        for vector in (center, inv_scale, scale_port):
            np.savetxt(stream, vector[None], fmt="%.9g")
        for layer in (model[0], model[2]):
            stream.write(f"{layer.out_features}\n")
            np.savetxt(stream, layer.weight.detach().numpy(), fmt="%.9g")
            np.savetxt(stream, layer.bias.detach().numpy()[None], fmt="%.9g")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rig-converter", type=Path, required=True)
    parser.add_argument("--captures", type=Path, required=True)
    parser.add_argument("--official-wave", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stock", action="store_true", help="Add the stock initial guess as input")
    parser.add_argument("--epochs", type=int, default=20)
    parser.add_argument("--width", type=int, default=14)
    parser.add_argument("--threads", type=int, default=6)
    parser.add_argument("--chunk", type=int, default=8192)
    parser.add_argument("--batch-size", type=int, default=8)
    args = parser.parse_args()
    if min(args.epochs, args.threads, args.batch_size) <= 0 or not 1 <= args.width <= 64 or args.chunk <= 1024:
        parser.error("Expected positive epochs, threads and batch size, width 1–64 and chunk larger than 1024")
    sys.path.insert(0, str(args.rig_converter.resolve() / "amp-fit"))
    from a2lite import TRAINING_LEVEL, loss as nam_loss

    capture = load_capture(args.captures, "train")
    guitar, circuit_id = capture[0], capture[-1]
    input_gain, max_error = verify_training_wave(guitar, args.official_wave)
    heldout = {track: load_capture(args.captures, track) for track in ("sire", "strat")}
    if any(capture[-1] != circuit_id for capture in heldout.values()):
        raise ValueError("Captured circuit or controls differ across tracks")
    settings = stats(capture, args.stock, TRAINING_LEVEL)
    torch.manual_seed(473)
    torch.set_num_threads(args.threads)
    width = 2 * PORTS + 1
    inputs = nn.Linear(width, args.width)
    if args.stock:
        extended = nn.Linear(width + PORTS, args.width)
        with torch.no_grad():
            extended.weight[:, :width - 1].copy_(inputs.weight[:, :-1])
            extended.weight[:, width - 1:-1].zero_()
            extended.weight[:, -1].copy_(inputs.weight[:, -1])
            extended.bias.copy_(inputs.bias)
        inputs = extended
    model = nn.Sequential(inputs, nn.LeakyReLU(.01), nn.Linear(args.width, PORTS))
    nn.init.zeros_(model[2].weight)
    nn.init.zeros_(model[2].bias)
    optimizer = torch.optim.Adam(model.parameters(), lr=.004, weight_decay=3.17e-7)
    schedule = torch.optim.lr_scheduler.ExponentialLR(optimizer, gamma=.994)
    rng = random.Random(473)
    spectral_rng = random.Random(641)
    sampled_ports = list(range(PORTS))
    coverage = np.zeros(PORTS, dtype=int)
    iterations = math.ceil(math.ceil(len(guitar) / args.chunk) / args.batch_size)
    args.output.mkdir(parents=True, exist_ok=True)
    report = {
        "circuit_id": circuit_id,
        "ports": PORTS,
        "stock_input": args.stock,
        "weights": sum(p.numel() for p in model.parameters()),
        "sample_rate": RATE,
        "samples_per_epoch": len(guitar),
        "training_signal_sha256": NAM_V3_SHA256,
        "official_input_gain": input_gain,
        "official_input_max_error": max_error,
        "loss": "All-port normalized MSE + 0.0005 MRSTFT on one balanced port per batch",
        "port_std_floor_volts": VOLTAGE_FLOOR,
        "epochs_requested": args.epochs,
        "updates_per_epoch": iterations,
        "heldout_before_training": evaluate(model, heldout, settings, args.stock),
    }
    (args.output / "training.json").write_text(json.dumps(report, allow_nan=False, indent=2) + "\n")
    print(json.dumps(report, allow_nan=False), flush=True)
    history = []
    updates = 0
    with fft.set_workers(args.threads):
        for epoch in range(1, args.epochs + 1):
            started = time.perf_counter()
            samples = 0
            mse = objective = 0.
            batches = epoch_batches(capture, args.stock, settings, args.chunk, args.batch_size, rng)
            for x, guess, target, mask in batches:
                if updates % PORTS == 0:
                    spectral_rng.shuffle(sampled_ports)
                port = sampled_ports[updates % PORTS]
                coverage[port] += 1
                count = int(mask.sum())
                correction = model(torch.from_numpy(x))
                predicted = guess + correction.detach().numpy()
                error = (predicted - target) * mask[..., None]
                batch_mse = float(np.sum(np.square(error), dtype=np.float64)) / (count * PORTS)
                gradient = error * np.float32(2. / (count * PORTS))
                scalar_loss, scalar_grad = nam_loss(predicted[..., port], target[..., port], mask, 1.)
                scalar_error = error[..., port]
                scalar_size = scalar_error.size
                # The sampled per-port objective only contributes its spectral term.
                spectral_loss = scalar_loss - float(np.square(scalar_error).mean())
                gradient[..., port] += scalar_grad - scalar_error * np.float32(2. / scalar_size)
                batch_objective = batch_mse + spectral_loss
                if not math.isfinite(batch_objective):
                    raise ValueError(f"Nonfinite loss in epoch {epoch}")
                optimizer.zero_grad()
                correction.backward(torch.from_numpy(gradient))
                optimizer.step()
                updates += 1
                samples += count
                mse += batch_mse * count
                objective += batch_objective * count
            schedule.step()
            if samples != len(guitar) or updates != epoch * iterations:
                raise ValueError(f"Epoch {epoch} did not cover all targets")
            score = {
                "epoch": epoch,
                "samples": samples,
                "optimizer_updates": updates,
                "spectral_updates_per_port": coverage.tolist(),
                "train_mse": mse / samples,
                "train_objective": objective / samples,
                "train_seconds": time.perf_counter() - started,
                "heldout": evaluate(model, heldout, settings, args.stock),
            }
            history.append(score)
            save(model, settings, circuit_id, args.output / "guess.txt")
            (args.output / "history.json").write_text(json.dumps(history, allow_nan=False, indent=2) + "\n")
            print(json.dumps(score, allow_nan=False), flush=True)


if __name__ == "__main__":
    main()
