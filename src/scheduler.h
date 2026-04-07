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
// RAII PATTERN - TaskGuard:
//   num_active_tasks must be incremented before starting a task and
//   decremented after finishing. TaskGuard uses RAII to guarantee the
//   decrement happens even on early returns, preventing check_done()
//   from getting stuck.
//
// CACHE PADDING:
//   execution_idx, validation_idx, num_active_tasks, decrease_cnt must
//   each sit on separate cache lines (alignas(64)) to avoid false sharing
//   between threads.
//
// TODO: implement
