#pragma once

// mvmemory.h - Multi-Version Data Store (Algorithm 2)
//
// The shared data structure at the heart of Block-STM. Stores multiple
// versions of each memory location, one per transaction that wrote to it.
//
// WHY MULTI-VERSION?
//   Different transactions need different "correct" values for the same
//   location. tx2 should read what tx1 wrote; tx4 should read what tx3
//   wrote. A single latest value cannot serve all readers simultaneously.
//
// STRUCTURE:
//   Outer: std::unordered_map<Key, VersionChain>
//     - One entry per memory location (account ID)
//     - Key = uint64_t, so hash collisions are negligible
//     - This level does NOT need to be lock-free
//
//   Inner: VersionChain (per location)
//     - Phase 2: std::map<txn_idx, Entry> + std::mutex
//       Entry = (incarnation_number, value) | ESTIMATE marker
//     - Phase 6: lock-free sorted linked list (Harris algorithm)
//
// KEY OPERATIONS (Algorithm 2):
//   read(location, txn_idx)
//     Find the entry with the highest txn_idx < caller's txn_idx.
//     Returns OK + value, or ESTIMATE (-> READ_ERROR), or NOT_FOUND.
//
//   record(version, read_set, write_set)
//     Apply write_set to data map, update last_written_locations,
//     store read_set. Returns whether a new location was written.
//
//   validate_read_set(txn_idx)
//     Re-read every location in the stored read_set and compare versions.
//     Returns false if any read is stale (-> triggers abort).
//
//   convert_writes_to_estimates(txn_idx)
//     Replace all entries written by this tx with ESTIMATE markers.
//     Called on abort. ESTIMATEs serve as dependency hints for other txs.
//
//   snapshot()
//     Called after all txs are committed. Returns the final value for
//     every location (read with txn_idx = BLOCK.size()).
//
// TODO: implement
