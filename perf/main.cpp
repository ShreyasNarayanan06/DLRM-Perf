#include "model.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

using namespace perf;
namespace {
double real(const std::string& text) {
    std::size_t end = 0;
    const auto n = std::stod(text, &end);
    if (end != text.size() || !std::isfinite(n) || n < 0) throw std::invalid_argument("invalid number: " + text);
    return n;
}
std::size_t integer(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("invalid integer: " + text);
    const auto n = std::stoull(text);
    if (n > std::numeric_limits<std::size_t>::max()) throw std::out_of_range("integer too large");
    return static_cast<std::size_t>(n);
}
double ratio(double a, double b) { return b == 0 ? 0 : a / b; }
std::ofstream file(const std::string& path) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot open " + path);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << std::fixed << std::setprecision(9);
    return out;
}
using Fields = std::vector<std::pair<std::string, double>>;
Fields inputs(const Config& c) {
    Fields fields;
#define INPUT(key) fields.emplace_back(#key, static_cast<double>(c.key))
    INPUT(agents); INPUT(lead_steps); INPUT(rounds); INPUT(prefix_reads); INPUT(leaf_writes); INPUT(metadata_ops);
    INPUT(prefix_lines); INPUT(leaf_lines); INPUT(stores_per_line); INPUT(cache_bytes); INPUT(outstanding);
    INPUT(compute_ns); INPUT(host_compute_ns); INPUT(hit_ns); INPUT(memory_ns); INPUT(peer_ns); INPUT(bandwidth_gbps);
    INPUT(directory_ns); INPUT(snoop_ns); INPUT(invalidate_ns); INPUT(writeback_ns);
    INPUT(directory_bytes); INPUT(snoop_bytes); INPUT(invalidate_bytes); INPUT(overlap_trigger);
    INPUT(fork_barrier_ns); INPUT(region_barrier_ns); INPUT(setup_ns); INPUT(region_bytes); INPUT(flush_at_fork);
    INPUT(recovery_rate); INPUT(recovery_ns); INPUT(recovery_bytes); INPUT(added_latency_ns);
    INPUT(table_lines); INPUT(sparse_reads); INPUT(scratch_lines); INPUT(scratch_reads); INPUT(scratch_writes);
    INPUT(hot_fraction); INPUT(hot_read_fraction); INPUT(zipf_alpha); INPUT(bottom_fraction);
    INPUT(directory_model); INPUT(directory_entries); INPUT(directory_banks); INPUT(directory_mshrs);
    INPUT(directory_issue_ns); INPUT(directory_miss_ns); INPUT(directory_backing_gbps); INPUT(warmup_rounds);
    INPUT(hot_update_every); INPUT(hot_updates); INPUT(cold_update_every); INPUT(cold_updates);
#undef INPUT
    return fields;
}
Traffic total(const Result& r) { Traffic t; for (const auto& kind : r.kinds) t += kind; return t; }
Fields traffic_fields(const Traffic& t) {
    Fields fields;
#define FIELD(label, key) fields.emplace_back(label, static_cast<double>(t.key))
    FIELD("logical_accesses", accesses); FIELD("reads", reads); FIELD("writes", writes); FIELD("cache_hits", hits);
    FIELD("fills", fills); FIELD("directory_lookups", directory); FIELD("snoops", snoops); FIELD("invalidations", invalidations);
    FIELD("writebacks", writebacks); FIELD("evictions", evictions); FIELD("dirty_evictions", dirty_evictions);
    FIELD("region_grants", region_grants); FIELD("region_barriers", region_barriers); FIELD("region_recoveries", region_recoveries);
    FIELD("region_downgrades", region_downgrades);
    FIELD("revoked_copies", revoked_copies); FIELD("collateral_copies", collateral_copies); FIELD("region_flushes", region_flushes);
#undef FIELD
    return fields;
}
DirectoryStats directory_total(const std::array<DirectoryStats,kind_count>& stats) {
    DirectoryStats result; for (const auto& s:stats) result+=s; return result;
}
Fields directory_fields(const DirectoryStats& s, const std::string& prefix="directory_") {
    Fields f;
#define DFIELD(key) f.emplace_back(prefix+#key,static_cast<double>(s.key))
    DFIELD(requests); DFIELD(hits); DFIELD(misses); DFIELD(merged); DFIELD(evictions);
    DFIELD(read_bytes); DFIELD(write_bytes); DFIELD(bank_wait_ns); DFIELD(mshr_wait_ns); DFIELD(capacity_wait_ns);
    DFIELD(bank_busy_ns); DFIELD(backing_busy_ns); DFIELD(backing_wait_ns); DFIELD(service_ns);
    DFIELD(peak_misses); DFIELD(peak_entries);
#undef DFIELD
    return f;
}
double reference_time(const std::vector<Result>& results, WorkloadMode mode, bool measured=false) {
    for (const auto& r : results) if (r.workload == mode && !r.scenario.counterfactual) return measured ? r.measured_execution_ns : r.execution_ns;
    throw std::logic_error("missing baseline");
}
Fields metrics(const Result& r, double baseline, double measured_baseline) {
    auto fields = traffic_fields(total(r));
    const Fields extra{{"latency_retained", r.scenario.latency_retained}, {"traffic_retained", r.scenario.traffic_retained},
        {"execution_us", r.execution_ns / 1000}, {"speedup", ratio(baseline, r.execution_ns)},
        {"execution_reduction_pct", 100 * ratio(baseline - r.execution_ns, baseline)},
        {"offchip_requests", static_cast<double>(r.offchip_requests)},
        {"fill_bytes", r.data_bytes}, {"writeback_bytes", r.writeback_bytes}, {"control_bytes", r.control_bytes},
        {"total_link_bytes", r.data_bytes + r.writeback_bytes + r.control_bytes},
        {"link_utilization_pct", 100 * ratio(r.link_busy_ns, r.execution_ns)},
        {"aggregate_queue_wait_ns", r.queue_wait_ns}, {"aggregate_worker_compute_ns", r.worker_compute_ns},
        {"aggregate_worker_memory_wait_ns", r.worker_memory_wait_ns}, {"aggregate_overlap_ns", r.overlap_ns},
        {"aggregate_event_latency_ns", r.assumed_coherence_latency_ns}, {"aggregate_added_latency_ns", r.added_latency_ns},
        {"charged_directory_events", r.charged_directory}, {"charged_snoop_events", r.charged_snoops},
        {"charged_invalidation_events", r.charged_invalidations}, {"expected_recoveries", r.expected_recoveries},
        {"aggregate_region_penalty_ns", r.region_penalty_ns}, {"host_sync_wait_ns", r.host_sync_wait_ns},
        {"fork_flushes", static_cast<double>(r.fork_flushes)}, {"fork_penalty_ns", r.fork_penalty_ns},
        {"prefix_regions", static_cast<double>(r.prefix_regions)}, {"peak_inflight", static_cast<double>(r.peak_inflight)}};
    fields.insert(fields.end(), extra.begin(), extra.end());
    fields.emplace_back("measured_execution_us",r.measured_execution_ns/1000);
    fields.emplace_back("measured_execution_reduction_pct",100*ratio(measured_baseline-r.measured_execution_ns,measured_baseline));
    Traffic measured; for (const auto& t:r.measured_kinds) measured+=t;
    fields.emplace_back("measured_logical_accesses",static_cast<double>(measured.accesses));
    fields.emplace_back("measured_cache_hits",static_cast<double>(measured.hits));
    fields.emplace_back("measured_evictions",static_cast<double>(measured.evictions));
    for (const auto& f:directory_fields(directory_total(r.directory))) fields.push_back(f);
    for (const auto& f:directory_fields(directory_total(r.measured_directory),"measured_directory_")) fields.push_back(f);
    return fields;
}
void csv_header(std::ostream& out, const Fields& fields) { for (const auto& f : fields) out << ',' << f.first; }
void csv_values(std::ostream& out, const Fields& fields) { for (const auto& f : fields) out << ',' << f.second; }
void usage() {
    std::cout << "Usage: architecture_perf --workload radix|dlrm|both [options]\n"
        << "Four policy-neutral cost scenarios; finite LRU, fixed-order MESI event profile, shared FIFO link.\n"
        << "Common: --agents 3 --rounds 64 --cache-bytes 65536 --outstanding 8 --overlap-trigger 0.5\n"
        << "        --compute-ns 200 --host-compute-ns 50 --hit-ns 2 --memory-ns 150 --peer-ns 150 --bandwidth-gbps 32\n"
        << "DLRM: --table-lines 4096 --hot-fraction 0.05 --hot-read-fraction 0.8 --zipf-alpha 1.1 --seed 42\n"
        << "      --sparse-reads 32 --scratch-lines 128 --scratch-reads 2 --scratch-writes 8 --bottom-fraction 0.4\n"
        << "      --hot-update-every 4 --hot-updates 2 --cold-update-every 32 --cold-updates 1 (every=0 disables)\n"
        << "Radix: --lead-steps 32 --prefix-reads 32 --leaf-writes 8 --metadata-ops 2 --prefix-lines 64 --leaf-lines 128\n"
        << "Common store packing: --stores-per-line 4\n"
        << "Events: --directory-ns 20 --snoop-ns 20 --invalidate-ns 12 --writeback-ns 10\n"
        << "        --directory-bytes 16 --snoop-bytes 16 --invalidate-bytes 16\n"
        << "Hints: --region-bytes 4096 --region-barrier-ns 40 --recovery-ns 200 --recovery-bytes 32 --setup-ns 0\n"
        << "       --fork-barrier-ns 200 --flush-at-fork 1 --recovery-rate 0.01 --added-latency-ns 0\n"
        << "Directory: --directory-model 1 --directory-entries 1024 --directory-banks 4 --directory-mshrs 16\n"
        << "           --directory-issue-ns 2 --directory-miss-ns 80 --directory-backing-gbps 32\n"
        << "Warm-up: --rounds 512 --warmup-rounds 128 (exclude initial batches from measured window)\n"
        << "Output: --csv metrics.csv --kinds access_kinds.csv --phases phases.csv\n"
        << "Cache bytes 0 disables retention. Region size is a power of two, 64B..1MiB.\n"
        << "Overlap trigger 1 disables next-step lookahead; bottom MLP still overlaps independent gathers.\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        Config c;
        bool both = false;
        std::string csv_path, kinds_path, phases_path;
        const std::map<std::string, std::size_t*> ints{
            {"--agents", &c.agents}, {"--rounds", &c.rounds}, {"--lead-steps", &c.lead_steps},
            {"--prefix-reads", &c.prefix_reads}, {"--leaf-writes", &c.leaf_writes}, {"--metadata-ops", &c.metadata_ops},
            {"--prefix-lines", &c.prefix_lines}, {"--leaf-lines", &c.leaf_lines}, {"--stores-per-line", &c.stores_per_line},
            {"--cache-bytes", &c.cache_bytes}, {"--region-bytes", &c.region_bytes}, {"--outstanding", &c.outstanding},
            {"--table-lines", &c.table_lines}, {"--sparse-reads", &c.sparse_reads}, {"--scratch-lines", &c.scratch_lines},
            {"--scratch-reads", &c.scratch_reads}, {"--scratch-writes", &c.scratch_writes},
            {"--hot-update-every", &c.hot_update_every}, {"--hot-updates", &c.hot_updates},
            {"--cold-update-every", &c.cold_update_every}, {"--cold-updates", &c.cold_updates},
            {"--directory-entries",&c.directory_entries}, {"--directory-banks",&c.directory_banks},
            {"--directory-mshrs",&c.directory_mshrs}, {"--warmup-rounds",&c.warmup_rounds}};
        const std::map<std::string, double*> reals{
            {"--compute-ns", &c.compute_ns}, {"--host-compute-ns", &c.host_compute_ns}, {"--hit-ns", &c.hit_ns},
            {"--memory-ns", &c.memory_ns}, {"--peer-ns", &c.peer_ns}, {"--bandwidth-gbps", &c.bandwidth_gbps},
            {"--directory-ns", &c.directory_ns}, {"--snoop-ns", &c.snoop_ns}, {"--invalidate-ns", &c.invalidate_ns},
            {"--writeback-ns", &c.writeback_ns}, {"--directory-bytes", &c.directory_bytes}, {"--snoop-bytes", &c.snoop_bytes},
            {"--invalidate-bytes", &c.invalidate_bytes}, {"--overlap-trigger", &c.overlap_trigger},
            {"--fork-barrier-ns", &c.fork_barrier_ns}, {"--region-barrier-ns", &c.region_barrier_ns},
            {"--setup-ns", &c.setup_ns}, {"--recovery-rate", &c.recovery_rate}, {"--recovery-ns", &c.recovery_ns},
            {"--recovery-bytes", &c.recovery_bytes}, {"--added-latency-ns", &c.added_latency_ns},
            {"--hot-fraction", &c.hot_fraction}, {"--hot-read-fraction", &c.hot_read_fraction},
            {"--zipf-alpha", &c.zipf_alpha}, {"--bottom-fraction", &c.bottom_fraction},
            {"--directory-issue-ns",&c.directory_issue_ns}, {"--directory-miss-ns",&c.directory_miss_ns},
            {"--directory-backing-gbps",&c.directory_backing_gbps}};
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") { usage(); return 0; }
            if (i + 1 == argc) throw std::invalid_argument("missing value for " + arg);
            const std::string v = argv[++i];
            if (arg == "--csv") csv_path = v;
            else if (arg == "--kinds") kinds_path = v;
            else if (arg == "--phases") phases_path = v;
            else if (arg == "--workload") {
                if (v != "radix" && v != "dlrm" && v != "both") throw std::invalid_argument("workload must be radix, dlrm or both");
                both = v == "both"; c.workload = v == "dlrm" ? WorkloadMode::DLRM : WorkloadMode::RadixTree;
            } else if (arg == "--seed") c.seed = integer(v);
            else if (arg=="--directory-model") {
                const auto n=integer(v); if (n>1) throw std::invalid_argument("directory-model must be 0 or 1");
                c.directory_model=n!=0;
            }
            else if (arg == "--flush-at-fork") {
                const auto n = integer(v);
                if (n > 1) throw std::invalid_argument("flush-at-fork must be 0 or 1");
                c.flush_at_fork = n != 0;
            } else if (ints.count(arg)) *ints.at(arg) = integer(v);
            else if (reals.count(arg)) *reals.at(arg) = real(v);
            else throw std::invalid_argument("unknown option: " + arg);
        }
        c.validate();
        if ((!csv_path.empty() && (csv_path == kinds_path || csv_path == phases_path)) ||
            (!kinds_path.empty() && kinds_path == phases_path)) throw std::invalid_argument("output paths must be distinct");
        std::vector<Result> results;
        const auto modes = both ? std::vector<WorkloadMode>{WorkloadMode::RadixTree, WorkloadMode::DLRM} : std::vector<WorkloadMode>{c.workload};
        for (const auto mode : modes) {
            auto config = c; config.workload = mode;
            const auto workload = make_workload(config);
            for (const auto& s : default_scenarios(mode)) results.push_back(simulate(workload, config, s));
        }
        std::cout << "Policy-neutral performance model: cache/node=" << c.cache_bytes << " B, link=" << c.bandwidth_gbps
                  << " GB/s, MLP/node=" << c.outstanding << ", region=" << c.region_bytes << " B\n"
                  << "Event counts use fixed logical interleaving; cycle and byte budgets are assumptions.\n";
        for (const auto mode : modes) {
            const auto base = reference_time(results, mode,c.warmup_rounds!=0);
            std::cout << '\n' << name(mode) << '\n' << std::left << std::setw(38) << "Scenario" << std::right
                      << std::setw(12) << (c.warmup_rounds!=0 ? "Measured us" : "Time us") << std::setw(12) << "Change %" << std::setw(12) << "Link KiB"
                      << std::setw(12) << "Recoveries" << '\n';
            for (const auto& r : results) if (r.workload == mode) {
                std::cout << std::left << std::setw(38) << r.scenario.label << std::right << std::fixed << std::setprecision(3)
                          << std::setw(12) << (c.warmup_rounds!=0 ? r.measured_execution_ns : r.execution_ns) / 1000
                          << std::setw(12) << 100 * ratio(base - (c.warmup_rounds!=0 ? r.measured_execution_ns : r.execution_ns), base)
                          << std::setw(12) << (r.data_bytes + r.writeback_bytes + r.control_bytes) / 1024
                          << std::setw(12) << total(r).region_recoveries << '\n';
                if (!r.scenario.counterfactual) {
                    std::cout << "  Baseline trace=" << r.trace_fingerprint << ", accesses=" << r.requests << '\n';
                    for (const auto k : all_kinds) {
                        const auto& t = r.kinds[index(k)]; if (t.accesses == 0) continue;
                        std::cout << "  " << name(k) << ": reads=" << t.reads << " writes=" << t.writes << " hits=" << t.hits
                                  << " directory=" << t.directory << " snoops=" << t.snoops << " invalidations=" << t.invalidations
                                  << " writebacks=" << t.writebacks << '\n';
                    }
                }
            }
        }
        std::cout << "\nChange % is time reduction vs each workload's baseline; negative means slower.\n"
                  << "Bypass/coarse rows are cost hints with explicit page costs, not implemented optimized protocols.\n";
        if (!csv_path.empty()) {
            auto out = file(csv_path);
            out << "workload,scenario,trace_fingerprint,seed";
            csv_header(out, metrics(results.front(), 1,1)); csv_header(out, inputs(c)); out << '\n';
            for (const auto& r : results) {
                out << name(r.workload) << ',' << r.scenario.label << ',' << r.trace_fingerprint << ',' << c.seed;
                csv_values(out, metrics(r, reference_time(results, r.workload),reference_time(results,r.workload,true))); csv_values(out, inputs(c)); out << '\n';
            }
        }
        if (!kinds_path.empty()) {
            auto out = file(kinds_path);
            out << "workload,scenario,scope,kind"; csv_header(out, traffic_fields({})); csv_header(out,directory_fields({})); out << '\n';
            for (const auto& r : results) for (const bool measured:{false,true}) for (const auto k : all_kinds) {
                const auto& t = (measured ? r.measured_kinds : r.kinds)[index(k)]; if (t.accesses == 0) continue;
                const auto& d = (measured ? r.measured_directory : r.directory)[index(k)];
                out << name(r.workload) << ',' << r.scenario.label << ',' << (measured ? "measured" : "all") << ',' << name(k);
                csv_values(out, traffic_fields(t)); csv_values(out,directory_fields(d)); out << '\n';
            }
        }
        if (!phases_path.empty()) {
            auto out = file(phases_path);
            out << "workload,scenario,phase,start_ns,end_ns,duration_ns,measured\n";
            for (const auto& r : results) for (const auto& p : r.phases)
                out << name(r.workload) << ',' << r.scenario.label << ',' << p.label << ',' << p.start_ns << ',' << p.end_ns << ',' << p.end_ns - p.start_ns << ',' << p.measured << '\n';
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
}
