#include "simulator.hpp"
#include "profile.hpp"
#include "directory.hpp"
#include "management.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace perf {
namespace {
enum class Kind { ComputeDone,StartBatch,TryAccess,DirectoryArrive,DirectoryIssue,DirectoryProbe,DataReady,Complete,Join,RegionChanged };
struct Event {
    double time;std::uint64_t order;std::size_t stream,step;Kind kind;
    std::size_t access=0,operation=0;
    bool operator>(const Event& r) const { return std::tie(time,order)>std::tie(r.time,r.order); }
};
struct RequestState {
    Transaction transaction;
    std::uint64_t id=0,admission_order=0,demand_misses=0,eviction_misses=0;
    double issued=0,admitted=0;
};
struct StepState {
    bool ready=false,started=false,next_compute=false,closed=false,blocked=false,returned=false,join_queued=false;
    std::size_t issued=0,completed=0,inflight=0;
    double wait_start=0,independent_end=0,memory_end=0,sync_wait_start=0,next_start=0,next_end=0;
    std::vector<RequestState> requests;
};
}
Result simulate(const Workload& workload,const Config& c,BlindProfiler& profiler,ManagementModel* management) {
    c.validate();Result result;result.workload=c.workload;result.trace_fingerprint=workload_fingerprint(workload);
    BaselineMemory memory(c);DirectoryModel directory(c);
    double link_available=0;std::uint64_t order=0,request_id=0,admission_order=0;std::size_t outstanding=0;
    const auto network=[&](double now,double bytes) {
        if(bytes==0)return now;
        const auto start=std::max(now,link_available),busy=bytes/c.bandwidth_gbps;
        result.queue_wait_ns+=start-now;result.link_busy_ns+=busy;link_available=start+busy;return link_available;
    };
    for(const auto& phase:workload) {
        const auto start=result.execution_ns;auto end=start;const auto directory_before=directory.stats();
        profiler.begin_phase(start,phase.measured);
        std::priority_queue<Event,std::vector<Event>,std::greater<Event>> events;
        std::vector<Event> blocked_requests;
        std::vector<std::vector<StepState>> states(phase.streams.size());
        std::vector<std::size_t> current(phase.streams.size(),0);
        std::unordered_map<std::size_t,std::size_t> by_node;
        for(std::size_t s=0;s<phase.streams.size();++s) {
            const auto& stream=phase.streams[s];
            if(stream.node>c.agents || !by_node.emplace(stream.node,s).second) throw std::invalid_argument("one stream per valid node required");
            states[s].resize(stream.steps.size());
            for(std::size_t i=0;i<stream.steps.size();++i) {
                const auto& step=stream.steps[i];
                for(auto duration:{step.compute_ns,step.concurrent_compute_ns})
                    if(!std::isfinite(duration) || duration<0) throw std::invalid_argument("invalid compute duration");
                states[s][i].requests.resize(step.accesses.size());
            }
            if(!stream.steps.empty()) {
                const auto compute=stream.steps.front().compute_ns;result.worker_compute_ns+=compute;
                events.push({start+compute,order++,s,0,Kind::ComputeDone});
            }
        }
        for(const auto& stream:phase.streams) for(const auto& step:stream.steps) if(step.sync_node!=-1) {
            auto target=by_node.find(static_cast<std::size_t>(step.sync_node));
            if(stream.node!=0 || step.sync_node<=0 || target==by_node.end() ||
                step.sync_step>=phase.streams[target->second].steps.size()) throw std::invalid_argument("invalid host pacing dependency");
        }
        while(!events.empty()) {
            const auto e=events.top();events.pop();end=std::max(end,e.time);
            const auto& stream=phase.streams[e.stream];const auto& step=stream.steps[e.step];auto& state=states[e.stream][e.step];
            if(e.kind==Kind::TryAccess) {
                auto& request=state.requests[e.access];const auto& access=step.accesses[e.access];
                if(management) {
                    const auto gate=management->gate(access.address);
                    if(gate==ManagementModel::Gate::Blocked) {blocked_requests.push_back(e);continue;}
                    if(gate==ManagementModel::Gate::Change) {
                        const auto change=management->begin_change(access.address,e.time,phase.measured,memory);
                        const auto done=network(e.time,management->change_bytes(change))+management->change_service(change);
                        result.control_bytes+=management->config().transition_message_bytes+
                            management->config().record_bytes*static_cast<double>(change.resident_lines);
                        result.writeback_bytes+=64*static_cast<double>(change.dirty_lines);
                        events.push({done,order++,e.stream,e.step,Kind::RegionChanged,e.access});continue;
                    }
                }
                const std::function<bool(std::uint64_t)> victim_allowed=management ?
                    std::function<bool(std::uint64_t)>([&](std::uint64_t address){return management->victim_allowed(address);}) :
                    std::function<bool(std::uint64_t)>{};
                if(!memory.admit(stream.node,access,request.transaction,request.id,victim_allowed)) {blocked_requests.push_back(e);continue;}
                request.admitted=e.time;request.admission_order=++admission_order;request.transaction.measured=phase.measured;
                if(management)management->admitted(request.transaction);
                result.dependency_wait_ns+=e.time-request.issued;
                profiler.access(e.time,stream.node,access.address,access.write,phase.measured);
                const auto& t=request.transaction.traffic;
                const auto controls=static_cast<double>(t.directory)*c.directory_bytes+static_cast<double>(t.evictions)*8+
                    (management ? management->request_bytes(request.transaction) : 0);
                result.control_bytes+=controls;
                events.push({network(e.time+(management ? management->check_ns() : 0),controls),order++,e.stream,e.step,Kind::DirectoryArrive,e.access});
                continue;
            }
            if(e.kind==Kind::RegionChanged) {
                management->finish_change(step.accesses[e.access].address,e.time,memory);
                events.push({e.time,order++,e.stream,e.step,Kind::TryAccess,e.access});
                for(auto waiter:blocked_requests) {waiter.time=e.time;waiter.order=order++;events.push(waiter);}blocked_requests.clear();
                continue;
            }
            if(e.kind==Kind::DirectoryArrive || e.kind==Kind::DirectoryIssue || e.kind==Kind::DirectoryProbe) {
                auto& request=state.requests[e.access];const auto& tx=request.transaction;
                if(e.operation==tx.operations.size()) {
                    const auto& t=tx.traffic;
                    const auto base=t.fills!=0 ? (tx.peer_fill ? c.peer_ns : c.memory_ns) : c.hit_ns;
                    const auto protocol=static_cast<double>(t.snoops)*c.snoop_ns+
                        static_cast<double>(t.invalidations)*c.invalidate_ns+static_cast<double>(t.writebacks)*c.writeback_ns;
                    events.push({e.time+base+protocol,order++,e.stream,e.step,Kind::DataReady,e.access});continue;
                }
                const auto& op=tx.operations[e.operation];const auto key=DirectoryModel::line_key(op.address);
                if(e.kind==Kind::DirectoryArrive && management) {
                    const auto ready=management->operation_ready(e.time,op.address,phase.measured);
                    if(ready>e.time) {
                        events.push({ready,order++,e.stream,e.step,Kind::DirectoryIssue,e.access,e.operation});continue;
                    }
                }
                if(e.kind==Kind::DirectoryArrive || e.kind==Kind::DirectoryIssue) {
                    const auto issue=directory.reserve(e.time,key,op.kind);
                    events.push({issue,order++,e.stream,e.step,Kind::DirectoryProbe,e.access,e.operation});
                } else {
                    const auto lookup=directory.lookup(e.time,key,op.kind);
                    if(lookup.miss) {
                        profiler.directory_miss(op.address,op.eviction,phase.measured);
                        if(op.eviction)++request.eviction_misses;else ++request.demand_misses;
                    }
                    events.push({lookup.time,order++,e.stream,e.step,lookup.retry ? Kind::DirectoryProbe : Kind::DirectoryArrive,
                        e.access,e.operation+(lookup.retry ? 0 : 1)});
                }
                continue;
            }
            if(e.kind==Kind::DataReady) {
                const auto& t=state.requests[e.access].transaction.traffic;
                const auto controls=static_cast<double>(t.snoops)*c.snoop_bytes+static_cast<double>(t.invalidations)*c.invalidate_bytes+
                    (management ? management->response_bytes(state.requests[e.access].transaction) : 0);
                const auto data=static_cast<double>(t.fills)*64,writeback=static_cast<double>(t.writebacks)*64;
                result.data_bytes+=data;result.writeback_bytes+=writeback;result.control_bytes+=controls;
                events.push({network(e.time,data+writeback+controls),order++,e.stream,e.step,Kind::Complete,e.access});continue;
            }
            if(e.kind==Kind::Complete) {
                auto& request=state.requests[e.access];const auto version=memory.complete(request.transaction);
                accumulate(result,request.transaction);
                if(management)management->completed(request.transaction);
                if(c.capture_events) {
                    EventRecord record;record.id=request.id;record.admission_order=request.admission_order;record.node=stream.node;record.address=request.transaction.access.address;
                    record.kind=request.transaction.access.kind;record.write=request.transaction.access.write;record.measured=phase.measured;
                    record.issued_ns=request.issued;record.admitted_ns=request.admitted;record.completed_ns=e.time;record.version=version;
                    record.traffic=request.transaction.traffic;record.has_victim=request.transaction.has_victim;record.victim=request.transaction.victim;
                    record.demand_misses=request.demand_misses;record.eviction_misses=request.eviction_misses;result.events.push_back(record);
                }
                for(auto waiter:blocked_requests) {waiter.time=e.time;waiter.order=order++;events.push(waiter);}blocked_requests.clear();
                --state.inflight;--outstanding;++state.completed;
            }
            if(e.kind==Kind::ComputeDone)state.ready=true;
            if(e.step!=current[e.stream] || state.closed)continue;
            if(!state.started) {
                if(!state.ready)throw std::logic_error("memory started before prerequisite compute");
                if(step.sync_node>=0 && !states[by_node.at(static_cast<std::size_t>(step.sync_node))][step.sync_step].started) {
                    if(!state.blocked)state.sync_wait_start=e.time;
                    state.blocked=true;continue;
                }
                if(state.blocked)result.host_sync_wait_ns+=e.time-state.sync_wait_start;
                state.blocked=false;state.started=true;state.wait_start=e.time;state.independent_end=e.time+step.concurrent_compute_ns;
                result.worker_compute_ns+=step.concurrent_compute_ns;
                for(std::size_t s=0;s<phase.streams.size();++s) if(current[s]<states[s].size()) {
                    const auto& waiting=phase.streams[s].steps[current[s]];
                    if(states[s][current[s]].blocked && waiting.sync_node==static_cast<int>(stream.node) && waiting.sync_step==e.step)
                        events.push({e.time,order++,s,current[s],Kind::StartBatch});
                }
            }
            const bool all=state.completed==step.accesses.size();
            const auto trigger=stream.node==0 ? 1.0 : c.overlap_trigger;
            const auto threshold=static_cast<std::size_t>(std::ceil(static_cast<double>(step.accesses.size())*trigger));
            if(!state.next_compute && state.completed>=threshold &&
                (step.allow_lookahead || (all && e.time>=state.independent_end)) && e.step+1<stream.steps.size()) {
                state.next_compute=true;state.next_start=std::max(e.time,state.independent_end);
                const auto compute=stream.steps[e.step+1].compute_ns;state.next_end=state.next_start+compute;
                result.worker_compute_ns+=compute;events.push({state.next_end,order++,e.stream,e.step+1,Kind::ComputeDone});
            }
            while(state.issued<step.accesses.size() && state.inflight<c.outstanding) {
                const auto a=state.issued++;state.requests[a].id=++request_id;state.requests[a].issued=e.time;
                events.push({e.time,order++,e.stream,e.step,Kind::TryAccess,a});++state.inflight;++outstanding;
                result.peak_inflight=std::max(result.peak_inflight,outstanding);
            }
            if(all) {
                if(!state.returned) {
                    state.returned=true;state.memory_end=e.time;result.worker_memory_wait_ns+=e.time-state.wait_start;
                    result.overlap_ns+=std::max(0.0,std::min(e.time,state.independent_end)-state.wait_start);
                }
                if(e.time<state.independent_end) {
                    if(!state.join_queued){state.join_queued=true;events.push({state.independent_end,order++,e.stream,e.step,Kind::Join});}continue;
                }
                if(state.next_compute)result.overlap_ns+=std::max(0.0,std::min(state.memory_end,state.next_end)-state.next_start);
                state.closed=true;++current[e.stream];
                if(current[e.stream]<states[e.stream].size() && states[e.stream][current[e.stream]].ready)
                    events.push({e.time,order++,e.stream,current[e.stream],Kind::StartBatch});
            }
        }
        if(!blocked_requests.empty() || outstanding!=0)throw std::logic_error("pending memory transactions at phase end");
        for(std::size_t s=0;s<states.size();++s)if(current[s]!=states[s].size())throw std::logic_error("unfinished stream or pacing deadlock");
        result.phases.push_back({phase.label,start,end,phase.measured});result.execution_ns=end;profiler.end_phase(end,phase.measured);
        if(phase.measured) {
            result.measured_execution_ns+=end-start;
            for(std::size_t k=0;k<kind_count;++k)result.measured_directory[k]+=directory.stats()[k]-directory_before[k];
        }
    }
    result.directory=directory.stats();result.peak_cache_lines=memory.peak_cache_lines();return result;
}
} // namespace perf
