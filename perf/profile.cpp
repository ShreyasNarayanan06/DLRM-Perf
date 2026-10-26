#include "profile.hpp"
#include <algorithm>
#include <stdexcept>

namespace perf {
Traffic& Traffic::operator+=(const Traffic& r) {
    accesses+=r.accesses; reads+=r.reads; writes+=r.writes; hits+=r.hits; fills+=r.fills;
    directory+=r.directory; snoops+=r.snoops; invalidations+=r.invalidations; writebacks+=r.writebacks;
    evictions+=r.evictions; dirty_evictions+=r.dirty_evictions; return *this;
}
BaselineMemory::BaselineMemory(const Config& c):c_(c),caches_(c.agents+1) {}
BaselineMemory::Line& BaselineMemory::line(std::uint64_t address,AccessKind label) {
    auto inserted=lines_.try_emplace(address);
    auto& value=inserted.first->second;
    if(inserted.second) { value.label=label; value.versions.resize(c_.agents+1,0); }
    return value;
}
void BaselineMemory::erase(std::size_t node,std::uint64_t address) {
    auto& cache=caches_[node];const auto found=cache.entries.find(address);
    if(found!=cache.entries.end()) {cache.lru.erase(found->second);cache.entries.erase(found);}
}
void BaselineMemory::check(const Line& value) const {
    if(value.owner>=0) {
        const auto bit=1ULL<<static_cast<unsigned>(value.owner);
        if(value.holders!=bit) throw std::logic_error("exclusive owner has invalid sharers");
    } else if(value.dirty) throw std::logic_error("dirty line has no owner");
    if(!value.dirty && value.memory!=value.latest) throw std::logic_error("clean memory is stale");
    for(std::size_t n=0;n<=c_.agents;++n)
        if((value.holders&(1ULL<<n))!=0 && value.versions[n]!=value.latest)
            throw std::logic_error("cached data is stale");
}
bool BaselineMemory::admit(std::size_t node,const Access& access,Transaction& transaction,std::uint64_t ticket) {
    if(node>c_.agents || access.address%64!=0) throw std::invalid_argument("invalid node/alignment");
    auto& target=line(access.address,access.kind);
    const auto bit=1ULL<<node;
    const bool local=(target.holders&bit)!=0;
    const bool read_hit=local && !access.write;
    if(target.waiting_ticket!=0 && target.waiting_ticket!=ticket) return false;
    if(target.exclusive || (!read_hit && target.readers!=0)) {
        if(!read_hit && target.waiting_ticket==0) target.waiting_ticket=ticket;
        return false;
    }
    auto& cache=caches_[node];
    const auto capacity=c_.cache_bytes/64;
    Transaction tx;tx.node=node;tx.access=access;tx.retained=capacity!=0;
    auto& t=tx.traffic;t.accesses=1;t.reads=access.write ? 0 : 1;t.writes=access.write ? 1 : 0;
    t.hits=local ? 1 : 0;t.fills=local ? 0 : 1;
    if(!local && capacity!=0 && cache.entries.size()+cache.reserved>=capacity) {
        auto victim=cache.lru.rbegin();
        for(;victim!=cache.lru.rend();++victim) {
            const auto& candidate=lines_.at(*victim);
            if(!candidate.exclusive && candidate.readers==0) break;
        }
        if(victim==cache.lru.rend()) return false;
        tx.has_victim=true;tx.victim=*victim;
        auto& value=lines_.at(tx.victim);
        tx.victim_dirty=value.dirty && value.owner==static_cast<int>(node);
        tx.victim_kind=value.label;value.exclusive=true;
        t.evictions=1;t.dirty_evictions=tx.victim_dirty ? 1 : 0;
        t.writebacks+=t.dirty_evictions;
        tx.operations.push_back({tx.victim,value.label,true});
        erase(node,tx.victim);
    }
    if(target.waiting_ticket==ticket) target.waiting_ticket=0;
    if(read_hit) ++target.readers;else target.exclusive=true;
    if(!local && capacity!=0) ++cache.reserved;
    if(local) cache.lru.splice(cache.lru.begin(),cache.lru,cache.entries.at(access.address));
    const bool permission=!local || (access.write && target.owner!=static_cast<int>(node));
    if(permission) {
        t.directory=1;tx.operations.push_back({access.address,access.kind,false});
        const bool remote_owner=target.owner>=0 && target.owner!=static_cast<int>(node);
        tx.peer_fill=remote_owner;
        t.snoops=remote_owner ? 1 : 0;
        if(remote_owner && target.dirty) ++t.writebacks;
        if(access.write) for(std::size_t n=0;n<=c_.agents;++n)
            if(n!=node && (target.holders&(1ULL<<n))!=0) ++t.invalidations;
    }
    if(!tx.retained && access.write) ++t.writebacks; // cache-disabled write-through
    peak_cache_lines_=std::max(peak_cache_lines_,cache.entries.size()+cache.reserved);
    if(capacity!=0 && cache.entries.size()+cache.reserved>capacity) throw std::logic_error("cache overcommitted");
    transaction=std::move(tx);return true;
}
std::uint64_t BaselineMemory::complete(const Transaction& tx) {
    const auto node=tx.node;const auto bit=1ULL<<node;
    if(tx.has_victim) {
        auto& victim=lines_.at(tx.victim);
        if(tx.victim_dirty) {victim.memory=victim.latest;victim.dirty=false;}
        victim.holders&=~bit;
        if(victim.owner==static_cast<int>(node)) victim.owner=-1;
        victim.exclusive=false;check(victim);
    }
    auto& target=lines_.at(tx.access.address);
    const bool local=(target.holders&bit)!=0;
    const auto source=local ? target.versions[node] :
        (target.dirty ? target.versions.at(static_cast<std::size_t>(target.owner)) : target.memory);
    if(source!=target.latest) throw std::logic_error("read/fill source has stale version");
    if(target.owner>=0 && target.owner!=static_cast<int>(node)) {
        if(target.dirty) {target.memory=target.latest;target.dirty=false;}
        target.owner=-1;
    }
    if(tx.access.write) {
        for(std::size_t n=0;n<=c_.agents;++n) if(n!=node && (target.holders&(1ULL<<n))!=0) erase(n,tx.access.address);
        target.holders=tx.retained ? bit : 0;
        ++target.latest;target.owner=tx.retained ? static_cast<int>(node) : -1;
        target.dirty=tx.retained;
        if(!tx.retained) target.memory=target.latest;
    } else if(tx.retained) {
        if(target.holders==0) target.owner=static_cast<int>(node);
        target.holders|=bit;
    }
    if(tx.retained) {
        target.versions[node]=target.latest;
        if(!local) {
            auto& cache=caches_[node];--cache.reserved;
            cache.lru.push_front(tx.access.address);cache.entries.emplace(tx.access.address,cache.lru.begin());
        }
    }
    if(tx.traffic.hits!=0 && !tx.access.write) --target.readers;else target.exclusive=false;
    check(target);return target.latest;
}
void accumulate(Result& r,const Transaction& tx) {
    const auto add=[&](std::array<Traffic,kind_count>& kinds) {
        auto target=tx.traffic;
        if(tx.has_victim) {
            --target.evictions;
            if(tx.victim_dirty) {--target.writebacks;--target.dirty_evictions;}
            auto& victim=kinds[index(tx.victim_kind)];++victim.evictions;
            if(tx.victim_dirty) {++victim.dirty_evictions;++victim.writebacks;}
        }
        kinds[index(tx.access.kind)]+=target;
    };
    add(r.kinds);if(tx.measured)add(r.measured_kinds);
}
} // namespace perf
