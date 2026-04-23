#!/bin/bash
# psc_batch_sweep.sh - sweep BATCH size {3,4,5,6} to find the sweet spot
# submit: sbatch scripts/psc_batch_sweep.sh

#SBATCH --job-name=blockstm-sweep
#SBATCH --partition=RM
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=128
#SBATCH --time=01:00:00
#SBATCH --output=sweep_%j.out
#SBATCH --error=sweep_%j.err

set -e
export HOSTNAME=$(hostname)
cd "$SLURM_SUBMIT_DIR"
module load gcc/13.3.1-p20240614

BLOCK=10000
READS=2
WRITES=2
COMPUTE=200
RUNS=10
THREADS="1,2,4,8,16,32,64,128"

HOT_RATIO=0.2
HOT_ACCOUNTS=50
HOT_KEYS=1

GIT_HASH=$(git rev-parse --short HEAD)
OUT_BASE="benchmark_records/psc_sweep_${SLURM_JOB_ID}"
mkdir -p "$OUT_BASE"

# build once per BATCH value
for b in 3 4 5 6; do
    echo "[build] BATCH=${b}"
    cmake -S . -B "build-b${b}" -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_CXX_FLAGS="-DEXEC_BATCH=${b}" > /dev/null 2>&1
    cmake --build "build-b${b}" -j > /dev/null 2>&1
done

for b in 3 4 5 6; do
    BIN="./build-b${b}/bench/bench_scaling"
    csv_a="${OUT_BASE}/exp_a_b${b}.csv"
    csv_b="${OUT_BASE}/exp_b_b${b}.csv"

    echo ""
    echo "=== BATCH=${b} exp A ==="
    cat > "$csv_a" <<HDR
# job_id: ${SLURM_JOB_ID}, BATCH=${b}, commit: ${GIT_HASH}
# fixed: block=${BLOCK}, reads=${READS}, writes=${WRITES}, compute=${COMPUTE}, runs=${RUNS}
HDR
    echo "threads,accounts,time_ms,throughput,validation_aborts,dependency_suspends,total_executions,abort_rate,wasted_exec_ratio" >> "$csv_a"

    for acc in 2 10 100 1000 10000; do
        echo "  BATCH=${b} accounts=${acc}"
        "$BIN" --threads $THREADS --block-size $BLOCK \
            --reads $READS --writes $WRITES --compute $COMPUTE --runs $RUNS \
            --accounts $acc \
            | tail -n +2 \
            | awk -F, -v a=$acc 'NF{print $1","a","$4","$5","$6","$7","$8","$9","$10}' \
            >> "$csv_a"
    done

    echo ""
    echo "=== BATCH=${b} exp B (hot/cold) ==="
    cat > "$csv_b" <<HDR
# job_id: ${SLURM_JOB_ID}, BATCH=${b}, commit: ${GIT_HASH}
HDR
    echo "threads,accounts,time_ms,throughput,validation_aborts,dependency_suspends,total_executions,abort_rate,wasted_exec_ratio" >> "$csv_b"

    "$BIN" --threads $THREADS --block-size $BLOCK \
        --reads $READS --writes $WRITES --compute $COMPUTE --runs $RUNS \
        --accounts 1000 \
        --hot-tx-ratio $HOT_RATIO --hot-accounts $HOT_ACCOUNTS --hot-keys $HOT_KEYS \
        | tail -n +2 \
        | awk -F, 'NF{print $1","$3","$4","$5","$6","$7","$8","$9","$10}' \
        >> "$csv_b"
done

echo ""
echo "done. outputs in $OUT_BASE/"
ls -la "$OUT_BASE"
