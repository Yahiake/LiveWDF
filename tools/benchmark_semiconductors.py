#!/usr/bin/env python3
"""Run the semiconductor fixtures, compare ngspice, and plot every card."""
from argparse import ArgumentParser
from csv import DictReader, DictWriter
from pathlib import Path
import re
import shutil
import subprocess

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np


MISTER_REVISION = '692e777'
MISTER_MODELS = {
    '2n3904-mister': ('discretes', 'q2n3904'),
    '2n3906-mister': ('discretes', 'q2n3906'),
    '2n3904-mister-ikf': ('discretes', 'q2n3904'),
    '2n3906-mister-ikf': ('discretes', 'q2n3906'),
    '1n914-nexperia': ('1n914', '1n914'),
    '1s1588-proxy': ('1s1588', '1s1588'),
    'ma150-proxy': ('ma150', 'ma150'),
    '1s2075': ('1s2075', 'd1s2075'),
    '1n34a-mister': ('d1n34a', 'd1n34a'),
    '1n4002': ('1n4002', '1n4002'),
}
LOW_LEAKAGE = ('1n914-nexperia', '1s2075', '1n4002')

SOURCE_REFERENCES = {
    '2n3904-mister': ('onsemi 2N3904, p. 2', 'https://www.onsemi.com/pdf/datasheet/2n3904-d.pdf'),
    '2n3906-mister': ('onsemi 2N3906, p. 2', 'https://www.onsemi.com/pdf/datasheet/2n3906-d.pdf'),
    '2sc2240': ('Toshiba 2SC2240, p. 2', 'https://www.taydaelectronics.com/datasheets/A-904.pdf'),
    'bc109c': ('Philips BC107 to BC109C, electrical table', 'https://www.josvandijken.nl/downloads/BC109.PDF'),
    '2sk184-gr': ('Toshiba 2SK184, electrical table', 'https://aitendo3.sakura.ne.jp/aitendo_data/product_img/transisitor/2SK184/2SK184_toshiba.pdf'),
    '2n5458': ('onsemi 2N5457/2N5458, electrical table', 'https://www.pilianidis.gr/images/sync_products/01-2N5458/2N5458.pdf'),
    '1n914-nexperia': ('onsemi 1N914/1N4148, electrical table', 'https://www.onsemi.com/pdf/datasheet/1n914-d.pdf'),
    '1s2075': ('Renesas 1S2075(K), electrical table', 'https://www.renesas.com/en/document/dst/1s2075k-datasheet'),
    '1n34a-mister': ('Central Semiconductor 1N34A, electrical table', 'https://my.centralsemi.com/datasheets/1N34A.PDF'),
    '1n4002': ('Vishay 1N4001–1N4007, p. 2', 'https://www.vishay.com/docs/88503/1n4001.pdf'),
    '1ss133': ('ROHM 1SS133', 'https://www.rohm.com/products/diodes/switching-diodes/standard/1ss133-product'),
}

# (target, lower limit, upper limit); a missing end is unspecified.
BJT_GAIN = {
    '2n3904-mister': (.010, 100, 300),
    '2n3906-mister': (.010, 100, 300),
    '2sc2240': (.002, 200, 400),  # GR bin
    'bc109c': (.002, 420, 800),  # C bin
}
JFET_IDSS = {'2sk184-gr': (.0026, .0065), '2n5458': (.002, .009)}
DIODE_LIMITS = {
    '1n914-nexperia': (.010, 1.0, 20.0, 25e-9),
    '1s2075': (.010, .8, 30.0, 100e-9),
    '1n34a-mister': (.005, 1.0, 10.0, 30e-6),
    '1n4002': (1.0, 1.1, 100.0, 5e-6),
    '1ss133': (.100, 1.2, 80.0, .5e-6),
}

plt.rcParams.update({'font.size': 10, 'axes.spines.top': False, 'axes.spines.right': False})
SERIES = [('LiveWDF', '#175c85', '-'), ('ngspice export', '#cf6022', '--'),
          ('MiSTer full card', '#765da7', ':')]


