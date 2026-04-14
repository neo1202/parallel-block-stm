#!/bin/bash
# =============================================================================
# run_benchmarks.sh
# =============================================================================
#
#   ./scripts/run_benchmarks.sh            correctness (parallel == sequential)
#   ./scripts/run_benchmarks.sh speed      main benchmark: 1,2,4,8 threads
#   ./scripts/run_benchmarks.sh sweep      contention sweep: low/mid/high
#
# =============================================================================

set -e
cd "$(dirname "$0")/.."

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

DIR_DEBUG="build"
DIR_RELEASE="build-release"

build_debug() {
    echo -e "${YELLOW}[build] Debug...${NC}"
    cmake -S . -B "$DIR_DEBUG" -DCMAKE_BUILD_TYPE=Debug -DSANITIZER="" > /dev/null 2>&1
    cmake --build "$DIR_DEBUG" > /dev/null 2>&1
    echo -e "${GREEN}[build] Done.${NC}"
}

build_release() {
    echo -e "${YELLOW}[build] Release (-O3)...${NC}"
    cmake -S . -B "$DIR_RELEASE" -DCMAKE_BUILD_TYPE=Release -DSANITIZER="" > /dev/null 2>&1
    cmake --build "$DIR_RELEASE" > /dev/null 2>&1
    echo -e "${GREEN}[build] Done.${NC}"
}

# --- Correctness: parallel == sequential ---
run_correctness() {
    build_debug
    echo ""
    echo -e "${YELLOW}=== Correctness Tests ===${NC}"
    for t in "$DIR_DEBUG"/test/test_*; do
        name=$(basename "$t")
        echo -e "\n${YELLOW}--- $name ---${NC}"
        "$t"
    done
    echo ""
    echo -e "${GREEN}All tests passed.${NC}"
}

# --- Speed: main benchmark ---
run_speed() {
    build_release
    echo ""
    echo -e "${YELLOW}=== Speed: 500k txs, 500 accounts, threads 1/2/4/8 ===${NC}"
    ./"$DIR_RELEASE"/bench/bench_scaling --threads 1,2,4,8 --block-size 500000 --accounts 500 --runs 5
}

# --- Sweep: contention levels ---
run_sweep() {
    build_release
    echo ""
    echo -e "${YELLOW}=== Low contention: 500k txs, 5000 accounts ===${NC}"
    ./"$DIR_RELEASE"/bench/bench_scaling --threads 1,2,4,8 --block-size 500000 --accounts 5000 --runs 5
    echo ""
    echo -e "${YELLOW}=== Mid contention: 500k txs, 500 accounts ===${NC}"
    ./"$DIR_RELEASE"/bench/bench_scaling --threads 1,2,4,8 --block-size 500000 --accounts 500 --runs 5
    echo ""
    echo -e "${YELLOW}=== High contention: 500k txs, 10 accounts ===${NC}"
    ./"$DIR_RELEASE"/bench/bench_scaling --threads 1,2,4,8 --block-size 500000 --accounts 10 --runs 5
}

# --- Main ---
case "${1:-default}" in
    default)
        run_correctness
        ;;
    speed)
        run_speed
        ;;
    sweep)
        run_sweep
        ;;
    *)
        echo "Usage: $0 [speed|sweep]"
        echo ""
        echo "  (no args)   Correctness tests (parallel == sequential)"
        echo "  speed       Main benchmark: 500k txs, 1/2/4/8 threads"
        echo "  sweep       Contention sweep: low/mid/high x 1/2/4/8 threads"
        ;;
esac
