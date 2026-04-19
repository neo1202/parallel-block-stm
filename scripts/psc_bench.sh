#!/bin/bash
# psc_bench.sh - run exp A + exp B for V0 (mutex) and V1 (lock-free chain).
# submit:  sbatch scripts/psc_bench.sh
# output:  benchmark_records/psc_<jobid>_VX/exp_{a,b}.csv

#SBATCH --job-name=blockstm
#SBATCH --partition=RM
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=128
#SBATCH --time=00:40:00
#SBATCH --output=bench_%j.out
#SBATCH --error=bench_%j.err

set -e
export HOSTNAME=$(hostname)
cd "$SLURM_SUBMIT_DIR"
module load gcc/13.3.1-p20240614

# fixed workload - calibrated locally so LF wins by ~15% on hot/cold
BLOCK=10000
READS=2
WRITES=2
COMPUTE=200
RUNS=10
THREADS="1,2,4,8,16,32,64,128"

# hot/cold
HOT_RATIO=0.2
HOT_ACCOUNTS=50
HOT_KEYS=1

# build mutex (V0) and lock-free-chain (V1) side by side
MUTEX_REF="8239b3f"
DIR_MUTEX="build-release-mutex"
DIR_LF="build-release-lf"

echo "[build] mutex (V0) @ $MUTEX_REF"
git checkout $MUTEX_REF -- src/mvmemory.h
cmake -S . -B "$DIR_MUTEX" -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
cmake --build "$DIR_MUTEX" -j > /dev/null 2>&1
git checkout HEAD -- src/mvmemory.h

echo "[build] lock-free chain (V1) @ HEAD"
cmake -S . -B "$DIR_LF" -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
cmake --build "$DIR_LF" -j > /dev/null 2>&1

GIT_HASH=$(git rev-parse --short HEAD)
OUT_BASE="benchmark_records/psc_${SLURM_JOB_ID}"

write_header() {
    local csv=$1 ver=$2 expid=$3 expnote=$4
    cat > "$csv" <<HDR
# job_id: ${SLURM_JOB_ID}
# version: ${ver} (commit ${GIT_HASH})
# experiment: ${expid} - ${expnote}
# node: $(hostname), cores: ${SLURM_CPUS_PER_TASK}
# fixed: block=${BLOCK}, reads=${READS}, writes=${WRITES}, compute=${COMPUTE}, runs=${RUNS}
# threads tested: ${THREADS}
HDR
}

run_exp_a() {
    local ver=$1 binary=$2
    local dir="${OUT_BASE}_${ver}"
    mkdir -p "$dir"
    local csv="${dir}/exp_a_contention.csv"
    write_header "$csv" "$ver" "A" "contention sweep, uniform accounts"
    echo "threads,accounts,time_ms,throughput" >> "$csv"

    for acc in 2 10 100 1000 10000; do
        # bench_scaling prints a header + one row per thread count. drop header, keep rows, remap columns to match ours.
        "$binary" --threads $THREADS --block-size $BLOCK \
            --reads $READS --writes $WRITES --compute $COMPUTE --runs $RUNS \
            --accounts $acc \
            | tail -n +2 \
            | awk -F, -v a=$acc 'NF{print $1","a","$4","$5}' \
            >> "$csv"
    done
    echo "  [A/${ver}] -> $csv"
}

run_exp_b() {
    local ver=$1 binary=$2
    local dir="${OUT_BASE}_${ver}"
    mkdir -p "$dir"
    local csv="${dir}/exp_b_hotcold.csv"
    write_header "$csv" "$ver" "B" "dex-style hot/cold, 1000 accounts (50 hot), hot_tx_ratio=${HOT_RATIO}"
    echo "threads,accounts,time_ms,throughput" >> "$csv"

    "$binary" --threads $THREADS --block-size $BLOCK \
        --reads $READS --writes $WRITES --compute $COMPUTE --runs $RUNS \
        --accounts 1000 \
        --hot-tx-ratio $HOT_RATIO --hot-accounts $HOT_ACCOUNTS --hot-keys $HOT_KEYS \
        | tail -n +2 \
        | awk -F, 'NF{print $1","$3","$4","$5}' \
        >> "$csv"
    echo "  [B/${ver}] -> $csv"
}

echo ""
echo "=== exp A - contention sweep ==="
run_exp_a V0-mutex   "./${DIR_MUTEX}/bench/bench_scaling"
run_exp_a V1-lfchain "./${DIR_LF}/bench/bench_scaling"

echo ""
echo "=== exp B - dex hot/cold ==="
run_exp_b V0-mutex   "./${DIR_MUTEX}/bench/bench_scaling"
run_exp_b V1-lfchain "./${DIR_LF}/bench/bench_scaling"

echo ""
echo "done. outputs under ${OUT_BASE}_*"
ls -la "${OUT_BASE}"* 2>/dev/null || true
