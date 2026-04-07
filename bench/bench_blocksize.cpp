// bench_blocksize.cpp - Block size sweep benchmark
//
// Measures throughput across different block sizes.
// Larger blocks amortize scheduling overhead.
//
// Usage:
//   ./bench_blocksize [options]
//
// Options:
//   --threads      Thread count                       (default: 1)
//   --block-sizes  Comma-separated block sizes        (default: 100,500,1000,5000,10000,50000)
//   --accounts     Number of accounts                 (default: 1000)
//   --seed         Random seed                        (default: 42)
//   --runs         Repetitions per config (median)    (default: 5)
//

#include "transaction.h"
#include "workload.h"
#include "sequential.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static std::vector<size_t> parse_size_list(const std::string& s) {
    std::vector<size_t> result;
    std::istringstream stream(s);
    std::string token;
    while (std::getline(stream, token, ',')) {
        result.push_back(std::stoull(token));
    }
    return result;
}

struct BenchConfig {
    int threads = 1;
    std::vector<size_t> block_sizes = {100, 500, 1000, 5000, 10000, 50000};
    size_t accounts = 1000;
    uint64_t seed = 42;
    int runs = 5;
};

static BenchConfig parse_args(int argc, char* argv[]) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc)      cfg.threads     = std::stoi(argv[++i]);
        else if (arg == "--block-sizes" && i + 1 < argc) cfg.block_sizes = parse_size_list(argv[++i]);
        else if (arg == "--accounts" && i + 1 < argc)    cfg.accounts    = std::stoull(argv[++i]);
        else if (arg == "--seed" && i + 1 < argc)        cfg.seed        = std::stoull(argv[++i]);
        else if (arg == "--runs" && i + 1 < argc)        cfg.runs        = std::stoi(argv[++i]);
        else if (arg == "--help") {
            std::cout << "Usage: bench_blocksize [--threads N] [--block-sizes 100,1000,10000] "
                         "[--accounts N] [--seed N] [--runs N]\n";
            std::exit(0);
        }
    }
    return cfg;
}

static double measure_sequential_ms(
    const std::vector<Transaction>& block,
    const std::unordered_map<Key, Value>& state
) {
    auto start = std::chrono::steady_clock::now();
    auto result = sequential_execute(block, state);
    auto end = std::chrono::steady_clock::now();
    if (result.empty()) std::abort();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static double median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 0) return (v[n / 2 - 1] + v[n / 2]) / 2.0;
    return v[n / 2];
}

int main(int argc, char* argv[]) {
    auto cfg = parse_args(argc, argv);

    std::cout << "threads,block_size,total_accounts,time_ms,throughput\n";

    for (size_t bs : cfg.block_sizes) {
        WorkloadConfig wl_cfg{
            .num_txs = bs,
            .num_accounts = cfg.accounts,
            .seed = cfg.seed
        };
        auto block = generate_workload(wl_cfg);
        auto state = generate_initial_state(cfg.accounts);

        std::vector<double> times;
        times.reserve(cfg.runs);

        for (int r = 0; r < cfg.runs; ++r) {
            times.push_back(measure_sequential_ms(block, state));
        }

        double med_ms = median(times);
        double tps = (bs / med_ms) * 1000.0;

        std::cout << cfg.threads << ","
                  << bs << ","
                  << cfg.accounts << ","
                  << med_ms << ","
                  << tps << "\n";
    }

    return 0;
}
