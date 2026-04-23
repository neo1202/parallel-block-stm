#pragma once

// scheduler.h - Algorithm 4 + 5 from the Block-STM paper.
// Collaborative: threads grab whichever task is next (validation preferred
// over execution when idx is lower). Two atomic counters drive everything.

#include "mvmemory.h"  // for Version

#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

// Cache-line padding against false sharing (paper section 4).
template<typename T>
struct alignas(64) CachePadded {
    T value;
    CachePadded() : value{} {}
    explicit CachePadded(T v) : value(v) {}
};

enum class TaskKind {
    EXECUTION_TASK,
    VALIDATION_TASK
};

struct Task {
    Version version;
    TaskKind kind;
};

// Per-tx status: (incarnation, status) packed into one 64-bit atomic so the
// transition READY -> EXECUTING -> EXECUTED -> ABORTING -> READY(i+1) can
// CAS both fields at once. alignas(64) is required. without it multiple
// TxnStatusEntry share a cache line and CAS traffic burst
enum class TxnStatus {
    READY_TO_EXECUTE,
    EXECUTING,
    EXECUTED,
    ABORTING
};

struct alignas(64) TxnStatusEntry {
    std::atomic<uint64_t> state;

    TxnStatusEntry() {
        state.store(pack(0, TxnStatus::READY_TO_EXECUTE), std::memory_order_relaxed);
    }

    static uint64_t pack(uint32_t incarnation, TxnStatus status) {
        return (static_cast<uint64_t>(incarnation) << 32) | static_cast<uint32_t>(status);
    }

    static std::pair<size_t, TxnStatus> unpack(uint64_t val) {
        return {static_cast<size_t>(val >> 32), static_cast<TxnStatus>(val & 0xFFFFFFFF)};
    }
};

// Per-tx dependency set
struct DepNode {
    size_t txn_idx;
    DepNode* next;
    DepNode(size_t idx) : txn_idx(idx), next(nullptr) {}
};

// A special marker to indicate the dependency list is closed (transaction EXECUTED).
static DepNode* const CLOSED_MARKER = reinterpret_cast<DepNode*>(static_cast<uintptr_t>(-1));

struct TxnDependency {
    std::atomic<DepNode*> head{nullptr};

    ~TxnDependency() {
        DepNode* cur = head.load(std::memory_order_relaxed);
        if (cur == CLOSED_MARKER) cur = nullptr;
        while (cur) {
            DepNode* next = cur->next;
            delete cur;
            cur = next;
        }
    }
};

// per-thread claim over a range of execution_idx. we batch the shared
// fetch_add so 128 threads aren't waiting for  one cache line every task.
#ifndef EXEC_BATCH
#define EXEC_BATCH 2
#endif

struct ExecClaim {
    int lo = 0;
    int hi = 0;
    static constexpr int BATCH = EXEC_BATCH;
};

