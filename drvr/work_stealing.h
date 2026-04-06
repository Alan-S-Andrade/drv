// work_stealing.h — Shared lock-free work-stealing primitives for PandoHammer drivers
//
// Provides:
//   - Circular WorkQueue<CAPACITY> with TTAS single-shot queue operations
//   - Sense-reversal barrier
//   - Victim selection with random probing and recently-failed tracking
//   - Range packing utilities and steal statistics
//
// Usage: declare __l2sp__ ws::WorkQueue<N> arrays in your driver, call ws:: functions.
// All queue operations use circular indexing (power-of-2 capacity, monotonic head/tail).
// This eliminates the head/tail reset races present in flat-array implementations.

#pragma once
#include <cstdint>
#include <atomic>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>

namespace ws {

// ===================== Range Packing =====================
// Pack a (begin, end) pair into a single int64_t for compact queue storage.

static inline int64_t pack_range(int32_t begin, int32_t end) {
    return ((int64_t)(uint32_t)begin << 32) | (int64_t)(uint32_t)end;
}
static inline int32_t range_begin(int64_t packed) { return (int32_t)(packed >> 32); }
static inline int32_t range_end(int64_t packed)   { return (int32_t)(packed & 0xFFFFFFFF); }

// ===================== Circular Work Queue =====================
// Lock-free deque with monotonically increasing head/tail counters and
// power-of-2 circular indexing.  Items are accessed via (index & MASK).
//
// Pop/steal operations are single-shot TTAS: one volatile pre-check to
// filter hopeless attempts, then one CAS.  On contention the caller bails
// and retries from its outer loop, which keeps cache-line traffic low.

template <int CAPACITY>
struct WorkQueue {
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of 2");
    static constexpr int64_t MASK = CAPACITY - 1;
    static constexpr int64_t CAP  = CAPACITY;

