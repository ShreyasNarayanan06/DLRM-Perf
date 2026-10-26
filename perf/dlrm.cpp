#include "model.hpp"
#include <algorithm>
#include <cmath>
#include <random>

namespace perf {
namespace {
constexpr std::uint64_t hot_base = 1ULL << 44;
constexpr std::uint64_t cold_base = 1ULL << 48;
constexpr std::uint64_t scratch_base = 1ULL << 52;
double uniform(std::mt19937_64& rng) {
    // Specify the conversion instead of relying on library-specific distributions.
    return static_cast<double>(rng() >> 11) * (1.0 / 9007199254740992.0);
}
std::size_t choose(std::mt19937_64& rng, std::size_t count) {
    return std::min(count - 1, static_cast<std::size_t>(uniform(rng) * static_cast<double>(count)));
}
}
std::size_t hot_lines(const Config& c) {
    return std::clamp(static_cast<std::size_t>(std::llround(static_cast<double>(c.table_lines) * c.hot_fraction)),
                      std::size_t{1}, c.table_lines - 1);
}
Workload make_dlrm_workload(const Config& c) {
    const auto hot = hot_lines(c), cold = c.table_lines - hot;
    std::vector<double> cdf(hot);
    double sum = 0;
    for (std::size_t i = 0; i < hot; ++i) {
        sum += 1 / std::pow(static_cast<double>(i + 1), c.zipf_alpha);
        cdf[i] = sum;
    }
    for (auto& value : cdf) value /= sum;
    const auto hot_address = [&](std::mt19937_64& rng) {
        const auto it = std::lower_bound(cdf.begin(), cdf.end(), uniform(rng));
        return hot_base + static_cast<std::uint64_t>(std::min(hot - 1,
            static_cast<std::size_t>(it - cdf.begin()))) * 64;
    };
    Phase phase{"inference_with_online_updates", {}, false};
    for (std::size_t node = 0; node <= c.agents; ++node) {
        // Independent streams keep accelerator traces stable when host cadence changes.
        std::mt19937_64 rng(c.seed + 0x9e3779b97f4a7c15ULL * node);
        Stream stream{node, {}};
        for (std::size_t r = 0; r < c.rounds; ++r) {
            Step gather, mlp;
            if (node == 0) {
                gather.compute_ns = c.host_compute_ns;
                gather.allow_lookahead = false;
                // Pace updates by accelerator 1's batches, not a burst at time zero.
                gather.sync_node = 1;
                gather.sync_step = 2 * r;
                if (c.hot_update_every != 0 && (r + 1) % c.hot_update_every == 0)
                    for (std::size_t u = 0; u < c.hot_updates; ++u)
                        gather.accesses.push_back({AccessKind::HotEmbeddings, hot_address(rng), true, {}});
                if (c.cold_update_every != 0 && (r + 1) % c.cold_update_every == 0)
                    for (std::size_t u = 0; u < c.cold_updates; ++u)
                        gather.accesses.push_back({AccessKind::ColdEmbeddings, cold_base + choose(rng, cold) * 64, true, {}});
            } else {
                // Bottom MLP uses dense features independently of sparse gathers.
                // Top MLP starts only after both embedding gathers and bottom MLP finish.
                gather.concurrent_compute_ns = c.bottom_fraction * c.compute_ns;
                gather.allow_lookahead = false;
                for (std::size_t a = 0; a < c.sparse_reads; ++a) {
                    const bool is_hot = uniform(rng) < c.hot_read_fraction;
                    gather.accesses.push_back({is_hot ? AccessKind::HotEmbeddings : AccessKind::ColdEmbeddings,
                        is_hot ? hot_address(rng) : cold_base + choose(rng, cold) * 64, false, {}});
                }
                mlp.compute_ns = (1 - c.bottom_fraction) * c.compute_ns;
                const auto base = scratch_base + node * (1ULL << 32);
                for (std::size_t a = 0; a < c.scratch_writes; ++a) {
                    const auto line = ((r * c.scratch_writes + a) / c.stores_per_line) % c.scratch_lines;
                    mlp.accesses.push_back({AccessKind::PrivateScratchpad, base + line * 64, true, {}});
                }
                for (std::size_t a = 0; a < c.scratch_reads; ++a) {
                    const auto line = ((r * c.scratch_writes) / c.stores_per_line + a) % c.scratch_lines;
                    mlp.accesses.push_back({AccessKind::PrivateScratchpad, base + line * 64, false, {}});
                }
            }
            stream.steps.push_back(std::move(gather));
            stream.steps.push_back(std::move(mlp));
        }
        phase.streams.push_back(std::move(stream));
    }
    return {std::move(phase)};
}
} // namespace perf
