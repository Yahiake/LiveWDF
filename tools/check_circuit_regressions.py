#!/usr/bin/env python3
"""Circuit-level waveform, control-motion and solver-work regression checks.

Authored by jopdorp.
"""
import argparse
from array import array
from collections import Counter
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile


MODES = ('accurate', 'fast', 'preview')
RATE = 48000
MIN_CONTROL_EFFECT = .005
CONTROL_CASES = (
    ('MXR Micro Amp', 'Gain', 'gain'),
    ('Boss SD-1 Super OverDrive', 'Drive', 'drive'),
    ('Boss SD-1 Super OverDrive', 'Tone', 'tone'),
    ('Boss SD-1 Super OverDrive', 'Level', 'gain'),
    ('Ibanez TS_808', 'TONE', 'tone'),
    ('Boss MT-2 Metal Zone', 'Mid Freq', 'tone'),
    ('Dunlop Cry Baby GCB-95', 'WAH WAH', 'tone'),
    ('Neve 1081 Gold Label Equalizer', 'Bass Gain', 'tone'),
    ('UREI-1176LN-Rev-D', 'Input', 'dynamics'),
    ('Boss CS-3 Compression Sustainer (August 1986)', 'Sustain', 'dynamics'),
    ('Boss DM-2 Analog Delay', 'Echo', 'delay'),
    ('Marshall 1959 Super Lead - Jumped', 'Presence', 'tone'),
    ('Marshall 1959 Super Lead - Jumped', 'Treble', 'tone', {'JCM800 era': 1}),
)


def read_wave(path, code='d'):
    data = array(code)
    data.frombytes(path.read_bytes())
    if sys.byteorder != 'little':
        data.byteswap()
    return data


def write_wave(path, wave, code='d'):
    data = array(code, wave)
    if sys.byteorder != 'little':
        data.byteswap()
    path.write_bytes(data.tobytes())


def rms(wave):
    return math.sqrt(sum(value * value for value in wave) / max(len(wave), 1))


def waveform_bounds(wave):
    mean = statistics.fmean(wave)
    return {'ac_rms': rms([value - mean for value in wave]), 'mean': mean,
            'peak': max(map(abs, wave)),
            'max_step': max((abs(right - left) for left, right in zip(wave, wave[1:])), default=0.0)}


def features(wave):
    result = waveform_bounds(wave)
    mean = result['mean']
    ac = [value - mean for value in wave]
    # Coherent frequencies make the checks independent of FFT libraries.
    for frequency in (375, 1500, 6000):
        real = sum(value * math.cos(2 * math.pi * frequency * n / RATE) for n, value in enumerate(ac))
        imaginary = sum(value * math.sin(2 * math.pi * frequency * n / RATE) for n, value in enumerate(ac))
        result[f'band_{frequency}'] = 2 * math.hypot(real, imaginary) / len(ac)
    return result


def stimulus(count, volts, kind='multitone', frequency=1000):
    for n in range(count):
        if kind == 'sine':
            yield volts * math.sin(2 * math.pi * frequency * n / RATE)
        else:
            envelope = 1.0 if n < count // 2 else 0.15
            yield volts * envelope * (
                .5 * math.sin(2 * math.pi * 375 * n / RATE)
                + .3 * math.sin(2 * math.pi * 1500 * n / RATE)
                + .2 * math.sin(2 * math.pi * 6000 * n / RATE))


def factory_documents(root):
    documents = {}
    for path in sorted((root / 'schematics').rglob('*.livewdf')):
        if path.stem in documents:
            raise RuntimeError(f'Duplicate factory circuit name: {path.stem}')
        documents[path.stem] = path
    return documents


def cases(root, suite):
    documents = factory_documents(root)
    if suite == 'smoke':
        for name, path in documents.items():
            yield {'name': name, 'circuit': path, 'samples': 2048, 'positions': {}, 'moves': []}
        return
    for title, control, behavior, *switch in CONTROL_CASES:
        path = documents.get(title)
        if path is None:
            raise RuntimeError(f'Control fixture circuit is missing: {title}')
        count = 49152 if behavior == 'delay' else 8192
        switches = switch[0] if switch else {'Bass 330 Hz': 1} if title == 'Neve 1081 Gold Label Equalizer' else {}
        for position in (.2, .5, .8):
            yield {'name': f'{title}/{control}/{position}', 'circuit': path, 'samples': count,
                   'positions': {control: position}, 'moves': [], 'behavior': behavior,
                   'control': control, 'position': position, 'switches': switches}
        moves = []
        # Sample-timed ramps reverse direction and finish at the original value.
        starts = (12288, 24576, 36864) if behavior == 'delay' else (512, 1536, 3072)
        for start, initial, target in zip(starts, (.5, .2, .8), (.2, .8, .5)):
            for offset in range(8, 513, 8):
                moves.append({'sample': start + offset, 'positions': {control: initial + (target - initial) * offset / 512}})
        yield {'name': f'{title}/{control}/moving', 'circuit': path, 'samples': count,
               'positions': {control: .5}, 'moves': moves, 'behavior': behavior, 'control': control,
               'switches': switches}


