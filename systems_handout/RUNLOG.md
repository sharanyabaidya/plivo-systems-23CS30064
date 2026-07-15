# RUNLOG.md — Experiment log for real-time UDP relay assignment

All runs: `python run.py --duration 30` (1500 frames at 50 fps).
Overhead = total uplink bytes / (1500 × 160 raw bytes). Cap ≤ 2.00×, miss-rate cap ≤ 1.00%.

---

## Protocol version history

| Version | Key change |
|---------|-----------|
| **v1** | Baseline custom protocol: 163-byte wire (1 type + 2 seq16 + 160 payload), adjacent-pair XOR FEC (0,1),(2,3)…, 128-slot gap table, NACK fired immediately + retry every 25ms, expire 200ms. |
| **v2** | Attempted interleaved FEC stride-4 + 8-byte timestamp. **Broken** — stride-4 FEC lag (80ms+relay) exceeds 45ms/100ms deadlines; 171-byte packets bust 2× cap. |
| **v3-a** | Back to 163-byte, stride-1 FEC, FIX-2 (512-slot gap table, oldest-evict), FIX-3 (30ms grace before first NACK), FIX-4 (retry/expire from DELAY_MS env), FIX-5 (first_nack_ms dedup). Grace=30ms too large for A. |
| **v3-b** | Grace=0 on delay≤60ms (A), grace=20ms on B. Fixed A, broke B on bad seeds. |
| **v3-c** | Grace=0 for all profiles, retry=min(20,delay/4), expire=delay_ms. Both profiles VALID; A seed=3 fails (5ms timing regression: first NACK deferred to service loop). |
| **v3-final** | Grace=0; immediate NACK sent inside gap-detection loop (matching v1 timing); gap service loop handles retries only. **Current code.** |

---

## Run table

### Profile A — `profiles/A.json`, `--delay_ms 45`

| Run | Version | Seed | Drops | Misses | Miss% | Overhead | Result |
|-----|---------|------|-------|--------|-------|----------|--------|
| A-01 | v1 | default | ~50 | 13 | 0.87% | 1.58× | **VALID** |
| A-02 | v2 (broken) | default | 20 | 7 | 1.40% | 2.15× | INVALID |
| A-03 | v3-a (grace=30) | default | 52 | 16 | 1.07% | 1.53× | INVALID |
| A-04 | v3-c (grace=0, no immediate NACK) | default | 54 | 9 | 0.60% | 1.58× | **VALID** |
| A-05 | v3-c | seed=1 | 54 | 10 | 0.67% | 1.57× | **VALID** |
| A-06 | v3-c | seed=2 | 44 | 9 | 0.60% | 1.56× | **VALID** |
| A-07 | v3-c | seed=3 | 52 | 17 | 1.13% | 1.57× | INVALID |
| A-08 | v3-final | seed=1 | 54 | 14 | 0.93% | 1.58× | **VALID** |
| A-09 | v3-final | seed=2 | 45 | 8 | 0.53% | 1.57× | **VALID** |
| A-10 | v3-final | seed=3 | 53 | 17 | 1.13% | 1.57× | INVALID |
| A-11 | v3-final | seed=4 | 37 | 12 | 0.80% | 1.58× | **VALID** |
| A-12 | v3-final | seed=5 | 30 | 10 | 0.67% | 1.57× | **VALID** |

> **Seed=3 analysis:** The immediate-NACK fix (v3-final) produced the same 17 misses as v3-c for seed=3. The 5ms timing improvement was not the bottleneck — seed=3 generates drops in burst patterns where relay delay is high enough that neither FEC nor NACK can recover within 45ms. With 53 drops at 32% miss/drop ratio = 17 misses = 1.13%, this exceeds the 1% cap by exactly 2 misses. This is a physics-limited failure: increasing delay to 50ms (tested below) extends the FEC arrival window from relay≤25ms to relay≤30ms and gives additional headroom.

---

### Profile A — `profiles/A.json`, `--delay_ms 50` (final locked-in delay)

| Run | Version | Seed | Drops | Misses | Miss% | Overhead | Result |
|-----|---------|------|-------|--------|-------|----------|--------|
| A-13 | v3-final | seed=1 | 54 | 8 | 0.53% | 1.58× | **VALID** |
| A-14 | v3-final | seed=2 | 45 | 11 | 0.73% | 1.57× | **VALID** |
| A-15 | v3-final | seed=3 | 53 | 14 | 0.93% | 1.57× | **VALID** |
| A-16 | v3-final | seed=4 | 37 | 10 | 0.67% | 1.58× | **VALID** |
| A-17 | v3-final | seed=5 | 30 | 8 | 0.53% | 1.57× | **VALID** |

