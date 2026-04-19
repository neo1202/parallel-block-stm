#pragma once

// workload.h - synthetic block generator. Contention controlled by
// num_accounts; hot/cold is optional (DEX-style). All txs are +1/-1
// transfers plus optional compute padding via iterated SHA-256.

#include "transaction.h"
#include "picosha2.h"

#include <array>
#include <cstring>
#include <random>
#include <unordered_map>

struct WorkloadConfig {
    size_t num_txs;                // number of transactions in the block
    size_t num_accounts;           // number of accounts (fewer = more contention)
    uint64_t seed;                 // random seed for reproducibility
    size_t reads_per_tx  = 2;     // number of keys each tx reads
    size_t writes_per_tx = 2;     // number of keys each tx writes (<= reads_per_tx)
    size_t compute_iters = 0;     // extra arithmetic iterations to simulate compute cost

    // hot/cold workload - simulates DEX-style access pattern
    // hot_tx_ratio fraction of txs are "hot": first hot_keys_per_tx of their keys
    // come from the hot pool (IDs 0..hot_accounts-1), the rest come from cold.
    // the remaining txs use only cold accounts.
    // hot_tx_ratio=0 disables this (default uniform pick).
    double hot_tx_ratio    = 0.0;
    size_t hot_accounts    = 50;
    size_t hot_keys_per_tx = 1;
};

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

// pick `count` distinct keys: first `n_hot` from hot pool, rest from cold pool.
// if n_hot == 0, everything is cold.
inline std::vector<Key> pick_keys_hot_cold_tx(
    size_t count, size_t num_accounts, size_t hot_accounts,
    size_t n_hot, std::mt19937_64& rng
) {
    std::uniform_int_distribution<Key> hot_dist(0, static_cast<Key>(hot_accounts - 1));
    std::uniform_int_distribution<Key> cold_dist(
        static_cast<Key>(hot_accounts), static_cast<Key>(num_accounts - 1));

    std::vector<Key> keys;
    keys.reserve(count);

    auto push_unique = [&](Key k) {
        for (Key existing : keys) if (existing == k) return false;
        keys.push_back(k);
        return true;
    };

    // hot keys first
    while (keys.size() < n_hot) {
        push_unique(hot_dist(rng));
    }
    // then fill with cold keys
    while (keys.size() < count) {
        push_unique(cold_dist(rng));
    }
    return keys;
}

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

    // hot/cold kicks in only when the pool sizes and ratio make sense
    bool use_hotcold = (config.hot_tx_ratio > 0.0)
                    && (config.hot_accounts > 0)
                    && (config.hot_accounts < config.num_accounts);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    size_t hot_keys = std::min(config.hot_keys_per_tx, reads);

    for (size_t i = 0; i < config.num_txs; ++i) {
        std::vector<Key> keys;
        if (use_hotcold && coin(rng) < config.hot_tx_ratio) {
            // this one is a hot tx -> first hot_keys come from hot pool
            keys = pick_keys_hot_cold_tx(reads, config.num_accounts,
                                         config.hot_accounts, hot_keys, rng);
        } else if (use_hotcold) {
            // cold tx -> entirely from cold pool (n_hot = 0)
            keys = pick_keys_hot_cold_tx(reads, config.num_accounts,
                                         config.hot_accounts, 0, rng);
        } else {
            keys = pick_distinct_keys(reads, config.num_accounts, rng);
        }

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
