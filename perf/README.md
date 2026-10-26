# Policy-neutral RadixTree / DLRM performance simulator

Build this directory alone, or build the whole repository. Only the baseline path of
`src/region.cpp` supplies directory events to `architecture_perf`; the legacy
`radix_coherence` predictor and oracle executable is independent of this frontend.

```bash
cmake -S perf -B build-perf -DCMAKE_BUILD_TYPE=Release
cmake --build build-perf -j4
ctest --test-dir build-perf --output-on-failure
build-perf/architecture_perf --workload both --csv metrics.csv --kinds kinds.csv --phases phases.csv
bash perf/run_experiments.sh
python3 scripts/audit_perf.py results/dlrm_refined
```

From the repository root on Great Lakes:

```bash
sbatch perf/run_slurm.sh
```

The script builds and tests on a compute node, then runs short and post-warm-up
paired regimes, the legacy reference, and directory controls. Results go to
`results/dlrm_refined/`.
See the root README for results and `MODEL.md` for equations and limitations.
`architecture_perf --help` lists every parameter. CSV output directories must
already exist. Seeds, all numeric configuration, logical trace hashes, raw
reference events and retained event budgets are exported.
