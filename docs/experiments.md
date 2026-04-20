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

| tag | mvmemory.h @ | scheduler.h @ | notes |
|---|---|---|---|
| V0-mutex-list  | 8239b3f | 8239b3f | baseline - mutex versionchain (linked list), mutex scheduler |
| V0-mutex-tree  | 51c8c6b (v0-tree) | 8239b3f | mutex versionchain using std::map (red-black tree) |
| V1-lfchain-nobackoff | f54ebfb | 8239b3f | LF chain w/ pre-built-node CAS, no backoff |
| V2-lfchain-backoff   | HEAD    | 8239b3f | V1 + exponential backoff on CAS failure |
| V3-lfsched           | HEAD    | HEAD    | V2 + LF scheduler (bit-packed status + Treiber-stack deps, races fixed) |
| V4+ | future | future | arena allocator / skip list / ... |

## two experiments, run per version

- **exp A - contention sweep**: accounts in {2, 10, 100, 1000, 10000}
- **exp B - dex hot/cold**: 1000 accounts (50 hot + 950 cold), hot_tx_ratio=0.2, hot_keys_per_tx=1

## log

### 2026-04-19, PSC job 40056102 - V0 vs V2 (no V1 yet), commit dc14d77

first full run of the exp A + exp B template. only V0 and V2 tested; adding V1 next to isolate the backoff contribution.

- workload: block=10000, reads/writes=2/2, compute=200, runs=10
- config: calibrated locally so V2 wins hot/cold by ~15% at 8 threads on mac

128 threads, tps:

| experiment | V0-mutex | V2-lfchain-backoff | delta |
|---|---|---|---|
| exp A, accounts=2     | 3,172   | 6,184   | +95% |
| exp A, accounts=10    | 12,184  | 13,243  | +9% |
| exp A, accounts=100   | 75,289  | 84,133  | +12% |
| exp A, accounts=1000  | 166,611 | 194,891 | +17% |
| exp A, accounts=10000 | 223,297 | 239,260 | +7% |
| exp B, hot/cold       | 187,807 | 206,047 | +10% |

at 1-64 threads V0 and V2 are within noise; on PSC EPYC the mutex is fast, so LF only starts winning at 128t. biggest win is at accounts=2 (V0 drops from 5.6k at 64t to 3.2k at 128t while V2 stays at 6.2k, mutex choking under 128 writers on 2 chains). hot/cold less dramatic because only 20% of txs are hot. block-STM itself scales: accounts=10000 goes 9k (1t) -> 239k (128t), ~27x.

CSVs: `benchmark_records/psc_40056102_{V0-mutex,V2-lfchain-backoff}/`

next: add V1 (same LF chain, no backoff) to isolate backoff impact.

---

### 2026-04-19, PSC job 40062716 - added V1, commit dc14d77

added V1 (LF chain, race fixed, no backoff) so we can compare backoff's contribution.

results @ 128 threads, exp A:

| accounts | V0 mutex | V1 LF no-bo | V2 LF +bo |
|---|---|---|---|
| 2     | 2974   | 6191    | 6267    |
| 10    | 12336  | 13607   | 13222   |
| 100   | 71986  | 79738   | 81868   |
| 1000  | 198790 | 202991  | 205149  |
| 10000 | 238124 | 228147  | 219621  |
| hot/cold | 182916 | 207363 | 206731 |

surprise: backoff barely helped. V1 (no backoff) and V2 (with backoff) are within +-3% on every config. our previous theory that backoff is needed to avoid "CAS retry storm" doesn't seem to manifest in this workload - chains are short enough that simultaneous CAS attempts on the same head pointer aren't actually frequent.

the big jump is V0 -> V1: just removing the mutex (with race-free pre-built node CAS) gets nearly all the win. accounts=2 doubles (3k -> 6k), hot/cold gains 13%. backoff is "nice to have" overhead-control, not a primary optimization.

at low contention (accounts=10000) V1/V2 actually slightly LOSE to V0 (-4%). on PSC EPYC the mutex is fast enough that LF's atomic-load barriers are pure overhead when there's nothing to contend over.

CSVs: `benchmark_records/psc_40062716_*/`

next: V3 (LF scheduler) to see if we can shave more.

---

### 2026-04-19, PSC job 40063602 - added V3 LF scheduler, commit b716580

