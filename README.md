# DLRM-Perf: policy-neutral RadixTree / DLRM performance simulator

The refined simulator adds a **finite, banked, memory-backed directory**, bounded
metadata miss buffers, and **512-batch runs with 128 warm-up batches**. Under the
exposed default directory assumptions, selective bypass reduces post-warm-up DLRM
time **23.97%**, while the cache-fitting case becomes **17.23% slower** because
page recovery discards useful cached data.

The assumed metadata-miss penalty matters: on the same long DLRM trace, selective
bypass saves **8.15% with 0 ns extra miss latency**, **12.18% at 20 ns**, **23.97% at
80 ns**, and **35.55% at 160 ns**. These are conditional simulation results, not
measured GPU/CXL speedups. The added mechanisms make resource limits explicit;
the new default nanosecond values are not hardware calibration.

## Post-warm-up results

Each case runs 512 rounds and excludes the first 128 from measurement. Positive
percentages mean less time versus that workload's own always-coherent baseline;
negative values mean slower. Both workloads use identical hardware within a case.

| Case | Workload | Baseline measured µs | Selective time reduction | Naive coarse time reduction | 50% budget time reduction |
|---|---|---:|---:|---:|---:|
| long_default | radix | 94.858 | 0.00% | -1,182.51% | 0.00% |
| long_default | dlrm | 172.791 | 23.97% | -77.05% | 13.15% |
| long_fitting | radix | 94.858 | 0.00% | -1,182.51% | 0.00% |
| long_fitting | dlrm | 102.773 | -17.23% | -167.52% | 2.22% |
| long_exceeding | radix | 1,778.048 | 38.17% | 15.71% | 18.44% |
| long_exceeding | dlrm | 379.316 | 23.04% | -2.28% | 13.05% |
| long_bandwidth | radix | 7,244.040 | 22.12% | 20.45% | 10.81% |
| long_bandwidth | dlrm | 1,067.794 | 8.28% | 3.51% | 8.80% |

Default/fitting use a 64 KiB data cache/node; exceeding/bandwidth use 4 KiB. The
link is 32 GB/s except 2 GB/s for bandwidth-constrained runs. DLRM table sizes
are respectively 256 KiB, 32 KiB, 512 KiB and 512 KiB. Other common inputs are
3 accelerators + host, 8 outstanding requests/node, 200 ns accelerator compute,
150 ns data-memory/peer service, 64B lines and 4 KiB regions. Defaults for the new
home controller are 1,024 cached entries, four banks, 2 ns issue interval,
16 miss buffers, 20 ns tag lookup, 80 ns additional miss service and a separate
32 GB/s metadata backing channel.

The default controller has little bank queuing. Much of the larger benefit comes
from avoiding metadata-cache misses, not a saturated controller port. The explicit
one-bank controller stress case is reported separately. The model treats one
metadata entry as one 64B transfer/cache line; packing several entries together
could reduce misses and traffic and is currently unmodeled.

[Complete results and sensitivity controls](PERFORMANCE_RESULTS.md) · [Model and limitations](MODEL.md)

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
build/perf/architecture_perf --workload both --rounds 512 --warmup-rounds 128 \
    --csv metrics.csv --kinds kinds.csv --phases phases.csv
```

Use `--workload dlrm` or `--workload radix` for one workload. `--help` lists inputs.
`--directory-model 0` reproduces the previous model; `--warmup-rounds 0` measures
the complete run. A standalone build is available with `cmake -S perf -B build-perf`.

On Great Lakes, submit from this repository root:

```bash
sbatch perf/run_slurm.sh
```

Or run against an existing build:

```bash
bash perf/run_experiments.sh "$PWD/build/perf/architecture_perf"
python3 scripts/audit_perf.py results/dlrm_refined
```

The script writes **18 configurations** to `results/dlrm_refined/`: short and
post-warm-up paired regimes, the legacy reference, directory capacity/throughput
and miss-latency controls, and workload controls. The original
`results/dlrm_comparison/` remains separate. Generated results and builds are
Git-ignored; the Markdown tables are normal source files.

## What changed

- Directory probes use finite LRU metadata storage, address-hashed bank issue
  slots, bounded outstanding misses and a queued backing channel. Pending fills
  cannot be treated as immediate hits or evicted while incomplete.
- Dirty metadata eviction writes permission records to backing memory; it never
  fabricates data-cache invalidations. Bypass grants/recoveries use the same
  controller resources as coherent line permissions.
- Same-line and page dependencies wait for actual controller completion. Data,
  controller and link state persist across a synchronized warm-up boundary.
- CSVs expose whole-run and measured times, per-class/scoped cache statistics,
  metadata hits/misses/merges, bank/miss-buffer/capacity/backing waits, and metadata
  read/write traffic. Data traffic remains separate from directory backing traffic.

DLRM retains the hot Zipf-skewed shared embeddings with host writes, scattered cold
embeddings with rare updates, and private scratch reads/stores. Bottom MLP compute
can overlap gathers; top MLP waits for the results. Radix retains its lead/fork,
shared-prefix reads, private leaves and contended metadata. The same four scenarios
remain: coherent baseline, selective bypass, naive coarse regions and a 50%
KV/embedding event-overhead budget. No optimized predictor is implemented.

## Source map

| File | Role |
|---|---|
| `perf/model.hpp` | Configuration, requests, scenarios and metrics |
| `perf/directory.hpp`, `perf/directory.cpp` | Isolated home metadata cache, ports, miss buffers and backing resource |
| `perf/model.cpp` | Radix generation, warm-up split and common discrete-event scheduler |
| `perf/dlrm.cpp` | Seeded DLRM traces and paced host updates |
| `perf/profile.cpp` | Finite data LRUs, reference MESI events and page-recovery effects |
| `perf/scenarios.cpp` | Editable cost scenarios and trace fingerprint |
| `perf/main.cpp` | CLI and scoped CSV exports |
| `perf/*tests.cpp` | Analytic timing, caches, queue, overlap, recovery and warm-up tests |
| `scripts/audit_perf.py` | CSV accounting, resource bounds and identical-hardware checks |

The legacy `radix_coherence` executable is preserved independently. The performance
frontend only uses its baseline directory path to obtain data-cache/coherence events.

## Validation and limits

All **14 CTest targets** passed on Great Lakes (GCC 8.5.0, job **64801017**, gl3018)
and locally with AddressSanitizer/UndefinedBehaviorSanitizer. The three extra
miss-latency controls passed in job **64801186**, gl3230. Both jobs completed with
exit **0:0**. **7,018 CSV checks** passed across all 18 configurations.

The main remaining limitation is that functional data-cache/coherence profiling
still uses a fixed logical interleaving; timing does not reorder those reference
events. The new directory cache/queues are timed, but this is not a cycle-accurate
protocol implementation. Link request/response timing is lumped, directory backing
and data memory are separate resources, and directory layout/costs need calibration.
Post-warm-up does not guarantee a stationary working set. Absolute times across
DLRM and Radix do different work. See `MODEL.md` for the full interpretation.
