"""Fit a continuous, fixed-permutation LU predictor on first-Newton systems."""

import argparse
from pathlib import Path

import numpy as np
import torch
from scipy.linalg import lu_factor

from train_lu_bank import load_trace


def equilibration(matrices, scaling):
    magnitude = (np.abs(matrices).max(axis=2) if scaling == "row" else
                 np.abs(np.diagonal(matrices, axis1=1, axis2=2)))
    return 1. / np.maximum(1., magnitude)


def unpack_factor(matrices, inverse_scale, permutation):
    factors = (matrices * inverse_scale[:, :, None])[:, permutation, :].copy()
    for row in range(factors.shape[1] - 1):
        pivot = factors[:, row, row]
        safe = np.where(np.abs(pivot) >= 1.e-12, pivot, 1.e-12)
        factors[:, row + 1:, row] /= safe[:, None]
        factors[:, row + 1:, row + 1:] -= (
            factors[:, row + 1:, row, None] * factors[:, row, None, row + 1:]
        )
    return factors


def pivot_order(pivots):
    order = np.arange(len(pivots))
    for index, other in enumerate(pivots):
        order[index], order[other] = order[other], order[index]
    return order


def choose_order(matrices, scaling):
    sample = matrices[::max(1, len(matrices) // 1200)]
    inv = equilibration(sample, scaling)
    choices = {tuple(range(matrices.shape[1]))}
    for i in np.linspace(0, len(sample) - 1, 16).astype(int):
        _, pivots = lu_factor(sample[i] * inv[i, :, None])
        choices.add(tuple(pivot_order(pivots)))
    best = (float("inf"), ())
    for order in sorted(choices):
        factors = unpack_factor(sample, inv, np.asarray(order))
        pivots = np.abs(np.diagonal(factors, axis1=1, axis2=2))
        instability = np.linalg.norm(factors, axis=(1, 2)) + 1. / np.maximum(pivots.min(axis=1), 1.e-9)
        score = float(np.quantile(instability, .95))
        if score < best[0]:
            best = score, order
    print("fixed pivot diagnostic", best[0], "permutation", best[1], flush=True)
    return np.asarray(best[1])


def factor_basis(factors, rank):
    n = factors.shape[1]
    packed = factors.reshape(-1, n * n)
    mean = packed.mean(axis=0)
    element_scale = np.maximum(packed.std(axis=0), 1.e-7)
    centered = (packed - mean) / element_scale
    rng = np.random.default_rng(18)
    sketch = centered @ rng.standard_normal((n * n, rank + 12))
    basis, _ = np.linalg.qr(sketch, mode="reduced")
    _, _, directions = np.linalg.svd(basis.T @ centered, full_matrices=False)
    directions = directions[:rank]
    scores = centered @ directions.T
    scales = np.maximum(scores.std(axis=0), 1.e-8)
    return mean, (directions * element_scale).T * scales, scores / scales


def model_inputs(trace, scaling):
    return np.column_stack((trace[2], equilibration(trace[5], scaling).astype(np.float32)))


def normalized_rhs(trace):
    value = np.maximum(np.max(np.abs(trace[3]), axis=1), 1.e-12)
    return trace[4] / value[:, None]


def factors_from_scores(scores, mean, basis):
    size = int(np.sqrt(mean.numel()))
    flat = mean + scores.to(torch.float64) @ basis.T
    return flat.reshape(-1, size, size)


def voltage_defect(factors, inverse_scale, permutation, actual, rhs, transfer):
    # A_scaled = L U, so find the correction for the scaled, permuted RHS.
    scaled = (rhs * inverse_scale)[:, permutation, None]
    lower = torch.linalg.solve_triangular(factors, scaled, upper=False, unitriangular=True)
    correction = torch.linalg.solve_triangular(factors, lower, upper=True)
    error = (actual @ correction).squeeze(2) - rhs
    return (error @ transfer.T).abs().amax(dim=1)


def summarize(label, model, samples, mean, basis, permutation, transfer):
    collect = []
    with torch.no_grad():
        for features, scaling, observed, rhs in zip(
            *(tensor.split(256) for tensor in samples), strict=True
        ):
            factors = factors_from_scores(model(features), mean, basis)
            collect.append(voltage_defect(factors, scaling, permutation, observed, rhs, transfer))
    d = torch.cat(collect).numpy()
    finite = np.isfinite(d).mean()
    d = np.where(np.isfinite(d), d, np.inf)
    print(f"{label}: <0.4 {(d < .4).mean():.1%}; <1 {(d < 1).mean():.1%}; "
          f"median {np.median(d):.4g}; p90 {np.quantile(d, .9):.4g}; finite {finite:.1%}", flush=True)
    return (d < .4).mean()


def samples(trace, center, normalizer, scaling):
    return (
        torch.from_numpy((model_inputs(trace, scaling) - center) * normalizer),
        torch.from_numpy(equilibration(trace[5], scaling).copy()),
        torch.from_numpy(trace[5].copy()),
        torch.from_numpy(normalized_rhs(trace).copy()),
    )


def save(path, fingerprint, transfer, permutation, center, normalizer, mean, basis, model, scaling):
    pivots = []
    ordered = list(range(len(permutation)))
    for i, row in enumerate(permutation.tolist()):
        p = ordered.index(row, i)
        pivots.append(p)
        ordered[i], ordered[p] = ordered[p], ordered[i]
    ports, size = transfer.shape
    with open(path, "w") as file:
        print(fingerprint, ports, size, model[0].out_features, basis.shape[1], scaling, file=file)
        for values in (transfer, pivots, center, normalizer, mean, basis, *model.parameters()):
            if isinstance(values, torch.Tensor):
                values = values.detach().numpy()
            np.savetxt(file, np.asarray(values).reshape(1, -1), fmt="%.17g")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--train", nargs="+", required=True)
    parser.add_argument("--holdout", nargs="+", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--scaling", choices=("diagonal", "row"), default="diagonal")
    parser.add_argument("--rank", type=int, default=8)
    parser.add_argument("--hidden", type=int, default=20)
    parser.add_argument("--epochs", type=int, default=70)
    parser.add_argument("--correction-epochs", type=int, default=12)
    args = parser.parse_args()
    torch.manual_seed(53)
    torch.set_num_threads(2)
    train = [load_trace(path) for path in args.train]
    heldout = [load_trace(path) for path in args.holdout]
    fingerprint, z = train[0][:2]
    for item in train + heldout:
        if item[0] != fingerprint or not np.array_equal(item[1], z):
            raise ValueError("All traces must use identical settled circuit transfer")
    train = (fingerprint, z, *(np.concatenate([t[i] for t in train]) for i in range(2, 6)))
    inputs = model_inputs(train, args.scaling)
    center, sd = inputs.mean(axis=0), inputs.std(axis=0)
    normalizer = np.divide(1., sd, out=np.zeros_like(sd), where=sd > 1.e-7)
    permutation = choose_order(train[5], args.scaling)
    exact = unpack_factor(train[5], equilibration(train[5], args.scaling), permutation)
    pivots = np.abs(np.diagonal(exact, axis1=1, axis2=2)).min(axis=1)
    use = (pivots > 1.e-5) & (np.abs(exact).max(axis=(1, 2)) < 1.e4)
    if use.mean() < .5:
        raise ValueError("Fixed pivot order too unstable")
    print(f"stable fixed-pivot factor training rows {use.sum()}/{len(use)}", flush=True)
    mean, basis, scores = factor_basis(exact[use], args.rank)
    mean, basis = map(torch.from_numpy, (mean, basis))
    x = torch.from_numpy((inputs[use] - center) * normalizer)
    y = torch.from_numpy(scores.astype(np.float32))
    model = torch.nn.Sequential(
        torch.nn.Linear(inputs.shape[1], args.hidden),
        torch.nn.LeakyReLU(.01),
        torch.nn.Linear(args.hidden, args.rank),
    )
    opt = torch.optim.AdamW(model.parameters(), lr=.004)
    for _ in range(args.epochs):
        for idx in torch.randperm(len(x)).split(512):
            opt.zero_grad()
            loss = torch.nn.functional.smooth_l1_loss(model(x[idx]), y[idx])
            loss.backward()
            opt.step()

    transfer = torch.from_numpy(z.copy())
    holdout = [samples(item, center, normalizer, args.scaling) for item in heldout]
    for path, data in zip(args.holdout, holdout):
        summarize("factor fit " + Path(path).stem, model, data, mean, basis, permutation, transfer)
    save(args.output, fingerprint, z, permutation, center, normalizer, mean, basis, model, args.scaling)

    train = (fingerprint, z, *(a[use] for a in train[2:]))
    x, scale, actual, rhs = samples(train, center, normalizer, args.scaling)
    opt.param_groups[0]["lr"] = .0008
    for epoch in range(args.correction_epochs):
        for idx in torch.randperm(len(x))[:8192].split(256):
            opt.zero_grad()
            prediction = model(x[idx])
            factors = factors_from_scores(prediction, mean, basis)
            defect = voltage_defect(factors, scale[idx], permutation, actual[idx], rhs[idx], transfer)
            loss = torch.log1p(defect).mean() + .02 * (prediction - y[idx]).square().mean()
            if torch.isfinite(loss):
                loss.backward()
                torch.nn.utils.clip_grad_norm_(model.parameters(), 10., error_if_nonfinite=False)
                if all(torch.isfinite(p.grad).all() for p in model.parameters()):
                    opt.step()
        if epoch % 5 == 4:
            print("correction objective epoch", epoch + 1, flush=True)
    for path, data in zip(args.holdout, holdout):
        summarize("defect fit " + Path(path).stem, model, data, mean, basis, permutation, transfer)
    save(args.output + ".correction", fingerprint, z, permutation, center, normalizer, mean, basis, model, args.scaling)


if __name__ == "__main__":
    main()
