# Timed baseline profiler: definitions and limits

`architecture_perf` defaults to an **always-coherent MESI-style baseline**, then
analyzes its observed addresses at 64B, 256B, 1KiB and 4KiB. The address profiler
has no percentage-based speedup scenarios. The optional
`--management compare` experiment now adds explicit software/hardware management
and timed region handoffs; see [COMPARISON_MODEL.md](COMPARISON_MODEL.md).
The block sizes are analysis lenses over the same 64B
baseline, not four simulated coherence protocols. The independent original
`radix_coherence` executable remains a legacy project and is not used by `perf/`.

## Mixed DLRM layout

One contiguous table starts at `embedding_base` and contains `table_lines` rows,
with one modeled 64B line per row. A seeded Fisher–Yates permutation selects hot
ranks throughout that table. The first `round(table_lines * hot_fraction)` shuffled
rows are hot candidates; the rest are cold candidates. Thus hot and cold rows share
pages. Layout seed and request seed are independent, exposed inputs.

The generator still chooses 80% of accelerator sparse reads from the hot set using
a Zipf rank distribution and 20% uniformly from the cold set. The host periodically
updates rows from those same sets. These choices are synthetic workload inputs;
the profiler does not receive heat classifications. Hot/cold `AccessKind` values
remain optional diagnostic labels in `.kinds.csv` and event logs. They never select
cache behavior, directory service, histogram membership or mode transitions. A
label-permutation test verifies identical timing, cache/miss distributions and
address histograms after labels are changed.

This hot-set Zipf plus uniform-tail mixture is not a single full-table Zipf law.
The optional address-based skew analyzer reports observed rank curves and an
independent calibrated full-table Zipf sample; it does not change simulation input.
See [SKEW_PROFILE.md](SKEW_PROFILE.md) for the equation, measured concentration,
cache-filtered fills and calibration limits.

The `embeddings` output space is selected numerically by the contiguous table's
address bounds. `all` includes every address, including private scratch and Radix
metadata/KV. Keeping both views prevents private scratch traffic from concealing
the table's behavior. All statistical groups are keyed by addresses, not labels.

An independent PRNG stream per node preserves accelerator request addresses when
host cadence changes. Scratch buffers remain separate per-accelerator allocations,
with reads/stores and sequential line reuse. Dense bottom compute may overlap
embedding gathers; top compute waits for both before scratch accesses. Host batch r
cannot start its memory requests before accelerator 1 starts gather r. Cadence is
in logical batches, not periodic wall-clock arrivals.

## Timing-driven functional memory state

`profile.cpp` is now an incremental `BaselineMemory` component. It no longer
walks the trace in round-robin order before simulation. A request goes through:

1. A bounded per-node outstanding window, then admission when its line and a data
   cache slot/victim are available. Admission consults current committed data state.
2. Upstream CXL permission requests / eviction notices, with FIFO serialization.
3. Timed home-directory bank/tag/miss-buffer/backing work, if permission or eviction
   bookkeeping is required. Eviction metadata and demand metadata are separate ops.
4. Data/peer service and snoop/invalidation/writeback service budgets.
5. Response/data serialization on the shared CXL resource, then functional commit.

Cache fills are not valid before commit. Writes advance the line version at commit;
readers return the newest committed version in their serialized access order.
Invalidations remove actual remote cache entries at commit. Dirty-owner intervention
and dirty victim writeback update memory before ownership/data become visible.
Version and owner/sharer invariants are checked throughout completion processing.
A test changes only memory latency and observes a different read version, access
order and writeback count, demonstrating that timing feeds back into coherence.

Data caches are finite fully associative LRUs. Pending fills reserve capacity.
Victim selection skips lines with active operations; if no slot/victim is available,
the request waits without holding its target lock. Target/victim reservations are
acquired atomically to avoid eviction dependency deadlocks. Dirty victim data stays
inaccessible to conflicting operations until its writeback completes. Cached reads
of a line can overlap; misses/upgrades/writes serialize at line granularity.
A queued exclusive request prevents later cached readers from indefinitely passing
it. The FIFO event tie-break is a deterministic sequence number.

A first clean read may receive E permission; a remote read of E/M requires an
owner snoop and downgrade. A dirty owner supplies a writeback. Subsequent reads of
clean S copies add a sharer through the directory. A write to S requests permission
and invalidates other copies; repeated local E/M writes are hits. Data hits on
write upgrade can still require a directory lookup. With `cache_bytes=0`, no data
is retained and writes use an explicitly modeled write-through/RFO approximation.

The implementation uses abstract data versions rather than embedding values.
This is a concrete MESI-style baseline model, not measured hardware ground truth.

## Finite directory and link

The home metadata cache is a memory-backed fully associative LRU, with hashed bank
issue slots and bounded metadata miss buffers. Pending tag fills cannot be evicted
or treated as immediate hits. Lookups can merge into an existing pending metadata
fill, while data-line transactions remain serialized separately. Evicting metadata
writes its modified owner/sharer record to backing memory; it **does not** invalidate
data caches or discard permissions. Permission updates and data-cache eviction
notices both probe the directory. A miss is attributed to the exact probed address,
including a victim address for an eviction notice.

Default assumptions: 1,024 metadata entries, four banks, 2ns issue interval, 16 miss
buffers, 20ns tag lookup, 80ns extra backing service and 32GB/s metadata backing
bandwidth. One metadata cache entry/transfer is modeled as 64B. Record packing,
set conflicts and actual product directory layouts are not calibrated. Metadata
backing uses its own channel, separate from CXL and data-memory service.

