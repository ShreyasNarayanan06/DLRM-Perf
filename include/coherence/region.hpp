#pragma once

#include "coherence/metrics.hpp"
#include "coherence/types.hpp"

#include <iosfwd>
#include <vector>

namespace coherence {

struct CacheCopy {
    bool valid = false;
    bool dirty = false;
    std::uint64_t version = 0;
};

struct Line {
    std::uint64_t memory_version = 0;
    std::uint64_t latest_version = 0;
    NodeId exclusive_owner = no_owner; // E/M permission in coherent mode
    std::vector<CacheCopy> copies;
};

struct Region {
    RegionSpec spec;
    Mode mode = Mode::Coherent;
    NodeId owner = no_owner;
    std::vector<Line> lines;
};

struct AccessResult {
    std::uint64_t version = 0;
    bool recovered = false;
};

// This class is the only place that changes coherence permissions or data state.
class RegionStateMachine {
public:
    RegionStateMachine(SystemConfig config, const std::vector<RegionSpec>& specs,
                       Metrics& metrics, Policy policy, std::ostream* log = nullptr);
    AccessResult access(const Request& request);
    void apply_hint(RegionId id, Hint hint, std::uint64_t sequence);
    bool evict(NodeId node, RegionId id, std::size_t line);
    void writeback_region(RegionId id);
    const Region& region(RegionId id) const { return regions_.at(id); }
    void check_region(RegionId id) const;

private:
    void transition(RegionId id, Mode target, NodeId owner,
                    const char* reason, std::uint64_t sequence);
    void flush(Line& line, bool during_transition);
    std::uint64_t coherent_access(Region& region, const Request& request);
    std::uint64_t bypass_access(Region& region, const Request& request);
    void fill(Line& line, NodeId node);
    void local_hit();

    SystemConfig config_;
    std::vector<Region> regions_;
    Metrics& metrics_;
    Policy policy_;
    std::ostream* log_;
};

} // namespace coherence
