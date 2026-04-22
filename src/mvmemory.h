#pragma once

// mvmemory.h - Algorithm 2 from the paper. One MVMemory per block.
// Stores one version per (key, txn that wrote it); reads find the highest
// written version < txn_idx. Writes are buffered in ParallelContext and
// flushed via record() once execution finishes without hitting an ESTIMATE.

#include "transaction.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>  // std::this_thread::yield for unknown architectures
#include <unordered_map>
#include <unordered_set>
#include <vector>

// (txn_idx, incarnation) - one execution attempt of a transaction.
struct Version {
    size_t txn_idx;
    size_t incarnation;

    bool operator==(const Version& o) const {
        return txn_idx == o.txn_idx && incarnation == o.incarnation;
    }
    bool operator!=(const Version& o) const { return !(*this == o); }
};

// Status returned by MVMemory::read()
enum class ReadStatus {
    OK,          // Found a valid entry: version and value are set
    READ_ERROR,  // Hit an ESTIMATE marker: blocking_txn_idx is set
    NOT_FOUND    // No entry with idx < txn_idx: caller needs to reads from Storage
};

// Full result of MVMemory::read()
struct ReadResult {
    ReadStatus status;
    Version version = {0, 0};       // valid when status == OK
    Value value = 0;                 // valid when status == OK
    size_t blocking_txn_idx = 0;     // valid when status == READ_ERROR 回傳哪個前面的txn卡著那位置
};

// 存：我讀的那個版本還在不在
// One entry in the read-set: which location was read, and what version was seen.
// version == nullopt means the value was read from Storage (NOT_FOUND in MVMemory).
struct ReadDescriptor {
    Key location;
    std::optional<Version> version;  // nullopt = read from Storage
};

// 只需要存「我要寫什麼」，不需要版本資訊，因為寫入方就是自己，版本就是自己的 (txn_idx, incarnation_number)
// 執行完之後傳給 MVMemory.record()，寫進 data map
// One entry in the write-set: which location to write and what value.
struct WriteDescriptor {
    Key location;
    Value value;
};

// One entry in a version chain, sorted by txn_idx descending (highest at
// head) so read(txn_idx) just scans from head until idx < txn_idx.
struct ChainNode {
    size_t txn_idx; // 建立後不變，不用atomic
    std::atomic<size_t> incarnation{0};
    std::atomic<Value> value{0};
    std::atomic<bool> is_estimate{false};
    std::atomic<bool> is_deleted{false}; // 防止aba問題所以不實際刪除Node, 反正block結束會清除
    std::atomic<ChainNode*> next{nullptr};

    ChainNode(size_t idx) : txn_idx(idx) {}
};

// Per-key version chain
// One lock per key - different keys can be accessed concurrently.
// Nodes are sorted by txn_idx descending (head = highest txn_idx).
struct VersionChain {
    std::atomic<ChainNode*> head{nullptr};

    // no destructor work ,nodes live in the per-block arena and are freed
    // together when the arena is destroyed. cleaning up the linked list
    // here would double-free.

    // Find the node for txn_idx, or nullptr if not found.
    ChainNode* find(size_t txn_idx) const {
        ChainNode* cur = head.load(std::memory_order_acquire);
        while (cur) {
            if (cur->txn_idx == txn_idx) return cur;
            if (cur->txn_idx < txn_idx) return nullptr;  // past it (descending)
            cur = cur->next.load(std::memory_order_acquire);
        }
        return nullptr;
    }

    // caller needs to fill all fields on new_node before calling this.
    // once CAS succeeds it's visible, late stores get lost (stale read of 0 etc.)
    // returns new_node if inserted, or existing node if the txn_idx was already there.
    // 此處的new node是 pre_built好的
    ChainNode* upsert(ChainNode* new_node) {
        size_t txn_idx = new_node->txn_idx;
        // exp backoff to avoid retry storm on hot chains
        int backoff_spins = 1;
        const int backoff_cap = 1024;

        while (true) {
            ChainNode* prev = nullptr;
            ChainNode* cur = head.load(std::memory_order_acquire);

            // Find insertion point
            while (cur && cur->txn_idx > txn_idx) {
                prev = cur;
                cur = cur->next.load(std::memory_order_acquire);
            }

            // already there -> un-delete and return it. new_node leaks into
            // the arena but gets freed with the arena at block end.
            if (cur && cur->txn_idx == txn_idx) {
                cur->is_deleted.store(false, std::memory_order_release);
                return cur;
            }

            // Not found, insert new_node between prev and cur
            new_node->next.store(cur, std::memory_order_relaxed);

            bool ok = (prev == nullptr)
                ? head.compare_exchange_weak(cur, new_node,
                    std::memory_order_release, std::memory_order_acquire)
                : prev->next.compare_exchange_weak(cur, new_node,
                    std::memory_order_release, std::memory_order_acquire);

            if (ok) return new_node;

            // CAS 沒搶到，pause 一下再從 head 重找
            for (int i = 0; i < backoff_spins; ++i) {
#if defined(__x86_64__) || defined(__i386__)
                __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
                __asm__ __volatile__("yield" ::: "memory");
#else
                std::this_thread::yield();
#endif
            }
            if (backoff_spins < backoff_cap) backoff_spins *= 2;
        }
    }

