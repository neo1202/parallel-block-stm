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

// aggregate stats for the whole block. derive abort rate etc. from these.
struct BlockStats {
    uint64_t validation_aborts = 0;
    uint64_t dependency_suspends = 0;
    uint64_t total_executions = 0;   // includes re-executions
};

inline std::unordered_map<Key, Value> parallel_execute(
    const std::vector<Transaction>& block,
    const std::unordered_map<Key, Value>& initial_state,
    int num_threads,
    BlockStats* out_stats = nullptr   // optional, nullptr -> don't collect
) {
    if (block.empty()) {
        if (out_stats) *out_stats = {};
        return initial_state;
    }

    size_t block_size = block.size();

    MVMemory memory(block_size, initial_state);
    Scheduler scheduler(block_size);

    // one ExecStats per worker. each worker writes only to its own slot for lower contention
    std::vector<ExecStats> per_thread_stats;
    if (out_stats) per_thread_stats.resize(num_threads);

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        ExecStats* slot = out_stats ? &per_thread_stats[i] : nullptr;
        threads.emplace_back([&scheduler, &memory, &block, &initial_state, slot]() {
            Executor executor(scheduler, memory, block, initial_state, slot);
            executor.run();
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // sum per-thread stats if requested
    if (out_stats) {
        *out_stats = {};
        for (const auto& s : per_thread_stats) {
            out_stats->validation_aborts   += s.validation_aborts;
            out_stats->dependency_suspends += s.dependency_suspends;
            out_stats->total_executions    += s.total_executions;
        }
    }

    // final state = initial + whatever MVMemory's snapshot overwrote
    std::unordered_map<Key, Value> final_state = initial_state;
    auto snapshot = memory.snapshot();
    for (const auto& [k, v] : snapshot) {
        final_state[k] = v;
    }

    return final_state;
}
