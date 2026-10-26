"""Statistical and filtering checks for the standalone skew analysis."""
import csv
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import profile_embedding_skew as skew


class SkewTests(unittest.TestCase):
    def test_calibration_and_zipf_ratio(self):
        alpha, probabilities = skew.calibrated_zipf(4096, 205, .8)
        self.assertAlmostEqual(sum(probabilities), 1., places=12)
        self.assertAlmostEqual(sum(probabilities[:205]), .8, places=12)
        self.assertAlmostEqual(probabilities[0] / probabilities[1], 2 ** alpha, places=12)
        self.assertAlmostEqual(alpha, 1.154067593, places=8)

    def test_uniform_and_impossible_targets(self):
        alpha, probabilities = skew.calibrated_zipf(100, 5, .05)
        self.assertAlmostEqual(alpha, 0., places=12)
        self.assertTrue(all(abs(p - .01) < 1e-12 for p in probabilities))
        for target in (.01, 1.):
            with self.assertRaises(ValueError):
                skew.calibrated_zipf(100, 5, target)

    def test_allocated_denominator_and_rounding(self):
        summary = skew.statistics([8, 1, 1, 0], 1)
        self.assertEqual(summary['empirical_top_configured_head_mass'], .8)
        self.assertEqual(summary['smallest_row_fraction_for_80pct'], .25)
        self.assertEqual(summary['untouched_rows'], 1)
        self.assertEqual(skew.hot_count(4096, .05), 205)

    def test_event_filters_ignore_labels(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'events.csv'
            b = skew.EMBEDDING_BASE
            events = [
                [1, b, 0, 1, 1, 'cold'], [2, b + 64, 0, 1, 0, 'hot'],
                [1, b + 64, 0, 0, 1, 'anything'],
                [0, b, 0, 1, 1, 'host_read'], [1, b, 1, 1, 1, 'store'],
                [1, b + 4096, 0, 1, 1, 'scratch'],
            ]
            with path.open('w', newline='') as stream:
                writer = csv.writer(stream)
                writer.writerow(['node', 'address', 'write', 'measured', 'fills', 'kind'])
                writer.writerows(events)
            counts = skew.event_counts(path, 4, 3)
            self.assertEqual(counts['all', 'logical_lookups'], [1, 2, 0, 0])
            self.assertEqual(counts['measured', 'logical_lookups'], [1, 1, 0, 0])
            self.assertEqual(counts['all', 'data_fills'], [1, 1, 0, 0])
            self.assertEqual(counts['measured', 'data_fills'], [1, 0, 0, 0])

    def test_sampling_and_interval(self):
        first = skew.sample_zipf([.6, .3, .1], 100_000, 42)
        self.assertEqual(first, skew.sample_zipf([.6, .3, .1], 100_000, 42))
        self.assertEqual(sum(first), 100_000)
        self.assertTrue(all(abs(c / 100_000 - p) < .01 for c, p in zip(first, [.6, .3, .1])))
        lo, hi = skew.wilson95(800_000, 1_000_000)
        self.assertLess(lo, .8)
        self.assertGreater(hi, .8)

    def test_fill_curve_has_no_assumed_zipf_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / 'report'
            result = skew.write_profiles(prefix, {'fills': [1, 1, 0]}, {'fills': None}, 1)
            self.assertIsNone(result['fills']['max_rank_cdf_difference'])
            with Path(str(prefix) + '.ranks.csv').open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 3)
            self.assertTrue(all(r['expected_probability'] == '' for r in rows))


if __name__ == '__main__':
    unittest.main()
