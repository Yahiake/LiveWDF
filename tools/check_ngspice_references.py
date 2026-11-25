#!/usr/bin/env python3
"""Check saved ngspice responses without requiring ngspice during ordinary tests."""
import argparse
import bisect
import cmath
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

from check_circuit_regressions import (
    MODES, RATE, case_key, factory_documents, recording_provenance, render_wave,
    rms, run_renderer, stimulus, write_manifest_atomic, write_wave,
)


AC_CASES = (
    ('MXR Micro Amp', 'Gain'),
    ('Boss SD-1 Super OverDrive', 'Tone'),
    ('Ibanez TS_808', 'TONE'),
    ('Neve 1081 Gold Label Equalizer', 'Bass Gain'),
)
TRANSIENT_CASES = (
    ('MXR Micro Amp', 'Gain', .8, .1),
    ('Boss SD-1 Super OverDrive', 'Drive', .5, .05),
)


def configurations(root):
    paths = factory_documents(root)
    for title, control in AC_CASES:
        for position in (.2, .5, .8):
            for frequency in (375, 1500, 6000):
                yield dict(name=f'{title}/{control}/{position}/{frequency}Hz', circuit=str(paths[title]),
                           positions={control: position}, analysis='ac', frequency=frequency,
                           amplitude=1e-5, samples=8192, normalized_input=False, warmup=1024,
                           switches={'Bass 330 Hz': 1} if control == 'Bass Gain' else {})
    for title, control, position, amplitude in TRANSIENT_CASES:
        yield dict(name=f'{title}/{control}/{position}/transient', circuit=str(paths[title]),
                   positions={control: position}, analysis='transient', frequency=750,
                   amplitude=amplitude, samples=4096, normalized_input=False, warmup=1024)


def read_deck(path):
    text = path.read_text(encoding='utf-8')
    return text, hashlib.sha256(text.encode('utf-8')).hexdigest()


def numbers(path):
    rows = []
    for line in path.read_text().splitlines():
        try:
            rows.append([float(value) for value in line.split()])
        except ValueError:
            continue
    if not rows:
        raise RuntimeError(f'No numeric reference in {path}')
    return rows


def simulate(ngspice, renderer, configuration, directory):
    key = case_key(configuration['name'])
    scratch = directory / key
    scratch.mkdir()
    prefix = scratch / 'reference'
    run_renderer(renderer, configuration, 'accurate', prefix, 'export')
    deck = prefix.with_suffix('.cir')
    result = subprocess.run([str(ngspice), '-b', '-o', 'ngspice.log', str(deck)],
                            cwd=scratch, text=True, capture_output=True, timeout=180)
    if result.returncode:
        raise RuntimeError((scratch / 'ngspice.log').read_text())
    text, digest = read_deck(deck)
    provenance = {'deck': key + '-' + digest[:12] + '.cir', 'deck_sha256': digest}
    if configuration['analysis'] == 'ac':
        row = numbers(scratch / 'ac.txt')[-1]
        provenance['response'] = row[-2:]
    else:
        rows = numbers(scratch / 'tran.txt')
        times, values = [row[0] for row in rows], [row[-1] for row in rows]
        reference = []
        for n in range(configuration['samples']):
            time = n / RATE
            high = bisect.bisect_left(times, time)
            if high == 0:
                reference.append(values[0])
            elif high == len(times):
                if time - times[-1] > 1e-8:
                    raise RuntimeError('Transient reference ended early')
                reference.append(values[-1])
            else:
                fraction = (time - times[high - 1]) / (times[high] - times[high - 1])
                reference.append(values[high - 1] + fraction * (values[high] - values[high - 1]))
        provenance['wave'] = reference[-1024:]
    return provenance, text


