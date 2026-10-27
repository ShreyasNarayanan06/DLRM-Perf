# DLRM shared-memory experiment

**What are we testing?** When several accelerators use the same embedding table,
can hardware save time by keeping their local copies up to date, instead of
leaving that work to software? Does choosing where to use hardware help?

This C++17 simulator estimates the time and communication needed for that work.
It does not run a real DLRM model on GPUs.

## What we compare

| System | How it keeps copies up to date |
|---|---|
| **Software-managed baseline** | Hardware does not automatically coordinate copies. Software tracks who has them and sends requests when sharing or writes require coordination. Valid cached reads stay fast. |
| **Always-on hardware coherence** | Hardware automatically tracks sharing and writes for individual cache lines. This is an additional reference. |
| **Dynamic reference** | Starts with software management. When a region repeatedly needs coordination across nodes, it enables hardware tracking there. It returns to software after a quiet period. |

All three must return correct, current data. “Non-coherent” does not mean old
values are allowed. The dynamic approach is a replaceable example, not a final
research policy.

## What the workload does

Three accelerators process recommendation batches. Each batch gathers embedding
rows, represents the dense MLP and combining work with a compute delay, and uses
private scratch space. Individual tensor operations are not simulated.

About 80% of embedding reads target a popular 5% of the rows. Popular and less
popular rows are mixed throughout the table. A CPU updates some rows while the
accelerators run. This represents **serving recommendations with concurrent
updates**, rather than a complete multi-GPU training job.

Every system gets the same request program, cache sizes, link speed and compute
work. Some work can overlap. The simulator charges for communication, waiting,
software handling and switching a region's manager, including writing back any
changed data first. It checks that reads do not use outdated copies.

## Current results

The default comparison uses 512 batches and 4KB regions. The first 128 batches
warm the caches; the table shows the remaining simulated execution time.

| System | Time (microseconds) | Less time than software baseline |
|---|---:|---:|
| Software-managed baseline | 409.40 | 0.0% |
| Always-on hardware coherence | 220.96 | 46.0% |
| Dynamic reference | 246.78 | 39.7% |

**The software-cost assumption matters.** Here, each request handled by the
software copy manager needs an extra **100 ns** of processing, plus a message
envelope. These costs are inputs, not measurements of today's GPU systems.
At **25 ns**, the dynamic saving falls to **6.9%**. If both extra software costs
are zero, dynamic is **3.4% slower**. With 256-byte regions and the same tracking
capacity, it is **1.3% slower** under the default software cost.

Always-on hardware is fastest in the default case. These results show a possible
benefit over one software-managed shared-memory design; they are **not measured
speedups over TorchRec, NCCL or a production GPU fleet**.

[All results and controls](COMPARISON_RESULTS.md) · [Detailed assumptions](COMPARISON_MODEL.md)

## Run the experiment

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
mkdir -p results/management
build/perf/architecture_perf --management compare --rounds 512 --warmup-rounds 128 \
    --events 1 --output-prefix results/management/example
```

The terminal prints the three times. CSV files record cache use, communication,
software/hardware tracking work and region changes. Positive “less time” means an
improvement; negative means a slowdown.

On Great Lakes, submit from this repository's directory:

```bash
sbatch perf/run_comparison_slurm.sh
```

This builds, tests, runs all **17 comparisons** and audits the results in
`results/management/`. Local reproduction commands are in the results document.

## Further reading

The original profiler and Multi-Agent RadixTree workload remain available.

[Original profiles](PERFORMANCE_RESULTS.md) · [Access-skew check](SKEW_PROFILE.md) · [Profiler details](MODEL.md)

Validation: **14/14 tests passed** on Great Lakes (job **64831183**, gl3018) and
under local memory/error checks. The audit verifies fresh reads and communication
accounting. All **550 cluster output files** match the local results.
