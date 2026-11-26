"""Select a bank of native Newton matrices and train a small selector."""

import argparse

import numpy as np
from scipy.linalg import lu_factor, lu_solve
import torch


def load_trace(path):
    with open(path, "rb") as file:
        identity, = np.fromfile(file, dtype="<u8", count=1)
        ports, size = map(int, np.fromfile(file, dtype="<u4", count=2))
        transfer = np.fromfile(file, dtype="<f8", count=ports * size).reshape(ports, size)
        trace = np.fromfile(file, dtype="<f8").reshape(-1, 2 * ports + size + size * size)
    trace = trace[np.isfinite(trace).all(axis=1)]
    voltages = trace[:, :ports].astype(np.float32)
    residual = trace[:, ports:2 * ports]
    rhs = trace[:, 2 * ports:2 * ports + size]
    matrices = trace[:, 2 * ports + size:].reshape(-1, size, size)
    return identity, transfer, voltages, residual, rhs, matrices


def defects(trace, bank):
    _, transfer, _, residual, rhs, matrices = trace
    scale = np.maximum(np.max(np.abs(residual), axis=1), 1e-12)
    errors = []
    for matrix in bank:
        correction = lu_solve(lu_factor(matrix), rhs.T, check_finite=False).T
        error = np.einsum("tij,tj->ti", matrices, correction) - rhs
        errors.append(np.max(np.abs(error @ transfer.T), axis=1) / scale)
    return np.column_stack(errors)


def choose_bank(trace, count, pool_size):
    sample = np.random.default_rng(42).choice(len(trace[-1]), size=pool_size, replace=False)
    candidates = trace[-1][sample]
    useful = np.linalg.cond(candidates) < 1e12
    sample, candidates = sample[useful], candidates[useful]
    if len(candidates) < count:
        raise ValueError("Insufficient well-conditioned bank candidates")
    costs = defects(trace, candidates)
    utility = np.maximum(0., .4 - np.minimum(costs, .4))
    selected = []
    covered = np.zeros(len(costs))
    for _ in range(count):
        gain = (np.maximum(covered[:, None], utility) - covered[:, None]).sum(axis=0)
        gain[selected] = -1
        best = int(gain.argmax())
        selected.append(best)
        covered = np.maximum(covered, utility[:, best])
    return sample[selected], costs[:, selected]


def describe(label, cost, pick):
    errors = cost[np.arange(len(cost)), pick]
    print(f"{label:8s} <.4 {(errors < .4).mean():.1%}; "
          f"median {np.median(errors):.4f}; p90 {np.quantile(errors, .9):.4f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--train", nargs="+", required=True)
    parser.add_argument("--holdout", nargs="+", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--bank-size", type=int, default=8)
    parser.add_argument("--pool-size", type=int, default=64)
    args = parser.parse_args()
    torch.manual_seed(42)
    torch.set_num_threads(2)
    trained = [load_trace(path) for path in args.train]
    holdout = [load_trace(path) for path in args.holdout]
    identity, transfer = trained[0][:2]
    for trace in trained + holdout:
        if trace[0] != identity or not np.array_equal(trace[1], transfer):
            raise ValueError("LU traces must use the same circuit and settled transfer")
    trace = (identity, transfer, *(np.concatenate([item[i] for item in trained]) for i in range(2, 6)))
    inputs = trace[2]
    center, std = inputs.mean(axis=0), inputs.std(axis=0)
    inv_scale = np.divide(1., std, out=np.zeros_like(std), where=std > 1e-6)
    inputs = (inputs - center) * inv_scale
    selected, cost = choose_bank(trace, args.bank_size, args.pool_size)
    bank = trace[-1][selected]
    representatives = inputs[selected]
    hidden = 10
    model = torch.nn.Sequential(torch.nn.Linear(inputs.shape[1], hidden),
                                torch.nn.LeakyReLU(.01),
                                torch.nn.Linear(hidden, len(bank)))
    optimizer = torch.optim.Adam(model.parameters(), lr=.01)
    values = torch.from_numpy(inputs)
    loss_per_candidate = torch.from_numpy(np.minimum(cost, .4).astype(np.float32))
    for _ in range(50):
        for i in torch.randperm(len(values)).split(512):
            optimizer.zero_grad()
            probability = model(values[i]).softmax(dim=1)
            loss = (probability * loss_per_candidate[i]).sum(dim=1).mean()
            loss.backward()
            optimizer.step()
    for path, sample in zip(args.holdout, holdout):
        cost = defects(sample, bank)
        values = (sample[2] - center) * inv_scale
        with torch.no_grad():
            neural = model(torch.from_numpy(values)).argmax(dim=1).numpy()
        nearest = ((values[:, None] - representatives[None])**2).sum(axis=2).argmin(axis=1)
        print(path, len(values), "held-out samples")
        describe("constant", cost, np.zeros(len(values), dtype=int))
        describe("nearest", cost, nearest)
        describe("neural", cost, neural)
        describe("oracle", cost, cost.argmin(axis=1))
    with open(args.output, "w") as model_file:
        ports, size = transfer.shape
        print(identity, ports, size, len(bank), hidden, file=model_file)
        for array in (transfer, center, inv_scale, representatives, bank, *model.parameters()):
            if isinstance(array, torch.Tensor):
                array = array.detach().numpy()
            np.savetxt(model_file, array.reshape(1, -1), fmt="%.17g")


if __name__ == "__main__":
    main()
