#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace perf {
// Diagnostic generator labels only. Cache/directory decisions and distributions
// never depend on these labels.
enum class AccessKind { Metadata, PrefixKV, LeafKV, HotEmbeddings, ColdEmbeddings, PrivateScratchpad };
constexpr std::size_t kind_count=6;
constexpr std::array<AccessKind,kind_count> all_kinds{{AccessKind::Metadata,AccessKind::PrefixKV,
    AccessKind::LeafKV,AccessKind::HotEmbeddings,AccessKind::ColdEmbeddings,AccessKind::PrivateScratchpad}};
inline std::size_t index(AccessKind kind) { return static_cast<std::size_t>(kind); }
const char* name(AccessKind kind);
enum class WorkloadMode { RadixTree, DLRM };
const char* name(WorkloadMode mode);
constexpr std::uint64_t embedding_base=1ULL<<44;

struct Config {
    WorkloadMode workload=WorkloadMode::DLRM;
    std::size_t agents=3,lead_steps=32,rounds=64,warmup_rounds=0;
    std::size_t prefix_reads=32,leaf_writes=8,metadata_ops=2;
    std::size_t prefix_lines=64,leaf_lines=128,stores_per_line=4;
    std::size_t cache_bytes=64*1024,outstanding=8;
    double compute_ns=200,host_compute_ns=50,overlap_trigger=.5;
    double hit_ns=2,memory_ns=150,peer_ns=150,bandwidth_gbps=32;
    double directory_ns=20,snoop_ns=20,invalidate_ns=12,writeback_ns=10;
    double directory_bytes=16,snoop_bytes=16,invalidate_bytes=16;
    std::size_t directory_entries=1024,directory_banks=4,directory_mshrs=16;
    double directory_issue_ns=2,directory_miss_ns=80,directory_backing_gbps=32;
    std::size_t table_lines=4096,sparse_reads=32,scratch_lines=128,scratch_reads=2,scratch_writes=8;
    double hot_fraction=.05,hot_read_fraction=.8,zipf_alpha=1.1,bottom_fraction=.4;
    std::size_t hot_update_every=4,hot_updates=2,cold_update_every=32,cold_updates=1;
    std::uint64_t seed=42,layout_seed=7;
    bool capture_events=false;
    void validate() const;
};
struct Traffic {
    std::uint64_t accesses=0,reads=0,writes=0,hits=0,fills=0;
    std::uint64_t directory=0,snoops=0,invalidations=0,writebacks=0;
    std::uint64_t evictions=0,dirty_evictions=0;
    Traffic& operator+=(const Traffic& other);
};
struct Access { AccessKind kind=AccessKind::Metadata; std::uint64_t address=0; bool write=false; };
struct Step {
    double compute_ns=0;
    std::vector<Access> accesses;
    bool allow_lookahead=true;
    double concurrent_compute_ns=0;
    int sync_node=-1;
    std::size_t sync_step=0;
};
struct Stream { std::size_t node=0; std::vector<Step> steps; };
struct Phase { std::string label; std::vector<Stream> streams; bool measured=true; };
using Workload=std::vector<Phase>;
struct DirectoryStats {
    std::uint64_t requests=0,hits=0,misses=0,merged=0,evictions=0,read_bytes=0,write_bytes=0;
    double bank_wait_ns=0,mshr_wait_ns=0,capacity_wait_ns=0,bank_busy_ns=0;
    double backing_busy_ns=0,backing_wait_ns=0,service_ns=0;
    std::size_t peak_misses=0,peak_entries=0;
    DirectoryStats& operator+=(const DirectoryStats& other);
    DirectoryStats operator-(const DirectoryStats& other) const;
};
struct DirectoryOp { std::uint64_t address=0; AccessKind kind=AccessKind::Metadata; bool eviction=false; };
struct Transaction {
    std::size_t node=0; Access access;
    Traffic traffic;
    bool retained=false,has_victim=false,peer_fill=false,measured=true;
    std::uint64_t victim=0,version=0;
    AccessKind victim_kind=AccessKind::Metadata;
    bool victim_dirty=false;
    std::vector<DirectoryOp> operations;
};
struct EventRecord {
    std::uint64_t id=0,admission_order=0,address=0,version=0,victim=0;
    std::size_t node=0; bool write=false,measured=true,has_victim=false;
    AccessKind kind=AccessKind::Metadata;
    double issued_ns=0,admitted_ns=0,completed_ns=0;
    Traffic traffic;
    std::uint64_t demand_misses=0,eviction_misses=0;
};
struct PhaseTiming { std::string label; double start_ns=0,end_ns=0; bool measured=true; };
struct Result {
    WorkloadMode workload=WorkloadMode::DLRM;
    std::uint64_t trace_fingerprint=0;
    double execution_ns=0,measured_execution_ns=0;
    double worker_compute_ns=0,worker_memory_wait_ns=0,overlap_ns=0,host_sync_wait_ns=0;
    double link_busy_ns=0,queue_wait_ns=0,data_bytes=0,writeback_bytes=0,control_bytes=0;
    double dependency_wait_ns=0;
    std::size_t peak_inflight=0,peak_cache_lines=0;
    std::array<Traffic,kind_count> kinds{},measured_kinds{};
    std::array<DirectoryStats,kind_count> directory{},measured_directory{};
    std::vector<PhaseTiming> phases;
    std::vector<EventRecord> events;
};
Workload make_workload(const Config& config);
std::size_t hot_lines(const Config& config);
std::vector<std::uint64_t> embedding_rows(const Config& config);
std::uint64_t workload_fingerprint(const Workload& workload);
} // namespace perf
