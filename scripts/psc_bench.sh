#!/bin/bash
# psc_bench.sh - run exp A + exp B across all tracked versions.
# submit:  sbatch scripts/psc_bench.sh
# output:  benchmark_records/psc_<jobid>_VX/exp_{a,b}.csv

#SBATCH --job-name=blockstm
#SBATCH --partition=RM
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=128
#SBATCH --time=01:00:00
#SBATCH --output=bench_%j.out
#SBATCH --error=bench_%j.err

set -e
export HOSTNAME=$(hostname)
cd "$SLURM_SUBMIT_DIR"
module load gcc/13.3.1-p20240614

# workload params (see docs/experiments.md for rationale)
BLOCK=10000
READS=2
WRITES=2
COMPUTE=200
RUNS=10
THREADS="1,2,4,8,16,32,64,128"

HOT_RATIO=0.2
HOT_ACCOUNTS=50
HOT_KEYS=1

# versions:
# V0: mutex chain (8239b3f) + mutex scheduler (8239b3f)
# V1: LF chain no-backoff (f54ebfb) + mutex scheduler (8239b3f)
# V2: LF chain w/ backoff (HEAD) + mutex scheduler (8239b3f)
# V3: LF chain w/ backoff (HEAD) + LF scheduler (HEAD)
MVMEM_V0="8239b3f"
MVMEM_V1="f54ebfb"
SCHED_MUTEX="8239b3f"

GIT_HASH=$(git rev-parse --short HEAD)
OUT_BASE="benchmark_records/psc_${SLURM_JOB_ID}"

# checks out mvmemory.h and scheduler.h at the requested refs, builds,
# then resets both files back to HEAD so we don't drift.
build_version() {
    local tag=$1 mvmem_ref=$2 sched_ref=$3 dir=$4
    echo "[build] ${tag} (mvmemory @ ${mvmem_ref}, scheduler @ ${sched_ref})"
    git checkout "$mvmem_ref" -- src/mvmemory.h
    git checkout "$sched_ref" -- src/scheduler.h
    cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
    cmake --build "$dir" -j > /dev/null 2>&1
    git checkout HEAD -- src/mvmemory.h src/scheduler.h
}

DIR_V0="build-release-V0"
DIR_V1="build-release-V1"
DIR_V2="build-release-V2"
DIR_V3="build-release-V3"

build_version "V0-mutex"             "$MVMEM_V0" "$SCHED_MUTEX" "$DIR_V0"
build_version "V1-lfchain-nobackoff" "$MVMEM_V1" "$SCHED_MUTEX" "$DIR_V1"
build_version "V2-lfchain-backoff"   "HEAD"      "$SCHED_MUTEX" "$DIR_V2"
build_version "V3-lfsched"           "HEAD"      "HEAD"         "$DIR_V3"

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
run_exp_a V0-mutex             "./${DIR_V0}/bench/bench_scaling"
run_exp_a V1-lfchain-nobackoff "./${DIR_V1}/bench/bench_scaling"
run_exp_a V2-lfchain-backoff   "./${DIR_V2}/bench/bench_scaling"
run_exp_a V3-lfsched           "./${DIR_V3}/bench/bench_scaling"

echo ""
echo "=== exp B - dex hot/cold ==="
run_exp_b V0-mutex             "./${DIR_V0}/bench/bench_scaling"
run_exp_b V1-lfchain-nobackoff "./${DIR_V1}/bench/bench_scaling"
run_exp_b V2-lfchain-backoff   "./${DIR_V2}/bench/bench_scaling"
run_exp_b V3-lfsched           "./${DIR_V3}/bench/bench_scaling"

echo ""
echo "done. outputs under ${OUT_BASE}_*"
ls -la "${OUT_BASE}"* 2>/dev/null || true
