#pragma once
#include "histograms.hpp"
namespace perf {
Result simulate(const Workload& workload,const Config& config,BlindProfiler& profiler);
} // namespace perf
