#include "model.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace perf {
void Config::validate() const {
    if (warmup_rounds >= rounds || directory_banks == 0 || directory_mshrs == 0)
        throw std::invalid_argument("warmup must be smaller than rounds; directory banks/MSHRs must be positive");
    if (agents == 0 || agents > 63 || lead_steps == 0 || rounds == 0 || outstanding == 0 ||
        prefix_lines == 0 || leaf_lines == 0 || stores_per_line == 0 ||
        table_lines < 2 || sparse_reads == 0 || scratch_lines == 0)
        throw std::invalid_argument("agents must be 1..63; steps, windows and working sets must be positive");
    if (cache_bytes % 64 != 0) throw std::invalid_argument("cache bytes must be a multiple of 64");
    if (prefix_lines > (1U << 24) || leaf_lines > (1U << 24) ||
        table_lines > (1U << 24) || scratch_lines > (1U << 24))
        throw std::invalid_argument("working set exceeds supported address range");
    for (const auto value : {compute_ns, host_compute_ns, hit_ns, memory_ns, peer_ns, directory_ns,
            snoop_ns, invalidate_ns, writeback_ns, directory_bytes, snoop_bytes, invalidate_bytes,
            directory_issue_ns, directory_miss_ns})
        if (!std::isfinite(value) || value < 0) throw std::invalid_argument("costs must be finite and nonnegative");
    for (const auto value : {overlap_trigger, hot_read_fraction, bottom_fraction})
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
            result.accesses.push_back({AccessKind::PrefixKV, ((round * c.prefix_reads + r) % c.prefix_lines) * 64, false});
    if (node != 0)
        for (std::size_t w = 0; w < c.leaf_writes; ++w) {
            const auto line = (round * c.leaf_writes + w) / c.stores_per_line;
            const auto address = lead ? (line % c.prefix_lines) * 64 :
                (1ULL << 40) + node * (1ULL << 32) + (line % c.leaf_lines) * 64;
            result.accesses.push_back({lead ? AccessKind::PrefixKV : AccessKind::LeafKV, address, true});
        }
    for (std::size_t m = 0; m < c.metadata_ops; ++m)
        result.accesses.push_back({AccessKind::Metadata, (1ULL << 60) + (m % 8) * 64, m % 2 == 0});
    return result;
}
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
        Phase warmup{phase.label+"_warmup",{},false};
        Phase measured{phase.label+"_measured",{},true};
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
    Phase lead{"lead", {{1, {}}}, true};
    for (std::size_t r = 0; r < c.lead_steps; ++r) lead.streams.front().steps.push_back(make_step(c, r, 1, true));
    w.push_back(std::move(lead));
    Phase children{"parallel_children", {}, true};
    for (std::size_t node = 0; node <= c.agents; ++node) {
        Stream stream{node, {}};
        for (std::size_t r = 0; r < c.rounds; ++r) stream.steps.push_back(make_step(c, r, node, false));
        children.streams.push_back(std::move(stream));
    }
    w.push_back(std::move(children));
    Phase complete{"completion", {{0, {}}}, true};
    for (std::size_t n = 0; n <= c.agents; ++n) complete.streams.front().steps.push_back(make_step(c, 0, 0, false));
    w.push_back(std::move(complete));
    return split_warmup(std::move(w),c);
}

const char* name(WorkloadMode mode) { return mode==WorkloadMode::DLRM ? "dlrm" : "radix"; }
const char* name(AccessKind kind) {
    switch(kind) {
    case AccessKind::Metadata:return "metadata";case AccessKind::PrefixKV:return "prefix_kv";
    case AccessKind::LeafKV:return "leaf_kv";case AccessKind::HotEmbeddings:return "hot_embeddings";
    case AccessKind::ColdEmbeddings:return "cold_embeddings";case AccessKind::PrivateScratchpad:return "private_scratchpad";
    }
    throw std::logic_error("invalid label");
}
std::uint64_t workload_fingerprint(const Workload& w) {
    std::uint64_t hash=14695981039346656037ULL;
    const auto add=[&](std::uint64_t v){for(unsigned i=0;i<8;++i){hash^=(v>>(8*i))&255U;hash*=1099511628211ULL;}};
    for(const auto& phase:w) for(const auto& stream:phase.streams) {
        add(stream.node);add(stream.steps.size());
        for(const auto& step:stream.steps) {
            add(step.accesses.size());
            for(const auto& a:step.accesses){add(a.address);add(a.write);}
        }
    }
    return hash; // Diagnostic hot/cold labels deliberately excluded.
}
} // namespace perf
