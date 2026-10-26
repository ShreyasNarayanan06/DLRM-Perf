# Model and interpretation

This is a C++17 discrete-event performance sensitivity model. It generates logical
memory traces, profiles them with a finite-cache directory reference model, then
schedules those measured model events using configurable latency and byte budgets.
It does not train or implement a particular sharing predictor or optimized coherence
protocol. The names of the three counterfactual scenarios describe cost assumptions.
The separate legacy `radix_coherence` executable is retained from `agentic-perf`;
its predictor, oracle, state switches and stall estimates are not used by `perf/`.

## Workloads

`--workload radix` preserves the lead-agent/fork/parallel-child/completion lifecycle.
The lead fills a prefix, three children repeatedly gather it while updating their
own leaves, and host and accelerators contend on tree metadata. `--workload dlrm`
uses one host and N inference streams; `--workload both` runs both traces with the
same `Config` hardware fields and reports speedup against each trace's own baseline.
Different workloads do different amounts of work: elapsed time across workloads is
not an equal-work throughput comparison.

DLRM has three disjoint page-aligned address ranges:

| Class | Default logical footprint | Access pattern |
|---|---:|---|
| HotEmbeddings | 205 of 4096 table lines (5.005%) | 80% of accelerator gathers; Zipf rank exponent 1.1; host writes 2 sampled hot lines every 4 batches |
| ColdEmbeddings | Remaining 3891 lines, about 243 KiB | 20% uniform scattered gathers; host writes 1 sampled cold line every 32 batches |
| PrivateScratchpad | 128 lines = 8 KiB per accelerator | 8 logical stores and 2 reads per batch; four sequential logical stores per line; ownership checked |

Hot and cold arrays are independently page-aligned: 5% refers to *logical table
addresses*, not padded allocated pages. Caches start empty; embeddings start
initialized in shared memory. Host writes can touch previously unused preallocated
entries, but allocator resizing, pointer metadata and insertion algorithms are not
modeled. Scratch stores represent activations or other private buffers, not an
explicit accelerator training computation.

Each accelerator batch has two stages. Bottom MLP dense-feature compute (40% of
`compute_ns`) runs independently of the sparse embedding gathers. Top MLP compute
(the other 60%) starts after **both** bottom compute and all gathers complete, then
issues scratch writes/reads. The following batch cannot issue gathers before the
current scratch memory batch completes. The general timing engine permits one
next-step compute lookahead after `ceil(overlap_trigger * batch_request_count)`
completions; this remains enabled for RadixTree. The DLRM gather-to-top edge disables
lookahead to respect the data dependency. `--bottom-fraction 0 --overlap-trigger 1`
disables compute/memory overlap in DLRM. Compute is a configurable timing budget,
not an executed tensor kernel or FLOP model.

The host has two matching stages per batch, with `host_compute_ns` per gather
stage. Host batch r cannot issue its updates before accelerator 1 starts gather r.
It may lag if its own preceding updates take longer. This paces the concurrent
stream instead of letting all updates complete at time zero. Cadence is in logical
batches, not wall-clock arrivals. Setting an update interval to zero disables that
stream. Per-node `mt19937_64` seeds and an explicit uniform-to-Zipf CDF sampler keep
accelerator addresses unchanged when host cadence is edited. Repeated runs on the
validated compilers give identical trace fingerprints; tiny floating-point CDF
boundary differences remain possible across other math libraries.

## Reference events and finite caches

`profile.cpp` reuses `RegionStateMachine` with **Policy::Baseline only**: 64B lines,
one exclusive E/M owner or clean shared copies, write invalidations, dirty-owner
writeback on intervention, and version invariants. Each node, including the host,
has a fully associative finite LRU of `cache_bytes / 64` lines shared by its memory
classes. Invalidated entries are removed before capacity selection. Dirty eviction
writes back 64B; repeated writes with local exclusive permission are hits. Zero
cache capacity disables retention. There is no infinite accelerator cache and no
flat coherence tax on every logical request.

The profile is deterministically ordered by phase, step ordinal, access ordinal,
then stream/node order. DLRM host accesses are in gather stages interleaved with
accelerator gathers; they do not all precede inference. Counters are exact for this
abstract directory and specified logical interleaving, **not ground-truth hardware
MESI measurements**. Data-cache set conflicts, MOESI Owned state, prefetchers, data-cache ports and
detailed DRAM data banks are not modeled. Home-directory capacity and timing
resources are described below.

