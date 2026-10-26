#pragma once
#include "model.hpp"
#include <functional>
#include <list>
#include <queue>
#include <unordered_map>

namespace perf {
// A timing/capacity model for a cached, memory-backed home directory. Evicting
// metadata never invalidates data caches: backing storage preserves permissions.
class DirectoryModel {
public:
    explicit DirectoryModel(const Config& config);
    double reserve(double arrival, std::uint64_t key, AccessKind kind);
    struct Lookup { bool retry; double time; bool miss=false; };
    Lookup lookup(double now, std::uint64_t key, AccessKind kind);
    const std::array<DirectoryStats, kind_count>& stats() const { return stats_; }
    static std::uint64_t line_key(std::uint64_t address) { return address / 64; }
private:
    struct Entry { double ready; std::list<std::uint64_t>::iterator lru; };
    const Config& c_;
    std::vector<double> banks_;
    std::array<DirectoryStats, kind_count> stats_{};
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::list<std::uint64_t> lru_;
    std::priority_queue<double, std::vector<double>, std::greater<double>> misses_;
    double backing_available_ = 0;
};
} // namespace perf