def compare(configuration, wave, reference):
    errors = []
    if configuration['analysis'] == 'ac':
        segment = wave[-4096:]
        start = len(wave) - len(segment)
        frequency, amplitude = configuration['frequency'], configuration['amplitude']
        real = 2 * sum(value * math.sin(2 * math.pi * frequency * (start + n) / RATE)
                       for n, value in enumerate(segment)) / len(segment) / amplitude
        imaginary = 2 * sum(value * math.cos(2 * math.pi * frequency * (start + n) / RATE)
                            for n, value in enumerate(segment)) / len(segment) / amplitude
        response = reference['response']
        if len(response) != 2:
            raise RuntimeError('Invalid saved AC response')
        expected = complex(*response)
        actual = complex(real, imaginary)
        if not (cmath.isfinite(expected) and cmath.isfinite(actual)):
            raise RuntimeError('Non-finite AC response')
        if abs(expected) < 1e-10 or abs(actual) < 1e-10:
            raise RuntimeError('Unexpected silent AC response')
        gain_error = 20 * math.log10(abs(actual / expected))
        phase_error = math.degrees(cmath.phase(actual / expected))
        # The pristine 48 kHz baseline differs from the analogue reference by up
        # to 0.98 dB at 6 kHz. Retain that measured discrepancy explicitly.
        gain_limit = 1.25 if frequency == 6000 else .75
        if abs(gain_error) > gain_limit:
            errors.append(f'Gain differs from ngspice by {gain_error:.3f} dB')
        if abs(phase_error) > 10:
            errors.append(f'Phase differs from ngspice by {phase_error:.3f} degrees')
        return dict(gain_error_db=gain_error, phase_error_degrees=phase_error, gain_limit_db=gain_limit,
                    gain_db=20 * math.log10(abs(actual))), errors
    expected = reference['wave']
    if len(expected) != 1024 or len(wave) != configuration['samples']:
        raise RuntimeError('Transient sample count changed')
    if not all(math.isfinite(value) for value in expected):
        raise RuntimeError('Non-finite saved transient')
    actual = wave[-len(expected):]
    difference = rms([value - original for value, original in zip(actual, expected)]) / max(rms(expected), 1e-9)
    if difference > .08:
        errors.append(f'Transient differs from saved ngspice by {difference:.2%} RMS')
    return {'wave_error_percent': difference * 100}, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--ngspice', type=Path)
    parser.add_argument('--record', action='store_true')
    parser.add_argument('--reference-revision', help='Core revision used to build the recording renderer; defaults to repository HEAD')
    parser.add_argument('--filter', default='')
    args = parser.parse_args()
    args.renderer = args.renderer.resolve()
    if args.record:
        args.baseline.mkdir(parents=True, exist_ok=True)
    manifest = args.baseline / 'ngspice.json'
    saved = json.loads(manifest.read_text()) if manifest.exists() else {'version': 1, 'cases': {}}
    if args.record and args.ngspice is None:
        parser.error('--record requires --ngspice')
    if args.ngspice:
        args.ngspice = args.ngspice.resolve()
    reports, failures, decks = {}, [], {}
    planned = list(configurations(args.root.resolve()))
    if not args.filter and not args.record:
        expected_names = {configuration['name'] for configuration in planned}
        for name in sorted(set(saved['cases']) - expected_names):
            failures.append(f'Saved ngspice case removed or renamed: {name}')
    if args.record and not args.filter:
        saved['cases'] = {}
    with tempfile.TemporaryDirectory(prefix='livewdf-ngspice-reference-') as scratch:
        directory = Path(scratch)
        for configuration in planned:
            if args.filter and args.filter not in configuration['name']:
                continue
            name = configuration['name']
            try:
                if args.record:
                    reference, deck = simulate(args.ngspice, args.renderer, configuration, directory)
                    reference['circuit_sha256'] = hashlib.sha256(Path(configuration['circuit']).read_bytes()).hexdigest()
                    saved['cases'][name] = reference
                    decks[reference['deck']] = deck
                else:
                    reference = saved['cases'][name]
                    _, digest = read_deck(args.baseline / reference['deck'])
                    if digest != reference['deck_sha256']:
                        raise RuntimeError('Saved ngspice deck hash mismatch')
                stimulus_path = directory / (case_key(name) + '.f64')
                write_wave(stimulus_path, stimulus(configuration['samples'], configuration['amplitude'], 'sine', configuration['frequency']))
                configuration['stimulus'] = str(stimulus_path)
                for mode in MODES:
                    prefix = directory / (case_key(name) + '-' + mode)
                    wave, telemetry = render_wave(args.renderer, configuration, mode, prefix)
                    detail, errors = compare(configuration, wave, reference)
                    if telemetry['rejected']:
                        errors.append(f'{telemetry["rejected"]} rejected states')
                    detail['failures'] = errors
                    reports[name + '/' + mode] = detail
                    failures.extend(name + '/' + mode + ': ' + error for error in errors)
            except (RuntimeError, KeyError, OSError, TypeError, ValueError, subprocess.TimeoutExpired) as error:
                failures.append(name + ': ' + str(error))
    if not reports:
        failures.append('No reference comparisons ran')
    if args.record and not failures:
        saved['simulator'] = subprocess.check_output([str(args.ngspice), '--version'], text=True).strip()
        saved['provenance'] = recording_provenance(args.root.resolve(), args.renderer, args.reference_revision)
        for name, deck in decks.items():
            (args.baseline / name).write_bytes(deck.encode('utf-8'))
        write_manifest_atomic(manifest, saved)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({'cases': reports, 'failures': failures}, indent=2))
    print(f'{len(reports)} saved-solver comparisons, {len(failures)} failures', flush=True)
    for failure in failures:
        print(failure, file=sys.stderr)
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
