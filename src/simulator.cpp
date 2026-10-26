#include "coherence/simulator.hpp"
#include "coherence/region.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace coherence {

Result simulate(const Workload& workload, const SystemConfig& system,
                const PredictorConfig& predictor_config, Policy policy, std::ostream* log) {
    Result result{policy, {}};
    auto& metrics = result.metrics;
    RegionStateMachine machine(system, workload.regions, metrics, policy, log);
    SharingPredictor predictor(predictor_config, workload.regions.size());
    for (const auto& request : workload.requests) {
        if (policy == Policy::Oracle)
            machine.apply_hint(request.region, {request.oracle_mode, request.oracle_owner,
                                               "oracle_lifecycle"}, request.sequence);
        const auto access = machine.access(request);
        if (access.version != request.expected_version) {
            std::ostringstream message;
            message << name(policy) << ": data mismatch at request " << request.sequence
                    << " region " << request.region << " line " << request.line
                    << ": got " << access.version << ", expected " << request.expected_version;
            throw std::runtime_error(message.str());
        }
        if (request.operation == Operation::Read) {
            for (const auto value : {static_cast<std::uint64_t>(request.region),
                    static_cast<std::uint64_t>(request.line), static_cast<std::uint64_t>(request.node),
                    access.version}) {
                metrics.read_checksum ^= value;
                metrics.read_checksum *= 1099511628211ULL;
            }
        }
        if (policy == Policy::Dynamic) {
            const auto& region = machine.region(request.region);
            const auto hint = predictor.observe(request.region, request.node, request.operation,
                                               region.spec.kind, region.mode, access.recovered);
            if (hint) machine.apply_hint(request.region, *hint, request.sequence);
        }
    }
    if (policy == Policy::Oracle && metrics.recoveries != 0)
        throw std::logic_error("oracle annotations violated permissions");
    return result;
}

void print_comparison(std::ostream& out, const std::vector<Result>& results) {
    if (results.empty()) return;
    out << std::left << std::setw(32) << "Metric";
    for (const auto& result : results) out << std::right << std::setw(24) << name(result.policy);
    out << '\n' << std::string(32 + 24 * results.size(), '-') << '\n';
    const auto row = [&](const char* label, auto getter) {
        out << std::left << std::setw(32) << label;
        for (const auto& result : results) out << std::right << std::setw(24) << getter(result.metrics);
        out << '\n';
    };
    row("Requests", [](const Metrics& m) { return m.requests; });
    row("Directory lookups", [](const Metrics& m) { return m.directory_lookups; });
    row("Downgrade snoops", [](const Metrics& m) { return m.snoops; });
    row("Line invalidations", [](const Metrics& m) { return m.invalidations; });
    row("Region control events", [](const Metrics& m) { return m.region_controls; });
    row("Coherence events (total)", [](const Metrics& m) { return m.coherence_events(); });
    row("Region endpoint revocations", [](const Metrics& m) { return m.region_revocations; });
    row("Data fills (64B lines)", [](const Metrics& m) { return m.data_fills; });
    row("Writebacks (64B lines)", [](const Metrics& m) { return m.writebacks; });
    row("Transition dirty flushes", [](const Metrics& m) { return m.transition_flushes; });
    row("Cache hits", [](const Metrics& m) { return m.cache_hits; });
    row("State transitions", [](const Metrics& m) { return m.transitions; });
    row("Permission recoveries", [](const Metrics& m) { return m.recoveries; });
    row("  remote private accesses", [](const Metrics& m) { return m.private_violations; });
    row("  writes to shared RO", [](const Metrics& m) { return m.readonly_violations; });
    row("Misprediction penalty cycles*", [](const Metrics& m) { return m.misprediction_penalty_cycles; });
    row("Data service cycles", [](const Metrics& m) { return m.data_cycles; });
    row("Line coherence stall cycles", [](const Metrics& m) { return m.coherence_cycles; });
    row("Transition stall cycles", [](const Metrics& m) { return m.transition_cycles; });
    row("Recovery detection cycles", [](const Metrics& m) { return m.recovery_cycles; });
    row("Estimated service/stall cycles", [](const Metrics& m) { return m.total_cycles(); });
    row("Accesses served coherent", [](const Metrics& m) { return m.mode_accesses[0]; });
    row("Accesses served private", [](const Metrics& m) { return m.mode_accesses[1]; });
    row("Accesses served shared RO", [](const Metrics& m) { return m.mode_accesses[2]; });
    row("Read-version checksum", [](const Metrics& m) { return m.read_checksum; });
    const auto baseline = results.front().metrics.coherence_events();
    out << "\nCoherence event reduction vs. " << name(results.front().policy) << ":\n";
    for (const auto& result : results) {
        const double reduction = baseline == 0 ? 0.0 : 100.0 *
            (1.0 - static_cast<double>(result.metrics.coherence_events()) / static_cast<double>(baseline));
        out << "  " << name(result.policy) << ": " << std::fixed << std::setprecision(2)
            << reduction << "%\n";
    }
    out << "\nTransition matrix (from -> to):\n";
    for (const auto& result : results) {
        out << "  " << name(result.policy) << ':';
        for (const auto from : {Mode::Coherent, Mode::Private, Mode::SharedReadOnly})
            for (const auto to : {Mode::Coherent, Mode::Private, Mode::SharedReadOnly})
                if (result.metrics.transition_matrix[index(from)][index(to)] != 0)
                    out << ' ' << name(from) << "->" << name(to) << '='
                        << result.metrics.transition_matrix[index(from)][index(to)];
        out << '\n';
    }
    out << "\n* Penalty is a subset of transition + recovery cycles; do not add it twice.\n"
        << "Counts are logical events, not CXL packets. Data traffic is reported separately.\n"
        << "Cycles sum serialized service costs, not elapsed multicore execution time.\n"
        << "Oracle has perfect lifecycle knowledge, pays barriers/flushes, and is not a cost-optimal bound.\n";
}

