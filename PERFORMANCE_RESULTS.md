# Refined performance results

Main validation: Great Lakes job **64801017**, **gl3018**, GCC **8.5.0**, C++17
Release, COMPLETED **0:0**, elapsed 1:36. Additional miss-latency controls: job
**64801186**, **gl3230**, COMPLETED **0:0**, elapsed 0:33. These job wall times
include simulator/test overhead; the tables report simulated application time.

## Added mechanisms and assumptions

A finite home metadata LRU, bank issue queues, bounded metadata miss buffers,
merged pending fills, dirty metadata writeback and a separate backing-bandwidth
resource replace the old unlimited fixed-latency directory assumption. Page grants
and recovery/publication operations consume the same resources. Evicting cached
metadata preserves permissions in backing memory and does not invalidate data.

The added defaults are assumptions: 1,024 entries, four banks, 2 ns bank issue,
16 miss buffers, 20 ns lookup, 80 ns additional miss service and 32 GB/s backing
bandwidth. One cached metadata entry/fetch is a 64B transaction; packing multiple
records into a metadata line is not modeled and could reduce modeled overhead.
These values must be calibrated before making hardware speedup claims.

## Post-warm-up paired comparison

All rows here run **512 rounds**, with the first **128 excluded** from measurement.
Data LRUs and directory state remain warm. Streams synchronize and drain at the
boundary; lead/fork setup and completion are excluded from the Radix measurement.
Each percentage is `100 * (baseline_measured_time - candidate_measured_time) /
baseline_measured_time`. Negative values are regressions.

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

| Case | Cache/node | CXL link | DLRM table | Radix prefix / leaf lines |
|---|---:|---:|---:|---:|
| long_default | 64 KiB | 32 GB/s | 4096 lines = 256 KiB | 64 / 128 |
| long_fitting | 64 KiB | 32 GB/s | 512 lines = 32 KiB | 64 / 128 |
| long_exceeding | 4 KiB | 32 GB/s | 8192 lines = 512 KiB | 1024 / 1024 |
| long_bandwidth | 4 KiB | 2 GB/s | 8192 lines = 512 KiB | 1024 / 1024 |

Both workloads share all hardware fields, including the directory, within each case.
Common inputs: 3 accelerators + host, MLP=8/node, accelerator compute=200 ns,
host compute=50 ns, data memory/peer=150 ns, hit=2 ns, 64B data lines, 4 KiB regions,
snoop=20 ns/16B, invalidation=12 ns/16B, directory messages=16B, dirty data
writeback=10 ns+64B, barrier=40 ns and explicit recovery=200 ns+32B. DLRM has an
80/20 hot/cold gather mix, 5% logical hot footprint, Zipf alpha=1.1, seed=42,
2 hot writes every 4 batches and 1 cold write every 32. The 50% row has a separate
1% expected residual-recovery budget. Absolute runtimes across workloads are not
an equal-work throughput comparison.

## Short runs and legacy reference

These cases measure the complete default 64-round run; no warm-up is excluded.
The legacy toggle reproduces the previous numbers exactly. Enabling the directory
adds assumptions and changes page completion scheduling, so the difference cannot
be interpreted as measured hardware overhead.

| Case | Workload | Baseline measured µs | Selective time reduction | Naive coarse time reduction | 50% budget time reduction |
|---|---|---:|---:|---:|---:|
| legacy_default | radix | 38.447 | 1.44% | -363.00% | -1.34% |
| legacy_default | dlrm | 39.153 | 5.48% | -49.16% | 4.55% |
| default | radix | 46.367 | 17.62% | -368.31% | 7.35% |
| default | dlrm | 51.053 | 23.29% | -25.51% | 15.47% |
| cache_fitting | radix | 46.367 | 17.62% | -368.31% | 7.35% |
| cache_fitting | dlrm | 44.154 | 24.43% | -33.33% | 13.83% |
| cache_exceeding | radix | 539.529 | 29.84% | 17.28% | 14.40% |
| cache_exceeding | dlrm | 64.962 | 18.16% | -6.08% | 13.90% |
| bandwidth_constrained | radix | 1,487.572 | 21.43% | 19.92% | 10.51% |
| bandwidth_constrained | dlrm | 179.496 | 3.71% | 0.65% | 8.71% |

## Directory-cost sensitivity on one long DLRM trace

Only the indicated directory parameter changes. All compare 384 post-warm-up
batches against the corresponding coherent baseline.

| Case | Workload | Baseline measured µs | Selective time reduction | Naive coarse time reduction | 50% budget time reduction |
|---|---|---:|---:|---:|---:|
| directory_miss_zero | dlrm | 142.752 | 8.15% | -114.31% | 5.25% |
| directory_miss_20 | dlrm | 149.373 | 12.18% | -104.81% | 7.24% |
| long_default | dlrm | 172.791 | 23.97% | -77.05% | 13.15% |
| directory_miss_160 | dlrm | 204.125 | 35.55% | -49.87% | 18.85% |
| directory_wide | dlrm | 158.214 | 16.96% | -93.37% | 9.82% |
| directory_small | dlrm | 177.980 | 24.01% | -71.89% | 14.37% |
| directory_queue_stress | dlrm | 214.275 | 38.03% | -42.91% | 22.95% |

- Miss-latency cases use **0, 20, 80 and 160 ns** additional backing service.
  The zero case still charges tag lookup, metadata transfer, dirty replacement,
  bank queuing and bounded buffers. It is an ablation, not free coherence.
