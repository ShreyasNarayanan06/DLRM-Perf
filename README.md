# DLRM / RadixTree baseline memory profiler

The timed baseline now uses **one mixed embedding address space** and updates
finite caches and MESI permissions during execution. The old counterfactual
scenarios have been removed from `perf/`. Its outputs describe the workload at
64B, 256B, 1KiB and 4KiB so a coherence policy can be designed from observations.

**Access-skew check:** the hottest 205 of 4,096 rows (about 5%) account for
**79.96% of measured logical embedding reads**. A standalone full-table Zipf
sampler can also be calibrated to an exact 80% target.
[Distribution, sample validation and reproduction commands](SKEW_PROFILE.md).

## First results: mixed DLRM embeddings

Default: 3 accelerators + host, 512 batches, 128 warm-up batches, 64KiB data
cache/node, 256KiB embedding table, request seed 42, layout seed 7.

| Analysis block size | Touched blocks | One historical accessor | Mixed written/read-only lines | Metadata misses on zero-write blocks |
|---|---:|---:|---:|---:|
| 64 B | 3,485 | 42.93% | 0.00% | 98.51% |
| 256 B | 1,024 | 1.86% | 6.93% | 93.39% |
| 1,024 B | 256 | 0.00% | 25.00% | 74.91% |
| 4,096 B | 64 | 0.00% | 68.75% | 31.53% |

Percentages cover the **measured embedding window**. Accessor and mixed-block
percentages count demand-touched blocks; miss attribution counts metadata misses
from both demand permissions and data-cache eviction notices. Zero-write means
zero observed writes in this window. Full-run write history is exported too.
A 64B block cannot be spatially mixed by definition.

At 4KiB, **44 of 64** touched pages mix written lines with lines that were only
read; at 256B, **71 of 1,024** blocks do. The proportion of metadata misses on
zero-write blocks falls from **98.51% at 64B** to **31.53% at 4KiB**. This exposes
the cost of grouping unlike rows together, but does not establish an optimal
policy or a speedup. The four sizes group one baseline's observations; they are
not four simulated coherence protocols.

Great Lakes job **64818048** completed on **gl3039** with exit **0:0**
(GCC 8.5.0, C++17 Release). All **11 CTest targets** and the **528,210-check**
CSV/replay audit passed. All 139 output files match the local Release results
byte for byte; ASan/UBSan validation also passed locally.

[Full baseline results](PERFORMANCE_RESULTS.md) · [Model, definitions and limits](MODEL.md)

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
mkdir -p results/blind
build/perf/architecture_perf --workload both --rounds 512 --warmup-rounds 128 \
    --events 1 --output-prefix results/blind/default
python3 scripts/audit_perf.py results/blind
```

`--workload dlrm` is the default; `radix` and `both` use the same hardware inputs.
Use `--help` for all inputs. `--layout-seed` controls which table rows are hot;
`--seed` controls requests. Set `--events 1` for an independently replayable log.
Standalone build: `cmake -S perf -B build-perf -DCMAKE_BUILD_TYPE=Release`.

On Great Lakes, submit from the repository root:

```bash
sbatch perf/run_slurm.sh
```

To reproduce the full **14 baseline runs across 10 configurations**:

```bash
bash perf/run_experiments.sh "$PWD/build/perf/architecture_perf"
python3 scripts/audit_perf.py results/blind
```

The batch script builds, runs all tests, generates the experiments and audits the
CSVs on a compute node. Results and builds are Git-ignored. Tables and model
explanations in Markdown are tracked source files.

## What the profiler measures

- **Historical accessor counts:** blocks accessed by 1 through N+1 nodes,
  including host node 0. These are window-wide unions, not simultaneous sharers.
- **Read runs / write intervals:** exact consecutive-read counts and logarithmic
  duration bins, with complete, leading, trailing and entirely censored samples.
- **Spatial purity:** demand-touched blocks containing written lines, read-only
  lines or both, independently at all four sizes.
- **Directory miss attribution:** metadata misses on blocks with zero versus
  positive writes, separated into demand/victim probes and measured/full history.

All four are keyed by numeric addresses. `.kinds.csv` retains diagnostic access
labels, but labels never choose caching, timing or histogram classification.
`.blocks.csv` exposes underlying per-address counts. `.events.csv` records actual
issue/admission/completion order, read versions and exact victim addresses.

## Timing and memory model

The scheduler admits a request using the current cache state and commits fills,
writes and invalidations at actual completion. Pending fills consume finite cache
capacity and cannot be read early. Timed queues therefore change the functional
interleaving rather than replaying precomputed round-robin coherence events.

Both workloads share finite fully associative data LRUs, bounded per-node memory
parallelism, partial compute-memory overlap, a queued CXL link and a finite banked
metadata cache with miss buffers and backing traffic. Host writes are concurrent
with DLRM gathers; hot and cold rows are shuffled through one contiguous table.
Private scratch remains a real per-accelerator allocation. These are synthetic
MESI-style baseline measurements, not calibrated GPU/CXL timings.

## Source map

| File | Responsibility |
|---|---|
| `perf/model.hpp` | Small configuration, request, transaction and metrics types |
| `perf/workload.cpp`, `perf/dlrm.cpp` | Radix/DLRM generation and warm-up phases |
| `perf/profile.hpp`, `perf/profile.cpp` | Incremental data caches, permissions and versions |
| `perf/directory.hpp`, `perf/directory.cpp` | Metadata LRU, banks, miss buffers and backing channel |
| `perf/model.cpp` | Event scheduling, bounded overlap and link timing |
| `perf/histograms.hpp`, `perf/histograms.cpp` | Address-only distributions at four sizes |
| `perf/report.cpp`, `perf/main.cpp` | CSV exports and CLI |
| `perf/tests.cpp` | Ten functional, timing, layout and distribution checks |
| `scripts/audit_perf.py` | Accounting audits and independent event replay |

The original `radix_coherence` predictor/state-machine executable is preserved
independently. The performance library does not link to it.

## Validation

Local ASan/UBSan: **11/11 CTest targets passed**, including seven legacy targets
and four baseline targets. The experiment audit passed **528,210 checks across
14 runs** and independently replayed **131,660 timed requests**. Tests include
latency-driven changes to observed data versions and coherence events, reserved
fill capacity, dirty ownership transfer, warm-up persistence, exact toy histograms
and invariance to diagnostic label changes.

The distributions suggest candidate block sizes and phase thresholds. Choosing a
policy still requires correctness rules, transition/recovery costs, predictor
errors and validation on real workloads. [MODEL.md](MODEL.md) specifies denominators,
censoring and the remaining timing approximations.
