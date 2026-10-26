#include "report.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace perf {
namespace {
using Fields=std::vector<std::pair<std::string,double>>;
std::ofstream file(const std::string& path) {
    std::ofstream out(path);if(!out)throw std::runtime_error("cannot open "+path+" (create its parent directory first)");
    out.exceptions(std::ios::badbit|std::ios::failbit);out<<std::fixed<<std::setprecision(9);return out;
}
double pct(std::uint64_t part,std::uint64_t total){return total==0 ? 0 : 100*static_cast<double>(part)/static_cast<double>(total);}
Fields traffic(const Traffic& t) {
    return {{"accesses",static_cast<double>(t.accesses)},{"reads",static_cast<double>(t.reads)},{"writes",static_cast<double>(t.writes)},
        {"cache_hits",static_cast<double>(t.hits)},{"fills",static_cast<double>(t.fills)},{"directory_lookups",static_cast<double>(t.directory)},
        {"snoops",static_cast<double>(t.snoops)},{"invalidations",static_cast<double>(t.invalidations)},
        {"writebacks",static_cast<double>(t.writebacks)},{"evictions",static_cast<double>(t.evictions)},{"dirty_evictions",static_cast<double>(t.dirty_evictions)}};
}
Fields directory(const DirectoryStats& s) {
    Fields f;
#define FIELD(key) f.emplace_back("directory_" #key,static_cast<double>(s.key))
    FIELD(requests);FIELD(hits);FIELD(misses);FIELD(merged);FIELD(evictions);FIELD(read_bytes);FIELD(write_bytes);
    FIELD(bank_wait_ns);FIELD(mshr_wait_ns);FIELD(capacity_wait_ns);FIELD(bank_busy_ns);FIELD(backing_busy_ns);FIELD(backing_wait_ns);FIELD(service_ns);
    FIELD(peak_misses);FIELD(peak_entries);
#undef FIELD
    return f;
}
Fields config_fields(const Config& c) {
    Fields f;
#define FIELD(key) f.emplace_back(#key,static_cast<double>(c.key))
    FIELD(agents);FIELD(rounds);FIELD(warmup_rounds);FIELD(lead_steps);FIELD(prefix_reads);FIELD(leaf_writes);FIELD(metadata_ops);
    FIELD(prefix_lines);FIELD(leaf_lines);FIELD(stores_per_line);FIELD(cache_bytes);FIELD(outstanding);
    FIELD(compute_ns);FIELD(host_compute_ns);FIELD(overlap_trigger);FIELD(hit_ns);FIELD(memory_ns);FIELD(peer_ns);FIELD(bandwidth_gbps);
    FIELD(directory_ns);FIELD(snoop_ns);FIELD(invalidate_ns);FIELD(writeback_ns);FIELD(directory_bytes);FIELD(snoop_bytes);FIELD(invalidate_bytes);
    FIELD(directory_entries);FIELD(directory_banks);FIELD(directory_mshrs);FIELD(directory_issue_ns);FIELD(directory_miss_ns);FIELD(directory_backing_gbps);
    FIELD(table_lines);FIELD(sparse_reads);FIELD(scratch_lines);FIELD(scratch_reads);FIELD(scratch_writes);
    FIELD(hot_fraction);FIELD(hot_read_fraction);FIELD(zipf_alpha);FIELD(bottom_fraction);
    FIELD(hot_update_every);FIELD(hot_updates);FIELD(cold_update_every);FIELD(cold_updates);
#undef FIELD
    return f;
}
void headers(std::ostream& out,const Fields& f){for(const auto& v:f)out<<','<<v.first;}
void values(std::ostream& out,const Fields& f){for(const auto& v:f)out<<','<<v.second;}
Traffic total(const std::array<Traffic,kind_count>& kinds){Traffic t;for(const auto& k:kinds)t+=k;return t;}
DirectoryStats total(const std::array<DirectoryStats,kind_count>& kinds){DirectoryStats t;for(const auto& k:kinds)t+=k;return t;}
std::uint64_t misses(const BlockObservation& b,int origin){return origin==0 ? b.demand_misses : origin==1 ? b.eviction_misses : b.demand_misses+b.eviction_misses;}
}
void write_report(const std::string& prefix,const Config& c,const Result& r,const BlindProfiler& profiler) {
    {
        auto out=file(prefix+".metrics.csv");
        Fields f{{"execution_us",r.execution_ns/1000},{"measured_execution_us",r.measured_execution_ns/1000},
            {"fill_bytes",r.data_bytes},{"writeback_bytes",r.writeback_bytes},{"control_bytes",r.control_bytes},
            {"total_link_bytes",r.data_bytes+r.writeback_bytes+r.control_bytes},{"link_busy_ns",r.link_busy_ns},
            {"aggregate_link_queue_ns",r.queue_wait_ns},{"aggregate_dependency_wait_ns",r.dependency_wait_ns},
            {"aggregate_compute_ns",r.worker_compute_ns},{"aggregate_memory_wait_ns",r.worker_memory_wait_ns},
            {"aggregate_overlap_ns",r.overlap_ns},{"host_sync_wait_ns",r.host_sync_wait_ns},
            {"peak_inflight",static_cast<double>(r.peak_inflight)},{"peak_cache_lines",static_cast<double>(r.peak_cache_lines)}};
        for(const auto& x:traffic(total(r.kinds)))f.push_back(x);
        for(const auto& x:traffic(total(r.measured_kinds)))f.emplace_back("measured_"+x.first,x.second);
        for(const auto& x:directory(total(r.directory)))f.push_back(x);
        for(const auto& x:directory(total(r.measured_directory)))f.emplace_back("measured_"+x.first,x.second);
        for(const auto& x:config_fields(c))f.push_back(x);
        out<<"workload,trace_fingerprint,seed,layout_seed";headers(out,f);out<<'\n';
        out<<name(r.workload)<<','<<r.trace_fingerprint<<','<<c.seed<<','<<c.layout_seed;values(out,f);out<<'\n';
    }
    {
        auto out=file(prefix+".kinds.csv");out<<"scope,kind";headers(out,traffic({}));headers(out,directory({}));out<<'\n';
        for(const bool measured:{false,true})for(const auto kind:all_kinds) {
            const auto& t=(measured ? r.measured_kinds : r.kinds)[index(kind)];if(t.accesses==0 && t.evictions==0)continue;
            out<<(measured ? "measured" : "all")<<','<<name(kind);values(out,traffic(t));
            values(out,directory((measured ? r.measured_directory : r.directory)[index(kind)]));out<<'\n';
        }
    }
    {
        auto out=file(prefix+".phases.csv");out<<"phase,start_ns,end_ns,duration_ns,measured\n";
        for(const auto& p:r.phases)out<<p.label<<','<<p.start_ns<<','<<p.end_ns<<','<<p.end_ns-p.start_ns<<','<<p.measured<<'\n';
    }
    if(c.capture_events) {
        auto out=file(prefix+".events.csv");out<<"id,admission_order,node,address,write,kind,measured,issued_ns,admitted_ns,completed_ns,version,victim_address,demand_metadata_misses,eviction_metadata_misses";
        headers(out,traffic({}));out<<'\n';
        for(const auto& e:r.events) {
            out<<e.id<<','<<e.admission_order<<','<<e.node<<','<<e.address<<','<<e.write<<','<<name(e.kind)<<','<<e.measured<<','<<e.issued_ns<<','<<e.admitted_ns<<','<<e.completed_ns<<','<<e.version<<',';
            if(e.has_victim)out<<e.victim;
            out<<','<<e.demand_misses<<','<<e.eviction_misses;values(out,traffic(e.traffic));out<<'\n';
        }
    }
    auto sharers=file(prefix+".sharers.csv"),reads=file(prefix+".read_runs.csv"),intervals=file(prefix+".write_intervals.csv");
    auto purity=file(prefix+".spatial.csv"),attribution=file(prefix+".miss_attribution.csv"),blocks=file(prefix+".blocks.csv");
    sharers<<"scope,space,block_bytes,node_count,blocks,pct_observed_blocks\n";
    reads<<"scope,space,block_bytes,run_type,consecutive_reads,runs,pct_runs_of_type\n";
    intervals<<"scope,space,block_bytes,run_type,bucket,lower_ns,upper_ns,runs,pct_runs_of_type\n";
    purity<<"scope,space,block_bytes,allocated_blocks,observed_blocks,read_only_blocks,written_lines_only_blocks,mixed_blocks,mixed_pct,observed_lines,written_lines,read_only_lines\n";
    attribution<<"scope,space,block_bytes,write_window,write_class,lookup_origin,blocks_with_misses,metadata_misses,pct_metadata_misses\n";
    blocks<<"scope,space,block_bytes,address,reads,writes,whole_run_writes,node_count,written_lines,read_only_lines,demand_metadata_misses,eviction_metadata_misses\n";
    for(std::size_t s=0;s<2;++s)for(std::size_t space=0;space<profiler.spaces();++space)for(std::size_t scale=0;scale<4;++scale) {
        const auto& w=profiler.window(s);const auto& view=w.views[space][scale];const auto bytes=block_sizes[scale];
        const auto key=std::string(s==0 ? "all" : "measured")+","+(space==0 ? "all" : "embeddings")+","+std::to_string(bytes)+",";
        std::map<std::size_t,std::uint64_t> sharing;
        std::uint64_t observed=0,readonly=0,written=0,mixed=0,lines=0,wlines=0,rlines=0;
        std::vector<std::uint64_t> addresses;for(const auto& entry:view.blocks)addresses.push_back(entry.first);std::sort(addresses.begin(),addresses.end());
        const auto& whole=profiler.window(0).views[space][scale].blocks;
        for(const auto address:addresses) {
            const auto& b=view.blocks.at(address);const auto ro=b.read_lines&~b.written_lines;
            const auto found=whole.find(address);const auto history=found==whole.end() ? 0 : found->second.writes;
            if(b.reads+b.writes!=0) {
                ++observed;++sharing[bit_count(b.nodes)];
                if(b.writes==0)++readonly;else if(ro==0)++written;else ++mixed;
                lines+=bit_count(b.read_lines|b.written_lines);wlines+=bit_count(b.written_lines);rlines+=bit_count(ro);
            }
            blocks<<key<<address*bytes<<','<<b.reads<<','<<b.writes<<','<<history<<','<<bit_count(b.nodes)<<','
                <<bit_count(b.written_lines)<<','<<bit_count(ro)<<','<<b.demand_misses<<','<<b.eviction_misses<<'\n';
        }
        for(std::size_t nodes=1;nodes<=c.agents+1;++nodes)sharers<<key<<nodes<<','<<sharing[nodes]<<','<<pct(sharing[nodes],observed)<<'\n';
        purity<<key<<(space==1 ? (c.table_lines*64+bytes-1)/bytes : 0)<<','<<observed<<','<<readonly<<','<<written<<','<<mixed<<','<<pct(mixed,observed)<<','<<lines<<','<<wlines<<','<<rlines<<'\n';
        const auto run=BlindProfiler::runs(view,w.start_ns,w.end_ns);
        for(const auto& type:run.first) {
            std::uint64_t count=0;for(const auto& bucket:type.second)count+=bucket.second;
            for(const auto& bucket:type.second)reads<<key<<name(type.first)<<','<<bucket.first<<','<<bucket.second<<','<<pct(bucket.second,count)<<'\n';
        }
        for(const auto& type:run.second) {
            std::uint64_t count=0;for(const auto& bucket:type.second)count+=bucket.second;
            for(const auto& bucket:type.second) {
                const bool zero=bucket.first==std::numeric_limits<int>::min();
                intervals<<key<<name(type.first)<<','<<(zero ? "zero" : "log2")<<','<<(zero ? 0 : std::ldexp(1.,bucket.first))<<','
                    <<(zero ? 0 : std::ldexp(1.,bucket.first+1))<<','<<bucket.second<<','<<pct(bucket.second,count)<<'\n';
            }
        }
        for(const bool history:{false,true})for(int origin=0;origin<3;++origin) {
            std::array<std::uint64_t,2> nmiss{},nblocks{};
            for(const auto& entry:view.blocks) {
                const auto n=misses(entry.second,origin);if(n==0)continue;
                const auto found=whole.find(entry.first);
                const bool writes=history ? (found!=whole.end() && found->second.writes!=0) : entry.second.writes!=0;
                nmiss[writes ? 1 : 0]+=n;++nblocks[writes ? 1 : 0];
            }
            for(std::size_t category=0;category<2;++category)
                attribution<<key<<(history ? "whole_run" : "scope")<<','<<(category==0 ? "zero_writes" : "positive_writes")<<','
                    <<(origin==0 ? "demand" : origin==1 ? "eviction" : "all")<<','<<nblocks[category]<<','<<nmiss[category]<<','<<pct(nmiss[category],nmiss[0]+nmiss[1])<<'\n';
        }
    }
}
void print_result(const Config& c,const Result& r,const BlindProfiler& profiler) {
    const auto t=total(r.kinds),m=total(r.measured_kinds);const auto d=total(r.directory);
    std::cout<<"\n"<<name(r.workload)<<" baseline MESI: "<<std::fixed<<std::setprecision(3)<<r.execution_ns/1000<<" us total; "
        <<r.measured_execution_ns/1000<<" us measured\n"
        <<"Requests="<<t.accesses<<" (measured "<<m.accesses<<") hits="<<t.hits<<" fills="<<t.fills<<" directory misses="<<d.misses<<'\n';
    const auto space=c.workload==WorkloadMode::DLRM ? 1U : 0U;
    std::cout<<"Measured address distributions ("<<(space==1 ? "embeddings" : "all memory")<<"): \n";
    for(std::size_t scale=0;scale<4;++scale) {
        const auto& view=profiler.window(1).views[space][scale];
        std::uint64_t count=0,single=0,mixed=0,ro_miss=0,total_miss=0;
        for(const auto& item:view.blocks) {
            const auto& b=item.second;if(b.reads+b.writes!=0) {
                ++count;if(bit_count(b.nodes)==1)++single;
                if(b.written_lines!=0 && (b.read_lines&~b.written_lines)!=0)++mixed;
            }
            const auto miss=b.demand_misses+b.eviction_misses;total_miss+=miss;if(b.writes==0)ro_miss+=miss;
        }
        std::cout<<"  "<<block_sizes[scale]<<" B: blocks="<<count<<" one accessor="<<pct(single,count)<<"% mixed="<<pct(mixed,count)
            <<"% metadata misses on zero-write blocks="<<pct(ro_miss,total_miss)<<"%\n";
    }
}
} // namespace perf
