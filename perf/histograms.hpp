#pragma once
#include "model.hpp"
#include <array>
#include <map>
#include <unordered_map>

namespace perf {
constexpr std::array<std::size_t,4> block_sizes{{64,256,1024,4096}};
enum class RunType { Closed, LeftCensored, RightCensored, BothCensored };
const char* name(RunType type);
struct BlockObservation {
    std::uint64_t reads=0,writes=0,nodes=0,read_lines=0,written_lines=0;
    std::uint64_t reads_since_write=0,demand_misses=0,eviction_misses=0;
    double last_write_ns=0;
};
using ReadRuns=std::map<RunType,std::map<std::uint64_t,std::uint64_t>>;
using TimeRuns=std::map<RunType,std::map<int,std::uint64_t>>;
struct AddressView {
    std::unordered_map<std::uint64_t,BlockObservation> blocks;
    ReadRuns reads;
    TimeRuns intervals;
};
struct ObservationWindow {
    bool started=false;
    double start_ns=0,end_ns=0;
    std::array<std::array<AddressView,4>,2> views; // all-addresses / embeddings
};
class BlindProfiler {
public:
    explicit BlindProfiler(const Config& config):c_(config) {}
    void begin_phase(double time,bool measured);
    void end_phase(double time,bool measured);
    void access(double time,std::size_t node,std::uint64_t address,bool write,bool measured);
    void directory_miss(std::uint64_t address,bool eviction,bool measured);
    const ObservationWindow& window(std::size_t scope) const { return windows_.at(scope); }
    bool embeddings(std::uint64_t address) const;
    std::size_t spaces() const { return c_.workload==WorkloadMode::DLRM ? 2 : 1; }
    static int time_bin(double duration);
    static std::pair<ReadRuns,TimeRuns> runs(const AddressView& view,double start,double end);
private:
    const Config& c_;
    std::array<ObservationWindow,2> windows_; // whole run / measured only
};
std::size_t bit_count(std::uint64_t value);
} // namespace perf
