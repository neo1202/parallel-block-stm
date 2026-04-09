#pragma once

// scheduler.h - Collaborative Scheduler (Algorithm 4 + 5)
//
// Coordinates execution and validation tasks among threads using shared
// atomic counters. This is what makes Block-STM "collaborative" - threads
// don't own specific transactions, they grab whatever task is next.
//
// CORE IDEA:
//   Two atomic counters (execution_idx, validation_idx) act as ordered sets
//   of pending tasks. Threads fetch-and-increment to claim the next task,
//   and decrease the counter to add new tasks (e.g., after an abort).
//   Validation tasks are prioritized over execution (lower idx first).
//
// KEY STATE (per transaction):
//   txn_status[txn_idx] = (incarnation_number, status)
//     status in {READY_TO_EXECUTE, EXECUTING, EXECUTED, ABORTING}
//     Protected by mutex. See status state machine in README.md.
//
//   txn_dependency[txn_idx] = set of dependent transaction indices
//     When tx_k reads an ESTIMATE from tx_j, k is added as j's dependency.
//     When tx_j finishes re-execution, all its dependents are resumed.
//
// KEY OPERATIONS (Algorithm 4):
//   next_task()         - pick the next execution or validation task
//   try_incarnate()     - transition READY_TO_EXECUTE -> EXECUTING
//   check_done()        - detect when all work is complete
//
// KEY OPERATIONS (Algorithm 5):
//   add_dependency()    - record that tx_k depends on tx_j (ESTIMATE read)
//   finish_execution()  - transition EXECUTING -> EXECUTED, resume deps
//   try_validation_abort() - transition EXECUTED -> ABORTING (first wins)
//   finish_validation() - schedule re-execution after abort
//
// CACHE PADDING:
//   execution_idx, validation_idx, num_active_tasks, decrease_cnt must
//   each sit on separate cache lines (alignas(64)) to avoid false sharing
//   between threads.
//

#include "mvmemory.h"  // for Version

#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

// Cache-line padding to prevent false sharing (Section 4)
template<typename T>
struct alignas(64) CachePadded {
    T value;
    CachePadded() : value{} {}
    explicit CachePadded(T v) : value(v) {}
};

// Task types returned by Scheduler to threads
enum class TaskKind {
    EXECUTION_TASK,
    VALIDATION_TASK
};

struct Task {
    Version version;
    TaskKind kind;
};

// Transaction status (per-transaction, mutex-protected)
// State machine: READY_TO_EXECUTE(i) -> EXECUTING(i) -> EXECUTED(i)
//                     ↑                                    ↓
//                READY_TO_EXECUTE(i+1) <- ABORTING(i) <-────┘
enum class TxnStatus {
    READY_TO_EXECUTE,
    EXECUTING,
    EXECUTED,
    ABORTING
};

struct TxnStatusEntry {
    std::mutex mtx;
    size_t incarnation = 0;
    TxnStatus status = TxnStatus::READY_TO_EXECUTE;
};

// Per-transaction dependency set (mutex-protected)
struct TxnDependency {
    std::mutex mtx;
    std::unordered_set<size_t> dependents;  // txn indices that depend on this tx
};

// RAII TaskGuard for num_active_tasks (Section 4)
// Increments on construction, decrements on destruction or release().
// Prevents check_done() from seeing 0 active tasks while work is in progress.
struct TaskGuard {
    std::atomic<int>& counter;
    bool active = true;

    TaskGuard(std::atomic<int>& c) : counter(c) { counter.fetch_add(1, std::memory_order_acq_rel); }
    void release() {
        if (active) {
            counter.fetch_sub(1, std::memory_order_acq_rel);
            active = false;
        }
    }
    ~TaskGuard() { release(); }

    // Non-copyable, non-movable
    TaskGuard(const TaskGuard&) = delete;
    TaskGuard& operator=(const TaskGuard&) = delete;
};

// Scheduler (Algorithm 4 + 5)
class Scheduler {
    size_t block_size_;

    // --- Atomic counters (each on its own cache line) ---
    CachePadded<std::atomic<int>> execution_idx_;
    CachePadded<std::atomic<int>> validation_idx_;
    CachePadded<std::atomic<int>> decrease_cnt_;
    CachePadded<std::atomic<int>> num_active_tasks_;
    std::atomic<bool> done_marker_{false};

