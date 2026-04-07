#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "transaction.h"
#include "workload.h"
#include "sequential.h"

#include <chrono>
#include <numeric>
#include <iostream>

// Helper: compute total balance across all accounts
static Value total_balance(const std::unordered_map<Key, Value>& state) {
    Value sum = 0;
    for (const auto& [key, val] : state) {
        sum += val;
    }
    return sum;
}

// Test: single hand-crafted transaction
TEST_CASE("sequential: single transaction correctness") {
    // Setup: two accounts, A=100, B=200
    std::unordered_map<Key, Value> initial = {{0, 100}, {1, 200}};

    // One transaction: transfer 1 from account 0 to account 1
    Transaction tx;
    tx.read_keys  = {0, 1};
    tx.write_keys = {0, 1};
    tx.logic = [](ExecutionContext& ctx) {
        Value a = ctx.read(0);
        Value b = ctx.read(1);
        ctx.write(0, a - 1);
        ctx.write(1, b + 1);
    };

    auto result = sequential_execute({tx}, initial);

    CHECK(result[0] == 99);
    CHECK(result[1] == 201);
    CHECK(total_balance(result) == 300);
}

// 同一個 seed 跑兩次，結果必須完全一樣
// Test: deterministic - same seed produces identical results
TEST_CASE("sequential: deterministic with same seed") {
    WorkloadConfig config{.num_txs = 200, .num_accounts = 50, .seed = 12345};
    auto state = generate_initial_state(config.num_accounts);
    auto block = generate_workload(config);

    auto result1 = sequential_execute(block, state);
    auto result2 = sequential_execute(block, state);

    CHECK(result1 == result2);
}

// Test: different seeds give different results (sanity check)
TEST_CASE("sequential: different seeds differ") {
    auto state = generate_initial_state(50);

    WorkloadConfig config_a{.num_txs = 200, .num_accounts = 50, .seed = 111};
    WorkloadConfig config_b{.num_txs = 200, .num_accounts = 50, .seed = 222};

    auto result_a = sequential_execute(generate_workload(config_a), state);
    auto result_b = sequential_execute(generate_workload(config_b), state);

    CHECK(result_a != result_b);
}

// Test: total balance is conserved (transfer amount = 1, zero-sum)
TEST_CASE("sequential: balance conservation") {
    constexpr size_t NUM_ACCOUNTS = 100;
    constexpr Value  INIT_BALANCE = 1000;

    auto state = generate_initial_state(NUM_ACCOUNTS, INIT_BALANCE);
    Value expected_total = NUM_ACCOUNTS * INIT_BALANCE;

    CHECK(total_balance(state) == expected_total);

    // Run with multiple seeds and contention levels
    for (uint64_t seed = 0; seed < 10; ++seed) {
        for (size_t accounts : {2, 10, 50, 100}) {
            WorkloadConfig config{
                .num_txs = 500,
                .num_accounts = accounts,
                .seed = seed
            };
            auto init = generate_initial_state(accounts, INIT_BALANCE);
            auto result = sequential_execute(generate_workload(config), init);

            CHECK(total_balance(result) == accounts * INIT_BALANCE);
        }
    }
}
// 只有 2 個帳戶、10000 筆交易（最極端的 contention），確認不 crash 且 balance 守恆
// Test: high contention (2 accounts) doesn't crash
TEST_CASE("sequential: high contention stress test") {
    // 2 accounts = every transaction conflicts with every other
    WorkloadConfig config{.num_txs = 10000, .num_accounts = 2, .seed = 42};
    auto state = generate_initial_state(2, 1000000);
    auto result = sequential_execute(generate_workload(config), state);

    // Should not crash, and balance must be conserved
    CHECK(total_balance(result) == 2 * 1000000);
}

// * 跑 1000 個 block 各 32 筆交易，驗證全部正確，順便計時
// Test: bulk run - 1000 blocks × 32 txs each, with timing
// Uses std::chrono::steady_clock (monotonic, not affected by NTP / system
// time changes - this is the "wall clock that doesn't lie" you want).
TEST_CASE("sequential: bulk 1000 blocks with timing") {
    constexpr size_t NUM_BLOCKS    = 1000;
    constexpr size_t TXS_PER_BLOCK = 32;
    constexpr size_t NUM_ACCOUNTS  = 100;
    constexpr Value  INIT_BALANCE  = 1000;

    auto initial_state = generate_initial_state(NUM_ACCOUNTS, INIT_BALANCE);
    Value expected_total = NUM_ACCOUNTS * INIT_BALANCE;

    // Pre-generate all blocks before timing
    std::vector<std::vector<Transaction>> blocks;
    blocks.reserve(NUM_BLOCKS);
    for (size_t i = 0; i < NUM_BLOCKS; ++i) {
        WorkloadConfig config{
            .num_txs = TXS_PER_BLOCK,
            .num_accounts = NUM_ACCOUNTS,
            .seed = i
        };
        blocks.push_back(generate_workload(config));
    }

    // === Timing: only execution, no verification ===
    auto start = std::chrono::steady_clock::now();

    std::vector<std::unordered_map<Key, Value>> results;
    results.reserve(NUM_BLOCKS);
    for (size_t i = 0; i < NUM_BLOCKS; ++i) {
        results.push_back(sequential_execute(blocks[i], initial_state));
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // === Verification: after timing ===
    for (size_t i = 0; i < NUM_BLOCKS; ++i) {
        CHECK(total_balance(results[i]) == expected_total);
    }

    std::cout << "\n  [bulk timing] " << NUM_BLOCKS << " blocks x "
              << TXS_PER_BLOCK << " txs = " << (NUM_BLOCKS * TXS_PER_BLOCK)
              << " total txs in " << ms << " ms\n";
}