void write_csv(std::ostream& out, const std::vector<Result>& results) {
    out << "policy,requests,reads,writes,directory_lookups,snoops,invalidations,region_controls,"
           "coherence_events,region_revocations,data_fills,writebacks,transition_flushes,cache_hits,"
           "transitions,recoveries,private_violations,readonly_violations,misprediction_penalty_cycles,"
           "data_cycles,coherence_cycles,transition_cycles,recovery_cycles,total_cycles,"
           "coherent_accesses,private_accesses,shared_ro_accesses,read_checksum";
    for (const auto from : {Mode::Coherent, Mode::Private, Mode::SharedReadOnly})
        for (const auto to : {Mode::Coherent, Mode::Private, Mode::SharedReadOnly})
            out << ',' << name(from) << "_to_" << name(to);
    out << '\n';
    for (const auto& result : results) {
        const auto& m = result.metrics;
        out << name(result.policy) << ',' << m.requests << ',' << m.reads << ',' << m.writes << ','
            << m.directory_lookups << ',' << m.snoops << ',' << m.invalidations << ','
            << m.region_controls << ',' << m.coherence_events() << ',' << m.region_revocations << ','
            << m.data_fills << ',' << m.writebacks << ',' << m.transition_flushes << ','
            << m.cache_hits << ',' << m.transitions << ',' << m.recoveries << ','
            << m.private_violations << ',' << m.readonly_violations << ','
            << m.misprediction_penalty_cycles << ',' << m.data_cycles << ',' << m.coherence_cycles
            << ',' << m.transition_cycles << ',' << m.recovery_cycles << ',' << m.total_cycles()
            << ',' << m.mode_accesses[0] << ',' << m.mode_accesses[1] << ',' << m.mode_accesses[2]
            << ',' << m.read_checksum;
        for (const auto& row : m.transition_matrix)
            for (const auto value : row) out << ',' << value;
        out << '\n';
    }
}

void write_workload_csv(std::ostream& out, const Workload& workload) {
    out << "sequence,node,region,line,operation,kind,phase,expected_version,oracle_mode,oracle_owner\n";
    for (const auto& request : workload.requests)
        out << request.sequence << ',' << request.node << ',' << request.region << ','
            << request.line << ',' << (request.operation == Operation::Read ? "R" : "W") << ','
            << (workload.regions.at(request.region).kind == MemoryKind::Metadata ? "metadata" : "kv")
            << ',' << request.phase << ',' << request.expected_version << ','
            << name(request.oracle_mode) << ',' << request.oracle_owner << '\n';
}

} // namespace coherence
