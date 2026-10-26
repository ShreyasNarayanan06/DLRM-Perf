#include "coherence/workload.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace coherence {

void WorkloadConfig::validate() const {
    if (episodes == 0 || subagents == 0 || prefix_regions == 0 || leaf_regions == 0 ||
        lead_writes == 0 || rounds == 0)
        throw std::invalid_argument("episodes, agents, regions, writes and rounds must be positive");
}

namespace {

class TraceBuilder {
public:
    explicit TraceBuilder(const SystemConfig& system) : system_(system) {}

    RegionId allocate(MemoryKind kind, const std::string& label) {
        const auto id = workload.regions.size();
        workload.regions.push_back({kind, label});
        versions_.emplace_back(system_.lines_per_region(), 0);
        return id;
    }

    void emit(NodeId node, RegionId region, std::size_t line, Operation operation,
              Mode oracle_mode, NodeId oracle_owner, const std::string& phase) {
        line %= system_.lines_per_region();
        auto& version = versions_.at(region).at(line);
        if (operation == Operation::Write) ++version;
        workload.requests.push_back({workload.requests.size(), node, region, line,
            operation, version, oracle_mode, oracle_owner, phase});
    }

    void metadata(NodeId node, std::size_t slot, Operation operation, const std::string& phase) {
        emit(node, 0, slot, operation, Mode::Coherent, no_owner, phase);
    }

    Workload workload;

private:
    const SystemConfig& system_;
    std::vector<std::vector<std::uint64_t>> versions_;
};

} // namespace

Workload make_workload(const SystemConfig& system, const WorkloadConfig& config) {
    system.validate();
    config.validate();
    TraceBuilder builder(system);
    builder.allocate(MemoryKind::Metadata, "radix_pointer_table_and_refcounts");
    builder.workload.tree.push_back({0, 0, 0, {}, {}, 0});

    for (std::size_t episode = 0; episode < config.episodes; ++episode) {
        auto& tree = builder.workload.tree;
        const auto lead = tree.size();
        tree.push_back({lead, 0, 1, {}, {}, 1});
        tree[0].children.push_back(lead);
        ++tree[0].reference_count;
        for (std::size_t p = 0; p < config.prefix_regions; ++p)
            tree[lead].kv_regions.push_back(builder.allocate(MemoryKind::KV,
                "episode" + std::to_string(episode) + "_prefix" + std::to_string(p)));
        const auto prefix = tree[lead].kv_regions;
        builder.metadata(0, 0, Operation::Write, "allocate_lead_pointer");
        builder.metadata(1, lead, Operation::Write, "lead_refcount");

        for (std::size_t step = 0; step < config.lead_writes; ++step) {
            for (const auto region : prefix)
                builder.emit(1, region, step, Operation::Write, Mode::Private, 1, "lead_append");
            if (step % 8 == 0) {
                builder.metadata(1, lead, Operation::Read, "lead_lookup");
                builder.metadata(0, lead, Operation::Write, "host_update");
            }
            // Read-heavy does not necessarily mean immutable. This deliberately
            // fools an aggressive owner-read freeze predictor in --stress mode.
            if (config.stress && step == config.lead_writes / 2) {
                for (std::size_t read = 0; read < config.review_reads; ++read)
                    for (const auto region : prefix)
                        builder.emit(1, region, read, Operation::Read, Mode::Private, 1,
                                     "temporary_read_burst");
            }
        }
        for (std::size_t read = 0; read < config.review_reads; ++read)
            for (const auto region : prefix)
                builder.emit(1, region, read, Operation::Read, Mode::Private, 1, "lead_review");

        std::vector<std::size_t> children;
        for (std::size_t agent = 0; agent < config.subagents; ++agent) {
            const auto child = tree.size();
            const auto node = static_cast<NodeId>(1 + (agent + 1) % system.accelerators);
            tree.push_back({child, lead, node, {}, {}, 1});
            tree[lead].children.push_back(child);
            ++tree[lead].reference_count;
            children.push_back(child);
            for (std::size_t p = 0; p < config.leaf_regions; ++p)
                tree[child].kv_regions.push_back(builder.allocate(MemoryKind::KV,
                    "episode" + std::to_string(episode) + "_agent" + std::to_string(agent) +
                    "_leaf" + std::to_string(p)));
            builder.metadata(0, lead, Operation::Write, "fork_refcount_increment");
            builder.metadata(node, lead, Operation::Read, "child_prefix_lookup");
            builder.metadata(node, child, Operation::Write, "child_pointer_insert");
        }
        --tree[lead].reference_count; // lead releases its own active reference
        builder.metadata(1, lead, Operation::Write, "lead_complete");

        // Deterministic round-robin interleaving models concurrent agents, without
        // pretending to model interconnect queueing or CPU instruction timing.
        for (std::size_t round = 0; round < config.rounds; ++round) {
            if (config.stress && round == config.rounds / 2)
                builder.emit(1, prefix.front(), 0, Operation::Write, Mode::Coherent, no_owner,
                             "unexpected_prefix_write");
            for (const auto child : children) {
                const auto node = tree[child].agent;
                for (const auto region : prefix)
                    builder.emit(node, region, round, Operation::Read, Mode::SharedReadOnly,
                                 no_owner, "shared_prefix_read");
                const auto leaf_index = (round / system.lines_per_region()) % config.leaf_regions;
                builder.emit(node, tree[child].kv_regions[leaf_index], round, Operation::Write,
                             Mode::Private, node, "private_leaf_append");
                if (round % 8 == 0) {
                    builder.metadata(node, lead, Operation::Read, "shared_tree_lookup");
                    builder.metadata(node, child, Operation::Write, "child_refcount_update");
                    builder.metadata(0, child, Operation::Write, "host_bookkeeping");
                }
            }
            if (config.stress && round == config.rounds / 3) {
                const auto leaf = tree[children.front()].kv_regions.front();
                builder.emit(0, leaf, 0, Operation::Read, Mode::Coherent, no_owner,
                             "remote_private_read");
                builder.emit(0, leaf, 0, Operation::Write, Mode::Coherent, no_owner,
                             "remote_private_write");
            }
        }
        for (const auto child : children) {
            --tree[lead].reference_count;
            --tree[child].reference_count;
            builder.metadata(tree[child].agent, lead, Operation::Write, "child_release_prefix");
            builder.metadata(0, child, Operation::Write, "child_complete");
        }
        --tree[0].reference_count;
        builder.metadata(0, lead, Operation::Write, "lead_retire");
    }
    return std::move(builder.workload);
}

} // namespace coherence