// RAII guard for num_active_tasks. Keeps check_done() from firing while
// a thread is mid-work even if try_execute returns early.
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
        // 因為 TxnDependency 裡面有 std::mutex，mutex 不能 be copy 也不能 be move
        // reserve() 已經一次配好記憶體了，push_back 只是填入指標，不會 reallocate。效能跟「直接開好」一樣
        for (size_t i = 0; i < block_size; ++i) {
            txn_status_.push_back(std::make_unique<TxnStatusEntry>());
            txn_dependency_.push_back(std::make_unique<TxnDependency>());
        }
    }

    // Algorithm 4, Line 101-102
    bool done() const {
        return done_marker_.load(std::memory_order_acquire);
    }

    std::atomic<int>& get_num_active_tasks_ref() {
        return num_active_tasks_.value;
    }

    // convenience overload for tests (single-shot, no claim batching)
    std::optional<Task> next_task() {
        ExecClaim dummy{};
        return next_task(dummy);
    }

    // thread 主迴圈呼叫，決定要拿執行任務還是驗證任務
    // 手上 claim 還有 idx 沒處理 → 先做完我手上的（new）
    // 手上沒工作了、但 val_idx < exec_idx（表示有 EXECUTED 的 tx 等驗證）→ 驗證優先（paper 原邏輯）
    // 都沒上面兩個 → CAS 去拿新的一批 exec idx（refill）
    // claim is per-thread state for batched exec_idx fetch. the loop has three
    // branches instead of the paper's two. 如果有還沒做完的任務就先做完已領取的
    //
    // under extreme contention val<exec stays true forever . aborts keep pulling val_idx back, so without this
    // priority a thread with pending claim would never fall into the exec branch
    std::optional<Task> next_task(ExecClaim& claim) {
        while (validation_idx_.value.load(std::memory_order_acquire) < static_cast<int>(block_size_) ||
               execution_idx_.value.load(std::memory_order_acquire) < static_cast<int>(block_size_) ||
               claim.lo < claim.hi) {
            if (claim.lo < claim.hi) {
                auto ver = next_version_to_execute(claim);
                if (ver.has_value()) {
                    return Task{*ver, TaskKind::EXECUTION_TASK};
                }
            } else if (validation_idx_.value.load(std::memory_order_acquire)
                       < execution_idx_.value.load(std::memory_order_acquire)) {
                auto ver = next_version_to_validate();
                if (ver.has_value()) {
                    return Task{*ver, TaskKind::VALIDATION_TASK};
                }
            } else {
                auto ver = next_version_to_execute(claim);
                if (ver.has_value()) {
                    return Task{*ver, TaskKind::EXECUTION_TASK};
                }
            }
        }
        check_done();
        return std::nullopt;
    }

    // 登記 tx_k 依賴 tx_j，等 tx_j 跑完再叫醒 tx_k
    // Record that txn_idx depends on blocking_txn_idx (read ESTIMATE).
    // Returns true if dependency was successfully added.
    // Returns false if blocking tx already EXECUTED (dependency resolved).
    // Caller (try_execute) should re-execute immediately if false.
    bool add_dependency(size_t txn_idx, size_t blocking_txn_idx) {
        auto& dep = *txn_dependency_[blocking_txn_idx];

        // 1. Push first. If CLOSED, blocking already finished its incarnation,
        //    so the caller can just retry try_execute with the same version.not touch our own status
        auto* new_node = new DepNode(txn_idx);
        DepNode* old_head = dep.head.load(std::memory_order_acquire);
        do {
            if (old_head == CLOSED_MARKER) {
                delete new_node;
                return false;
            }
            new_node->next = old_head;
        } while (!dep.head.compare_exchange_weak(old_head, new_node,
                    std::memory_order_release, std::memory_order_acquire));

        // 2. Push succeeded. now mark ourselves ABORTING.
        //    CAS expects EXECUTING(inc). if it fails, resume_dependencies has
        //    already set us to READY+1. next_task will pick up the new incarnation later.
        {
            auto& entry = *txn_status_[txn_idx];
            uint64_t expected = entry.state.load(std::memory_order_relaxed);
            while (true) {
                auto [inc, st] = TxnStatusEntry::unpack(expected);
                if (st != TxnStatus::EXECUTING) break;  // resume beat us
                uint64_t desired = TxnStatusEntry::pack(inc, TxnStatus::ABORTING);
                if (entry.state.compare_exchange_weak(expected, desired,
                        std::memory_order_acq_rel)) break;
                // CAS failure reloads expected; loop again
            }
        }

        // 3. Suspended.
        num_active_tasks_.value.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }

    // Called after try_execute() succeeds. Transitions EXECUTING -> EXECUTED,
    // resumes dependent transactions, and schedules validation.
    // Returns a validation task for the caller if appropriate, or nullopt.
    std::optional<Task> finish_execution(size_t txn_idx, size_t incarnation,
                                          bool wrote_new_location) {
        // EXECUTING(inc) -> EXECUTED(inc). only the executor reaches this code
        // path, and the executor's status was set to EXECUTING by try_incarnate.
        // the strict CAS catches any state-machine violation (would be a bug).
        {
            auto& entry = *txn_status_[txn_idx];
            uint64_t expected = TxnStatusEntry::pack(
                static_cast<uint32_t>(incarnation), TxnStatus::EXECUTING);
            uint64_t desired = TxnStatusEntry::pack(
                static_cast<uint32_t>(incarnation), TxnStatus::EXECUTED);
            // if this fails the world is broken; keep going so we don't deadlock,
            // but the test suite would catch wrong final state.
            entry.state.compare_exchange_strong(expected, desired,
                std::memory_order_acq_rel);
        }
        DepNode* list = txn_dependency_[txn_idx]->head.exchange(CLOSED_MARKER, std::memory_order_acq_rel);
        resume_dependencies(list);
        if (validation_idx_.value.load(std::memory_order_acquire)
            > static_cast<int>(txn_idx)) {
            if (wrote_new_location) {
                decrease_validation_idx(static_cast<int>(txn_idx));
            } else {
                // return validation task directly to caller
                return Task{{txn_idx, incarnation}, TaskKind::VALIDATION_TASK};
            }
        }

        num_active_tasks_.value.fetch_sub(1, std::memory_order_acq_rel);
        return std::nullopt;
    }

    // Attempt to abort a transaction after validation failure.
    // Only the first thread to see (incarnation, EXECUTED) succeeds.
    // Returns true if this thread performed the abort.
    bool try_validation_abort(size_t txn_idx, size_t incarnation) {
        auto& entry = *txn_status_[txn_idx];
        uint64_t expected = TxnStatusEntry::pack(static_cast<uint32_t>(incarnation), TxnStatus::EXECUTED);
        uint64_t desired = TxnStatusEntry::pack(static_cast<uint32_t>(incarnation), TxnStatus::ABORTING);
        // !Only the first thread to see (incarnation, EXECUTED) succeeds
        return entry.state.compare_exchange_strong(expected, desired, std::memory_order_acq_rel);
    }

    // Called after validation completes. If aborted, schedule re-execution
    // for this tx and re-validation for higher txs.
    // Returns an execution task for the caller if appropriate, or nullopt.
    std::optional<Task> finish_validation(size_t txn_idx, bool aborted) {
        if (aborted) {
            set_ready_status(txn_idx);
            decrease_validation_idx(static_cast<int>(txn_idx) + 1);
            // schedule re-execution for this tx
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

    void check_done() {
        int observed_cnt = decrease_cnt_.value.load(std::memory_order_acquire);
        int block_sz = static_cast<int>(block_size_);

        // Initial check, counters passed block end AND no active work
        if (execution_idx_.value.load(std::memory_order_acquire) < block_sz ||
            validation_idx_.value.load(std::memory_order_acquire) < block_sz ||
            num_active_tasks_.value.load(std::memory_order_acquire) != 0) {
            return;
        }

        // Final confirmation, if decrease_cnt hasn't changed, we are truly done.
        if (observed_cnt == decrease_cnt_.value.load(std::memory_order_acquire)) {
            done_marker_.store(true, std::memory_order_release);
        }
    }

    std::optional<Version> try_incarnate(int txn_idx) {
        if (txn_idx < static_cast<int>(block_size_)) {
            auto& entry = *txn_status_[txn_idx];
            uint64_t expected = entry.state.load(std::memory_order_relaxed);
            while (true) {
                auto [inc, st] = TxnStatusEntry::unpack(expected);
                if (st == TxnStatus::READY_TO_EXECUTE) {
                    uint64_t desired = TxnStatusEntry::pack(inc, TxnStatus::EXECUTING);
                    if (entry.state.compare_exchange_weak(expected, desired, std::memory_order_acq_rel)) {
                        return Version{static_cast<size_t>(txn_idx), inc};
                    }
                } else {
                    break;
                }
            }
        }
        // !Caller is responsible for decrementing num_active_tasks if this returns nullopt
        return std::nullopt;
    }

    // batched: claim [lo, hi) chunks via CAS clamped to block_size, then walk
    // through them locally. one shared-counter touch per BATCH instead of per task.
    //
    // num_active_tasks accounting: on refill we reserve (hi - lo) slots. each
    // consumed idx gives one slot back - either via the caller's finish_* (on
    // success) or via local -- (on try_incarnate fail). total balance is zero
    // per refill. keeping slots reserved while claim is non-empty prevents
    // check_done() from racing ahead while the thread still has pending work.
    std::optional<Version> next_version_to_execute(ExecClaim& claim) {
        int block_sz = static_cast<int>(block_size_);

        if (claim.lo >= claim.hi) {
            // refill via CAS. upper bound the block_size so counter never overshoots
            int cur = execution_idx_.value.load(std::memory_order_acquire);
            while (cur < block_sz) {
                int new_val = std::min(cur + ExecClaim::BATCH, block_sz);
                if (execution_idx_.value.compare_exchange_weak(
                        cur, new_val, std::memory_order_acq_rel)) {
                    claim.lo = cur;
                    claim.hi = new_val;
                    num_active_tasks_.value.fetch_add(new_val - cur,
                        std::memory_order_acq_rel);
                    break;
                }
                // CAS failure: cur reloaded by compare_exchange_weak, loop again:(
            }
            if (claim.lo >= claim.hi) return std::nullopt;
        }

        while (claim.lo < claim.hi) {
            int idx = claim.lo++;
            auto ver = try_incarnate(idx);
            if (ver.has_value()) {
                return ver;
            }
            // try_incarnate failed (status not READY), skip and release slot
            num_active_tasks_.value.fetch_sub(1, std::memory_order_acq_rel);
        }
        return std::nullopt;
    }

    std::optional<Version> next_version_to_validate() {
        int block_sz = static_cast<int>(block_size_);
        if (validation_idx_.value.load(std::memory_order_acquire) >= block_sz) {
            return std::nullopt;
        }
        num_active_tasks_.value.fetch_add(1, std::memory_order_acq_rel);
        int idx = validation_idx_.value.fetch_add(1, std::memory_order_acq_rel);
        if (idx < block_sz) {
            auto& entry = *txn_status_[idx];
            auto [inc, st] = TxnStatusEntry::unpack(entry.state.load(std::memory_order_acquire));
            if (st == TxnStatus::EXECUTED) {
                return Version{static_cast<size_t>(idx), inc};
            }
        }
        num_active_tasks_.value.fetch_sub(1, std::memory_order_acq_rel);
        return std::nullopt;
    }

    // only valid from ABORTING(inc) -> READY(inc+1). called by:
    //   (a) finish_validation when our own validation aborted us
    //   (b) resume_dependencies when blocking finished and we were waiting
    // both guarantee we're in ABORTING status. 
    void set_ready_status(size_t txn_idx) {
        auto& entry = *txn_status_[txn_idx];
        uint64_t expected = entry.state.load(std::memory_order_relaxed);
        while (true) {
            auto [inc, st] = TxnStatusEntry::unpack(expected);
            if (st != TxnStatus::ABORTING) return;  // someone else already promoted
            uint64_t desired = TxnStatusEntry::pack(inc + 1, TxnStatus::READY_TO_EXECUTE);
            if (entry.state.compare_exchange_weak(expected, desired,
                    std::memory_order_acq_rel)) return;
            // CAS failure reloads expected
        }
    }

    void resume_dependencies(DepNode* list) {
        if (!list || list == CLOSED_MARKER) return;

        size_t min_dep = block_size_;
        DepNode* cur = list;
        while (cur) {
            size_t dep_idx = cur->txn_idx;
            set_ready_status(dep_idx);
            min_dep = std::min(min_dep, dep_idx);
            
            DepNode* next = cur->next;
            delete cur;
            cur = next;
        }

        if (min_dep < block_size_) {
            decrease_execution_idx(static_cast<int>(min_dep));
        }
    }
};