def get_mister_model(repo, part):
    stem, model = MISTER_MODELS[part]
    cid = f'{MISTER_REVISION}:circuits/parts/{stem}.lib'
    lines = subprocess.check_output(['git', '-C', repo, 'show', cid], text=True).splitlines()
    pattern = re.compile(rf'^\.model\s+{re.escape(model)}\s+', re.IGNORECASE)
    begin = next(i for i, line in enumerate(lines) if pattern.search(line))
    end = begin + 1
    while end < len(lines) and lines[end].lstrip().startswith('+'):
        end += 1
    return model, '\n'.join(lines[begin:end])


def run_ngspice(executable, path, stem):
    deck, log = path / f'{stem}.cir', path / f'{stem}.log'
    subprocess.run([executable, '-b', '-o', log.name, deck.name], cwd=path,
                   capture_output=True, text=True, check=True)


def source_deck(exported, sweep, model, card, kind, vendor_shunt=False):
    body, control = exported.split('\n.control\n', 1)
    # Keep the exact fixture and .options, replacing only the exported DUT.
    parts = body.splitlines()
    options = next(line for line in parts if line.startswith('.options '))
    if kind == 'bjt':
        fixture = [line for line in parts if line.lower().startswith(('v_input ', 'v_fixture ', 'r_'))]
        element = f'Q_original sense bias 0 {model}'
    else:
        fixture = [line for line in parts if line.lower().startswith(('v_input ', 'r_meter '))]
        element = f'D_original sense 0 {model}'
        if vendor_shunt:
            fixture.append('R_vendor sense 0 5.827e9')
    control = control.replace(f'{sweep}-ngspice.txt', f'{sweep}-original.txt')
    return '\n'.join(['Original MiSTer model, measured with the LiveWDF fixture',
                       *fixture, element, card, options, '.control', control])


def ngspice_traces(path, data, suffix):
    raw = np.loadtxt(path / f'{data["sweep"]}-{suffix}.txt')
    drive, node, bias = raw.T
    meter, supply = float(data['meter_ohms']), float(data['supply_V'])
    if data['kind'] == 'diode':
        current = (drive - node) / meter
        i_bias = np.zeros_like(drive)
    else:
        sign = -1 if supply < 0 else 1
        current = sign * (supply - node) / meter
        i_bias = sign * (drive - bias)
    return np.column_stack((drive, node, bias, current, i_bias))


def load_sweep(path, row, spice, mister_repo):
    identifier = row['sweep']
    native = np.loadtxt(path / f'{identifier}-native.csv', delimiter=',', skiprows=1)
    run_ngspice(spice, path, f'{identifier}-export')
    export = ngspice_traces(path, row, 'ngspice')
    curves = [native, export]
    donor = row['id'].removesuffix('-lowleakage')
    if mister_repo and donor in MISTER_MODELS:
        model, card = get_mister_model(mister_repo, donor)
        filename = path / f'{identifier}-original.cir'
        template = (path / f'{identifier}-export.cir').read_text()
        vendor_shunt = row['id'] == '1n914-nexperia-lowleakage'
        filename.write_text(source_deck(template, identifier, model, card, row['kind'], vendor_shunt))
        run_ngspice(spice, path, f'{identifier}-original')
        curves.append(ngspice_traces(path, row, 'original'))
    for curve in curves[1:]:
        if curve.shape != native.shape or not np.allclose(curve[:, 0], native[:, 0], atol=1e-8, rtol=0):
            raise RuntimeError(f'ngspice drive grid differs for {identifier}')
    return curves


def residual_metrics(row, curves):
    for curve, ref in zip(curves[1:], ('export', 'mister')):
        difference = curves[0][:, 3] - curve[:, 3]
        valid = np.abs(curve[:, 3]) >= 1e-6
        relative = 100 * np.abs(difference[valid] / curve[valid, 3])
        regime = (curves[0][:, 3] >= .0001) & (curves[0][:, 3] <= .001)
        yield dict(sweep=row['sweep'], reference=ref, points=len(difference),
                   current_rmse_A=np.sqrt(np.mean(difference**2)),
                   current_max_abs_A=np.max(np.abs(difference)),
                   current_max_relative_percent=np.max(relative, initial=0),
                   relative_points=int(np.count_nonzero(valid)),
                   audio_bias_01_to_1_mA_rmse_A=(np.sqrt(np.mean(difference[regime]**2))
                                                 if np.any(regime) else ''),
                   sweep_rejected=row['rejected'], sweep_unconverged=row['unconverged'])