    // --- Per-transaction state ---
    std::vector<std::unique_ptr<TxnStatusEntry>> txn_status_; // 每個txn到哪個incarnation了, 還有當下的執行狀態是executed還是...
    std::vector<std::unique_ptr<TxnDependency>> txn_dependency_;

public:
    explicit Scheduler(size_t block_size)
        : block_size_(block_size)
    {
        execution_idx_.value.store(0, std::memory_order_relaxed);
        validation_idx_.value.store(0, std::memory_order_relaxed);
        decrease_cnt_.value.store(0, std::memory_order_relaxed);
        num_active_tasks_.value.store(0, std::memory_order_relaxed);

        txn_status_.reserve(block_size);
        txn_dependency_.reserve(block_size);
        // 為什麼 push_back 不直接開好
        // 因為 TxnStatusEntry 和 TxnDependency 裡面有 std::mutex，mutex 不能 be copy 也不能 be move
        // reserve() 已經一次配好記憶體了，push_back 只是填入指標，不會 reallocate。效能跟「直接開好」一樣
        for (size_t i = 0; i < block_size; ++i) {
            txn_status_.push_back(std::make_unique<TxnStatusEntry>());
            txn_dependency_.push_back(std::make_unique<TxnDependency>());
        }
    }

    // --- Algorithm 4, Line 101-102: done() ---
    bool done() const {
        return done_marker_.load(std::memory_order_acquire);
    }

    std::atomic<int>& get_num_active_tasks_ref() {
        return num_active_tasks_.value;
    }

    // --- Algorithm 4, Line 137-146: next_task() ---
    // thread 主迴圈呼叫，決定要拿執行任務還是驗證任務
    // Pick the next task to perform. Validation tasks are prioritized
    // (lower idx first). Returns nullopt if no task is available.
    std::optional<Task> next_task() {
        while (validation_idx_.value.load(std::memory_order_acquire) < static_cast<int>(block_size_) ||
               execution_idx_.value.load(std::memory_order_acquire) < static_cast<int>(block_size_)) {
            if (validation_idx_.value.load(std::memory_order_acquire)
                < execution_idx_.value.load(std::memory_order_acquire)) {
                // Line 139: try validation first
                auto ver = next_version_to_validate();
                if (ver.has_value()) {
                    return Task{*ver, TaskKind::VALIDATION_TASK};
                }
            } else {
                // Line 143: try execution
                auto ver = next_version_to_execute();
                if (ver.has_value()) {
                    return Task{*ver, TaskKind::EXECUTION_TASK};
                }
            }
        }
        check_done();
        return std::nullopt;
    }

    // --- Algorithm 5, Line 147-154: add_dependency() ---
    // 登記 tx_k 依賴 tx_j，等 tx_j 跑完再叫醒 tx_k
    // Record that txn_idx depends on blocking_txn_idx (read ESTIMATE).
    // Returns true if dependency was successfully added.
    // Returns false if blocking tx already EXECUTED (dependency resolved).
    // Caller (try_execute) should re-execute immediately if false.
    bool add_dependency(size_t txn_idx, size_t blocking_txn_idx) {
        // Line 148: lock the dependency set of the blocking tx
        auto& dep = *txn_dependency_[blocking_txn_idx];
        std::lock_guard<std::mutex> dep_lock(dep.mtx);

        // Line 149-150: check if blocking tx already finished
        {
            auto& blocking_status = *txn_status_[blocking_txn_idx];
            std::lock_guard<std::mutex> status_lock(blocking_status.mtx);
            if (blocking_status.status == TxnStatus::EXECUTED) {
                return false;  // dependency resolved, re-execute immediately
            }
        }

        // Line 151: set txn_idx status to ABORTING
        {
            auto& status = *txn_status_[txn_idx];
            std::lock_guard<std::mutex> status_lock(status.mtx);
            status.status = TxnStatus::ABORTING;
        }

        // Line 152: record the dependency
        dep.dependents.insert(txn_idx);

        // Line 153: decrement active tasks (this execution is suspended)
        num_active_tasks_.value.fetch_sub(1, std::memory_order_acq_rel);

        return true;
    }

