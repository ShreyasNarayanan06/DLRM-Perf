#include "coherence/region.hpp"
#include "coherence/simulator.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace coherence;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Function> void must_throw(Function function) {
    bool threw = false;
    try { function(); } catch (const std::exception&) { threw = true; }
    require(threw, "expected exception");
}

SystemConfig small_system() {
    SystemConfig config;
    config.accelerators = 3;
    config.region_bytes = 256;
    return config;
}

struct Fixture {
    SystemConfig config = small_system();
    Metrics metrics;
    RegionStateMachine machine{config, {{MemoryKind::KV, "kv"}, {MemoryKind::Metadata, "meta"}},
                               metrics, Policy::Dynamic};
    std::uint64_t sequence = 0;

    AccessResult access(NodeId node, std::size_t line, Operation op, RegionId region = 0) {
        Request request;
        request.sequence = sequence++;
        request.node = node;
        request.region = region;
        request.line = line;
        request.operation = op;
        return machine.access(request);
    }
    void mode(Mode mode, NodeId owner = no_owner) {
        machine.apply_hint(0, {mode, owner, "test"}, sequence);
    }
};

void accounting(const Metrics& m) {
    require(m.requests == m.reads + m.writes, "request partition");
    require(m.requests == m.mode_accesses[0] + m.mode_accesses[1] + m.mode_accesses[2], "mode partition");
    require(m.requests == m.cache_hits + m.data_fills, "cache partition");
    require(m.recoveries == m.private_violations + m.readonly_violations, "recovery partition");
    std::uint64_t transitions = 0;
    for (const auto& row : m.transition_matrix) for (const auto value : row) transitions += value;
    require(transitions == m.transitions, "transition partition");
    require(m.transition_flushes <= m.writebacks, "flush subset");
    require(m.misprediction_penalty_cycles <= m.transition_cycles + m.recovery_cycles, "penalty subset");
}

void coherent_cache_behavior() {
    Fixture f;
    require(f.access(1, 0, Operation::Write).version == 1, "first write");
    require(f.access(1, 0, Operation::Write).version == 2, "local write");
    require(f.access(1, 0, Operation::Read).version == 2, "local read");
    require(f.metrics.directory_lookups == 1, "coherent local hits must skip directory");
    require(f.access(2, 0, Operation::Read).version == 2, "remote read must see dirty owner");
    require(f.metrics.snoops == 1 && f.metrics.writebacks == 1, "owner intervention accounting");
    require(f.access(2, 0, Operation::Write).version == 3, "shared writer upgrade");
    require(f.metrics.invalidations == 1, "invalidate old reader");
    require(f.access(1, 0, Operation::Read).version == 3, "invalidated reader refills");
    accounting(f.metrics);
}

void clean_exclusive_downgrade() {
    Fixture f;
    f.access(1, 0, Operation::Read);
    f.access(2, 0, Operation::Read);
    require(f.metrics.snoops == 1 && f.metrics.writebacks == 0, "clean E downgrade");
    f.access(1, 0, Operation::Write);
    require(f.metrics.directory_lookups == 3 && f.metrics.invalidations == 1,
            "sole reader after sharing still needs write upgrade");
}

void zero_steady_state_coherence() {
    Fixture f;
    f.mode(Mode::Private, 1);
    const auto before = f.metrics.coherence_events();
    f.access(1, 0, Operation::Write);
    f.access(1, 1, Operation::Write);
    f.access(1, 0, Operation::Read);
    require(f.metrics.coherence_events() == before, "private generated coherence traffic");
    f.mode(Mode::SharedReadOnly);
    require(f.metrics.transition_flushes == 2, "must flush each dirty line once");
    const auto frozen = f.metrics.coherence_events();
    for (NodeId node = 0; node <= 3; ++node)
        require(f.access(node, 0, Operation::Read).version == 1, "shared RO stale read");
    require(f.metrics.coherence_events() == frozen, "shared RO generated coherence traffic");
    require(f.metrics.data_fills > 0, "zero coherence does not imply zero data traffic");
    accounting(f.metrics);
}

void remote_private_read_recovery() {
    Fixture f;
    f.mode(Mode::Private, 1);
    f.access(1, 0, Operation::Write);
    const auto result = f.access(2, 0, Operation::Read);
    require(result.recovered && result.version == 1, "remote private read recovery");
    require(f.machine.region(0).mode == Mode::Coherent, "must recover coherent");
    require(f.metrics.transition_flushes == 1 && f.metrics.private_violations == 1, "recovery flush");
    require(f.metrics.misprediction_penalty_cycles == f.config.costs.recovery +
        2 * f.config.costs.region_control + f.config.costs.writeback, "exact recovery penalty");
}