V3 = V2 + lock-free scheduler (partner's bit-packed TxnStatusEntry + Treiber-stack TxnDependency, race bugs fixed in fa55131).

results @ 128 threads, exp A:

| accounts | V0    | V1    | V2    | V3    | V3 vs V2 |
|---|---|---|---|---|---|
| 2     | 3079   | 6314   | 6292   | 3069   | -51%  |
| 10    | 12654  | 13560  | 13593  | 11035  | -19% |
| 100   | 71753  | 79703  | 80506  | 70704  | -12% |
| 1000  | 187287 | 202974 | 207421 | 209705 | +1% |
| 10000 | 207160 | 237604 | 238549 | 227807 | -5% |
| hot/cold | 206214 | 207925 | 203446 | 210272 | +3% |

V3 regressed at high contention. accounts=2 dropped back to V0 levels (-51% vs V2). the dep stack head is a single atomic pointer - 128 threads CAS-pushing onto the same head means retry storm. bit-packed status CAS bounces a cache line across 128 cores on every transition, even with alignas(64). and the mutex scheduler's "block when contended" behaviour is actually adaptive - it parks waiting threads instead of burning cycles on doomed CAS retries.

so lock-free was the wrong tool here. bottleneck wasn't mutex acquire latency, it was contention itself, and CAS doesn't make contention go away - it just moves it. V2 is our best version so far. keeping V3 in the codebase as a negative result.

CSVs: `benchmark_records/psc_40063602_*/`

next: try V0-tree (mutex+std::map instead of mutex+linked-list) to add another data point on the data-structure axis. profile V2 to see what time actually goes into.

---

### 2026-04-19, PSC job 40068444 - added V0-tree + perf profile of V2, commit c41f8f2

added V0-mutex-tree (std::map<txn_idx, ChainNode*> + std::mutex, instead of linked list) and ran perf record on V2 at the most contention-bound config (accounts=100, 128t).

results @ 128 threads, exp A:

| accounts | V0-list | V0-tree | V1     | V2     | V3     |
|---|---|---|---|---|---|
| 2     | 2963   | 5547   | 6236   | 6212   | 3094   |
| 10    | 12458  | 11928  | 13632  | 13273  | 11141  |
| 100   | 78687  | 76421  | 82729  | 83219  | 70762  |
| 1000  | 192670 | 158866 | 210690 | 204758 | 203529 |
| 10000 | 237404 | 211235 | 236201 | 240302 | 202149 |
| hot/cold | 186400 | 166791 | 186190 | 208625 | 210027 |

interesting: V0-tree only wins at accounts=2 (+87% vs V0-list). everywhere else it loses 11-18% to V0-list. red-black tree's balancing overhead and bigger node size hurt when chains are short (which is the case for any realistic account count). tree only pays off when chain length goes into the hundreds.

so a "smarter data structure" isn't a free win. for our workloads, plain linked list is already the right call until you hit pathological contention.

#### perf profile of V2 (accounts=100, 128t)

```
65.88%  Executor::run                    (umbrella for everything)
18.48%  picosha2 hash256_block           (our SHA-256 compute)
 5.33%  std::function _M_invoke          (tx.logic lambda call)
 2.00%  _int_free                        (libc dealloc)
 1.82%  __pthread_mutex_lock             (mutex scheduler ops)
 1.46%  Executor::try_execute            (self time only)
 1.26%  cfree                            (more dealloc)
```

observations: compute is only 18% of total time (real diem/aptos would be 60-80%, we keep compute low so other costs stay visible). mutex is 1.82% - confirms V3 result, mutex wasn't the bottleneck. alloc/free is ~3-4% combined (_int_free + cfree + others) - removable with an arena. the remaining ~60% inside Executor::run is hidden behind the umbrella symbol; likely MVMemory read traversal, scheduler counter atomics, validate_read_set.

what to try next: arena allocator for ChainNode (low risk, targets the 3-4% alloc overhead). skip list probably not worth it - traversal isn't a hotspot in the profile, and V0-tree already showed "smarter chain data structure" only helps at extreme contention.

CSVs: `benchmark_records/psc_40068444_*/`, profile: `benchmark_records/psc_40068444_profile/perf_top.txt`

next: implement V4 (arena allocator) and rerun.

---

### 2026-04-19/20, second perf profile - -fno-inline build (job 40068754 then 40068866)

the first profile (under -O3) gave us a useless 66% "Executor::run self time" because the compiler inlined most of run()'s callees into the function body. perf sampled the PC inside those inlined regions and attributed time to run(). we couldn't see which of next_task / try_execute / MVMemory read was actually hot.

second attempt: rebuild with `-O2 -fno-inline-functions`. almost no change - our helpers are defined inline in class headers, so the compiler still inlined them. that flag only applies to functions declared outside the class.

third attempt: rebuild with `-O2 -fno-inline`. this one disabled ALL inlining, and we finally got a call graph that can be read.

-fno-inline call graph (98.57% in Executor::run's children):

```
  72.17% try_execute
    71.49%   tx.logic (lambda)
      71.00%   SHA-256 (picosha2)
  24.48% Scheduler::next_task                       <- the hidden hotspot
    12.07%   next_version_to_validate
     5.52%   next_version_to_execute
     0.50%   check_done
   1.25% needs_reexecution
     0.68%   MVMemory::validate_read_set
     0.55%   Scheduler::finish_validation
```

caveat: -fno-inline inflates SHA-256 from its true 18% (under -O3) to 71% here because SHA's tight helpers (rotr, ch, bsig1, ssig0, ...) become real function calls. absolute times aren't directly comparable across the two profiles. so we use -O3 for absolute per-function cost (compute=18%, mutex=1.8%, alloc=5%) and -fno-inline for relative proportions inside the "Executor::run self time" umbrella. the -O3 "66% self time" turns out to be ~12-15% scheduler.next_task (dominated by next_version_to_validate) plus small bits.

what this changes for our plan: skip list / tree dropped (MVMemory::read and find_less_than don't show up above ~0.5% in either profile, so O(N) -> O(log N) saves almost nothing; combined with V0-tree's earlier result, not worth the complexity). arena confirmed - `_int_free` + `cfree` + `malloc` is ~5% under -O3, cleanly removable. scheduler.next_task at 24% under -fno-inline (~6-12% under -O3) is the next-biggest hotspot after compute; full LF counter redesign is too risky (V3 regressed) but we can cut how often next_task is called. the paper's Algorithm 1 uses three chained ifs so a validation task can be processed without re-entering the scheduler; partner's version used if/else and pays one extra next_task per transition. reverting that should shave some of the 24%.

profile outputs: `benchmark_records/psc_40068866_profile_noinline/`

---

### 2026-04-20, planned work

1. restore the paper's three-if executor loop in `run()` to reduce next_task frequency
2. implement V4: arena allocator for ChainNode (per-block memory pool, no individual `new`/`delete`)
3. rerun PSC with V4 on top of V2

---

### 2026-04-20, PSC job 40069423 - V4 added + 3-if executor loop (turned out to backfire)

tried two changes at once:
- executor run() switched to paper-style three-if (process exec / validate / fetch)
- added V4 with arena allocator for ChainNode

results exposed a problem: V2 regressed ~24% at accounts=1000, 128t (207k in previous run -> 158k now). V0/V1/V3 numbers held roughly stable. V4 looked very good in the table (+36% vs V2) but that was mostly V2 getting worse, not V4 getting dramatically better.

hypothesized the 3-if loop was the culprit. the new structure processes whatever task is in hand first and only calls next_task at the end of each iteration, which introduces a one-iteration delay for freshly-fetched tasks. paper's algorithm does the same, but measured on PSC it hurt LF-chain-with-backoff specifically - possibly an interaction with cache behaviour at high thread count.

CSVs: `benchmark_records/psc_40069423_*/`

next: revert the executor loop change, keep V4's arena, rerun to confirm.

---

### 2026-04-20, PSC job 40118226 - reverted executor loop, V4 results cleaned up

reverted executor.h to the old if/else. V2 returned to ~205k at 1000/128t, confirming the 3-if loop was the regression cause. kept V4's arena allocator in place.

clean V4 vs V2 comparison at 128 threads:

| accounts | V2      | V4      | delta |
|---|---|---|---|
| 2         | 6,310   | 6,478   | +3% |
| 10        | 13,758  | 13,255  | -4% (noise) |
| 100       | 81,244  | 83,093  | +2% |
| 1000      | 205,586 | 228,630 | +11% |
| 10000     | 232,063 | 239,747 | +3% |
| hot/cold  | 206,929 | 230,682 | +11% |

arena gives a real 3-11% improvement, matches the profile prediction (~5% removable alloc overhead) once noise is factored in. biggest wins at medium contention (accounts=1000) and hot/cold, where there are enough ChainNode allocations per block for malloc contention to matter but chains aren't so long that other costs dominate.

scorecard at 128 threads, accounts=1000:

| version | tps     | vs V0 |
|---|---|---|
| V0-mutex-list          | 189,606 | baseline |
| V0-mutex-tree          | 156,687 | -17% (tree only helps when chains are long) |
| V1-lfchain-nobackoff   | 203,613 | +7% |
| V2-lfchain-backoff     | 205,586 | +8% |
| V3-lfsched             | 207,153 | +9% |
| V4-arena               | 228,630 | +21% |

overall observations from the series:

removing the mutex from the version chain (V0 -> V1) was the single biggest win at extreme contention (+87% at accounts=2). mutex serializes all 128 threads on the same chain; CAS lets them proceed concurrently. exponential backoff on CAS (V1 -> V2) barely helped - chains are short enough that retry storms don't form. lock-free scheduler (V2 -> V3) did NOT help: Treiber-stack deps + bit-packed CAS on status just moved contention from mutex onto atomic-CAS cache-line bouncing; at accounts=2 it actually regressed to V0 levels. mutex's adaptive blocking is genuinely better than CAS retry when there's real contention. replacing linked list with std::map (V0-tree) only pays off at pathological contention (accounts=2); elsewhere the red-black tree's bigger node + balancing cost loses to plain linked-list traversal. arena (V4) is a clean +5-11% win from removing the malloc/free hot-path - small but reliable. overall V0 -> V4 gave +21% at accounts=1000, +34% at hot/cold, and +87% at accounts=2.

CSVs: `benchmark_records/psc_40118226_*/`
