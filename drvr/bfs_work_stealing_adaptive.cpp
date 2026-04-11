// Level-synchronous BFS with per-core work stealing (FIFO pop + throttled stealing)
// FIX: level termination uses a global remaining-work counter, not per-hart empty rounds.
// - Each level: frontier nodes distributed to per-core queues (imbalanced: odd cores get 2x)
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

static constexpr int MAX_HARTS  = 1024;
static constexpr int MAX_CORES  = 64;
static constexpr int MAX_HARTS_PER_CORE = 16;
static constexpr int MAX_DYNAMIC_WQ_EVENTS = 131072;
static constexpr int BFS_CHUNK_SIZE = 64;   // nodes per batch local pop
static constexpr int STEAL_K_MIN = 64;      // minimum nodes per steal
static constexpr int STEAL_K_MAX = 256;     // maximum nodes per steal
static constexpr int STEAL_VICTIMS_MIN = 2; // minimum victims probed per steal episode
static constexpr int STEAL_VICTIMS_MAX = 8; // maximum victims probed per steal episode

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

// Per-core steal token lives in each core's L1SP.
// All 16 harts on a core share the same L1SP and compete for the
// token via CAS.
static constexpr uintptr_t THIEF_TOKEN_L1SP_OFFSET = 0; // 8 bytes at L1SP base

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