void remote_private_write_recovery() {
    Fixture f;
    f.mode(Mode::Private, 1);
    f.access(1, 0, Operation::Write);
    const auto result = f.access(0, 0, Operation::Write);
    require(result.recovered && result.version == 2, "remote private writer");
    require(f.access(1, 0, Operation::Read).version == 2, "old private owner read stale");
}

void readonly_write_recovery() {
    Fixture f;
    f.mode(Mode::Private, 1);
    f.access(1, 0, Operation::Write);
    f.mode(Mode::SharedReadOnly);
    f.access(2, 0, Operation::Read);
    f.access(3, 0, Operation::Read);
    const auto result = f.access(2, 0, Operation::Write);
    require(result.recovered && result.version == 2, "RO writer recovery");
    require(f.metrics.readonly_violations == 1 && f.metrics.region_revocations == 4,
            "all RO endpoints must be revoked");
    require(f.access(3, 0, Operation::Read).version == 2, "RO reader retained stale copy");
}

void readonly_revoke_is_broadcast() {
    Fixture f;
    f.mode(Mode::SharedReadOnly);
    const auto before = f.metrics.region_controls;
    f.mode(Mode::Coherent);
    require(f.metrics.region_controls - before == f.config.accelerators + 2,
            "RO revocation must contact every endpoint even without known readers");
    require(f.metrics.region_revocations == f.config.accelerators + 1,
            "RO permissions are globally revoked");
}

void transition_edges() {
    Fixture f;
    f.access(2, 0, Operation::Write);
    f.mode(Mode::Private, 1);
    require(f.metrics.transition_flushes == 1, "C->P must flush old owner");
    require(f.access(1, 0, Operation::Read).version == 1, "C->P lost old owner's data");
    f.access(1, 0, Operation::Write);
    f.mode(Mode::Private, 3);
    require(f.metrics.transitions == 3, "private transfer must go via coherent");
    require(f.access(3, 0, Operation::Read).version == 2, "private transfer lost data");
    f.mode(Mode::SharedReadOnly);
    f.mode(Mode::Private, 2);
    require(f.access(2, 0, Operation::Read).version == 2, "S->C->P lost data");
    f.mode(Mode::Coherent);
    f.access(2, 1, Operation::Write);
    f.mode(Mode::SharedReadOnly);
    require(f.access(0, 1, Operation::Read).version == 1, "C->S lost dirty data");
    accounting(f.metrics);
}

void invalid_requests_and_metadata() {
    Fixture f;
    must_throw([&] { f.machine.apply_hint(1, {Mode::Private, 1, "bad"}, 0); });
    must_throw([&] { f.mode(Mode::Private, no_owner); });
    must_throw([&] { f.access(4, 0, Operation::Read); });
    must_throw([&] { f.access(1, 4, Operation::Read); });
    require(f.machine.region(1).mode == Mode::Coherent, "metadata changed mode");
}

void predictor_history_and_cooldown() {
    SharingPredictor predictor({3, 4, 2, 2}, 1);
    for (int n = 0; n < 2; ++n)
        require(!predictor.observe(0, 1, Operation::Write, MemoryKind::KV, Mode::Coherent, false),
                "private predicted too early");
    const auto private_hint = predictor.observe(0, 1, Operation::Write, MemoryKind::KV, Mode::Coherent, false);
    require(private_hint && private_hint->mode == Mode::Private && private_hint->owner == 1,
            "private threshold failed");
    predictor.observe(0, 1, Operation::Read, MemoryKind::KV, Mode::Private, false);
    const auto freeze = predictor.observe(0, 1, Operation::Read, MemoryKind::KV, Mode::Private, false);
    require(freeze && freeze->mode == Mode::SharedReadOnly, "owner read freeze failed");
    require(predictor.history(0).longest_private_phase == 2, "phase history not measured");
    predictor.observe(0, 2, Operation::Write, MemoryKind::KV, Mode::Coherent, true);
    for (int n = 0; n < 2; ++n)
        require(!predictor.observe(0, 2, Operation::Write, MemoryKind::KV, Mode::Coherent, false),
                "cooldown ignored");
    require(predictor.observe(0, 2, Operation::Write, MemoryKind::KV, Mode::Coherent, false).has_value(),
            "cooldown never expired");
    require(predictor.history(0).ownership_changes == 1, "ownership changes not tracked");
}

void predictor_shared_read_detection() {
    SharingPredictor predictor({100, 4, 100, 2}, 2);
    for (int n = 0; n < 4; ++n) {
        const auto hint = predictor.observe(0, 1 + n % 2, Operation::Read,
                                            MemoryKind::KV, Mode::Coherent, false);
        require(n == 3 ? hint && hint->mode == Mode::SharedReadOnly : !hint,
                "shared read threshold failed");
    }
    for (int n = 0; n < 120; ++n)
        require(!predictor.observe(1, 1, Operation::Read, MemoryKind::Metadata, Mode::Coherent, false),
                "metadata predictor hint");
}

