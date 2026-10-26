#include "model.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
using namespace perf;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void near(double actual, double expected) {
    if (std::abs(actual - expected) > 1e-7) throw std::runtime_error("expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}
Config dlrm() { Config c; c.workload=WorkloadMode::DLRM; return c; }
Config zero_cost() {
    auto c=dlrm(); c.directory_model=false; c.directory_ns=c.snoop_ns=c.invalidate_ns=c.writeback_ns=0;
    c.directory_bytes=c.snoop_bytes=c.invalidate_bytes=0; c.recovery_rate=0;
    c.region_barrier_ns=c.recovery_ns=c.recovery_bytes=0; c.bandwidth_gbps=64;
    c.memory_ns=c.peer_ns=100; return c;
}
const Scenario base{"base",1,1,false};
const Scenario selective{"selective",0,0,true,Strategy::SelectiveBypass};
const Scenario coarse{"coarse",0,0,true,Strategy::CoarseAll};
Traffic sum(const Result& r) { Traffic t; for(const auto& k:r.kinds)t+=k; return t; }
void trace_structure() {
    const auto c=dlrm(); const auto w=make_workload(c);
    std::map<std::uint64_t,AccessKind> pages;
    std::map<std::uint64_t,std::size_t> owners;
    std::uint64_t hot=0,cold=0,hot_w=0,cold_w=0;
    std::map<std::uint64_t,std::size_t> hot_popularity;
    for(const auto& stream:w[0].streams) {
        check(stream.steps.size()==2*c.rounds,"two stages per batch");
        for(std::size_t s=0;s<stream.steps.size();++s) {
            const auto& step=stream.steps[s];
            if(stream.node!=0 && s%2==0) check(!step.allow_lookahead,"top cannot bypass gather dependency");
            for(const auto& a:step.accesses) {
                const auto page=a.address/c.region_bytes;
                auto p=pages.emplace(page,a.kind); check(p.first->second==a.kind,"classes share page");
                if(a.kind==AccessKind::PrivateScratchpad) {
                    auto o=owners.emplace(page,stream.node); check(o.first->second==stream.node && stream.node!=0,"scratch shared");
                } else if(a.write) {
                    check(stream.node==0,"accelerator wrote embedding");
                    if(a.kind==AccessKind::HotEmbeddings)++hot_w;else ++cold_w;
                } else if(a.kind==AccessKind::HotEmbeddings) {++hot;++hot_popularity[a.address];} else ++cold;
            }
        }
    }
    check(hot_lines(c)==205,"hot footprint rounding");
    check(std::abs(static_cast<double>(hot)/static_cast<double>(hot+cold)-.8)<.025,"hot read mix");
    check(hot_w==32 && cold_w==2,"host update cadence");
    check(hot_popularity.begin()->second>hot_popularity.rbegin()->second*10,"Zipf skew missing");
}
void reproducibility_and_host_independence() {
    auto c=dlrm(); const auto w=make_workload(c); const auto hash=workload_fingerprint(w);
    check(hash==workload_fingerprint(make_workload(c)),"seed not repeatable");
    c.hot_update_every=0; c.cold_update_every=0; const auto nohost=make_workload(c);
    for(std::size_t n=1;n<w[0].streams.size();++n) {
        Workload a{{"x",{w[0].streams[n]},false}},b{{"x",{nohost[0].streams[n]},false}};
        check(workload_fingerprint(a)==workload_fingerprint(b),"host cadence perturbed accelerator reads");
    }
    ++c.seed; check(hash!=workload_fingerprint(make_workload(c)),"different seed same trace");
}
void bottom_top_dependency() {
    auto c=zero_cost();
    Step gather; gather.concurrent_compute_ns=300; gather.allow_lookahead=false;
    gather.accesses={{AccessKind::ColdEmbeddings,0,false,{}}};
    Workload w{{"stages",{{1,{gather,{20,{}}}}},false}};
    auto r=simulate(w,c,base); near(r.execution_ns,320); near(r.worker_compute_ns,320); near(r.overlap_ns,101);
    w[0].streams[0].steps[0].concurrent_compute_ns=50;
    r=simulate(w,c,base); near(r.execution_ns,121); near(r.overlap_ns,50);
}
void barrier_wait_does_not_reserve_link() {
    auto c=zero_cost(); c.region_barrier_ns=40; c.outstanding=2;
    Workload w{{"ready",{{1,{{0,{{AccessKind::ColdEmbeddings,0,false,{}},
        {AccessKind::ColdEmbeddings,64,false,{}}}}}},
        {2,{{50,{{AccessKind::ColdEmbeddings,4096,false,{}}}}}}},false}};
    const auto r=simulate(w,c,selective);
    near(r.queue_wait_ns,0); // node 2 uses the link while node 1 waits for its grant
    near(r.execution_ns,242);
}
void host_pacing() {
    auto c=zero_cost();
    Step host; host.sync_node=1; host.sync_step=0;
    host.accesses={{AccessKind::HotEmbeddings,0,true,{}}};
    Workload w{{"paced",{{0,{host}},{1,{{100,{{AccessKind::ColdEmbeddings,4096,false,{}}}}}}},false}};
    const auto r=simulate(w,c,base); near(r.host_sync_wait_ns,100);
    check(r.execution_ns>=201,"host was not paced");
    auto real=dlrm(); const auto output=simulate(make_workload(real),real,base);
    check(output.host_sync_wait_ns>0,"real host stream ran entirely at start");
}
Workload page_sharing(AccessKind kind) {
    return {{"read",{{1,{{0,{{kind,0,false,{}}}}}},{2,{{0,{{kind,64,false,{}}}}}}},false},
            {"update",{{0,{{0,{{kind,128,true,{}}}}}}},false},
            {"reread",{{1,{{0,{{kind,0,false,{}}}}}}},false}};
}
void cold_recovery() {
    auto c=dlrm(); const auto w=page_sharing(AccessKind::ColdEmbeddings);
    const auto b=simulate(w,c,base),s=simulate(w,c,selective);
    const auto t=sum(s);
    check(t.region_recoveries==1 && t.collateral_copies==2 && t.revoked_copies==2,"cold revoke misses page copies");
    check(t.region_flushes==1 && t.writebacks==1,"cold publication did not flush exactly once");
    check(t.fills==4 && sum(b).fills==3,"cold reread was not invalidated");
    near(s.writeback_bytes,64); near(s.expected_recoveries,0);
    check(s.region_penalty_ns>=2*c.region_barrier_ns+c.recovery_ns,"cold recovery missing costs");
}
void false_sharing_vs_line_size() {
    auto c=dlrm(); const auto w=page_sharing(AccessKind::HotEmbeddings);
    const auto b=simulate(w,c,base),s=simulate(w,c,selective),r=simulate(w,c,coarse);
    near(b.execution_ns,s.execution_ns); near(s.charged_directory,b.charged_directory);
    check(sum(r).collateral_copies==2 && sum(r).region_recoveries==1,"coarse missed hot false sharing");
    c.region_bytes=64; const auto line=simulate(w,c,coarse);
    check(sum(line).collateral_copies==0 && sum(line).region_recoveries==0,"line granularity has collateral copies");
}
void private_and_hot_only() {
    auto c=dlrm(); Workload w{{"scratch",{{1,{{0,{{AccessKind::PrivateScratchpad,0,true,{}},
        {AccessKind::PrivateScratchpad,64,true,{}},{AccessKind::PrivateScratchpad,0,false,{}}}}}}},false}};
    auto r=simulate(w,c,coarse); check(sum(r).region_recoveries==0 && sum(r).hits==1,"private scratch revoked");
    near(r.charged_directory,0);
    w[0].streams.push_back({2,{{0,{{AccessKind::PrivateScratchpad,0,false,{}}}}}});
    bool threw=false;try{simulate(w,c,base);}catch(const std::invalid_argument&){threw=true;}check(threw,"shared scratch accepted");
}
void dirty_victim_classification() {
    auto c=dlrm(); c.cache_bytes=64;
    Workload w{{"victim",{{1,{{0,{{AccessKind::PrivateScratchpad,0,true,{}},
        {AccessKind::ColdEmbeddings,4096,false,{}}}}}}},false}};
    const auto p=profile_workload(w,c,base);
    const auto& scratch=p.kinds[index(AccessKind::PrivateScratchpad)];
    const auto& cold=p.kinds[index(AccessKind::ColdEmbeddings)];
    check(scratch.writebacks==1 && scratch.evictions==1 && scratch.dirty_evictions==1,
          "dirty victim attributed to triggering cold read");
    check(cold.writebacks==0 && cold.evictions==0,"cold class incorrectly owns scratch writeback");
}
void dlrm_accounting_and_fitting() {
    auto c=dlrm(); const auto w=make_workload(c);
    for(const auto& scenario:default_scenarios(WorkloadMode::DLRM)) {
        const auto r=simulate(w,c,scenario); const auto t=sum(r);
        check(r.trace_fingerprint==workload_fingerprint(w),"scenarios changed logical trace");
        check(t.hits+t.fills==r.requests && t.accesses==r.requests,"access accounting");
        near(r.writeback_bytes,static_cast<double>(t.writebacks)*64);
        near(r.data_bytes,static_cast<double>(t.fills)*64);
        check(r.peak_inflight<=(c.agents+1)*c.outstanding,"MLP window exceeded");
        check(r.overlap_ns<=r.worker_compute_ns && r.link_busy_ns<=r.execution_ns,"overlap/link accounting");
    }
    c.hot_update_every=c.cold_update_every=0; c.table_lines=128; c.scratch_lines=8;
    const auto fit=profile_workload(make_workload(c),c,base);
    c.cache_bytes=512; const auto small=profile_workload(make_workload(c),c,base);
    check(small.kinds[index(AccessKind::ColdEmbeddings)].fills>fit.kinds[index(AccessKind::ColdEmbeddings)].fills,"capacity not affecting DLRM");
}
void validation() {
    auto c=dlrm(); c.table_lines=1; bool threw=false;
    try{make_workload(c);}catch(const std::invalid_argument&){threw=true;}check(threw,"empty cold/hot accepted");
    c=dlrm();c.region_bytes=192;threw=false;
    try{c.validate();}catch(const std::invalid_argument&){threw=true;}check(threw,"non power-of-two pages accepted");
}
}
int main() {
    const std::vector<std::pair<const char*,std::function<void()>>> tests{
        {"DLRM footprint skew cadence and private ownership",trace_structure},
        {"deterministic trace and independent host seed",reproducibility_and_host_independence},
        {"bottom/gather join and top dependency",bottom_top_dependency}, {"host paced alongside inference",host_pacing},
        {"barrier wait reserves no premature link slot",barrier_wait_does_not_reserve_link},
        {"rare cold page revoke and clean publication",cold_recovery}, {"hot false sharing versus line granularity",false_sharing_vs_line_size},
        {"private scratch has no remote revocations",private_and_hot_only}, {"DLRM accounting cache capacity and shared trace",dlrm_accounting_and_fitting},
        {"dirty victim counters use actual memory class",dirty_victim_classification},
        {"DLRM validation",validation}};
    std::size_t failed=0;
    for(const auto& t:tests)try{t.second();std::cout<<"PASS "<<t.first<<'\n';}
        catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<t.first<<": "<<e.what()<<'\n';}
    std::cout<<tests.size()-failed<<'/'<<tests.size()<<" DLRM tests passed\n";
    return failed==0?0:1;
}
