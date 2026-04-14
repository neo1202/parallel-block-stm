#!/bin/bash
# =============================================================================
# run_benchmarks.sh - Build and run tests / benchmarks
# =============================================================================
#
# Usage:
#   ./scripts/run_benchmarks.sh              # correctness + speed (the lazy command)
#   ./scripts/run_benchmarks.sh test         # correctness tests only (Debug)
#   ./scripts/run_benchmarks.sh bench        # all benchmarks (Release)
#   ./scripts/run_benchmarks.sh all          # everything
#
# =============================================================================

set -e
cd "$(dirname "$0")/.."

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# ---- Build ----
build_debug() {
    echo -e "${YELLOW}[build] Debug...${NC}"
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSANITIZER="" > /dev/null 2>&1
    cmake --build build > /dev/null 2>&1
    echo -e "${GREEN}[build] Done.${NC}"
}

build_release() {
    echo -e "${YELLOW}[build] Release (-O3)...${NC}"
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSANITIZER="" > /dev/null 2>&1
    cmake --build build > /dev/null 2>&1
    echo -e "${GREEN}[build] Done.${NC}"
}

# ---- Correctness tests (Debug) ----
run_tests() {
    build_debug
    echo ""
    echo -e "${YELLOW}=== Correctness Tests ===${NC}"
    for t in build/test/test_*; do
        name=$(basename "$t")
        echo -e "\n${YELLOW}--- $name ---${NC}"
        "$t"
    done
    echo ""
    echo -e "${GREEN}All tests passed.${NC}"
}

# ---- Speed comparison: sequential vs parallel 2,4,8 threads (Release) ----
run_speed() {
    build_release
    echo ""
    echo -e "${YELLOW}=== Speed: sequential vs parallel (1,2,4,8 threads) ===${NC}"
    ./build/bench/bench_scaling --threads 1,2,4,8 --block-size 1000 --accounts 100 --runs 5
}

# ---- Full benchmarks (Release) ----
run_bench() {
    build_release
    echo ""
    echo -e "${YELLOW}=== Thread Scaling ===${NC}"
    ./build/bench/bench_scaling --threads 1,2,4,8 --block-size 1000 --accounts 100 --runs 5
    echo ""
    echo -e "${YELLOW}=== Contention Sweep ===${NC}"
    ./build/bench/bench_contention --threads 8 --accounts 2,10,100,1000 --runs 5
    echo ""
    echo -e "${YELLOW}=== Block Size Sweep ===${NC}"
    ./build/bench/bench_blocksize --threads 8 --block-sizes 64,256,1024,4096 --runs 5
}

# ---- Main ----
case "${1:-default}" in
    default)
        # The lazy command: correctness + speed
        run_tests
        echo ""
        run_speed
        ;;
    test)
        run_tests
        ;;
    speed)
        run_speed
        ;;
    bench)
        run_bench
        ;;
    all)
        run_tests
        echo ""
        run_bench
        ;;
    *)
        echo "Usage: $0 [test|speed|bench|all]"
        echo ""
        echo "  (no args)   Correctness tests + speed comparison (the lazy command)"
        echo "  test        Correctness tests only (Debug)"
        echo "  speed       Sequential vs parallel speed only (Release)"
        echo "  bench       All benchmarks (Release)"
        echo "  all         Correctness + all benchmarks"
        ;;
esac
