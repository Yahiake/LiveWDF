"""Estimate a stable parallel-pole model from the reference circuit impulse response."""
import numpy as np
from scipy import linalg, signal


def modes_from_impulse(impulse, orders):
    # A nonuniform Hankel matrix observes fast poles and long low-frequency tails.
    indices = np.unique(np.r_[np.arange(80), np.geomspace(80, len(impulse) // 2 - 4, 150).astype(int)])
    weights = np.sqrt(np.gradient(indices))
    times = indices[:, None] + indices[None, :]
    scale = weights[:, None] * weights[None, :]
    u, singular, vh = linalg.svd(impulse[times + 1] * scale, full_matrices=False)
    shifted = impulse[times + 2] * scale
    max_order = np.count_nonzero(singular > singular[0] * 1e-9)
    samples = np.arange(len(impulse) - 1)
    for order in orders:
        rank = min(order, max_order)
        if rank == 0:
            # A memoryless linear gain needs only the feedthrough coefficient.
            yield float(impulse[0]), [], np.r_[impulse[0], np.zeros_like(samples)]
            return
        root = np.sqrt(singular[:rank])
        transition = (u[:, :rank].T @ shifted @ vh[:rank].T) / root[None, :] / root[:, None]
        poles = linalg.eigvals(transition)
        poles = sorted((p for p in poles if abs(p) < 1 - 1e-12 and p.imag >= -1e-10), key=abs)
        basis = []
        for pole in poles:
            response = pole ** samples
            basis.append(response.real)
            if abs(pole.imag) > 1e-8:
                basis.append(response.imag)
        if not basis:
            continue
        basis = np.array(basis).T
        residues = iter(linalg.lstsq(basis, impulse[1:], cond=1e-12)[0])
        modes = []
        coefficients = []
        for pole in poles:
            a = float(next(residues))
            coefficients.append(a)
            if abs(pole.imag) > 1e-8:
                b = float(next(residues))
                coefficients.append(b)
                modes.append([pole.real, abs(pole.imag), a, b])
            else:
                modes.append([pole.real, 0.0, a, 0.0])
        yield float(impulse[0]), modes, np.r_[impulse[0], basis @ coefficients]


def render(input_volts, direct, modes):
    output = direct * input_volts
    for real, imag, a, b in modes:
        if imag:
            output += signal.lfilter([0, a, b * imag - a * real],
                                     [1, -2 * real, real * real + imag * imag], input_volts)
        else:
            output += signal.lfilter([0, a], [1, -real], input_volts)
    return output
