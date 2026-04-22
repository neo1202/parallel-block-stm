#!/bin/bash
# psc_bench_current.sh - run exp A + exp B on the current HEAD build.
# no version juggling — just build main and sweep.
# submit: sbatch scripts/psc_bench_current.sh

#SBATCH --job-name=blockstm-cur
#SBATCH --partition=RM
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=128
#SBATCH --time=00:30:00
#SBATCH --output=bench_cur_%j.out
#SBATCH --error=bench_cur_%j.err

set -e
export HOSTNAME=$(hostname)
cd "$SLURM_SUBMIT_DIR"
module load gcc/13.3.1-p20240614

# same workload params as psc_bench.sh so numbers are comparable
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
OUT_BASE="benchmark_records/psc_current_${SLURM_JOB_ID}"
mkdir -p "$OUT_BASE"

echo "[build] current HEAD"
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
cmake --build build-release -j > /dev/null 2>&1

BIN="./build-release/bench/bench_scaling"

write_header() {
    local csv=$1 expid=$2 note=$3
    cat > "$csv" <<HDR
# job_id: ${SLURM_JOB_ID}
# commit: ${GIT_HASH}
# experiment: ${expid} - ${note}
# node: $(hostname), cores: ${SLURM_CPUS_PER_TASK}
# fixed: block=${BLOCK}, reads=${READS}, writes=${WRITES}, compute=${COMPUTE}, runs=${RUNS}
# threads tested: ${THREADS}
HDR
}

echo ""
echo "=== exp A - contention sweep ==="
csv_a="${OUT_BASE}/exp_a_contention.csv"
write_header "$csv_a" "A" "contention sweep, uniform accounts"
echo "threads,accounts,time_ms,throughput,validation_aborts,dependency_suspends,total_executions,abort_rate,wasted_exec_ratio" >> "$csv_a"

for acc in 2 10 100 1000 10000; do
    echo "  accounts=$acc"
    "$BIN" --threads $THREADS --block-size $BLOCK \
        --reads $READS --writes $WRITES --compute $COMPUTE --runs $RUNS \
        --accounts $acc \
        | tail -n +2 \
        | awk -F, -v a=$acc 'NF{print $1","a","$4","$5","$6","$7","$8","$9","$10}' \
        >> "$csv_a"
done

echo ""
echo "=== exp B - dex hot/cold ==="
csv_b="${OUT_BASE}/exp_b_hotcold.csv"
write_header "$csv_b" "B" "dex-style hot/cold, 1000 accounts (50 hot), hot_tx_ratio=${HOT_RATIO}"
echo "threads,accounts,time_ms,throughput,validation_aborts,dependency_suspends,total_executions,abort_rate,wasted_exec_ratio" >> "$csv_b"

"$BIN" --threads $THREADS --block-size $BLOCK \
    --reads $READS --writes $WRITES --compute $COMPUTE --runs $RUNS \
    --accounts 1000 \
    --hot-tx-ratio $HOT_RATIO --hot-accounts $HOT_ACCOUNTS --hot-keys $HOT_KEYS \
    | tail -n +2 \
    | awk -F, 'NF{print $1","$3","$4","$5","$6","$7","$8","$9","$10}' \
    >> "$csv_b"

echo ""
echo "done. outputs in $OUT_BASE/"
ls -la "$OUT_BASE"
