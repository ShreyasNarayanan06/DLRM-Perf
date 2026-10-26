#!/usr/bin/env python3
"""Optional matplotlib figure from profile_embedding_skew.py exports."""
import argparse
import csv
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('prefix', type=Path)
args = parser.parse_args()
summary = json.loads(Path(str(args.prefix) + '.summary.json').read_text())
curves = {}
with Path(str(args.prefix) + '.ranks.csv').open() as stream:
    for row in csv.DictReader(stream):
        curves.setdefault(row['dataset'], []).append(row)

plt.rcParams.update({'font.size': 10, 'axes.spines.top': False, 'axes.spines.right': False})
fig, axes = plt.subplots(1, 2, figsize=(12, 4.8), constrained_layout=True)
logical = curves['timed_measured_logical_lookups']
fill = curves['timed_measured_data_fills']
zipf = curves['iid_full_zipf']
x = [100 * int(row['rank']) / summary['rows'] for row in logical]
ax = axes[0]
for rows, label, color, style in [
    (logical, 'Timed logical lookups', '#176B9B', '-'),
    (zipf, 'Independent full-Zipf sample', '#8A48B1', '--'),
    (fill, 'Timed data-cache fills', '#D37914', '-'),
]:
    if rows[0]['cumulative_fraction']:
        ax.plot(x, [100 * float(r['cumulative_fraction']) for r in rows], label=label,
                color=color, linestyle=style, linewidth=2)
ax.axvline(100 * summary['head_fraction_after_rounding'], color='#777777', linestyle=':', linewidth=1)
ax.axhline(100 * summary['target_head_mass'], color='#999999', linestyle=':', linewidth=1)
ax.set(xscale='log', xlim=(x[0], 100), ylim=(0, 101), xlabel='Hottest rows (% of all allocated rows)',
       ylabel='Cumulative requests (%)', title='Observed concentration after cache filtering')
ax.legend(loc='lower right', fontsize=9)
ax.grid(alpha=.15)

ax = axes[1]
ranks = [int(r['rank']) for r in logical]
ax.plot(ranks, [float(r['expected_probability']) for r in logical],
        color='#176B9B', label='Current hot-set Zipf + uniform tail', linewidth=2)
ax.plot(ranks, [float(r['expected_probability']) for r in zipf], color='#8A48B1',
        label=f'Full-table Zipf, alpha={summary["calibrated_alpha"]:.4f}', linewidth=2)
ax.axvline(summary['head_rows'], color='#777777', linestyle=':', linewidth=1)
ax.set(xscale='log', yscale='log', xlabel='True popularity rank', ylabel='Probability per row',
       title=f'Two laws: {100 * summary["target_head_mass"]:g}% in {100 * summary["head_fraction_after_rounding"]:.2f}% of rows')
ax.legend(loc='upper right', fontsize=9)
ax.grid(alpha=.15)
fig.suptitle(f'Synthetic DLRM embedding access profile — {summary["rows"]:,} rows', fontsize=14)
fig.savefig(str(args.prefix) + '.png', dpi=180)
fig.savefig(str(args.prefix) + '.svg')
print(f'Wrote {args.prefix}.png and .svg')
