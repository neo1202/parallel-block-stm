# parallel-block-stm

Parallel speculative transaction execution engine implementing the Block-STM algorithm (Gelashvili et al., PPoPP 2023) in C++.

**CMU 15-418/618 Final Project, Spring 2026**

**Team:** Hua-Yo Wu (huayow), Pi-Jung Chang (pijungc)

**[Project Page](https://neo1202.github.io/parallel-block-stm/)**

---

## Overview

Given a block of N ordered transactions, this engine speculatively executes them in parallel across multiple threads, detects read/write conflicts at runtime, aborts and re-executes conflicting transactions via cascading rollback, and guarantees the final state is identical to sequential execution in the preset order.

The core data structure is a **lock-free multi-version data store (MVMemory)** where each key maintains a version chain accessed concurrently by all threads using `std::atomic` CAS operations. A **collaborative scheduler** coordinates speculative execution, validation, and abort tasks through shared atomic counters, with an **ESTIMATE marker** mechanism that converts wasted aborts into informed dependency waits.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                 Collaborative Scheduler                  │
│   Shared atomic counters: execution_idx | validation_idx │
│                                                          │
│   Thread 0        Thread 1        ...        Thread N    │
│   ┌──────────┐   ┌──────────┐              ┌──────────┐ │
│   │ execute  │   │ execute  │              │ execute  │ │
│   │ validate │   │ validate │              │ validate │ │
│   │ abort?   │   │ abort?   │              │ abort?   │ │
│   └────┬─────┘   └────┬─────┘              └────┬─────┘ │
│        │              │                         │        │
│        ▼              ▼                         ▼        │
│   ┌──────────────────────────────────────────────────┐   │
│   │          Multi-Version Data Store (MVMemory)      │   │
│   │                                                    │   │
│   │  Key -> version chain: [(tx_id, value), ...]        │   │
│   │  Lock-free CAS · ESTIMATE markers · snapshot reads │   │
│   └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘

Transaction lifecycle:
  ReadyToExecute -> Executing -> Validated -> Committed
        ↑                          │
        └──── Aborted (conflict) ──┘
```

## Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Dependencies

- C++20 compiler (g++ 10+)
- OpenMP
- CUDA Toolkit 11+ (stretch goal)

## Usage

```bash
# Run correctness tests
./build/test_blockstm

# Run benchmarks
./build/bench_scaling --threads 8 --block-size 10000 --accounts 1000
./build/bench_contention --threads 8 --accounts 2,10,100,1000,10000
```

## Project Structure

```
parallel-block-stm/
├── docs/                    # GitHub Pages project website
│   └── index.html
├── src/
│   ├── transaction.h        # Transaction struct (read/write sets, status)
│   ├── workload.h           # Synthetic workload generator (tunable contention)
│   ├── mvmemory.h           # Multi-version data store (lock-free version chains)
│   ├── scheduler.h          # Collaborative scheduler (atomic counters, task dispatch)
│   ├── executor.h           # Speculative execution + validation + abort logic
│   ├── blockstm.h           # Top-level Block-STM engine
│   └── sequential.h         # Sequential baseline (correctness reference)
├── bench/
│   ├── bench_scaling.cpp    # Thread scaling benchmark
│   └── bench_contention.cpp # Contention sweep benchmark
├── test/
│   └── test_blockstm.cpp   # Correctness tests (parallel vs sequential equivalence)
├── CMakeLists.txt
├── .gitignore
└── README.md
```

## References

- Gelashvili, Spiegelman, Xiang, et al. *Block-STM: Scaling Blockchain Execution by Turning Ordering Curse to a Performance Blessing.* PPoPP, 2023.
- Shavit, Touitou. *Software Transactional Memory.* PODC, 1995.
