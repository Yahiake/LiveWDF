"""Focused circuit-checker regressions authored by jopdorp."""
from array import array
from contextlib import redirect_stdout
import gzip
import hashlib
import io
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import check_circuit_regressions as CHECKER
import check_ngspice_references as NGSPICE

ROUTE_KEYS = ('loaded_hits', 'affine_hits', 'affine_node_hits', 'plan_hits', 'bipolar_hits')


def sine():
    return array('d', (math.sin(2 * math.pi * 375 * sample / CHECKER.RATE) for sample in range(2048)))


def report_for(wave):
    return {'samples': len(wave), 'rejected': 0, 'features': CHECKER.features(wave),
            'newton': 0, 'evaluations': 0, 'line_trials': 0,
            **{key: 0 for key in ROUTE_KEYS}}


class SnapshotChecks(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.wave = sine()
        self.saved = CHECKER.save_reference(self.wave, report_for(self.wave), self.root)

    def tearDown(self):
        self.directory.cleanup()

    def check_wave(self, wave):
        return CHECKER.check_reference(wave, report_for(wave), self.saved, self.root, 'accurate')

    def test_unchanged_quantized_snapshot_passes(self):
        failures, _ = self.check_wave(self.wave)
        self.assertEqual(failures, [])

    def test_recording_excludes_timing_telemetry(self):
        report = report_for(self.wave)
        timing = {'blocks': [{'cpu_us': 1}], 'mean_cpu_us': 1, 'max_cpu_us': 1, 'late_cpu_blocks': 0}
        report.update(timing)
        saved = CHECKER.save_reference(self.wave, report, self.root)
        self.assertTrue(saved.keys().isdisjoint(timing))

    def test_nonfinite_saved_ac_response_fails(self):
        config = {'analysis': 'ac', 'frequency': 375, 'amplitude': 1.}
        with self.assertRaisesRegex(RuntimeError, 'Non-finite AC response'):
            NGSPICE.compare(config, self.wave, {'response': [float('nan'), 0.]})

    def test_truncated_reference_is_rejected(self):
        path = self.root / self.saved['wave']
        data = gzip.decompress(path.read_bytes())
        path.write_bytes(gzip.compress(data[:32 * 4], mtime=0))
        failures, _ = self.check_wave(self.wave)
        self.assertTrue(any('reference length' in failure for failure in failures), failures)

    def test_full_length_reference_with_changed_content_is_rejected(self):
        path = self.root / self.saved['wave']
        data = bytearray(gzip.decompress(path.read_bytes()))
        data[0] ^= 1
        path.write_bytes(gzip.compress(data, mtime=0))
        failures, _ = self.check_wave(self.wave)
        self.assertTrue(any('content hash does not match' in failure for failure in failures), failures)

    def test_legacy_reference_requires_hash_during_checks(self):
        del self.saved['wave_sha256']
        failures, _ = self.check_wave(self.wave)
        self.assertTrue(any('content hash is missing' in failure for failure in failures), failures)

    def test_single_doubled_peak_is_rejected_below_rms_error_limit(self):
        changed = array('d', self.wave)
        changed[32] *= 2
        report = report_for(changed)
        failures, _ = CHECKER.check_reference(changed, report, self.saved, self.root, 'accurate')
        self.assertLess(report['saved_wave_error_percent'], 5)
        self.assertTrue(any('Full-wave peak' in failure for failure in failures), failures)
        self.assertTrue(any('Full-wave max_step' in failure for failure in failures), failures)

    def test_small_dc_shift_is_rejected_below_rms_error_limit(self):
        changed = array('d', (value + .01 for value in self.wave))
        report = report_for(changed)
        failures, _ = CHECKER.check_reference(changed, report, self.saved, self.root, 'accurate')
        self.assertLess(report['saved_wave_error_percent'], 5)
        self.assertTrue(any('Full-wave mean' in failure for failure in failures), failures)

    def test_affine_node_admission_changes_skip_work_comparison(self):
        report = report_for(self.wave)
        report['affine_node_hits'] = len(self.wave)
        report['newton'] = 100000
        failures, observations = CHECKER.check_reference(self.wave, report, self.saved, self.root, 'accurate')
        self.assertEqual(failures, [])
        self.assertFalse(report['work_route_comparable'])
        self.assertTrue(observations)

    def test_full_hash_covers_decoded_reference(self):
        raw = gzip.decompress((self.root / self.saved['wave']).read_bytes())
        self.assertEqual(self.saved['wave_sha256'], hashlib.sha256(raw).hexdigest())
        self.assertEqual(len(self.saved['wave_sha256']), 64)

    def test_sampled_reference_matches_all_expected_checkpoints(self):
        saved = CHECKER.save_reference(self.wave, report_for(self.wave), self.root, stride=7)
        raw = gzip.decompress((self.root / saved['wave']).read_bytes())
        self.assertEqual(len(raw), ((len(self.wave) + 6) // 7) * 4)
        failures, _ = CHECKER.check_reference(self.wave, report_for(self.wave), saved, self.root, 'accurate')
        self.assertEqual(failures, [])

    def test_spike_between_checkpoints_still_fails_full_rate_bounds(self):
        saved = CHECKER.save_reference(self.wave, report_for(self.wave), self.root, stride=7)
        changed = array('d', self.wave)
        changed[32] *= 2  # Not a stored checkpoint.
        report = report_for(changed)
        failures, _ = CHECKER.check_reference(changed, report, saved, self.root, 'accurate')
        self.assertLess(report['saved_wave_error_percent'], .001)
        self.assertTrue(any('Full-wave peak' in failure for failure in failures), failures)
        self.assertTrue(any('Full-wave max_step' in failure for failure in failures), failures)

    def test_sampled_reference_requires_full_rate_bounds(self):
        saved = CHECKER.save_reference(self.wave, report_for(self.wave), self.root, stride=7)
        del saved['wave_bounds']
        failures, _ = CHECKER.check_reference(self.wave, report_for(self.wave), saved, self.root, 'accurate')
        self.assertTrue(any('full-rate bounds' in failure for failure in failures), failures)

    def test_stimulus_aligned_stride_is_rejected(self):
        self.saved['reference_stride'] = 8
        failures, _ = self.check_wave(self.wave)
        self.assertTrue(any('reference stride' in failure for failure in failures), failures)

    def test_truncated_sampled_reference_is_rejected(self):
        saved = CHECKER.save_reference(self.wave, report_for(self.wave), self.root, stride=7)
        path = self.root / saved['wave']
        path.write_bytes(gzip.compress(gzip.decompress(path.read_bytes())[:-4], mtime=0))
        failures, _ = CHECKER.check_reference(self.wave, report_for(self.wave), saved, self.root, 'accurate')
        self.assertTrue(any('reference length' in failure for failure in failures), failures)


class ReferencePayloadBudgetChecks(unittest.TestCase):
    def test_complete_fixture_payload_stays_below_two_mib(self):
        directory = ROOT / 'tests/fixtures/circuit_regressions'
        size = sum(path.stat().st_size for path in directory.iterdir() if path.is_file())
        self.assertLessEqual(size, 2 * 1024 * 1024,
                             'Review reference storage before increasing the 2 MiB fixture budget')


class InventoryAndRecordingChecks(unittest.TestCase):
    def test_duplicate_case_names_fail(self):
        failures = CHECKER.check_case_inventory([{'name': 'same'}, {'name': 'same'}], {}, True)
        self.assertEqual(failures, ['Duplicate case name: same'])

    def test_removed_case_is_detected_for_full_suite(self):
        failures = CHECKER.check_case_inventory([{'name': 'kept'}], {'removed/accurate': {}}, True)
        self.assertEqual(failures, ['Saved case was removed or renamed: removed/accurate'])

    def test_filtered_check_allows_unselected_saved_cases(self):
        self.assertEqual(CHECKER.check_case_inventory([{'name': 'kept'}], {'removed/accurate': {}}, False), [])

    def test_atomic_manifest_uses_unique_pending_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / 'suite.json'
            pending = []
            replace = CHECKER.os.replace

            def capture(source, target):
                pending.append(Path(source))
                replace(source, target)

            with patch.object(CHECKER.os, 'replace', side_effect=capture):
                CHECKER.write_manifest_atomic(destination, {'version': 1})
                CHECKER.write_manifest_atomic(destination, {'version': 2})
            self.assertNotEqual(pending[0], pending[1])
            self.assertTrue(all(path.parent == destination.parent for path in pending))
            self.assertTrue(all(not path.exists() for path in pending))
            self.assertEqual(json.loads(destination.read_text()), {'version': 2})

    def test_recording_accepts_existing_legacy_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = root / 'baseline'
            baseline.mkdir()
            (baseline / 'smoke.json').write_text(json.dumps({'version': 1, 'sample_rate': CHECKER.RATE,
                                                           'cases': {'legacy/accurate': {'samples': 2048}}}))
            case = {'name': 'legacy', 'circuit': root / 'fixture.livewdf', 'samples': 2048,
                    'positions': {}, 'moves': []}
            wave = sine()
            (root / 'unused').write_bytes(b'recording renderer fixture')
            arguments = ['checker', '--renderer', str(root / 'unused'), '--root', str(root),
                         '--suite', 'smoke', '--baseline', str(baseline), '--report', str(root / 'report.json'), '--record']
            with patch.object(CHECKER, 'cases', return_value=iter([case])), \
                    patch.object(CHECKER, 'render', side_effect=lambda *args: (wave, report_for(wave))), \
                    patch('sys.argv', arguments), redirect_stdout(io.StringIO()):
                self.assertFalse(CHECKER.main())
            saved = json.loads((baseline / 'smoke.json').read_text())
            self.assertEqual(saved['version'], 2)
            self.assertEqual(len(saved['cases']), 3)
            self.assertTrue(all(len(record['wave_sha256']) == 64 for record in saved['cases'].values()))
            self.assertEqual(saved['provenance']['renderer_sha256'], hashlib.sha256(b'recording renderer fixture').hexdigest())


class ControlBehaviorChecks(unittest.TestCase):
    def group(self, behavior, scales):
        positions = {}
        original = sine()
        for position, scale in zip((.2, .5, .8), scales):
            wave = array('d', (value * scale for value in original))
            positions[position] = {'wave': wave, 'features': CHECKER.features(wave)}
        return {'control/accurate': {'behavior': behavior, 'positions': positions}}

    def test_monotonic_gain_passes(self):
        failures, _ = CHECKER.check_control_behaviors(self.group('gain', (.2, .5, .8)))
        self.assertEqual(failures, [])

    def test_gain_dip_fails(self):
        failures, _ = CHECKER.check_control_behaviors(self.group('gain', (.4, .2, .8)))
        self.assertTrue(any('not monotonic' in failure for failure in failures))

    def test_flat_tone_control_fails(self):
        failures, _ = CHECKER.check_control_behaviors(self.group('tone', (1, 1, 1)))
        self.assertTrue(any('less than 0.5%' in failure for failure in failures))

    def test_measurable_control_effect_passes(self):
        failures, _ = CHECKER.check_control_behaviors(self.group('tone', (.99, 1, 1.01)))
        self.assertEqual(failures, [])

    def test_neve_band_is_engaged_and_delay_runs_long_enough(self):
        definitions = list(CHECKER.cases(ROOT, 'controls'))
        neve = [case for case in definitions if case['name'].startswith('Neve 1081')]
        delay = [case for case in definitions if case['behavior'] == 'delay']
        self.assertTrue(all(case['switches'] == {'Bass 330 Hz': 1} for case in neve))
        self.assertTrue(all(case['samples'] == 49152 for case in delay))
        moving = next(case for case in delay if case['name'].endswith('/moving'))
        self.assertGreater(moving['moves'][0]['sample'], 8192)


if __name__ == '__main__':
    unittest.main()
