# Experiments

working notes. what we ran, what we got, what we noticed.

## fixed workload params (don't change across experiments)

- block size = 10000 txs
- reads/writes per tx = 21 / 4  (diem-like)
- compute per tx = 100 sha-256 rounds over the read values (simulates vm + gas)
- runs = 10, take median
- seed fixed so each run is reproducible

seq throughput ends up around 30k tps on PSC, which is faster than real diem (5k) but close enough given we don't actually run move vm.

## versions tracked

| tag | what changed | scheduler | versionchain |
|---|---|---|---|
| V0-mutex | baseline | mutex | mutex |
| V1-lfchain | lock-free version chain w/ pre-built node CAS + exp backoff | mutex | lock-free |
| V2-lfsched | + lock-free scheduler (bit-packed status + Treiber-stack deps, race fixed) | lock-free | lock-free |

each version gets two experiments:

- **exp A - contention sweep**: accounts in {2, 10, 100, 1000, 10000}, threads in {1..128}
- **exp B - dex hot/cold**: 1000 accounts (50 hot + 950 cold), hot_tx_ratio=0.2, hot_keys=1, threads in {1..128}

## log

### exp_?_VX on YYYY-MM-DD, PSC bridges-2, commit abc1234

(template - fill in as we go)

- what we changed since last run:
- what we expected:
- what we got: (points to CSVs under benchmark_records/)
- notes:

---

(older PSC runs before this template existed have been deleted, they used the wrong workload - 2r/2w instead of 21r/4w - and different compute.)
