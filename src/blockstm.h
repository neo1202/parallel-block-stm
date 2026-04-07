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
// TODO: implement