    // Logically remove the node for txn_idx.
    void erase(size_t txn_idx) {
        ChainNode* node = find(txn_idx);
        if (node) {
            node->is_deleted.store(true, std::memory_order_release);
        }
    }

    // Find the node with the highest valid txn_idx strictly less than the given idx.
    ChainNode* find_highest_valid_less_than(size_t txn_idx) const {
        ChainNode* cur = head.load(std::memory_order_acquire);
        while (cur) {
            if (cur->txn_idx < txn_idx) {
                if (!cur->is_deleted.load(std::memory_order_acquire)) {
                    return cur;
                }
            }
            cur = cur->next.load(std::memory_order_acquire);
        }
        return nullptr;
    }
};

// Bump allocator for ChainNode - one arena per MVMemory (per block). all
// ChainNodes are freed when the arena is destroyed, which skips
// the per-node delete taking ~5% in the -O3 profile experiment in PSC.
//
// fast path: one atomic fetch_add on the current block's offset.
// slow path (when block is full): mutex-protected new-block allocation.
class NodeArena {
    struct Block {
        std::byte* data;
        std::atomic<size_t> offset;
        size_t cap;
        Block* prev;  // singly-linked for cleanup
    };

    static constexpr size_t BLOCK_CAP = 64 * 1024;  // 64 KB

    std::atomic<Block*> cur_;
    std::mutex grow_mtx_;

    Block* make_block(size_t cap, Block* prev) {
        auto* b = new Block;
        b->data = new std::byte[cap];
        b->offset.store(0, std::memory_order_relaxed);
        b->cap = cap;
        b->prev = prev;
        return b;
    }

public:
    NodeArena() {
        cur_.store(make_block(BLOCK_CAP, nullptr), std::memory_order_relaxed);
    }

    ~NodeArena() {
        Block* b = cur_.load(std::memory_order_relaxed);
        while (b) {
            // ChainNode has atomic members, their destructors are trivial
            Block* p = b->prev;
            delete[] b->data;
            delete b;
            b = p;
        }
    }

    ChainNode* alloc(size_t txn_idx) {
        constexpr size_t SZ = sizeof(ChainNode);
        static_assert(alignof(ChainNode) <= 8);
        while (true) {
            Block* block = cur_.load(std::memory_order_acquire);
            size_t off = block->offset.fetch_add(SZ, std::memory_order_relaxed);
            if (off + SZ <= block->cap) {
                return new (block->data + off) ChainNode(txn_idx);
            }
            // overflow, try to grow (only the first thread through the mutex
            // actually allocates; others retry with the new block).
            std::lock_guard<std::mutex> lk(grow_mtx_);
            if (cur_.load(std::memory_order_acquire) == block) {
                size_t cap = std::max(BLOCK_CAP, SZ * 2); //防止SZ太大
                Block* nb = make_block(cap, block);
                cur_.store(nb, std::memory_order_release);
            }
        }
    }
};

// Per-transaction auxiliary data (paper: last_written_locations, last_read_set).
// Accessed via shared_ptr for RCU-style safe concurrent reads.
struct TxnAuxData {
    std::mutex mtx;
    std::shared_ptr<std::vector<Key>> written_locations;
    std::shared_ptr<std::vector<ReadDescriptor>> read_set;

    TxnAuxData()
        : written_locations(std::make_shared<std::vector<Key>>())
        , read_set(std::make_shared<std::vector<ReadDescriptor>>()) {}
};

class MVMemory {
    size_t block_size_;
    // chains' destructors do nothing now since arena owns the storage.
    NodeArena arena_;

    // location -> VersionChain.
    // Pre-populated in constructor for all keys in initial_state.
    // The outer map is never modified during execution, so no lock is needed.
    std::unordered_map<Key, VersionChain> data_;

