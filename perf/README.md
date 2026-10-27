# Performance simulation

The simulator has two uses:

1. **Compare ways to keep shared embedding copies current.** Run with
   `--management compare` for software-managed, always-hardware and dynamic modes.
2. **Profile the original hardware baseline.** Omit `--management` to retain the
   prior address distributions and output format.

From the repository root:

```bash
cmake -S perf -B build-perf -DCMAKE_BUILD_TYPE=Release
cmake --build build-perf -j4
ctest --test-dir build-perf --output-on-failure
mkdir -p results/management
build-perf/architecture_perf --management compare --rounds 512 --warmup-rounds 128 \
    --events 1 --output-prefix results/management/example
bash perf/run_comparisons.sh "$PWD/build-perf/architecture_perf"
python3 scripts/audit_comparison.py results/management
```

For Great Lakes, use `sbatch perf/run_comparison_slurm.sh` from the repository
root. The script builds the full project, tests, runs all comparisons and audits.
`--help` lists inputs; `--software-ns`, `--region-bytes`, `--region-entries` and
`--transition-ns` are useful controls. These are assumptions to test, not tuned
values established by hardware measurements.

The original profiler is still available:

```bash
bash perf/run_experiments.sh "$PWD/build-perf/architecture_perf"
python3 scripts/audit_perf.py results/blind
```

`--workload dlrm|radix|both` uses a shared event scheduler and hardware inputs.
Requests update cache/copy state during execution. There is no static round-robin
coherence prepass. Hot/cold rows share a shuffled contiguous DLRM table, and all
address histograms ignore diagnostic labels.

[Plain-English project guide](../README.md) · [Comparison results](../COMPARISON_RESULTS.md) ·
[Comparison details](../COMPARISON_MODEL.md) · [Original profiler details](../MODEL.md)

[Access-skew analysis](../SKEW_PROFILE.md) includes the standalone Python
`profile_embedding_skew.py` tool. It does not replace the timed workload generator.
