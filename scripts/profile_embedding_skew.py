#!/usr/bin/env python3
"""Profile numeric embedding addresses and sample a calibrated finite Zipf law.

This analysis does not change the simulator or its timing results. Event labels
are ignored. Rank statistics include unobserved allocated rows, not just hits.
"""
import argparse
import bisect
import csv
import json
import math
from pathlib import Path
import random

EMBEDDING_BASE = 1 << 44


def hot_count(rows, fraction):
    return max(1, min(rows - 1, math.floor(rows * fraction + .5)))


def zipf_probabilities(rows, alpha):
    if rows < 2 or not math.isfinite(alpha) or alpha < 0:
        raise ValueError('need >=2 rows and finite nonnegative alpha')
    weights = [math.exp(-alpha * math.log(rank)) for rank in range(1, rows + 1)]
    total = math.fsum(weights)
    return [weight / total for weight in weights]


def calibrated_zipf(rows, head_rows, head_mass):
    """Find alpha >=0 with sum(p[0:head_rows]) == head_mass."""
    if not 0 < head_rows < rows or not head_rows / rows <= head_mass < 1:
        raise ValueError('target mass must be >= head_rows/rows and <1')
    lower, upper = 0., 1.
    while math.fsum(zipf_probabilities(rows, upper)[:head_rows]) < head_mass:
        upper *= 2
    for _ in range(60):
        alpha = (lower + upper) / 2
        if math.fsum(zipf_probabilities(rows, alpha)[:head_rows]) < head_mass:
            lower = alpha
        else:
            upper = alpha
    alpha = (lower + upper) / 2
    return alpha, zipf_probabilities(rows, alpha)


def hot_cold_probabilities(rows, head_rows, head_mass, alpha):
    weights = zipf_probabilities(head_rows, alpha) if head_rows > 1 else [1.]
    return sorted([head_mass * p for p in weights] +
                  [(1 - head_mass) / (rows - head_rows)] * (rows - head_rows), reverse=True)


def sample_zipf(probabilities, samples, seed):
    cdf, cumulative = [], 0.
    for probability in probabilities:
        cumulative += probability
        cdf.append(cumulative)
    cdf[-1] = 1.
    rng = random.Random(seed)
    counts = [0] * len(probabilities)
    for _ in range(samples):
        counts[bisect.bisect_right(cdf, rng.random())] += 1
    return counts


def wilson95(successes, trials):
    if trials == 0:
        return [None, None]
    z = 1.959963984540054
    p, correction = successes / trials, z * z / trials
    center = (p + correction / 2) / (1 + correction)
    half = z * math.sqrt(p * (1 - p) / trials + correction / (4 * trials)) / (1 + correction)
    return [center - half, center + half]


def event_counts(path, rows, agents):
    counts = {(scope, population): [0] * rows for scope in ('all', 'measured')
              for population in ('logical_lookups', 'data_fills')}
    with path.open() as stream:
        for event in csv.DictReader(stream):
            node, address = int(event['node']), int(event['address'])
            if not 1 <= node <= agents or event['write'] != '0':
                continue
            if not EMBEDDING_BASE <= address < EMBEDDING_BASE + rows * 64:
                continue
            if address % 64:
                raise ValueError('embedding address is not 64B aligned')
            row = (address - EMBEDDING_BASE) // 64
            for scope in ('all', 'measured') if event['measured'] == '1' else ('all',):
                counts[scope, 'logical_lookups'][row] += 1
                if int(float(event['fills'])):
                    counts[scope, 'data_fills'][row] += 1
    return counts


def statistics(counts, head_rows):
    total = sum(counts)
    ranked = sorted(counts, reverse=True)
    def mass(k):
        return sum(ranked[:k]) / total if total else None
    needed, cumulative = None, 0
    if total:
        for rank, count in enumerate(ranked, 1):
            cumulative += count
            if cumulative >= .8 * total:
                needed = rank
                break
    return {
        'requests': total,
        'allocated_rows': len(counts),
        'touched_rows': sum(count > 0 for count in counts),
        'untouched_rows': sum(count == 0 for count in counts),
        'empirical_top_1pct_mass': mass(max(1, math.ceil(len(counts) * .01))),
        'empirical_top_configured_head_mass': mass(head_rows),
        'empirical_top_10pct_mass': mass(math.ceil(len(counts) * .1)),
        'smallest_row_count_for_80pct': needed,
        'smallest_row_fraction_for_80pct': needed / len(counts) if needed else None,
    }