> **Why 50ms beats 45ms:** The 5ms extra deadline extends the FEC recovery window: the parity packet for pair(2k, 2k+1) is emitted 20ms after the even frame, so FEC reaches the receiver at `20ms + relay_delay`. At 45ms, FEC is useful only when `relay_delay ≤ 25ms` (≈50% of the relay distribution). At 50ms, the threshold rises to `relay_delay ≤ 30ms` (≈67%). This converts seed=3's 3 un-recoverable misses into recoverable ones, bringing miss% from 1.13% → 0.93%.

| Run | Version | Seed | Drops | Misses | Miss% | Overhead | Result |
|-----|---------|------|-------|--------|-------|----------|--------|
| B-01 | v1 | default | ~115 | 9 | 0.60% | 1.78× | **VALID** |
| B-02 | v2 (broken) | default | 68 | 31 | 6.20% | 2.25× | INVALID |
| B-03 | v3-a (grace=30) | default | 125 | 11 | 0.73% | 1.56× | **VALID** |
| B-04 | v3-b (grace=20) | default | 139 | 19 | 1.27% | 1.60× | INVALID |
| B-05 | v3-c (grace=0) | default | 158 | 10 | 0.67% | 1.75× | **VALID** |
| B-06 | v3-c | seed=1 | 158 | 10 | 0.67% | 1.75× | **VALID** |
| B-07 | v3-c | seed=2 | 145 | 8 | 0.53% | 1.72× | **VALID** |
| B-08 | v3-c | seed=3 | 143 | 10 | 0.67% | 1.75× | **VALID** |

---

### Profile A & B — Adaptive FEC Controller

### Profile A & B — Sliding-Window XOR FEC (Adaptive W size)

| Run | Profile | Delay | Seed | Drops | Misses | Miss% | Overhead | Result |
|-----|---------|-------|------|-------|--------|-------|----------|--------|
| SW-01 | A | 45 ms | default | 60 | 14 | 0.93% | 1.67× | **VALID** |
| SW-02 | A | 45 ms | seed=1 | 59 | 11 | 0.73% | 1.66× | **VALID** |
| SW-03 | A | 45 ms | seed=2 | 48 | 8 | 0.53% | 1.64× | **VALID** |
| SW-04 | A | 45 ms | seed=3 | 54 | 9 | 0.60% | 1.64× | **VALID** |
| SW-05 | B | 100 ms | default | 176 | 12 | 0.80% | 1.86× | **VALID** |
| SW-06 | C | 120 ms | default | 540 | 14 | 0.93% | 1.71× | **VALID** |
| SW-07 | C | 110 ms | default | 546 | 28 | 1.87% | 1.72× | INVALID |
| SW-08 | C | 100 ms | default | 549 | 25 | 1.67% | 1.73× | INVALID |
| SW-09 | C | 80 ms | default | 560 | 63 | 4.20% | 1.74× | INVALID |

> **Note on Profile C performance:** Profile C has a severe 18% loss rate. Because NACK retransmissions require an RTT of up to 80ms (delay max is 40ms), the playout delay must be at least **120 ms** to allow enough time for NACK retransmissions to arrive before the deadline. With a playout delay of 120 ms, the system achieves a valid miss rate of **0.93%** at **1.71× bandwidth overhead** (under the 2.0× cap).

---

## Locked-in parameters

| Parameter | Profile A | Profile B | Profile C |
|-----------|-----------|-----------|-----------|
| `--delay_ms` | **45** | **100** | **120** |
| Wire packet size | 163 B | 163 B | 163 B |
| FEC scheme | Sliding XOR (W=2) | Sliding XOR (W=4) | Sliding XOR (W=4) |
| Feedback Reports | Every 500 ms | Every 500 ms | Every 500 ms |
| Grace before NACK | 0 ms | 0 ms | 0 ms |
| NACK retry interval | 25 ms | 25 ms | 25 ms |
| Gap expiry | 45 ms | 100 ms | 120 ms |
| Gap table slots | 512 | 512 | 512 |
| History buffer | 2048 | 2048 | 2048 |

### Final validated miss rates (Sliding Window Adaptive FEC)

| Profile | Delay | Seeds tested | Worst miss% | All VALID? |
|---------|-------|-------------|------------|----------|
| A | 45ms | 1 (default), 1, 2, 3 | **0.93%** (default) | ✅ Yes |
| B | 100ms | 1 (default) | **0.80%** | ✅ Yes |
| C | 120ms | 1 (default) | **0.93%** | ✅ Yes |

