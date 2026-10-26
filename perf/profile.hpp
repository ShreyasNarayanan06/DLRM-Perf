#pragma once
#include "model.hpp"
#include <list>
#include <unordered_map>

namespace perf {
// Incremental baseline MESI data state. No page hints, hot/cold rules or trace
// prepass. A successful admission reserves cache capacity and compatible locks;
// permissions/data become visible only at complete().
class BaselineMemory {
public:
    explicit BaselineMemory(const Config& config);
    bool admit(std::size_t node,const Access& access,Transaction& transaction,std::uint64_t ticket=1);
    std::uint64_t complete(const Transaction& transaction);
    std::size_t peak_cache_lines() const { return peak_cache_lines_; }
private:
    struct Line {
        std::uint64_t holders=0,latest=0,memory=0,waiting_ticket=0;
        int owner=-1;
        bool dirty=false,exclusive=false;
        std::size_t readers=0;
        AccessKind label=AccessKind::Metadata;
        std::vector<std::uint64_t> versions;
    };
    struct Cache {
        std::list<std::uint64_t> lru;
        std::unordered_map<std::uint64_t,std::list<std::uint64_t>::iterator> entries;
        std::size_t reserved=0;
    };
    Line& line(std::uint64_t address,AccessKind label);
    void erase(std::size_t node,std::uint64_t address);
    void check(const Line& line) const;
    const Config& c_;
    std::unordered_map<std::uint64_t,Line> lines_;
    std::vector<Cache> caches_;
    std::size_t peak_cache_lines_=0;
};
void accumulate(Result& result,const Transaction& transaction);
} // namespace perf
