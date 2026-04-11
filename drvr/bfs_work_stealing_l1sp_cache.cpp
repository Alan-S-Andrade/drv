// Level-synchronous BFS with per-core work stealing (FIFO pop + throttled stealing)
// FIX: level termination uses a global remaining-work counter, not per-hart empty rounds.
// - Each level: next frontier repartitioned evenly across cores
// - Harts pop from local queue; if empty, steal from other cores
// - Neighbors claimed via visited[v] = amoswap.d(&visited[v], 1)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <atomic>

#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>
#include <pandohammer/address.h>
#include <pandohammer/staticdecl.h>

#ifdef NO_STEAL_BASELINE
static constexpr const char* BENCH_TITLE = "BFS with L1SP Work Cache Baseline (No Stealing)";
static constexpr const char* BENCH_LOG_PREFIX = "run_l1sp_cache_baseline";
#else
static constexpr const char* BENCH_TITLE = "BFS with L1SP Work Cache + Adaptive Work Stealing";
static constexpr const char* BENCH_LOG_PREFIX = "run_l1sp_cache";
#endif

static constexpr int MAX_HARTS  = 1024;
static constexpr int MAX_CORES  = 64;
static constexpr int MAX_HARTS_PER_CORE = 16;
static constexpr int BFS_CHUNK_SIZE = 64;   // nodes per batch local pop
static constexpr int STEAL_K_MIN = 64;      // minimum nodes per steal
static constexpr int STEAL_K_MAX = 256;     // maximum nodes per steal
static constexpr int STEAL_VICTIMS_MIN = 2; // minimum victims probed per steal episode
static constexpr int STEAL_VICTIMS_MAX = 8; // maximum victims probed per steal episode
static constexpr int L1_CACHE_GUARD_BYTES = 5 * 1024;
static constexpr int L1_CACHE_MAX_ITEMS = 2048;
static constexpr int L1_SPILL_MAX_ITEMS = 4096;

static constexpr int INITIAL_SKEW_WEIGHT = 20;

__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

struct WorkQueue {
    volatile int64_t head;              // pop/steal reservation (CAS)
    volatile int64_t tail;              // one-past-last valid item
    int64_t start_idx;                  // logical start in current frontier storage
};

__l2sp__ WorkQueue core_queues[MAX_CORES];

struct FrontierBuffer {
    volatile int64_t tail;
    int64_t capacity;
    int64_t l2sp_capacity;
    int64_t* l2sp_items;
    int64_t* dram_items;
    int32_t* l2sp_owner_core;
    int32_t* dram_owner_core;
};

__l2sp__ FrontierBuffer g_next_frontier;
__l2sp__ int64_t *g_current_frontier_storage = nullptr;
__l2sp__ int64_t *g_current_frontier_dram_storage = nullptr;

__l2sp__ int64_t g_local_sense[MAX_HARTS];

// best-effort hint: 1 if queue likely non-empty, 0 if likely empty
__l2sp__ volatile int32_t core_has_work[MAX_CORES];
__l2sp__ volatile int32_t core_steal_batch_size[MAX_CORES];
__l2sp__ volatile int32_t core_steal_victim_probes[MAX_CORES];
__l2sp__ volatile int32_t core_last_steal_feedback[MAX_CORES];
__l2sp__ volatile uint64_t g_core_l1sp_bytes = 0;
__l2sp__ volatile uint64_t g_hart_stack_capacity_bytes = 0;
__l2sp__ volatile uint64_t g_hart_stack_live_bytes[MAX_HARTS];
__l2sp__ volatile uint64_t g_hart_stack_peak_bytes[MAX_HARTS];