def interpolate(x, y, value):
    order = np.argsort(x)
    x, y = x[order], y[order]
    if not min(x) - 2e-5 <= value <= max(x) + 2e-5:
        return np.nan
    return np.interp(value, x, y)


def physical_checks(item, forward, reverse):
    base = item.removesuffix('-ikf').removesuffix('-lowleakage')
    card = forward[0]
    if base in BJT_GAIN:
        test_i, low, high = BJT_GAIN[base]
        gain = interpolate(card[:, 3], card[:, 3] / np.maximum(card[:, 4], 1e-15), test_i)
        vce = abs(interpolate(card[:, 3], card[:, 1], test_i))
        yield f'hFE {gain:.3g} at {test_i*1000:g} mA, |VCE| {vce:.4g} V (range {low}–{high})', low <= gain <= high
    if base in JFET_IDSS:
        low, high = JFET_IDSS[base]
        idss = interpolate(card[:, 2], card[:, 3], 0)
        yield f'IDSS {idss*1000:.3g} mA at VGS 0 V (range {low*1000:g}–{high*1000:g} mA)', low <= idss <= high
        test_i = 1e-7 if base == '2sk184-gr' else 1e-8
        above = np.flatnonzero(card[:, 3] >= test_i)
        crossing = above[0] if len(above) else 0
        vgs = abs(interpolate(card[crossing-1:crossing+1, 3],
                              card[crossing-1:crossing+1, 2], test_i)) if crossing else np.nan
        low, high = (.2, 1.5) if base == '2sk184-gr' else (1.0, 7.0)
        yield (f'|VGS(off)| {vgs:.3g} V at {test_i*1e9:g} nA (range {low:g}–{high:g} V)',
               low <= vgs <= high)
    if reverse and base in DIODE_LIMITS:
        test_i, vf_limit, test_v, ir_limit = DIODE_LIMITS[base]
        vf = interpolate(card[:, 3], card[:, 1], test_i)
        rev = reverse[0]
        ir = abs(interpolate(rev[:, 1], rev[:, 3], -test_v))
        yield f'VF {vf:.3g} V at {test_i*1000:g} mA (max {vf_limit:g} V)', vf <= vf_limit
        yield (f'IR {ir*1e6:.3g} µA at {test_v:g} V reverse (max {ir_limit*1e6:g} µA, {ir/ir_limit:.3g}×)',
               ir <= ir_limit)


