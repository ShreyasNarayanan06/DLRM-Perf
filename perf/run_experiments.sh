#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
simulator="${1:-$project_dir/build-perf/architecture_perf}"
destination="${2:-results/dlrm_refined}"
mkdir -p "$destination"
run_case() {
    local case_name="$1"
    shift
    "$simulator" "$@" --csv "$destination/${case_name}.csv" \
        --kinds "$destination/${case_name}_kinds.csv" \
        --phases "$destination/${case_name}_phases.csv" > "$destination/${case_name}.txt"
}
run_case legacy_default --workload both --directory-model 0
run_case default --workload both
run_case cache_fitting --workload both --table-lines 512
exceeding=(--cache-bytes 4096 --prefix-lines 1024 --leaf-lines 1024 --lead-steps 512 --prefix-reads 128 --table-lines 8192)
run_case cache_exceeding --workload both "${exceeding[@]}"
run_case bandwidth_constrained --workload both "${exceeding[@]}" --bandwidth-gbps 2
long=(--rounds 512 --warmup-rounds 128)
run_case long_default --workload both "${long[@]}"
run_case long_fitting --workload both "${long[@]}" --table-lines 512
run_case long_exceeding --workload both "${long[@]}" "${exceeding[@]}"
run_case long_bandwidth --workload both "${long[@]}" "${exceeding[@]}" --bandwidth-gbps 2
run_case directory_wide --workload dlrm "${long[@]}" --directory-entries 65536
run_case directory_small --workload dlrm "${long[@]}" --directory-entries 64
# Explicit controller stress, not a claim about production CXL hardware.
run_case directory_queue_stress --workload dlrm "${long[@]}" --directory-banks 1 --directory-issue-ns 20 --directory-mshrs 4
run_case long_no_cold_writes --workload dlrm "${long[@]}" --cold-update-every 0
run_case long_hot_updates --workload dlrm "${long[@]}" --hot-update-every 1
run_case long_seed43 --workload dlrm "${long[@]}" --seed 43
run_case directory_miss_zero --workload dlrm "${long[@]}" --directory-miss-ns 0
run_case directory_miss_20 --workload dlrm "${long[@]}" --directory-miss-ns 20
run_case directory_miss_160 --workload dlrm "${long[@]}" --directory-miss-ns 160
printf '%s\n'  "Wrote 9 paired regimes/controls and 9 DLRM controls in $destination"
