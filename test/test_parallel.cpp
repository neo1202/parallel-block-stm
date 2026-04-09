#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "blockstm.h"
#include "sequential.h"
#include "workload.h"

#include <chrono>
#include <iostream>

// Helper: compute total balance across all accounts
static Value total_balance(const std::unordered_map<Key, Value>& state) {
    Value sum = 0;
    for (const auto& [key, val] : state) {
        sum += val;
    }
    return sum;
}

TEST_CASE("Parallel vs Sequential: Single transaction") {
    std::unordered_map<Key, Value> initial = {{0, 100}, {1, 200}};

    Transaction tx;
    tx.read_keys  = {0, 1};
    tx.write_keys = {0, 1};
    tx.logic = [](ExecutionContext& ctx) {
        Value a = ctx.read(0);
        Value b = ctx.read(1);
        ctx.write(0, a - 1);
        ctx.write(1, b + 1);
    };

    auto seq_result = sequential_execute({tx}, initial);
    auto par_result = parallel_execute({tx}, initial, 4);

    CHECK(par_result == seq_result);
    CHECK(par_result[0] == 99);
    CHECK(par_result[1] == 201);
}

TEST_CASE("Parallel vs Sequential: Small workload") {
    WorkloadConfig config{.num_txs = 100, .num_accounts = 20, .seed = 42};
    auto initial_state = generate_initial_state(config.num_accounts, 1000);
    auto block = generate_workload(config);

    auto seq_result = sequential_execute(block, initial_state);
    auto par_result = parallel_execute(block, initial_state, 4);

    CHECK(par_result == seq_result);
    CHECK(total_balance(par_result) == total_balance(initial_state));
}

TEST_CASE("Parallel vs Sequential: High contention (2 accounts)") {
    WorkloadConfig config{.num_txs = 500, .num_accounts = 2, .seed = 123};
    auto initial_state = generate_initial_state(2, 10000);
    auto block = generate_workload(config);

    auto seq_result = sequential_execute(block, initial_state);
    auto par_result = parallel_execute(block, initial_state, 8);

    CHECK(par_result == seq_result);
    CHECK(total_balance(par_result) == total_balance(initial_state));
}

TEST_CASE("Parallel vs Sequential: Large block") {
    WorkloadConfig config{.num_txs = 2000, .num_accounts = 100, .seed = 777};
    auto initial_state = generate_initial_state(config.num_accounts, 1000);
    auto block = generate_workload(config);

    auto seq_result = sequential_execute(block, initial_state);
    
    // Test with different thread counts
    for (int threads : {1, 2, 4, 8, 16}) {
        auto par_result = parallel_execute(block, initial_state, threads);
        CHECK(par_result == seq_result);
    }
}

TEST_CASE("Parallel vs Sequential: Multiple blocks stress test") {
    constexpr size_t NUM_BLOCKS = 50;
    constexpr size_t TXS_PER_BLOCK = 128;
    constexpr size_t NUM_ACCOUNTS = 50;
    
    auto initial_state = generate_initial_state(NUM_ACCOUNTS, 1000);
    
    for (size_t i = 0; i < NUM_BLOCKS; ++i) {
        WorkloadConfig config{
            .num_txs = TXS_PER_BLOCK,
            .num_accounts = NUM_ACCOUNTS,
            .seed = i
        };
        auto block = generate_workload(config);
        
        auto seq_result = sequential_execute(block, initial_state);
        auto par_result = parallel_execute(block, initial_state, 8);
        
        if (par_result != seq_result) {
            FAIL("Parallel result mismatch at block " << i);
        }
    }
}
