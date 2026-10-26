#include "histograms.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace perf {
const char* name(RunType type) {
    switch(type) {
    case RunType::Closed:return "closed";case RunType::LeftCensored:return "left_censored";
    case RunType::RightCensored:return "right_censored";case RunType::BothCensored:return "both_censored";
    }
    throw std::logic_error("invalid run type");
}
std::size_t bit_count(std::uint64_t value) { std::size_t n=0;while(value!=0){value&=value-1;++n;}return n; }
bool BlindProfiler::embeddings(std::uint64_t address) const {
    return c_.workload==WorkloadMode::DLRM && address>=embedding_base && address-embedding_base<c_.table_lines*64;
}
int BlindProfiler::time_bin(double duration) {
    return duration==0 ? std::numeric_limits<int>::min() : static_cast<int>(std::floor(std::log2(duration)));
}
void BlindProfiler::begin_phase(double time,bool measured) {
    for(std::size_t s=0;s<2;++s) if(s==0 || measured) {
        auto& w=windows_[s];if(!w.started){w.started=true;w.start_ns=time;}
    }
}
void BlindProfiler::end_phase(double time,bool measured) {
    windows_[0].end_ns=time;if(measured)windows_[1].end_ns=time;
}
void BlindProfiler::access(double time,std::size_t node,std::uint64_t address,bool write,bool measured) {
    for(std::size_t scope=0;scope<2;++scope) if(scope==0 || measured)
        for(std::size_t space=0;space<spaces();++space) if(space==0 || embeddings(address))
            for(std::size_t scale=0;scale<block_sizes.size();++scale) {
                auto& window=windows_[scope];auto& view=window.views[space][scale];
                const auto bytes=block_sizes[scale];auto& block=view.blocks[address/bytes];
                block.nodes|=1ULL<<node;const auto bit=1ULL<<((address%bytes)/64);
                if(write) {
                    const auto type=block.writes==0 ? RunType::LeftCensored : RunType::Closed;
                    const auto start=block.writes==0 ? window.start_ns : block.last_write_ns;
                    ++view.reads[type][block.reads_since_write];++view.intervals[type][time_bin(time-start)];
                    ++block.writes;block.written_lines|=bit;block.reads_since_write=0;block.last_write_ns=time;
                } else {++block.reads;block.read_lines|=bit;++block.reads_since_write;}
            }
}
void BlindProfiler::directory_miss(std::uint64_t address,bool eviction,bool measured) {
    for(std::size_t scope=0;scope<2;++scope) if(scope==0 || measured)
        for(std::size_t space=0;space<spaces();++space) if(space==0 || embeddings(address))
            for(std::size_t scale=0;scale<block_sizes.size();++scale) {
                auto& block=windows_[scope].views[space][scale].blocks[address/block_sizes[scale]];
                if(eviction)++block.eviction_misses;else ++block.demand_misses;
            }
}
std::pair<ReadRuns,TimeRuns> BlindProfiler::runs(const AddressView& view,double start,double end) {
    auto reads=view.reads;auto intervals=view.intervals;
    for(const auto& entry:view.blocks) {
        const auto& b=entry.second;if(b.reads+b.writes==0)continue;
        const auto type=b.writes==0 ? RunType::BothCensored : RunType::RightCensored;
        ++reads[type][b.reads_since_write];
        ++intervals[type][time_bin(end-(b.writes==0 ? start : b.last_write_ns))];
    }
    return {std::move(reads),std::move(intervals)};
}
} // namespace perf