Timing changes do **not** feed back into cache/coherence interleaving. Host pacing
controls when profiled work can be issued, but is not a timing-driven cache
consistency simulation. Actual update/read races could change the directory events
and therefore the result. This is the main accuracy limit to resolve before using
numbers as an architecture performance prediction.

## Four cost scenarios

1. **Baseline Always-Coherent:** all observed directory, snoop, invalidation and
   writeback events retain their configured costs. No region-mode tax.
2. **Cold RO + Private Scratchpad Bypass:** cold reads and private scratch accesses
   retain none of their reference directory/snoop/invalidation budgets. Hot events
   retain full costs. Each new bypass page pays a grant/barrier. A cold host write
   broadcasts revocation to all endpoints, evicts every cached copy on that page,
   obtains normal coherent write permission, and flushes/publishes the update
   before read-only access resumes. A write to an as-yet ungranted page needs no
   pre-revocation but still pays clean publication. For RadixTree this row applies
   the equivalent prefix/leaf hint, retaining metadata coherence.
3. **Naive Coarse Region Coherence:** all memory classes receive bypass budgets.
   Any write with remote cached copies anywhere on its page revokes the whole
   page, evicting valid copies at all endpoints. Other-line copies are counted as
   collateral false sharing. Dirty data is flushed, a recovery/barrier is charged,
   and the write is published clean. A remote read of a page containing dirty
   remote lines downgrades/flushes the whole page, even if the requested line is
   clean. An owner-only scratch write does not spuriously revoke its page. These
   are transparent page-cost rules, not a proposed adaptive coherence policy.
4. **50% KV/Embedding Overhead Reduction:** halve the *reference directory/snoop/
   invalidation* latency and control-byte budgets for all KV/embedding/scratch
   accesses, including hot embeddings. Radix metadata stays fully coherent.
   Reference hit/fill behavior is preserved, except for explicitly moving prefix
   dirty publication to the fork. It does not halve data transfer, writebacks,
   compute or total application time. This is a broader hypothetical capability
   than selective bypass, so the two rows need not have a fixed ordering.

RO bypass has no precise per-line sharer directory to provide free knowledge of
readers: grant/revocation/publication broadcasts cost `directory_bytes * (N + 2)`
(endpoint exchanges plus commit). Private grants cost two exchanges. Each barrier
adds `region_barrier_ns`; initial grants also add `setup_ns`. Explicit unexpected
writes add `recovery_ns` and `recovery_bytes`. Revocation applies to actual valid
cached copies, not all 64 lines unconditionally. Dirty revocation/publication costs
`ceil(region_flushes / outstanding) * (memory_ns + writeback_ns)` in service waves;
its 64B per dirty line traverses the shared link **once**. These penalties are
conservative budgets, not calibrated protocol message schedules. Home directory metadata for both line permissions and region operations shares
the finite cache and controller below. Accelerator-local region permission caches
and a separate barrier-engine bandwidth limit are not modeled.

All counterfactual Radix rows additionally pay `fork_barrier_ns + setup_ns +
prefix_regions * region_barrier_ns`, control exchanges and bounded outstanding
dirty-prefix publication. DLRM has no artificial fork. The 50% budget alone has
an expected residual recovery tax: `recovery_rate` per non-metadata access requiring
a reference directory event, default 1%, plus `recovery_ns`/bytes. It is a fractional
expected-cost sensitivity, not randomly observed mispredictions. The two explicit
page-hint rows instead count actual page write recoveries, avoiding double charging.
`added_latency_ns` is an optional per-logical-KV/embedding/scratch hint-check cost.

## Finite home directory (refinement)

`directory.hpp` / `directory.cpp` isolate the new controller model. Defaults are
**1,024 cached entries, four banks, 2 ns issue interval per bank, 16 metadata miss
buffers, 20 ns tag lookup latency, 80 ns additional backing service, and 32 GB/s
local metadata backing bandwidth**. These are explicit synthetic assumptions,
not measurements or specifications for Great Lakes, a GPU or a CXL product.
A metadata entry occupies one modeled 64B transaction. A real compressed directory
may pack more entries per transaction and have very different cache coverage.
This proof of concept omits that spatial packing, which can overstate metadata
misses and backing traffic. The miss-cost/capacity sweeps expose sensitivity but
do not substitute for a hardware directory-layout measurement.

