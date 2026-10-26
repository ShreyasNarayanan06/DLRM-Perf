#include "coherence/predictor.hpp"

#include <algorithm>
#include <stdexcept>

namespace coherence {

void PredictorConfig::validate() const {
    if (private_threshold == 0 || shared_threshold == 0 || freeze_threshold == 0)
        throw std::invalid_argument("predictor thresholds must be positive");
}

SharingPredictor::SharingPredictor(PredictorConfig config, std::size_t regions)
    : config_(config), histories_(regions) { config_.validate(); }

std::optional<Hint> SharingPredictor::observe(RegionId region, NodeId node,
        Operation operation, MemoryKind kind, Mode mode, bool recovered) {
    if (node < 0 || node > 63) throw std::invalid_argument("predictor node out of range");
    auto& history = histories_.at(region);
    if (history.last_node != no_owner && history.last_node != node)
        ++history.ownership_changes;
    history.same_node_run = history.last_node == node ? history.same_node_run + 1 : 1;
    history.last_node = node;
    if (operation == Operation::Read) {
        ++history.read_run;
        history.reader_mask |= std::uint64_t{1} << node;
    } else {
        history.read_run = 0;
        history.reader_mask = 0;
    }
    history.private_phase = mode == Mode::Private ? history.private_phase + 1 : 0;
    history.readonly_phase = mode == Mode::SharedReadOnly ? history.readonly_phase + 1 : 0;
    history.longest_private_phase = std::max(history.longest_private_phase, history.private_phase);
    history.longest_readonly_phase = std::max(history.longest_readonly_phase, history.readonly_phase);

    if (recovered) {
        history.same_node_run = 0;
        history.read_run = 0;
        history.reader_mask = 0;
        history.cooldown_remaining = config_.cooldown;
        return std::nullopt;
    }
    if (history.cooldown_remaining != 0) {
        --history.cooldown_remaining;
        return std::nullopt;
    }
    if (kind == MemoryKind::Metadata) return std::nullopt;
    if (mode == Mode::Private && history.read_run >= config_.freeze_threshold)
        return Hint{Mode::SharedReadOnly, no_owner, "owner_read_phase"};
    if (mode != Mode::Coherent) return std::nullopt;
    const auto readers = history.reader_mask;
    const bool multiple_readers = readers != 0 && (readers & (readers - 1)) != 0;
    if (history.read_run >= config_.shared_threshold && multiple_readers)
        return Hint{Mode::SharedReadOnly, no_owner, "shared_read_phase"};
    if (history.same_node_run >= config_.private_threshold)
        return Hint{Mode::Private, node, "single_node_phase"};
    return std::nullopt;
}

} // namespace coherence
