#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace coherence {

using NodeId = int;                 // 0 = host, 1..N = accelerators
using RegionId = std::size_t;
constexpr NodeId no_owner = -1;
constexpr std::size_t line_bytes = 64;

enum class Mode { Coherent, Private, SharedReadOnly };
enum class MemoryKind { Metadata, KV };
enum class Operation { Read, Write };
enum class Policy { Baseline, Dynamic, Oracle };

inline const char* name(Mode mode) {
    switch (mode) {
    case Mode::Coherent: return "coherent";
    case Mode::Private: return "private";
    case Mode::SharedReadOnly: return "shared_ro";
    }
    throw std::logic_error("invalid mode");
}
inline const char* name(Policy policy) {
    switch (policy) {
    case Policy::Baseline: return "Always coherent";
    case Policy::Dynamic: return "Dynamic predictor";
    case Policy::Oracle: return "Perfect oracle";
    }
    throw std::logic_error("invalid policy");
}
inline std::size_t index(Mode mode) { return static_cast<std::size_t>(mode); }

struct Costs {
    std::uint64_t local_hit = 2;
    std::uint64_t data_fill = 80;
    std::uint64_t directory = 24;
    std::uint64_t snoop = 32;
    std::uint64_t invalidate = 24;
    std::uint64_t writeback = 80;
    std::uint64_t region_control = 40;
    std::uint64_t recovery = 160;
};

struct SystemConfig {
    std::size_t accelerators = 4;
    std::size_t region_bytes = 4096;
    Costs costs;
    bool check_invariants = true;

    std::size_t lines_per_region() const { return region_bytes / line_bytes; }
    void validate() const {
        if (accelerators == 0 || accelerators > 63)
            throw std::invalid_argument("accelerators must be in [1, 63]");
        if (region_bytes == 0 || region_bytes % line_bytes != 0)
            throw std::invalid_argument("region bytes must be a positive multiple of 64");
    }
};

struct RegionSpec {
    MemoryKind kind = MemoryKind::KV;
    std::string label;
};

struct Request {
    std::uint64_t sequence = 0;
    NodeId node = 0;
    RegionId region = 0;
    std::size_t line = 0;
    Operation operation = Operation::Read;
    std::uint64_t expected_version = 0; // independent reference trace
    Mode oracle_mode = Mode::Coherent; // never passed to the predictor
    NodeId oracle_owner = no_owner;
    std::string phase;
};

struct Hint {
    Mode mode = Mode::Coherent;
    NodeId owner = no_owner;
    const char* reason = "none";
};

} // namespace coherence
