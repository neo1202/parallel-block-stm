#!/bin/bash
# =============================================================================
# psc_bench.sh - PSC Bridges-2 batch benchmark
# =============================================================================
#
# Submit:  sbatch scripts/psc_bench.sh
# Status:  squeue -u $USER
# Output:  bench_<jobid>.out
# =============================================================================

#SBATCH --job-name=blockstm
#SBATCH --partition=RM
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=128
#SBATCH --time=00:30:00
#SBATCH --output=bench_%j.out
#SBATCH --error=bench_%j.err

set -e
export HOSTNAME=$(hostname)
cd "$SLURM_SUBMIT_DIR"

module load gcc/13.3.1-p20240614

MUTEX_REF="8239b3f"
DIR_MUTEX="build-release-mutex"
DIR_LF="build-release-lf"

# --- Build both versions ---
echo "[1/2] Building MUTEX baseline..."
git checkout $MUTEX_REF -- src/mvmemory.h
cmake -S . -B "$DIR_MUTEX" -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
cmake --build "$DIR_MUTEX" -j > /dev/null 2>&1
git checkout HEAD -- src/mvmemory.h

echo "[2/2] Building LOCK-FREE..."
cmake -S . -B "$DIR_LF" -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
cmake --build "$DIR_LF" -j > /dev/null 2>&1

# --- Setup ---
BLOCK=10000
COMPUTE=500
RUNS=10
THREADS="1,2,4,8,16,32,64,128"
OUT_DIR="psc_results_${SLURM_JOB_ID}"
mkdir -p "$OUT_DIR"

run_pair() {
    local label=$1; local accounts=$2
    echo ""
    echo "============================================================"
    echo "  $label  (accounts=$accounts, compute=$COMPUTE, runs=$RUNS)"
    echo "============================================================"

    echo ""
    echo "--- MUTEX VersionChain ---"
    ./"$DIR_MUTEX"/bench/bench_scaling \
        --threads $THREADS --block-size $BLOCK \
        --accounts $accounts --compute $COMPUTE --runs $RUNS \
        | tee "$OUT_DIR/${label}_mutex.csv"

    echo ""
    echo "--- LOCK-FREE VersionChain ---"
    ./"$DIR_LF"/bench/bench_scaling \
        --threads $THREADS --block-size $BLOCK \
        --accounts $accounts --compute $COMPUTE --runs $RUNS \
        | tee "$OUT_DIR/${label}_lf.csv"
}

# --- Run experiments ---
echo "Node: $(hostname), Cores: $SLURM_CPUS_PER_TASK"
echo "Block size: $BLOCK, Compute: $COMPUTE iterations of SHA-256"
echo "Threads: $THREADS"

run_pair "low_contention"  10000
run_pair "mid_contention"   1000
run_pair "high_contention"   100

echo ""
echo "============================================================"
echo "DONE. Results in $OUT_DIR/"
echo "============================================================"
ls -la "$OUT_DIR/"
