#include "simulator.hpp"
#include "profile.hpp"
#include "directory.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace perf;
namespace {
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void near(double a,double b){if(std::abs(a-b)>1e-7)throw std::runtime_error("expected "+std::to_string(b)+", got "+std::to_string(a));}
Config quiet() {
    Config c;c.capture_events=true;c.directory_ns=c.directory_miss_ns=c.directory_issue_ns=0;
    c.directory_bytes=c.snoop_bytes=c.invalidate_bytes=0;c.snoop_ns=c.invalidate_ns=c.writeback_ns=0;
    c.directory_backing_gbps=c.bandwidth_gbps=1e12;c.memory_ns=c.peer_ns=100;c.compute_ns=0;c.host_compute_ns=0;
    return c;
}
void local_permissions_and_dirty_eviction() {
    auto c=quiet();c.cache_bytes=64;BaselineMemory memory(c);Transaction tx;
    for(std::uint64_t w=1;w<=4;++w) {
        require(memory.admit(1,{AccessKind::PrivateScratchpad,0,true},tx,w),"private store not admitted");
        require(tx.traffic.directory==(w==1 ? 1U : 0U),"repeated exclusive store took directory permission");
        require(memory.complete(tx)==w,"write version did not advance");
    }
    require(memory.admit(1,{AccessKind::ColdEmbeddings,4096,false},tx,5),"evicting read failed");
    require(tx.has_victim && tx.victim_dirty && tx.operations.size()==2,"dirty eviction notice missing");
    Transaction reader;require(!memory.admit(2,{AccessKind::PrivateScratchpad,0,false},reader,6),"dirty victim visible before writeback");
    memory.complete(tx);require(memory.admit(2,{AccessKind::PrivateScratchpad,0,false},reader,6),"evicted line did not become readable");
    require(memory.complete(reader)==4,"dirty eviction lost data");
}
void dirty_owner_and_invalidation() {
    auto c=quiet();BaselineMemory memory(c);Transaction tx;
    memory.admit(1,{AccessKind::HotEmbeddings,0,true},tx,1);memory.complete(tx);
    require(memory.admit(2,{AccessKind::ColdEmbeddings,0,false},tx,2),"labels affected physical sharing");
    require(tx.traffic.snoops==1 && tx.traffic.writebacks==1,"dirty-owner intervention lost");require(memory.complete(tx)==1,"reader saw stale data");
    memory.admit(3,{AccessKind::HotEmbeddings,0,false},tx,3);require(tx.traffic.snoops==0,"shared clean read snooped owner");memory.complete(tx);
    memory.admit(2,{AccessKind::ColdEmbeddings,0,true},tx,4);require(tx.traffic.hits==1 && tx.traffic.invalidations==2,"shared-write upgrade did not invalidate");memory.complete(tx);
    memory.admit(1,{AccessKind::HotEmbeddings,0,false},tx,5);require(tx.traffic.fills==1,"invalidated read hit stale data");require(memory.complete(tx)==2,"write not observed");
}
void fills_and_cache_slots_wait_for_completion() {
    auto c=quiet();c.cache_bytes=64;BaselineMemory memory(c);Transaction first,second;
    require(memory.admit(1,{AccessKind::ColdEmbeddings,0,false},first,1),"first fill failed");
    require(!memory.admit(1,{AccessKind::ColdEmbeddings,64,false},second,2),"pending fill did not consume capacity");
    require(!memory.admit(2,{AccessKind::ColdEmbeddings,0,false},second,3),"remote request passed uncompleted fill");
    memory.complete(first);require(memory.admit(1,{AccessKind::ColdEmbeddings,64,false},second,2),"completion did not free slot for eviction");memory.complete(second);
    require(memory.peak_cache_lines()==1,"cache capacity exceeded");
}
void timing_changes_interleaving() {
    auto c=quiet();Workload w{{"race",{{1,{{0,{{AccessKind::PrivateScratchpad,4096,false}}},{0,{{AccessKind::HotEmbeddings,0,true}}}}},
        {0,{{50,{{AccessKind::ColdEmbeddings,0,false}}}}}},true}};
    c.memory_ns=c.peer_ns=10;BlindProfiler fast_profile(c);const auto fast=simulate(w,c,fast_profile);
    c.memory_ns=c.peer_ns=100;BlindProfiler slow_profile(c);const auto slow=simulate(w,c,slow_profile);
    auto read=[](const Result& r)->const EventRecord& {return *std::find_if(r.events.begin(),r.events.end(),[](const EventRecord& e){return e.node==0;});};
    require(read(fast).version==1 && read(slow).version==0,"latency did not change read/write order");
    require(read(fast).traffic.writebacks==1 && read(slow).traffic.writebacks==0,"coherence events were precomputed");
}
void mixed_contiguous_layout_and_blind_labels() {
    Config c;const auto rows=embedding_rows(c);std::set<std::uint64_t> unique(rows.begin(),rows.end());
    require(unique.size()==c.table_lines && *unique.begin()==0 && *unique.rbegin()==c.table_lines-1,"layout is not a contiguous permutation");
    std::set<std::uint64_t> hotpages,coldpages;
    for(std::size_t i=0;i<rows.size();++i)(i<hot_lines(c) ? hotpages : coldpages).insert(rows[i]/64);
    std::size_t mixed=0;for(auto p:hotpages)if(coldpages.count(p))++mixed;require(mixed>40,"hot rows still segregated by page");
    c.rounds=4;c.capture_events=true;auto w=make_workload(c);BlindProfiler a(c);const auto first=simulate(w,c,a);
    for(auto& p:w)for(auto& s:p.streams)for(auto& step:s.steps)for(auto& access:step.accesses)
        access.kind=access.kind==AccessKind::HotEmbeddings ? AccessKind::ColdEmbeddings : AccessKind::HotEmbeddings;
    BlindProfiler b(c);const auto second=simulate(w,c,b);near(first.execution_ns,second.execution_ns);
    require(first.trace_fingerprint==second.trace_fingerprint,"trace identity depends on labels");
    for(std::size_t scope=0;scope<2;++scope)for(std::size_t space=0;space<2;++space)for(std::size_t scale=0;scale<4;++scale) {
        const auto& left=a.window(scope).views[space][scale];const auto& right=b.window(scope).views[space][scale];
        require(left.blocks.size()==right.blocks.size(),"histogram block set depends on labels");
        for(const auto& entry:left.blocks) {
            const auto& x=entry.second;const auto& y=right.blocks.at(entry.first);
            require(x.reads==y.reads && x.writes==y.writes && x.nodes==y.nodes && x.read_lines==y.read_lines &&
                x.written_lines==y.written_lines && x.demand_misses==y.demand_misses && x.eviction_misses==y.eviction_misses,
                "blind address distributions depend on diagnostic labels");
        }
        require(left.reads==right.reads && left.intervals==right.intervals,"phase distributions depend on labels");
    }
}
void histograms_and_censoring() {
    Config c;BlindProfiler p(c);p.begin_phase(0,true);
    const auto a=embedding_base,b=a+64,d=a+4096;
    p.access(0,1,a,false,true);p.access(1,2,b,false,true);p.directory_miss(a,false,true);
    p.access(2,0,a,true,true);p.access(3,1,b,false,true);p.access(4,2,b,false,true);p.access(5,0,a,true,true);
    p.access(6,1,a,false,true);p.access(7,1,d,false,true);p.directory_miss(d,true,true);p.end_phase(10,true);
    const auto& v=p.window(1).views[1][3];const auto& first=v.blocks.at(a/4096);
    require(bit_count(first.nodes)==3 && first.writes==2,"sharer count / writes incorrect");
    require(bit_count(first.written_lines)==1 && bit_count(first.read_lines&~first.written_lines)==1,"mixed-page purity incorrect");
    const auto runs=BlindProfiler::runs(v,0,10);
    require(runs.first.at(RunType::LeftCensored).at(2)==1,"leading read phase lost");
    require(runs.first.at(RunType::Closed).at(2)==1,"closed read-run length incorrect");
    require(runs.first.at(RunType::RightCensored).at(1)==1 && runs.first.at(RunType::BothCensored).at(1)==1,"censored tails lost");
    require(first.demand_misses==1 && first.writes>0,"future write did not change final attribution");
    const auto& lines=p.window(1).views[1][0];for(const auto& e:lines.blocks)
        require((e.second.read_lines&~e.second.written_lines)==0 || e.second.written_lines==0,"64B block spatially mixed");
}
void warmup_and_accounting() {
    Config c;c.rounds=24;c.warmup_rounds=8;c.table_lines=128;c.scratch_lines=8;c.capture_events=true;
    BlindProfiler p(c);const auto r=simulate(make_workload(c),c,p);
    near(r.measured_execution_ns,r.phases[1].end_ns-r.phases[1].start_ns);
    Traffic t,m;DirectoryStats d;for(const auto& k:r.kinds)t+=k;for(const auto& k:r.measured_kinds)m+=k;for(const auto& k:r.directory)d+=k;
    require(t.accesses==r.events.size() && t.hits+t.fills==t.accesses && m.accesses<t.accesses,"request partition/warmup");
    near(r.data_bytes,64*static_cast<double>(t.fills));near(r.writeback_bytes,64*static_cast<double>(t.writebacks));
    require(d.requests==t.directory+t.evictions && d.requests==d.hits+d.misses+d.merged,"home probes omit eviction notices");
    require(r.peak_cache_lines<=c.cache_bytes/64 && r.peak_inflight<=(c.agents+1)*c.outstanding,"bounded resources exceeded");
    require(r.link_busy_ns<=r.execution_ns && r.overlap_ns<=r.worker_compute_ns,"invalid time accounting");
}
void bounded_mlp_parallel_hits_and_compute_join() {
    auto c=quiet();c.outstanding=1;
    Workload w{{"batch",{{1,{{0,{{AccessKind::ColdEmbeddings,0,false},{AccessKind::ColdEmbeddings,64,false}}}}}},true}};
    BlindProfiler serial(c);near(simulate(w,c,serial).execution_ns,200);
    c.outstanding=2;BlindProfiler parallel(c);near(simulate(w,c,parallel).execution_ns,100);
    Step gather;gather.concurrent_compute_ns=300;gather.allow_lookahead=false;
    gather.accesses={{AccessKind::ColdEmbeddings,0,false}};
    w={{"join",{{1,{gather,{20,{}}}}},true}};BlindProfiler join(c);near(simulate(w,c,join).execution_ns,320);
    w={{"first",{{1,{{0,{{AccessKind::ColdEmbeddings,0,false}}}}}},true},
       {"second",{{2,{{0,{{AccessKind::ColdEmbeddings,0,false}}}}}},true},
       {"shared_hits",{{1,{{0,{{AccessKind::ColdEmbeddings,0,false}}}}},{2,{{0,{{AccessKind::ColdEmbeddings,0,false}}}}}},true}};
    BlindProfiler hits(c);const auto result=simulate(w,c,hits);near(result.phases.back().end_ns-result.phases.back().start_ns,2);
}
void disabled_cache_preserves_memory() {
    auto c=quiet();c.cache_bytes=0;BaselineMemory memory(c);Transaction tx;
    memory.admit(1,{AccessKind::HotEmbeddings,0,true},tx,1);require(tx.traffic.writebacks==1,"uncached write not written through");memory.complete(tx);
    memory.admit(2,{AccessKind::ColdEmbeddings,0,false},tx,2);require(tx.traffic.hits==0,"cache-disabled access hit");require(memory.complete(tx)==1,"uncached store lost");
    require(memory.peak_cache_lines()==0,"disabled cache retained lines");
}
void directory_pending_metadata() {
    Config c;c.directory_entries=1;c.directory_banks=1;c.directory_mshrs=1;c.directory_issue_ns=2;
    c.directory_ns=10;c.directory_miss_ns=50;c.directory_backing_gbps=64;DirectoryModel d(c);const auto label=AccessKind::HotEmbeddings;
    near(d.reserve(0,0,label),0);auto first=d.lookup(0,0,label);near(first.time,61);require(first.miss,"metadata miss not reported");
    d.reserve(0,2,label);auto blocked=d.lookup(2,2,label);require(blocked.retry && !blocked.miss,"retry became duplicate miss");near(blocked.time,61);
    require(d.lookup(61,2,label).miss,"replacement did not miss");
}
}
int main() {
    const std::vector<std::pair<const char*,std::function<void()>>> tests{
        {"private permission reuse and dirty eviction",local_permissions_and_dirty_eviction},
        {"dirty-owner handoff and write invalidation",dirty_owner_and_invalidation},
        {"pending fills reserve slots and hide data",fills_and_cache_slots_wait_for_completion},
        {"memory latency changes collisions and versions",timing_changes_interleaving},
        {"mixed contiguous layout and label independence",mixed_contiguous_layout_and_blind_labels},
        {"blind distributions and censored runs",histograms_and_censoring},
        {"warm-up persists and accounts resources",warmup_and_accounting},
        {"MLP parallel reads and compute join",bounded_mlp_parallel_hits_and_compute_join},
        {"cache-disabled stores preserve memory",disabled_cache_preserves_memory},
        {"directory cache miss backpressure",directory_pending_metadata}};
    std::size_t failures=0;for(const auto& t:tests)try{t.second();std::cout<<"PASS "<<t.first<<'\n';}
        catch(const std::exception& e){++failures;std::cerr<<"FAIL "<<t.first<<": "<<e.what()<<'\n';}
    return failures==0 ? 0 : 1;
}
