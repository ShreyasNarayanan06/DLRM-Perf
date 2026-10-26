#include "model.hpp"
#include "directory.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <deque>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace perf {
void Config::validate() const {
    if (warmup_rounds >= rounds || directory_banks == 0 || directory_mshrs == 0)
        throw std::invalid_argument("warmup must be smaller than rounds; directory banks/MSHRs must be positive");
    if (agents == 0 || agents > 63 || lead_steps == 0 || rounds == 0 || outstanding == 0 ||
        prefix_lines == 0 || leaf_lines == 0 || stores_per_line == 0 ||
        table_lines < 2 || sparse_reads == 0 || scratch_lines == 0)
        throw std::invalid_argument("agents must be 1..63; steps, windows and working sets must be positive");
    if (cache_bytes % 64 != 0 || region_bytes == 0 || region_bytes % 64 != 0 ||
        (region_bytes & (region_bytes - 1)) != 0 || region_bytes > (1U << 20))
        throw std::invalid_argument("cache bytes must be a multiple of 64; region size must be a power of two from 64B to 1MiB");
    if (prefix_lines > (1U << 24) || leaf_lines > (1U << 24) ||
        table_lines > (1U << 24) || scratch_lines > (1U << 24))
        throw std::invalid_argument("working set exceeds supported address range");
    for (const auto value : {compute_ns, host_compute_ns, hit_ns, memory_ns, peer_ns, directory_ns,
            snoop_ns, invalidate_ns, writeback_ns, directory_bytes, snoop_bytes, invalidate_bytes,
            fork_barrier_ns, region_barrier_ns, setup_ns, recovery_ns, recovery_bytes, added_latency_ns,
            directory_issue_ns, directory_miss_ns})
        if (!std::isfinite(value) || value < 0) throw std::invalid_argument("costs must be finite and nonnegative");
    for (const auto value : {overlap_trigger, recovery_rate, hot_read_fraction, bottom_fraction})
        if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("fractions must be in [0, 1]");
    if (!std::isfinite(hot_fraction) || hot_fraction <= 0 || hot_fraction >= 1 ||
        !std::isfinite(zipf_alpha) || zipf_alpha < 0)
        throw std::invalid_argument("hot fraction must be in (0, 1); Zipf alpha must be finite and nonnegative");
    if (!std::isfinite(directory_backing_gbps) || directory_backing_gbps <= 0)
        throw std::invalid_argument("directory backing bandwidth must be finite and positive");
    if (!std::isfinite(bandwidth_gbps) || bandwidth_gbps <= 0)
        throw std::invalid_argument("bandwidth must be finite and positive");
}

namespace {
Step make_step(const Config& c, std::size_t round, std::size_t node, bool lead) {
    Step result;
    result.compute_ns = node == 0 ? c.host_compute_ns : c.compute_ns;
    if (node != 0 && !lead)
        for (std::size_t r = 0; r < c.prefix_reads; ++r)
            result.accesses.push_back({AccessKind::PrefixKV, ((round * c.prefix_reads + r) % c.prefix_lines) * 64, false, {}});
    if (node != 0)
        for (std::size_t w = 0; w < c.leaf_writes; ++w) {
            const auto line = (round * c.leaf_writes + w) / c.stores_per_line;
            const auto address = lead ? (line % c.prefix_lines) * 64 :
                (1ULL << 40) + node * (1ULL << 32) + (line % c.leaf_lines) * 64;
            result.accesses.push_back({lead ? AccessKind::PrefixKV : AccessKind::LeafKV, address, true, {}});
        }
    for (std::size_t m = 0; m < c.metadata_ops; ++m)
        result.accesses.push_back({AccessKind::Metadata, (1ULL << 60) + (m % 8) * 64, m % 2 == 0, {}});
    return result;
}
enum class EventKind { ComputeDone, StartBatch, Completion, StageGate, RequestReady, DirectoryReady, DirectoryIssue };
struct Event {
    double time;
    std::uint64_t order;
    std::size_t stream, step;
    EventKind kind;
    std::size_t access = 0, directory_index = 0;
    bool page_released = false;
    bool operator>(const Event& r) const { return std::tie(time, order) > std::tie(r.time, r.order); }
};
struct RequestState { double tail_ns = 0; };
struct PageState { std::size_t active = 0; bool barrier = false; std::deque<Event> waiting; };
struct StepState {
    std::vector<RequestState> requests;
    bool ready = false, started = false, next_compute = false, closed = false, blocked = false;
    bool memory_done = false, gate_scheduled = false;
    std::size_t issued = 0, completed = 0, inflight = 0;
    double wait_start = 0, next_compute_start = 0, next_compute_end = 0;
    double independent_end = 0, memory_end = 0, sync_wait_start = 0;
};
} // namespace

