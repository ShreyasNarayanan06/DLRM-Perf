# Timed baseline profiling results

Great Lakes job **64818048** completed on **gl3039** with exit **0:0**
(GCC 8.5.0, C++17 Release). All **11 CTest targets** and the **528,210-check**
CSV/replay audit passed. All 139 output files match the local Release results
byte for byte; ASan/UBSan validation also passed locally.

The active `perf/` frontend contains one always-coherent MESI-style baseline.
The earlier selective/naive/50%-reduction tables have been replaced; they do not
apply to this mixed-layout, timing-driven model. No speedup is reported here.

## Row-popularity validation

An address-only profile of the measured DLRM logical reads places **79.9642%**
of 36,864 lookups in the empirically hottest 205/4,096 rows. Cache-filtered
data fills have a much flatter observed distribution. A separate calibrated
full-Zipf sample reproduces the target within sampling uncertainty.
[SKEW_PROFILE.md](SKEW_PROFILE.md) gives populations, confidence bounds and limits;
this additional analysis does not change the baseline execution-time tables.

## Default embedding distributions

| Analysis block size | Demand-touched blocks | One historical accessor | Mixed written/read-only lines | Metadata misses on zero-write blocks |
|---|---:|---:|---:|---:|
| 64 B | 3,485 | 42.93% | 0.00% | 98.51% |
| 256 B | 1,024 | 1.86% | 6.93% | 93.39% |
| 1,024 B | 256 | 0.00% | 25.00% | 74.91% |
| 4,096 B | 64 | 0.00% | 68.75% | 31.53% |

These rows use `scope=measured`, `space=embeddings`, and for miss attribution
`write_window=scope`, `lookup_origin=all`. The denominator for the last column is
**10,106 measured metadata misses** at every block size: **4,910 demand** and
**5,196 eviction-notice** misses. The address grouping changes the final write
classification, not the number of baseline misses. At 4KiB, 3,186 misses are on
20 zero-write pages and 6,920 are on 44 pages with writes. The old 5,266-miss result
used segregated layout, fixed functional order and different eviction bookkeeping;
it is not a directly comparable baseline.

Zero-write classification is computed after the observation window. A future
write within that window reclassifies earlier misses on that block. For example,
using the **whole run** to classify measured misses reduces the 4KiB zero-write
fraction from **31.53% to 26.73%**. Neither classification guarantees future safety.

At 64B, historical node-count percentages are **42.93% / 39.40% / 15.93% / 1.75%**
for 1 / 2 / 3 / 4 distinct accessors. At 4KiB, **31.25%** of pages have three
accessors and **68.75%** have four. Host node 0 is included. These are unions over
384 measured batches, not simultaneous MESI sharer counts.

## Read runs and write intervals

| Block size | Complete intervals | Complete median reads | Complete P90 reads | Left censored | Right censored | Both censored |
|---|---:|---:|---:|---:|---:|---:|
| 64 B | 131 | 73 | 248 | 73 | 73 | 3412 |
| 256 B | 133 | 78 | 252 | 71 | 71 | 953 |
| 1,024 B | 140 | 75 | 278 | 64 | 64 | 192 |
| 4,096 B | 160 | 85 | 294 | 44 | 44 | 20 |

A complete interval is bounded by two observed writes to that block. Counts
include zero-read intervals. The median/P90 use the nearest-rank complete-run
histogram, and must not be interpreted as phase lengths for all blocks. With no
observed write, a block contributes a both-censored run. Leading/trailing samples
also remain separate, avoiding survivor bias from dropping long read-only runs.

`.read_runs.csv` exports exact read-count frequencies; `.write_intervals.csv`
exports admission-time duration buckets `[2^k, 2^(k+1))` ns plus zero. Censored
values are observed lower bounds. Event logs include a unique admission order
to resolve same-time requests on different lines within a larger analysis block.

## Baseline timing and hardware regimes

All long runs generate 512 batches and exclude the first 128 from measurement.
Caches/controller state persist across a drained, synchronized boundary. Radix
lead/setup and final completion are also excluded. Times are simulated application
times in microseconds, not simulator wall time.

| Configuration | Workload | Measured µs | Requests | Data-cache hits | Metadata misses |
|---|---|---:|---:|---:|---:|
| default | dlrm | 220.964 | 48,588 | 41,709 | 10,106 |
| default | radix | 334.130 | 49,152 | 47,616 | 0 |
| cache_fitting | dlrm | 125.944 | 48,588 | 47,741 | 5 |
| cache_fitting | radix | 334.130 | 49,152 | 47,616 | 0 |
| cache_exceeding | dlrm | 415.567 | 48,588 | 26,768 | 10,625 |
| cache_exceeding | radix | 1887.053 | 159,744 | 7,669 | 51,456 |
| slow_link | dlrm | 372.236 | 48,588 | 41,713 | 10,109 |
| slow_link | radix | 460.850 | 49,152 | 47,616 | 0 |

| Configuration | Cache/node | CXL bandwidth | DLRM table | Radix prefix / leaf lines |
|---|---:|---:|---:|---:|
| default | 64KiB | 32GB/s | 4,096 lines / 256KiB | 64 / 128 |
| cache_fitting | 64KiB | 32GB/s | 512 lines / 32KiB | 64 / 128 |
| cache_exceeding | 4KiB | 32GB/s | 8,192 lines / 512KiB | 1,024 / 1,024 |
| slow_link | 64KiB | 2GB/s | 4,096 lines / 256KiB | 64 / 128 |

