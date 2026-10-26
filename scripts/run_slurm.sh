#!/usr/bin/env bash
#SBATCH --job-name=radix-coherence
#SBATCH --account=eecs570f26s001_class
#SBATCH --partition=standard
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=1G
#SBATCH --time=00:05:00
#SBATCH --output=radix-coherence-%j.log

set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# Slurm copies batch scripts into its spool directory. Use the submission
# directory when launched by sbatch; submit from the project root as documented.
if [[ -n "${SLURM_SUBMIT_DIR:-}" ]]; then project_dir="$SLURM_SUBMIT_DIR"; fi
cd "$project_dir"

hostname
g++ --version
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "${SLURM_CPUS_PER_TASK:-2}"
ctest --test-dir build --output-on-failure
mkdir -p results
build/radix_coherence --csv results/nominal.csv \
    --transitions results/nominal_transitions.csv \
    --dump-workload results/nominal_workload.csv > results/nominal.txt
build/radix_coherence --stress --csv results/stress.csv \
    --transitions results/stress_transitions.csv \
    --dump-workload results/stress_workload.csv > results/stress.txt
bash scripts/sweep.sh
cat results/nominal.txt
cat results/stress.txt
echo "Validation and experiments complete: $project_dir/results"