    // --- Algorithm 5, Line 165-175: finish_execution() ---
    //
    // Called after try_execute() succeeds. Transitions EXECUTING -> EXECUTED,
    // resumes dependent transactions, and schedules validation.
    // Returns a validation task for the caller if appropriate, or nullopt.
    std::optional<Task> finish_execution(size_t txn_idx, size_t incarnation,
                                          bool wrote_new_location) {
        // Line 166: set status to EXECUTED
        {
            auto& status = *txn_status_[txn_idx];
            std::lock_guard<std::mutex> lock(status.mtx);
            status.status = TxnStatus::EXECUTED;
        }

        // Line 167: swap out dependent txn indices
        std::unordered_set<size_t> deps;
        {
            auto& dep = *txn_dependency_[txn_idx];
            std::lock_guard<std::mutex> lock(dep.mtx);
            deps.swap(dep.dependents);
        }

        // Line 168: resume all dependents
        resume_dependencies(deps);

        // Line 169-173: schedule validation
        // Conservative: always decrease validation_idx to txn_idx to ensure 
        // higher transactions are re-validated if we wrote anything.
        if (wrote_new_location) {
            decrease_validation_idx(static_cast<int>(txn_idx));
        } else {
            // If we didn't write anything new, we might still need to validate ourselves.
            // But if validation_idx was already passed us, we should return a task.
            if (validation_idx_.value.load(std::memory_order_acquire) > static_cast<int>(txn_idx)) {
                return Task{{txn_idx, incarnation}, TaskKind::VALIDATION_TASK};
            }
        }

        num_active_tasks_.value.fetch_sub(1, std::memory_order_acq_rel);
        return std::nullopt;
    }

    // --- Algorithm 5, Line 176-181: try_validation_abort() ---
    // Attempt to abort a transaction after validation failure.
    // Only the first thread to see (incarnation, EXECUTED) succeeds.
    // Returns true if this thread performed the abort.
    bool try_validation_abort(size_t txn_idx, size_t incarnation) {
        auto& entry = *txn_status_[txn_idx];
        std::lock_guard<std::mutex> lock(entry.mtx);
        // Line 178: check status is still (incarnation, EXECUTED)
        if (entry.incarnation == incarnation && entry.status == TxnStatus::EXECUTED) {
            // Line 179: transition to ABORTING
            entry.status = TxnStatus::ABORTING;
            return true;
        }
        return false;
    }

    // --- Algorithm 5, Line 182-191: finish_validation() ---
    //
    // Called after validation completes. If aborted, schedule re-execution
    // for this tx and re-validation for higher txs.
    // Returns an execution task for the caller if appropriate, or nullopt.
    std::optional<Task> finish_validation(size_t txn_idx, bool aborted) {
        if (aborted) {
            // Line 184: ABORTING -> READY_TO_EXECUTE(incarnation+1)
            set_ready_status(txn_idx);

            // Line 185: schedule validation for higher transactions
            decrease_validation_idx(static_cast<int>(txn_idx) + 1);

            // Line 186-189: schedule re-execution for this tx
            if (execution_idx_.value.load(std::memory_order_acquire)
                > static_cast<int>(txn_idx)) {
                auto ver = try_incarnate(static_cast<int>(txn_idx));
                if (ver.has_value()) {
                    return Task{*ver, TaskKind::EXECUTION_TASK};
                }
            }
        }

        num_active_tasks_.value.fetch_sub(1, std::memory_order_acq_rel);
        return std::nullopt;
    }

private:
    // --- Algorithm 4, Line 98-100: decrease_execution_idx() ---
    void decrease_execution_idx(int target_idx) {
        int cur = execution_idx_.value.load(std::memory_order_acquire);
        while (cur > target_idx) {
            if (execution_idx_.value.compare_exchange_weak(
                    cur, target_idx, std::memory_order_acq_rel)) {
                break;
            }
        }
        decrease_cnt_.value.fetch_add(1, std::memory_order_acq_rel);
    }

