#pragma once
#include "simulator.hpp"
namespace perf {
void write_report(const std::string& prefix,const Config& config,const Result& result,const BlindProfiler& profiler);
void print_result(const Config& config,const Result& result,const BlindProfiler& profiler);
} // namespace perf