The CXL link is one shared FIFO serialization resource covering both directions.
Requests blocked on cache/line resources do not reserve future link slots. Demand
permission requests charge `directory_bytes`; data-cache eviction notices charge
8B. Response snoop/invalidation controls use their configured budgets. Fills and
dirty data writebacks each transfer 64B. Metadata backing traffic is separate.
Data/peer service, snoop/invalidation service and dirty writeback latency remain
configurable abstract costs. Eviction and demand directory work are serialized
within each transaction. Bank pipelines and misses can overlap other transactions.

## Observation windows and exact denominators

`scope=all` covers the full run. `scope=measured` covers phases marked measured;
with positive warm-up, the first `warmup_rounds` inference/child batches are excluded.
Data caches, directory state, PRNG streams and addresses persist across warm-up.
The boundary drains outstanding work and synchronizes streams. Radix lead/setup
and final completion are excluded when warm-up is enabled. Without warm-up the
entire workload is measured. Post-warm-up does not imply statistical stationarity.

Each logical request is observed **once at successful memory admission**, before
its directory service, rather than once per retry or when the CPU issues it into
a blocked window. Time distributions use these admission timestamps. Optional event
logs record issue, admission, completion and exact admission order, making waits
and tie ordering explicit. Metadata misses are recorded at actual timed tag probes.

### Distinct-accessor histogram (`.sharers.csv`)

For each touched block, take the union of node IDs that admitted a demand access
within the observation window. Report how many blocks have 1, 2, ... N+1 distinct
accessors (host 0 is included). Percentages use **demand-touched blocks** as the
denominator. This is historical accessor count, not simultaneous resident MESI
sharers and not proof of a block's current ownership. Metadata-only probes do not
create a demand accessor or inflate the denominator.

### Read runs and write intervals

`.read_runs.csv` gives exact counts of consecutive reads between writes to the same
address block. `.write_intervals.csv` gives corresponding admission-time durations
in logarithmic `[2^k, 2^(k+1))` ns bins, plus an explicit point mass at zero.
Read counts are exact, including zero-read intervals between consecutive writes.
Both exports distinguish:

- `closed`: bounded by two observed writes. Only these are complete intervals.
- `left_censored`: reads from window start until the first write; the preceding
  write is outside the window or unknown. Zero-read leading intervals are retained.
- `right_censored`: reads after the final observed write until window end.
- `both_censored`: no write was observed for the block during the entire window.

Censored durations/read counts are observed lower bounds on phase length. Long
read-only blocks are therefore not silently dropped or claimed to have infinite
reuse. Percentages are within each run type; they are not access-weighted rates.
Unobserved blocks contribute no run samples. Simultaneous admissions to different
lines in a larger block use deterministic admission order; that tie order is not a
physical total order guaranteed by hardware.

### Spatial purity (`.spatial.csv`)

Within each block/window, track distinct touched 64B lines. A written line has at
least one observed write. A read-only line has at least one observed read and zero
observed writes in that window. Categories are `read_only_blocks`,
`written_lines_only_blocks` and `mixed_blocks` (both written and read-only lines).
A written line may also have reads; "written lines only" does not mean write-only
operations. Untouched lines are excluded. A 64B block cannot be spatially mixed.

`mixed_pct` is the fraction of **demand-touched blocks**, not the fraction of
allocated pages, accesses or bytes. Embedding exports also give the allocated
block count, including the final partial block; `allocated_blocks=0` in the `all`
space means allocation coverage is undefined across disjoint address ranges.

### Directory miss attribution (`.miss_attribution.csv`)

Classify blocks using zero vs positive **observed** writes, after the selected
window has finished. A later write changes the final classification of earlier
misses; no hot/cold label or first-touch guess is used. Counts are metadata cache
misses, not all directory probes and not data-cache misses. They are exported for
`lookup_origin=demand`, `eviction` and `all`. Percentages use the total misses for
the same scope/space/block size/origin.

Both `write_window=scope` and `whole_run` are provided. Thus a block evicted during
measurement after a warm-up write can be zero-write in the measured window while
written in whole-run history. A metadata-only block can have no demand access in
measurement yet contribute miss attribution; it is excluded from spatial/accessor
denominators. These are workload-history statements, not guarantees of immutability.

`.blocks.csv` contains the underlying address-level counts, node counts, written
and read-only line counts, whole-run write counts, and demand/eviction miss counts.
This supports alternative weights or analysis without heat-based aggregation.

## Output interpretation and remaining limits

`.metrics.csv` / `.kinds.csv` report ordinary baseline hits, fills, permission
lookups, snoops, invalidations and writebacks. `directory_lookups` counts demand
permission probes; `directory_requests` additionally includes eviction notices.
Dirty data evictions/writebacks are attributed to the actual victim label in the
optional diagnostic class table; event-row service counts include all work caused
by that request. Metadata replacement counters belong to the probed request class.
Wait/service counters are aggregates, not additive critical-path fractions. Peak
resource occupancy fields are run-wide maxima, also in measured diagnostics.

The model still omits cache-set conflicts, cache ports, data-miss coalescing across
requesters, TLB/page walks, DRAM data banks, precise protocol packet networks,
shared metadata/data memory-controller contention and real DLRM tensor kernels.
Same-line transaction serialization is conservative. The single-row=64B layout,
Zipf skew, update cadence, compute and nanosecond costs are synthetic inputs.
External insertion/allocation algorithms and unmodeled agents are not represented.

These distributions can identify candidate granularity and phase thresholds. An
optimal policy additionally requires transition cost, correctness rules, prediction
error and hardware validation. Neither historical sharers nor observed zero writes
alone establishes that a block can safely bypass coherence in the future.
