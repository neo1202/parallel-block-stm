#pragma once

// blockstm.h - Top-Level Block-STM Engine
//
// The public entry point. Users call parallel_execute() with a block of
// transactions, initial state, and thread count. Everything else is internal.
//
// WHAT IT DOES:
//   1. Create MVMemory (shared multi-version store for this block)
//   2. Create Scheduler (task dispatcher for this block)
//   3. Spawn N threads, each running the executor's run() loop
//   4. Wait for all threads to join (Scheduler.done() triggers exit)
//   5. Call MVMemory.snapshot() to collect final state
//   6. Return final state (must == sequential_execute result)
//
// LIFETIME:
//   One block = one MVMemory + one Scheduler + N threads.
//   All created at the start of parallel_execute(), all destroyed at the end.
//   The next block starts fresh.
//
// PUBLIC API:
//   std::unordered_map<Key, Value> parallel_execute(
//       const std::vector<Transaction>& block,
//       const std::unordered_map<Key, Value>& initial_state,
//       int num_threads
//   );
//

#include "transaction.h"
#include "mvmemory.h"
#include "scheduler.h"
#include "executor.h"

#include <vector>
#include <unordered_map>
#include <thread>

// --- parallel_execute ---
// Executes a block of transactions concurrently using the Block-STM algorithm.
// Returns a new state map reflecting all applied changes.
inline std::unordered_map<Key, Value> parallel_execute(
    const std::vector<Transaction>& block,
    const std::unordered_map<Key, Value>& initial_state,
    int num_threads
) {
    if (block.empty()) {
        return initial_state;
    }

    size_t block_size = block.size();

    // 1. Initialize shared structures for this block
    MVMemory memory(block_size, initial_state);
    Scheduler scheduler(block_size);

    // 2. Spawn worker threads
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&scheduler, &memory, &block, &initial_state]() {
            Executor executor(scheduler, memory, block, initial_state);
            executor.run();
        });
    }

    // 3. Wait for the scheduler to mark done() and threads to exit
    for (auto& t : threads) {
        t.join();
    }

    // 4. Collect final state
    // Start with a copy of the initial state, as transactions may not write to all keys
    std::unordered_map<Key, Value> final_state = initial_state;
    
    // Merge the snapshot of written values from MVMemory
    auto snapshot = memory.snapshot();
    for (const auto& [k, v] : snapshot) {
        final_state[k] = v;
    }

    return final_state;
}
