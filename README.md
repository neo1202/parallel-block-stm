# parallel-block-stm

Parallel speculative transaction execution engine implementing the Block-STM algorithm (Gelashvili et al., PPoPP 2023) in C++.

**CMU 15-418/618 Final Project, Spring 2026**

**Team:** Hua-Yo Wu (huayow), Pi-Jung Chang (pijungc)

**[Project Page](https://neo1202.github.io/parallel-block-stm/)**

---

## Overview

Given a block of N ordered transactions, the engine executes them speculatively in parallel across threads, detects read/write conflicts, aborts and re-executes conflicting transactions, and guarantees the final state matches sequential execution in the given order.

Two pieces do most of the work:

- `MVMemory` - a multi-version store. Each key keeps a version chain (lock-free sorted linked list, CAS insert, exponential backoff under contention). Readers find the highest write from a tx with idx less than theirs.
- `Scheduler` - a collaborative scheduler with two shared atomic counters (`execution_idx`, `validation_idx`). Any thread can grab any task; aborted tx's become `ESTIMATE` markers so readers suspend instead of reading stale data.

### Transaction status

Each `(txn_idx, incarnation)` transitions through:

- `READY_TO_EXECUTE(i)` -> `EXECUTING(i)` via `try_incarnate()`
- `EXECUTING(i)` -> `EXECUTED(i)` via `finish_execution()`
- `EXECUTING(i)` -> `ABORTING(i)` via `add_dependency()` when a read hits an ESTIMATE
- `EXECUTED(i)` -> `ABORTING(i)` via `try_validation_abort()` (first caller wins)
- `ABORTING(i)` -> `READY_TO_EXECUTE(i+1)` via `set_ready_status()`

## Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Dependencies

- C++20 compiler (g++ 10+ / clang 13+ / AppleClang)
- CMake 3.20+

## Usage

```bash
./build/test/test_blockstm
./build/bench/bench_scaling --threads 8 --block-size 10000 --accounts 1000
```

## References

- Gelashvili, Spiegelman, Xiang, et al. *Block-STM: Scaling Blockchain Execution by Turning Ordering Curse to a Performance Blessing.* PPoPP, 2023.
- Shavit, Touitou. *Software Transactional Memory.* PODC, 1995.
