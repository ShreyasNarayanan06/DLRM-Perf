#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
simulator="${1:-build/radix_coherence}"
mkdir -p results/sweep
for threshold in 1 4 8 16 32 64; do
    "$simulator" --stress --private-threshold "$threshold" \
        --csv "results/sweep/private-${threshold}.csv" > "results/sweep/private-${threshold}.txt"
done
for threshold in 4 8 16 32 64; do
    "$simulator" --stress --freeze-threshold "$threshold" \
        --csv "results/sweep/freeze-${threshold}.csv" > "results/sweep/freeze-${threshold}.txt"
done
# Short phases make barrier/flush costs visible and can erase the benefit.
"$simulator" --stress --episodes 1 --lead-writes 8 --review-reads 2 --rounds 8 \
    --private-threshold 1 --freeze-threshold 1 --cooldown 0 \
    --csv results/sweep/short-phases.csv > results/sweep/short-phases.txt
echo "Wrote private/freeze threshold sweeps and short-phase experiment to results/sweep"
