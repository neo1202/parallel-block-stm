#!/bin/bash
# psc_profile.sh - re-profile V2 with -O2 + no-inline so call graph is
# actually interpretable. only does the profile portion of psc_bench.sh.
# submit:  sbatch scripts/psc_profile.sh

#SBATCH --job-name=blockstm-prof
#SBATCH --partition=RM
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=128
#SBATCH --time=00:15:00
#SBATCH --output=prof_%j.out
#SBATCH --error=prof_%j.err

set -e
export HOSTNAME=$(hostname)
cd "$SLURM_SUBMIT_DIR"
module load gcc/13.3.1-p20240614

BLOCK=10000
READS=2
WRITES=2
COMPUTE=200
SCHED_MUTEX="8239b3f"

PROF_DIR="benchmark_records/psc_${SLURM_JOB_ID}_profile_noinline"
DIR_BUILD="build-release-V2-prof"
mkdir -p "$PROF_DIR"

# v2 = LF chain + mutex scheduler. mvmem at HEAD, scheduler at mutex ref.
echo "[build] V2 with -O2 -fno-inline-functions"
git checkout HEAD -- src/mvmemory.h
git checkout "$SCHED_MUTEX" -- src/scheduler.h
cmake -S . -B "$DIR_BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O2 -fno-inline -fno-omit-frame-pointer" \
    > /dev/null 2>&1
cmake --build "$DIR_BUILD" -j > /dev/null 2>&1
git checkout HEAD -- src/mvmemory.h src/scheduler.h

echo "[perf] recording"
perf record -F 400 -g --call-graph dwarf -o "$PROF_DIR/perf.data" \
    "./${DIR_BUILD}/bench/bench_scaling" \
    --threads 128 --block-size $BLOCK --reads $READS --writes $WRITES \
    --compute $COMPUTE --accounts 100 --runs 3

echo "[perf] reporting"
perf report -i "$PROF_DIR/perf.data" --stdio 2>/dev/null \
    | head -200 > "$PROF_DIR/perf_callgraph.txt"
perf report -i "$PROF_DIR/perf.data" --stdio --sort=overhead,symbol 2>/dev/null \
    | head -50 > "$PROF_DIR/perf_flat.txt"

echo "done. outputs in $PROF_DIR/"
