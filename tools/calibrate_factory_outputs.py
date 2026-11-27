#!/usr/bin/env python3
"""Measure factory output matching through LiveWDF_PluginBench at 48 kHz / 1x Accurate.

Run from any directory with Python, NumPy, SciPy and ffmpeg installed. In fit mode
the runner changes only a temporary copy's output divisor. In verify mode it reads
the checked-in preset verbatim. Both modes measure the same unchanged wet circuit.
"""
import argparse
import csv
import json
import math
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
from scipy.signal import sosfilt

ROOT = Path(__file__).resolve().parents[1]
FS = 48000
FRAME = 2400                 # 50 ms active-frame gate
PROGRAM = 16 * FRAME
REST = 8 * FRAME
LEVELS = (-12, -9, -6)
# Two excerpts at each fitted level; the third musical excerpt tests transfer.
TRIALS = [(take, level) for take in (0, 1) for level in LEVELS] + [(2, -9)]
# ITU-R BS.1770 K weighting at 48 kHz. Gain differences cancel the channel constant.
K_FILTER = [[1.53512485958697, -2.69169618940638, 1.19839281085285,
             1., -1.69065929318241, .73248077421585],
            [1., -2., 1., 1., -1.99004745483398, .99007225036621]]
SOURCES = {
    'palm': ('pf300-palm-mutes', (0., 3.6, 7.6)),
    'chord': ('pf300-major-chords', (0., 4., 8.)),
    'bend': ('pf300-high-bends', (1., 7.2, 12.5)),
    'picked': ('sire-t7-performance-11', (0., 2.4, 5.6)),
}


def db(amplitude):
    return 20 * math.log10(max(float(amplitude), 1e-14))


def family(path, doc):
    name = path.stem.lower()
    if path.parent.name == 'preamps' and 'line preamp' not in name:
        return 'voice', False
    gate = any(token in name for token in ('ns-2', 'decimator', 'ds201'))
    if 'studio' in path.parts:
        return ('voice' if 'de-esser' in name else 'bus'), gate
    tag = doc.get('category', '').lower()
    if any(x in tag + name for x in ('fuzz', 'octavia', 'bender', 'muff')):
        return 'bend', gate
    if gate or path.parent.name == 'amps' or 'distortion' in tag or 'overdrive' in tag:
        return 'palm', gate
    if any(x in tag + name for x in ('dynamics', 'comp', 'envelope', 'mu-tron')):
        return 'picked', gate
    return 'chord', gate


def program(profile, take):
    if profile in SOURCES:
        stem, starts = SOURCES[profile]
        source = ROOT / 'tests/fixtures/guitar-techs-di' / (stem + '.flac')
        audio = subprocess.check_output([
            'ffmpeg', '-v', 'error', '-i', str(source), '-ac', '1', '-ar', str(FS),
            '-f', 'f32le', 'pipe:1',
        ])
        x = np.frombuffer(audio, dtype='<f4')
        i = int(starts[take] * FS)
        x = np.array(x[i:i + PROGRAM], dtype=float)
    else:
        t = np.arange(PROGRAM) / FS
        noise = np.random.default_rng(4060 + take).normal(size=len(t))
        bright = np.diff(noise, prepend=0)
        if profile == 'voice':
            # A repeatable voiced vowel / sibilant sequence at the microphone port.
            f = (113, 150, 131)[take] + 8 * np.sin(2 * np.pi * 3 * t)
            phase = 2 * np.pi * np.cumsum(f) / FS
            x = np.zeros_like(t)
            for h in range(1, 38):
                gain = .07 + sum(np.exp(-((f * h - fc) / width) ** 2)
                                 for fc, width in ((650, 120), (1450, 210), (2900, 310)))
                x += gain * np.sin(h * phase) / h
            local = t % .255
            x = x * np.exp(-6 * local) + .022 * bright * (local > .17)
        else:
            # Broadband bus with distinct kick, bass, hats and snare transients.
            local = t % (.160, .130, .147)[take]
            kick = np.sin(2 * np.pi * (52 * local + 1.4 * (1 - np.exp(-38 * local))))
            bass = (98, 111, 82)[take]
            x = (.64 * kick * np.exp(-29 * local) + .20 * np.sin(2 * np.pi * bass * t)
                 + .07 * np.sin(2 * np.pi * 3 * bass * t)
                 + .08 * bright * np.exp(-65 * local)
                 + .08 * noise * np.exp(-42 * (t % .32)) + .025 * np.sin(2 * np.pi * 3341 * t))
    if len(x) != PROGRAM or not np.isfinite(x).all() or not np.any(x):
        raise ValueError(f'{profile} excerpt {take + 1} is missing, silent or invalid')
    x[:96] *= np.linspace(0., 1., 96)
    x[-96:] *= np.linspace(1., 0., 96)
    return x / np.max(np.abs(x))