Every retained coherent permission request accesses a line entry. Each page grant,
revocation/publication barrier or dirty-read downgrade also accesses a region entry
in the **same** finite resource. Line/page keys occupy disjoint namespaces, so
bypass does not get a free unlimited home permission store. Banks are address-hashed;
issue rate is separate from lookup latency, allowing pipelined hits. Port requests
are queued in arrival order per bank. Actual tag probes run as chronological events.

A missing entry fetches 64B from a memory-backed directory. Active owner/sharer
changes make cached entries dirty; replacing one writes back 64B of **metadata**.
This traffic uses a separately modeled, bandwidth-limited home backing channel,
not the CXL data link. Dirty replacement plus fetch consumes two backing transfers;
backing service is pipelined. Metadata cache eviction **does not invalidate data
caches** or discard permissions: backing memory preserves the full directory.

Miss buffers are bounded. A full miss-buffer set stalls new tag misses while hits
can pass. A pending metadata entry cannot be evicted or treated as an immediate
hit: repeated lookups merge with its fill and wait for readiness. If all metadata
cache entries are pending, replacement waits for one to become ready. Retried
probes do not count as extra logical requests. `directory_entries=0` disables
metadata retention, so every permission probe misses. Metadata state and data
LRUs carry across phases and the warm-up boundary.

The 50% overhead row retains every reference lookup, but halves the non-metadata
lookup latency, issue-slot occupancy, backing service and backing bandwidth budget.
Its cache tags still occupy full entries. This is an event-equivalent cost
sensitivity, not a physical implementation that somehow transmits half of a dirty
metadata line. Raw metadata byte counters remain whole transactions; backing busy
time reports the retained cost. Page-hint rows instead eliminate their bypassed
per-line probes while paying full-cost region operations and coherent hot probes.

`--directory-model 0` reproduces the previous timing path exactly, including its
fixed directory latency and page scheduling. The new path holds line/page
operations through actual controller completion; thus the toggle also changes that
scheduling detail for page hints. It is a legacy reference, not a pure isolated
capacity ablation. `directory_entries`, `directory_banks`, `directory_issue_ns`,
`directory_mshrs`, `directory_miss_ns` and `directory_backing_gbps` independently
expose resource and latency sensitivity. Eviction notices still use the existing
8B/4ns approximation; they do not separately probe the cached home directory.

