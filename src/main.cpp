#include "coherence/simulator.hpp"

#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace coherence;

namespace {

void usage(std::ostream& out) {
    out << "Usage: radix_coherence [options]\n"
        << "Replays one deterministic trace through baseline, dynamic, and oracle policies.\n\n"
        << "System/workload:\n"
        << "  --accelerators N       Accelerator nodes, excluding host (default 4, max 63)\n"
        << "  --region-bytes N       Region size, multiple of 64 (default 4096)\n"
        << "  --episodes N           Independent lead/fork lifecycles (default 4)\n"
        << "  --subagents N          Children per lead (default 3)\n"
        << "  --prefix-regions N     Regions in each lead's prefix (default 2)\n"
        << "  --leaf-regions N       Regions per child (default 1)\n"
        << "  --lead-writes N        Appends per prefix region (default 96)\n"
        << "  --review-reads N       Lead reads before fork, may be 0 (default 16)\n"
        << "  --rounds N             Round-robin child iterations (default 128)\n"
        << "  --stress               Inject false freezes, remote access, shared writes\n\n"
        << "Predictor (thresholds count requests to that region):\n"
        << "  --private-threshold N  Same-node requests before private hint (default 8)\n"
        << "  --shared-threshold N   Read run with >=2 readers before sharing (default 8)\n"
        << "  --freeze-threshold N   Owner read run before freezing private (default 16)\n"
        << "  --cooldown N           Requests after violation before new hints (default 16)\n\n"
        << "Costs (illustrative cycles, all may be 0):\n"
        << "  --hit-cost N           Local cache access (default 2)\n"
        << "  --fill-cost N          64B data fill (default 80)\n"
        << "  --directory-cost N     Directory lookup (default 24)\n"
        << "  --snoop-cost N         Owner downgrade snoop (default 32)\n"
        << "  --invalidate-cost N    Line invalidation per target (default 24)\n"
        << "  --writeback-cost N     Dirty 64B data writeback (default 80)\n"
        << "  --control-cost N       Logical region control event (default 40)\n"
        << "  --recovery-cost N      Violation detection/replay overhead (default 160)\n\n"
        << "Output:\n"
        << "  --csv PATH             Comparison metrics CSV\n"
        << "  --transitions PATH     State transition CSV for all policies\n"
        << "  --dump-workload PATH   Request trace with oracle/reference annotations\n"
        << "  --help                 Show this text\n";
}

std::uint64_t number(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected a nonnegative integer, got: " + text);
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed);
    if (consumed != text.size() || value > std::numeric_limits<std::size_t>::max())
        throw std::out_of_range("number too large: " + text);
    return value;
}

std::ofstream output_file(const std::string& path) {
    std::ofstream stream(path);
    if (!stream) throw std::runtime_error("cannot open output: " + path);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    return stream;
}

} // namespace

int main(int argc, char** argv) {
    try {
        SystemConfig system;
        WorkloadConfig config;
        PredictorConfig predictor;
        std::string csv_path, transitions_path, workload_path;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") { usage(std::cout); return 0; }
            if (option == "--stress") { config.stress = true; continue; }
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + option);
            const std::string value = argv[++i];
            if (option == "--csv") { csv_path = value; continue; }
            if (option == "--transitions") { transitions_path = value; continue; }
            if (option == "--dump-workload") { workload_path = value; continue; }
            const auto n = number(value);
            if (option == "--accelerators") system.accelerators = n;
            else if (option == "--region-bytes") system.region_bytes = n;
            else if (option == "--episodes") config.episodes = n;
            else if (option == "--subagents") config.subagents = n;
            else if (option == "--prefix-regions") config.prefix_regions = n;
            else if (option == "--leaf-regions") config.leaf_regions = n;
            else if (option == "--lead-writes") config.lead_writes = n;
            else if (option == "--review-reads") config.review_reads = n;
            else if (option == "--rounds") config.rounds = n;
            else if (option == "--private-threshold") predictor.private_threshold = n;
            else if (option == "--shared-threshold") predictor.shared_threshold = n;
            else if (option == "--freeze-threshold") predictor.freeze_threshold = n;
            else if (option == "--cooldown") predictor.cooldown = n;
            else if (option == "--hit-cost") system.costs.local_hit = n;
            else if (option == "--fill-cost") system.costs.data_fill = n;
            else if (option == "--directory-cost") system.costs.directory = n;
            else if (option == "--snoop-cost") system.costs.snoop = n;
            else if (option == "--invalidate-cost") system.costs.invalidate = n;
            else if (option == "--writeback-cost") system.costs.writeback = n;
            else if (option == "--control-cost") system.costs.region_control = n;
            else if (option == "--recovery-cost") system.costs.recovery = n;
            else throw std::invalid_argument("unknown option: " + option);
        }
        system.validate();
        config.validate();
        predictor.validate();
        if ((!csv_path.empty() && (csv_path == transitions_path || csv_path == workload_path)) ||
            (!transitions_path.empty() && transitions_path == workload_path))
            throw std::invalid_argument("output paths must be distinct");
        const auto workload = make_workload(system, config);
        std::ofstream transitions;
        if (!transitions_path.empty()) {
            transitions = output_file(transitions_path);
            transitions << "policy,sequence,region,from,to,owner,reason,dirty_flushes,"
                           "revoked_endpoints,stall_cycles\n";
        }
        std::vector<Result> results;
        for (const auto policy : {Policy::Baseline, Policy::Dynamic, Policy::Oracle})
            results.push_back(simulate(workload, system, predictor, policy,
                                      transitions.is_open() ? &transitions : nullptr));
        for (const auto& result : results)
            if (result.metrics.read_checksum != results.front().metrics.read_checksum)
                throw std::logic_error("policies disagree on read checksum");
        std::cout << "Dynamic Region-Level Coherence for Multi-Agent Radix Prefix Trees\n"
                  << "Host + " << system.accelerators << " accelerators, " << system.region_bytes
                  << "B regions / 64B lines\n" << config.episodes << " episodes, " << config.subagents
                  << " subagents, " << workload.tree.size() << " radix nodes, "
                  << workload.regions.size() << " regions, " << workload.requests.size()
                  << " requests, stress=" << (config.stress ? "on" : "off") << "\n\n";
        print_comparison(std::cout, results);
        if (!csv_path.empty()) { auto out = output_file(csv_path); write_csv(out, results); }
        if (!workload_path.empty()) { auto out = output_file(workload_path); write_workload_csv(out, workload); }
        std::cout << "\nPASS: every access matches the reference trace; all read checksums match.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