    volatile int64_t head;
    volatile int64_t tail;
    volatile int64_t items[CAPACITY];
};

// Initialize (single-thread, before use).
template <int C>
static inline void queue_init(WorkQueue<C>* q) {
    q->head = 0;
    q->tail = 0;
}

// Approximate element count (non-atomic snapshot).
template <int C>
static inline int64_t queue_size(WorkQueue<C>* q) {
    int64_t s = q->tail - q->head;
    return s > 0 ? s : 0;
}

// Read the item at a given logical index (handles wrap-around).
// Useful for iterating a queue's contents from a single thread.
template <int C>
static inline int64_t queue_item_at(WorkQueue<C>* q, int64_t index) {
    return q->items[index & WorkQueue<C>::MASK];
}

// Single-writer push.  Not thread-safe — use for hart-0 distribution.
template <int C>
static inline bool queue_push(WorkQueue<C>* q, int64_t item) {
    int64_t t = q->tail;
    if (t - q->head >= C) return false;  // full
    q->items[t & WorkQueue<C>::MASK] = item;
    q->tail = t + 1;
    return true;
}

// Multi-writer atomic push (CAS on tail).  Spins until success or full.
template <int C>
static inline bool queue_push_atomic(WorkQueue<C>* q, int64_t item) {
    while (true) {
        int64_t t = atomic_load_i64(&q->tail);
        if (t - q->head >= C) return false;  // full (approximate)
        int64_t old_t = atomic_compare_and_swap_i64(&q->tail, t, t + 1);
        if (old_t == t) {
            q->items[t & WorkQueue<C>::MASK] = item;
            return true;
        }
        hartsleep(1);
    }
}

// FIFO pop from head (multi-consumer safe).
// Single-shot TTAS: bails on contention.  Returns item or -1.
template <int C>
static inline int64_t queue_pop(WorkQueue<C>* q) {
    // TEST phase — cheap volatile reads
    int64_t h = q->head;
    int64_t t = q->tail;
    if (h >= t) return -1;

    // TAS phase — coherent read + CAS
    h = atomic_load_i64(&q->head);
    t = q->tail;
    if (h >= t) return -1;

    int64_t old_h = atomic_compare_and_swap_i64(&q->head, h, h + 1);
    if (old_h == h) {
        return q->items[h & WorkQueue<C>::MASK];
    }
    return -1;  // contention — caller retries from outer loop
}

// Batch FIFO pop/steal from head.
// Claims up to max_k items in one CAS, copies them into out_buf.
// Returns count (0 if empty or contention).
template <int C>
static inline int64_t queue_pop_chunk(WorkQueue<C>* q, int64_t* out_buf, int64_t max_k) {
    // TEST phase
    int64_t h = q->head;
    int64_t t = q->tail;
    if (h >= t) return 0;

    // TAS phase
    h = atomic_load_i64(&q->head);
    t = q->tail;
    if (h >= t) return 0;

    int64_t avail = t - h;
    int64_t k = (avail < max_k) ? avail : max_k;

    int64_t old_h = atomic_compare_and_swap_i64(&q->head, h, h + k);
    if (old_h != h) return 0;

    // Copy items to caller's buffer (handles wrap-around transparently)
    for (int64_t i = 0; i < k; i++) {
        out_buf[i] = q->items[(h + i) & WorkQueue<C>::MASK];
    }
    return k;
}

// ===================== Barrier (Sense-Reversal) =====================

template <int MAX_HARTS>
struct BarrierState {
    int64_t local_sense[MAX_HARTS];
    std::atomic<int64_t> count;
    std::atomic<int64_t> sense;
};

template <int MH>
static inline void barrier_init(BarrierState<MH>* bs, int total_harts) {
    for (int i = 0; i < total_harts; i++) bs->local_sense[i] = 0;
    bs->count.store(0, std::memory_order_relaxed);
    bs->sense.store(0, std::memory_order_relaxed);
}

template <int MH>
static inline void barrier(BarrierState<MH>* bs, int tid, int total_harts) {
    int64_t local = bs->local_sense[tid] ^ 1;
    bs->local_sense[tid] = local;

    int64_t old = bs->count.fetch_add(1, std::memory_order_acq_rel);
    if (old == total_harts - 1) {
        bs->count.store(0, std::memory_order_relaxed);
        bs->sense.store(local, std::memory_order_release);
    } else {
        long w = 1;
        long wmax = 64 * total_harts;
        while (bs->sense.load(std::memory_order_acquire) != local) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }
}

// ===================== Steal Statistics =====================

struct StealStats {
    int64_t work_processed;
    int64_t steal_attempts;
    int64_t steal_success;
    int64_t steal_failures;
};

static inline void stats_init(StealStats* s) {
    s->work_processed = 0;
    s->steal_attempts = 0;
    s->steal_success  = 0;
    s->steal_failures = 0;
}

// ===================== Victim Selection =====================

// Fast xorshift PRNG — seeded per-hart to decorrelate thieves.
static inline uint32_t xorshift_next(uint32_t seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

// Golden-ratio hash seed from hart id for good dispersion.
static inline uint32_t xorshift_seed(int tid) {
    return (uint32_t)(tid + 1) * 2654435761u;
}

// Pick a random victim core, skipping self, empty queues (if has_work_hints
// is non-null), and recently-failed cores.  Returns core id or -1.
template <int RECENT_SIZE>
static inline int pick_victim(uint32_t* rng, int my_core, int total_cores,
                              const volatile int32_t* has_work_hints,
                              int* recently_tried) {
    for (int tries = 0; tries < total_cores; tries++) {
        *rng = xorshift_next(*rng);
        int victim = (int)(*rng % (uint32_t)total_cores);
        if (victim == my_core) continue;
        if (has_work_hints && has_work_hints[victim] == 0) continue;

        bool in_recent = false;
        for (int i = 0; i < RECENT_SIZE; i++) {
            if (recently_tried[i] == victim) { in_recent = true; break; }
        }
        if (!in_recent) return victim;
    }
    return -1;
}

static inline void clear_recent(int* buf, int size) {
    for (int i = 0; i < size; i++) buf[i] = -1;
}

static inline void record_recent(int* buf, int* idx, int size, int victim) {
    buf[*idx] = victim;
    *idx = (*idx + 1) % size;
}

} // namespace ws
