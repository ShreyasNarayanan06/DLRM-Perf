#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
probe_dir=results/dlrm_comparison/hardware
mkdir -p "$probe_dir"
{
    hostname
    lscpu
    lscpu -e=CPU,CORE,SOCKET,NODE
    cat /proc/sys/kernel/perf_event_paranoid
} > "$probe_dir/environment.txt" 2>&1
if [[ ! -x build-perf/cpu_coherence_probe ]]; then
    printf '%s\n' 'Probe unavailable: Linux x86-64 build required.' > "$probe_dir/status.txt"
    exit 0
fi
for bytes in 65536 4194304; do
    for mode in warm cold handoff private; do
        build-perf/cpu_coherence_probe "$mode" "$bytes" 5 > "$probe_dir/${mode}_${bytes}.csv"
    done
done
if command -v perf >/dev/null 2>&1; then
    perf list > "$probe_dir/perf-list.txt" 2>&1
    if perf stat -x, -o "$probe_dir/perf-stat.csv" -e cycles,instructions,cache-references,cache-misses \
        -- build-perf/cpu_coherence_probe handoff 4194304 5 \
        > "$probe_dir/perf-probe.csv" 2> "$probe_dir/perf-errors.txt"; then
        printf '%s\n' 'Generic PMU counters collected; they include initialization and synchronization.' > "$probe_dir/status.txt"
    else
        printf '%s\n' 'PMU counters unavailable; see perf-errors.txt. No kernel settings were changed.' > "$probe_dir/status.txt"
    fi
else
    printf '%s\n' 'perf is not installed. CPU timing measurements were still collected.' > "$probe_dir/status.txt"
fi
