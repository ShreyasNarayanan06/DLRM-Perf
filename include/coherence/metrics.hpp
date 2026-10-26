#pragma once

#include <array>
#include <cstdint>

namespace coherence {

struct Metrics {
    std::uint64_t requests = 0, reads = 0, writes = 0;
    std::uint64_t directory_lookups = 0, snoops = 0, invalidations = 0;
    std::uint64_t region_controls = 0, region_revocations = 0;
    std::uint64_t data_fills = 0, writebacks = 0, transition_flushes = 0;
    std::uint64_t cache_hits = 0, transitions = 0;
    std::uint64_t recoveries = 0, private_violations = 0, readonly_violations = 0;
    std::uint64_t data_cycles = 0, coherence_cycles = 0;
    std::uint64_t transition_cycles = 0, recovery_cycles = 0;
    std::uint64_t misprediction_penalty_cycles = 0; // subset of total, not additive
    std::uint64_t read_checksum = 14695981039346656037ULL;
    std::array<std::uint64_t, 3> mode_accesses{};
    std::array<std::array<std::uint64_t, 3>, 3> transition_matrix{};

    std::uint64_t coherence_events() const {
        return directory_lookups + snoops + invalidations + region_controls;
    }
    std::uint64_t total_cycles() const {
        return data_cycles + coherence_cycles + transition_cycles + recovery_cycles;
    }
};

} // namespace coherence
