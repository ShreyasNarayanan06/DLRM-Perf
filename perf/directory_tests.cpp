#include "directory.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace perf;
namespace {
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
void near(double a,double b) { if (std::abs(a-b)>1e-7) throw std::runtime_error("expected "+std::to_string(b)+", got "+std::to_string(a)); }
Config config() {
    Config c; c.directory_entries=1; c.directory_banks=1; c.directory_mshrs=1;
    c.directory_ns=10; c.directory_miss_ns=50; c.directory_issue_ns=2; c.directory_backing_gbps=64;
    return c;
}
DirectoryStats sum(const DirectoryModel& d) { DirectoryStats s;for (const auto& k:d.stats())s+=k;return s; }
void miss_hit_and_eviction() {
    auto c=config(); DirectoryModel d(c);
    const auto k=AccessKind::ColdEmbeddings;
    near(d.reserve(0,0,1,k),0); near(d.lookup(0,0,1,k).time,61);
    near(d.reserve(62,0,1,k),62); near(d.lookup(62,0,1,k).time,72);
    near(d.reserve(73,2,1,k),73); near(d.lookup(73,2,1,k).time,135);
    const auto s=sum(d);
    require(s.requests==3 && s.misses==2 && s.hits==1 && s.evictions==1,"LRU metadata hit/miss/eviction counters");
    require(s.read_bytes==128 && s.write_bytes==64,"backing traffic did not include dirty eviction exactly once");
    require(s.peak_entries==1 && s.peak_misses==1,"capacity bounds");
}
void bounded_buffers_and_pending_tags() {
    auto c=config(); DirectoryModel d(c); const auto k=AccessKind::ColdEmbeddings;
    d.reserve(0,0,1,k); d.lookup(0,0,1,k);
    near(d.reserve(0,2,1,k),2);
    const auto blocked=d.lookup(2,2,1,k);
    require(blocked.retry,"full miss buffers did not backpressure"); near(blocked.time,61);
    near(d.lookup(61,2,1,k).time,123);
    near(sum(d).mshr_wait_ns,59); near(sum(d).bank_wait_ns,2);
    c.directory_mshrs=8; DirectoryModel tags(c);
    tags.reserve(0,0,1,k); tags.lookup(0,0,1,k);
    tags.reserve(0,2,1,k); const auto full=tags.lookup(2,2,1,k);
    require(full.retry,"pending tag incorrectly evicted"); near(full.time,61); near(sum(tags).capacity_wait_ns,59);
}
void pending_merge_and_budget() {
    auto c=config(); c.directory_entries=4; c.directory_mshrs=4; DirectoryModel d(c);
    const auto k=AccessKind::HotEmbeddings;
    d.reserve(0,0,1,k); d.lookup(0,0,1,k); d.reserve(0,0,1,k);
    near(d.lookup(2,0,1,k).time,61);
    require(sum(d).merged==1 && sum(d).misses==1,"pending fill counted as free hit / duplicate miss");
    DirectoryModel half(c); near(half.reserve(0,0,.5,k),0); near(half.lookup(0,0,.5,k).time,30.5);
    near(sum(half).bank_busy_ns,1); near(sum(half).backing_busy_ns,.5);
}
void cache_disable_and_namespaces() {
    auto c=config(); c.directory_entries=0; DirectoryModel d(c); const auto k=AccessKind::ColdEmbeddings;
    d.reserve(0,0,1,k); d.lookup(0,0,1,k); d.reserve(62,0,1,k); d.lookup(62,0,1,k);
    require(sum(d).misses==2 && sum(d).hits==0 && sum(d).peak_entries==0,"zero metadata cache retains entries");
    require(DirectoryModel::line_key(4096)!=DirectoryModel::region_key(4096,4096),"line and page tag namespaces collide");
}
void pipeline_dependency_and_link() {
    auto c=config(); c.workload=WorkloadMode::DLRM; c.directory_ns=20; c.directory_bytes=0;
    c.snoop_bytes=c.invalidate_bytes=0; c.memory_ns=100; c.bandwidth_gbps=64;
    c.hit_ns=2; c.recovery_rate=0; c.directory_mshrs=8;
    Workload w{{"dependency",{{1,{{0,{{AccessKind::ColdEmbeddings,0,false,{}},
        {AccessKind::ColdEmbeddings,0,false,{}}}}}}},false}};
    const auto r=simulate(w,c,{"base",1,1,false});
    near(r.execution_ns,174); require(r.directory[index(AccessKind::ColdEmbeddings)].misses==1,"one fill should cause one metadata miss");
    c.region_barrier_ns=40;
    w={{"link",{{1,{{0,{{AccessKind::ColdEmbeddings,0,false,{}},{AccessKind::ColdEmbeddings,64,false,{}}}}}},
        {2,{{50,{{AccessKind::ColdEmbeddings,4096,false,{}}}}}}},false}};
    const auto b=simulate(w,c,{"bypass",0,0,true,Strategy::SelectiveBypass});
    near(b.queue_wait_ns,0); near(b.execution_ns,313);
}
void warmup_preserves_cache_and_excludes_time() {
    auto c=config(); c.workload=WorkloadMode::DLRM; c.directory_entries=4096;
    c.rounds=32; c.warmup_rounds=8; c.table_lines=128; c.scratch_lines=8;
    const auto w=make_workload(c);
    require(w.size()==2 && !w[0].measured && w[1].measured,"warm-up phase flags");
    const auto r=simulate(w,c,{"base",1,1,false});
    near(r.measured_execution_ns,r.phases[1].end_ns-r.phases[1].start_ns);
    require(r.measured_execution_ns<r.execution_ns,"warm-up time retained in measurement");
    const auto& scratch=r.measured_kinds[index(AccessKind::PrivateScratchpad)];
    require(scratch.hits==scratch.accesses,"warm private data was reset at measurement boundary");
    std::uint64_t all=0,measured=0; for (const auto& k:r.kinds) all+=k.accesses;
    for (const auto& k:r.measured_kinds) measured+=k.accesses;
    require(measured<all,"warm-up counters retained");
    c.workload=WorkloadMode::RadixTree;
    const auto radix=simulate(make_workload(c),c,{"base",1,1,false});
    require(std::count_if(radix.phases.begin(),radix.phases.end(),[](const PhaseTiming& phase){return phase.measured;})==1 && !radix.phases[0].measured && !radix.phases.back().measured,"radix measurement includes setup/completion");
}
void constraints_and_legacy() {
    auto c=config(); c.directory_banks=0;
    bool threw=false;try{c.validate();}catch(const std::invalid_argument&){threw=true;}require(threw,"zero banks accepted");
    c=config(); c.warmup_rounds=c.rounds; threw=false;
    try{c.validate();}catch(const std::invalid_argument&){threw=true;}require(threw,"empty measurement accepted");
    c=Config{}; c.directory_model=false;
    const auto w=make_workload(c); near(simulate(w,c,default_scenarios()[0]).execution_ns,38447);
}
}
int main() {
    const std::vector<std::pair<const char*,std::function<void()>>> tests{
        {"metadata LRU hit miss dirty eviction",miss_hit_and_eviction},
        {"bounded miss buffers and pending entries",bounded_buffers_and_pending_tags},
        {"pending fill merge and retained service budget",pending_merge_and_budget},
        {"disabled metadata cache and tag namespaces",cache_disable_and_namespaces},
        {"directory pipeline dependencies and eligible link use",pipeline_dependency_and_link},
        {"warm-up preserves caches and excludes time/counters",warmup_preserves_cache_and_excludes_time},
        {"constraints and legacy timing compatibility",constraints_and_legacy}};
    std::size_t failures=0;
    for (const auto& t:tests) try { t.second(); std::cout<<"PASS "<<t.first<<'\n'; }
        catch(const std::exception& e) {++failures;std::cerr<<"FAIL "<<t.first<<": "<<e.what()<<'\n';}
    std::cout<<tests.size()-failures<<'/'<<tests.size()<<" directory tests passed\n";
    return failures==0 ? 0 : 1;
}
