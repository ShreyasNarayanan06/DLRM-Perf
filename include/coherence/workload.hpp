#pragma once

#include "coherence/types.hpp"

#include <vector>

namespace coherence {

struct WorkloadConfig {
    std::size_t episodes = 4;
    std::size_t subagents = 3;
    std::size_t prefix_regions = 2;
    std::size_t leaf_regions = 1;
    std::size_t lead_writes = 96;
    std::size_t review_reads = 16;
    std::size_t rounds = 128;
    bool stress = false;
    void validate() const;
};

// A compressed path segment is represented by its allocated KV regions.
struct RadixNode {
    std::size_t id = 0;
    std::size_t parent = 0;
    NodeId agent = 0;
    std::vector<std::size_t> children;
    std::vector<RegionId> kv_regions;
    std::size_t reference_count = 0;
};

struct Workload {
    std::vector<RegionSpec> regions;
    std::vector<RadixNode> tree;
    std::vector<Request> requests;
};

Workload make_workload(const SystemConfig& system, const WorkloadConfig& config);

} // namespace coherence