def plot_card(path, part, kind, title, data):
    _, curves = data[part]
    reverse = data[part + '-reverse'][1] if kind == 'diode' else None
    series = SERIES
    if part == '1n914-nexperia-lowleakage':
        series = [*SERIES[:2], ('Nexperia junction and shunt', *SERIES[2][1:])]
    fig, axes = plt.subplots(3, 1, figsize=(8.0, 9.2), layout='constrained')
    fig.suptitle(title, fontsize=17, weight='bold', x=.025, ha='left')
    for curve, (name, color, style) in zip(curves, series):
        terminal, bias, current, bias_i = curve[:, 1:].T
        control = terminal if kind == 'diode' else bias
        axes[0].semilogy(np.abs(control) if kind == 'bjt' else control,
                         np.maximum(np.abs(current), 1e-12)*1000, c=color, ls=style,
                         label=name, lw=1.8)
        if kind == 'bjt':
            visible = (bias_i > 1e-10) & (current > 1e-6)
            axes[1].semilogx(current[visible]*1000, current[visible]/bias_i[visible],
                             c=color, ls=style, lw=1.8)
        elif kind == 'jfet':
            axes[1].plot(bias, 1000*current, c=color, ls=style, lw=1.8)
    if reverse:
        for curve, (name, color, style) in zip(reverse, series):
            axes[1].plot(-curve[:, 1], -curve[:, 3]*1e6, c=color, ls=style, lw=1.8, label=name)
        limits = DIODE_LIMITS.get(part.removesuffix('-lowleakage'))
        if limits:
            test_i, vf, test_v, ir = limits
            axes[0].plot([vf], [1000*test_i], marker='v', ms=7, color='#243036', label='Manufacturer maximum')
            axes[1].plot([test_v], [ir*1e6], marker='^', ms=7, color='#243036', label='Manufacturer maximum')
        axes[1].set(xlabel='Reverse voltage (V)', ylabel='Reverse current magnitude (µA)')
    elif kind == 'bjt':
        axes[1].set(xlabel='Collector current (mA)', ylabel='DC current gain hFE')
        limits = BJT_GAIN.get(part.removesuffix('-ikf'))
        if limits:
            test_i, low, high = limits
            center = (low + high) / 2
            axes[1].errorbar(1000*test_i, center, yerr=(high-low)/2, fmt='o',
                             color='#243036', capsize=4, label='Manufacturer range')
    else:
        axes[1].set(xlabel='Gate–source voltage (V)', ylabel='Drain current (mA)')
        if part in JFET_IDSS:
            low, high = [1000*x for x in JFET_IDSS[part]]
            axes[1].errorbar(0, (low+high)/2, yerr=(high-low)/2, fmt='o',
                             color='#243036', capsize=4, label='Manufacturer IDSS range')
    axes[0].set(xlabel={'bjt': '|Base–emitter voltage| (V)', 'jfet': 'Gate–source voltage (V)',
                        'diode': 'Forward terminal voltage (V)'}[kind], ylabel='Current magnitude (mA)')
    for curve, (label, color, style) in zip(curves[1:], series[1:]):
        delta = np.abs(curves[0][:, 3] - curve[:, 3])
        axes[2].semilogy(curves[0][:, 0], np.maximum(delta, 1e-15)*1e6,
                         c=color, ls=style, lw=1.8, label=f'vs. {label}')
    axes[2].set(xlabel='Swept source voltage (V)', ylabel='Absolute current residual (µA)')
    for axis in axes:
        axis.grid(alpha=.22)
        if axis.get_legend_handles_labels()[0]:
            axis.legend(frameon=False, loc='best')
    fig.savefig(path / f'{part}.png', dpi=160)
    plt.close(fig)


def plot_leakage_comparison(path, cases):
    if not all(f'{ident}-lowleakage' in cases and ident in cases for ident in LOW_LEAKAGE):
        return
    fig, axes = plt.subplots(2, 3, figsize=(12, 6), layout='constrained')
    fig.suptitle('Diode shunt comparison', fontsize=17, weight='bold')
    for (reverse, forward), ident in zip(axes.T, LOW_LEAKAGE):
        corrected = f'{ident}-lowleakage'
        reverse.set_title(cases[ident][0]['card'], weight='bold')
        for key, label, color in ((ident, 'Original LiveWDF card', '#175c85'),
                                  (corrected, 'LowLeakage card', '#cf6022')):
            curve = cases[key+'-reverse'][1][0]
            reverse.semilogy(-curve[1:, 1], np.abs(curve[1:, 3])*1e6,
                             color=color, label=label, lw=1.8)
        _, _, test_v, limit = DIODE_LIMITS[ident]
        reverse.plot([test_v], [limit*1e6], '^', color='#243036', label='Manufacturer maximum')
        reverse.set(xlabel='Reverse voltage (V)', ylabel='Reverse current (µA)')
        before, after = cases[ident][1][0], cases[corrected][1][0]
        aligned = np.interp(before[:, 1], after[:, 1], after[:, 3])
        forward.plot(before[:, 1], (before[:, 3] - aligned)*1e6, color='#175c85', lw=1.8)
        forward.set(xlabel='Forward voltage (V)', ylabel='Current reduction (µA)')
        reverse.grid(alpha=.24)
        forward.grid(alpha=.24)
    axes[0, 0].legend(frameon=False)
    fig.savefig(path / 'leakage-before-after.png', dpi=160)
    plt.close(fig)