- `directory_wide` has 65,536 cached metadata entries; `directory_small` has 64.
  All other parameters remain unchanged.
- `directory_queue_stress` has **one bank, 20 ns issue interval and four miss
  buffers**. This is an explicitly stressed resource, not a production-hardware claim.

Thus the long default selective benefit ranges from **8.15% to 35.55%** over the
miss-latency sweep. The result at 80 ns (**23.97%**) is conditional on that chosen
cost. A real directory with greater coverage, record packing, inexpensive misses,
or a different topology could show much smaller gains. The simulator now exposes
this sensitivity rather than establishing a universal benefit.

## What is driving the result

Measured-window DLRM controller and data-cache events:

| Case / scenario | Requests | Data cache hits | Data capacity evictions | Home probes | Home metadata misses | Aggregate bank wait ns | Aggregate miss-buffer wait ns | Aggregate backing wait ns |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| long_default / baseline | 48,588 | 41,702 | 6,266 | 7,034 | 5,266 | 0.00 | 158.50 | 8,742.25 |
| long_default / selective | 48,588 | 41,732 | 5,868 | 980 | 12 | 1.75 | 0.00 | 0.00 |
| long_fitting / baseline | 48,588 | 47,731 | 0 | 1,035 | 5 | 1.00 | 0.00 | 0.00 |
| long_fitting / selective | 48,588 | 46,113 | 0 | 770 | 12 | 2.50 | 0.00 | 0.00 |
| long_exceeding / baseline | 48,588 | 26,922 | 21,260 | 21,796 | 10,636 | 7.50 | 0.00 | 5,482.50 |
| long_exceeding / selective | 48,588 | 26,923 | 21,251 | 12,144 | 13 | 4.00 | 0.00 | 0.00 |
| directory_queue_stress / baseline | 48,588 | 41,702 | 6,266 | 7,034 | 5,266 | 484,576.25 | 49,741.75 | 0.00 |
| directory_queue_stress / selective | 48,588 | 41,732 | 5,868 | 980 | 12 | 10,042.25 | 0.00 | 0.00 |

The long default now has **6,266 real data-capacity evictions** during measurement,
versus zero in the original short default. Its coherent baseline performs **7,034
home probes and 5,266 metadata misses**; selective bypass performs **980 probes
and 12 misses**, including page-control operations. Default bank queuing is tiny:
most of the larger gain comes from avoided metadata misses. Queue pressure becomes
substantial only in the explicitly stressed controller case.

The fitting baseline has only **5 measured metadata misses**. Cold-page write
recoveries in selective bypass discard cached embeddings, increasing data refills
and making this case **17.23% slower** despite fewer probes. Sparsity alone therefore
does not determine the sign or magnitude of the benefit.

Wait/service counters are aggregates across requests and are not critical-path
time fractions. `aggregate_event_latency_ns` retains the reference event budget;
`directory_service_ns` includes lookup service again, plus directory miss effects.
Those overlapping diagnostics must not be summed to estimate execution time.

## Workload controls

| Case | Workload | Baseline measured µs | Selective time reduction | Naive coarse time reduction | 50% budget time reduction |
|---|---|---:|---:|---:|---:|
| long_no_cold_writes | dlrm | 173.402 | 25.06% | -76.51% | 13.45% |
| long_hot_updates | dlrm | 197.422 | 20.08% | -245.47% | 12.16% |
| long_seed43 | dlrm | 171.829 | 24.06% | -79.09% | 13.37% |

These respectively disable rare cold writes, increase hot writes to every batch,
and change the PRNG seed to 43. Other inputs remain unchanged. Per-node PRNGs keep
accelerator addresses stable when host cadence changes.

## Validation and outputs

- **14/14 CTest targets** passed on the cluster and under local ASan/UBSan.
  Unit executables include 12 previous perf checks, 11 DLRM checks and seven new
  directory/warm-up checks, plus the independent legacy coherence tests.
- Tests cover exact analytical miss/hit/dirty replacement timing, finite queues,
  pending-fill merging, no early use of dependent data, link eligibility, retained
  budgets, cache persistence across warm-up and exact legacy timing reproduction.
- **7,018 CSV checks** passed over **18 cases**. They verify access partitions,
  per-class/scoped aggregation, metadata/data bytes, MLP/entry/miss-buffer bounds,
  backing bandwidth budgets, phase windows, reduction arithmetic, trace identity
  within scenarios and equal hardware assumptions across workload pairs.

Reproduce with:

```bash
bash perf/run_experiments.sh "$PWD/build/perf/architecture_perf"
python3 scripts/audit_perf.py results/dlrm_refined
```

Outputs are `results/dlrm_refined/{case}.csv`, `_kinds.csv`, `_phases.csv` and `.txt`.
Totals retain whole-run semantics; `measured_*` columns exclude warm-up.
`_kinds.csv` has `scope=all` and `scope=measured`; `_phases.csv` marks measured phases.
Raw metadata byte counters are whole transfers. The 50% hypothetical row scales
service/bandwidth budgets while retaining logical probes and full-sized tags.
Backing metadata uses its own modeled channel and is not added to CXL data bytes.

Functional data-cache/coherence events still use a fixed logical interleaving;
controller timing does not reorder that reference trace. Detailed protocol packet
round trips, metadata-record packing, shared data/metadata DRAM contention and
hardware calibration remain outside this proof of concept. [MODEL.md](MODEL.md)
documents these limits and all exposed inputs. The earlier generated results are
preserved under `results/dlrm_comparison/` on the cluster.
