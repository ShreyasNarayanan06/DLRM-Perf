#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
simulator="${1:-$project_dir/build-perf/architecture_perf}"
destination="${2:-results/blind}"
mkdir -p "$destination"
run_case() {
    local case_name="$1"
    shift
    "$simulator" "$@" --output-prefix "$destination/$case_name" > "$destination/$case_name.txt"
}
long=(--rounds 512 --warmup-rounds 128)
run_case default --workload both "${long[@]}" --events 1
run_case cache_fitting --workload both "${long[@]}" --table-lines 512
run_case cache_exceeding --workload both "${long[@]}" --cache-bytes 4096 --table-lines 8192 \
    --prefix-lines 1024 --leaf-lines 1024 --lead-steps 512 --prefix-reads 128
# Same program addresses as default, different timing; data events are recomputed.
run_case slow_link --workload both "${long[@]}" --bandwidth-gbps 2
run_case fast_memory --workload dlrm "${long[@]}" --memory-ns 75 --peer-ns 75
run_case layout_seed8 --workload dlrm "${long[@]}" --layout-seed 8
run_case seed43 --workload dlrm "${long[@]}" --seed 43
run_case no_host_writes --workload dlrm "${long[@]}" --hot-update-every 0 --cold-update-every 0
run_case frequent_updates --workload dlrm "${long[@]}" --hot-update-every 1
run_case replay_short --workload dlrm --rounds 8 --events 1
printf '%s\n' "Wrote timed baseline profiles and address distributions in $destination"