    // --- Algorithm 4, Line 103-105: decrease_validation_idx() ---
    void decrease_validation_idx(int target_idx) {
        int cur = validation_idx_.value.load(std::memory_order_acquire);
        while (cur > target_idx) {
            if (validation_idx_.value.compare_exchange_weak(
                    cur, target_idx, std::memory_order_acq_rel)) {
                break;
            }
        }
        decrease_cnt_.value.fetch_add(1, std::memory_order_acq_rel);
    }

    // --- Algorithm 4, Line 106-109: check_done() ---
    void check_done() {
        int observed_cnt = decrease_cnt_.value.load(std::memory_order_acquire);
        int block_sz = static_cast<int>(block_size_);

        // 1. Initial check: counters passed block end AND no active work
        if (execution_idx_.value.load(std::memory_order_acquire) < block_sz ||
            validation_idx_.value.load(std::memory_order_acquire) < block_sz ||
            num_active_tasks_.value.load(std::memory_order_acquire) != 0) {
            return;
        }

        // 2. Final confirmation: if decrease_cnt hasn't changed, we are truly done.
        if (observed_cnt == decrease_cnt_.value.load(std::memory_order_acquire)) {
            done_marker_.store(true, std::memory_order_release);
        }
    }

    // --- Algorithm 4, Line 110-117: try_incarnate() ---
    std::optional<Version> try_incarnate(int txn_idx) {
        if (txn_idx < static_cast<int>(block_size_)) {
            auto& entry = *txn_status_[txn_idx];
            std::lock_guard<std::mutex> lock(entry.mtx);
            if (entry.status == TxnStatus::READY_TO_EXECUTE) {
                entry.status = TxnStatus::EXECUTING;
                return Version{static_cast<size_t>(txn_idx), entry.incarnation};
            }
        }
        // Caller is responsible for decrementing num_active_tasks if this returns nullopt
        return std::nullopt;
    }

    // --- Algorithm 4, Line 118-124: next_version_to_execute() ---
    std::optional<Version> next_version_to_execute() {
        int block_sz = static_cast<int>(block_size_);
        if (execution_idx_.value.load(std::memory_order_acquire) >= block_sz) {
            return std::nullopt;
        }
        num_active_tasks_.value.fetch_add(1, std::memory_order_acq_rel);
        int idx = execution_idx_.value.fetch_add(1, std::memory_order_acq_rel);
        if (idx >= block_sz) {
            num_active_tasks_.value.fetch_sub(1, std::memory_order_acq_rel);
            return std::nullopt;
        }
        
        auto ver = try_incarnate(idx);
        if (!ver.has_value()) {
            num_active_tasks_.value.fetch_sub(1, std::memory_order_acq_rel);
        }
        return ver;
    }

    // --- Algorithm 4, Line 125-136: next_version_to_validate() ---
    std::optional<Version> next_version_to_validate() {
        int block_sz = static_cast<int>(block_size_);
        if (validation_idx_.value.load(std::memory_order_acquire) >= block_sz) {
            return std::nullopt;
        }
        num_active_tasks_.value.fetch_add(1, std::memory_order_acq_rel);
        int idx = validation_idx_.value.fetch_add(1, std::memory_order_acq_rel);
        if (idx < block_sz) {
            auto& entry = *txn_status_[idx];
            std::lock_guard<std::mutex> lock(entry.mtx);
            if (entry.status == TxnStatus::EXECUTED) {
                return Version{static_cast<size_t>(idx), entry.incarnation};
            }
        }
        num_active_tasks_.value.fetch_sub(1, std::memory_order_acq_rel);
        return std::nullopt;
    }

    // --- Algorithm 5, Line 155-158: set_ready_status() ---
    void set_ready_status(size_t txn_idx) {
        auto& entry = *txn_status_[txn_idx];
        std::lock_guard<std::mutex> lock(entry.mtx);
        entry.incarnation += 1;
        entry.status = TxnStatus::READY_TO_EXECUTE;
    }

    // --- Algorithm 5, Line 159-164: resume_dependencies() ---
    void resume_dependencies(const std::unordered_set<size_t>& deps) {
        if (deps.empty()) return;

        size_t min_dep = block_size_;
        for (size_t dep_idx : deps) {
            set_ready_status(dep_idx);
            min_dep = std::min(min_dep, dep_idx);
        }

        decrease_execution_idx(static_cast<int>(min_dep));
    }
};
