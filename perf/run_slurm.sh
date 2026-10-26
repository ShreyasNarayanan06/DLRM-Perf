#!/usr/bin/env bash
#SBATCH --job-name=dlrm-blind-profiler
#SBATCH --account=eecs570f26s001_class
#SBATCH --partition=standard
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=2G
#SBATCH --time=00:10:00
#SBATCH --output=dlrm-blind-profiler-%j.log
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${SLURM_SUBMIT_DIR:-}" ]]; then project_dir="$SLURM_SUBMIT_DIR"; fi
cd "$project_dir"
hostname
g++ --version
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "${SLURM_CPUS_PER_TASK:-2}"
ctest --test-dir build --output-on-failure
bash perf/run_experiments.sh "$project_dir/build/perf/architecture_perf"
python3 scripts/audit_perf.py results/blind
cat results/blind/default.txt
cat results/blind/cache_exceeding.txt
