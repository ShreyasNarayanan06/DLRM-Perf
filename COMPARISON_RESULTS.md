# Software-managed versus hardware-managed sharing

Great Lakes job **64831183** completed on **gl3018** with exit **0:0** (GCC 8.5.0,
C++17 Release, 47s job wall time). All 14 CTest targets and six Python skew checks
passed. All **550 output files** match the local Release run byte for byte. This experiment uses the
same shared-memory machine for all three modes; it does not reproduce an existing
sharded GPU training framework.

The software-managed baseline caches data and explicitly maintains valid copies.
It does not automatically miss on every lookup or discard all data every batch.
Its home-table requests use four software worker slots, with 100ns processing per
request and a 16B additional envelope by default. Those are uncalibrated inputs.
Always-hardware uses the same physical permission-record storage without those
software costs. Dynamic switches the manager by region and pays for its checks,
barriers, record transfer and dirty-data publication.

## Hardware and workload cases

All long runs contain 512 batches with 128 warm-up batches; costs and caches persist
across the drained measurement boundary. Both caches and copy management remain
active in every mode. No outdated reads are permitted.

| Case | Software us | Always-hardware us | Dynamic us | Dynamic time reduction vs software | Dynamic speedup |
|---|---:|---:|---:|---:|---:|
| Default | 409.400 | 220.964 | 246.783 | 39.72% | 1.659x |
| Table fits cache | 172.240 | 125.944 | 171.272 | 0.56% | 1.006x |
| Table exceeds cache | 1162.581 | 415.567 | 538.351 | 53.69% | 2.160x |
| Slow interconnect | 514.828 | 372.236 | 392.344 | 23.79% | 1.312x |
| More CPU updates | 574.950 | 294.045 | 303.212 | 47.26% | 1.896x |
| No CPU updates | 374.488 | 205.740 | 222.393 | 40.61% | 1.684x |
| Eight accelerators | 961.964 | 312.845 | 365.481 | 62.01% | 2.632x |

Default: 3 accelerators + CPU, 64KiB local cache/node, 4,096-row/256KiB embedding
table and a 32GB/s interconnect. Cache-fitting changes the table to 512 rows/32KiB.
Cache-exceeding changes the cache to 4KiB and table to 8,192 rows/512KiB. Slow-link
retains default sizes but uses 2GB/s. More-updates uses four hot updates every
batch instead of two every four batches. The eight-accelerator case increases
concurrency and total work; its absolute runtime is not equal-work scaling.

## Dependence on software costs

| Case | Software us | Always-hardware us | Dynamic us | Dynamic time reduction vs software | Dynamic speedup |
|---|---:|---:|---:|---:|---:|
| 0ns + no extra envelope | 220.964 | 220.964 | 228.493 | -3.41% | 0.967x |
| 25ns software handling | 250.692 | 220.964 | 233.271 | 6.95% | 1.075x |
| 100ns software handling | 409.400 | 220.964 | 246.783 | 39.72% | 1.659x |
| 250ns software handling | 872.959 | 220.964 | 287.892 | 67.02% | 3.032x |

Only the indicated runtime assumptions change. The zero-cost control removes both
extra software processing and its message envelope; ordinary ownership metadata,
data transfers and permission messages remain. In this control software and
always-hardware times are exactly equal. Dynamic adds overhead and loses.

At the default 100ns assumption, dynamic takes **39.72% less time / 1.659x speedup**
than this software baseline. At 25ns the gain is **6.95% / 1.075x**. This sensitivity
is central to interpreting the result; 39.72% must not be quoted as a general
DLRM or GPU-fleet speedup. Actual runtime/message costs need measurement.

## Granularity and policy controls

| Case | Software us | Always-hardware us | Dynamic us | Dynamic time reduction vs software | Dynamic speedup |
|---|---:|---:|---:|---:|---:|
| 64B regions | 409.400 | 220.964 | 414.568 | -1.26% | 0.988x |
| 256B regions | 409.400 | 220.964 | 414.755 | -1.31% | 0.987x |
| 1KiB regions | 409.400 | 220.964 | 354.846 | 13.33% | 1.154x |
| 4KiB regions | 409.400 | 220.964 | 246.783 | 39.72% | 1.659x |
| Only 16 region records | 409.400 | 220.964 | 415.000 | -1.37% | 0.987x |
| 1,000ns switching delay | 409.400 | 220.964 | 249.871 | 38.97% | 1.638x |
| Request seed 43 | 403.006 | 217.827 | 249.042 | 38.20% | 1.618x |
| Short run, no warmup | 14.687 | 8.550 | 14.684 | 0.02% | 1.000x |

The same 256-entry region tracker is used at each block size, so these controls
change both spatial grouping and address coverage. They do **not** isolate a pure
region-size effect at equal byte coverage. Smaller regions can evict learning
history before the 64-access review threshold is reached. A hardware-mode record
cannot simply be dropped: when the table is full, untracked regions safely use
software. The 16-entry control illustrates that fallback.

The reference never looks at future writes or hot/cold access labels. It is a
replaceable starting point, rather than a policy tuned to win. Always-hardware
is faster than dynamic in the default experiment, and several controls show
that dynamic management can lose to software.

## What the counters mean

Both modes maintain owner and cached-copy information. The difference is whether
an explicit software handler or a hardware controller manages each operation.
The raw `.metrics.csv` files retain historical `directory_*` column names; in
software mode they refer to the software home permission table, **not enabled
hardware cache coherence**. The `.comparison.csv` uses `permission_requests` and
separates `software_requests` from `hardware_requests`.

Mode changes transfer state after all accesses to the region have finished. Dirty
lines are written back and remain cached as clean copies. Every changed line costs
64B on the same link used by application traffic. Region changes also transfer
resident permission records and pay a barrier. Data is not magically updated and
no whole-region data invalidation is assumed. All these costs are included in time.

## Validation and reproduction

Local validation: **14/14 CTest targets passed under ASan/UBSan**, including seven
new management unit checks in one target. **1,057,809 audit checks** passed across
17 comparisons / 51 runs; **197,388 captured requests** were independently replayed.
Replay verifies current data versions, address histograms and that target/victim
accesses do not overlap a handoff. Audits also check bytes, metadata probes,
software queue budgets, finite capacities, identical inputs and speedup arithmetic.

The unchanged baseline frontend reproduces all **139 original result files byte
for byte**, and its existing **528,210-check** audit still passes. Tests include an
unused promotion hint that must not pin cache victims and deadlock other work.

```bash
bash perf/run_comparisons.sh "$PWD/build/perf/architecture_perf"
python3 scripts/audit_comparison.py results/management
```

Outputs per case: `{case}.dlrm.comparison.csv` plus `{case}.dlrm.{software|hardware|dynamic}`
raw metrics, phase/kind summaries, address distributions and `.region_changes.csv`.
Default and short runs additionally capture `.events.csv`. Region changes are
logged for the whole run, including warmup; summaries distinguish measurement.
All speeds are simulated application times, not job wall time.

[COMPARISON_MODEL.md](COMPARISON_MODEL.md) describes consistency, scheduling,
manager decisions and the missing production-system mechanisms.
