#pragma once

#include "coherence/types.hpp"

#include <optional>
#include <vector>

namespace coherence {

struct PredictorConfig {
    std::uint64_t private_threshold = 8;
    std::uint64_t shared_threshold = 8;
    std::uint64_t freeze_threshold = 16;
    std::uint64_t cooldown = 16;
    void validate() const;
};

struct History {
    NodeId last_node = no_owner;
    std::uint64_t same_node_run = 0, read_run = 0, reader_mask = 0;
    std::uint64_t ownership_changes = 0, cooldown_remaining = 0;
    std::uint64_t private_phase = 0, readonly_phase = 0;
    std::uint64_t longest_private_phase = 0, longest_readonly_phase = 0;
};

class SharingPredictor {
public:
    SharingPredictor(PredictorConfig config, std::size_t regions);
    // History only: no Request (which also carries oracle annotations) is accepted.
    std::optional<Hint> observe(RegionId region, NodeId node, Operation operation,
                               MemoryKind kind, Mode mode, bool recovered);
    const History& history(RegionId region) const { return histories_.at(region); }

private:
    PredictorConfig config_;
    std::vector<History> histories_;
};

} // namespace coherence
