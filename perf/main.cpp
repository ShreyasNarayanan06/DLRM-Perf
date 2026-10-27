#include "report.hpp"
#include "management.hpp"
#include "comparison_report.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
using namespace perf;
namespace {
std::size_t integer(const std::string& text) {
    if(text.empty() || text.find_first_not_of("0123456789")!=std::string::npos)throw std::invalid_argument("invalid integer: "+text);
    const auto value=std::stoull(text);if(value>std::numeric_limits<std::size_t>::max())throw std::out_of_range("integer too large");
    return static_cast<std::size_t>(value);
}
double real(const std::string& text) {
    std::size_t end=0;const auto value=std::stod(text,&end);
    if(end!=text.size() || !std::isfinite(value) || value<0)throw std::invalid_argument("invalid nonnegative number: "+text);
    return value;
}
void usage() {
    std::cout<<"Usage: architecture_perf --workload dlrm|radix|both --output-prefix PATH [options]\n"
        <<"Address profiles; optional --management software|hardware|dynamic|compare.\n"
        <<"Run: --agents 3 --rounds 64 --warmup-rounds 0 --seed 42 --layout-seed 7 --events 0\n"
        <<"DLRM: --table-lines 4096 --sparse-reads 32 --hot-fraction .05 --hot-read-fraction .8 --zipf-alpha 1.1\n"
        <<"      --scratch-lines 128 --scratch-reads 2 --scratch-writes 8 --bottom-fraction .4\n"
        <<"      --hot-update-every 4 --hot-updates 2 --cold-update-every 32 --cold-updates 1\n"
        <<"Radix: --lead-steps 32 --prefix-reads 32 --leaf-writes 8 --metadata-ops 2 --prefix-lines 64 --leaf-lines 128\n"
        <<"Hardware: --cache-bytes 65536 --outstanding 8 --stores-per-line 4 --overlap-trigger .5\n"
        <<"          --compute-ns 200 --host-compute-ns 50 --hit-ns 2 --memory-ns 150 --peer-ns 150 --bandwidth-gbps 32\n"
        <<"Directory: --directory-entries 1024 --directory-banks 4 --directory-mshrs 16\n"
        <<"           --directory-ns 20 --directory-issue-ns 2 --directory-miss-ns 80 --directory-backing-gbps 32\n"
        <<"Protocol budgets: --snoop-ns 20 --invalidate-ns 12 --writeback-ns 10\n"
        <<"                  --directory-bytes 16 --snoop-bytes 16 --invalidate-bytes 16\n"
        <<"Management: --software-ns 100 --software-workers 4 --software-message-bytes 16\n"
        <<"            --region-bytes 4096 --region-entries 256 --review-accesses 64 --promote-operations 8\n"
        <<"            --region-lookup-ns 2 --transition-ns 200 --transition-message-bytes 32 --record-bytes 16\n"
        <<"Software means runtime-managed cached copies, not hardware coherence; dynamic adapts the manager.\n"
        <<"A positive warmup must be smaller than rounds. Output parent directories must exist.\n"
        <<"Hot/cold labels affect generation/diagnostics only; rows share one shuffled contiguous table.\n";
}
}
int main(int argc,char** argv) {
    try {
        Config c;ManagementConfig management;bool both=false,compare=false,managed=false;std::string prefix;
        const std::map<std::string,std::size_t*> ints{
            {"--agents",&c.agents},{"--rounds",&c.rounds},{"--warmup-rounds",&c.warmup_rounds},{"--lead-steps",&c.lead_steps},
            {"--prefix-reads",&c.prefix_reads},{"--leaf-writes",&c.leaf_writes},{"--metadata-ops",&c.metadata_ops},
            {"--prefix-lines",&c.prefix_lines},{"--leaf-lines",&c.leaf_lines},{"--stores-per-line",&c.stores_per_line},
            {"--cache-bytes",&c.cache_bytes},{"--outstanding",&c.outstanding},{"--directory-entries",&c.directory_entries},
            {"--directory-banks",&c.directory_banks},{"--directory-mshrs",&c.directory_mshrs},{"--table-lines",&c.table_lines},
            {"--sparse-reads",&c.sparse_reads},{"--scratch-lines",&c.scratch_lines},{"--scratch-reads",&c.scratch_reads},
            {"--scratch-writes",&c.scratch_writes},{"--hot-update-every",&c.hot_update_every},{"--hot-updates",&c.hot_updates},
            {"--cold-update-every",&c.cold_update_every},{"--cold-updates",&c.cold_updates},
            {"--region-bytes",&management.region_bytes},{"--region-entries",&management.region_entries},
            {"--review-accesses",&management.review_accesses},{"--promote-operations",&management.promote_operations},
            {"--software-workers",&management.software_workers}};
        const std::map<std::string,double*> reals{
            {"--compute-ns",&c.compute_ns},{"--host-compute-ns",&c.host_compute_ns},{"--overlap-trigger",&c.overlap_trigger},
            {"--hit-ns",&c.hit_ns},{"--memory-ns",&c.memory_ns},{"--peer-ns",&c.peer_ns},{"--bandwidth-gbps",&c.bandwidth_gbps},
            {"--directory-ns",&c.directory_ns},{"--snoop-ns",&c.snoop_ns},{"--invalidate-ns",&c.invalidate_ns},
            {"--writeback-ns",&c.writeback_ns},{"--directory-bytes",&c.directory_bytes},{"--snoop-bytes",&c.snoop_bytes},
            {"--invalidate-bytes",&c.invalidate_bytes},{"--directory-issue-ns",&c.directory_issue_ns},
            {"--directory-miss-ns",&c.directory_miss_ns},{"--directory-backing-gbps",&c.directory_backing_gbps},
            {"--hot-fraction",&c.hot_fraction},{"--hot-read-fraction",&c.hot_read_fraction},{"--zipf-alpha",&c.zipf_alpha},{"--bottom-fraction",&c.bottom_fraction},
            {"--software-ns",&management.software_ns},{"--software-message-bytes",&management.software_message_bytes},
            {"--region-lookup-ns",&management.region_lookup_ns},{"--transition-ns",&management.transition_ns},
            {"--transition-message-bytes",&management.transition_message_bytes},{"--record-bytes",&management.record_bytes}};
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];if(arg=="--help"){usage();return 0;}
            if(i+1==argc)throw std::invalid_argument("missing value for "+arg);
            const std::string v=argv[++i];
            if(arg=="--workload") {
                if(v!="dlrm" && v!="radix" && v!="both")throw std::invalid_argument("invalid workload");
                both=v=="both";c.workload=v=="radix" ? WorkloadMode::RadixTree : WorkloadMode::DLRM;
            } else if(arg=="--management") {
                managed=true;compare=v=="compare";
                if(v=="software")management.mode=ManagementMode::Software;
                else if(v=="hardware")management.mode=ManagementMode::Hardware;
                else if(v=="dynamic")management.mode=ManagementMode::Dynamic;
                else if(!compare)throw std::invalid_argument("invalid management mode");
            } else if(arg=="--output-prefix")prefix=v;
            else if(arg=="--seed")c.seed=integer(v);
            else if(arg=="--layout-seed")c.layout_seed=integer(v);
            else if(arg=="--events") {const auto n=integer(v);if(n>1)throw std::invalid_argument("events must be 0 or 1");c.capture_events=n!=0;}
            else if(ints.count(arg)) *ints.at(arg)=integer(v);
            else if(reals.count(arg)) *reals.at(arg)=real(v);
            else throw std::invalid_argument("unknown option: "+arg);
        }
        c.validate();management.validate();
        const auto modes=both ? std::vector<WorkloadMode>{WorkloadMode::RadixTree,WorkloadMode::DLRM} : std::vector<WorkloadMode>{c.workload};
        for(auto mode:modes) {
            c.workload=mode;const auto workload=make_workload(c);
            if(!managed) {
                BlindProfiler profiler(c);const auto result=simulate(workload,c,profiler);
                print_result(c,result,profiler);if(!prefix.empty())write_report(prefix+"."+name(mode),c,result,profiler);
                continue;
            }
            std::vector<ManagedRun> runs;
            const auto managers=compare ? std::vector<ManagementMode>{ManagementMode::Software,ManagementMode::Hardware,ManagementMode::Dynamic} :
                std::vector<ManagementMode>{management.mode};
            for(const auto manager:managers) {
                management.mode=manager;ManagementModel model(c,management);BlindProfiler profiler(c);
                auto result=simulate(workload,c,profiler,&model);
                if(!prefix.empty()) {
                    const auto path=prefix+"."+name(mode)+"."+name(manager);
                    write_report(path,c,result,profiler);write_region_changes(path+".region_changes.csv",model);
                }
                result.events.clear();result.events.shrink_to_fit();
                runs.push_back({management,std::move(result),model.stats(),model.stats(true)});
            }
            print_comparison(runs);
            if(!prefix.empty())write_comparison(prefix+"."+name(mode)+".comparison.csv",c,runs);
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<"error: "<<e.what()<<'\n';return 1;}
}
