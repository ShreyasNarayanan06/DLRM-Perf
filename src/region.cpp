#include "coherence/region.hpp"

#include <ostream>
#include <stdexcept>
#include <utility>

namespace coherence {

RegionStateMachine::RegionStateMachine(SystemConfig config,
        const std::vector<RegionSpec>& specs, Metrics& metrics, Policy policy, std::ostream* log)
    : config_(config), metrics_(metrics), policy_(policy), log_(log) {
    config_.validate();
    for (const auto& spec : specs) {
        Region region;
        region.spec = spec;
        region.lines.resize(config_.lines_per_region());
        for (auto& line : region.lines) line.copies.resize(config_.accelerators + 1);
        regions_.push_back(std::move(region));
    }
}

void RegionStateMachine::flush(Line& line, bool during_transition) {
    for (auto& copy : line.copies) {
        if (!copy.valid || !copy.dirty) continue;
        line.memory_version = copy.version;
        copy.dirty = false;
        ++metrics_.writebacks;
        if (during_transition) {
            ++metrics_.transition_flushes;
            metrics_.transition_cycles += config_.costs.writeback;
        } else {
            metrics_.data_cycles += config_.costs.writeback;
        }
    }
}

void RegionStateMachine::apply_hint(RegionId id, Hint hint, std::uint64_t sequence) {
    auto& region = regions_.at(id);
    if (region.spec.kind == MemoryKind::Metadata && hint.mode != Mode::Coherent)
        throw std::logic_error("metadata must remain coherent");
    if (hint.mode == Mode::Private &&
        (hint.owner < 0 || static_cast<std::size_t>(hint.owner) > config_.accelerators))
        throw std::invalid_argument("private hint requires a valid owner");
    if (region.mode == hint.mode &&
        (hint.mode != Mode::Private || region.owner == hint.owner)) return;

    // Direct ownership transfers and S->P go through a permission-revocation barrier.
    if (hint.mode == Mode::Private && region.mode != Mode::Coherent)
        transition(id, Mode::Coherent, no_owner, "reacquire", sequence);
    transition(id, hint.mode, hint.owner, hint.reason, sequence);
    if (config_.check_invariants) check_region(id);
}

void RegionStateMachine::transition(RegionId id, Mode target, NodeId owner,
                                  const char* reason, std::uint64_t sequence) {
    auto& region = regions_.at(id);
    const Mode from = region.mode;
    if (from == target) return;
    const auto before_cycles = metrics_.transition_cycles;
    const auto before_flushes = metrics_.transition_flushes;
    std::vector<bool> participants(config_.accelerators + 1, false);
    std::vector<bool> revoked(config_.accelerators + 1, false);
    if (region.owner != no_owner) participants.at(static_cast<std::size_t>(region.owner)) = true;
    if (from == Mode::SharedReadOnly) {
        // RO reads never register directory sharers. Revoke by broadcasting to
        // every endpoint, rather than assuming a free, perfectly known reader set.
        for (std::size_t n = 0; n < participants.size(); ++n) {
            participants[n] = true;
            revoked[n] = true;
        }
    }

    // All transitions are atomic region barriers: stop accesses, collect acks,
    // flush as needed, revoke old permissions, and then publish the new mode.
    // C->P can retain the chosen owner's dirty lines. All other paths flush.
    for (auto& line : region.lines) {
        for (std::size_t n = 0; n < line.copies.size(); ++n)
            if (line.copies[n].valid) participants[n] = true;

        bool preserve_dirty = false;
        if (target == Mode::Private)
            preserve_dirty = line.copies.at(static_cast<std::size_t>(owner)).dirty;
        if (!preserve_dirty) flush(line, true);

        for (std::size_t n = 0; n < line.copies.size(); ++n) {
            auto& copy = line.copies[n];
            const bool invalidate = target == Mode::Coherent ||
                (target == Mode::Private && static_cast<NodeId>(n) != owner);
            if (copy.valid && invalidate) {
                revoked[n] = true;
                copy = CacheCopy{};
            }
        }
        line.exclusive_owner = no_owner;
        if (target == Mode::Private && line.copies.at(static_cast<std::size_t>(owner)).valid)
            line.exclusive_owner = owner;
    }
    // One broadcast commit plus one fence/ack exchange per participant.
    // An exchange is one logical control event, not a packet/byte count.
    ++metrics_.region_controls;
    metrics_.transition_cycles += config_.costs.region_control;
    std::uint64_t revocations = 0;
    for (std::size_t n = 0; n < participants.size(); ++n) {
        if (participants[n]) {
            ++metrics_.region_controls;
            metrics_.transition_cycles += config_.costs.region_control;
        }
        if (revoked[n]) { ++metrics_.region_revocations; ++revocations; }
    }
    region.mode = target;
    region.owner = target == Mode::Private ? owner : no_owner;
    ++metrics_.transitions;
    ++metrics_.transition_matrix[index(from)][index(target)];
    if (log_) {
        *log_ << name(policy_) << ',' << sequence << ',' << id << ',' << name(from)
              << ',' << name(target) << ',' << region.owner << ',' << reason << ','
              << metrics_.transition_flushes - before_flushes << ',' << revocations
              << ',' << metrics_.transition_cycles - before_cycles << '\n';
    }
}

void RegionStateMachine::local_hit() {
    ++metrics_.cache_hits;
    metrics_.data_cycles += config_.costs.local_hit;
}

bool RegionStateMachine::evict(NodeId node, RegionId id, std::size_t offset) {
    auto& region = regions_.at(id);
    if (region.mode != Mode::Coherent) throw std::logic_error("capacity profiler requires coherent mode");
    auto& line = region.lines.at(offset);
    auto& copy = line.copies.at(static_cast<std::size_t>(node));
    if (!copy.valid) return false;
    const bool dirty = copy.dirty;
    if (dirty) flush(line, false);
    copy = CacheCopy{};
    if (line.exclusive_owner == node) line.exclusive_owner = no_owner;
    if (config_.check_invariants) check_region(id);
    return dirty;
}

void RegionStateMachine::writeback_region(RegionId id) {
    auto& region = regions_.at(id);
    if (region.mode != Mode::Coherent) throw std::logic_error("profiling fence requires coherent mode");
    for (auto& line : region.lines) flush(line, false);
    if (config_.check_invariants) check_region(id);
}

void RegionStateMachine::fill(Line& line, NodeId node) {
    auto& copy = line.copies.at(static_cast<std::size_t>(node));
    copy = {true, false, line.memory_version};
    ++metrics_.data_fills;
    metrics_.data_cycles += config_.costs.data_fill;
}

std::uint64_t RegionStateMachine::coherent_access(Region& region, const Request& request) {
    auto& line = region.lines.at(request.line);
    auto& local = line.copies.at(static_cast<std::size_t>(request.node));
    if (request.operation == Operation::Read && local.valid) {
        local_hit();
        return local.version;
    }
    if (request.operation == Operation::Write && local.valid &&
        line.exclusive_owner == request.node) {
        local_hit();
    } else {
        ++metrics_.directory_lookups;
        metrics_.coherence_cycles += config_.costs.directory;
        if (request.operation == Operation::Read) {
            bool any_copy = false;
            for (const auto& copy : line.copies) any_copy = any_copy || copy.valid;
            if (line.exclusive_owner != no_owner) {
                // Downgrade the previous E/M owner, including clean E permission.
                ++metrics_.snoops;
                metrics_.coherence_cycles += config_.costs.snoop;
                flush(line, false);
            }
            fill(line, request.node);
            line.exclusive_owner = any_copy ? no_owner : request.node;
            return local.version;
        }
        // A write upgrade/miss invalidates other copies. A dirty invalidation
        // response carries data; it does not also count as a separate snoop.
        flush(line, false);
        for (std::size_t n = 0; n < line.copies.size(); ++n) {
            if (static_cast<NodeId>(n) == request.node || !line.copies[n].valid) continue;
            ++metrics_.invalidations;
            metrics_.coherence_cycles += config_.costs.invalidate;
            line.copies[n] = CacheCopy{};
        }
        if (!local.valid) fill(line, request.node);
        else local_hit();
        line.exclusive_owner = request.node;
    }
    local.version = ++line.latest_version;
    local.dirty = true;
    return local.version;
}

std::uint64_t RegionStateMachine::bypass_access(Region& region, const Request& request) {
    auto& line = region.lines.at(request.line);
    auto& local = line.copies.at(static_cast<std::size_t>(request.node));
    if (local.valid) local_hit();
    else fill(line, request.node);
    if (request.operation == Operation::Write) {
        local.version = ++line.latest_version;
        local.dirty = true;
        line.exclusive_owner = request.node;
    }
    return local.version;
}

AccessResult RegionStateMachine::access(const Request& request) {
    if (request.node < 0 || static_cast<std::size_t>(request.node) > config_.accelerators)
        throw std::invalid_argument("request has invalid node");
    auto& region = regions_.at(request.region);
    if (request.line >= region.lines.size()) throw std::out_of_range("invalid line");
    bool recovered = false;
    if (region.mode == Mode::Private && request.node != region.owner) {
        recovered = true;
        ++metrics_.private_violations;
    }
    if (region.mode == Mode::SharedReadOnly && request.operation == Operation::Write) {
        recovered = true;
        ++metrics_.readonly_violations;
    }
    if (recovered) {
        ++metrics_.recoveries;
        metrics_.recovery_cycles += config_.costs.recovery;
        const auto before = metrics_.transition_cycles;
        transition(request.region, Mode::Coherent, no_owner, "violation_recovery", request.sequence);
        metrics_.misprediction_penalty_cycles += config_.costs.recovery +
            metrics_.transition_cycles - before;
    }
    ++metrics_.requests;
    if (request.operation == Operation::Read) ++metrics_.reads;
    else ++metrics_.writes;
    ++metrics_.mode_accesses[index(region.mode)];
    const auto version = region.mode == Mode::Coherent
        ? coherent_access(region, request) : bypass_access(region, request);
    if (config_.check_invariants) check_region(request.region);
    return {version, recovered};
}

void RegionStateMachine::check_region(RegionId id) const {
    const auto& region = regions_.at(id);
    if (region.spec.kind == MemoryKind::Metadata && region.mode != Mode::Coherent)
        throw std::logic_error("metadata escaped coherent mode");
    if ((region.mode == Mode::Private) != (region.owner != no_owner))
        throw std::logic_error("invalid region ownership");
    for (const auto& line : region.lines) {
        if (region.mode == Mode::SharedReadOnly && line.exclusive_owner != no_owner)
            throw std::logic_error("shared RO line retains exclusive permission");
        if (region.mode == Mode::Private && line.exclusive_owner != no_owner &&
            line.exclusive_owner != region.owner)
            throw std::logic_error("private line permission belongs to another node");
        std::size_t valid = 0, dirty = 0;
        for (std::size_t n = 0; n < line.copies.size(); ++n) {
            const auto& copy = line.copies[n];
            if (!copy.valid) {
                if (copy.dirty) throw std::logic_error("invalid dirty copy");
                continue;
            }
            ++valid;
            if (copy.version != line.latest_version) throw std::logic_error("stale cache copy");
            if (region.mode == Mode::Private && static_cast<NodeId>(n) != region.owner)
                throw std::logic_error("private region has remote cached data");
            if (copy.dirty) {
                ++dirty;
                if (region.mode == Mode::SharedReadOnly)
                    throw std::logic_error("shared RO region has dirty data");
                if (line.exclusive_owner != static_cast<NodeId>(n))
                    throw std::logic_error("dirty copy lacks exclusive permission");
            }
        }
        if (dirty > 1 || (line.exclusive_owner != no_owner &&
            (valid != 1 || !line.copies.at(static_cast<std::size_t>(line.exclusive_owner)).valid)))
            throw std::logic_error("exclusive ownership invariant failed");
        if (line.memory_version > line.latest_version ||
            (dirty == 0 && line.memory_version != line.latest_version))
            throw std::logic_error("dirty data was lost");
    }
}

} // namespace coherence
