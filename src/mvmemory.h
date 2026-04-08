#pragma once

// mvmemory.h - Multi-Version Data Store (Algorithm 2)
// MVMemory 是 per-block，不跨block。論文：一個 block = 一個 MVMemory + 一個 Scheduler
//
// The shared data structure at the heart of Block-STM. Stores multiple
// versions of each memory location, one per transaction that wrote to it.
//
// WHY MULTI-VERSION?
//   Different transactions need different "correct" values for the same
//   location. tx2 should read what tx1 wrote; tx4 should read what tx3
//   wrote. A single latest value cannot serve all readers simultaneously.
//
// STRUCTURE (Phase 2 - mutex-based):
//   Outer: std::unordered_map<Key, VersionChain>
//     - One entry per memory location (account ID)
//     - Pre-populated in constructor, read-only during execution
//
//   Inner: VersionChain (per location)
//     - std::map<txn_idx, MVEntry> + std::mutex (one lock per key)
//     - MVEntry = (incarnation_number, value) | ESTIMATE marker
//     - std::map gives O(log n) lower_bound for "find highest idx < txn_idx"
//     - Phase 6: will be replaced with lock-free sorted linked list (Harris)
//
// KEY OPERATIONS (Algorithm 2, paper line numbers):
//   read(location, txn_idx)                      - Line 47-54
//   apply_write_set(txn_idx, incarnation, ws)     - Line 27-29
//   rcu_update_written_locations(txn_idx, locs)   - Line 30-35
//   record(version, read_set, write_set)          - Line 36-42
//   convert_writes_to_estimates(txn_idx)          - Line 43-46
//   validate_read_set(txn_idx)                    - Line 62-72
//   snapshot()                                    - Line 55-61
//
// WRITE TIMING:
//   ParallelContext buffers writes locally during tx.logic() execution.
//   Only when execution completes WITHOUT hitting an ESTIMATE (no READ_ERROR),
//   record() is called to write the entire write-set into shared memory at once.
//

#include "transaction.h"

#include <cassert>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Types shared between MVMemory and Executor

// !Version = (txn_idx, incarnation_number)
// Uniquely identifies one execution attempt of a transaction.
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

// Internal types

// Node in a version chain linked list.
// Each node represents one transaction's write to this location.
// Sorted by txn_idx descending (highest at head) for fast read():
//   read() scans from head, first node with idx < caller's txn_idx is the answer.
//
// Fields:
//   txn_idx     - which transaction wrote this entry
//   incarnation - which incarnation of that transaction
//   value       - the value written
//   is_estimate - true if this entry was marked as ESTIMATE (abort marker)
//   next        - pointer to the next node (lower txn_idx)
struct ChainNode {
    size_t txn_idx;
    size_t incarnation = 0;
    Value value = 0;
    bool is_estimate = false;
    ChainNode* next = nullptr;

    ChainNode(size_t idx) : txn_idx(idx) {}
};

// Per-key version chain (Phase 2: sorted linked list + mutex).
// One lock per key - different keys can be accessed concurrently.
// Nodes are sorted by txn_idx descending (head = highest txn_idx).
// Phase 6: will be replaced with Harris lock-free sorted linked list.
struct VersionChain {
    std::mutex mtx;
    ChainNode* head = nullptr;  // highest txn_idx at head

    ~VersionChain() {
        ChainNode* cur = head;
        while (cur) {
            ChainNode* tmp = cur;
            cur = cur->next;
            delete tmp;
        }
    }

    // Find the node for txn_idx, or nullptr if not found.
    // Caller must hold mtx.
    ChainNode* find(size_t txn_idx) {
        ChainNode* cur = head;
        while (cur) {
            if (cur->txn_idx == txn_idx) return cur;
            if (cur->txn_idx < txn_idx) return nullptr;  // past it (descending)
            cur = cur->next;
        }
        return nullptr;
    }

    // Insert or update a node for txn_idx.
    // If exists: overwrite incarnation/value/is_estimate.
    // If not: insert in sorted position (descending by txn_idx).
    // Caller must hold mtx.
    ChainNode* upsert(size_t txn_idx) {
        // Check if already exists
        ChainNode* existing = find(txn_idx);
        if (existing) return existing;

        // Insert new node in sorted position (descending)
        auto* node = new ChainNode(txn_idx);
        if (!head || txn_idx > head->txn_idx) {
            // Insert at head
            node->next = head;
            head = node;
        } else {
            // Find insertion point: after prev, before prev->next
            ChainNode* prev = head;
            while (prev->next && prev->next->txn_idx > txn_idx) {
                prev = prev->next;
            }
            node->next = prev->next;
            prev->next = node;
        }
        return node;
    }

