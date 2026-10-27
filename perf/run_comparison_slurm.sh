#!/usr/bin/env bash
#SBATCH --job-name=dlrm-management
#SBATCH --account=eecs570f26s001_class
#SBATCH --partition=standard
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=3G
#SBATCH --time=00:10:00
#SBATCH --output=dlrm-management-%j.log
set -euo pipefail
cd "${SLURM_SUBMIT_DIR:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)}"
hostname
g++ --version
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "${SLURM_CPUS_PER_TASK:-2}"
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p test_embedding_skew.py -v
bash perf/run_comparisons.sh "$PWD/build/perf/architecture_perf"
python3 scripts/audit_comparison.py results/management
cat results/management/default.txt results/management/software_free.txt results/management/region_256.txt
