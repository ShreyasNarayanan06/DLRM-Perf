#pragma once
#include "histograms.hpp"
namespace perf {
class ManagementModel;
Result simulate(const Workload& workload,const Config& config,BlindProfiler& profiler,ManagementModel* management=nullptr);
} // namespace perf