__dram__ int64_t g_core_spill_items[MAX_CORES][L1_SPILL_MAX_ITEMS];
__l2sp__ volatile int64_t g_core_spill_count[MAX_CORES];
__l2sp__ volatile int64_t stat_nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_attempts[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_success[MAX_HARTS];
__l2sp__ volatile int64_t discovered = 0;
__l2sp__ volatile int64_t stat_l1_cache_items_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_spill_items_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_queue_refill_items_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_stolen_items_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_items[MAX_HARTS];
__l2sp__ volatile int64_t stat_idle_wait_cycles[MAX_HARTS];
__l2sp__ volatile int64_t stat_l1_refill_success[MAX_CORES];
__l2sp__ volatile int64_t stat_l1_refill_items[MAX_CORES];
__l2sp__ volatile int64_t stat_l1_refill_cycles[MAX_CORES];
__l2sp__ volatile int64_t stat_l1_spill_items[MAX_CORES];
__l2sp__ volatile int64_t stat_l1_spill_cycles[MAX_CORES];
__l2sp__ volatile int64_t stat_l1_cache_occupancy_samples[MAX_CORES];
__l2sp__ volatile int64_t stat_l1_cache_occupancy_sum[MAX_CORES];
__l2sp__ volatile int64_t stat_l1_cache_occupancy_max[MAX_CORES];
__l2sp__ volatile int64_t stat_edges_traversed = 0;

// Per-core steal token lives in each core's L1SP.
// All 16 harts on a core share the same L1SP and compete for the
// token via CAS.
static constexpr uintptr_t THIEF_TOKEN_L1SP_OFFSET = 0; // 8 bytes at L1SP base
static constexpr uintptr_t REFILL_TOKEN_L1SP_OFFSET = 8;

struct L1WorkCacheHeader {
    volatile int64_t count;
    volatile int64_t capacity_items;
    volatile int64_t max_capacity_items;
    volatile int64_t spills;
    volatile int64_t evictions;
};

static constexpr uintptr_t L1_WORK_CACHE_HEADER_OFFSET = 64;
static constexpr uintptr_t L1_WORK_CACHE_ITEMS_OFFSET =
    L1_WORK_CACHE_HEADER_OFFSET + sizeof(L1WorkCacheHeader);

// Get pointer to this core's thief token in L1SP
static inline volatile int64_t* thief_token_ptr() {
    uintptr_t l1sp_base;
    asm volatile("csrr %0, " __stringify(MCSR_L1SPBASE) : "=r"(l1sp_base));
    return (volatile int64_t *)(l1sp_base + THIEF_TOKEN_L1SP_OFFSET);
}

// Try to acquire the thief token via CAS (0 -> 1).  Returns true if acquired.
static inline bool thief_token_try_acquire() {
    volatile int64_t *tok = thief_token_ptr();
    return atomic_compare_and_swap_i64(tok, 0, 1) == 0;
}

// Release the thief token (store 0).
static inline void thief_token_release() {
    volatile int64_t *tok = thief_token_ptr();
    *tok = 0;
}

static inline volatile int64_t* refill_token_ptr() {
    uintptr_t l1sp_base;
    asm volatile("csrr %0, " __stringify(MCSR_L1SPBASE) : "=r"(l1sp_base));
    return (volatile int64_t *)(l1sp_base + REFILL_TOKEN_L1SP_OFFSET);
}

static inline bool refill_token_try_acquire() {
    volatile int64_t *tok = refill_token_ptr();
    return atomic_compare_and_swap_i64(tok, 0, 1) == 0;
}

static inline void refill_token_release() {
    volatile int64_t *tok = refill_token_ptr();
    *tok = 0;
}

// Iniialize the thief token (called once per core at startup).
static inline void thief_token_init() {
    volatile int64_t *tok = thief_token_ptr();
    *tok = 0;
}

static inline L1WorkCacheHeader* l1_work_cache_header_ptr() {
    uintptr_t l1sp_base;
    asm volatile("csrr %0, " __stringify(MCSR_L1SPBASE) : "=r"(l1sp_base));
    return (L1WorkCacheHeader *)(l1sp_base + L1_WORK_CACHE_HEADER_OFFSET);
}

static inline int64_t* l1_work_cache_items_ptr() {
    uintptr_t l1sp_base;
    asm volatile("csrr %0, " __stringify(MCSR_L1SPBASE) : "=r"(l1sp_base));
    return (int64_t *)(l1sp_base + L1_WORK_CACHE_ITEMS_OFFSET);
}

static inline void l1_work_cache_init() {
    L1WorkCacheHeader* hdr = l1_work_cache_header_ptr();
    const uint64_t per_hart_slot = g_hart_stack_capacity_bytes;
    int64_t max_items = 0;
    if (per_hart_slot > (uint64_t)(L1_WORK_CACHE_ITEMS_OFFSET + L1_CACHE_GUARD_BYTES)) {
        max_items = (int64_t)((per_hart_slot - (uint64_t)L1_WORK_CACHE_ITEMS_OFFSET -
                               (uint64_t)L1_CACHE_GUARD_BYTES) / sizeof(int64_t));
        if (max_items > L1_CACHE_MAX_ITEMS) max_items = L1_CACHE_MAX_ITEMS;
    }
    hdr->count = 0;
    hdr->capacity_items = max_items;
    hdr->max_capacity_items = max_items;
    hdr->spills = 0;
    hdr->evictions = 0;

    volatile int64_t *tok = refill_token_ptr();
    *tok = 0;
}

// Global remaining-work counter for the current level
__l2sp__ std::atomic<int64_t> g_level_remaining = 0;

// -------------------- Barrier --------------------
__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;

static inline int get_thread_id() {
    // Assumes <= 16 harts/core as in your environment; if not, change this encoding.
    return (myCoreId() << 4) + myThreadId();
}

static inline void spin_pause(int64_t iters) {
    for (int64_t i = 0; i < iters; i++) {
        asm volatile("" ::: "memory");
    }
}

static inline void wait_no_sleep_if_level_work(int64_t backoff) {
    if (g_level_remaining.load(std::memory_order_acquire) > 0) {
        spin_pause(backoff);
    } else {
        hartsleep(backoff);
    }
}

// Like wait_no_sleep_if_level_work but also never sleeps if our core's queue
// has work (e.g. a thief just pushed items into it).
static inline void wait_check_local(int64_t backoff, int my_core) {
    if (core_has_work[my_core]) {
        return;  // local work appeared, get back to popping immediately
    } else if (g_level_remaining.load(std::memory_order_acquire) > 0) {
        spin_pause(backoff);
    } else {
        hartsleep(backoff);
    }
}

static inline void barrier() {
    int tid = get_thread_id();
    int total = g_total_harts;

    int64_t local = g_local_sense[tid];
    local ^= 1;
    g_local_sense[tid] = local;

    int64_t old = g_count.fetch_add(1, std::memory_order_acq_rel);
    if (old == total - 1) {
        g_count.store(0, std::memory_order_relaxed);
        g_sense.store(local, std::memory_order_release);
    } else {
        long w = 1;
        long wmax = 64 * total;
        while (g_sense.load(std::memory_order_acquire) != local) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }
}

static inline void queue_init(WorkQueue* q) {
    q->head = 0;
    q->tail = 0;
    q->start_idx = 0;
}

static inline int64_t queue_depth(WorkQueue* q) {
    int64_t depth = atomic_load_i64(&q->tail) - atomic_load_i64(&q->head);
    return (depth > 0) ? depth : 0;
}

static inline int64_t* frontier_current_ptr(int64_t idx) {
    return (idx < g_next_frontier.l2sp_capacity)
        ? (g_current_frontier_storage + idx)
        : (g_current_frontier_dram_storage + (idx - g_next_frontier.l2sp_capacity));
}

static inline int64_t frontier_current_get(int64_t idx) {
    return *frontier_current_ptr(idx);
}

static inline void frontier_current_set(int64_t idx, int64_t value) {
    *frontier_current_ptr(idx) = value;
}

static inline int64_t* frontier_next_item_ptr(int64_t idx) {
    return (idx < g_next_frontier.l2sp_capacity)
        ? (g_next_frontier.l2sp_items + idx)
        : (g_next_frontier.dram_items + (idx - g_next_frontier.l2sp_capacity));
}

static inline int32_t* frontier_next_owner_ptr(int64_t idx) {
    return (idx < g_next_frontier.l2sp_capacity)
        ? (g_next_frontier.l2sp_owner_core + idx)
        : (g_next_frontier.dram_owner_core + (idx - g_next_frontier.l2sp_capacity));
}

static inline int64_t frontier_next_item_get(int64_t idx) {
    return *frontier_next_item_ptr(idx);
}

static inline void frontier_next_item_set(int64_t idx, int64_t value) {
    *frontier_next_item_ptr(idx) = value;
}

static inline int32_t frontier_next_owner_get(int64_t idx) {
    return *frontier_next_owner_ptr(idx);
}

static inline void frontier_next_owner_set(int64_t idx, int32_t value) {
    *frontier_next_owner_ptr(idx) = value;
}

static inline void queue_assign_slice(WorkQueue* q, int64_t start_idx, int64_t count) {
    q->start_idx = start_idx;
    q->head = 0;
    q->tail = count;
}

static inline bool next_frontier_push_atomic(int64_t work) {
    int64_t idx = atomic_fetch_add_i64(&g_next_frontier.tail, 1);
    if (idx >= g_next_frontier.capacity) {
        return false;
    }
    frontier_next_item_set(idx, work);
    frontier_next_owner_set(idx, myCoreId());
    return true;
}

// Linearizable pop/steal: reserve index by CAS on head (FIFO)
static inline int64_t queue_pop_fifo(WorkQueue* q, int core_id) {
    int64_t h = atomic_load_i64(&q->head);
    int64_t t = atomic_load_i64(&q->tail);

    if (h >= t) {
        core_has_work[core_id] = 0; // best-effort empty hint
        return -1;
    }

    int64_t old_h = atomic_compare_and_swap_i64(&q->head, h, h + 1);
    if (old_h == h) {
        return frontier_current_get(q->start_idx + h);
    }
    return -1;
}

// Batch pop: reserve up to max_chunk items from head in one CAS.
// Returns count of items claimed (0 if empty/contention). Sets *out_begin.
static inline int64_t queue_pop_chunk(WorkQueue* q, int core_id, int64_t* out_begin, int64_t max_chunk) {
    int64_t h = atomic_load_i64(&q->head);
    int64_t t = atomic_load_i64(&q->tail);
    if (h >= t) {
        core_has_work[core_id] = 0;
        return 0;
    }
    int64_t avail = t - h;
    int64_t chunk = (avail < max_chunk) ? avail : max_chunk;
    int64_t old_h = atomic_compare_and_swap_i64(&q->head, h, h + chunk);
    if (old_h == h) {
        *out_begin = h;
        return chunk;
    }
    return 0;
}

static inline void adapt_steal_policy(int my_core, int64_t victim_depth_before, int64_t stolen_count) {
    int32_t batch = core_steal_batch_size[my_core];
    int32_t probes = core_steal_victim_probes[my_core];
    int32_t feedback = 0;

    if (victim_depth_before >= 4 * stolen_count && stolen_count == batch) {
        feedback = 1; // last steal was too small for a deep victim
    } else if (victim_depth_before <= stolen_count + (stolen_count >> 1)) {
        feedback = -1; // last steal drained most of the victim queue
    }

    if (feedback > 0) {
        if (batch < STEAL_K_MAX) {
            batch <<= 1;
            if (batch > STEAL_K_MAX) batch = STEAL_K_MAX;
        } else if (probes < STEAL_VICTIMS_MAX) {
            probes++;
        }
    } else if (feedback < 0) {
        if (probes > STEAL_VICTIMS_MIN) {
            probes--;
        } else if (batch > STEAL_K_MIN) {
            batch >>= 1;
            if (batch < STEAL_K_MIN) batch = STEAL_K_MIN;
        }
    }

    core_steal_batch_size[my_core] = batch;
    core_steal_victim_probes[my_core] = probes;
    core_last_steal_feedback[my_core] = feedback;
}

static inline bool spill_push(int core_id, int64_t value) {
    int64_t idx = atomic_fetch_add_i64(&g_core_spill_count[core_id], 1);
    if (idx >= L1_SPILL_MAX_ITEMS) {
        atomic_fetch_add_i64(&g_core_spill_count[core_id], -1);
        return false;
    }
    g_core_spill_items[core_id][idx] = value;
    return true;
}

static inline int64_t spill_pop_chunk(int core_id, int64_t* out, int64_t max_chunk) {
    while (true) {
        int64_t count = atomic_load_i64(&g_core_spill_count[core_id]);
        if (count <= 0) return 0;
        int64_t chunk = (count < max_chunk) ? count : max_chunk;
        int64_t new_count = count - chunk;
        if (atomic_compare_and_swap_i64(&g_core_spill_count[core_id], count, new_count) == count) {
            for (int64_t i = 0; i < chunk; i++) {
                out[i] = g_core_spill_items[core_id][new_count + i];
            }
            return chunk;
        }
    }
}

static inline int64_t l1_cache_pop_chunk(int64_t* out, int64_t max_chunk) {
    L1WorkCacheHeader* hdr = l1_work_cache_header_ptr();
    int64_t* items = l1_work_cache_items_ptr();
    while (true) {
        int64_t count = atomic_load_i64(&hdr->count);
        if (count <= 0) return 0;
        int64_t chunk = (count < max_chunk) ? count : max_chunk;
        int64_t new_count = count - chunk;
        if (atomic_compare_and_swap_i64(&hdr->count, count, new_count) == count) {
            for (int64_t i = 0; i < chunk; i++) {
                out[i] = items[new_count + i];
            }
            return chunk;
        }
    }
}

static inline bool l1_cache_push_batch(const int64_t* values, int64_t count) {
    L1WorkCacheHeader* hdr = l1_work_cache_header_ptr();
    int64_t* items = l1_work_cache_items_ptr();
    int64_t cur = hdr->count;
    int64_t cap = hdr->capacity_items;
    if (count <= 0 || cur + count > cap) return false;
    for (int64_t i = 0; i < count; i++) {
        items[cur + i] = values[i];
    }
    hdr->count = cur + count;
    return true;
}

static inline void monitor_l1_work_cache(int core_id) {
    if (myThreadId() != 0) return;
    if (!refill_token_try_acquire()) return;

    uint64_t start_cycles = 0;
    asm volatile("rdcycle %0" : "=r"(start_cycles));

    L1WorkCacheHeader* hdr = l1_work_cache_header_ptr();
    int64_t* items = l1_work_cache_items_ptr();
    const int bottom_hart = core_id * g_harts_per_core + (g_harts_per_core - 1);
    const uint64_t bottom_used = g_hart_stack_live_bytes[bottom_hart];
    int64_t safe_bytes = 0;
    if (g_hart_stack_capacity_bytes > bottom_used + (uint64_t)L1_CACHE_GUARD_BYTES +
                                     (uint64_t)L1_WORK_CACHE_ITEMS_OFFSET) {
        safe_bytes = (int64_t)(g_hart_stack_capacity_bytes - bottom_used -
                               (uint64_t)L1_CACHE_GUARD_BYTES -
                               (uint64_t)L1_WORK_CACHE_ITEMS_OFFSET);
    }
    int64_t safe_items = safe_bytes / (int64_t)sizeof(int64_t);
    if (safe_items > hdr->max_capacity_items) safe_items = hdr->max_capacity_items;
    if (safe_items < 0) safe_items = 0;
    hdr->capacity_items = safe_items;

    while (hdr->count > hdr->capacity_items) {
        const int64_t idx = hdr->count - 1;
        const int64_t work = items[idx];
        hdr->count = idx;
        hdr->evictions++;
        if (!spill_push(core_id, work)) {
            std::printf("ERROR: L1 spill overflow on core %d\n", core_id);
            std::abort();
        }
        hdr->spills++;
        stat_l1_spill_items[core_id]++;
    }

    uint64_t end_cycles = 0;
    asm volatile("rdcycle %0" : "=r"(end_cycles));
    stat_l1_spill_cycles[core_id] += (int64_t)(end_cycles - start_cycles);
    refill_token_release();
}

static inline bool refill_l1_work_cache_from_queue(WorkQueue* q, int core_id, int64_t fetch_limit) {
    if (!refill_token_try_acquire()) return false;

    uint64_t start_cycles = 0;
    asm volatile("rdcycle %0" : "=r"(start_cycles));

    L1WorkCacheHeader* hdr = l1_work_cache_header_ptr();
    const int64_t room = hdr->capacity_items - hdr->count;
    if (room <= 0) {
        uint64_t end_cycles = 0;
        asm volatile("rdcycle %0" : "=r"(end_cycles));
        stat_l1_refill_cycles[core_id] += (int64_t)(end_cycles - start_cycles);
        refill_token_release();
        return false;
    }

    int64_t begin_idx = 0;
    int64_t fetch = fetch_limit;
    if (fetch > room) fetch = room;
    int64_t count = queue_pop_chunk(q, core_id, &begin_idx, fetch);
    if (count <= 0) {
        uint64_t end_cycles = 0;
        asm volatile("rdcycle %0" : "=r"(end_cycles));
        stat_l1_refill_cycles[core_id] += (int64_t)(end_cycles - start_cycles);
        refill_token_release();
        return false;
    }

    int64_t tmp[STEAL_K_MAX];
    for (int64_t i = 0; i < count; i++) {
        tmp[i] = frontier_current_get(q->start_idx + begin_idx + i);
    }
    const bool ok = l1_cache_push_batch(tmp, count);
    uint64_t end_cycles = 0;
    asm volatile("rdcycle %0" : "=r"(end_cycles));
    stat_l1_refill_cycles[core_id] += (int64_t)(end_cycles - start_cycles);
    if (ok) {
        stat_l1_refill_success[core_id]++;
        stat_l1_refill_items[core_id] += count;
    }
    refill_token_release();
    return ok;
}

// -------------------- BFS Shared State --------------------
// Pointers to dynamically-loaded CSR graph (into a malloc'd DRAM buffer)
__l2sp__ int32_t *g_row_ptr    = nullptr;
__l2sp__ int32_t *g_col_idx    = nullptr;
__l2sp__ volatile int64_t *visited = nullptr;
__l2sp__ int32_t *dist_arr     = nullptr;
__l2sp__ char    *g_file_buffer = nullptr;
__l2sp__ int32_t  g_num_vertices = 0;
__l2sp__ int32_t  g_num_edges    = 0;
__l2sp__ int32_t  g_bfs_source   = 0;
__l2sp__ int32_t  g_dist_in_l2sp = 0;
__l2sp__ int32_t  g_visited_in_l2sp = 0;
__l2sp__ int32_t  g_frontiers_in_l2sp = 0;

static constexpr int MAX_WQ_TRACE_SAMPLES = 32768;

enum WQTracePhase : int32_t {
    WQ_PHASE_INIT = 0,
    WQ_PHASE_LEVEL_BEGIN = 1,
    WQ_PHASE_POST_PROCESS = 2,
    WQ_PHASE_POST_REDISTRIBUTE = 3,
    WQ_PHASE_FINAL = 4
};

struct WQTraceSample {
    int32_t level;
    int32_t phase;
};

__dram__ WQTraceSample g_wq_trace_samples[MAX_WQ_TRACE_SAMPLES];
__dram__ int32_t g_wq_trace_depths[MAX_WQ_TRACE_SAMPLES][MAX_CORES];
__dram__ uint64_t g_l1sp_trace_total_bytes[MAX_WQ_TRACE_SAMPLES];
__dram__ uint64_t g_l1sp_trace_core0_hart_bytes[MAX_WQ_TRACE_SAMPLES][MAX_HARTS_PER_CORE];
__l2sp__ volatile int32_t g_wq_trace_count = 0;
__l2sp__ volatile int32_t g_wq_trace_dropped = 0;

// ---- Fine-grained work-queue snapshots (inside process_bfs_level) ----
// Each hart records a snapshot every SNAP_EVERY batches it processes.
// A global atomic counter allocates slots; once full, snaps are silently dropped.
static constexpr int MAX_WQ_SNAPS  = 4096;
static constexpr int SNAP_EVERY    = 4;  // record every Nth batch per hart

enum WQSnapEvent : int32_t {
    WQ_SNAP_POP   = 0,
    WQ_SNAP_STEAL = 1,
};

struct WQSnapMeta {
    int32_t level;
    int32_t event;       // WQSnapEvent
    int32_t actor_core;  // core that popped / stole
    int32_t pad;
};

__dram__ WQSnapMeta  g_wq_snap_meta[MAX_WQ_SNAPS];
__dram__ int32_t     g_wq_snap_depths[MAX_WQ_SNAPS][MAX_CORES];
__dram__ uint32_t    g_wq_snap_stack_bytes[MAX_WQ_SNAPS][MAX_HARTS]; // per-hart stack usage at each snap
__l2sp__ volatile int32_t g_wq_snap_count = 0;

extern "C" char l2sp_end[];

static inline uintptr_t align_up_uintptr(uintptr_t value, uintptr_t align) {
    return (value + align - 1) & ~(align - 1);
}

static void* try_alloc_l2sp(uintptr_t* heap, size_t bytes, size_t align, uintptr_t l2sp_limit) {
    const uintptr_t start = align_up_uintptr(*heap, (uintptr_t)align);
    const uintptr_t end = start + (uintptr_t)bytes;
    if (end > l2sp_limit) {
        return nullptr;
    }
    *heap = end;
    return (void*)start;
}

static inline bool claim_node(int64_t v) {
    const int64_t old = atomic_swap_i64((volatile int64_t *)&visited[v], 1);
    return old == 0;
}

static inline const char* wq_phase_name(int32_t phase) {
    switch (phase) {
    case WQ_PHASE_INIT: return "init";
    case WQ_PHASE_LEVEL_BEGIN: return "level_begin";
    case WQ_PHASE_POST_PROCESS: return "post_process";
    case WQ_PHASE_POST_REDISTRIBUTE: return "post_redistribute";
    case WQ_PHASE_FINAL: return "final";
    default: return "unknown";
    }
}

static void record_wq_trace(int32_t phase, int32_t level) {
    int32_t idx = g_wq_trace_count;
    if (idx >= MAX_WQ_TRACE_SAMPLES) {
        g_wq_trace_dropped++;
        return;
    }
    g_wq_trace_count = idx + 1;

    g_wq_trace_samples[idx] = {level, phase};
    for (int c = 0; c < g_total_cores; c++) {
        int64_t depth = core_queues[c].tail - core_queues[c].head;
        if (depth < 0) depth = 0;
        g_wq_trace_depths[idx][c] = (int32_t)depth;
    }
}

static inline uint64_t current_stack_usage_bytes() {
    uintptr_t sp = 0;
    asm volatile("mv %0, sp" : "=r"(sp));

    const uint64_t hart_capacity = g_hart_stack_capacity_bytes;
    uint64_t l1sp_base = 0;
    asm volatile("csrr %0, " __stringify(MCSR_L1SPBASE) : "=r"(l1sp_base));
    const uint64_t stack_top =
        l1sp_base + (uint64_t)g_core_l1sp_bytes - ((uint64_t)myThreadId() * hart_capacity);

    if ((uint64_t)sp >= stack_top || hart_capacity == 0) {
        return 0;
    }

    uint64_t used = stack_top - (uint64_t)sp;
    if (used > hart_capacity) {
        used = hart_capacity;
    }
    return used;
}

// Record a fine-grained snapshot of all core queue depths and
// the calling hart's stack usage.  Other harts' stack bytes are
// left as 0 for this slot (they record their own when *they* snap).
static inline void record_wq_snap(int32_t level, WQSnapEvent event,
                                  int32_t actor_core, int tid) {
    int32_t idx = atomic_fetch_add_i32((volatile int32_t *)&g_wq_snap_count, 1);
    if (idx >= MAX_WQ_SNAPS) return;
    g_wq_snap_meta[idx] = {level, (int32_t)event, actor_core, 0};
    for (int c = 0; c < g_total_cores; c++) {
        int64_t depth = core_queues[c].tail - core_queues[c].head;
        if (depth < 0) depth = 0;
        g_wq_snap_depths[idx][c] = (int32_t)depth;
    }
    // Record every hart's current stack usage
    // Only the calling hart is guaranteed accurate; others are best-effort reads.
    g_wq_snap_stack_bytes[idx][tid] = (uint32_t)current_stack_usage_bytes();
}

static void record_l1sp_trace_sample(int tid) {
    const int32_t idx = g_wq_trace_count - 1;
    if (idx < 0 || idx >= MAX_WQ_TRACE_SAMPLES) {
        return;
    }

    const uint64_t used = current_stack_usage_bytes();
    g_hart_stack_peak_bytes[tid] = (used > g_hart_stack_peak_bytes[tid])
        ? used
        : g_hart_stack_peak_bytes[tid];
    atomic_fetch_add_i64((volatile int64_t *)&g_l1sp_trace_total_bytes[idx], (int64_t)used);

    if (myCoreId() == 0 && myThreadId() < MAX_HARTS_PER_CORE) {
        g_l1sp_trace_core0_hart_bytes[idx][myThreadId()] = used;
    }
}

static void dump_wq_trace() {
    const uint64_t core_l1sp_bytes = g_core_l1sp_bytes;
    const uint64_t global_l1sp_bytes = core_l1sp_bytes * (uint64_t)g_total_cores;

    auto emit = [&](FILE* out) {
        std::fprintf(out, "WQTRACE_DUMP_BEGIN,bench=bfs_work_stealing_l1sp_cache,cores=%d,samples=%d,dropped=%d\n",
                     g_total_cores, (int)g_wq_trace_count, (int)g_wq_trace_dropped);
        for (int32_t i = 0; i < g_wq_trace_count; i++) {
            const WQTraceSample& s = g_wq_trace_samples[i];
            std::fprintf(out, "WQTRACE,bench=bfs_work_stealing_l1sp_cache,cores=%d,sample=%d,phase=%s,level=%d,iter=-1,queue=core,depths=",
                         g_total_cores, (int)i, wq_phase_name(s.phase), (int)s.level);
            for (int c = 0; c < g_total_cores; c++) {
                if (c > 0) std::fprintf(out, "|");
                std::fprintf(out, "%d", (int)g_wq_trace_depths[i][c]);
            }
            std::fprintf(out, "\n");
        }
        std::fprintf(out, "WQTRACE_DUMP_END,bench=bfs_work_stealing_l1sp_cache\n");

        std::fprintf(out,
                     "L1SPTRACE_DUMP_BEGIN,bench=bfs_work_stealing_l1sp_cache,cores=%d,harts=%d,samples=%d\n",
                     g_total_cores, g_total_harts, (int)g_wq_trace_count);
        std::fprintf(out,
                     "L1SPTRACE_CONFIG,bench=bfs_work_stealing_l1sp_cache,core_bytes=%lu,global_bytes=%lu\n",
                     (unsigned long)core_l1sp_bytes, (unsigned long)global_l1sp_bytes);
        for (int32_t i = 0; i < g_wq_trace_count; i++) {
            const WQTraceSample& s = g_wq_trace_samples[i];
            std::fprintf(out,
                         "L1SPTRACE_GLOBAL,bench=bfs_work_stealing_l1sp_cache,sample=%d,phase=%s,level=%d,iter=-1,bytes=%lu\n",
                         (int)i, wq_phase_name(s.phase), (int)s.level,
                         (unsigned long)g_l1sp_trace_total_bytes[i]);
            for (int t = 0; t < g_harts_per_core && t < MAX_HARTS_PER_CORE; t++) {
                std::fprintf(out,
                             "L1SPTRACE_CORE_HART,bench=bfs_work_stealing_l1sp_cache,sample=%d,core=0,thread=%d,hart=%d,bytes=%lu\n",
                             (int)i, t, t,
                             (unsigned long)g_l1sp_trace_core0_hart_bytes[i][t]);
            }
        }
        for (int h = 0; h < g_total_harts; h++) {
            std::fprintf(out,
                         "L1SPTRACE_HART,bench=bfs_work_stealing_l1sp_cache,hart=%d,core=%d,thread=%d,bytes=%lu\n",
                         h, h / g_harts_per_core, h % g_harts_per_core,
                         (unsigned long)g_hart_stack_peak_bytes[h]);
        }
        std::fprintf(out, "L1SPTRACE_DUMP_END,bench=bfs_work_stealing_l1sp_cache\n");

        // ---- Fine-grained work-queue snapshots ----
        int32_t snap_total = g_wq_snap_count;
        if (snap_total > MAX_WQ_SNAPS) snap_total = MAX_WQ_SNAPS;
        std::fprintf(out,
                     "WQSNAP_DUMP_BEGIN,bench=bfs_work_stealing_l1sp_cache,cores=%d,snaps=%d,capacity=%d\n",
                     g_total_cores, (int)snap_total, MAX_WQ_SNAPS);
        for (int32_t i = 0; i < snap_total; i++) {
            const WQSnapMeta& m = g_wq_snap_meta[i];
            std::fprintf(out,
                         "WQSNAP,bench=bfs_work_stealing_l1sp_cache,cores=%d,idx=%d,level=%d,event=%s,actor_core=%d,depths=",
                         g_total_cores, (int)i, (int)m.level,
                         (m.event == WQ_SNAP_STEAL) ? "steal" : "pop",
                         (int)m.actor_core);
            for (int c = 0; c < g_total_cores; c++) {
                if (c > 0) std::fprintf(out, "|");
                std::fprintf(out, "%d", (int)g_wq_snap_depths[i][c]);
            }
            std::fprintf(out, "\n");
        }
        std::fprintf(out, "WQSNAP_DUMP_END,bench=bfs_work_stealing_l1sp_cache\n");

        // ---- Fine-grained L1SP stack snapshots (one per WQSNAP) ----
        const int hpc = g_harts_per_core;
        const int tc = g_total_cores;
        const int th = g_total_harts;
        std::fprintf(out,
                     "L1SPSNAP_DUMP_BEGIN,bench=bfs_work_stealing_l1sp_cache,cores=%d,harts_per_core=%d,harts=%d,snaps=%d\n",
                     tc, hpc, th, (int)snap_total);
        for (int32_t i = 0; i < snap_total; i++) {
            const WQSnapMeta& m = g_wq_snap_meta[i];
            // Per-core aggregate: sum of all harts' stack bytes on that core
            for (int c = 0; c < tc; c++) {
                uint64_t core_sum = 0;
                for (int t = 0; t < hpc; t++) {
                    int h = c * hpc + t;
                    core_sum += g_wq_snap_stack_bytes[i][h];
                }
                std::fprintf(out,
                             "L1SPSNAP_CORE,bench=bfs_work_stealing_l1sp_cache,idx=%d,level=%d,core=%d,bytes=%lu\n",
                             (int)i, (int)m.level, c, (unsigned long)core_sum);
            }
            // Per-hart detail for all cores
            for (int h = 0; h < th; h++) {
                uint32_t b = g_wq_snap_stack_bytes[i][h];
                if (b == 0) continue;  // skip zeros to reduce output
                std::fprintf(out,
                             "L1SPSNAP_HART,bench=bfs_work_stealing_l1sp_cache,idx=%d,level=%d,core=%d,thread=%d,hart=%d,bytes=%u\n",
                             (int)i, (int)m.level, h / hpc, h % hpc, h, (unsigned)b);
            }
        }
        std::fprintf(out, "L1SPSNAP_DUMP_END,bench=bfs_work_stealing_l1sp_cache\n");
    };

    emit(stdout);

    char path[64];
    std::snprintf(path, sizeof(path), "%s_%dcores.log", BENCH_LOG_PREFIX, g_total_cores);
    FILE* fp = std::fopen(path, "w");
    if (fp != nullptr) {
        emit(fp);
        std::fclose(fp);
        std::printf("WQTRACE_FILE_WRITTEN,bench=%s,path=%s\n", BENCH_LOG_PREFIX, path);
    } else {
        std::printf("WQTRACE_FILE_ERROR,bench=%s,path=%s\n", BENCH_LOG_PREFIX, path);
    }
}

// Returns true on success, false on failure.
static bool load_graph_from_file() {
    // File layout (all int32 little-endian, same as gen_rmat.py):
    //   header [5 x int32]: N, num_edges, degree, unused, source
    //   offsets: (N+1) x int32  -- CSR row pointers
    //   edges:   num_edges x int32 -- CSR column indices
    //   degrees: N x int32
    //
    // IMPORTANT: filename must be on stack (L1SP) — not a string literal
    // (.rodata lives in DRAM which may not be flushed to backing store yet).
    char fname[32];
    const char *src = "rmat_r16.bin";
    int fi = 0;
    while (src[fi]) { fname[fi] = src[fi]; fi++; }
    fname[fi] = '\0';

    int32_t header[5];
    struct ph_bulk_load_desc header_desc;
    header_desc.filename_addr = (long)fname;
    header_desc.dest_addr     = (long)header;
    header_desc.size          = (long)sizeof(header);
    header_desc.result        = 0;
    ph_bulk_load_file(&header_desc);

    if (header_desc.result <= 0) {
        std::printf("ERROR: bulk load of %s header failed (result=%ld)\n",
                    src, header_desc.result);
        return false;
    }

    int hdr_N      = header[0];
    int hdr_E      = header[1];
    int hdr_D      = header[2];
    int hdr_source = header[4];

    if (hdr_N <= 0 || hdr_E < 0) {
        std::printf("ERROR: invalid CSR header in %s: N=%d E=%d D=%d source=%d\n",
                    src, hdr_N, hdr_E, hdr_D, hdr_source);
        return false;
    }

    const size_t file_size = 20
        + (size_t)(hdr_N + 1) * sizeof(int32_t)
        + (size_t)hdr_E * sizeof(int32_t)
        + (size_t)hdr_N * sizeof(int32_t);

    char *buf = (char *)std::malloc(file_size);
    if (!buf) {
        std::printf("ERROR: malloc failed for file buffer (%lu bytes)\n",
                    (unsigned long)file_size);
        return false;
    }

    struct ph_bulk_load_desc desc;
    desc.filename_addr = (long)fname;
    desc.dest_addr     = (long)buf;
    desc.size          = (long)file_size;
    desc.result        = 0;
    ph_bulk_load_file(&desc);

    if (desc.result <= 0) {
        std::printf("ERROR: bulk load of %s failed (result=%ld)\n", src, desc.result);
        std::free(buf);
        return false;
    }

    g_file_buffer  = buf;
    g_num_vertices = hdr_N;
    g_num_edges    = hdr_E;
    g_bfs_source   = hdr_source;

    // Point CSR arrays into the loaded buffer (no additional allocation)
    g_row_ptr = (int32_t *)(buf + 20);
    g_col_idx = (int32_t *)(buf + 20 + (size_t)(hdr_N + 1) * sizeof(int32_t));

    // Use the highest-degree vertex as the BFS source so RMAT graphs produce
    // a large first frontier and make the skewed initial distribution visible.
    int32_t max_deg = -1;
    int32_t max_deg_vertex = 0;
    for (int32_t v = 0; v < hdr_N; v++) {
        const int32_t deg = g_row_ptr[v + 1] - g_row_ptr[v];
        if (deg > max_deg) {
            max_deg = deg;
            max_deg_vertex = v;
        }
    }
    g_bfs_source = max_deg_vertex;

    const size_t visited_bytes = (size_t)hdr_N * sizeof(int64_t);
    const size_t dist_bytes = (size_t)hdr_N * sizeof(int32_t);
    const size_t frontier_bytes = (size_t)hdr_N * sizeof(int64_t);
    const size_t frontier_owner_bytes = (size_t)hdr_N * sizeof(int32_t);
    uintptr_t l2sp_heap = align_up_uintptr((uintptr_t)l2sp_end, 8);
    const uintptr_t l2sp_base = 0x20000000;
    const uintptr_t l2sp_limit = l2sp_base + podL2SPSize();
    const size_t frontier_total_bytes = 2 * frontier_bytes + frontier_owner_bytes;

    g_dist_in_l2sp = 0;
    g_visited_in_l2sp = 0;
    g_frontiers_in_l2sp = 0;

    visited = (volatile int64_t *)try_alloc_l2sp(&l2sp_heap, visited_bytes, alignof(int64_t), l2sp_limit);
    if (visited != nullptr) {
        g_visited_in_l2sp = 1;
    } else {
        visited = (volatile int64_t *)std::malloc(visited_bytes);
    }

    dist_arr = (int32_t *)try_alloc_l2sp(&l2sp_heap, dist_bytes, alignof(int32_t), l2sp_limit);
    if (dist_arr != nullptr) {
        g_dist_in_l2sp = 1;
    } else {
        dist_arr = (int32_t *)std::malloc(dist_bytes);
    }

    if (!visited || !dist_arr) {
        std::printf("ERROR: allocation failed for visited/dist_arr\n");
        std::free(buf);
        if (!g_visited_in_l2sp) std::free((void *)visited);
        if (!g_dist_in_l2sp) std::free(dist_arr);
        g_file_buffer = nullptr;
        visited  = nullptr;
        dist_arr = nullptr;
        g_dist_in_l2sp = 0;
        g_visited_in_l2sp = 0;
        return false;
    }

    const uintptr_t frontier_heap = align_up_uintptr(l2sp_heap, alignof(int64_t));
    const uintptr_t frontier_l2sp_bytes =
        (frontier_heap < l2sp_limit) ? (l2sp_limit - frontier_heap) : 0;
    const int64_t frontier_l2sp_vertices =
        (int64_t)((frontier_l2sp_bytes / (2 * sizeof(int64_t) + sizeof(int32_t))) > (uintptr_t)hdr_N
            ? hdr_N
            : (frontier_l2sp_bytes / (2 * sizeof(int64_t) + sizeof(int32_t))));
    const int64_t frontier_dram_vertices = hdr_N - frontier_l2sp_vertices;

    g_next_frontier.l2sp_capacity = frontier_l2sp_vertices;
    g_current_frontier_storage = nullptr;
    g_current_frontier_dram_storage = nullptr;
    g_next_frontier.l2sp_items = nullptr;
    g_next_frontier.dram_items = nullptr;
    g_next_frontier.l2sp_owner_core = nullptr;
    g_next_frontier.dram_owner_core = nullptr;

    if (frontier_l2sp_vertices > 0) {
        const size_t l2sp_frontier_bytes = (size_t)frontier_l2sp_vertices * sizeof(int64_t);
        const size_t l2sp_owner_bytes = (size_t)frontier_l2sp_vertices * sizeof(int32_t);
        g_current_frontier_storage =
            (int64_t *)try_alloc_l2sp(&l2sp_heap, l2sp_frontier_bytes, alignof(int64_t), l2sp_limit);
        g_next_frontier.l2sp_items =
            (int64_t *)try_alloc_l2sp(&l2sp_heap, l2sp_frontier_bytes, alignof(int64_t), l2sp_limit);
        g_next_frontier.l2sp_owner_core =
            (int32_t *)try_alloc_l2sp(&l2sp_heap, l2sp_owner_bytes, alignof(int32_t), l2sp_limit);
        g_frontiers_in_l2sp = 1;
    }
    if (frontier_dram_vertices > 0) {
        const size_t dram_frontier_bytes = (size_t)frontier_dram_vertices * sizeof(int64_t);
        const size_t dram_owner_bytes = (size_t)frontier_dram_vertices * sizeof(int32_t);
        g_current_frontier_dram_storage = (int64_t *)std::malloc(dram_frontier_bytes);
        g_next_frontier.dram_items = (int64_t *)std::malloc(dram_frontier_bytes);
        g_next_frontier.dram_owner_core = (int32_t *)std::malloc(dram_owner_bytes);
    }
    g_next_frontier.tail = 0;
    g_next_frontier.capacity = hdr_N;

    const bool missing_l2sp_segment =
        frontier_l2sp_vertices > 0 &&
        (!g_current_frontier_storage || !g_next_frontier.l2sp_items || !g_next_frontier.l2sp_owner_core);
    const bool missing_dram_segment =
        frontier_dram_vertices > 0 &&
        (!g_current_frontier_dram_storage || !g_next_frontier.dram_items || !g_next_frontier.dram_owner_core);

    if (missing_l2sp_segment || missing_dram_segment) {
        std::printf("ERROR: allocation failed for dynamic frontier buffers "
                    "(need %lu bytes, L2SP available %lu bytes)\n",
                    (unsigned long)frontier_total_bytes,
                    (unsigned long)(l2sp_limit - frontier_heap));
        std::free(buf);
        if (!g_visited_in_l2sp) std::free((void *)visited);
        if (!g_dist_in_l2sp) std::free(dist_arr);
        std::free(g_current_frontier_dram_storage);
        std::free(g_next_frontier.dram_items);
        std::free(g_next_frontier.dram_owner_core);
        g_file_buffer = nullptr;
        g_current_frontier_storage = nullptr;
        g_current_frontier_dram_storage = nullptr;
        g_next_frontier.l2sp_items = nullptr;
        g_next_frontier.dram_items = nullptr;
        g_next_frontier.l2sp_owner_core = nullptr;
        g_next_frontier.dram_owner_core = nullptr;
        g_next_frontier.l2sp_capacity = 0;
        visited = nullptr;
        dist_arr = nullptr;
        g_dist_in_l2sp = 0;
        g_visited_in_l2sp = 0;
        g_frontiers_in_l2sp = 0;
        return false;
    }

    std::printf("Graph loaded: N=%d E=%d D=%d source=%d max_deg=%d (%lu bytes)\n",
                hdr_N, hdr_E, hdr_D, g_bfs_source, max_deg, (unsigned long)file_size);
    return true;
}

// Process a single BFS node from CSR, claim unvisited, push to next level
static inline void process_single_node(int64_t u, int32_t level) {
    const int32_t row_start = g_row_ptr[u];
    const int32_t row_end = g_row_ptr[u + 1];
    atomic_fetch_add_i64((volatile int64_t *)&stat_edges_traversed, (int64_t)(row_end - row_start));
    for (int32_t ei = row_start; ei < row_end; ei++) {
        const int64_t v = g_col_idx[ei];
        if (claim_node(v)) {
            dist_arr[v] = level + 1;
            if (!next_frontier_push_atomic(v)) {
                std::printf("ERROR: next frontier overflow at level %d (capacity=%ld)\n",
                            level, (long)g_next_frontier.capacity);
                std::abort();
            }
            atomic_fetch_add_i64(&discovered, 1);
        }
    }
}

// -------------------- Victim Selection --------------------

// Fast xorshift PRNG for victim selection — mixes tid + counter to decorrelate thieves
static inline uint32_t xorshift_victim(uint32_t seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static constexpr int RECENTLY_TRIED_SIZE = 4; // small "recently failed" ring buffer

// -------------------- BFS Level Processing --------------------
static void process_bfs_level(int tid, int32_t level) {
    const int harts_per_core = g_harts_per_core;
    const int total_cores = g_total_cores;
    const int my_core = tid / harts_per_core;
    const int my_local_id = tid % harts_per_core;  // hart index within core

    WorkQueue* my_queue = &core_queues[my_core];
    // Steal policy knobs
    const int STEAL_START = 1;             // wait for N local misses before starting steal episodes
    const int64_t PROACTIVE_LOW_WATERMARK = BFS_CHUNK_SIZE / 2;
    const int64_t PROACTIVE_VICTIM_MIN_DEPTH = 2 * BFS_CHUNK_SIZE;
    int64_t steal_backoff = 4;
    const int64_t steal_backoff_max = 512;

    // Xorshift RNG state — seeded per-hart so thieves diverge
    uint32_t rng_state = (uint32_t)(tid + 1) * 2654435761u;

    // Small ring buffer of recently-failed victims to avoid immediate retries
    int recently_tried[RECENTLY_TRIED_SIZE];
    for (int i = 0; i < RECENTLY_TRIED_SIZE; i++) recently_tried[i] = -1;
    int rt_idx = 0;

    int64_t local_processed = 0;
    int64_t local_steal_attempts = 0;
    int64_t local_steal_success = 0;
    int64_t local_steal_items = 0;
    int64_t local_l1_items = 0;
    int64_t local_spill_items = 0;
    int64_t local_refill_items = 0;
    int64_t local_stolen_items = 0;
    int64_t local_idle_wait_cycles = 0;
    int32_t local_snap_counter = 0;  // snapshot throttle counter

    int empty_streak = 0;

    // Local buffer for stolen items — avoid re-touching victim's cache lines
    int64_t stolen_buf[STEAL_K_MAX];
    int64_t local_buf[BFS_CHUNK_SIZE];
    L1WorkCacheHeader* l1hdr = l1_work_cache_header_ptr();

    int64_t local_backoff = 4;
    const int64_t local_backoff_max = 128;

    while (g_level_remaining.load(std::memory_order_acquire) > 0) {
        g_hart_stack_live_bytes[tid] = current_stack_usage_bytes();
        monitor_l1_work_cache(my_core);
        if (my_local_id == 0) {
            const int64_t occ = atomic_load_i64(&l1hdr->count);
            stat_l1_cache_occupancy_samples[my_core]++;
            stat_l1_cache_occupancy_sum[my_core] += occ;
            if (occ > stat_l1_cache_occupancy_max[my_core]) {
                stat_l1_cache_occupancy_max[my_core] = occ;
            }
        }

        const int32_t steal_batch = core_steal_batch_size[my_core];
#ifndef NO_STEAL_BASELINE
        const int32_t steal_victims = core_steal_victim_probes[my_core];
        bool should_try_proactive_steal = false;

        {
            const int64_t local_depth =
                atomic_load_i64(&l1hdr->count) +
                atomic_load_i64(&g_core_spill_count[my_core]) +
                queue_depth(my_queue);
            if (local_depth > 0 && local_depth <= PROACTIVE_LOW_WATERMARK &&
                g_level_remaining.load(std::memory_order_acquire) > local_depth) {
                should_try_proactive_steal = true;
            }
        }

        if (should_try_proactive_steal && thief_token_try_acquire()) {
            bool stole_proactively = false;
            for (int k = 0; k < steal_victims; k++) {
                rng_state = xorshift_victim(rng_state);
                const int victim = (int)(rng_state % (uint32_t)total_cores);
                if (victim == my_core || core_has_work[victim] == 0) continue;

                const int64_t victim_depth_before = queue_depth(&core_queues[victim]);
                if (victim_depth_before < PROACTIVE_VICTIM_MIN_DEPTH) continue;

                local_steal_attempts++;
                int64_t begin_idx = 0;
                int64_t count = queue_pop_chunk(&core_queues[victim], victim, &begin_idx, steal_batch);
                if (count <= 0) continue;

                local_steal_success++;
                stole_proactively = true;
                for (int64_t i = 0; i < count; i++) {
                    stolen_buf[i] =
                        frontier_current_get(core_queues[victim].start_idx + begin_idx + i);
                }
                for (int64_t i = 0; i < count; i++) {
                    local_processed++;
                    local_steal_items++;
                    local_stolen_items++;
                    process_single_node(stolen_buf[i], level);
                    g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
                }
                record_wq_snap(level, WQ_SNAP_STEAL, my_core, tid);
                adapt_steal_policy(my_core, victim_depth_before, count);
                empty_streak = 0;
                steal_backoff = 4;
                local_backoff = 4;
                for (int ri = 0; ri < RECENTLY_TRIED_SIZE; ri++) recently_tried[ri] = -1;
                break;
            }
            thief_token_release();
            if (stole_proactively) {
                continue;
            }
        }
#endif

        // Prefer core-local L1 cache, then spill, then refill from the L2 queue.
        int64_t count = l1_cache_pop_chunk(local_buf, BFS_CHUNK_SIZE);
        int work_source = 1; // 1=L1, 2=spill, 3=refill
        if (count == 0) {
            count = spill_pop_chunk(my_core, local_buf, BFS_CHUNK_SIZE);
            if (count > 0) work_source = 2;
        }
        if (count == 0) {
            int64_t fetch_limit = steal_batch;
            if (fetch_limit < BFS_CHUNK_SIZE) fetch_limit = BFS_CHUNK_SIZE;
            refill_l1_work_cache_from_queue(my_queue, my_core, fetch_limit);
            count = l1_cache_pop_chunk(local_buf, BFS_CHUNK_SIZE);
            if (count > 0) work_source = 3;
        }

        if (count > 0) {
            for (int64_t i = 0; i < count; i++) {
                int64_t u = local_buf[i];
                local_processed++;
                if (work_source == 1) local_l1_items++;
                else if (work_source == 2) local_spill_items++;
                else if (work_source == 3) local_refill_items++;
                process_single_node(u, level);
                g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
            }

            // Periodic snapshot of all queue depths
            if (++local_snap_counter % SNAP_EVERY == 0) {
                record_wq_snap(level, WQ_SNAP_POP, my_core, tid);
            }

            empty_streak = 0;
            steal_backoff = 4;
            local_backoff = 4;
        } else {
            // Re-check local sources: another hart may have just refilled or spilled work.
            {
                const int64_t local_depth =
                    atomic_load_i64(&l1hdr->count) +
                    atomic_load_i64(&g_core_spill_count[my_core]) +
                    queue_depth(my_queue);
                if (local_depth > 0) {
                    continue;
                }
            }

            // Queue is truly empty.
            core_has_work[my_core] = 0;

            // Count empty rounds without holding the thief token — every
            // hart can independently track its streak.
            empty_streak++;

            // Short local backoff first, before trying to steal
            if (empty_streak < STEAL_START) {
                uint64_t wait_start = 0, wait_end = 0;
                asm volatile("rdcycle %0" : "=r"(wait_start));
                wait_check_local(local_backoff, my_core);
                asm volatile("rdcycle %0" : "=r"(wait_end));
                local_idle_wait_cycles += (int64_t)(wait_end - wait_start);
                if (local_backoff < local_backoff_max) local_backoff <<= 1;
                continue;
            }

#ifdef NO_STEAL_BASELINE
            uint64_t wait_start = 0, wait_end = 0;
            asm volatile("rdcycle %0" : "=r"(wait_start));
            wait_check_local(local_backoff, my_core);
            asm volatile("rdcycle %0" : "=r"(wait_end));
            local_idle_wait_cycles += (int64_t)(wait_end - wait_start);
            if (local_backoff < local_backoff_max) local_backoff <<= 1;
            continue;
#else
            // Ready to steal — acquire the per-core thief token so only
            // one hart per core issues remote CAS probes at a time.
            if (!thief_token_try_acquire()) {
                // Another hart on this core is already stealing — back off, retry local
                uint64_t wait_start = 0, wait_end = 0;
                asm volatile("rdcycle %0" : "=r"(wait_start));
                wait_check_local(local_backoff * 4, my_core);
                asm volatile("rdcycle %0" : "=r"(wait_end));
                local_idle_wait_cycles += (int64_t)(wait_end - wait_start);
                if (local_backoff < local_backoff_max) local_backoff <<= 1;
                continue;
            }

            bool found = false;
            for (int k = 0; k < steal_victims; k++) {
                int victim;
                int pick_tries = 0;
                do {
                    rng_state = xorshift_victim(rng_state);
                    victim = (int)(rng_state % (uint32_t)total_cores);
                    bool in_recent = false;
                    if (victim != my_core && core_has_work[victim] != 0) {
                        for (int ri = 0; ri < RECENTLY_TRIED_SIZE; ri++) {
                            if (recently_tried[ri] == victim) { in_recent = true; break; }
                        }
                        if (!in_recent) break;
                    }
                    pick_tries++;
                } while (pick_tries < total_cores);
                if (victim == my_core || core_has_work[victim] == 0) continue;

                local_steal_attempts++;
                const int64_t victim_depth_before = queue_depth(&core_queues[victim]);
                int64_t begin_idx = 0;
                count = queue_pop_chunk(&core_queues[victim], victim, &begin_idx, steal_batch);
                if (count > 0) {
                    local_steal_success++;
                    found = true;
                    for (int64_t i = 0; i < count; i++) {
                        stolen_buf[i] =
                            frontier_current_get(core_queues[victim].start_idx + begin_idx + i);
                    }
                    for (int64_t i = 0; i < count; i++) {
                        local_processed++;
                        local_steal_items++;
                        local_stolen_items++;
                        process_single_node(stolen_buf[i], level);
                        g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
                    }

                    record_wq_snap(level, WQ_SNAP_STEAL, my_core, tid);
                    adapt_steal_policy(my_core, victim_depth_before, count);

                    empty_streak = 0;
                    steal_backoff = 4;
                    local_backoff = 4;
                    for (int ri = 0; ri < RECENTLY_TRIED_SIZE; ri++) recently_tried[ri] = -1;
                    break;
                } else {
                    recently_tried[rt_idx] = victim;
                    rt_idx = (rt_idx + 1) % RECENTLY_TRIED_SIZE;
                }
            }

            thief_token_release();

            if (!found) {
                uint64_t wait_start = 0, wait_end = 0;
                asm volatile("rdcycle %0" : "=r"(wait_start));
                wait_check_local(steal_backoff, my_core);
                asm volatile("rdcycle %0" : "=r"(wait_end));
                local_idle_wait_cycles += (int64_t)(wait_end - wait_start);
                if (steal_backoff < steal_backoff_max) steal_backoff <<= 1;
                continue;
            }
#endif
        }
    }

    stat_nodes_processed[tid] += local_processed;
    stat_steal_attempts[tid] += local_steal_attempts;
    stat_steal_success[tid] += local_steal_success;
    stat_l1_cache_items_processed[tid] += local_l1_items;
    stat_spill_items_processed[tid] += local_spill_items;
    stat_queue_refill_items_processed[tid] += local_refill_items;
    stat_stolen_items_processed[tid] += local_stolen_items;
    stat_steal_items[tid] += local_steal_items;
    stat_idle_wait_cycles[tid] += local_idle_wait_cycles;
}

// -------------------- Level Advance (static balanced repartition) --------------------
static void advance_to_next_level_balanced(int tid) {
    if (tid != 0) return;

    int total_cores = g_total_cores;
    int64_t total_nodes = g_next_frontier.tail;

    if (total_nodes == 0) {
        for (int c = 0; c < total_cores; c++) {
            queue_init(&core_queues[c]);
            core_has_work[c] = 0;
        }
        g_next_frontier.tail = 0;
        return;
    }

    int64_t quotas[MAX_CORES];
    const int64_t base_quota = total_nodes / total_cores;
    const int64_t remainder = total_nodes % total_cores;
    for (int c = 0; c < total_cores; c++) {
        quotas[c] = base_quota + ((c < remainder) ? 1 : 0);
    }

    int64_t base = 0;
    for (int c = 0; c < total_cores; c++) {
        queue_assign_slice(&core_queues[c], base, quotas[c]);
        core_has_work[c] = (quotas[c] > 0) ? 1 : 0;
        base += quotas[c];
    }

    for (int64_t i = 0; i < total_nodes; i++) {
        frontier_current_set(i, frontier_next_item_get(i));
    }

    g_next_frontier.tail = 0;
}

// -------------------- Work Count --------------------
static int64_t count_total_work() {
    int64_t total = 0;
    for (int c = 0; c < g_total_cores; c++) {
        int64_t count = core_queues[c].tail - core_queues[c].head;
        total += count;
    }
    return total;
}

// -------------------- BFS Driver --------------------
static void bfs() {
    const int tid = get_thread_id();

    if (tid == 0) {
        g_wq_trace_count = 0;
        g_wq_trace_dropped = 0;
        g_wq_snap_count = 0;

        // Load graph from file; abort on failure
        if (!load_graph_from_file()) {
            std::abort();
        }

        const int32_t num_verts = g_num_vertices;
        const int64_t source_id = g_bfs_source;

        for (int64_t i = 0; i < num_verts; i++) {
            visited[i]  = 0;
            dist_arr[i] = -1;
        }
        stat_edges_traversed = 0;

        for (int i = 0; i < g_total_harts; i++) {
            g_local_sense[i] = 0;
            stat_nodes_processed[i] = 0;
            stat_steal_attempts[i] = 0;
            stat_steal_success[i] = 0;
            stat_l1_cache_items_processed[i] = 0;
            stat_spill_items_processed[i] = 0;
            stat_queue_refill_items_processed[i] = 0;
            stat_stolen_items_processed[i] = 0;
            stat_steal_items[i] = 0;
            stat_idle_wait_cycles[i] = 0;
            g_hart_stack_live_bytes[i] = 0;
        }

        for (int c = 0; c < g_total_cores; c++) {
            queue_init(&core_queues[c]);
            core_has_work[c] = 0;
            core_steal_batch_size[c] = STEAL_K_MIN;
            core_steal_victim_probes[c] = STEAL_VICTIMS_MIN;
            core_last_steal_feedback[c] = 0;
            g_core_spill_count[c] = 0;
            stat_l1_refill_success[c] = 0;
            stat_l1_refill_items[c] = 0;
            stat_l1_refill_cycles[c] = 0;
            stat_l1_spill_items[c] = 0;
            stat_l1_spill_cycles[c] = 0;
            stat_l1_cache_occupancy_samples[c] = 0;
            stat_l1_cache_occupancy_sum[c] = 0;
            stat_l1_cache_occupancy_max[c] = 0;
        }
        g_next_frontier.tail = 0;

        visited[source_id] = 1;
        dist_arr[source_id] = 0;
        discovered = 1;

        queue_assign_slice(&core_queues[0], 0, 1);
        frontier_current_set(0, source_id);
        core_has_work[0] = 1;
        for (int c = 1; c < g_total_cores; c++) {
            queue_assign_slice(&core_queues[c], 1, 0);
        }

        std::printf("=== %s ===\n", BENCH_TITLE);
        std::printf("Graph: N=%d E=%d (RMAT CSR from rmat.bin)\n",
                    g_num_vertices, g_num_edges);
        std::printf("Graph storage: row_ptr/col_idx in DRAM (malloc)\n");
        std::printf("Hot state: visited in %s, dist_arr in %s\n",
                    g_visited_in_l2sp ? "L2SP" : "DRAM",
                    g_dist_in_l2sp ? "L2SP" : "DRAM");
        const char* frontier_storage =
            (g_next_frontier.l2sp_capacity == 0) ? "DRAM" :
            (g_next_frontier.l2sp_capacity == g_next_frontier.capacity) ? "L2SP" :
            "L2SP+DRAM";
        std::printf("Frontiers: dynamic %s buffers with static balanced repartition "
                    "(capacity=%d vertices, l2sp_vertices=%ld)\n",
                    frontier_storage,
                    g_num_vertices,
                    (long)g_next_frontier.l2sp_capacity);
        std::printf("Hardware: %d cores x %d harts = %d total harts\n",
                    g_total_cores, g_harts_per_core, g_total_harts);
        std::printf("Source: node %ld (highest-degree vertex)\n", (long)source_id);
        g_core_l1sp_bytes = coreL1SPSize();
        g_hart_stack_capacity_bytes = g_core_l1sp_bytes / (uint64_t)g_harts_per_core;
        std::printf("L1SP: per-core=%lu bytes, global=%lu bytes\n",
                    (unsigned long)g_core_l1sp_bytes,
                    (unsigned long)(g_core_l1sp_bytes * (uint64_t)g_total_cores));
        std::printf("L1SP stack slot: per-hart=%lu bytes\n",
                    (unsigned long)g_hart_stack_capacity_bytes);
#ifdef NO_STEAL_BASELINE
        std::printf("Stealing: disabled; only balanced per-level redistribution + L1 work cache\n");
#else
        std::printf("Stealing: proactive low-watermark + adaptive batch/probe sizing "
                    "(batch=%d..%d, probes=%d..%d)\n",
                    STEAL_K_MIN, STEAL_K_MAX, STEAL_VICTIMS_MIN, STEAL_VICTIMS_MAX);
#endif
        std::printf("L1 work cache: per-core shared cache in lower L1SP with %d-byte guard\n",
                    L1_CACHE_GUARD_BYTES);
        std::printf("\n");
        record_wq_trace(WQ_PHASE_INIT, 0);

        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }

    // Each core's first hart initializes its L1SP thief token.
    // All harts on the core share the same L1SP, so only one write needed.
    if (myThreadId() == 0) {
        thief_token_init();
        l1_work_cache_init();
    }

    barrier();
    record_l1sp_trace_sample(tid);
    barrier();

    uint64_t start_cycles = 0;
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(start_cycles));
    }

    int32_t level = 0;

    while (true) {
        // Compute work for this level after previous redistribution
        int64_t total_work = count_total_work();

        // Publish remaining-work counter (one writer is enough; we gate with barrier)
        if (tid == 0) {
            g_level_remaining.store(total_work, std::memory_order_release);
        }

        barrier();

        if (total_work == 0) break;

        if (tid == 0) {
            std::printf("Level %d: total_work=%ld, discovered=%ld\n",
                        level, (long)total_work, (long)discovered);
            std::printf("  Distribution: ");
            for (int c = 0; c < g_total_cores; c++) {
                int64_t count = core_queues[c].tail - core_queues[c].head;
                std::printf("C%d:%ld ", c, (long)count);
            }
            std::printf("\n");
            record_wq_trace(WQ_PHASE_LEVEL_BEGIN, level);
        }

        barrier();
        record_l1sp_trace_sample(tid);
        barrier();
        ph_stat_phase(1);  // mark useful work: L2SP accesses counted as useful_*
        process_bfs_level(tid, level);
        ph_stat_phase(0);
        barrier();
        if (tid == 0) {
            record_wq_trace(WQ_PHASE_POST_PROCESS, level);
        }
        barrier();
        record_l1sp_trace_sample(tid);
        barrier();
        advance_to_next_level_balanced(tid);
        barrier();
        if (tid == 0) {
            record_wq_trace(WQ_PHASE_POST_REDISTRIBUTE, level);
        }
        barrier();
        record_l1sp_trace_sample(tid);
        barrier();
        level++;
    }

    uint64_t end_cycles = 0;
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(end_cycles));
    }

    barrier();

    if (tid == 0) {
        record_wq_trace(WQ_PHASE_FINAL, level);
    }
    barrier();
    record_l1sp_trace_sample(tid);
    barrier();

    if (tid == 0) {
        std::printf("\n=== BFS Complete ===\n");
        std::printf("Levels traversed: %d\n", level);
        std::printf("Nodes discovered: %ld / %d\n", (long)discovered, g_num_vertices);

        std::printf("dist[source=%d] = %d (expected 0)\n",
                    g_bfs_source, dist_arr[g_bfs_source]);

        int64_t total_processed = 0;
        int64_t total_attempts = 0;
        int64_t total_success = 0;
        int64_t total_l1_items = 0;
        int64_t total_spill_items = 0;
        int64_t total_refill_items_processed = 0;
        int64_t total_stolen_items_processed = 0;
        int64_t total_steal_items = 0;
        int64_t total_idle_wait_cycles = 0;
        int64_t total_refill_success = 0;
        int64_t total_refill_items = 0;
        int64_t total_refill_cycles = 0;
        int64_t total_spill_items_evicted = 0;
        int64_t total_spill_cycles = 0;

        std::printf("\nPer-hart statistics:\n");
        std::printf("Hart | Processed | L1 | Spill | Refill | Stolen | Steal Attempts | Steals OK | Idle Cycles\n");
        std::printf("-----|-----------|----|-------|--------|--------|----------------|-----------|------------\n");

        for (int h = 0; h < g_total_harts; h++) {
            int64_t processed = stat_nodes_processed[h];
            int64_t attempts  = stat_steal_attempts[h];
            int64_t success   = stat_steal_success[h];
            int64_t l1_items = stat_l1_cache_items_processed[h];
            int64_t spill_items = stat_spill_items_processed[h];
            int64_t refill_items_processed = stat_queue_refill_items_processed[h];
            int64_t stolen_items_processed = stat_stolen_items_processed[h];
            int64_t idle_wait_cycles = stat_idle_wait_cycles[h];

            std::printf("%4d | %9ld | %2ld | %5ld | %6ld | %6ld | %14ld | %9ld | %10ld\n",
                        h, (long)processed, (long)l1_items, (long)spill_items,
                        (long)refill_items_processed, (long)stolen_items_processed,
                        (long)attempts, (long)success, (long)idle_wait_cycles);

            total_processed += processed;
            total_attempts  += attempts;
            total_success   += success;
            total_l1_items += l1_items;
            total_spill_items += spill_items;
            total_refill_items_processed += refill_items_processed;
            total_stolen_items_processed += stolen_items_processed;
            total_steal_items += stat_steal_items[h];
            total_idle_wait_cycles += idle_wait_cycles;
        }

        std::printf("\nPer-core L1 cache statistics:\n");
        std::printf("Core | Refill OK | Refill Items | Refill Cycles | Spill Items | Spill Cycles | Avg Occ | Max Occ | Batch | Probes\n");
        std::printf("-----|-----------|--------------|---------------|-------------|--------------|---------|---------|-------|-------\n");
        for (int c = 0; c < g_total_cores; c++) {
            const int64_t occ_samples = stat_l1_cache_occupancy_samples[c];
            const int64_t avg_occ = (occ_samples > 0)
                ? (stat_l1_cache_occupancy_sum[c] / occ_samples)
                : 0;
            const int64_t refill_ok = stat_l1_refill_success[c];
            const int64_t refill_items = stat_l1_refill_items[c];
            const int64_t refill_cycles = stat_l1_refill_cycles[c];
            const int64_t spill_items = stat_l1_spill_items[c];
            const int64_t spill_cycles = stat_l1_spill_cycles[c];
            const int64_t max_occ = stat_l1_cache_occupancy_max[c];

            total_refill_success += refill_ok;
            total_refill_items += refill_items;
            total_refill_cycles += refill_cycles;
            total_spill_items_evicted += spill_items;
            total_spill_cycles += spill_cycles;

            std::printf("%4d | %9ld | %12ld | %13ld | %11ld | %12ld | %7ld | %7ld | %5d | %6d\n",
                        c, (long)refill_ok, (long)refill_items, (long)refill_cycles,
                        (long)spill_items, (long)spill_cycles, (long)avg_occ, (long)max_occ,
                        (int)core_steal_batch_size[c], (int)core_steal_victim_probes[c]);
        }

        std::printf("\nSummary:\n");
        std::printf("  Total nodes processed: %ld\n", (long)total_processed);
        std::printf("  Total steal attempts:  %ld\n", (long)total_attempts);
        if (total_attempts > 0) {
            std::printf("Successful steals:     %ld (%ld%%)\n",
                        (long)total_success,
                        (long)(100 * total_success / total_attempts));
        }
        std::printf("  Total steal items:     %ld\n", (long)total_steal_items);
        if (total_success > 0) {
            std::printf("  Avg items/steal:       %ld\n",
                        (long)(total_steal_items / total_success));
        }
        std::printf("  Total edges traversed: %ld\n", (long)stat_edges_traversed);
        std::printf("  L1 items processed:    %ld\n", (long)total_l1_items);
        std::printf("  Spill items processed: %ld\n", (long)total_spill_items);
        std::printf("  Refill items processed:%ld\n", (long)total_refill_items_processed);
        std::printf("  Stolen items processed:%ld\n", (long)total_stolen_items_processed);
        std::printf("  L1 refill success:     %ld\n", (long)total_refill_success);
        std::printf("  L1 refill items:       %ld\n", (long)total_refill_items);
        std::printf("  L1 refill cycles:      %ld\n", (long)total_refill_cycles);
        std::printf("  L1 spill items:        %ld\n", (long)total_spill_items_evicted);
        std::printf("  L1 spill cycles:       %ld\n", (long)total_spill_cycles);
        std::printf("  Idle wait cycles:      %ld\n", (long)total_idle_wait_cycles);

        uint64_t elapsed = end_cycles - start_cycles;
        std::printf("Cycles elapsed:        %lu\n", (unsigned long)elapsed);
        if (total_processed > 0) {
            std::printf("Cycles per node:       %lu\n",
                        (unsigned long)(elapsed / total_processed));
        }
        if (stat_edges_traversed > 0) {
            std::printf("Cycles per edge:       %lu\n",
                        (unsigned long)(elapsed / (uint64_t)stat_edges_traversed));
        }
        std::printf("WORKSRC: from_l1=%ld from_spill=%ld from_refill=%ld from_steal=%ld\n",
                    (long)total_l1_items, (long)total_spill_items,
                    (long)total_refill_items_processed, (long)total_stolen_items_processed);
        std::printf("L1CACHE: refill_ok=%ld refill_items=%ld spill_items=%ld refill_cycles=%ld spill_cycles=%ld\n",
                    (long)total_refill_success, (long)total_refill_items,
                    (long)total_spill_items_evicted, (long)total_refill_cycles,
                    (long)total_spill_cycles);
        dump_wq_trace();

        // Release dynamically-allocated graph memory
        std::free(g_file_buffer);   // owns g_row_ptr and g_col_idx
        if (!g_visited_in_l2sp) std::free((void *)visited);
        if (!g_dist_in_l2sp) std::free(dist_arr);
        std::free(g_current_frontier_dram_storage);
        std::free(g_next_frontier.dram_items);
        std::free(g_next_frontier.dram_owner_core);
        g_file_buffer = nullptr;
        g_row_ptr     = nullptr;
        g_col_idx     = nullptr;
        g_current_frontier_storage = nullptr;
        g_current_frontier_dram_storage = nullptr;
        g_next_frontier.l2sp_items = nullptr;
        g_next_frontier.dram_items = nullptr;
        g_next_frontier.l2sp_owner_core = nullptr;
        g_next_frontier.dram_owner_core = nullptr;
        g_next_frontier.tail = 0;
        g_next_frontier.capacity = 0;
        g_next_frontier.l2sp_capacity = 0;
        visited       = nullptr;
        dist_arr      = nullptr;
        g_dist_in_l2sp = 0;
        g_visited_in_l2sp = 0;
        g_frontiers_in_l2sp = 0;
    }
}

int main(int argc, char** argv) {
    const int tid = get_thread_id();

    if (tid == 0) {
        g_total_cores = numPodCores();
        g_harts_per_core = myCoreThreads();
        g_total_harts = g_total_cores * g_harts_per_core;
    }

    barrier();
    bfs();
    barrier();

    return 0;
}