This is a cached, memory-backed directory design assumption. Some hardware uses
an inclusive finite snoop filter whose eviction back-invalidates cached data; this
model does **not** implement that alternative. The [gem5 CHI model documentation](https://www.gem5.org/documentation/general_docs/ruby/CHI/)
describes controller pipeline and finite transaction resources that motivate
modeling these bottlenecks. Our small controller is not a CHI implementation or a
replacement for a calibrated full-system simulator.

## Warm-up and longer traces

`--rounds 512 --warmup-rounds 128` runs all 512 logical batches and reports a
separate measurement of the final 384. Host cadence, scratch cursors, PRNG streams,
data caches and directory state continue from warm-up. The boundary drains
outstanding work and synchronizes streams; no reset, extra cache flush or synthetic
transition is added. This barrier can affect inter-stream timing, so it is not a
fully uninterrupted trace measurement.

For DLRM, warm-up and measured inference phases are separated. For RadixTree,
lead/fork setup, the first 128 child rounds, and final completion work are outside
the measured window. The remaining child rounds are measured. Default
`warmup_rounds=0` retains the whole execution, including setup/completion. A positive
warm-up must be smaller than `rounds`. Post-warm-up is not automatically stationary:
large random address spaces can keep encountering new lines throughout measurement.

CSV totals keep whole-run semantics. `measured_execution_us` and
`measured_execution_reduction_pct` compare just the measured windows. `_kinds.csv`
contains `scope=all` and `scope=measured`; `_phases.csv` marks measured phases. New
directory counters distinguish hits, misses, merged fills, dirty metadata evictions,
bank queue wait, miss-buffer wait, pending-entry capacity wait, backing queue wait,
and separate backing read/write bytes. Per-kind directory counters attribute work
to the triggering request class; this includes backing writes of displaced
metadata. Data-cache eviction/writeback counters still use the actual victim class.
Directory peak occupancy fields are run-wide maxima, including in measured rows;
counts, service and waits in measured rows exclude warm-up.

## Timing and metrics

Eligible requests enter a FIFO link shared by host and accelerators. Link serialization
is `bytes / bandwidth_gbps`, with GB/s interpreted as decimal bytes/ns. Data fills
and writebacks each transfer 64B. Service latency follows serialization; it can
pipeline with other requests. Page- or fill-dependent requests are rescheduled at
their eligible time before joining the link queue, so they do not reserve future
link slots while unrelated requests wait. With the finite directory enabled,
same-line dependencies are held until actual completion through the controller,
not an optimistic timestamp calculated before queuing. Same-node same-line accesses wait for
prior service; bypass page grants and recovery operations also gate the page and
wait for previously issued page accesses. Each node has at most `outstanding`
issued memory operations, including pending operations blocked by these dependencies.
No extra MLP comes from compute lookahead. Phases join at the last completion.

For each access the modeled completion is:

```
eligible = max(batch_issue_time, same_node_line_ready, applicable_page_gate)
link_done = max(eligible, link_available) + (fill_bytes + writeback_bytes + control_bytes) / GBps
directory_done = queued_line_lookups_and_region_operations(link_done)
completion = directory_done + (fill ? memory_or_peer_ns : hit_ns)
           + retained(snoop_ns * snoops + invalidate_ns * invalidations)
           + writeback_ns * non_region_writebacks + 4ns * capacity_evictions
           + explicit_region_service + expected_recovery_service + optional_check_service
```

All scenarios conservatively retain an 8B/4ns capacity-eviction notice. Counterfactual
budget changes scale only directory/snoop/invalidation events, never the data or
dirty flush payload. `peer_ns` applies when a line has another exclusive owner;
this is not a modeled CXL fabric topology. A single serialization resource covers
both directions and control/data traffic. Queue wait, worker compute, memory wait,
overlap and event latency are *aggregate request/worker quantities*, not additive
critical-path components. Execution time is the actual final event timestamp.

`--csv` exports totals, estimated time, time reduction versus the same workload's
baseline, trace fingerprint, hardware/workload configuration and all page penalties.
`--kinds` exports both whole-run and measured-window reads, writes, cache hits, fills, directory lookups, snoops,
invalidations, writebacks, evictions and region events by memory class **for each
scenario**. Capacity evictions and writebacks are attributed to the actual victim memory
class, even when another class triggers them; service time remains on that request.
Fork writebacks are separate. Baseline rows
are the reference event counts. Counterfactual raw events describe the reference
bookkeeping after explicit page revocations; they are not claimed to be literal
transactions of an implemented bypass protocol. `charged_*_events` are retained
*event-equivalent latency budgets* and may be fractional in the 50% row. Actual
transmitted byte budgets are in `control_bytes`; grants/revocations are additional
messages. `region_recoveries`, `region_downgrades`, `region_grants` and
`region_barriers` expose permission work; `collateral_copies` isolates false sharing.

The link combines request/response bytes before directory and memory service;
this is a lumped transaction timing model, not separate protocol packet round trips.
The directory backing channel is modeled separately from DRAM data service, so
metadata/data contention at a shared physical memory controller remains unmodeled.
Snoop/invalidation processing still uses additive event budgets; it has no separate
controller queue. Finite controller queues improve resource realism but do **not**
fix the earlier static functional cache/coherence interleaving limitation.

The costs are assumptions, not measurements of GPUs or CXL on Great Lakes. The
cluster runs validate C++ code and determinism. Default results include cold-start
and finite 64-batch phases, not an isolated steady-state measurement. A cache-fitting
run still pays first-touch permissions, hot host invalidations and rare cold page
updates. The default cold address space exceeds cache capacity, but its short
64-batch sampled trace does not fill the entire cache: default cold misses are
largely first touches. The 4 KiB cache-exceeding regime explicitly exercises
capacity evictions. A new seed or transition budget can change benefit size and sign.

Workload structure follows the dense-feature MLP, sparse embedding and top MLP
organization documented by the [original DLRM project](https://github.com/facebookresearch/dlrm).
The chosen skew, host cadence, footprints and nanosecond budgets are exposed synthetic
research inputs, not a claim that all production DLRM deployments behave this way.
