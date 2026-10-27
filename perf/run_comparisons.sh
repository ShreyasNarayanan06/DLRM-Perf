#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
simulator="${1:-$project_dir/build/perf/architecture_perf}"
destination="${2:-results/management}"
mkdir -p "$destination"
run_case() {
    local case_name="$1"
    shift
    "$simulator" --management compare --rounds 512 --warmup-rounds 128 "$@" \
        --output-prefix "$destination/$case_name" > "$destination/$case_name.txt"
}
run_case default --events 1
run_case cache_fitting --table-lines 512
run_case cache_exceeding --cache-bytes 4096 --table-lines 8192
run_case slow_link --bandwidth-gbps 2
run_case frequent_updates --hot-update-every 1 --hot-updates 4
run_case no_updates --hot-update-every 0 --cold-update-every 0
run_case software_free --software-ns 0 --software-message-bytes 0
run_case software_25ns --software-ns 25
run_case software_250ns --software-ns 250
run_case region_64 --region-bytes 64
run_case region_256 --region-bytes 256
run_case region_1024 --region-bytes 1024
run_case region_table_16 --region-entries 16
run_case costly_transitions --transition-ns 1000
run_case seed43 --seed 43
run_case agents8 --agents 8
run_case replay_short --rounds 8 --warmup-rounds 0 --events 1
printf '%s\n' "Wrote 17 software/hardware/dynamic comparisons in $destination"
