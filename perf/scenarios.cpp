#include "model.hpp"

namespace perf {
const char* name(WorkloadMode mode) { return mode == WorkloadMode::DLRM ? "dlrm" : "radix"; }
bool bypass_kind(const Scenario& s, AccessKind kind) {
    if (s.strategy == Strategy::CoarseAll) return true;
    return s.strategy == Strategy::SelectiveBypass &&
        (kind == AccessKind::ColdEmbeddings || kind == AccessKind::PrivateScratchpad ||
         kind == AccessKind::PrefixKV || kind == AccessKind::LeafKV);
}
double latency_fraction(const Scenario& s, AccessKind kind) {
    if (s.strategy != Strategy::EventBudget) return bypass_kind(s, kind) ? 0 : 1;
    return kind == AccessKind::Metadata ? 1 : s.latency_retained;
}
double traffic_fraction(const Scenario& s, AccessKind kind) {
    if (s.strategy != Strategy::EventBudget) return bypass_kind(s, kind) ? 0 : 1;
    return kind == AccessKind::Metadata ? 1 : s.traffic_retained;
}
std::vector<Scenario> default_scenarios(WorkloadMode mode) {
    return {{"Baseline Always-Coherent", 1, 1, false},
        {mode == WorkloadMode::DLRM ? "Cold RO + Private Scratchpad Bypass" : "Prefix RO + Private Leaf Bypass",
            0, 0, true, Strategy::SelectiveBypass},
        {"Naive Coarse Region Coherence", 0, 0, true, Strategy::CoarseAll},
        {"50% KV/Embedding Overhead Reduction", .5, .5, true}};
}
std::uint64_t workload_fingerprint(const Workload& workload) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto add = [&](std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) { hash ^= (value >> (8 * i)) & 255U; hash *= 1099511628211ULL; }
    };
    add(workload.size());
    for (const auto& phase : workload) {
        add(phase.fork_boundary); add(phase.streams.size());
        for (const auto& stream : phase.streams) {
            add(stream.node); add(stream.steps.size());
            for (const auto& step : stream.steps) {
                // Timing-independent trace identity: compare logical requests/dependencies.
                add(step.accesses.size()); add(step.allow_lookahead);
                add(static_cast<std::uint64_t>(step.sync_node + 1)); add(step.sync_step);
                for (const auto& access : step.accesses) {
                    add(index(access.kind)); add(access.address); add(access.write);
                }
            }
        }
    }
    return hash;
}
} // namespace perf
