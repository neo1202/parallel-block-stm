#pragma once

// executor.h - Speculative Execution + Validation + Abort (Algorithm 1 + 3)
//
// Contains two things:
//
// 1. ExecutionContext implementations (replaces VM module, Algorithm 3)
//    - SequentialContext: defined in sequential.h (directly reads/writes a map)
//    - ParallelContext: defined here (reads from MVMemory, writes to local buffer)
//
//    ParallelContext provides read(key) and write(key, value) to transaction
//    lambdas. On read: check local write-set first, then MVMemory, then Storage.
//    On write: buffer locally (never touch shared memory during execution).
//    After execution, the caller (try_execute) passes the collected read-set
//    and write-set to MVMemory.record().
//
// 2. Thread logic (Algorithm 1)
//    - run(): main loop - each thread repeatedly grabs tasks from Scheduler
//    - try_execute(version): execute a transaction incarnation
//      -> create ParallelContext, call tx.logic(ctx), then MVMemory.record()
//      -> if read hits ESTIMATE: call Scheduler.add_dependency()
//    - needs_reexecution(version): validate a transaction incarnation
//      -> call MVMemory.validate_read_set()
//      -> if invalid: Scheduler.try_validation_abort() + convert_writes_to_estimates()
//
// CALL CHAIN:
//   run() loop
//     -> Scheduler.next_task()
//     -> try_execute(version)
//         -> ParallelContext created (per tx, per incarnation)
//         -> tx.logic(ctx)           <- transaction runs here
//         -> MVMemory.record(...)    <- write-set applied to shared memory
//         -> Scheduler.finish_execution(...)
//     -> needs_reexecution(version)
//         -> MVMemory.validate_read_set(...)
//         -> Scheduler.finish_validation(...)
//     -> repeat until Scheduler.done()
//
// TODO: implement