def case_key(name):
    readable = ''.join(character if character.isalnum() else '-' for character in name)[:65]
    return readable + '-' + hashlib.sha256(name.encode()).hexdigest()[:10]


def run_renderer(renderer, configuration, mode, prefix, action='render'):
    path = prefix.with_suffix('.configuration.json')
    path.write_text(json.dumps(configuration))
    result = subprocess.run([str(renderer), str(path), mode, str(prefix), action],
                            text=True, capture_output=True, timeout=180)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def render_wave(renderer, configuration, mode, prefix):
    run_renderer(renderer, configuration, mode, prefix)
    wave = read_wave(prefix.with_suffix('.f64'))
    report = json.loads(prefix.with_suffix('.json').read_text())
    if len(wave) != configuration['samples'] or report['samples'] != len(wave):
        raise RuntimeError('Unexpected rendered sample count')
    if not all(math.isfinite(value) for value in wave):
        raise RuntimeError('Non-finite rendered waveform')
    return wave, report


def render(renderer, case, mode, scratch):
    path = case['circuit']
    prefix = scratch / (case_key(case['name']) + '-' + mode)
    wave_path = prefix.with_suffix('.input.f64')
    write_wave(wave_path, stimulus(case['samples'], .04))
    config = {**case, 'circuit': str(path), 'stimulus': str(wave_path), 'warmup': 1024}
    wave, report = render_wave(renderer, config, mode, prefix)
    report['features'] = features(wave[-2048:])
    report['wave_bounds'] = waveform_bounds(wave)
    report['circuit_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
    cpu = [block['cpu_us'] for block in report['blocks']]
    report['mean_cpu_us'] = statistics.fmean(cpu)
    report['max_cpu_us'] = max(cpu)
    report['late_cpu_blocks'] = sum(value > 128 / RATE * 1e6 for value in cpu)
    return wave, report


def check_reference(wave, report, saved, baseline, mode):
    failures, observations = [], []
    if len(wave) != saved['samples']:
        return ['Rendered sample count changed'], observations
    stride = saved.get('reference_stride', 1)
    if type(stride) is not int or stride not in (1, 7):
        return ['Invalid saved reference stride'], observations
    if stride != 1 and 'wave_bounds' not in saved:
        return ['Sampled reference is missing its full-rate bounds'], observations
    with gzip.open(baseline / saved['wave'], 'rb') as source:
        data = source.read()
    expected_bytes = ((saved['samples'] + stride - 1) // stride) * 4
    if len(data) != expected_bytes:
        return [f'Saved reference length is {len(data)} bytes, expected {expected_bytes}'], observations
    if not saved.get('wave_sha256'):
        return ['Saved reference content hash is missing; record the baseline again'], observations
    if hashlib.sha256(data).hexdigest() != saved['wave_sha256']:
        return ['Saved reference content hash does not match its waveform'], observations
    reference = array('f')
    reference.frombytes(data)
    if sys.byteorder != 'little':
        reference.byteswap()
    if not all(math.isfinite(value) for value in reference):
        return ['Non-finite saved reference'], observations
    difference = rms([value - expected for value, expected in zip(wave[::stride], reference)])
    relative_error = difference / max(rms(reference), 1e-9)
    report['saved_wave_error_percent'] = relative_error * 100
    report['reference_stride'] = stride
    tolerance = .05 if mode == 'accurate' else .10
    if difference > 1e-7 and relative_error > tolerance:
        failures.append(f'Waveform changed by {relative_error:.2%}, limit {tolerance:.0%}')
    if report['rejected'] > saved['rejected']:
        failures.append(f'Rejected samples increased {saved["rejected"]} to {report["rejected"]}')
    old_bounds = saved.get('wave_bounds') or waveform_bounds(reference)
    current_bounds = report.get('wave_bounds') or waveform_bounds(wave)
    report['wave_bounds'] = current_bounds
    for metric in ('peak', 'mean', 'max_step', 'ac_rms'):
        old = old_bounds[metric]
        scale = max(abs(old), old_bounds['ac_rms'] * .02, 1e-8)
        if abs(current_bounds[metric] - old) > .15 * scale:
            failures.append(f'Full-wave {metric} changed from {old:.6g} to {current_bounds[metric]:.6g}')
    for metric in ('ac_rms', 'band_375', 'band_1500', 'band_6000'):
        old = saved['features'][metric]
        scale = max(abs(old), saved['features']['ac_rms'] * .02, 1e-8)
        if abs(report['features'][metric] - old) > .15 * scale:
            failures.append(f'{metric} changed from {old:.6g} to {report["features"][metric]:.6g}')
    profile = ('loaded_hits', 'affine_hits', 'affine_node_hits', 'plan_hits', 'bipolar_hits')
    if any(key not in saved or key not in report for key in profile):
        return failures + ['Solver-route metadata is incomplete; record the baseline again'], observations
    same_route = all(abs(report[key] - saved[key]) <= report['samples'] * .01 for key in profile)
    report['work_route_comparable'] = same_route
    if same_route:
        for key in ('newton', 'evaluations', 'line_trials'):
            ceiling = saved[key] * 1.5 + report['samples'] * 2
            if report[key] > ceiling:
                failures.append(f'{key} work increased {saved[key]} to {report[key]}, budget {ceiling:.0f}')
    else:
        observations.append('Prepared solver admission changed, work counters retained for inspection')
    return failures, observations


def save_reference(wave, report, directory, stride=1):
    if type(stride) is not int or stride not in (1, 7):
        raise ValueError('Reference stride must be 1 or 7')
    # Seven is coprime to the coherent stimulus periods (8, 32 and 128).
    # Keep full-rate bounds and spectra while storing fewer waveform checkpoints.
    data = array('f', wave[::stride])
    if sys.byteorder != 'little':
        data.byteswap()
    raw = data.tobytes()
    digest = hashlib.sha256(raw).hexdigest()
    destination = directory / (digest + '.f32.gz')
    if not destination.exists():
        destination.write_bytes(gzip.compress(raw, mtime=0))
    timing = {'blocks', 'mean_cpu_us', 'max_cpu_us', 'late_cpu_blocks'}
    recorded = {name: value for name, value in report.items() if name not in timing}
    return {**recorded, 'wave': destination.name, 'wave_sha256': digest,
            'reference_stride': stride, 'wave_bounds': report.get('wave_bounds') or waveform_bounds(wave)}


def check_case_inventory(definitions, saved_cases, full_suite):
    counts = Counter(case['name'] for case in definitions)
    failures = [f'Duplicate case name: {name}' for name, count in counts.items() if count > 1]
    if full_suite:
        expected = {case['name'] + '/' + mode for case in definitions for mode in MODES}
        failures.extend('Saved case was removed or renamed: ' + name for name in sorted(set(saved_cases) - expected))
    return failures


def check_control_behaviors(groups):
    failures, reports = [], {}
    for name, group in groups.items():
        positions = group['positions']
        if set(positions) != {.2, .5, .8}:
            reports[name] = {'observation': 'A complete three-position control sweep was not selected'}
            continue
        levels = [positions[position]['features']['ac_rms'] for position in (.2, .5, .8)]
        result = {'behavior': group['behavior'], 'ac_rms_20_50_80': levels}
        if group['behavior'] == 'gain':
            if any(right < left * .98 for left, right in zip(levels, levels[1:])):
                failures.append(name + ': Gain control output is not monotonic within 2%')
            if levels[2] <= max(levels[0] * 1.01, 1e-9):
                failures.append(name + ': Gain control endpoints have less than 1% measurable gain increase')
        else:
            low, high = positions[.2], positions[.8]
            low_mean, high_mean = statistics.fmean(low['wave']), statistics.fmean(high['wave'])
            low_ac = [value - low_mean for value in low['wave']]
            high_ac = [value - high_mean for value in high['wave']]
            if len(low_ac) != len(high_ac):
                failures.append(name + ': Control endpoints rendered different sample counts')
                continue
            waveform_effect = rms([right - left for left, right in zip(low_ac, high_ac)]) / max(rms(low_ac), rms(high_ac), 1e-9)
            spectral_effect = max(abs(high['features'][key] - low['features'][key]) /
                                  max(abs(high['features'][key]), abs(low['features'][key]),
                                      max(levels) * .02, 1e-9)
                                  for key in ('band_375', 'band_1500', 'band_6000'))
            result.update(waveform_effect=waveform_effect, spectral_effect=spectral_effect)
            # Existing MT-2 Mid Freq endpoints differ by about 1.8% RMS.
            # A 0.5% threshold preserves margin while rejecting flat controls.
            if max(waveform_effect, spectral_effect) < MIN_CONTROL_EFFECT:
                failures.append(name + ': Control endpoints have less than 0.5% waveform or spectral effect')
        reports[name] = result
    return failures, reports


def write_manifest_atomic(path, reference):
    pending = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                         prefix=path.name + '.', suffix='.pending', delete=False) as stream:
            pending = Path(stream.name)
            json.dump(reference, stream, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(pending, path)
    finally:
        if pending is not None:
            pending.unlink(missing_ok=True)


def recording_provenance(root, renderer, revision=None):
    if revision is None:
        try:
            revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'],
                                               text=True, stderr=subprocess.DEVNULL).strip()
        except (OSError, subprocess.CalledProcessError):
            revision = 'unavailable'
    return {'declared_core_revision': revision, 'renderer_sha256': hashlib.sha256(renderer.read_bytes()).hexdigest(),
            'sample_rate': RATE, 'block_size': 128, 'oversampling': 1, 'warmup_samples': 1024,
            'note': 'Revision identifies the declared renderer source; rebuild before recording.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--suite', choices=('smoke', 'controls'), required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--record', action='store_true')
    parser.add_argument('--reference-stride', type=int, choices=(1, 7), default=7,
                        help='Store every seventh sample by default; full-rate metrics are always retained')
    parser.add_argument('--reference-revision', help='Core revision used to build the recording renderer; defaults to repository HEAD')
    parser.add_argument('--filter', default='')
    args = parser.parse_args()
    args.renderer = args.renderer.resolve()
    if args.record:
        args.baseline.mkdir(parents=True, exist_ok=True)
    manifest_path = args.baseline / (args.suite + '.json')
    reference = {'version': 2, 'sample_rate': RATE, 'cases': {}}
    if manifest_path.exists():
        reference = json.loads(manifest_path.read_text())
    elif not args.record:
        raise RuntimeError(f'Saved reference is missing: {manifest_path}')
    reports, failures, control_results = {}, [], {}
    try:
        definitions = list(cases(args.root.resolve(), args.suite))
        failures.extend(check_case_inventory(definitions, reference['cases'], not args.filter and not args.record))
    except RuntimeError as error:
        failures.append(str(error))
        definitions = []
    if failures:
        definitions = []
    if args.record and not args.filter:
        reference['cases'] = {}
    if args.record:
        reference['version'] = 2
        reference['provenance'] = recording_provenance(args.root.resolve(), args.renderer, args.reference_revision)
    staging_parent = args.baseline.parent if args.record else None
    with tempfile.TemporaryDirectory(prefix='livewdf-regression-', dir=staging_parent) as directory:
        for case in definitions:
            if args.filter and args.filter not in case['name']:
                continue
            for mode in MODES:
                name = case['name'] + '/' + mode
                try:
                    wave, report = render(args.renderer, case, mode, Path(directory))
                    report['case'] = case['name']
                    if args.record:
                        reference['cases'][name] = save_reference(wave, report, Path(directory), args.reference_stride)
                        errors, observations = [], []
                    else:
                        errors, observations = check_reference(wave, report, reference['cases'][name], args.baseline, mode)
                    report['failures'], report['observations'] = errors, observations
                    reports[name] = report
                    if 'position' in case:
                        control_name = case['circuit'].stem + '/' + case['control'] + '/' + mode
                        group = control_results.setdefault(control_name, {'behavior': case['behavior'], 'positions': {}})
                        group['positions'][case['position']] = {'wave': wave, 'features': report['features']}
                    failures.extend(f'{name}: {error}' for error in errors)
                except (RuntimeError, KeyError, OSError, EOFError, ValueError, subprocess.TimeoutExpired) as error:
                    failures.append(name + ': ' + str(error))
                    reports[name] = {'failures': [str(error)]}
        behavior_failures, behavior_reports = check_control_behaviors(control_results)
        failures.extend(behavior_failures)
        if args.record and reports and not failures:
            for path in Path(directory).glob('*.f32.gz'):
                os.replace(path, args.baseline / path.name)
    if not reports and not failures:
        failures.append('No circuits selected')
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({'suite': args.suite, 'cases': reports, 'behavior_checks': behavior_reports,
                                       'failures': failures}, indent=2))
    if args.record and not failures:
        # Content-addressed waves keep the prior manifest valid until replacement.
        write_manifest_atomic(manifest_path, reference)
    print(f'{len(reports)} mode/circuit recordings, {len(failures)} failures', flush=True)
    for failure in failures:
        print(failure, file=sys.stderr)
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
