// bench_scaling.cpp - Thread scaling benchmark
//
// Measures execution time across different thread counts for a fixed workload.
// Currently runs sequential baseline only (parallel added in Phase 4).
//
// Usage:
//   ./bench_scaling [options]
//
// Options:
//   --threads     Comma-separated thread counts  (default: 1)
//   --block-size  Transactions per block          (default: 10000)
//   --accounts    Number of accounts              (default: 1000)
//   --seed        Random seed                     (default: 42)
//   --runs        Repetitions per config (median) (default: 5)
//
// Examples:
//   ./bench_scaling
//   ./bench_scaling --block-size 5000 --accounts 100 --seed 99
//   ./bench_scaling --threads 1,2,4,8 --block-size 10000 --accounts 1000
//
// Output: CSV to stdout (pipe to file with > results.csv)
//
/*
t == 1 時跑的是真的 sequential_execute（Line 139），不是 parallel 開 1 個 thread。這是正確的做法：

threads=1  -> sequential_execute()     <- 純 sequential，沒有 MVMemory/Scheduler
threads=2+ -> parallel_execute(t)      <- parallel 版，有全部 overhead
如果用 parallel 開 1 thread，那個時間包含了建 MVMemory、建 Scheduler、開 thread、鎖 version chain 等所有 overhead，不是公平的 baseline。

所以 bench_scaling 的輸出：
threads=1  的時間 = 純 sequential 的速度
threads=8  的時間 = parallel 的速度
speedup = threads=1 的時間 / threads=8 的時間
這就是論文裡衡量加速比的方式。
*/
#include "transaction.h"
#include "workload.h"
#include "sequential.h"
#include "blockstm.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// --- Parse comma-separated integers (e.g., "1,2,4,8") ---
static std::vector<int> parse_int_list(const std::string& s) {
    std::vector<int> result;
    std::istringstream stream(s);
    std::string token;
    while (std::getline(stream, token, ',')) {
        result.push_back(std::stoi(token));
    }
    return result;
}

// --- Simple argument parser ---
struct BenchConfig {
    std::vector<int> threads = {1};
    size_t block_size = 10000;
    size_t accounts   = 1000;
    uint64_t seed     = 42;
    int runs          = 5;
};

static BenchConfig parse_args(int argc, char* argv[]) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc)     cfg.threads    = parse_int_list(argv[++i]);
        else if (arg == "--block-size" && i + 1 < argc) cfg.block_size = std::stoull(argv[++i]);
        else if (arg == "--accounts" && i + 1 < argc)   cfg.accounts   = std::stoull(argv[++i]);
        else if (arg == "--seed" && i + 1 < argc)       cfg.seed       = std::stoull(argv[++i]);
        else if (arg == "--runs" && i + 1 < argc)       cfg.runs       = std::stoi(argv[++i]);
        else if (arg == "--help") {
            std::cout << "Usage: bench_scaling [--threads 1,2,4,8] [--block-size N] "
                         "[--accounts N] [--seed N] [--runs N]\n";
            std::exit(0);
        }
    }
    return cfg;
}

// --- Measure sequential execution time (milliseconds) ---
static double measure_sequential_ms(
    const std::vector<Transaction>& block,
    const std::unordered_map<Key, Value>& initial_state
) {
    auto start = std::chrono::steady_clock::now();
    auto result = sequential_execute(block, initial_state);
    auto end = std::chrono::steady_clock::now();

    // Prevent compiler from optimizing away the result
    if (result.empty()) std::abort();

    return std::chrono::duration<double, std::milli>(end - start).count();
}

// --- Measure parallel execution time (milliseconds) ---
static double measure_parallel_ms(
    const std::vector<Transaction>& block,
    const std::unordered_map<Key, Value>& initial_state,
    int num_threads
) {
    auto start = std::chrono::steady_clock::now();
    auto result = parallel_execute(block, initial_state, num_threads);
    auto end = std::chrono::steady_clock::now();

    // Prevent compiler from optimizing away the result
    if (result.empty()) std::abort();

    return std::chrono::duration<double, std::milli>(end - start).count();
}

// --- Compute median of a vector ---
static double median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 0) return (v[n / 2 - 1] + v[n / 2]) / 2.0;
    return v[n / 2];
}

int main(int argc, char* argv[]) {
    auto cfg = parse_args(argc, argv);

    // Generate workload once (reused across all runs)
    WorkloadConfig wl_cfg{
        .num_txs = cfg.block_size,
        .num_accounts = cfg.accounts,
        .seed = cfg.seed
    };
    auto block = generate_workload(wl_cfg);
    auto initial_state = generate_initial_state(cfg.accounts);

    // Print CSV header
    std::cout << "threads,block_size,total_accounts,time_ms,throughput\n";

    for (int t : cfg.threads) {
        std::vector<double> times;
        times.reserve(cfg.runs);

        for (int r = 0; r < cfg.runs; ++r) {
            if (t == 1) {
                // Sequential baseline
                times.push_back(measure_sequential_ms(block, initial_state));
            } else {
                times.push_back(measure_parallel_ms(block, initial_state, t));
            }
        }

        double med_ms = median(times);
        double tps = (cfg.block_size / med_ms) * 1000.0;

        std::cout << t << ","
                  << cfg.block_size << ","
                  << cfg.accounts << ","
                  << med_ms << ","
                  << tps << "\n";
    }

    return 0;
}