def fixture(profile):
    profiles = ('palm', 'chord', 'picked') if profile == 'amp' else (profile,) * 3
    excerpts = [program(name, i) for i, name in enumerate(profiles)]
    parts = [np.zeros(REST)]
    for take, level in TRIALS:
        parts.extend([excerpts[take] * 10**(level / 20), np.zeros(REST)])
    return np.concatenate(parts).astype('=f4')


def measure(x, y, gate, volts, vin, verify):
    if len(y) != len(x) or not np.all(np.isfinite(y)):
        raise ValueError('invalid render length or non-finite output')
    # Rest before each excerpt lets us include delay tails in the headroom check.
    xw, yw = sosfilt(K_FILTER, x), sosfilt(K_FILTER, y)
    deltas = []
    for i in range(len(TRIALS)):
        start = REST + i * (PROGRAM + REST)
        sel = slice(start, start + PROGRAM)
        xx = np.mean(xw[sel].reshape(-1, FRAME)**2, axis=1)
        yy = np.mean(yw[sel].reshape(-1, FRAME)**2, axis=1)
        # For a gate, measure on strong input frames only. Silence and intended
        # closure must not increase the recommended makeup gain.
        active = xx >= xx.max() * 10**((-9 if gate else -30) / 10)
        deltas.append(db(math.sqrt(np.sum(yy[active]) / np.sum(xx[active]))))
    heldout_start = REST + (len(TRIALS) - 1) * (PROGRAM + REST)
    fit_peak = float(np.max(np.abs(y[:heldout_start])))
    peak = float(np.max(np.abs(y)))
    start_peak = float(np.max(np.abs(y[:REST])))
    active_rms = np.sqrt(np.mean(yw**2))
    settled_dc = abs(np.mean(y[REST//2:REST], dtype=float))
    levels = np.mean(np.reshape(deltas[:-1], (2, 3)), axis=0)
    median = float(np.median(levels))
    headroom = db(fit_peak) + 1.0  # at least 1 dB below full scale including startup/tails
    correction = max(median, headroom)
    status = 'matched'
    if db(active_rms * volts) < -110 or median < -90:
        status = 'review-silent'
    elif settled_dc > max(1.e-7 / volts, active_rms * .30):
        status = 'review-dc'
    elif start_peak > max(1.e-7 / volts, active_rms * 12) and db(start_peak) + 1 > median + 6:
        status = 'review-startup'
    elif gate and median + db(volts / vin) < -18:
        status = 'review-gate'
    elif verify and peak >= 1.:
        status = 'review-digital-overload'
    elif verify and abs(median) > 1.0:
        status = 'review-level'
    elif headroom > median + .05:
        status = 'peak-limited'
    return {
        'status': status, 'measured_output_divisor_V': volts,
        'recommended_output_divisor_V': volts * 10**(correction / 20),
        'match_error_minus12_dB': levels[0], 'match_error_minus9_dB': levels[1],
        'match_error_minus6_dB': levels[2], 'holdout_minus9_dB': deltas[-1],
        'median_match_error_dB': median, 'max_output_peak_dBFS': db(peak),
        'settled_silence_dc_V': settled_dc * volts, 'startup_peak_dBFS': db(start_peak),
    }


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bench', type=Path)
    parser.add_argument('report', type=Path)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--preamp-only', action='store_true', help='calibrate the selected presets at their preamp tap')
    parser.add_argument('--fast-live', action='store_true', help='verify the same divisor in 1x Fast Live')
    parser.add_argument('--live-preview', action='store_true', help='verify the same divisor in 1x Preview')
    parser.add_argument('--jobs', type=int, default=1)
    parser.add_argument('--catalog', type=Path, default=ROOT / 'schematics')
    parser.add_argument('--filter', default='')
    parser.add_argument('--amp-di', action='store_true',
                        help='fit with palm mutes and strummed chords, then verify with picked notes')
    args = parser.parse_args()
    if (args.fast_live or args.live_preview) and not args.verify:
        parser.error('--fast-live and --live-preview are verification modes; fit once in Accurate')
    bench = args.bench.resolve()
    catalog = args.catalog.resolve()
    paths = [p for p in sorted(catalog.rglob('*.livewdf')) if args.filter.lower() in p.stem.lower()]
    if not paths or args.jobs < 1:
        parser.error('no matching schematics or invalid worker count')
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        profiles = ('amp',) if args.amp_di else (*SOURCES, 'bus', 'voice')
        inputs = {p: fixture(p) for p in profiles}
        for key, values in inputs.items():
            values.tofile(work / (key + '.f32'))

        def render(pair):
            i, path = pair
            row = {'schematic_path': str(Path('schematics') / path.relative_to(catalog))}
            try:
                doc = json.loads(path.read_text())
                profile, gate = family(path, doc)
                if args.amp_di:
                    profile = 'amp'
                vin = next(c for c in doc['components'] if c['type'] == 'in')['values']['referenceMillivoltsRms'] / 89.01946956877224
                if args.preamp_only:
                    tap = next(c for c in doc['components'] if c.get('properties', {}).get('preampTap'))
                    out, scale = tap.setdefault('values', {}), 'preampFullScaleVolts'
                else:
                    terminal = next(c for c in doc['components'] if c['type'] == 'out')
                    out, scale = terminal.setdefault('values', {}), 'fullScaleVolts'
                row.update(profile=profile, input_full_scale_V_peak=vin)
                load = path
                if not args.verify:
                    out[scale] = 1.
                    load = work / f'{i}.livewdf'
                    load.write_text(json.dumps(doc))
                volts = float(out[scale])
                dest = work / f'{i}.f32'
                result = subprocess.run([str(bench), str(load), '--calibration-render',
                                         str(work / (profile + '.f32')), str(dest),
                                         *(['--fast-live'] if args.fast_live else []),
                                         *(['--live-preview'] if args.live_preview else []),
                                         *(['--preamp-only'] if args.preamp_only else [])],
                                        capture_output=True, timeout=300, text=True)
                if result.returncode:
                    raise RuntimeError((result.stderr or result.stdout)[-350:])
                values = np.fromfile(dest, dtype='=f4')
                row.update(measure(inputs[profile], values, gate, volts, vin, args.verify))
                if args.fast_live or args.live_preview:
                    # A mode whose level differs takes this as its fastTrim or previewTrim beside fullScaleVolts.
                    trim = out.get('previewTrim' if args.live_preview else 'fastTrim', 1.)
                    row['recommended_trim'] = trim * row['recommended_output_divisor_V'] / volts
                dest.unlink()
            except (ValueError, KeyError, RuntimeError, OSError, StopIteration,
                    subprocess.SubprocessError) as e:
                row.update(status='error', error=str(e))
            return row

        keys = ['schematic_path', 'profile', 'status', 'input_full_scale_V_peak',
                'measured_output_divisor_V', 'recommended_output_divisor_V',
                'match_error_minus12_dB', 'match_error_minus9_dB', 'match_error_minus6_dB',
                'median_match_error_dB', 'holdout_minus9_dB', 'max_output_peak_dBFS',
                'settled_silence_dc_V', 'startup_peak_dBFS', 'recommended_trim', 'error']
        bad = 0
        with args.report.open('w', newline='') as target, ThreadPoolExecutor(args.jobs) as pool:
            writer = csv.DictWriter(target, keys)
            writer.writeheader()
            for row in pool.map(render, enumerate(paths)):
                writer.writerow({k: f'{v:.8g}' if isinstance(v, float) else v for k, v in row.items()})
                target.flush()
                print(row['status'], row['schematic_path'], file=sys.stderr, flush=True)
                bad += row['status'] == 'error' or (args.verify and row['status'].startswith('review-'))
        return bool(bad)


if __name__ == '__main__':
    sys.exit(run())
