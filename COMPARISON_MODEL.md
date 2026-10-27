# Comparison model and assumptions

The new experiment compares **software-managed cached copies**, **always-on
hardware management**, and a **reference that selects the manager per region**.
It uses the shared-memory architecture from the project proposal. It is not a
simulation of TorchRec's sharded tables, NCCL collectives or a measured production
GPU fleet. The workload is inference with concurrent CPU updates, not a complete
training/backpropagation/optimizer workload.

## Shared correctness contract

Both software and hardware keep an owner/copy record for every used 64B line.
They allow valid cached reads and repeated writes by an exclusive owner without
contacting the home manager. A cache miss, ownership upgrade or data-cache eviction
sends an explicit permission/retirement request. Remote dirty owners supply their
latest data; writes retire other cached copies before the new value is visible.
No scheme is allowed to silently retain an outdated copy or skip required data.

`BaselineMemory` in `profile.cpp` is the **functional copy/permission engine** used
by all modes. Its MESI-like permissions are also a possible software protocol;
reusing this state machine does not mean hardware coherence is enabled in software
mode. `ManagementModel` determines who executes the protocol operations and how
much processing/traffic they cost. Software mode's `hardware_requests` is always
zero, and the audit checks that property.

Requests are classified when admitted and data changes commit at completion.
Fills reserve finite LRU slots and cannot be used early. Same-line misses/writes
are serialized, while cached reads can overlap. Software messages are abstracted
as timed request/response work; this is not an instruction-level implementation
of a runtime or a network packet simulator. Early line reservation is conservative
and may block a cached reader before a real revoke message would arrive.

Data versions advance on committed writes. Both paths check owner/sharer/version
invariants. Read freshness means the latest completed write in the simulated
per-line serialization, rather than matching read values across runs with different
request interleavings. There is no relaxed freshness or batch snapshot shortcut.

## Software-managed non-coherent baseline

The memory hardware does not automatically maintain coherent cached copies. A
software runtime maintains the home permission table, handles grants and sends
copy-retirement messages. Cache hits with already valid permissions do not pay a
network or software-home round trip. This is a software distributed-shared-memory
reference, not an assertion that all deployed DLRMs use this protocol.

Every home-table operation (demand permission or cache eviction notice) executes
one task on a finite pool of software workers. Default: four workers, **100ns per
operation** beyond the common metadata lookup/transfer costs. This models processing
on reserved CPU resources; it does not compete with the workload's host compute
stream. Remote handler execution is not separately charged, making that portion
of the baseline optimistic. These costs must be calibrated to a specific runtime.

Requests have the common 16B permission message or 8B eviction notice. Software
adds a 16B envelope to each home request and to each remote intervention or
invalidation response. All envelopes use the same shared link as data. The software
case uses targeted copy information, not a broadcast to every GPU or validation
on every cached read. Batching/compression of runtime operations is not modeled;
the 25ns and zero-extra-cost cases expose how much this choice affects the result.

## Always-on hardware reference

The hardware controller executes the same permission/copy actions, without the
software task or extra software envelope. It does not have infinite metadata
capacity or zero communication costs. A finite home record cache, bank issue
queues, miss buffers and backing channel are common to all three modes. The same
record storage is shared by the software/hardware frontends in dynamic mode;
it is not two independent caches with doubled capacity.

The older `architecture_perf` default remains this hardware reference. A unit
check compares that unchanged path with both explicit hardware mode and the
zero-extra-software-cost mode. All 139 original result files still match byte for
byte, independently checking the default path after the scheduler extension.

## Replaceable dynamic reference

This reference **chooses software versus hardware management**. It does not add
an immutable-prefix broadcast protocol or eliminate the hardware directory on
shared read misses. It is one implementation to evaluate, not a claim to have
selected the right research policy.

Each tracked region starts under software management. After 64 successfully
admitted demand accesses, it reviews the recent window:

- If at least two nodes accessed the region and at least eight accesses needed
  new line permissions, request hardware management.
- If already under hardware management and no access in the window needed a
  new permission, request software management.
- Otherwise leave the manager unchanged, then begin a new observation window.

Only past numeric addresses, node IDs and actual permission events are used.
No hot/cold/scratch labels or future writes select a mode. Cache eviction notices
still use the manager of their victim region; they do not count as demand accessors
or trigger heat classification. The algorithm is isolated in `management.cpp`.

