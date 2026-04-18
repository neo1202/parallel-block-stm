#pragma once

// workload.h - Synthetic workload generator with tunable contention
//
// Generates blocks of transactions for testing and benchmarking.
// Supports configurable read/write ratios to simulate different workload
// patterns from the Block-STM paper (Section 4.1):
//
//   P2P transfer (default):  2 reads, 2 writes - baseline, like Aptos transfers
//   Read-heavy (DEX-like):   8 reads, 2 writes - price lookups with few updates
//   Write-heavy (batch):     2 reads, 8 writes - bulk state mutations
//   Compute-heavy:           + compute_iters    - simulate VM execution cost
//
// Contention is controlled by num_accounts (fewer = more conflicts).
// All workloads conserve total balance (zero-sum transfers).
//
// DESIGN DECISION: Fixed seed for reproducibility.
//   Every generated workload is fully determined by the seed. If a test fails,
//   you can reproduce the exact same block by reusing the seed.
//

#include "transaction.h"
#include "picosha2.h"

#include <array>
#include <cstring>
#include <random>
#include <unordered_map>

// --- WorkloadConfig ---
// Parameters for workload generation.
//
// Workload patterns (matching Block-STM paper Section 4.1):
//   P2P transfer:  reads_per_tx=2, writes_per_tx=2  (default)
//   Read-heavy:    reads_per_tx=8, writes_per_tx=2   (DEX price queries)
//   Write-heavy:   reads_per_tx=2, writes_per_tx=8   (batch updates)
//   Compute-heavy: reads_per_tx=2, writes_per_tx=2, compute_iters=1000
struct WorkloadConfig {
    size_t num_txs;                // number of transactions in the block
    size_t num_accounts;           // number of accounts (fewer = more contention)
    uint64_t seed;                 // random seed for reproducibility
    size_t reads_per_tx  = 2;     // number of keys each tx reads
    size_t writes_per_tx = 2;     // number of keys each tx writes (<= reads_per_tx)
    size_t compute_iters = 0;     // extra arithmetic iterations to simulate compute cost
};

// --- generate_initial_state ---
// Creates the initial state: each account starts with the given balance.
// Returns: { 0: balance, 1: balance, ..., (num_accounts-1): balance }
inline std::unordered_map<Key, Value> generate_initial_state(
    size_t num_accounts,
    Value initial_balance = 1000
) {
    std::unordered_map<Key, Value> state;
    state.reserve(num_accounts);
    for (size_t i = 0; i < num_accounts; ++i) {
        state[static_cast<Key>(i)] = initial_balance;
    }
    return state;
}

// --- pick_distinct_keys ---
// Helper: pick `count` distinct random keys from [0, num_accounts).
inline std::vector<Key> pick_distinct_keys(
    size_t count, size_t num_accounts, std::mt19937_64& rng
) {
    std::uniform_int_distribution<Key> dist(0, static_cast<Key>(num_accounts - 1));
    std::vector<Key> keys;
    keys.reserve(count);
    while (keys.size() < count) {
        Key k = dist(rng);
        bool dup = false;
        for (Key existing : keys) {
            if (existing == k) { dup = true; break; }
        }
        if (!dup) keys.push_back(k);
    }
    return keys;
}

// --- generate_workload ---
// Generates a block of transactions with configurable read/write counts.
//
// Default (reads_per_tx=2, writes_per_tx=2): p2p transfer, same as paper.
// Read-heavy (reads_per_tx=8, writes_per_tx=2): reads extra accounts,
//   writes only the first two (transfer between them).
// Write-heavy (reads_per_tx=2, writes_per_tx=8): reads two accounts,
//   writes to multiple accounts (distributes value).
//
// Balance conservation: the first two keys always do a +1/-1 transfer.
// Extra writes distribute zero-sum across remaining write keys.
// Extra reads are read-only (no write to those keys).
//
// compute_iters: adds dummy arithmetic to simulate VM compute cost.
//   Loop count is fixed at generation time (not data-dependent).
inline std::vector<Transaction> generate_workload(const WorkloadConfig& config) {
    std::mt19937_64 rng(config.seed);

    // writes_per_tx must not exceed reads_per_tx (every written key is also read)
    size_t writes = std::min(config.writes_per_tx, config.reads_per_tx);
    size_t reads  = std::max(config.reads_per_tx, writes);
    // Need at least 2 keys for a transfer
    reads  = std::max(reads,  size_t(2));
    writes = std::max(writes, size_t(2));

    std::vector<Transaction> block;
    block.reserve(config.num_txs);

    for (size_t i = 0; i < config.num_txs; ++i) {
        // Pick `reads` distinct accounts. First `writes` of them are also written.
        auto keys = pick_distinct_keys(reads, config.num_accounts, rng);

        Transaction tx;
        tx.read_keys = keys;
        tx.write_keys = std::vector<Key>(keys.begin(), keys.begin() + writes);

        size_t iters = config.compute_iters;

        // Capture keys and iters by value.
        tx.logic = [keys, writes, iters](ExecutionContext& ctx) {
            // Read all keys
            std::vector<Value> vals;
            vals.reserve(keys.size());
            for (Key k : keys) {
                vals.push_back(ctx.read(k));
            }

            // Simulated VM compute: iterated SHA-256 over the read values.
            // Models the CPU cost of Move VM bytecode execution, signature
            // verification, and Merkle hashing in real blockchain transactions.
            // Input depends on tx read-set so the compiler cannot optimize away.
            if (iters > 0) {
                std::array<uint8_t, 32> buf{};
                Value v0 = vals[0];
                Value v1 = vals[1];
                std::memcpy(buf.data(),     &v0, sizeof(Value));
                std::memcpy(buf.data() + 8, &v1, sizeof(Value));
                std::array<uint8_t, 32> out{};
                for (size_t j = 0; j < iters; ++j) {
                    picosha2::hash256(buf.begin(), buf.end(),
                                      out.begin(), out.end());
                    buf = out;
                }
                // Force the hash result to be observable so the loop can't be dropped.
                volatile uint64_t sink = 0;
                std::memcpy((void*)&sink, buf.data(), sizeof(uint64_t));
                (void)sink;
            }

            // Write: first two keys do a +1/-1 transfer (balance conserving)
            ctx.write(keys[0], vals[0] - 1);
            ctx.write(keys[1], vals[1] + 1);

            // Extra writes (index 2..writes-1): read-back same value (no net change)
            for (size_t w = 2; w < writes; ++w) {
                ctx.write(keys[w], vals[w]);
            }
        };

        block.push_back(std::move(tx));
    }

    return block;
}
