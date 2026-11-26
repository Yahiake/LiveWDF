"""Fit and export a circuit-specific Newton correction from accepted DI solves."""

import argparse

import numpy as np
import torch
from torch import nn


def load(paths):
    traces = []
    identity = None
    ports = None
    for path in paths:
        with open(path, "rb") as file:
            fingerprint = int(np.fromfile(file, dtype="<u8", count=1)[0])
            count = int(np.fromfile(file, dtype="<u4", count=1)[0])
            if identity is not None and (fingerprint != identity or count != ports):
                raise ValueError("Traces have different circuits, control orders or port counts")
            identity, ports = fingerprint, count
            traces.append(np.fromfile(file, dtype="<f4").reshape(-1, 4 * ports + 2))
    data = np.concatenate(traces)
    if not len(data) or not np.isfinite(data).all():
        raise ValueError("Expected finite training rows")
    width = 3 * ports + 1
    return data[:, :width], data[:, width:-1], data[:, -1], ports, identity


def normalized_data(x, y, cost, validation):
    center = x.mean(axis=0, dtype=np.float64).astype(np.float32)
    scale = np.maximum(x.std(axis=0, dtype=np.float64), 1e-10).astype(np.float32)
    inv_scale = 1 / scale
    # Constant features contain no information, including a fixed knob value.
    inv_scale[x.min(axis=0) == x.max(axis=0)] = 0
    correction_scale = np.maximum(np.sqrt((y.astype(np.float64)**2).mean(axis=0)), 1e-9)
    correction_scale = correction_scale.astype(np.float32)
    weight = np.minimum(4, np.sqrt(cost / max(1, np.median(cost))))
    x_val, y_val, _, _, _ = validation
    return ((x - center) * inv_scale, y / correction_scale, weight,
            (x_val - center) * inv_scale, y_val / correction_scale,
            center, inv_scale, correction_scale)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--train", nargs="+", required=True)
    parser.add_argument("--validation", nargs="+", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--widths", nargs="*", type=int, default=[14])
    parser.add_argument("--epochs", type=int, default=70)
    args = parser.parse_args()
    torch.set_num_threads(2)
    torch.manual_seed(473)
    x, y, cost, ports, identity = load(args.train)
    validation = load(args.validation)
    if validation[-2:] != (ports, identity):
        raise ValueError("Training and validation port order must match")
    x, y, weight, xv, yv, center, inv_scale, correction_scale = normalized_data(x, y, cost, validation)

    if args.widths:
        layers = []
        sizes = [x.shape[1], *args.widths, ports]
        for inp, out in zip(sizes, sizes[1:]):
            if layers:
                layers.append(nn.LeakyReLU(0.01))
            layers.append(nn.Linear(inp, out))
        model = nn.Sequential(*layers)
        optimizer = torch.optim.AdamW(model.parameters(), lr=0.003)
        x, y, weight, xv, yv = map(torch.from_numpy, (x, y, weight, xv, yv))
        best = float("inf")
        for epoch in range(args.epochs):
            for ids in torch.randperm(len(x))[:160_000].split(1024):
                error = model(x[ids]) - y[ids]
                loss = (weight[ids, None] * error.square()).mean()
                optimizer.zero_grad()
                loss.backward()
                optimizer.step()
            with torch.no_grad():
                validation_error = (model(xv) - yv).square().mean().item()
            if validation_error < best:
                best = validation_error
                checkpoint = [param.detach().numpy().copy() for param in model.parameters()]
            if epoch == args.epochs - 1 or epoch % 10 == 0:
                print(f"epoch {epoch + 1:3d} validation {validation_error:.6g}", flush=True)
        stock_error = yv.square().mean().item()
    else:
        features = np.column_stack([x, np.ones(len(x))])
        weighted = np.sqrt(weight[:, None])
        solution = np.linalg.lstsq(features * weighted, y * weighted, rcond=1e-6)[0]
        checkpoint = [solution[:-1].T, solution[-1]]
        best = np.mean((xv @ solution[:-1] + solution[-1] - yv)**2)
        stock_error = np.mean(yv**2)

    print(f"validation normalized MSE stock={stock_error:.6g} learned={best:.6g}")
    with open(args.output, "w", encoding="utf-8") as stream:
        stream.write(f"{identity} {len(checkpoint) // 2} {len(center)}\n")
        for vector in (center, inv_scale, correction_scale):
            np.savetxt(stream, vector[None], fmt="%.9g")
        for matrix, bias in zip(checkpoint[::2], checkpoint[1::2]):
            stream.write(f"{len(bias)}\n")
            np.savetxt(stream, matrix, fmt="%.9g")
            np.savetxt(stream, bias[None], fmt="%.9g")


if __name__ == "__main__":
    main()