// Iniialize the thief token (called once per core at startup).
static inline void thief_token_init() {
    volatile int64_t *tok = thief_token_ptr();
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

// -------------------- BFS Shared State --------------------
// Pointers to dynamically-loaded CSR graph (into a malloc'd DRAM buffer)
__l2sp__ int32_t *g_row_ptr    = nullptr;
__l2sp__ int32_t *g_col_idx    = nullptr;
__l2sp__ volatile int64_t *visited = nullptr;
__l2sp__ volatile int64_t *visited_dram = nullptr;
__l2sp__ int32_t *dist_arr     = nullptr;
__l2sp__ int32_t *dist_arr_dram = nullptr;
__l2sp__ char    *g_file_buffer = nullptr;
__l2sp__ int32_t  g_num_vertices = 0;
__l2sp__ int32_t  g_num_edges    = 0;
__l2sp__ int32_t  g_bfs_source   = 0;
__l2sp__ int32_t  g_dist_in_l2sp = 0;
__l2sp__ int32_t  g_visited_in_l2sp = 0;
__l2sp__ int32_t  g_dist_l2sp_count = 0;
__l2sp__ int32_t  g_visited_l2sp_count = 0;
__l2sp__ int32_t  g_frontiers_in_l2sp = 0;

__l2sp__ volatile int64_t stat_nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_attempts[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_success[MAX_HARTS];
__l2sp__ volatile int64_t discovered = 0;
__l2sp__ volatile uint64_t g_core_l1sp_bytes = 0;
__l2sp__ volatile uint64_t g_hart_stack_capacity_bytes = 0;
__l2sp__ volatile uint64_t g_hart_stack_peak_bytes[MAX_HARTS];
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

enum DynamicWQOp : int32_t {
    DYN_WQ_INIT = 0,
    DYN_WQ_POP = 1,
    DYN_WQ_STEAL = 2,
    DYN_WQ_REDISTRIBUTE = 3,
};

struct DynamicWQEvent {
    uint64_t cycle;
    int32_t actor_core;
    int32_t actor_thread;
    int32_t target_core;
    int32_t depth;
    int32_t op;
    int32_t peer_core;
};

__dram__ WQTraceSample g_wq_trace_samples[MAX_WQ_TRACE_SAMPLES];
__dram__ int32_t g_wq_trace_depths[MAX_WQ_TRACE_SAMPLES][MAX_CORES];
__dram__ uint64_t g_l1sp_trace_total_bytes[MAX_WQ_TRACE_SAMPLES];
__dram__ uint64_t g_l1sp_trace_core0_hart_bytes[MAX_WQ_TRACE_SAMPLES][MAX_HARTS_PER_CORE];
__l2sp__ volatile int32_t g_wq_trace_count = 0;
__l2sp__ volatile int32_t g_wq_trace_dropped = 0;
__dram__ DynamicWQEvent g_dynamic_wq_events[MAX_DYNAMIC_WQ_EVENTS];
__dram__ std::atomic<int32_t> g_dynamic_wq_event_count = 0;
__dram__ std::atomic<int32_t> g_dynamic_wq_event_dropped = 0;

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

static inline volatile int64_t* visited_ptr(int64_t idx) {
    return (idx < g_visited_l2sp_count)
        ? (visited + idx)
        : (visited_dram + (idx - g_visited_l2sp_count));
}

static inline int32_t* dist_ptr(int64_t idx) {
    return (idx < g_dist_l2sp_count)
        ? (dist_arr + idx)
        : (dist_arr_dram + (idx - g_dist_l2sp_count));
}

static inline int64_t visited_get(int64_t idx) {
    return *visited_ptr(idx);
}

static inline void visited_set(int64_t idx, int64_t value) {
    *visited_ptr(idx) = value;
}

static inline int32_t dist_get(int64_t idx) {
    return *dist_ptr(idx);
}

static inline void dist_set(int64_t idx, int32_t value) {
    *dist_ptr(idx) = value;
}

static inline bool claim_node(int64_t v) {
    const int64_t old = atomic_swap_i64(visited_ptr(v), 1);
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

static inline const char* dynamic_wq_op_name(int32_t op) {
    switch (op) {
    case DYN_WQ_INIT: return "init";
    case DYN_WQ_POP: return "pop";
    case DYN_WQ_STEAL: return "steal";
    case DYN_WQ_REDISTRIBUTE: return "redistribute";
    default: return "unknown";
    }
}

static inline void record_dynamic_wq_event(int target_core, DynamicWQOp op, int actor_tid, int peer_core) {
    const int32_t idx = g_dynamic_wq_event_count.fetch_add(1, std::memory_order_acq_rel);
    if (idx >= MAX_DYNAMIC_WQ_EVENTS) {
        g_dynamic_wq_event_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    DynamicWQEvent& event = g_dynamic_wq_events[idx];
    event.cycle = (op == DYN_WQ_INIT) ? 0 : cycle();
    if (actor_tid >= 0) {
        event.actor_core = actor_tid / g_harts_per_core;
        event.actor_thread = actor_tid % g_harts_per_core;
    } else {
        event.actor_core = -1;
        event.actor_thread = -1;
    }
    event.target_core = target_core;
    event.depth = (int32_t)queue_depth(&core_queues[target_core]);
    event.op = op;
    event.peer_core = peer_core;
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
        std::fprintf(out, "WQTRACE_DUMP_BEGIN,bench=bfs_work_stealing_adaptive,cores=%d,samples=%d,dropped=%d\n",
                     g_total_cores, (int)g_wq_trace_count, (int)g_wq_trace_dropped);
        for (int32_t i = 0; i < g_wq_trace_count; i++) {
            const WQTraceSample& s = g_wq_trace_samples[i];
            std::fprintf(out, "WQTRACE,bench=bfs_work_stealing_adaptive,cores=%d,sample=%d,phase=%s,level=%d,iter=-1,queue=core,depths=",
                         g_total_cores, (int)i, wq_phase_name(s.phase), (int)s.level);
            for (int c = 0; c < g_total_cores; c++) {
                if (c > 0) std::fprintf(out, "|");
                std::fprintf(out, "%d", (int)g_wq_trace_depths[i][c]);
            }
            std::fprintf(out, "\n");
        }
        std::fprintf(out, "WQTRACE_DUMP_END,bench=bfs_work_stealing_adaptive\n");

        std::fprintf(out,
                     "L1SPTRACE_DUMP_BEGIN,bench=bfs_work_stealing_adaptive,cores=%d,harts=%d,samples=%d\n",
                     g_total_cores, g_total_harts, (int)g_wq_trace_count);
        std::fprintf(out,
                     "L1SPTRACE_CONFIG,bench=bfs_work_stealing_adaptive,core_bytes=%lu,global_bytes=%lu\n",
                     (unsigned long)core_l1sp_bytes, (unsigned long)global_l1sp_bytes);
        for (int32_t i = 0; i < g_wq_trace_count; i++) {
            const WQTraceSample& s = g_wq_trace_samples[i];
            std::fprintf(out,
                         "L1SPTRACE_GLOBAL,bench=bfs_work_stealing_adaptive,sample=%d,phase=%s,level=%d,iter=-1,bytes=%lu\n",
                         (int)i, wq_phase_name(s.phase), (int)s.level,
                         (unsigned long)g_l1sp_trace_total_bytes[i]);
            for (int t = 0; t < g_harts_per_core && t < MAX_HARTS_PER_CORE; t++) {
                std::fprintf(out,
                             "L1SPTRACE_CORE_HART,bench=bfs_work_stealing_adaptive,sample=%d,core=0,thread=%d,hart=%d,bytes=%lu\n",
                             (int)i, t, t,
                             (unsigned long)g_l1sp_trace_core0_hart_bytes[i][t]);
            }
        }
        for (int h = 0; h < g_total_harts; h++) {
            std::fprintf(out,
                         "L1SPTRACE_HART,bench=bfs_work_stealing_adaptive,hart=%d,core=%d,thread=%d,bytes=%lu\n",
                         h, h / g_harts_per_core, h % g_harts_per_core,
                         (unsigned long)g_hart_stack_peak_bytes[h]);
        }
        std::fprintf(out, "L1SPTRACE_DUMP_END,bench=bfs_work_stealing_adaptive\n");

        // ---- Fine-grained work-queue snapshots ----
        int32_t snap_total = g_wq_snap_count;
        if (snap_total > MAX_WQ_SNAPS) snap_total = MAX_WQ_SNAPS;
        std::fprintf(out,
                     "WQSNAP_DUMP_BEGIN,bench=bfs_work_stealing_adaptive,cores=%d,snaps=%d,capacity=%d\n",
                     g_total_cores, (int)snap_total, MAX_WQ_SNAPS);
        for (int32_t i = 0; i < snap_total; i++) {
            const WQSnapMeta& m = g_wq_snap_meta[i];
            std::fprintf(out,
                         "WQSNAP,bench=bfs_work_stealing_adaptive,cores=%d,idx=%d,level=%d,event=%s,actor_core=%d,depths=",
                         g_total_cores, (int)i, (int)m.level,
                         (m.event == WQ_SNAP_STEAL) ? "steal" : "pop",
                         (int)m.actor_core);
            for (int c = 0; c < g_total_cores; c++) {
                if (c > 0) std::fprintf(out, "|");
                std::fprintf(out, "%d", (int)g_wq_snap_depths[i][c]);
            }
            std::fprintf(out, "\n");
        }
        std::fprintf(out, "WQSNAP_DUMP_END,bench=bfs_work_stealing_adaptive\n");

        // ---- Fine-grained L1SP stack snapshots (one per WQSNAP) ----
        const int hpc = g_harts_per_core;
        const int tc = g_total_cores;
        const int th = g_total_harts;
        std::fprintf(out,
                     "L1SPSNAP_DUMP_BEGIN,bench=bfs_work_stealing_adaptive,cores=%d,harts_per_core=%d,harts=%d,snaps=%d\n",
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
                             "L1SPSNAP_CORE,bench=bfs_work_stealing_adaptive,idx=%d,level=%d,core=%d,bytes=%lu\n",
                             (int)i, (int)m.level, c, (unsigned long)core_sum);
            }
            // Per-hart detail for all cores
            for (int h = 0; h < th; h++) {
                uint32_t b = g_wq_snap_stack_bytes[i][h];
                if (b == 0) continue;  // skip zeros to reduce output
                std::fprintf(out,
                             "L1SPSNAP_HART,bench=bfs_work_stealing_adaptive,idx=%d,level=%d,core=%d,thread=%d,hart=%d,bytes=%u\n",
                             (int)i, (int)m.level, h / hpc, h % hpc, h, (unsigned)b);
            }
        }
        std::fprintf(out, "L1SPSNAP_DUMP_END,bench=bfs_work_stealing_adaptive\n");
    };

    emit(stdout);

    char path[64];
    std::snprintf(path, sizeof(path), "run_adaptive_%dcores.log", g_total_cores);
    FILE* fp = std::fopen(path, "w");
    if (fp != nullptr) {
        emit(fp);
        std::fclose(fp);
        std::printf("WQTRACE_FILE_WRITTEN,bench=bfs_work_stealing_adaptive,path=%s\n", path);
    } else {
        std::printf("WQTRACE_FILE_ERROR,bench=bfs_work_stealing_adaptive,path=%s\n", path);
    }

    char csv_path[96];
    std::snprintf(csv_path, sizeof(csv_path),
                  "run_bfs_work_stealing_adaptive_queue_depth_%dcores.csv", g_total_cores);
    FILE* csv = std::fopen(csv_path, "w");
    if (csv != nullptr) {
        const int32_t total_events = g_dynamic_wq_event_count.load(std::memory_order_acquire);
        const int32_t emitted = (total_events > MAX_DYNAMIC_WQ_EVENTS) ? MAX_DYNAMIC_WQ_EVENTS : total_events;
        const int32_t dropped = g_dynamic_wq_event_dropped.load(std::memory_order_acquire);
        std::fprintf(csv,
                     "# bench=bfs_work_stealing_adaptive,cores=%d,harts_per_core=%d,harts=%d,events=%d,dropped=%d\n",
                     g_total_cores, g_harts_per_core, g_total_harts, emitted, dropped);
        std::fprintf(csv, "event,cycle,actor_core,actor_thread,target_core,depth,op,peer_core\n");
        for (int32_t i = 0; i < emitted; i++) {
            const DynamicWQEvent& event = g_dynamic_wq_events[i];
            std::fprintf(csv, "%d,%llu,%d,%d,%d,%d,%s,%d\n",
                         (int)i,
                         (unsigned long long)event.cycle,
                         event.actor_core,
                         event.actor_thread,
                         event.target_core,
                         event.depth,
                         dynamic_wq_op_name(event.op),
                         event.peer_core);
        }
        std::fclose(csv);
        std::printf("WQDEPTH_TRACE_FILE_WRITTEN,bench=bfs_work_stealing_adaptive,path=%s\n", csv_path);
    } else {
        std::printf("WQDEPTH_TRACE_FILE_ERROR,bench=bfs_work_stealing_adaptive,path=%s\n", csv_path);
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
    g_dist_l2sp_count = 0;
    g_visited_l2sp_count = 0;
    g_frontiers_in_l2sp = 0;

    visited = nullptr;
    visited_dram = nullptr;
    dist_arr = nullptr;
    dist_arr_dram = nullptr;

    const uintptr_t visited_heap = align_up_uintptr(l2sp_heap, alignof(int64_t));
    const uintptr_t visited_l2sp_bytes =
        (visited_heap < l2sp_limit) ? (l2sp_limit - visited_heap) : 0;
    g_visited_l2sp_count = (int32_t)(
        ((visited_l2sp_bytes / sizeof(int64_t)) > (uintptr_t)hdr_N) ? hdr_N : (visited_l2sp_bytes / sizeof(int64_t)));
    if (g_visited_l2sp_count > 0) {
        visited = (volatile int64_t *)try_alloc_l2sp(
            &l2sp_heap, (size_t)g_visited_l2sp_count * sizeof(int64_t), alignof(int64_t), l2sp_limit);
        g_visited_in_l2sp = 1;
    }
    if (g_visited_l2sp_count < hdr_N) {
        visited_dram = (volatile int64_t *)std::malloc(((size_t)hdr_N - (size_t)g_visited_l2sp_count) * sizeof(int64_t));
    }

    const uintptr_t dist_heap = align_up_uintptr(l2sp_heap, alignof(int32_t));
    const uintptr_t dist_l2sp_bytes =
        (dist_heap < l2sp_limit) ? (l2sp_limit - dist_heap) : 0;
    g_dist_l2sp_count = (int32_t)(
        ((dist_l2sp_bytes / sizeof(int32_t)) > (uintptr_t)hdr_N) ? hdr_N : (dist_l2sp_bytes / sizeof(int32_t)));
    if (g_dist_l2sp_count > 0) {
        dist_arr = (int32_t *)try_alloc_l2sp(
            &l2sp_heap, (size_t)g_dist_l2sp_count * sizeof(int32_t), alignof(int32_t), l2sp_limit);
        g_dist_in_l2sp = 1;
    }
    if (g_dist_l2sp_count < hdr_N) {
        dist_arr_dram = (int32_t *)std::malloc(((size_t)hdr_N - (size_t)g_dist_l2sp_count) * sizeof(int32_t));
    }

    if ((g_visited_l2sp_count > 0 && !visited) ||
        (g_visited_l2sp_count < hdr_N && !visited_dram) ||
        (g_dist_l2sp_count > 0 && !dist_arr) ||
        (g_dist_l2sp_count < hdr_N && !dist_arr_dram)) {
        std::printf("ERROR: allocation failed for visited/dist_arr split buffers\n");
        std::free(buf);
        std::free((void *)visited_dram);
        std::free(dist_arr_dram);
        g_file_buffer = nullptr;
        visited = nullptr;
        visited_dram = nullptr;
        dist_arr = nullptr;
        dist_arr_dram = nullptr;
        g_dist_in_l2sp = 0;
        g_visited_in_l2sp = 0;
        g_dist_l2sp_count = 0;
        g_visited_l2sp_count = 0;
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
        std::free((void *)visited_dram);
        std::free(dist_arr_dram);
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
        visited_dram = nullptr;
        dist_arr = nullptr;
        dist_arr_dram = nullptr;
        g_dist_in_l2sp = 0;
        g_visited_in_l2sp = 0;
        g_dist_l2sp_count = 0;
        g_visited_l2sp_count = 0;
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
    for (int32_t ei = row_start; ei < row_end; ei++) {
        const int64_t v = g_col_idx[ei];
        if (claim_node(v)) {
            dist_set(v, level + 1);
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
    int32_t local_snap_counter = 0;  // snapshot throttle counter

    int empty_streak = 0;

    // Local buffer for stolen items — avoid re-touching victim's cache lines
    int64_t stolen_buf[STEAL_K_MAX];

    int64_t local_backoff = 4;
    const int64_t local_backoff_max = 128;

    while (g_level_remaining.load(std::memory_order_acquire) > 0) {
        const int32_t steal_batch = core_steal_batch_size[my_core];
        const int32_t steal_victims = core_steal_victim_probes[my_core];
        bool should_try_proactive_steal = false;

        {
            const int64_t local_depth = queue_depth(my_queue);
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
                    process_single_node(stolen_buf[i], level);
                    g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
                }
                record_dynamic_wq_event(victim, DYN_WQ_STEAL, tid, my_core);
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

        // Batch pop: grab up to BFS_CHUNK_SIZE nodes at once
        int64_t begin_idx;
        int64_t count = queue_pop_chunk(my_queue, my_core, &begin_idx, BFS_CHUNK_SIZE);

        if (count > 0) {
            for (int64_t i = 0; i < count; i++) {
                int64_t u = frontier_current_get(my_queue->start_idx + begin_idx + i);
                local_processed++;
                process_single_node(u, level);
                g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
            }
            record_dynamic_wq_event(my_core, DYN_WQ_POP, tid, -1);

            // Periodic snapshot of all queue depths
            if (++local_snap_counter % SNAP_EVERY == 0) {
                record_wq_snap(level, WQ_SNAP_POP, my_core, tid);
            }

            empty_streak = 0;
            steal_backoff = 4;
            local_backoff = 4;
        } else {
            // Pop returned 0 — could be CAS contention, not truly empty.
            // Re-check: if local queue still has items, retry immediately.
            {
                int64_t h = atomic_load_i64(&my_queue->head);
                int64_t t = atomic_load_i64(&my_queue->tail);
                if (h < t) {
                    continue;  // CAS contention, queue still has work
                }
            }

            // Queue is truly empty.
            core_has_work[my_core] = 0;

            // Count empty rounds without holding the thief token — every
            // hart can independently track its streak.
            empty_streak++;

            // Short local backoff first, before trying to steal
            if (empty_streak < STEAL_START) {
                wait_check_local(local_backoff, my_core);
                if (local_backoff < local_backoff_max) local_backoff <<= 1;
                continue;
            }

            // Ready to steal — acquire the per-core thief token so only
            // one hart per core issues remote CAS probes at a time.
            if (!thief_token_try_acquire()) {
                // Another hart on this core is already stealing — back off, retry local
                wait_check_local(local_backoff * 4, my_core);
                if (local_backoff < local_backoff_max) local_backoff <<= 1;
                continue;
            }

            bool found = false;
            for (int k = 0; k < steal_victims; k++) {
                // Pick a random victim, skip self, empty hints, and recently-failed
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
                count = queue_pop_chunk(&core_queues[victim], victim, &begin_idx, steal_batch);
                if (count > 0) {
                    local_steal_success++;
                    found = true;

                    // Don't subtract from g_level_remaining here — items are
                    // pushed into local queue and subtracted when actually processed.

                    // Copy stolen items to local buffer, then release victim's cache lines
                    for (int64_t i = 0; i < count; i++) {
                        stolen_buf[i] =
                            frontier_current_get(core_queues[victim].start_idx + begin_idx + i);
                    }
                    // Thief processes ALL stolen items itself — no pushing
                    // back into shared queue where siblings would compete.
                    for (int64_t i = 0; i < count; i++) {
                        local_processed++;
                        process_single_node(stolen_buf[i], level);
                        g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
                    }
                    record_dynamic_wq_event(victim, DYN_WQ_STEAL, tid, my_core);

                    // Snapshot on every successful steal
                    record_wq_snap(level, WQ_SNAP_STEAL, my_core, tid);
                    adapt_steal_policy(my_core, victim_depth_before, count);

                    empty_streak = 0;
                    steal_backoff = 4;
                    local_backoff = 4;
                    // Clear recently-tried on success
                    for (int ri = 0; ri < RECENTLY_TRIED_SIZE; ri++) recently_tried[ri] = -1;
                    break;
                } else {
                    // Record this victim as recently failed
                    recently_tried[rt_idx] = victim;
                    rt_idx = (rt_idx + 1) % RECENTLY_TRIED_SIZE;
                }
            }

            // Release steal token
            thief_token_release();

            if (!found) {
                wait_check_local(steal_backoff, my_core);
                if (steal_backoff < steal_backoff_max) steal_backoff <<= 1;
                continue;
            }
        }
    }

    stat_nodes_processed[tid] += local_processed;
    stat_steal_attempts[tid] += local_steal_attempts;
    stat_steal_success[tid] += local_steal_success;
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
            record_dynamic_wq_event(c, DYN_WQ_REDISTRIBUTE, tid, -1);
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
        record_dynamic_wq_event(c, DYN_WQ_REDISTRIBUTE, tid, -1);
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
        g_dynamic_wq_event_count.store(0, std::memory_order_relaxed);
        g_dynamic_wq_event_dropped.store(0, std::memory_order_relaxed);

        // Load graph from file; abort on failure
        if (!load_graph_from_file()) {
            std::abort();
        }

        const int32_t num_verts = g_num_vertices;
        const int64_t source_id = g_bfs_source;

        for (int64_t i = 0; i < num_verts; i++) {
            visited_set(i, 0);
            dist_set(i, -1);
        }

        for (int i = 0; i < g_total_harts; i++) {
            g_local_sense[i] = 0;
            stat_nodes_processed[i] = 0;
            stat_steal_attempts[i] = 0;
            stat_steal_success[i] = 0;
        }

        for (int c = 0; c < g_total_cores; c++) {
            queue_init(&core_queues[c]);
            core_has_work[c] = 0;
            core_steal_batch_size[c] = STEAL_K_MIN;
            core_steal_victim_probes[c] = STEAL_VICTIMS_MIN;
            core_last_steal_feedback[c] = 0;
            record_dynamic_wq_event(c, DYN_WQ_INIT, -1, -1);
        }
        g_next_frontier.tail = 0;

        visited_set(source_id, 1);
        dist_set(source_id, 0);
        discovered = 1;

        queue_assign_slice(&core_queues[0], 0, 1);
        frontier_current_set(0, source_id);
        core_has_work[0] = 1;
        record_dynamic_wq_event(0, DYN_WQ_REDISTRIBUTE, 0, -1);
        for (int c = 1; c < g_total_cores; c++) {
            queue_assign_slice(&core_queues[c], 1, 0);
            record_dynamic_wq_event(c, DYN_WQ_REDISTRIBUTE, 0, -1);
        }

        std::printf("=== BFS with Adaptive Work Stealing (FIFO + global remaining) ===\n");
        std::printf("Graph: N=%d E=%d (RMAT CSR from rmat.bin)\n",
                    g_num_vertices, g_num_edges);
        std::printf("Graph storage: row_ptr/col_idx in DRAM (malloc)\n");
        std::printf("Hot state: visited in %s, dist_arr in %s\n",
                    (g_visited_l2sp_count == g_num_vertices) ? "L2SP" :
                    (g_visited_l2sp_count == 0) ? "DRAM" : "L2SP+DRAM",
                    (g_dist_l2sp_count == g_num_vertices) ? "L2SP" :
                    (g_dist_l2sp_count == 0) ? "DRAM" : "L2SP+DRAM");
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
        std::printf("Stealing: proactive low-watermark + adaptive batch/probe sizing "
                    "(batch=%d..%d, probes=%d..%d)\n",
                    STEAL_K_MIN, STEAL_K_MAX, STEAL_VICTIMS_MIN, STEAL_VICTIMS_MAX);
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
                    g_bfs_source, dist_get(g_bfs_source));

        int64_t total_processed = 0;
        int64_t total_attempts = 0;
        int64_t total_success = 0;

        std::printf("\nPer-hart statistics:\n");
        std::printf("Hart | Processed | Steal Attempts | Steals OK\n");
        std::printf("-----|-----------|----------------|----------\n");

        for (int h = 0; h < g_total_harts; h++) {
            int64_t processed = stat_nodes_processed[h];
            int64_t attempts  = stat_steal_attempts[h];
            int64_t success   = stat_steal_success[h];

            std::printf("%4d | %9ld | %14ld | %9ld\n",
                        h, (long)processed, (long)attempts, (long)success);

            total_processed += processed;
            total_attempts  += attempts;
            total_success   += success;
        }

        std::printf("\nSummary:\n");
        std::printf("  Total nodes processed: %ld\n", (long)total_processed);
        std::printf("  Total steal attempts:  %ld\n", (long)total_attempts);
        if (total_attempts > 0) {
            std::printf("Successful steals:     %ld (%ld%%)\n",
                        (long)total_success,
                        (long)(100 * total_success / total_attempts));
        }

        uint64_t elapsed = end_cycles - start_cycles;
        std::printf("Cycles elapsed:        %lu\n", (unsigned long)elapsed);
        if (total_processed > 0) {
            std::printf("Cycles per node:       %lu\n",
                        (unsigned long)(elapsed / total_processed));
        }
        dump_wq_trace();

        // Release dynamically-allocated graph memory
        std::free(g_file_buffer);   // owns g_row_ptr and g_col_idx
        std::free((void *)visited_dram);
        std::free(dist_arr_dram);
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
        visited_dram  = nullptr;
        dist_arr      = nullptr;
        dist_arr_dram = nullptr;
        g_dist_in_l2sp = 0;
        g_visited_in_l2sp = 0;
        g_dist_l2sp_count = 0;
        g_visited_l2sp_count = 0;
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
