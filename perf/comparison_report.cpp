#include "comparison_report.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace perf {
namespace {
using Fields=std::vector<std::pair<std::string,double>>;
std::ofstream output(const std::string& path) {
    std::ofstream stream(path);if(!stream)throw std::runtime_error("cannot open "+path);
    stream.exceptions(std::ios::badbit|std::ios::failbit);stream<<std::fixed<<std::setprecision(9);return stream;
}
Traffic total(const std::array<Traffic,kind_count>& kinds) {Traffic t;for(const auto& k:kinds)t+=k;return t;}
DirectoryStats total(const std::array<DirectoryStats,kind_count>& kinds) {DirectoryStats t;for(const auto& k:kinds)t+=k;return t;}
Fields management(const ManagementStats& s,const std::string& prefix="") {
    Fields f;
#define FIELD(key) f.emplace_back(prefix+#key,static_cast<double>(s.key))
    FIELD(software_requests);FIELD(hardware_requests);FIELD(software_busy_ns);FIELD(software_queue_ns);
    FIELD(software_control_bytes);FIELD(region_checks);FIELD(table_pressure);FIELD(transitions);
    FIELD(to_hardware);FIELD(to_software);FIELD(flushed_lines);FIELD(transition_control_bytes);
    FIELD(transition_data_bytes);FIELD(transition_service_ns);FIELD(peak_regions);
#undef FIELD
    return f;
}
Fields fields(const Config& c,const ManagedRun& run) {
    const auto& r=run.result;const auto& m=run.config;const auto t=total(r.measured_kinds),a=total(r.kinds);
    const auto directory=total(r.measured_directory);
    Fields f{{"execution_us",r.execution_ns/1000},{"measured_execution_us",r.measured_execution_ns/1000},
        {"accesses",static_cast<double>(a.accesses)},{"measured_accesses",static_cast<double>(t.accesses)},
        {"measured_cache_hits",static_cast<double>(t.hits)},{"measured_data_fills",static_cast<double>(t.fills)},
        {"measured_permission_requests",static_cast<double>(t.directory+t.evictions)},
        {"measured_metadata_misses",static_cast<double>(directory.misses)},
        {"measured_interventions",static_cast<double>(t.snoops)},{"measured_invalidations",static_cast<double>(t.invalidations)},
        {"measured_line_writebacks",static_cast<double>(t.writebacks)},
        {"total_link_bytes",r.data_bytes+r.writeback_bytes+r.control_bytes},
        {"data_bytes",r.data_bytes},{"writeback_bytes",r.writeback_bytes},{"control_bytes",r.control_bytes},
        {"agents",static_cast<double>(c.agents)},{"rounds",static_cast<double>(c.rounds)},
        {"warmup_rounds",static_cast<double>(c.warmup_rounds)},{"cache_bytes",static_cast<double>(c.cache_bytes)},
        {"bandwidth_gbps",c.bandwidth_gbps},{"compute_ns",c.compute_ns},{"memory_ns",c.memory_ns},{"peer_ns",c.peer_ns},
        {"table_lines",static_cast<double>(c.table_lines)},{"hot_update_every",static_cast<double>(c.hot_update_every)},
        {"cold_update_every",static_cast<double>(c.cold_update_every)}};
#define FIELD(key) f.emplace_back(#key,static_cast<double>(m.key))
    FIELD(region_bytes);FIELD(region_entries);FIELD(review_accesses);FIELD(promote_operations);
    FIELD(software_ns);FIELD(software_workers);FIELD(software_message_bytes);FIELD(region_lookup_ns);
    FIELD(transition_ns);FIELD(transition_message_bytes);FIELD(record_bytes);
#undef FIELD
    for(const auto& entry:management(run.all))f.push_back(entry);
    for(const auto& entry:management(run.measured,"measured_"))f.push_back(entry);
    return f;
}
double reference(const std::vector<ManagedRun>& runs) {
    for(const auto& run:runs)if(run.config.mode==ManagementMode::Software)return run.result.measured_execution_ns;
    return 0;
}
}
void write_comparison(const std::string& path,const Config& c,const std::vector<ManagedRun>& runs) {
    if(runs.empty())return;
    auto out=output(path);const auto baseline=reference(runs);
    out<<"mode,workload,trace_fingerprint,seed,layout_seed,software_baseline_us,time_reduction_pct,speedup";
    for(const auto& field:fields(c,runs.front()))out<<','<<field.first;
    out<<'\n';
    for(const auto& run:runs) {
        out<<name(run.config.mode)<<','<<name(c.workload)<<','<<run.result.trace_fingerprint<<','<<c.seed<<','<<c.layout_seed<<',';
        if(baseline>0 && run.result.measured_execution_ns>0)
            out<<baseline/1000<<','<<100*(baseline-run.result.measured_execution_ns)/baseline<<','<<baseline/run.result.measured_execution_ns;
        else out<<",,";
        for(const auto& field:fields(c,run))out<<','<<field.second;
        out<<'\n';
    }
}
void write_region_changes(const std::string& path,const ManagementModel& manager) {
    auto out=output(path);
    out<<"address,from,to,start_ns,end_ns,measured,resident_lines,dirty_lines,control_bytes,data_bytes\n";
    for(const auto& change:manager.changes()) {
        const auto& c=manager.config();out<<change.region*c.region_bytes<<','<<name(change.from)<<','<<name(change.to)<<','
            <<change.start_ns<<','<<change.end_ns<<','<<change.measured<<','<<change.resident_lines<<','<<change.dirty_lines<<','
            <<c.transition_message_bytes+c.record_bytes*static_cast<double>(change.resident_lines)<<','<<64*change.dirty_lines<<'\n';
    }
}
void print_comparison(const std::vector<ManagedRun>& runs) {
    const auto baseline=reference(runs);
    std::cout<<"\nMemory-management comparison (post-warm-up simulated time)\n";
    for(const auto& run:runs) {
        std::cout<<"  "<<name(run.config.mode)<<": "<<std::fixed<<std::setprecision(3)<<run.result.measured_execution_ns/1000<<" us";
        if(baseline>0 && run.result.measured_execution_ns>0)
            std::cout<<"; "<<100*(baseline-run.result.measured_execution_ns)/baseline<<"% less time vs software; "
                <<baseline/run.result.measured_execution_ns<<"x";
        std::cout<<"; changes="<<run.measured.transitions<<"; software operations="<<run.measured.software_requests
            <<"; hardware operations="<<run.measured.hardware_requests<<'\n';
    }
}
} // namespace perf
