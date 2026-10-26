# Timed baseline memory profiler

`architecture_perf` runs DLRM, RadixTree or both with incremental finite data caches,
MESI-style permissions, a finite directory and a queued CXL link. There is no
static round-robin coherence prepass. The performance library is independent of
the original `radix_coherence` predictor/state-machine executable.

From the repository root:

```bash
cmake -S perf -B build-perf -DCMAKE_BUILD_TYPE=Release
cmake --build build-perf -j4
ctest --test-dir build-perf --output-on-failure
mkdir -p results/example
build-perf/architecture_perf --workload both --rounds 512 --warmup-rounds 128 \
    --events 1 --output-prefix results/example/default
python3 scripts/audit_perf.py results/example
```

`--output-prefix results/example/default` writes separate `default.dlrm.*` and
`default.radix.*` exports. Parent directories must exist. `--help` lists hardware,
generator and timing inputs. `--seed` selects requests and `--layout-seed` selects
the placement of hot/cold ranks within one contiguous DLRM table.

Reproduce all 14 baseline runs:

```bash
bash perf/run_experiments.sh "$PWD/build-perf/architecture_perf"
python3 scripts/audit_perf.py results/blind
```

On Great Lakes, the repository-root batch script builds the whole project,
runs 11 CTest targets, executes the experiments and audits the results:

```bash
sbatch perf/run_slurm.sh
```

Address distributions are exported at 64B, 256B, 1KiB and 4KiB: historical distinct
accessors, exact read runs, time between writes, spatial purity and directory-miss
write attribution. They use numeric addresses rather than hot/cold labels.
Scopes distinguish full/measurement windows and all-memory/embedding-table views.
`--events 1` enables a replayable timed trace. The audit script independently
reconstructs distributions and committed data versions from those events.

See [../README.md](../README.md) for results, [../MODEL.md](../MODEL.md) for exact
sample definitions and [../PERFORMANCE_RESULTS.md](../PERFORMANCE_RESULTS.md) for
regime tables and validation. The active frontend has a single coherent baseline;
previous scenario flags and CSV flags have been replaced by `--output-prefix`.

Address-only row popularity and a separate calibrated full-Zipf sample:

```bash
python3 scripts/profile_embedding_skew.py \
    --metrics results/blind/default.dlrm.metrics.csv \
    --events results/blind/default.dlrm.events.csv \
    --output-prefix results/blind/skew
```

See [../SKEW_PROFILE.md](../SKEW_PROFILE.md) for the 80/5 check and its limits.