    // Remove the node for txn_idx. Caller must hold mtx.
    void erase(size_t txn_idx) {
        if (!head) return;
        if (head->txn_idx == txn_idx) {
            ChainNode* tmp = head;
            head = head->next;
            delete tmp;
            return;
        }
        ChainNode* prev = head;
        while (prev->next) {
            if (prev->next->txn_idx == txn_idx) {
                ChainNode* tmp = prev->next;
                prev->next = tmp->next;
                delete tmp;
                return;
            }
            prev = prev->next;
        }
    }

    // Find the node with the highest txn_idx strictly less than the given idx.
    // Caller must hold mtx.
    ChainNode* find_less_than(size_t txn_idx) {
        ChainNode* cur = head;
        while (cur) {
            if (cur->txn_idx < txn_idx) return cur;  // first one less (descending)
            cur = cur->next;
        }
        return nullptr;
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

// MVMemory (Algorithm 2)
class MVMemory {
    size_t block_size_;

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

    // --- Algorithm 2, Line 47-54: read() ---
    //
    // Find the entry written by the highest txn_idx LESS THAN the caller's txn_idx.
    //   - OK:         found a valid entry -> return version + value
    //   - READ_ERROR: found an ESTIMATE marker -> return blocking_txn_idx
    //   - NOT_FOUND:  no entry exists -> caller should read from Storage
    ReadResult read(Key location, size_t txn_idx) {
        auto it = data_.find(location);
        if (it == data_.end()) {
            return {ReadStatus::NOT_FOUND};
        }
        auto& chain = it->second;
        std::lock_guard<std::mutex> lock(chain.mtx);
        return read_locked(chain, txn_idx);
    }

    // --- Algorithm 2, Line 36-42: record() ---
    //
    // Called after successful execution (no READ_ERROR).
    // 1. Write the write-set into version chains (apply_write_set)
    // 2. Compare written locations with previous incarnation (rcu_update)
    // 3. Store the read-set for later validation
    // Returns true if a NEW location was written (not written by prev incarnation).
    bool record(Version version,
                std::vector<ReadDescriptor> read_set,
                std::vector<WriteDescriptor> write_set) {
        size_t txn_idx = version.txn_idx;
        size_t incarnation = version.incarnation;

        // Line 38: apply write-set to version chains
        apply_write_set(txn_idx, incarnation, write_set);

        // Line 39: extract the set of written locations
        std::vector<Key> new_locations;
        new_locations.reserve(write_set.size());
        for (const auto& wd : write_set) {
            new_locations.push_back(wd.location);
        }

        // Line 40: compare with previous incarnation, clean up stale entries
        bool wrote_new_location = rcu_update_written_locations(txn_idx, new_locations);

        // Line 41: store read-set for later validation (RCU update)
        {
            auto& aux = *txn_aux_[txn_idx];
            std::lock_guard<std::mutex> lock(aux.mtx);
            aux.read_set = std::make_shared<std::vector<ReadDescriptor>>(std::move(read_set));
        }

        // Line 42
        return wrote_new_location;
    }

    // --- Algorithm 2, Line 62-72: validate_read_set() ---
    //
    // Re-read every location in the stored read-set and compare versions.
    // Returns false if any read is stale (-> triggers abort + re-execution).
    bool validate_read_set(size_t txn_idx) {
        // Load read-set (RCU read)
        std::shared_ptr<std::vector<ReadDescriptor>> prior_reads;
        {
            auto& aux = *txn_aux_[txn_idx];
            std::lock_guard<std::mutex> lock(aux.mtx);
            prior_reads = aux.read_set;
        }

        for (const auto& rd : *prior_reads) {
            ReadResult cur = read(rd.location, txn_idx);

            // Line 66-67: previously OK, now ESTIMATE
            if (cur.status == ReadStatus::READ_ERROR) {
                return false;
            }

            // Line 68-69: previously from data (had version), now NOT_FOUND
            if (cur.status == ReadStatus::NOT_FOUND && rd.version.has_value()) {
                return false;
            }

            // Line 70-71: read some entry, but version changed
            //   Also catches: previously NOT_FOUND (nullopt), now OK (has version)
            if (cur.status == ReadStatus::OK) {
                if (!rd.version.has_value() || cur.version != *rd.version) {
                    return false;
                }
            }

            // Remaining case: both NOT_FOUND -> still valid (Storage unchanged)
        }
        return true;
    }

    // --- Algorithm 2, Line 43-46: convert_writes_to_estimates() ---
    //
    // Replace all entries written by this tx with ESTIMATE markers.
    // Called on abort. ESTIMATEs tell other transactions:
    //   "this location will probably be written again - don't use the old value."
    void convert_writes_to_estimates(size_t txn_idx) {
        // Load last written locations (RCU read)
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
            std::lock_guard<std::mutex> lock(chain.mtx);
            ChainNode* node = chain.find(txn_idx);
            if (node) {
                node->is_estimate = true;
            }
        }
    }

    // --- Algorithm 2, Line 55-61: snapshot() ---
    //
    // Collect the final value for every location written by any transaction.
    // Called after all threads have joined (no concurrent access).
    // Returns only written locations - caller merges with initial_state
    // for the complete final state.
    std::unordered_map<Key, Value> snapshot() {
        std::unordered_map<Key, Value> ret;
        for (auto& [location, chain] : data_) {
            std::lock_guard<std::mutex> lock(chain.mtx);
            ReadResult result = read_locked(chain, block_size_);
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
    // Overwrites any existing entry for this txn_idx (e.g., from a previous
    // incarnation or an ESTIMATE marker).
    void apply_write_set(size_t txn_idx, size_t incarnation,
                         const std::vector<WriteDescriptor>& write_set) {
        for (const auto& wd : write_set) {
            auto it = data_.find(wd.location);
            assert(it != data_.end() && "write to unknown location");
            auto& chain = it->second;
            std::lock_guard<std::mutex> lock(chain.mtx);
            ChainNode* node = chain.upsert(txn_idx);
            node->incarnation = incarnation;
            node->value = wd.value;
            node->is_estimate = false;
        }
    }

    // --- Algorithm 2, Line 30-35: rcu_update_written_locations() ---
    /*
    比對「這次寫了哪些 key」vs「上次寫了哪些 key」
    - 上次寫了 {A, B, C}，這次寫了 {A, B}
        -> 刪掉 key=C 的 version chain 裡 txn_idx=5 的 node
    - 上次寫了 {A, B}，這次寫了 {A, B, D}
        -> 多了 D，回傳 true */
    // Compare this incarnation's written locations with the previous incarnation's.
    //   1. Remove entries for locations no longer written (prev but not new)
    //   2. Store the new locations list (RCU update)
    //   3. Return whether any NEW location was written (new but not prev)
    //
    // Why this matters:
    //   If wrote_new_location == true -> all higher txs need re-validation
    //   If wrote_new_location == false -> only this tx needs validation
    bool rcu_update_written_locations(size_t txn_idx,
                                      const std::vector<Key>& new_locations) {
        auto& aux = *txn_aux_[txn_idx];

        // Load previous locations (RCU read)
        std::shared_ptr<std::vector<Key>> prev;
        {
            std::lock_guard<std::mutex> lock(aux.mtx);
            prev = aux.written_locations;
        }

        // Line 32-33: remove entries for locations written before but not now
        std::unordered_set<Key> new_set(new_locations.begin(), new_locations.end());
        for (Key loc : *prev) {
            if (new_set.find(loc) == new_set.end()) {
                auto it = data_.find(loc);
                if (it != data_.end()) {
                    auto& chain = it->second;
                    std::lock_guard<std::mutex> lock(chain.mtx);
                    chain.erase(txn_idx);
                }
            }
        }

        // Line 35: check if new_locations \ prev_locations is non-empty
        std::unordered_set<Key> prev_set(prev->begin(), prev->end());
        bool wrote_new = false;
        for (Key loc : new_locations) {
            if (prev_set.find(loc) == prev_set.end()) {
                wrote_new = true;
                break;
            }
        }

        // Line 34: store new locations (RCU update)
        {
            std::lock_guard<std::mutex> lock(aux.mtx);
            aux.written_locations = std::make_shared<std::vector<Key>>(new_locations);
        }

        return wrote_new;
    }

    // Internal read helper - caller must already hold chain.mtx.
    // Used by read() and snapshot() to avoid double-locking.
    // Scans the linked list (descending txn_idx) for the first node < txn_idx.
    ReadResult read_locked(VersionChain& chain, size_t txn_idx) const {
        // Line 48: find highest idx < txn_idx
        ChainNode* node = chain.find_less_than(txn_idx);
        // Line 49-50: no entry with smaller idx
        if (!node) {
            return {ReadStatus::NOT_FOUND};
        }
        // Line 52-53: ESTIMATE marker
        if (node->is_estimate) {
            return {ReadStatus::READ_ERROR, {}, 0, node->txn_idx};
        }
        // Line 54: valid entry
        return {ReadStatus::OK, {node->txn_idx, node->incarnation}, node->value, 0};
    }
};
