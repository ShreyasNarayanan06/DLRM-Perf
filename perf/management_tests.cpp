#include "simulator.hpp"
#include "management.hpp"
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace perf;
namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void near(double a,double b) {if(std::abs(a-b)>1e-6)throw std::runtime_error("timing mismatch");}
struct Run {Result result;ManagementStats all,measured;std::vector<RegionChange> changes;};
Run run(const Workload& w,const Config& c,const ManagementConfig& config) {
    ManagementModel manager(c,config);BlindProfiler profile(c);auto result=simulate(w,c,profile,&manager);
    return {std::move(result),manager.stats(),manager.stats(true),manager.changes()};
}
void versions(const Result& r) {
    std::unordered_map<std::uint64_t,std::uint64_t> latest;
    for(const auto& event:r.events) {
        if(event.write)++latest[event.address];
        require(event.version==latest[event.address],"software/hardware path returned an outdated copy");
    }
}
void zero_software_cost_is_same_reference() {
    Config c;c.rounds=12;c.warmup_rounds=4;c.capture_events=true;
    const auto w=make_workload(c);ManagementConfig config;config.software_ns=config.software_message_bytes=0;
    config.mode=ManagementMode::Software;const auto software=run(w,c,config);
    config.mode=ManagementMode::Hardware;const auto hardware=run(w,c,config);
    BlindProfiler original(c);const auto legacy=simulate(w,c,original);
    near(software.result.execution_ns,hardware.result.execution_ns);near(hardware.result.execution_ns,legacy.execution_ns);
    near(software.result.control_bytes,hardware.result.control_bytes);
    require(software.all.software_requests>0 && software.all.hardware_requests==0,"software baseline silently used hardware management");
    require(hardware.all.hardware_requests==software.all.software_requests && hardware.all.software_requests==0,"manager routing mismatch");
    versions(software.result);versions(hardware.result);
}
void software_worker_queue_and_private_hits() {
    Config c;ManagementConfig config;config.mode=ManagementMode::Software;config.software_workers=1;config.software_ns=50;
    ManagementModel queue(c,config);near(queue.operation_ready(0,0,true),50);near(queue.operation_ready(0,64,true),100);
    near(queue.operation_ready(75,128,true),150);near(queue.stats().software_queue_ns,75);
    c.capture_events=true;Step step;for(int n=0;n<8;++n)step.accesses.push_back({AccessKind::PrivateScratchpad,0,true});
    const auto result=run({{"private",{{1,{step}}},true}},c,config);
    require(result.all.software_requests==1,"cached private stores unnecessarily contacted software home");versions(result.result);
}
void handoff_drains_and_flushes_dirty_data() {
    Config c;c.capture_events=true;ManagementConfig config;config.mode=ManagementMode::Dynamic;
    config.review_accesses=2;config.promote_operations=1;
    const auto base=embedding_base;
    Workload w;
    const auto append=[&](std::size_t node,std::uint64_t address,bool write) {
        w.push_back({"ordered",{{node,{{0,{{AccessKind::ColdEmbeddings,address,write}}}}}},true});
    };
    append(1,base+64,true);append(1,base,false);append(2,base,false);append(0,base+128,true);
    append(2,base+64,false);append(0,base+64,true);append(1,base+64,false);
    const auto r=run(w,c,config);versions(r.result);
    require(r.all.to_hardware>0 && r.all.flushed_lines>=2,"dirty region handoff not modeled");
    require(r.all.transition_data_bytes==64*static_cast<double>(r.all.flushed_lines),"handoff flush bytes missing");
    require(r.result.events.back().version==2,"handoff lost a write");
    for(const auto& change:r.changes)require(change.end_ns>=change.start_ns+config.transition_ns,"transition cost skipped");
}
void demotion_and_bounded_table() {
    Config c;c.capture_events=true;ManagementConfig config;config.mode=ManagementMode::Dynamic;
    config.review_accesses=2;config.promote_operations=1;config.region_entries=1;
    const auto a=embedding_base,b=a+4096;
    Workload w{{"a",{{1,{{0,{{AccessKind::HotEmbeddings,a,false}}}}}},true},
               {"b",{{2,{{0,{{AccessKind::HotEmbeddings,a,false}}}}}},true},
               {"promote",{{1,{{0,{{AccessKind::HotEmbeddings,a,false}}}}}},true},
               {"overflow",{{3,{{0,{{AccessKind::ColdEmbeddings,b,false}}}}}},true},
               {"idle",{{2,{{0,{{AccessKind::HotEmbeddings,a,false}}}}}},true},
               {"demote",{{1,{{0,{{AccessKind::HotEmbeddings,a,false}}}}}},true}};
    const auto r=run(w,c,config);versions(r.result);
    require(r.all.peak_regions==1 && r.all.table_pressure>0,"finite region table was bypassed");
    require(r.all.to_hardware>0 && r.all.to_software>0,"reference never returned to software after a quiet window");
}
void victim_cannot_cross_pending_handoff() {
    Config c;c.cache_bytes=64;BaselineMemory memory(c);Transaction tx;
    require(memory.admit(1,{AccessKind::PrivateScratchpad,0,true},tx,1),"first write failed");memory.complete(tx);
    require(!memory.admit(1,{AccessKind::PrivateScratchpad,64,false},tx,2,[](std::uint64_t){return false;}),"blocked victim was evicted");
    require(memory.admit(1,{AccessKind::PrivateScratchpad,64,false},tx,2),"failed admission damaged cache state");
    require(tx.victim_dirty && tx.victim==0,"dirty victim lost after handoff wait");memory.complete(tx);
}
void dormant_hint_does_not_pin_cache_victims() {
    Config c;c.cache_bytes=64;c.capture_events=true;
    ManagementConfig config;config.mode=ManagementMode::Dynamic;config.review_accesses=2;config.promote_operations=1;
    Workload w{{"first",{{1,{{0,{{AccessKind::ColdEmbeddings,0,false}}}}}},true},
               {"hint",{{2,{{0,{{AccessKind::ColdEmbeddings,0,false}}}}}},true},
               {"different_region",{{1,{{0,{{AccessKind::ColdEmbeddings,4096,false}}}}}},true}};
    const auto r=run(w,c,config);versions(r.result);
    require(r.result.events.size()==3,"unused promotion hint prevented eviction and stalled the workload");
}
void pressure_warmup_and_label_independence() {
    Config c;c.rounds=20;c.warmup_rounds=4;c.cache_bytes=256;c.table_lines=64;c.scratch_lines=4;c.capture_events=true;
    c.hot_update_every=1;c.hot_updates=4;
    ManagementConfig config;config.mode=ManagementMode::Dynamic;config.region_entries=4;config.review_accesses=4;config.promote_operations=2;
    auto w=make_workload(c);const auto first=run(w,c,config);versions(first.result);
    for(auto& phase:w)for(auto& stream:phase.streams)for(auto& step:stream.steps)for(auto& access:step.accesses)access.kind=AccessKind::Metadata;
    const auto second=run(w,c,config);near(first.result.execution_ns,second.result.execution_ns);
    require(first.all.transitions==second.all.transitions && first.all.software_requests==second.all.software_requests,"manager depends on diagnostic heat labels");
    require(first.all.peak_regions<=config.region_entries && first.measured.software_requests<=first.all.software_requests,"warmup/resource stats invalid");
    require(first.result.peak_cache_lines<=c.cache_bytes/64,"data cache capacity exceeded");
    require(first.all.transitions>0,"pressure test did not exercise handoffs");
}
}
int main() {
    const std::vector<std::pair<const char*,std::function<void()>>> tests{
        {"zero software overhead matches hardware and original baseline",zero_software_cost_is_same_reference},
        {"software queues and cached private fast path",software_worker_queue_and_private_hits},
        {"dirty data survives drained region handoff",handoff_drains_and_flushes_dirty_data},
        {"finite region records and demotion",demotion_and_bounded_table},
        {"victim reservation respects pending region handoff",victim_cannot_cross_pending_handoff},
        {"dormant region hints cannot pin victims",dormant_hint_does_not_pin_cache_victims},
        {"concurrent pressure, warmup and label independence",pressure_warmup_and_label_independence}};
    std::size_t failures=0;
    for(const auto& test:tests)try {test.second();std::cout<<"PASS "<<test.first<<'\n';}
        catch(const std::exception& error){++failures;std::cerr<<"FAIL "<<test.first<<": "<<error.what()<<'\n';}
    return failures==0 ? 0 : 1;
}