The default region size is **4KiB**, matching the original page-sized proposal.
The region table has **256 records**, with LRU replacement of idle software-mode
history. Hardware-mode records cannot be silently evicted. If no record is
available, new regions safely continue using software. Thus smaller regions at
fixed table capacity reduce byte coverage as well as changing spatial grouping.
Each dynamic demand access pays a **2ns region lookup**, including local hits.

A requested mode change occurs when the next demand access arrives:

1. Block new target/victim use of that region and drain accesses already using it.
2. Transfer owner/copy records for resident lines (16B per line plus a 32B message).
3. Flush each dirty line (64B of data, plus writeback service), retaining clean
   cached copies. The transfer cannot race with new writes or victim selection.
4. Pay a 200ns handoff/barrier delay, publish clean memory and install the manager.

Dirty publication additionally charges the configured memory service once when
a handoff contains any dirty data, plus `writeback_ns` per dirty line. Payloads
serialize on the same link. All changes, including a quick reversal, pay these
costs. This reference does not speculate that writes cannot happen, so it has no
separate read-only violation recovery path; wrong profitability decisions appear
as wasted lookups/changes and can cause a slowdown.

An unused future promotion hint must not pin cached lines forever. Only an active
drain or handoff blocks victim selection. A regression test covers a region that
is never read again after its promotion hint. Idle software hints may be discarded
under region-table pressure; this loses prediction history without losing data.

## Event integration and measurement

`model.cpp` remains the single event scheduler. A nullable management adapter
preserves the original baseline path. The adapter gates region changes, routes
home operations and adds software work/envelopes. Worker completion is scheduled
before directory issue so a delayed software request cannot reserve a future
hardware bank slot ahead of requests that have actually arrived.

Data service, ownership interventions, invalidations and responses follow the
same scheduler. Mode changes are separate completion events. Target and victim
region reservations last until functional completion. Requests blocked by a handoff
retry when prior work or the handoff finishes, without occupying a future link slot.

Warm-up drains streams but preserves caches, record storage and region histories.
A mode change begun after the measurement boundary is measured, even if earlier
observations requested it. Summary maxima are run-wide peaks. Software/transition
wait counters are aggregates across requests and are not critical-path fractions.

Raw `.metrics.csv` fields retain the existing `directory_*` names for compatibility.
In software mode they count **software permission-table** operations. The separate
`.comparison.csv` records manager-specific routing, costs and software-relative
speedups. `.region_changes.csv` records addresses, times, directions, copied
records and dirty-data bytes. Those bytes are included in link and execution time.

## Limits and interpretation

The software handler/envelope, region lookup and handoff costs are **assumed
parameters**. Default gains can be dominated by avoided software processing.
The zero-extra-cost control makes software and always-hardware equivalent and
shows that dynamic overhead can then lose. This is intentional, not a prediction
that real software is free or that real hardware coordination is free.

This model omits batched software messages, data placement on multiple sharded
home GPUs, topology/routing, full packet round trips, gradient exchanges, training
collectives, tensor kernels, cache-set conflicts, TLBs, data DRAM banks and CPU
instruction execution. Software and hardware share one idealized home-record
backing store. Permission records are modeled one-per-64B metadata cache entry;
record packing could change costs. Handoff metadata enumeration is included in
the configurable barrier/record-transfer budget rather than individually probing
the finite home cache for every transferred record. Line and region locks are abstract protocol
reservations rather than concrete ISA/software synchronization instructions.

Results are conditional performance comparisons of explicit reference designs.
They do not establish superiority to real non-coherent DLRM frameworks or an
optimal policy. The original address distributions remain available to motivate
and evaluate alternative choices.

## Relevant sources

[PyTorch's TorchRec tutorial](https://docs.pytorch.org/tutorials/intermediate/torchrec_intro_tutorial.html)
describes GPU table sharding and explicit collective communication. This reference
does not reproduce that deployment; the source motivates keeping that distinction
clear. [A Primer on Memory Consistency and Cache Coherence](https://pages.cs.wisc.edu/~markhill/papers/primer2020_2nd_edition.pdf)
describes permissions, owner/sharer bookkeeping and coarse tracking. Neither
source validates the simulator's selected nanosecond values or its speedups.
