#pragma once

#include "coherence/metrics.hpp"
#include "coherence/predictor.hpp"
#include "coherence/workload.hpp"

#include <iosfwd>
#include <vector>

namespace coherence {

struct Result {
    Policy policy;
    Metrics metrics;
};

Result simulate(const Workload& workload, const SystemConfig& system,
                const PredictorConfig& predictor, Policy policy, std::ostream* log = nullptr);
void print_comparison(std::ostream& out, const std::vector<Result>& results);
void write_csv(std::ostream& out, const std::vector<Result>& results);
void write_workload_csv(std::ostream& out, const Workload& workload);

} // namespace coherence
