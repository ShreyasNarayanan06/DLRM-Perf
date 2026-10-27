#pragma once
#include "management.hpp"
namespace perf {
struct ManagedRun {ManagementConfig config;Result result;ManagementStats all,measured;};
void write_comparison(const std::string& path,const Config& hardware,const std::vector<ManagedRun>& runs);
void write_region_changes(const std::string& path,const ManagementModel& manager);
void print_comparison(const std::vector<ManagedRun>& runs);
} // namespace perf
