#pragma once

// executor.h - Algorithm 1 (thread loop) + Algorithm 3 (VM execution).
// ParallelContext intercepts tx.logic reads/writes: reads go through MVMemory
// with local write-set check; writes are buffered and only flushed to shared
// memory at the end via MVMemory.record().

#include "transaction.h"
#include "mvmemory.h"
#include "scheduler.h"

#include <vector>
#include <unordered_map>
#include <optional>
#include <thread>

// context = 「你在什麼環境下操作」 決定了同一段邏輯在不同環境下的行為
class ParallelContext : public ExecutionContext {
    MVMemory& memory_;
    const std::unordered_map<Key, Value>& initial_state_;
    size_t txn_idx_;
    
    std::vector<ReadDescriptor> read_set_;
    std::vector<WriteDescriptor> write_set_;
    
    bool has_read_error_ = false;
    size_t blocking_txn_idx_ = 0;

public:
    ParallelContext(MVMemory& memory, const std::unordered_map<Key, Value>& initial_state, size_t txn_idx)
        : memory_(memory), initial_state_(initial_state), txn_idx_(txn_idx) {}

    Value read(Key key) override {
        // 1. Read-Your-Own-Writes (Check local write-set first)
        for (auto& wd : write_set_) {
            if (wd.location == key) {
                return wd.value;
            }
        }

        // 2. Read from shared multi-version memory
        ReadResult res = memory_.read(key, txn_idx_);

        if (res.status == ReadStatus::READ_ERROR) {
            // Encountered an ESTIMATE marker from an aborted transaction
            has_read_error_ = true;
            blocking_txn_idx_ = res.blocking_txn_idx;
            return 0; // Dummy value, execution will be aborted by Executor anyway
        }

        if (res.status == ReadStatus::OK) {
            // Valid version found in MVMemory
            read_set_.push_back({key, res.version});
            return res.value;
        }

        // 3. Read from Storage (NOT_FOUND in MVMemory means no lower txn wrote to it)
        read_set_.push_back({key, std::nullopt});
        auto it = initial_state_.find(key);
        if (it != initial_state_.end()) {
            return it->second;
        }
        
        return 0; // Fallback (should not happen if workload only accesses valid keys)
    }

    void write(Key key, Value value) override {
        // Buffer writes locally. Overwrite if already exists, otherwise append.
        for (auto& wd : write_set_) {
            if (wd.location == key) {
                wd.value = value;
                return;
            }
        }
        write_set_.push_back({key, value});
    }

    bool has_error() const { return has_read_error_; }
    size_t get_blocking_txn_idx() const { return blocking_txn_idx_; }
    
    // We use std::move to avoid copying these vectors into MVMemory during record()
    std::vector<ReadDescriptor> take_read_set() { return std::move(read_set_); }
    std::vector<WriteDescriptor> take_write_set() { return std::move(write_set_); }
};

class Executor {
    Scheduler& scheduler_;
    MVMemory& memory_;
    const std::vector<Transaction>& block_;
    const std::unordered_map<Key, Value>& initial_state_;

public:
    Executor(Scheduler& scheduler,
             MVMemory& memory,
             const std::vector<Transaction>& block,
             const std::unordered_map<Key, Value>& initial_state)
        : scheduler_(scheduler), memory_(memory), block_(block), initial_state_(initial_state) {}

    // --- Algorithm 1: Thread Main Loop ---
    void run() {
        std::optional<Task> task = std::nullopt;
        
        // Threads loop until check_done() conditions are met
        while (!scheduler_.done()) {
            // 1. Ask scheduler for the next task if we don't have one
            if (!task) {
                task = scheduler_.next_task();
            }
            
            // 2. Perform the task
            if (task) {
                if (task->kind == TaskKind::EXECUTION_TASK) {
                    task = try_execute(task->version);
                } else {
                    task = needs_reexecution(task->version);
                }
            }
        }
    }

private:
    // --- Algorithm 3: Execution Task ---
    std::optional<Task> try_execute(Version version) {
        size_t txn_idx = version.txn_idx;
        
        ParallelContext ctx(memory_, initial_state_, txn_idx);
        
        // 1. Speculative Execution
        block_[txn_idx].logic(ctx);
        
        // 2. Handle read dependency (ESTIMATE encountered)
        if (ctx.has_error()) {
            if (!scheduler_.add_dependency(txn_idx, ctx.get_blocking_txn_idx())) {
                // Dependency was resolved while we were trying to add it!
                // Re-execute immediately without waiting.
                return Task{version, TaskKind::EXECUTION_TASK};
            }
            return std::nullopt; // Successfully suspended
        }
        
        // 3. Execution succeeded, record write-set to shared memory
        bool wrote_new_location = memory_.record(
            version, 
            ctx.take_read_set(), 
            ctx.take_write_set()
        );
        
        // 4. Update scheduler status and potentially get a validation task back
        return scheduler_.finish_execution(txn_idx, version.incarnation, wrote_new_location);
    }

    // --- Algorithm 3: Validation Task ---
    std::optional<Task> needs_reexecution(Version version) {
        size_t txn_idx = version.txn_idx;
        
        // 1. Validate the read-set
        bool read_set_valid = memory_.validate_read_set(txn_idx);
        bool aborted = false;
        
        // 2. If validation fails, try to abort it
        if (!read_set_valid) {
            aborted = scheduler_.try_validation_abort(txn_idx, version.incarnation);
            if (aborted) {
                // 3. Successful abort: mark writes as ESTIMATE to warn higher txns
                memory_.convert_writes_to_estimates(txn_idx);
            }
        }
        
        // 4. Update scheduler and potentially get a re-execution task back
        return scheduler_.finish_validation(txn_idx, aborted);
    }
};
