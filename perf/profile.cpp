#include "model.hpp"
#include "coherence/region.hpp"

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace perf {
const char* name(AccessKind kind) {
    switch (kind) {
    case AccessKind::Metadata: return "metadata";
    case AccessKind::PrefixKV: return "prefix_kv";
    case AccessKind::LeafKV: return "leaf_kv";
    case AccessKind::HotEmbeddings: return "hot_embeddings";
    case AccessKind::ColdEmbeddings: return "cold_embeddings";
    case AccessKind::PrivateScratchpad: return "private_scratchpad";
    }
    throw std::logic_error("invalid access kind");
}
Traffic& Traffic::operator+=(const Traffic& r) {
    accesses += r.accesses; reads += r.reads; writes += r.writes;
    hits += r.hits; fills += r.fills; directory += r.directory; snoops += r.snoops;
    invalidations += r.invalidations; writebacks += r.writebacks;
    evictions += r.evictions; dirty_evictions += r.dirty_evictions;
    region_grants += r.region_grants; region_barriers += r.region_barriers;
    region_recoveries += r.region_recoveries; region_downgrades += r.region_downgrades; revoked_copies += r.revoked_copies;
    collateral_copies += r.collateral_copies; region_flushes += r.region_flushes;
    return *this;
}

Traffic Traffic::operator-(const Traffic& r) const {
    auto d=*this;
    d.accesses-=r.accesses; d.reads-=r.reads; d.writes-=r.writes; d.hits-=r.hits; d.fills-=r.fills;
    d.directory-=r.directory; d.snoops-=r.snoops; d.invalidations-=r.invalidations; d.writebacks-=r.writebacks;
    d.evictions-=r.evictions; d.dirty_evictions-=r.dirty_evictions;
    d.region_grants-=r.region_grants; d.region_barriers-=r.region_barriers;
    d.region_recoveries-=r.region_recoveries; d.region_downgrades-=r.region_downgrades;
    d.revoked_copies-=r.revoked_copies; d.collateral_copies-=r.collateral_copies; d.region_flushes-=r.region_flushes;
    return d;
}

Profile profile_workload(const Workload& workload, const Config& config, const Scenario& scenario) {
    config.validate();
    Profile profile;
    profile.workload = workload;
    std::map<std::uint64_t, std::size_t> regions;
    std::map<std::uint64_t, AccessKind> address_kinds;
    std::vector<coherence::RegionSpec> specs;
    std::vector<AccessKind> region_kinds;
    std::map<std::uint64_t, std::size_t> scratch_owners;
    std::set<std::size_t> prefixes;
    for (const auto& phase : workload)
        for (const auto& stream : phase.streams) {
            if (stream.node > config.agents) throw std::invalid_argument("node exceeds accelerator count");
            for (const auto& step : stream.steps)
                for (const auto& access : step.accesses) {
                    if (access.address % 64 != 0) throw std::invalid_argument("access must be 64B aligned");
                    const auto line_kind = address_kinds.emplace(access.address, access.kind);
                    if (line_kind.first->second != access.kind)
                        throw std::invalid_argument("one address cannot have two memory classes");
                    const auto page = access.address / config.region_bytes;
                    auto insertion = regions.emplace(page, specs.size());
                    const auto kind = access.kind == AccessKind::Metadata ? coherence::MemoryKind::Metadata : coherence::MemoryKind::KV;
                    if (insertion.second) {
                        specs.push_back({kind, name(access.kind)});
                        region_kinds.push_back(access.kind);
                    } else if (specs.at(insertion.first->second).kind != kind ||
                        (config.workload == WorkloadMode::DLRM && region_kinds.at(insertion.first->second) != access.kind))
                        throw std::invalid_argument("distinct memory classes must occupy separate regions");
                    if (access.kind == AccessKind::PrivateScratchpad) {
                        const auto owner = scratch_owners.emplace(page, stream.node);
                        if (stream.node == 0 || owner.first->second != stream.node)
                            throw std::invalid_argument("scratch pages must belong to one accelerator");
                    }
                    if (access.kind == AccessKind::PrefixKV) prefixes.insert(insertion.first->second);
                }
        }
    profile.prefix_regions = prefixes.size();
    coherence::SystemConfig system;
    system.accelerators = config.agents;
    system.region_bytes = config.region_bytes;
    coherence::Metrics metrics;
    coherence::RegionStateMachine machine(system, specs, metrics, coherence::Policy::Baseline);
    std::vector<std::list<std::uint64_t>> lru(config.agents + 1);
    std::set<std::size_t> granted;
    const auto location = [&](std::uint64_t address) {
        return std::make_pair(regions.at(address / config.region_bytes),
                              static_cast<std::size_t>((address % config.region_bytes) / 64));
    };
    const auto valid = [&](std::size_t node, std::uint64_t address) {
        const auto pos = location(address);
        return machine.region(pos.first).lines.at(pos.second).copies.at(node).valid;
    };
    const auto touch = [&](std::size_t node, Access& access) {
        const auto before = metrics;
        Service service;
        auto& t = service.traffic;
        const auto pos = location(access.address);
        const auto barrier = [&](bool private_page) {
            ++t.region_barriers;
            service.region_latency_ns += config.region_barrier_ns;
            // RO bypass has no per-line reader directory: broadcast to every endpoint.
            service.region_control_bytes += config.directory_bytes *
                static_cast<double>(private_page ? 2 : config.agents + 2);
        };
        const bool bypass = bypass_kind(scenario, access.kind);
        const bool private_page = access.kind == AccessKind::PrivateScratchpad || access.kind == AccessKind::LeafKV;
        const bool had_grant = granted.count(pos.first) != 0;
        if (bypass && granted.insert(pos.first).second) {
            ++t.region_grants;
            barrier(private_page);
            service.region_latency_ns += config.setup_ns;
        }
        bool remote_copy = false, remote_dirty = false;
        for (const auto& line : machine.region(pos.first).lines)
            for (std::size_t n = 0; n < line.copies.size(); ++n)
                if (n != node && line.copies[n].valid) {
                    remote_copy = true;
                    remote_dirty = remote_dirty || line.copies[n].dirty;
                }
        if (bypass && !access.write && scenario.strategy == Strategy::CoarseAll && remote_dirty) {
            // A remote read of any line downgrades the whole dirty page, even if
            // the requested line is clean: this is another source of false sharing.
            ++t.region_downgrades;
            barrier(false);
            const auto wb_before = metrics.writebacks;
            machine.writeback_region(pos.first);
            t.region_flushes += metrics.writebacks - wb_before;
        }
        const bool cold_write = scenario.strategy == Strategy::SelectiveBypass &&
            access.kind == AccessKind::ColdEmbeddings && access.write;
        const bool revoke = bypass && access.write &&
            ((cold_write && had_grant) || (scenario.strategy == Strategy::CoarseAll && remote_copy));
        if (revoke) {
            ++t.region_recoveries;
            barrier(false); // revoke/ack before granting write permission
            service.region_latency_ns += config.recovery_ns;
            service.region_control_bytes += config.recovery_bytes;
            const auto wb_before = metrics.writebacks;
            const auto count = machine.region(pos.first).lines.size();
            for (std::size_t line = 0; line < count; ++line)
                for (std::size_t n = 0; n <= config.agents; ++n)
                    if (machine.region(pos.first).lines[line].copies[n].valid) {
                        ++t.revoked_copies;
                        if (line != pos.second) ++t.collateral_copies;
                        machine.evict(static_cast<int>(n), pos.first, line);
                    }
            t.region_flushes += metrics.writebacks - wb_before;
        }
        auto& order = lru.at(node);
        for (auto it = order.begin(); it != order.end();) {
            if (!valid(node, *it)) it = order.erase(it);
            else ++it;
        }
        auto victim_kind = access.kind;
        bool capacity_dirty = false;
        const auto existing = std::find(order.begin(), order.end(), access.address);
        if (existing != order.end()) order.erase(existing);
        else if (config.cache_bytes != 0 && order.size() == config.cache_bytes / 64) {
            const auto victim = location(order.back());
            victim_kind = address_kinds.at(order.back());
            ++t.evictions;
            capacity_dirty = machine.evict(static_cast<int>(node), victim.first, victim.second);
            if (capacity_dirty) ++t.dirty_evictions;
            order.pop_back();
        }
        const auto& line = machine.region(pos.first).lines.at(pos.second);
        service.peer_fill = line.exclusive_owner != coherence::no_owner &&
            line.exclusive_owner != static_cast<int>(node);
        coherence::Request request;
        request.node = static_cast<int>(node);
        request.region = pos.first;
        request.line = pos.second;
        request.operation = access.write ? coherence::Operation::Write : coherence::Operation::Read;
        machine.access(request);
        // Republish cold writes / revoked coarse pages clean before bypass readers resume.
        if (bypass && access.write && (cold_write || revoke)) {
            const auto wb_before = metrics.writebacks;
            machine.writeback_region(pos.first);
            t.region_flushes += metrics.writebacks - wb_before;
            barrier(false);
        }
        if (config.cache_bytes == 0) {
            ++t.evictions;
            if (machine.evict(static_cast<int>(node), pos.first, pos.second)) ++t.dirty_evictions;
        } else order.push_front(access.address);
        // Flush data appears once in traffic below; service uses bounded parallel waves.
        service.region_latency_ns += std::ceil(static_cast<double>(t.region_flushes) /
            static_cast<double>(config.outstanding)) * (config.memory_ns + config.writeback_ns);
        t.accesses = 1; t.reads = access.write ? 0 : 1; t.writes = access.write ? 1 : 0;
        t.hits = metrics.cache_hits - before.cache_hits;
        t.fills = metrics.data_fills - before.data_fills;
        t.directory = metrics.directory_lookups - before.directory_lookups;
        t.snoops = metrics.snoops - before.snoops;
        t.invalidations = metrics.invalidations - before.invalidations;
        t.writebacks = metrics.writebacks - before.writebacks;
        access.service = service;
        profile.kinds[index(access.kind)] += t;
        // Service is charged to the request that triggered eviction, but exports
        // attribute eviction/writeback counters to the actual victim memory class.
        if (victim_kind != access.kind) {
            auto& triggering = profile.kinds[index(access.kind)];
            auto& victim = profile.kinds[index(victim_kind)];
            --triggering.evictions; ++victim.evictions;
            if (capacity_dirty) {
                --triggering.dirty_evictions; ++victim.dirty_evictions;
                --triggering.writebacks; ++victim.writebacks;
            }
        }
    };

    // Fixed logical interleaving, independent of scenario timing: step ordinal,
    // access ordinal, then streams in node order. Timing does not reorder this profile.
    for (auto& phase : profile.workload) {
        const auto before_phase=profile.kinds;
        if (phase.fork_boundary) {
            for (const auto id : prefixes)
                for (const auto& line : machine.region(id).lines)
                    for (const auto& copy : line.copies)
                        if (copy.valid && copy.dirty) ++profile.fork_dirty_lines;
            if (scenario.counterfactual && config.flush_at_fork) {
                const auto before = metrics.writebacks;
                for (const auto id : prefixes) machine.writeback_region(id);
                profile.fork_flushes += metrics.writebacks - before;
            }
        }
        std::size_t steps = 0;
        for (const auto& stream : phase.streams) steps = std::max(steps, stream.steps.size());
        for (std::size_t s = 0; s < steps; ++s) {
            std::size_t accesses = 0;
            for (const auto& stream : phase.streams)
                if (s < stream.steps.size()) accesses = std::max(accesses, stream.steps[s].accesses.size());
            for (std::size_t a = 0; a < accesses; ++a)
                for (auto& stream : phase.streams)
                    if (s < stream.steps.size() && a < stream.steps[s].accesses.size())
                        touch(stream.node, stream.steps[s].accesses[a]);
        }
        if (phase.measured) for (std::size_t k=0;k<kind_count;++k)
            profile.measured_kinds[k] += profile.kinds[k]-before_phase[k];
    }
    return profile;
}
} // namespace perf