Workload make_dlrm_workload(const Config& c);
namespace {
Workload split_warmup(Workload workload, const Config& c) {
    if (c.warmup_rounds==0) return workload;
    Workload result;
    for (auto& phase:workload) {
        if (phase.label!="parallel_children" && phase.label!="inference_with_online_updates") {
            phase.measured=false; result.push_back(std::move(phase)); continue;
        }
        const auto split=c.warmup_rounds*(c.workload==WorkloadMode::DLRM ? 2 : 1);
        Phase warmup{phase.label+"_warmup",{},phase.fork_boundary,false};
        Phase measured{phase.label+"_measured",{},false,true};
        for (auto& stream:phase.streams) {
            Stream first{stream.node,{}},second{stream.node,{}};
            for (std::size_t i=0;i<stream.steps.size();++i) {
                auto step=std::move(stream.steps[i]);
                if (i>=split && step.sync_node>=0) step.sync_step-=split;
                (i<split ? first.steps : second.steps).push_back(std::move(step));
            }
            warmup.streams.push_back(std::move(first)); measured.streams.push_back(std::move(second));
        }
        result.push_back(std::move(warmup)); result.push_back(std::move(measured));
    }
    return result;
}
}
Workload make_workload(const Config& c) {
    c.validate();
    if (c.workload == WorkloadMode::DLRM) return split_warmup(make_dlrm_workload(c),c);
    Workload w;
    Phase lead{"lead", {{1, {}}}, false};
    for (std::size_t r = 0; r < c.lead_steps; ++r) lead.streams.front().steps.push_back(make_step(c, r, 1, true));
    w.push_back(std::move(lead));
    Phase children{"parallel_children", {}, true};
    for (std::size_t node = 0; node <= c.agents; ++node) {
        Stream stream{node, {}};
        for (std::size_t r = 0; r < c.rounds; ++r) stream.steps.push_back(make_step(c, r, node, false));
        children.streams.push_back(std::move(stream));
    }
    w.push_back(std::move(children));
    Phase complete{"completion", {{0, {}}}, false};
    for (std::size_t n = 0; n <= c.agents; ++n) complete.streams.front().steps.push_back(make_step(c, 0, 0, false));
    w.push_back(std::move(complete));
    return split_warmup(std::move(w),c);
}
Result simulate(const Workload& workload, const Config& c, const Scenario& scenario) {
    c.validate();
    for (const auto fraction : {scenario.latency_retained, scenario.traffic_retained})
        if (!std::isfinite(fraction) || fraction < 0 || fraction > 1)
            throw std::invalid_argument("retained fractions must be in [0, 1]");
    const auto profile = profile_workload(workload, c, scenario);
    Result result;
    result.scenario = scenario;
    result.workload = c.workload;
    result.trace_fingerprint = workload_fingerprint(workload);
    result.kinds = profile.kinds;
    result.measured_kinds = profile.measured_kinds;
    DirectoryModel directory(c);
    result.fork_flushes = profile.fork_flushes;
    result.prefix_regions = profile.prefix_regions;
    double link_available = 0;
    std::size_t inflight = 0;
    std::uint64_t order = 0;
    std::vector<std::unordered_map<std::uint64_t, double>> line_ready(c.agents + 1);
    const auto network = [&](double issue, double bytes) {
        if (bytes == 0) return issue;
        const auto start = std::max(issue, link_available);
        const auto duration = bytes / c.bandwidth_gbps;
        result.queue_wait_ns += start - issue;
        result.link_busy_ns += duration;
        link_available = start + duration;
        return link_available;
    };
    std::unordered_map<std::uint64_t, double> page_gate, page_access_end;
    std::vector<std::set<std::uint64_t>> busy_lines(c.agents+1);
    std::vector<std::unordered_map<std::uint64_t,std::vector<Event>>> line_waiters(c.agents+1);
    std::unordered_map<std::uint64_t,PageState> active_pages;
    bool fork_processed = false;
    for (const auto& phase : profile.workload) {
        const auto directory_before=directory.stats();
        if (phase.fork_boundary && !fork_processed) {
            fork_processed = true;
            const auto begin = result.execution_ns;
            if (scenario.counterfactual && profile.prefix_regions != 0) {
                result.execution_ns += c.setup_ns + c.fork_barrier_ns +
                    static_cast<double>(profile.prefix_regions) * c.region_barrier_ns;
                const auto controls = c.directory_bytes * static_cast<double>(c.agents + 1 + profile.prefix_regions);
                result.control_bytes += controls;
                result.execution_ns = network(result.execution_ns, controls);
                std::priority_queue<double, std::vector<double>, std::greater<double>> pending;
                for (std::uint64_t n = 0; n < profile.fork_flushes; ++n) {
                    auto issue = result.execution_ns;
                    if (pending.size() == c.outstanding) { issue = pending.top(); pending.pop(); }
                    const auto done = network(issue, 64) + c.memory_ns + c.writeback_ns;
                    pending.push(done);
                    result.writeback_bytes += 64;
                }
                while (!pending.empty()) {
                    result.execution_ns = std::max(result.execution_ns, pending.top()); pending.pop();
                }
            }
            result.fork_penalty_ns = result.execution_ns - begin;
            result.phases.push_back({"fork_overhead", begin, result.execution_ns,phase.measured});
            if (phase.measured) result.measured_execution_ns+=result.execution_ns-begin;
        }
        const auto phase_start = result.execution_ns;
        auto phase_end = phase_start;
        std::priority_queue<Event, std::vector<Event>, std::greater<Event>> events;
        std::vector<std::vector<StepState>> states(phase.streams.size());
        std::vector<std::size_t> current(phase.streams.size(), 0);
        std::set<std::size_t> nodes;
        std::unordered_map<std::size_t, std::size_t> stream_by_node;
        for (std::size_t s = 0; s < phase.streams.size(); ++s) {
            const auto& stream = phase.streams[s];
            if (!nodes.insert(stream.node).second) throw std::invalid_argument("one stream per node is required");
            stream_by_node.emplace(stream.node, s);
            for (const auto& step : stream.steps)
                for (const auto duration : {step.compute_ns, step.concurrent_compute_ns})
                    if (!std::isfinite(duration) || duration < 0) throw std::invalid_argument("invalid compute duration");
            states[s].resize(stream.steps.size());
            for (std::size_t i=0;i<stream.steps.size();++i) states[s][i].requests.resize(stream.steps[i].accesses.size());
            if (stream.steps.empty()) continue;
            result.worker_compute_ns += stream.steps.front().compute_ns;
            events.push({phase_start + stream.steps.front().compute_ns, order++, s, 0, EventKind::ComputeDone});
        }
        // Only host-to-accelerator pacing dependencies are supported; no cyclic waits.
        for (const auto& stream : phase.streams) for (const auto& step : stream.steps)
            if (step.sync_node != -1) {
                const auto target = stream_by_node.find(static_cast<std::size_t>(step.sync_node));
                if (stream.node != 0 || step.sync_node <= 0 || target == stream_by_node.end() ||
                    step.sync_step >= phase.streams[target->second].steps.size())
                    throw std::invalid_argument("invalid host pacing dependency");
            }
        while (!events.empty()) {
            const auto event = events.top(); events.pop();
            phase_end = std::max(phase_end, event.time);
            const auto& stream = phase.streams[event.stream];
            const auto& step = stream.steps.at(event.step);
            auto& state = states[event.stream].at(event.step);
            if (event.kind == EventKind::DirectoryReady || event.kind == EventKind::DirectoryIssue) {
                const auto& access=step.accesses[event.access];
                const auto& t=access.service.traffic;
                const bool cold_store=scenario.strategy==Strategy::SelectiveBypass &&
                    access.kind==AccessKind::ColdEmbeddings && access.write;
                const auto fraction=cold_store ? 1 : latency_fraction(scenario,access.kind);
                const auto normal=fraction>0 ? t.directory : 0;
                const auto total=normal+t.region_barriers;
                if (event.directory_index==total) {
                    events.push({event.time+state.requests[event.access].tail_ns,order++,event.stream,event.step,
                        EventKind::Completion,event.access});
                    continue;
                }
                const bool region=event.directory_index>=normal;
                const auto key=region ? DirectoryModel::region_key(access.address,c.region_bytes) : DirectoryModel::line_key(access.address);
                const auto weight=region ? 1 : fraction;
                if (event.kind==EventKind::DirectoryReady) {
                    const auto start=directory.reserve(event.time,key,weight,access.kind);
                    events.push({start,order++,event.stream,event.step,EventKind::DirectoryIssue,event.access,event.directory_index});
                } else {
                    const auto lookup=directory.lookup(event.time,key,weight,access.kind);
                    events.push({lookup.time,order++,event.stream,event.step,
                        lookup.retry ? EventKind::DirectoryIssue : EventKind::DirectoryReady,event.access,
                        event.directory_index+(lookup.retry ? 0 : 1)});
                }
                continue;
            }
            if (event.kind == EventKind::RequestReady) {
                const auto& access = step.accesses[event.access];
                const auto& t = access.service.traffic;
                const bool kv = access.kind != AccessKind::Metadata;
                const bool cold_store = scenario.strategy == Strategy::SelectiveBypass &&
                    access.kind == AccessKind::ColdEmbeddings && access.write;
                const auto fraction_ns = cold_store ? 1 : latency_fraction(scenario, access.kind);
                const auto fraction_bytes = cold_store ? 1 : traffic_fraction(scenario, access.kind);
                const auto coherence = static_cast<double>(t.directory) * c.directory_ns +
                    static_cast<double>(t.snoops) * c.snoop_ns + static_cast<double>(t.invalidations) * c.invalidate_ns;
                const auto controls = static_cast<double>(t.directory) * c.directory_bytes +
                    static_cast<double>(t.snoops) * c.snoop_bytes + static_cast<double>(t.invalidations) * c.invalidate_bytes;
                const auto recoveries = kv && scenario.counterfactual && scenario.strategy == Strategy::EventBudget &&
                    t.directory != 0 ? c.recovery_rate : 0;
                const auto extra = kv && scenario.counterfactual ? c.added_latency_ns : 0;
                const auto control = controls * fraction_bytes + static_cast<double>(t.evictions) * 8 +
                    recoveries * c.recovery_bytes + access.service.region_control_bytes;
                const auto payload = static_cast<double>(t.fills + t.writebacks) * 64;
                const auto page=access.address/c.region_bytes;
                auto earliest=event.time;
                if (c.directory_model) {
                    if (busy_lines[stream.node].count(access.address)!=0) {
                        line_waiters[stream.node][access.address].push_back(event); continue;
                    }
                    if (bypass_kind(scenario,access.kind)) {
                        auto& active=active_pages[page];
                        const bool barrier=t.region_barriers!=0;
                        if (active.barrier || (barrier && active.active!=0) ||
                            (!active.waiting.empty() && !event.page_released)) {
                            active.waiting.push_back(event); continue;
                        }
                        ++active.active; active.barrier=barrier;
                    }
                    busy_lines[stream.node].insert(access.address);
                } else {
                    earliest=std::max(event.time,line_ready[stream.node][access.address]);
                    if (bypass_kind(scenario,access.kind)) {
                        earliest=std::max(earliest,page_gate[page]);
                        if (access.service.region_latency_ns!=0 || access.service.region_control_bytes!=0)
                            earliest=std::max(earliest,page_access_end[page]);
                    }
                    if (earliest>event.time) {
                        events.push({earliest,order++,event.stream,event.step,EventKind::RequestReady,event.access}); continue;
                    }
                }
                const auto base = t.fills != 0 ? (access.service.peer_fill ? c.peer_ns : c.memory_ns) : c.hit_ns;
                const auto link_done=network(earliest,payload+control);
                const auto tail=base+coherence*fraction_ns+
                    static_cast<double>(t.writebacks-t.region_flushes)*c.writeback_ns+
                    static_cast<double>(t.evictions)*4+recoveries*c.recovery_ns+extra+access.service.region_latency_ns;
                if (c.directory_model) {
                    state.requests[event.access].tail_ns=tail-static_cast<double>(t.directory)*c.directory_ns*fraction_ns;
                    events.push({link_done,order++,event.stream,event.step,EventKind::DirectoryReady,event.access});
                } else {
                    const auto done=link_done+tail;
                    line_ready[stream.node][access.address]=done;
                    if (bypass_kind(scenario,access.kind)) {
                        page_access_end[page]=std::max(page_access_end[page],done);
                        if (access.service.region_latency_ns!=0 || access.service.region_control_bytes!=0) page_gate[page]=done;
                    }
                    events.push({done,order++,event.stream,event.step,EventKind::Completion,event.access});
                }
                result.data_bytes += static_cast<double>(t.fills) * 64;
                result.writeback_bytes += static_cast<double>(t.writebacks) * 64;
                result.control_bytes += control;
                result.assumed_coherence_latency_ns += coherence * fraction_ns;
                result.charged_directory += static_cast<double>(t.directory) * fraction_ns;
                result.charged_snoops += static_cast<double>(t.snoops) * fraction_ns;
                result.charged_invalidations += static_cast<double>(t.invalidations) * fraction_ns;
                result.region_penalty_ns += access.service.region_latency_ns;
                result.added_latency_ns += recoveries * c.recovery_ns + extra;
                result.expected_recoveries += recoveries;
                ++result.requests;
                if (payload + control != 0) ++result.offchip_requests;
                continue;
            }
            if (event.kind == EventKind::ComputeDone) state.ready = true;
            if (event.step != current[event.stream] || state.closed) continue;
            if (!state.started) {
                if (!state.ready) throw std::logic_error("memory batch started before compute");
                if (step.sync_node >= 0) {
                    const auto target = stream_by_node.at(static_cast<std::size_t>(step.sync_node));
                    if (!states[target][step.sync_step].started) {
                        if (!state.blocked) state.sync_wait_start = event.time;
                        state.blocked = true;
                        continue;
                    }
                }
                if (state.blocked) result.host_sync_wait_ns += event.time - state.sync_wait_start;
                state.blocked = false;
                state.started = true; state.wait_start = event.time;
                state.independent_end = event.time + step.concurrent_compute_ns;
                result.worker_compute_ns += step.concurrent_compute_ns;
                // Wake host stages waiting for this accelerator's batch start.
                for (std::size_t s = 0; s < phase.streams.size(); ++s) {
                    const auto next = current[s];
                    if (next >= states[s].size()) continue;
                    const auto& waiting = phase.streams[s].steps[next];
                    if (states[s][next].blocked && waiting.sync_node == static_cast<int>(stream.node) &&
                        waiting.sync_step == event.step)
                        events.push({event.time, order++, s, next, EventKind::StartBatch});
                }
            }
            if (event.kind == EventKind::Completion) {
                if (c.directory_model) {
                    const auto& access=step.accesses[event.access];
                    busy_lines[stream.node].erase(access.address);
                    auto& waiting=line_waiters[stream.node][access.address];
                    for (auto waiter:waiting) { waiter.time=event.time; waiter.order=order++; events.push(waiter); }
                    waiting.clear();
                    if (bypass_kind(scenario,access.kind)) {
                        auto& active=active_pages[access.address/c.region_bytes];
                        --active.active;
                        if (access.service.traffic.region_barriers!=0) active.barrier=false;
                        if (active.active==0 && !active.waiting.empty()) {
                            bool released=false;
                            while (!active.waiting.empty()) {
                                auto waiter=active.waiting.front();
                                const auto& queued=phase.streams[waiter.stream].steps[waiter.step].accesses[waiter.access];
                                const bool barrier=queued.service.traffic.region_barriers!=0;
                                if (released && barrier) break;
                                active.waiting.pop_front(); waiter.time=event.time; waiter.order=order++; waiter.page_released=true;
                                events.push(waiter); released=true;
                                if (barrier) break;
                            }
                        }
                    }
                }
                --state.inflight; --inflight; ++state.completed;
            }
            const auto trigger = stream.node == 0 ? 1.0 : c.overlap_trigger;
            const auto threshold = static_cast<std::size_t>(std::ceil(static_cast<double>(step.accesses.size()) * trigger));
            const bool all_returned = state.completed == step.accesses.size();
            if (!state.next_compute && state.completed >= threshold &&
                (step.allow_lookahead || (all_returned && event.time >= state.independent_end)) &&
                event.step + 1 < stream.steps.size()) {
                state.next_compute = true;
                state.next_compute_start = std::max(event.time, state.independent_end);
                const auto compute = stream.steps[event.step + 1].compute_ns;
                state.next_compute_end = state.next_compute_start + compute;
                result.worker_compute_ns += compute;
                events.push({state.next_compute_end, order++, event.stream, event.step + 1, EventKind::ComputeDone});
            }
            while (state.issued < step.accesses.size() && state.inflight < c.outstanding) {
                const auto access = state.issued++;
                events.push({event.time, order++, event.stream, event.step, EventKind::RequestReady, access});
                ++state.inflight; ++inflight;
                result.peak_inflight = std::max(result.peak_inflight, inflight);
            }
            if (all_returned) {
                if (!state.memory_done) {
                    state.memory_done = true; state.memory_end = event.time;
                    result.worker_memory_wait_ns += event.time - state.wait_start;
                    result.overlap_ns += std::max(0.0, std::min(event.time, state.independent_end) - state.wait_start);
                }
                if (event.time < state.independent_end) {
                    if (!state.gate_scheduled) {
                        state.gate_scheduled = true;
                        events.push({state.independent_end, order++, event.stream, event.step, EventKind::StageGate});
                    }
                    continue;
                }
                if (state.next_compute)
                    result.overlap_ns += std::max(0.0, std::min(state.memory_end, state.next_compute_end) - state.next_compute_start);
                state.closed = true;
                ++current[event.stream];
                if (current[event.stream] < stream.steps.size() && states[event.stream][current[event.stream]].ready)
                    events.push({event.time, order++, event.stream, current[event.stream], EventKind::StartBatch});
            }
        }
        for (std::size_t s = 0; s < states.size(); ++s)
            if (current[s] != states[s].size()) throw std::logic_error("unfinished stream / pacing deadlock");
        result.phases.push_back({phase.label, phase_start, phase_end,phase.measured});
        if (phase.measured) {
            result.measured_execution_ns+=phase_end-phase_start;
            for (std::size_t k=0;k<kind_count;++k) result.measured_directory[k]+=directory.stats()[k]-directory_before[k];
        }
        result.execution_ns = phase_end;
    }
    result.directory=directory.stats();
    return result;
}
} // namespace perf