def write_profiles(prefix, datasets, expected, head_rows):
    """Expected curves are ordered by true popularity; observed curves by counts."""
    summaries = {}
    with Path(str(prefix) + '.ranks.csv').open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['dataset', 'rank', 'row_address', 'count', 'cumulative_fraction',
                         'expected_probability', 'expected_cumulative_fraction'])
        for dataset, counts in datasets.items():
            summaries[dataset] = statistics(counts, head_rows)
            total, actual_cdf, expected_cdf, max_error = sum(counts), 0, 0., 0.
            ranking = sorted(range(len(counts)), key=lambda row: (-counts[row], row))
            for rank, row in enumerate(ranking, 1):
                actual_cdf += counts[row]
                probability = expected[dataset][rank - 1] if expected[dataset] is not None else None
                if probability is not None:
                    expected_cdf += probability
                actual_fraction = actual_cdf / total if total else None
                if total and probability is not None:
                    max_error = max(max_error, abs(actual_fraction - expected_cdf))
                writer.writerow([dataset, rank, EMBEDDING_BASE + row * 64, counts[row],
                                 actual_fraction, probability, expected_cdf if probability is not None else None])
            # Data-dependent rank ordering makes this descriptive, not a KS test.
            summaries[dataset]['max_rank_cdf_difference'] = max_error if total and expected[dataset] is not None else None
    return summaries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metrics', type=Path, required=True)
    parser.add_argument('--events', type=Path, required=True)
    parser.add_argument('--output-prefix', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=1_000_000)
    parser.add_argument('--seed', type=int, default=42)
    args = parser.parse_args()
    with args.metrics.open() as stream:
        metrics = next(csv.DictReader(stream))
    if metrics['workload'] != 'dlrm' or args.samples <= 0:
        parser.error('need DLRM metrics and positive sample count')
    rows = int(float(metrics['table_lines']))
    head = hot_count(rows, float(metrics['hot_fraction']))
    target = float(metrics['hot_read_fraction'])
    alpha, full_zipf = calibrated_zipf(rows, head, target)
    mixture = hot_cold_probabilities(rows, head, target, float(metrics['zipf_alpha']))
    observed = event_counts(args.events, rows, int(float(metrics['agents'])))
    datasets = {f'timed_{scope}_{population}': values for (scope, population), values in observed.items()}
    # Cache-filtered fills have no expected Zipf law in this profiler.
    expected = {name: None if name.endswith('data_fills') else mixture for name in datasets}
    synthetic = sample_zipf(full_zipf, args.samples, args.seed)
    fixed_head_successes = sum(synthetic[:head])
    permutation = list(range(rows))
    random.Random(int(metrics['layout_seed'])).shuffle(permutation)
    physical = [0] * rows
    for rank, row in enumerate(permutation):
        physical[row] = synthetic[rank]
    datasets['iid_full_zipf'] = physical
    expected['iid_full_zipf'] = full_zipf
    summaries = write_profiles(args.output_prefix, datasets, expected, head)
    for name in summaries:
        summaries[name]['cdf_reference'] = (None if name.endswith('data_fills') else
                                            'calibrated_full_zipf' if name == 'iid_full_zipf'
                                            else 'logical_hot_cold_mixture')
        summaries[name]['cdf_comparison_is_descriptive'] = True
    result = {
        'model': 'finite_zipf_calibrated_to_head_mass',
        'rows': rows, 'head_rows': head, 'head_fraction_after_rounding': head / rows,
        'target_head_mass': target, 'calibrated_alpha': alpha,
        'expected_zipf_head_mass': math.fsum(full_zipf[:head]),
        'iid_samples': args.samples, 'iid_seed': args.seed,
        'iid_fixed_true_head_mass': fixed_head_successes / args.samples,
        'iid_fixed_true_head_mass_wilson95': wilson95(fixed_head_successes, args.samples),
        'datasets': summaries,
        'notes': [
            'Timed counts use numeric addresses, accelerator reads and the full allocated table; labels are ignored.',
            'Logical lookups include data-cache hits. Data fills are a separately filtered population.',
            'Observed top-head mass selects rows on the same sample and has upward selection bias.',
            'Wilson interval applies only to the known fixed head in the independent iid sample.',
            'CDF differences compare sorted curves; they are not goodness-of-fit p-values.',
            'Synthetic full-Zipf sampling is standalone analysis; it is not input to the timed C++ simulator.',
            'Python layout shuffling is separate from the C++ permutation; both scatter ranks over the table.',
            '80%/5% is a configurable hypothesis, not a universal production property.',
        ],
    }
    Path(str(args.output_prefix) + '.summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'Calibrated full Zipf: N={rows}, head={head}, alpha={alpha:.9f}; '
          f'expected head mass={math.fsum(full_zipf[:head]):.6%}')
    print(f'Independent iid sample: fixed-head share={fixed_head_successes / args.samples:.6%}; '
          f'95% Wilson interval={result["iid_fixed_true_head_mass_wilson95"]}')
    for name, summary in summaries.items():
        mass = summary['empirical_top_configured_head_mass']
        print(f'{name}: n={summary["requests"]}, observed top-{head} mass='
              f'{mass:.6%}' if mass is not None else f'{name}: no requests')
    print(f'Wrote {args.output_prefix}.summary.json and .ranks.csv')


if __name__ == '__main__':
    main()
