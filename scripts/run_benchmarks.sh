#!/bin/bash
# =============================================================================
# run_benchmarks.sh
# =============================================================================
#
#   ./scripts/run_benchmarks.sh            correctness (parallel == sequential)
#   ./scripts/run_benchmarks.sh speed      main benchmark: 1,2,4,8 threads
#   ./scripts/run_benchmarks.sh sweep      contention sweep: low/mid/high
#   ./scripts/run_benchmarks.sh record     run benchmarks, save CSVs & plots
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

# --- Heavy Workload: simulate VM execution ---
run_heavy() {
    build_release
    echo ""
    echo -e "${YELLOW}=== Heavy Workload (Simulated VM): 50k txs, 500 accounts ===${NC}"
    echo -e "${YELLOW}This demonstrates the true parallel scaling power of Block-STM when transactions are complex.${NC}"
    ./"$DIR_RELEASE"/bench/bench_scaling --threads 1,2,4,8 --block-size 50000 --accounts 500 --compute 100000 --runs 5
}

# --- Record: save outputs & plot ---
run_record() {
    build_release
    
    TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
    OUT_DIR="benchmark_records/${TIMESTAMP}"
    mkdir -p "$OUT_DIR"
    
    echo ""
    echo -e "${YELLOW}=== Recording benchmark results to ${OUT_DIR} ===${NC}"
    
    # Speed
    echo -e "${YELLOW}Running scaling benchmark...${NC}"
    ./"$DIR_RELEASE"/bench/bench_scaling --threads 1,2,4,8 --block-size 500000 --accounts 500 --runs 5 > "${OUT_DIR}/scaling.csv"
    
    # Sweep
    echo -e "${YELLOW}Running sweep benchmark...${NC}"
    ./"$DIR_RELEASE"/bench/bench_scaling --threads 1,2,4,8 --block-size 500000 --accounts 5000 --runs 5 > "${OUT_DIR}/sweep_low.csv"
    ./"$DIR_RELEASE"/bench/bench_scaling --threads 1,2,4,8 --block-size 500000 --accounts 500 --runs 5 > "${OUT_DIR}/sweep_mid.csv"
    ./"$DIR_RELEASE"/bench/bench_scaling --threads 1,2,4,8 --block-size 500000 --accounts 10 --runs 5 > "${OUT_DIR}/sweep_high.csv"
    
    # Heavy
    echo -e "${YELLOW}Running heavy workload benchmark...${NC}"
    ./"$DIR_RELEASE"/bench/bench_scaling --threads 1,2,4,8 --block-size 50000 --accounts 500 --compute 100000 --runs 5 > "${OUT_DIR}/heavy.csv"
    
    # Combine Sweep CSVs
    head -n 1 "${OUT_DIR}/sweep_low.csv" > "${OUT_DIR}/sweep.csv"
    tail -n +2 "${OUT_DIR}/sweep_low.csv" >> "${OUT_DIR}/sweep.csv"
    tail -n +2 "${OUT_DIR}/sweep_mid.csv" >> "${OUT_DIR}/sweep.csv"
    tail -n +2 "${OUT_DIR}/sweep_high.csv" >> "${OUT_DIR}/sweep.csv"
    rm "${OUT_DIR}/sweep_low.csv" "${OUT_DIR}/sweep_mid.csv" "${OUT_DIR}/sweep_high.csv"

    # Plot
    echo -e "${YELLOW}Plotting results...${NC}"
    python3 scripts/plot_results.py "${OUT_DIR}/scaling.csv" "${OUT_DIR}/sweep.csv" "${OUT_DIR}/heavy.csv" "${OUT_DIR}"
    
    echo -e "${GREEN}Done! Records and plots saved to ${OUT_DIR}${NC}"
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
    record)
        run_record
        ;;
    *)
        echo "Usage: $0 [speed|sweep|record]"
        echo ""
        echo "  (no args)   Correctness tests (parallel == sequential)"
        echo "  speed       Main benchmark: 500k txs, 1/2/4/8 threads"
        echo "  sweep       Contention sweep: low/mid/high x 1/2/4/8 threads"
        echo "  record      Run benchmarks, save CSVs and generate plots"
        ;;
esac