void lifecycle_integration() {
    SystemConfig system;
    WorkloadConfig config;
    for (const bool stress : {false, true}) {
        config.stress = stress;
        const auto workload = make_workload(system, config);
        require(workload.tree.size() == 1 + config.episodes * (1 + config.subagents), "tree shape");
        for (const auto& node : workload.tree) require(node.reference_count == 0, "reference leak");
        const auto baseline = simulate(workload, system, {}, Policy::Baseline);
        const auto dynamic = simulate(workload, system, {}, Policy::Dynamic);
        const auto oracle = simulate(workload, system, {}, Policy::Oracle);
        require(baseline.metrics.read_checksum == dynamic.metrics.read_checksum &&
                baseline.metrics.read_checksum == oracle.metrics.read_checksum, "data checksum mismatch");
        require(baseline.metrics.transitions == 0 && oracle.metrics.recoveries == 0, "policy semantics");
        require(dynamic.metrics.coherence_events() < baseline.metrics.coherence_events(),
                "default lifecycle should reduce coherence events");
        if (stress) {
            require(dynamic.metrics.private_violations > 0 && dynamic.metrics.readonly_violations > 0,
                    "stress did not exercise both recovery paths");
        } else require(dynamic.metrics.recoveries == 0, "unexpected nominal recovery");
        accounting(baseline.metrics); accounting(dynamic.metrics); accounting(oracle.metrics);
    }
}

void no_future_leakage() {
    SystemConfig system;
    WorkloadConfig config;
    config.episodes = 1;
    auto workload = make_workload(system, config);
    const auto before = simulate(workload, system, {}, Policy::Dynamic);
    for (auto& request : workload.requests) {
        request.oracle_mode = Mode::Private;
        request.oracle_owner = 63;
        request.phase = "poisoned future information";
    }
    const auto after = simulate(workload, system, {}, Policy::Dynamic);
    require(before.metrics.coherence_events() == after.metrics.coherence_events() &&
            before.metrics.transitions == after.metrics.transitions &&
            before.metrics.total_cycles() == after.metrics.total_cycles(), "dynamic used oracle information");
    PredictorConfig disabled{1000000, 1000000, 1000000, 16};
    const auto dynamic = simulate(workload, system, disabled, Policy::Dynamic);
    const auto baseline = simulate(workload, system, disabled, Policy::Baseline);
    require(dynamic.metrics.total_cycles() == baseline.metrics.total_cycles() &&
            dynamic.metrics.coherence_events() == baseline.metrics.coherence_events(), "disabled predictor differs");
    workload.requests.front().expected_version += 1;
    must_throw([&] { simulate(workload, system, {}, Policy::Baseline); });
}

void adversarial_trace() {
    const auto system = small_system();
    Workload workload;
    for (std::size_t r = 0; r < 6; ++r)
        workload.regions.push_back({r == 0 ? MemoryKind::Metadata : MemoryKind::KV, "random"});
    std::vector<std::vector<std::uint64_t>> reference(6, std::vector<std::uint64_t>(4));
    std::uint64_t rng = 17;
    const auto next = [&]() { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL; return rng >> 32; };
    for (std::uint64_t i = 0; i < 10000; ++i) {
        const auto region = static_cast<std::size_t>(next() % 6);
        const auto line = static_cast<std::size_t>(next() % 4);
        const auto node = static_cast<NodeId>(next() % 4);
        const auto op = next() % 4 == 0 ? Operation::Write : Operation::Read;
        auto& version = reference[region][line];
        if (op == Operation::Write) ++version;
        workload.requests.push_back({i, node, region, line, op, version, Mode::Coherent, no_owner, "adversarial"});
    }
    const auto baseline = simulate(workload, system, {}, Policy::Baseline);
    const auto dynamic = simulate(workload, system, {2, 3, 2, 1}, Policy::Dynamic);
    require(dynamic.metrics.recoveries > 0, "adversarial trace did not recover");
    require(baseline.metrics.read_checksum == dynamic.metrics.read_checksum, "adversarial data mismatch");
    accounting(dynamic.metrics);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"coherent cache behavior", coherent_cache_behavior},
        {"clean exclusive downgrade", clean_exclusive_downgrade},
        {"zero steady state coherence", zero_steady_state_coherence},
        {"remote private read recovery", remote_private_read_recovery},
        {"remote private write recovery", remote_private_write_recovery},
        {"readonly write recovery", readonly_write_recovery},
        {"readonly revoke is broadcast", readonly_revoke_is_broadcast},
        {"transition edges", transition_edges},
        {"invalid requests and metadata", invalid_requests_and_metadata},
        {"predictor history and cooldown", predictor_history_and_cooldown},
        {"predictor shared read detection", predictor_shared_read_detection},
        {"lifecycle integration", lifecycle_integration},
        {"no future leakage", no_future_leakage},
        {"adversarial trace", adversarial_trace},
    };
    std::size_t failures = 0;
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