    // !Per-txn auxiliary data (indexed by txn_idx, size = block_size_).
    std::vector<std::unique_ptr<TxnAuxData>> txn_aux_;

public:
    MVMemory(size_t block_size, const std::unordered_map<Key, Value>& initial_state)
        : block_size_(block_size)
    {
        // Pre-populate empty version chains for all keys.
        // All transaction access keys must be in initial_state.
        for (const auto& [key, val] : initial_state) {
            data_[key];  // default-constructs an empty VersionChain
        }

        // Initialize per-txn auxiliary data.
        txn_aux_.reserve(block_size);
        for (size_t i = 0; i < block_size; ++i) {
            txn_aux_.push_back(std::make_unique<TxnAuxData>());
        }
    }


    // Find the entry written by the highest txn_idx < the caller's txn_idx.
    //   - OK:         found a valid entry -> return version + value
    //   - READ_ERROR: found an ESTIMATE marker -> return blocking_txn_idx
    //   - NOT_FOUND:  no entry exists -> caller should read from Storage
    ReadResult read(Key location, size_t txn_idx) {
        auto it = data_.find(location);
        if (it == data_.end()) {
            return {ReadStatus::NOT_FOUND};
        }
        auto& chain = it->second;
        return read_lockfree(chain, txn_idx);
    }

    // Called after successful execution (no READ_ERROR).
    // 1. Write the write-set into version chains (apply_write_set)
    // 2. Compare written locations with previous incarnation (rcu_update)
    // 3. Store the read-set for later validation
    // Returns true if a new location was written OR a value changed.
    bool record(Version version,
                std::vector<ReadDescriptor> read_set,
                std::vector<WriteDescriptor> write_set) {
        size_t txn_idx = version.txn_idx;
        size_t incarnation = version.incarnation;

        bool changed = rcu_update_written_locations_and_check_changes(txn_idx, write_set);
        apply_write_set(txn_idx, incarnation, write_set);

        // store the read-set so later validations can re-check versions
        {
            auto& aux = *txn_aux_[txn_idx];
            std::lock_guard<std::mutex> lock(aux.mtx);
            aux.read_set = std::make_shared<std::vector<ReadDescriptor>>(std::move(read_set));
        }
        return changed;
    }

    // Re-read every location in the stored read-set and compare versions.
    // Returns false if any read is stale -> triggers abort + re-execution
    bool validate_read_set(size_t txn_idx) {
        std::shared_ptr<std::vector<ReadDescriptor>> prior_reads;
        {
            auto& aux = *txn_aux_[txn_idx];
            std::lock_guard<std::mutex> lock(aux.mtx);
            prior_reads = aux.read_set;
        }

        for (const auto& rd : *prior_reads) {
            ReadResult cur = read(rd.location, txn_idx);

            // was OK, now hit ESTIMATE -> someone wrote + aborted
            if (cur.status == ReadStatus::READ_ERROR) return false;

            // had a version, now gone (storage-only)
            if (cur.status == ReadStatus::NOT_FOUND && rd.version.has_value()) return false;

            // version changed (also catches prev NOT_FOUND -> now OK)
            if (cur.status == ReadStatus::OK) {
                if (!rd.version.has_value() || cur.version != *rd.version) return false;
            }
            // both NOT_FOUND -> still valid
        }
        return true;
    }

    // on abort, flip every entry this tx wrote into an ESTIMATE marker.
    // readers seeing an ESTIMATE know to suspend rather than read stale data.
    void convert_writes_to_estimates(size_t txn_idx) {
        std::shared_ptr<std::vector<Key>> locations;
        {
            auto& aux = *txn_aux_[txn_idx];
            std::lock_guard<std::mutex> lock(aux.mtx);
            locations = aux.written_locations;
        }

        for (Key loc : *locations) {
            auto it = data_.find(loc);
            if (it == data_.end()) continue;
            auto& chain = it->second;
            ChainNode* node = chain.find(txn_idx);
            if (node) {
                node->is_estimate.store(true, std::memory_order_release);
            }
        }
    }