`slow_link` deliberately retains the default program addresses and changes only
bandwidth. Cache-exceeding Radix also uses 512 lead steps and 128 prefix reads per
round. Cache-fitting Radix is unchanged because that knob only changes DLRM's
table. Workloads perform different work; absolute DLRM/Radix times are not an
equal-work throughput comparison.

Common inputs: 3 accelerators + host, 8 outstanding requests/node, 200ns accelerator
compute, 50ns host compute, 2ns cache hits, 150ns data/peer service, 20ns snoops,
12ns invalidations and 10ns dirty-writeback service. DLRM bottom MLP is 40% of
compute; top MLP joins bottom compute with gathers before scratch accesses.
Next-step overlap starts after half the current memory batch completes.

The home metadata cache has 1,024 entries, four banks, 2ns issue spacing, 16 miss
buffers, 20ns tag lookup, 80ns extra backing service and a separate 32GB/s backing
channel. Each entry/transfer is 64B. Metadata-record packing is not modeled.
Demand controls are 16B, eviction notices 8B, snoop/invalidation controls 16B,
with 64B fills/writebacks on one FIFO bidirectional CXL resource. These costs are
exposed assumptions, not measurements of a particular CXL implementation.

DLRM defaults: 5% randomly placed hot rows, 80% hot reads with Zipf alpha 1.1,
32 gathers, 2 scratch reads and 8 scratch stores per accelerator batch. Host
updates are two hot stores every four batches and one cold store every 32.
Scratch holds 128 lines/node; four logical stores reuse each line. Independent
per-node request streams preserve accelerator addresses when host cadence changes.

## Timing, layout and update controls

| DLRM configuration | Measured µs | Metadata misses | Mixed 4KiB blocks | Misses on zero-write 4KiB blocks |
|---|---:|---:|---:|---:|
| default | 220.964 | 10,106 | 68.75% | 31.53% |
| fast_memory | 181.881 | 10,108 | 68.75% | 31.54% |
| slow_link | 372.236 | 10,109 | 68.75% | 31.49% |
| layout_seed8 | 220.705 | 10,104 | 70.31% | 29.64% |
| seed43 | 217.827 | 9,877 | 73.44% | 25.93% |
| no_host_writes | 205.740 | 9,996 | 0.00% | 100.00% |
| frequent_updates | 260.714 | 10,291 | 95.31% | 4.66% |

- `fast_memory`: data/peer latency is 75ns instead of 150ns.
- `slow_link`: CXL bandwidth is 2GB/s instead of 32GB/s.
- `layout_seed8`: only the permutation of hot/cold rows changes.
- `seed43`: only the request seed changes.
- `no_host_writes`: hot/cold update cadence is disabled.
- `frequent_updates`: hot updates occur every batch; cold cadence is unchanged.

Default versus slow-link data-cache hits change **41,709 -> 41,713**, metadata
misses **10,106 -> 10,109**, snoops **1,696 -> 1,701**, and invalidations
**561 -> 562**, despite identical program address fingerprints. Timing therefore
feeds back into functional memory events. Historical accessor/purity unions stay
the same for these two cases because their complete demand sets are the same.
A focused unit test also changes only memory latency and checks a different read
version and dirty-writeback count under the resulting execution order.

In the cache-fitting case there are only **five measured metadata misses**;
percentage attribution of those five should not be generalized. The no-host-write
control has zero mixed embedding blocks and all embedding misses are on observed
zero-write blocks, providing a useful histogram check. Increased hot updates raise
mixed-page incidence to **95.31%** under these inputs.

## Validation and raw exports

- Local ASan/UBSan: **11/11 CTest targets passed**.
- Standalone baseline build: **4/4 CTest targets passed**; its unit target exercises
  ten functional/timing/distribution test functions.
- **528,210 audit checks across 14 runs** passed, including independent replay
  of **131,660 captured timed requests** (default DLRM/Radix and short DLRM).
- Replay checks committed data versions, raw block counts, node unions, spatial
  classes, read runs, interval bins and exact demand/victim miss attribution.
  Other checks cover bytes, scoped totals, resource bounds and phase accounting.
- Layout and diagnostic-label permutation tests prevent heat labels from deciding
  baseline timing or address classifications.

Reproduce from the repository root:

```bash
bash perf/run_experiments.sh "$PWD/build/perf/architecture_perf"
python3 scripts/audit_perf.py results/blind
```

Each `results/blind/{case}.{dlrm|radix}` prefix has `.metrics.csv`, `.kinds.csv`,
`.phases.csv`, `.sharers.csv`, `.read_runs.csv`, `.write_intervals.csv`, `.spatial.csv`,
`.miss_attribution.csv` and `.blocks.csv`. Default and short replay runs additionally
have `.events.csv`. `{case}.txt` is a readable summary. Distribution exports include whole-run/measured scopes and an all-memory view.
DLRM additionally exports the embedding-table view; Radix uses `space=all`.

The model remains a small abstract baseline: no cache-set conflicts, cache ports,
TLB walks, data DRAM banks, full packet network, tensor kernels or calibrated
hardware costs. Same-line misses/upgrades serialize conservatively; cached shared
reads can overlap. Distributions suggest candidate granularities and thresholds;
correctness, transition costs and prediction errors must be evaluated before
claiming that a particular policy improves execution time. See [MODEL.md](MODEL.md).
