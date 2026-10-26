#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace perf {
enum class AccessKind { Metadata, PrefixKV, LeafKV, HotEmbeddings, ColdEmbeddings, PrivateScratchpad };
constexpr std::size_t kind_count = 6;
constexpr std::array<AccessKind, kind_count> all_kinds{{AccessKind::Metadata, AccessKind::PrefixKV,
    AccessKind::LeafKV, AccessKind::HotEmbeddings, AccessKind::ColdEmbeddings, AccessKind::PrivateScratchpad}};
enum class WorkloadMode { RadixTree, DLRM };
enum class Strategy { EventBudget, SelectiveBypass, CoarseAll };
inline std::size_t index(AccessKind kind) { return static_cast<std::size_t>(kind); }
const char* name(AccessKind kind);
const char* name(WorkloadMode mode);

struct Config {
    WorkloadMode workload = WorkloadMode::RadixTree;
    std::size_t agents = 3, lead_steps = 32, rounds = 64;
    std::size_t prefix_reads = 32, leaf_writes = 8, metadata_ops = 2;
    std::size_t prefix_lines = 64, leaf_lines = 128, stores_per_line = 4;
    std::size_t cache_bytes = 64 * 1024, outstanding = 8;
    double compute_ns = 200, host_compute_ns = 50;
    double hit_ns = 2, memory_ns = 150, peer_ns = 150, bandwidth_gbps = 32;
    double directory_ns = 20, snoop_ns = 20, invalidate_ns = 12, writeback_ns = 10;
    double directory_bytes = 16, snoop_bytes = 16, invalidate_bytes = 16;
    double overlap_trigger = .5;
    double fork_barrier_ns = 200, region_barrier_ns = 40, setup_ns = 0;
    std::size_t region_bytes = 4096;
    bool flush_at_fork = true;
    double recovery_rate = .01, recovery_ns = 200, recovery_bytes = 32;
    double added_latency_ns = 0;
    std::size_t table_lines = 4096, sparse_reads = 32;
    std::size_t scratch_lines = 128, scratch_reads = 2, scratch_writes = 8;
    double hot_fraction = .05, hot_read_fraction = .8, zipf_alpha = 1.1, bottom_fraction = .4;
    std::size_t hot_update_every = 4, hot_updates = 2, cold_update_every = 32, cold_updates = 1;
    std::uint64_t seed = 42;
    bool directory_model = true;
    std::size_t directory_entries = 1024, directory_banks = 4, directory_mshrs = 16;
    double directory_issue_ns = 2, directory_miss_ns = 80, directory_backing_gbps = 32;
    std::size_t warmup_rounds = 0;
    void validate() const;
};
struct Traffic {
    std::uint64_t accesses = 0, reads = 0, writes = 0, hits = 0, fills = 0;
    std::uint64_t directory = 0, snoops = 0, invalidations = 0, writebacks = 0;
    std::uint64_t evictions = 0, dirty_evictions = 0;
    std::uint64_t region_grants = 0, region_barriers = 0, region_recoveries = 0, region_downgrades = 0;
    std::uint64_t revoked_copies = 0, collateral_copies = 0, region_flushes = 0;
    Traffic& operator+=(const Traffic& other);
    Traffic operator-(const Traffic& other) const;
};
struct Service {
    Traffic traffic;
    bool peer_fill = false;
    double region_latency_ns = 0, region_control_bytes = 0;
};
struct Access {
    AccessKind kind = AccessKind::Metadata;
    std::uint64_t address = 0;
    bool write = false;
    Service service;
};
struct Step {
    double compute_ns = 0;
    std::vector<Access> accesses;
    bool allow_lookahead = true;
    double concurrent_compute_ns = 0;
    int sync_node = -1;
    std::size_t sync_step = 0;
};
struct Stream { std::size_t node = 0; std::vector<Step> steps; };
struct Phase { std::string label; std::vector<Stream> streams; bool fork_boundary = false; bool measured = true; };
using Workload = std::vector<Phase>;
struct Profile {
    Workload workload;
    std::array<Traffic, kind_count> kinds{}, measured_kinds{};
    std::uint64_t fork_dirty_lines = 0, fork_flushes = 0, prefix_regions = 0;
};
struct Scenario {
    std::string label;
    double latency_retained = 1, traffic_retained = 1;
    bool counterfactual = false;
    Strategy strategy = Strategy::EventBudget;
};
double latency_fraction(const Scenario& scenario, AccessKind kind);
double traffic_fraction(const Scenario& scenario, AccessKind kind);
bool bypass_kind(const Scenario& scenario, AccessKind kind);
struct DirectoryStats {
    std::uint64_t requests = 0, hits = 0, misses = 0, merged = 0, evictions = 0;
    std::uint64_t read_bytes = 0, write_bytes = 0;
    double bank_wait_ns = 0, mshr_wait_ns = 0, capacity_wait_ns = 0;
    double bank_busy_ns = 0, backing_busy_ns = 0, backing_wait_ns = 0, service_ns = 0;
    std::size_t peak_misses = 0, peak_entries = 0;
    DirectoryStats& operator+=(const DirectoryStats& other);
    DirectoryStats operator-(const DirectoryStats& other) const;
};
struct PhaseTiming { std::string label; double start_ns = 0, end_ns = 0; bool measured = true; };
struct Result {
    Scenario scenario;
    WorkloadMode workload = WorkloadMode::RadixTree;
    std::uint64_t trace_fingerprint = 0;
    double measured_execution_ns = 0;
    double execution_ns = 0, worker_compute_ns = 0, worker_memory_wait_ns = 0, overlap_ns = 0;
    double link_busy_ns = 0, queue_wait_ns = 0, data_bytes = 0, writeback_bytes = 0, control_bytes = 0;
    double assumed_coherence_latency_ns = 0, added_latency_ns = 0, expected_recoveries = 0;
    double fork_penalty_ns = 0;
    double region_penalty_ns = 0, host_sync_wait_ns = 0;
    double charged_directory = 0, charged_snoops = 0, charged_invalidations = 0;
    std::uint64_t requests = 0, offchip_requests = 0, fork_flushes = 0, prefix_regions = 0;
    std::size_t peak_inflight = 0;
    std::array<Traffic, kind_count> kinds{}, measured_kinds{};
    std::array<DirectoryStats, kind_count> directory{}, measured_directory{};
    std::vector<PhaseTiming> phases;
};
Workload make_workload(const Config& config);
Profile profile_workload(const Workload& workload, const Config& config, const Scenario& scenario);
std::vector<Scenario> default_scenarios(WorkloadMode mode = WorkloadMode::RadixTree);
std::size_t hot_lines(const Config& config);
std::uint64_t workload_fingerprint(const Workload& workload);
Result simulate(const Workload& workload, const Config& config, const Scenario& scenario);
} // namespace perf