    // collect final value per location. called after all threads joined.
    std::unordered_map<Key, Value> snapshot() {
        std::unordered_map<Key, Value> ret;
        for (auto& [location, chain] : data_) {
            ReadResult result = read_lockfree(chain, block_size_);
            if (result.status == ReadStatus::OK) {
                ret[location] = result.value;
            }
        }
        return ret;
    }

private:
    // --- Algorithm 2, Line 27-29: apply_write_set() ---
    //
    // Write each (location, value) pair into the version chain at txn_idx.
    // Overwrites any existing entry for this txn_idx, like from a previous
    // incarnation or an ESTIMATE marker
    void apply_write_set(size_t txn_idx, size_t incarnation,
                         const std::vector<WriteDescriptor>& write_set) {
        for (const auto& wd : write_set) {
            auto it = data_.find(wd.location);
            if (it == data_.end()) continue;  // shouldn't happen but don't crash
            auto& chain = it->second;

            // 先填好所有欄位再 CAS 進 chain，讀者才不會看到 value=0 的半成品
            auto* candidate = arena_.alloc(txn_idx);
            candidate->incarnation.store(incarnation, std::memory_order_relaxed);
            candidate->value.store(wd.value, std::memory_order_relaxed);
            candidate->is_estimate.store(false, std::memory_order_relaxed);
            candidate->is_deleted.store(false, std::memory_order_relaxed);

            ChainNode* node = chain.upsert(candidate);
            if (node != candidate) {
                // existing node - update its fields. candidate is wasted but
                // sits in the arena until block cleanup.
                node->incarnation.store(incarnation, std::memory_order_relaxed);
                node->value.store(wd.value, std::memory_order_relaxed);
                node->is_estimate.store(false, std::memory_order_relaxed);
                node->is_deleted.store(false, std::memory_order_release);
            }
        }
    }


    // Compare this incarnation's written locations and values with the previous one.
    //   1. Remove entries for locations no longer written (prev but not new)
    //   2. Store the new locations list (RCU update)
    //   3. Return whether any new location was written OR any value changed.
    bool rcu_update_written_locations_and_check_changes(size_t txn_idx,
                                                       const std::vector<WriteDescriptor>& write_set) {
        auto& aux = *txn_aux_[txn_idx];

        std::shared_ptr<std::vector<Key>> prev_locs;
        {
            std::lock_guard<std::mutex> lock(aux.mtx);
            prev_locs = aux.written_locations;
        }

        std::vector<Key> new_locations;
        new_locations.reserve(write_set.size());
        for (const auto& wd : write_set) {
            new_locations.push_back(wd.location);
        }

        // drop entries we wrote last time but not this time
        std::unordered_set<Key> new_set(new_locations.begin(), new_locations.end());
        for (Key loc : *prev_locs) {
            if (new_set.find(loc) == new_set.end()) {
                auto it = data_.find(loc);
                if (it != data_.end()) {
                    it->second.erase(txn_idx);
                }
            }
        }

        bool changed = false;
        if (prev_locs->size() != new_locations.size()) {
            changed = true;
        } else {
            std::unordered_set<Key> prev_set(prev_locs->begin(), prev_locs->end());
            for (Key loc : new_locations) {
                if (prev_set.find(loc) == prev_set.end()) {
                    changed = true;
                    break;
                }
            }
        }

        // even with same location set, values might differ. check existing nodes
        // BEFORE apply_write_set overwrites them.
        for (const auto& wd : write_set) {
            auto it = data_.find(wd.location);
            if (it == data_.end()) continue;
            auto& chain = it->second;
            ChainNode* node = chain.find(txn_idx);
            // missing / logically deleted / still an ESTIMATE / value differs -> changed
            if (!node ||
                node->is_deleted.load(std::memory_order_acquire) ||
                node->is_estimate.load(std::memory_order_acquire) ||
                node->value.load(std::memory_order_acquire) != wd.value) {
                changed = true;
                break;
            }
        }

        // publish new locations via RCU pointer swap
        {
            std::lock_guard<std::mutex> lock(aux.mtx);
            aux.written_locations = std::make_shared<std::vector<Key>>(new_locations);
        }

        return changed;
    }

    // lock-free scan down the chain for the first non-deleted entry with idx < txn_idx
    ReadResult read_lockfree(const VersionChain& chain, size_t txn_idx) const {
        ChainNode* node = chain.find_highest_valid_less_than(txn_idx);
        if (!node) {
            return {ReadStatus::NOT_FOUND};
        }
        // ESTIMATE means a lower tx wrote here then aborted -> caller must suspend
        if (node->is_estimate.load(std::memory_order_acquire)) {
            return {ReadStatus::READ_ERROR, {}, 0, node->txn_idx};
        }
        return {ReadStatus::OK, {node->txn_idx, node->incarnation.load(std::memory_order_acquire)}, node->value.load(std::memory_order_acquire), 0};
    }
};
