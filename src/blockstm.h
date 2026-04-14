#pragma once

// blockstm.h - entry point. One block = one MVMemory + one Scheduler + N
// threads spawned, joined, then snapshot() gives the final state.
//
// TODO(neo): 改成thread pool? 不要每次block都銷毀thread
// * 很多thread一起處理一個block, 他們改同一份MVMemory有很多版本
// * snapshot時得到此block改動過後那些key最終的值, 然後寫進global state以後回傳
// * 這是一個sequential bottlenect, 因為下一個block得看到所有key最新的狀態

#include "transaction.h"
#include "mvmemory.h"
#include "scheduler.h"
#include "executor.h"

#include <vector>
#include <unordered_map>
#include <thread>

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