def report(path, cases, metrics):
    with (path / 'residuals.csv').open('w') as file:
        writer = DictWriter(file, fieldnames=list(metrics[0]))
        writer.writeheader()
        writer.writerows(metrics)
    with (path / 'timing.csv').open() as file:
        timing = list(DictReader(file))
    with (path / 'summary.md').open('w') as file:
        file.write('# Semiconductor validation\n\n')
        file.write('The timed circuits drive one device per test at 48 kHz with a 440 Hz sine. '
                   'Median and range cover seven passes of 48,000 samples. '
                   'Warm-up and diagnostic collection take place outside the timed region. '
                   'The DC comparison uses a separate low-resistance sense fixture and excludes '
                   'JFET displacement currents. The timing fixture uses the complete JFET card.\n\n')
        file.write('| Device | Median ns/frame | Range ns/frame | Iterations/frame | Fallbacks | Rejects |\n'
                   '| --- | ---: | ---: | ---: | ---: | ---: |\n')
        for row in timing:
            ident = row['id']
            file.write(f'| [{cases[ident][0]["card"]}]({ident}.png) | {float(row["median_ns_frame"]):.1f} | '
                       f'{float(row["min_ns_frame"]):.1f}–{float(row["max_ns_frame"]):.1f} | '
                       f'{float(row["mean_iterations"]):.3f} | {row["fallbacks"]} | {row["rejected"]} |\n')
        file.write('\n## Independent datasheet checks\n\n')
        file.write('The datasheet values apply at the published room-temperature test conditions. The '
                   'shipped cards use fixed room-temperature thermal voltages. Sense resistors drop '
                   'at most 1 mV at the BJT gain comparison points.\n\n'
                   '| Device | Native result and manufacturer limit | Result |\n| --- | --- | --- |\n')
        for row in timing:
            ident = row['id']
            _, primary = cases[ident]
            reverse = cases.get(ident+'-reverse')
            ref_case = ident.removesuffix('-ikf').removesuffix('-lowleakage')
            for summary, meets in physical_checks(ident, primary, reverse[1] if reverse else None):
                reference = SOURCE_REFERENCES[ref_case]
                title = cases[ident][0]['card']
                result = 'Within limit' if meets else 'Outside limit'
                summary = summary.replace('|', r'\|')
                file.write(f'| {title} | {summary}. [{reference[0]}]({reference[1]}) | {result} |\n')
        file.write('\n[DC residuals](residuals.csv) · [timing and diagnostic counts](timing.csv)\n')
        if (path / 'clipping-timing.csv').exists():
            file.write('\nThe clipping fixture uses two opposing diodes, a 1 kΩ source resistance '
                       'and a 100 kΩ load. It uses the same timing protocol and a ±1.8 V sine. '
                       '[Clipping timing](clipping-timing.csv)\n')
        if (path / 'leakage-before-after.png').exists():
            file.write('\n[Forward and reverse shunt comparison](leakage-before-after.png)\n')


def main():
    args = ArgumentParser(description=__doc__)
    args.add_argument('benchmark', type=Path)
    args.add_argument('output', type=Path)
    args.add_argument('--ngspice', default='ngspice')
    args.add_argument('--mister-source', type=Path,
                      help='Local MiSTer-Discrete checkout containing the source cards')
    args.add_argument('--card', help='Run one card ID')
    args.add_argument('--reuse-benchmark', action='store_true', help='Reuse existing native sweeps and timing in output')
    args.add_argument('--cpu', type=int, help='Pin the benchmark to one CPU with taskset')
    config = args.parse_args()
    spice = shutil.which(config.ngspice)
    if spice is None:
        args.error('ngspice was not found')
    spice = str(Path(spice).resolve())
    output = config.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if not config.reuse_benchmark:
        command = [str(config.benchmark.resolve())]
        if config.card:
            command += [config.card]
        if config.cpu is not None:
            command = ['taskset', '-c', str(config.cpu), *command]
        subprocess.run(command, cwd=output, check=True)
    with (output / 'sweeps.csv').open() as file:
        sweeps = list(DictReader(file))
    cases, metrics = {}, []
    for row in sweeps:
        curves = load_sweep(output, row, spice, config.mister_source)
        cases[row['sweep']] = (row, curves)
        metrics += list(residual_metrics(row, curves))
    for ident, (row, _) in cases.items():
        if ident == row['id']:
            plot_card(output, ident, row['kind'], row['card'], cases)
    plot_leakage_comparison(output, cases)
    report(output, cases, metrics)


if __name__ == '__main__':
    main()
