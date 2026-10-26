#include "model.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
using namespace perf;
namespace {
void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
void close(double a, double b) {
    if (std::abs(a-b) > 1e-7 * std::max(1.0, std::abs(b)))
        throw std::runtime_error("expected " + std::to_string(b) + ", got " + std::to_string(a));
}
template<class F> void must_throw(F fn) { bool caught=false; try { fn(); } catch(const std::exception&) { caught=true; } require(caught,"expected error"); }
const Scenario reference{"reference",1,1,false}, half{"half",.5,.5,true};
Config quiet() {
    Config c; c.directory_model=false;
    c.directory_ns=c.snoop_ns=c.invalidate_ns=c.writeback_ns=0;
    c.directory_bytes=c.snoop_bytes=c.invalidate_bytes=0;
    c.fork_barrier_ns=c.region_barrier_ns=c.setup_ns=0;
    c.flush_at_fork=false; c.recovery_rate=0; c.overlap_trigger=1;
    return c;
}
Workload single(std::vector<Access> accesses,double compute=0) { return {{"one",{{1,{{compute,std::move(accesses)}}}},false}}; }
void serial_analytical() {
    auto c=quiet(); c.outstanding=1; c.memory_ns=100; c.directory_ns=20; c.directory_bytes=16; c.bandwidth_gbps=16;
    const auto w=single({{AccessKind::LeafKV,0,false,{}},{AccessKind::PrefixKV,64,false,{}}},10);
    const auto b=simulate(w,c,reference),h=simulate(w,c,half);
    close(b.execution_ns,260); close(h.execution_ns,239); close(b.queue_wait_ns,0);
}
void parallel_analytical() {
    auto c=quiet(); c.memory_ns=c.peer_ns=100; c.bandwidth_gbps=64; c.outstanding=1;
    Workload w{{"parallel",{{1,{{0,{{AccessKind::LeafKV,0,false,{}}}}}},
                                  {2,{{0,{{AccessKind::LeafKV,64,false,{}}}}}}},false}};
    const auto r=simulate(w,c,reference);
    close(r.execution_ns,102); close(r.worker_memory_wait_ns,203); close(r.queue_wait_ns,1);
    auto seq=single({{AccessKind::LeafKV,0,false,{}},{AccessKind::LeafKV,64,false,{}}});
    close(simulate(seq,c,reference).execution_ns,202); c.outstanding=2;
    close(simulate(seq,c,reference).execution_ns,102);
}
void finite_cache_and_eviction() {
    auto c=quiet(); c.cache_bytes=128;
    Workload w{{"one",{{1,{{0,{{AccessKind::LeafKV,0,true,{}}}},
                               {0,{{AccessKind::LeafKV,64,false,{}}}},
                               {0,{{AccessKind::LeafKV,0,false,{}}}}}}},false}};
    auto p=profile_workload(w,c,reference);
    require(p.kinds[2].fills==2 && p.kinds[2].hits==1,"fitting cache must reuse lines");
    c.cache_bytes=64; p=profile_workload(w,c,reference);
    require(p.kinds[2].fills==3 && p.kinds[2].evictions==2,"capacity misses not modeled");
    require(p.kinds[2].writebacks==1 && p.kinds[2].dirty_evictions==1,"dirty eviction lost writeback");
    c.cache_bytes=0; p=profile_workload(w,c,reference);
    require(p.kinds[2].hits==0 && p.kinds[2].evictions==3,"disabled retention");
}
void private_store_reuse() {
    auto c=quiet();
    const auto p=profile_workload(single({{AccessKind::LeafKV,0,true,{}},{AccessKind::LeafKV,0,true,{}},
        {AccessKind::LeafKV,0,true,{}},{AccessKind::LeafKV,0,true,{}}}),c,reference);
    require(p.kinds[2].directory==1 && p.kinds[2].fills==1 && p.kinds[2].hits==3,
            "private repeated stores should retain exclusive ownership");
}
Workload handoff() {
    return {{"lead",{{1,{{0,{{AccessKind::PrefixKV,0,true,{}}}}}}},false},
            {"children",{{2,{{0,{{AccessKind::PrefixKV,0,false,{}}}}}},
                         {3,{{0,{{AccessKind::PrefixKV,0,false,{}}}}}}},true}};
}
void dirty_handoff_and_fork_flush() {
    auto c=quiet(); c.flush_at_fork=true;
    const auto b=profile_workload(handoff(),c,reference);
    const auto& first=b.workload[1].streams[0].steps[0].accesses[0].service.traffic;
    const auto& second=b.workload[1].streams[1].steps[0].accesses[0].service.traffic;
    require(first.directory==1 && first.snoops==1 && first.writebacks==1,"first reader intervention");
    require(second.directory==1 && second.snoops==0 && second.writebacks==0,"later clean sharer");
    const auto h=profile_workload(handoff(),c,half);
    require(h.fork_flushes==1 && h.kinds[1].writebacks==0,"fork must move, not duplicate, dirty writeback");
    require(b.kinds[1].writebacks==h.kinds[1].writebacks+h.fork_flushes,"writeback double counted");
    const auto a=simulate(handoff(),c,half); c.fork_barrier_ns=100;
    const auto z=simulate(handoff(),c,half);
    close(z.execution_ns-a.execution_ns,100); close(z.fork_penalty_ns-a.fork_penalty_ns,100);
    require(z.phases[1].label=="fork_overhead" && z.phases[1].start_ns==z.phases[0].end_ns,
            "fork penalty was not charged at boundary");
}
void bounded_compute_overlap() {
    auto c=quiet(); c.outstanding=1; c.bandwidth_gbps=64; c.memory_ns=150;
    Workload w{{"one",{{1,{{100,{{AccessKind::LeafKV,0,false,{}},{AccessKind::LeafKV,64,false,{}}}},
                                {100,{{AccessKind::LeafKV,128,false,{}},{AccessKind::LeafKV,192,false,{}}}}}}},false}};
    close(simulate(w,c,reference).execution_ns,804);
    c.overlap_trigger=.5; const auto r=simulate(w,c,reference);
    close(r.execution_ns,704); close(r.overlap_ns,100); close(r.worker_compute_ns,200);
    require(r.peak_inflight==1,"lookahead incorrectly increased memory window");
}
void dependent_hits_wait_for_fill() {
    auto c=quiet(); c.outstanding=8; c.bandwidth_gbps=64; c.memory_ns=100; c.hit_ns=2;
    const auto r=simulate(single({{AccessKind::PrefixKV,0,false,{}},{AccessKind::PrefixKV,0,false,{}}}),c,reference);
    close(r.execution_ns,103); // 101 ns fill then 2 ns local dependent access
}
void fitting_prefix_read_hits() {
    Config c;
    const auto p=profile_workload(make_workload(c),c,reference);
    std::uint64_t reads=0,misses=0;
    const auto& children=p.workload[1];
    for(const auto& stream:children.streams) if(stream.node!=0)
        for(const auto& step:stream.steps) for(const auto& a:step.accesses)
            if(a.kind==AccessKind::PrefixKV) { ++reads; misses+=a.service.traffic.fills; }
    require(reads==6144 && misses==128,"lead retains its prefix; only two new child endpoints fill it");
}
void budgets_and_accounting() {
    Config c;
    for(const auto& scenario:default_scenarios()) {
        const auto r=simulate(make_workload(c),c,scenario);
        Traffic t; for(const auto& k:r.kinds) t+=k;
        require(t.accesses==r.requests && t.hits+t.fills==r.requests,"request partition");
        close(r.data_bytes,static_cast<double>(t.fills)*64);
        close(r.writeback_bytes,static_cast<double>(t.writebacks+r.fork_flushes)*64);
        require(r.link_busy_ns<=r.execution_ns,"link utilization exceeds capacity");
        require(r.overlap_ns<=r.worker_compute_ns,"overlap exceeds computation");
        close(r.phases.back().end_ns,r.execution_ns);
    }
    c.recovery_rate=1; c.recovery_ns=1000; const auto r=simulate(make_workload(c),c,half);
    require(r.expected_recoveries>0 && r.added_latency_ns>=r.expected_recoveries*1000,"recovery budget missing");
}
void no_headroom_checks() {
    auto c=quiet(); const auto w=make_workload(c); const auto b=simulate(w,c,reference);
    for(const auto& s:default_scenarios()) if(s.strategy==Strategy::EventBudget)
        close(simulate(w,c,s).execution_ns,b.execution_ns);
    c=Config{}; c.prefix_reads=c.leaf_writes=0; const auto meta=make_workload(c);
    close(simulate(meta,c,reference).execution_ns,simulate(meta,c,half).execution_ns);
    c=quiet(); c.prefix_reads=c.leaf_writes=c.metadata_ops=0; c.compute_ns=10; c.host_compute_ns=0;
    const auto compute=simulate(make_workload(c),c,reference);
    close(compute.execution_ns,static_cast<double>(c.lead_steps+c.rounds)*10);
    require(compute.requests==0,"compute-only workload issued requests");
}
void expensive_candidate_can_lose() {
    Config c; c.added_latency_ns=1000; const auto w=make_workload(c);
    require(simulate(w,c,half).execution_ns>simulate(w,c,reference).execution_ns,"expensive candidate should regress");
}
void validation() {
    Config c; c.bandwidth_gbps=0; must_throw([&]{c.validate();});
    c=Config{}; c.cache_bytes=65; must_throw([&]{c.validate();});
    c=Config{}; c.overlap_trigger=1.1; must_throw([&]{c.validate();});
    c=Config{}; c.directory_ns=std::numeric_limits<double>::quiet_NaN(); must_throw([&]{c.validate();});
    c=Config{}; must_throw([&]{simulate({},c,{"bad",-1,1,true});});
}
} // namespace
int main() {
    const std::vector<std::pair<const char*,std::function<void()>>> tests{
        {"serial analytical timing",serial_analytical},{"parallel and MLP analytical timing",parallel_analytical},
        {"finite cache and dirty eviction",finite_cache_and_eviction},{"private stores retain permission",private_store_reuse},
        {"dirty handoff and fork flush accounting",dirty_handoff_and_fork_flush},
        {"bounded within-agent compute overlap",bounded_compute_overlap},{"hits wait for in-flight fills",dependent_hits_wait_for_fill},
        {"fitting prefix is locally reused",fitting_prefix_read_hits},{"budgets and accounting",budgets_and_accounting},
        {"zero headroom checks",no_headroom_checks},{"candidate costs can regress",expensive_candidate_can_lose},{"validation",validation}};
    std::size_t failed=0;
    for(const auto& test:tests) try {test.second();std::cout<<"PASS "<<test.first<<'\n';}
        catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<test.first<<": "<<e.what()<<'\n';}
    std::cout<<tests.size()-failed<<'/'<<tests.size()<<" tests passed\n";
    return failed==0?0:1;
}
