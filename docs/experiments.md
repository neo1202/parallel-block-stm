# Experiments

working notes. what we ran, what we got, what we noticed.

## fixed workload params (don't change across experiments)

- block size = 10000 txs
- reads/writes per tx = 2 / 2  (simplified diem p2p - conflict-relevant state only)
- compute per tx = 200 sha-256 rounds over the read values (simulates VM + gas + signature work)
- runs = 10, take median
- seed fixed so each run is reproducible
- threads swept over {1, 2, 4, 8, 16, 32, 64, 128}

why 2r/2w + compute instead of diem's actual 21r/4w: real diem's 21 reads are mostly to static state (module bytecode, gas schedule, chain params) that never conflict. treating all 21 as dynamic MVMemory reads overestimates contention and penalizes lock-free with unnecessary atomic barriers. we model conflict-causing accesses (2r/2w = balance changes) + compute padding for the rest of the VM work. sequential throughput lands around 15-30k tps on PSC.

## versions tracked

each differs only in src/mvmemory.h (the rest of the tree is identical). ref gives the commit whose mvmemory.h is used.

| tag | mvmemory.h @ | notes |
|---|---|---|
| V0-mutex             | 8239b3f | baseline - mutex VersionChain, mutex scheduler |
| V1-lfchain-nobackoff | f54ebfb | LF chain with pre-built-node CAS (race fixed), no backoff |
| V2-lfchain-backoff   | HEAD    | V1 + exponential backoff on CAS failure |
| V3-lfsched           | future  | + lock-free scheduler (bit-packed status, Treiber-stack deps), race fixed |
| V4+                  | future  | arena allocator / skip list / ... |

## two experiments, run per version

- **exp A - contention sweep**: accounts in {2, 10, 100, 1000, 10000}
- **exp B - dex hot/cold**: 1000 accounts (50 hot + 950 cold), hot_tx_ratio=0.2, hot_keys_per_tx=1

## log

### 2026-04-19, PSC job 40056102 - V0 vs V2 (no V1 yet), commit dc14d77

first full run of the exp A + exp B template. only V0 and V2 tested; adding V1 next to isolate the backoff contribution.

- workload: block=10000, reads/writes=2/2, compute=200, runs=10
- config: calibrated locally so V2 wins hot/cold by ~15% at 8 threads on mac

**key numbers (128 threads, throughput tps):**

| experiment | V0-mutex | V2-lfchain-backoff | delta |
|---|---|---|---|
| exp A, accounts=2     | 3,172   | 6,184   | **+95%** |
| exp A, accounts=10    | 12,184  | 13,243  | +9% |
| exp A, accounts=100   | 75,289  | 84,133  | +12% |
| exp A, accounts=1000  | 166,611 | 194,891 | +17% |
| exp A, accounts=10000 | 223,297 | 239,260 | +7% |
| exp B, hot/cold       | 187,807 | 206,047 | +10% |

takeaways:
- at 1-64 threads V0 and V2 are within noise. the PSC AMD EPYC mutex implementation is fast, so the LF benefit doesn't kick in until 128-thread contention bottlenecks the mutex.
- highest LF win is at accounts=2 (extreme contention): V0 drops from 5.6k (64t) to 3.2k (128t) while V2 stays at 6.2k. that's mutex choking under 128 writers on 2 chains.
- hot/cold is less dramatic - only 20% of txs are hot, so 80% parallelize regardless of implementation.
- block-STM itself scales well: accounts=10000 goes 9k (1t) -> 239k (128t) ~= 27x speedup.

CSVs: `benchmark_records/psc_40056102_{V0-mutex,V2-lfchain-backoff}/`

next: add V1 (same LF chain, no backoff) to isolate backoff impact.
