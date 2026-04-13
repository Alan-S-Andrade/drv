// Level-synchronous BFS with tiered work queues (L1SP + L2SP) + FAA-based queues
// Combines tiered architecture with fetch-and-add for lower-latency queue ops.
// - Per-core L1SP work queue: fast (~1 cycle), FAA-based pop by compute harts
// - Per-core L2SP shared work queue: medium (~10 cycles), FAA-based pop/steal
// - Hart 0 per core: dedicated fetcher, pulls work from L2SP into L1SP
// - Harts 1..N-1: compute harts, pop from L1SP first, then L2SP fallback
// - Work stealing across cores via L2SP queues when both tiers are empty
// - All queue head updates use atomic_fetch_add instead of compare_and_swap

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

static constexpr const char* BENCH_TITLE = "BFS Tiered WQ + FAA (L1SP + L2SP)";
static constexpr const char* BENCH_LOG_PREFIX = "run_tiered_wq_faa";

static constexpr int MAX_HARTS  = 1024;
static constexpr int MAX_CORES  = 64;
static constexpr int MAX_HARTS_PER_CORE = 32;
static constexpr int BFS_CHUNK_SIZE = 64;   // nodes per batch pop
static constexpr int STEAL_K = 64;          // nodes per batch steal
static constexpr int STEAL_VICTIMS = 2;     // victims probed per steal episode

// L1SP work queue sizing
static constexpr int L1_QUEUE_GUARD_BYTES = 4 * 1024;  // reserved for stacks
static constexpr int L1_QUEUE_MAX_ITEMS = 2048;         // hard cap on L1 queue depth
static constexpr int FETCHER_BATCH_SIZE = 128;          // items fetched per L2SP→L1SP transfer
static constexpr int FETCHER_LOW_WATERMARK = 32;        // refill when L1 queue drops below this
static constexpr int FETCHER_HIGH_WATERMARK = 1024;     // stop filling above this

__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

// ────────────────────── L2SP Work Queue (shared per core) ──────────────────────
struct WorkQueue {
    volatile int64_t head;
    volatile int64_t tail;
    int64_t start_idx;
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

// best-effort hints
__l2sp__ volatile int32_t core_has_work[MAX_CORES];

// ────────────────────── L1SP Tiered Work Queue ──────────────────────
// Layout in L1SP (shared by all harts on a core):
//   [0..7]       thief_token (8 bytes)
//   [8..15]      reserved
//   [64..127]    L1QueueHeader (metadata)
//   [128..]      L1 queue item storage (int64_t array)

static constexpr uintptr_t THIEF_TOKEN_L1SP_OFFSET = 0;

struct L1QueueHeader {
    volatile int64_t head;          // consumer index (FAA by compute harts)
    volatile int64_t tail;          // producer index (written by fetcher hart only)
    volatile int64_t capacity;      // max items
    volatile int64_t fetch_active;  // 1 if fetcher is currently transferring
    volatile int64_t stat_fetched;  // total items fetched from L2SP→L1SP
    volatile int64_t stat_fetch_ops;// number of fetch operations
    volatile int64_t stat_fetch_stall_cycles; // cycles fetcher spent waiting
};

static constexpr uintptr_t L1_QUEUE_HEADER_OFFSET = 64;
static constexpr uintptr_t L1_QUEUE_ITEMS_OFFSET =
    L1_QUEUE_HEADER_OFFSET + sizeof(L1QueueHeader);

// Per-core steal token in L1SP
static inline volatile int64_t* thief_token_ptr() {
    uintptr_t l1sp_base;
    asm volatile("csrr %0, " __stringify(MCSR_L1SPBASE) : "=r"(l1sp_base));
    return (volatile int64_t *)(l1sp_base + THIEF_TOKEN_L1SP_OFFSET);
}

static inline bool thief_token_try_acquire() {
    volatile int64_t *tok = thief_token_ptr();
    return atomic_swap_i64(tok, 1) == 0;
}

static inline void thief_token_release() {
    volatile int64_t *tok = thief_token_ptr();
    *tok = 0;
}

static inline void thief_token_init() {
    volatile int64_t *tok = thief_token_ptr();
    *tok = 0;
}

// L1 queue accessors
static inline L1QueueHeader* l1_queue_header_ptr() {
    uintptr_t l1sp_base;
    asm volatile("csrr %0, " __stringify(MCSR_L1SPBASE) : "=r"(l1sp_base));
    return (L1QueueHeader *)(l1sp_base + L1_QUEUE_HEADER_OFFSET);
}

static inline int64_t* l1_queue_items_ptr() {
    uintptr_t l1sp_base;
    asm volatile("csrr %0, " __stringify(MCSR_L1SPBASE) : "=r"(l1sp_base));
    return (int64_t *)(l1sp_base + L1_QUEUE_ITEMS_OFFSET);
}

// Initialize L1 queue (called once per core by hart 0)
static inline void l1_queue_init(int64_t capacity) {
    L1QueueHeader* hdr = l1_queue_header_ptr();
    hdr->head = 0;
    hdr->tail = 0;
    hdr->capacity = capacity;
    hdr->fetch_active = 0;
    hdr->stat_fetched = 0;
    hdr->stat_fetch_ops = 0;
    hdr->stat_fetch_stall_cycles = 0;
}

// L1 queue depth (approximate - head/tail may be stale)
static inline int64_t l1_queue_depth() {
    L1QueueHeader* hdr = l1_queue_header_ptr();
    int64_t depth = hdr->tail - hdr->head;
    return (depth > 0) ? depth : 0;
}

// Pop a batch from L1 queue using FAA (used by compute harts)
// FAA reserves slots atomically — no retry loop needed
static inline int64_t l1_queue_pop_chunk(int64_t* out, int64_t max_chunk) {
    L1QueueHeader* hdr = l1_queue_header_ptr();
    int64_t* items = l1_queue_items_ptr();

    // Speculatively read tail to size the reservation
    int64_t t = atomic_load_i64(&hdr->tail);
    int64_t h_est = atomic_load_i64(&hdr->head);
    if (h_est >= t) return 0;

    int64_t avail = t - h_est;
    int64_t chunk = (avail < max_chunk) ? avail : max_chunk;

    // FAA unconditionally reserves 'chunk' slots — no CAS retry loop
    int64_t old_h = atomic_fetch_add_i64(&hdr->head, chunk);

    // Validate: some or all slots may be past tail
    if (old_h >= t) {
        // Entire reservation is invalid — roll back
        atomic_fetch_add_i64(&hdr->head, -chunk);
        return 0;
    }
    // Clamp to actual available items
    int64_t actual = t - old_h;
    if (actual > chunk) actual = chunk;
    // Roll back unused portion of the reservation
    if (actual < chunk) {
        atomic_fetch_add_i64(&hdr->head, -(chunk - actual));
    }

    int64_t cap = hdr->capacity;
    for (int64_t i = 0; i < actual; i++) {
        out[i] = items[(old_h + i) % cap];
    }
    return actual;
}

// Push a batch to L1 queue (used ONLY by fetcher hart - no atomics needed on tail)
// Returns number of items actually pushed (may be less if queue is full)
static inline int64_t l1_queue_push_batch(const int64_t* values, int64_t count) {
    L1QueueHeader* hdr = l1_queue_header_ptr();
    int64_t* items = l1_queue_items_ptr();

    int64_t t = hdr->tail;
    int64_t h = atomic_load_i64(&hdr->head);
    int64_t cap = hdr->capacity;
    int64_t used = t - h;
    int64_t room = cap - used;

    if (room <= 0) return 0;
    if (count > room) count = room;

    for (int64_t i = 0; i < count; i++) {
        items[(t + i) % cap] = values[i];
    }
    // Memory fence before publishing new tail
    asm volatile("fence w, rw" ::: "memory");
    hdr->tail = t + count;
    return count;
}

// Global remaining-work counter for the current level
__l2sp__ std::atomic<int64_t> g_level_remaining = 0;

// Per-hart/core statistics
__l2sp__ volatile int64_t stat_nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_attempts[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_success[MAX_HARTS];
__l2sp__ volatile int64_t stat_l1_pops[MAX_HARTS];       // items popped from L1SP queue
__l2sp__ volatile int64_t stat_l2_pops[MAX_HARTS];       // items popped directly from L2SP queue
__l2sp__ volatile int64_t stat_stolen_items[MAX_HARTS];   // items from stealing
__l2sp__ volatile int64_t stat_idle_wait_cycles[MAX_HARTS];
__l2sp__ volatile int64_t stat_edges_traversed = 0;
__l2sp__ volatile int64_t discovered = 0;
__l2sp__ volatile uint64_t g_core_l1sp_bytes = 0;
__l2sp__ volatile uint64_t g_hart_stack_capacity_bytes = 0;
__l2sp__ volatile uint64_t g_hart_stack_peak_bytes[MAX_HARTS];

// Per-core fetcher stats
__l2sp__ volatile int64_t stat_fetcher_ops[MAX_CORES];
__l2sp__ volatile int64_t stat_fetcher_items[MAX_CORES];
__l2sp__ volatile int64_t stat_fetcher_stall_cycles[MAX_CORES];
__l2sp__ volatile int64_t stat_fetcher_idle_cycles[MAX_CORES];

// ────────────────────── Queue Depth Sampling ──────────────────────
static constexpr int QDEPTH_MAX_LEVELS = 64;
static constexpr int QDEPTH_SAMPLE_INTERVAL = 32;  // sample every N loop iterations

struct QDepthStats {
    int64_t l1_sum;
    int64_t l2_sum;
    int64_t l1_min;
    int64_t l1_max;
    int64_t l2_min;
    int64_t l2_max;
    int64_t samples;
    int64_t idle_samples;  // samples where both L1+L2 were 0
};

// Per-core, per-level depth stats (written by fetcher hart only)
__l2sp__ QDepthStats qdepth[MAX_CORES][QDEPTH_MAX_LEVELS];

static inline void qdepth_reset(int core, int level) {
    if (level >= QDEPTH_MAX_LEVELS) return;
    QDepthStats* s = &qdepth[core][level];
    s->l1_sum = 0; s->l2_sum = 0;
    s->l1_min = INT64_MAX; s->l1_max = 0;
    s->l2_min = INT64_MAX; s->l2_max = 0;
    s->samples = 0; s->idle_samples = 0;
}

static inline void qdepth_sample(int core, int level, int64_t l1d, int64_t l2d) {
    if (level >= QDEPTH_MAX_LEVELS) return;
    QDepthStats* s = &qdepth[core][level];
    s->l1_sum += l1d;
    s->l2_sum += l2d;
    if (l1d < s->l1_min) s->l1_min = l1d;
    if (l1d > s->l1_max) s->l1_max = l1d;
    if (l2d < s->l2_min) s->l2_min = l2d;
    if (l2d > s->l2_max) s->l2_max = l2d;
    s->samples++;
    if (l1d == 0 && l2d == 0) s->idle_samples++;
}

// ────────────────────── Barrier ──────────────────────
__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;

static inline int get_thread_id() {
    // Use shift of 6 to support up to 64 harts/core without needing
    // g_harts_per_core (which isn't set at first call in main())
    return (myCoreId() << 6) + myThreadId();
}

static inline void spin_pause(int64_t iters) {
    for (int64_t i = 0; i < iters; i++) {
        asm volatile("" ::: "memory");
    }
}

static inline void wait_check_local(int64_t backoff, int my_core) {
    if (core_has_work[my_core]) {
        return;
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

// ────────────────────── L2SP Queue Operations (FAA-based) ──────────────────────
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

// FAA-based batch pop from L2SP queue (no CAS retry loop)
static inline int64_t queue_pop_chunk_faa(WorkQueue* q, int core_id, int64_t* out_begin, int64_t max_chunk) {
    // Speculatively read tail to size the reservation
    int64_t t = atomic_load_i64(&q->tail);
    int64_t h_est = atomic_load_i64(&q->head);
    if (h_est >= t) {
        core_has_work[core_id] = 0;
        return 0;
    }
    int64_t avail = t - h_est;
    int64_t chunk = (avail < max_chunk) ? avail : max_chunk;

    // FAA unconditionally reserves 'chunk' slots
    int64_t old_h = atomic_fetch_add_i64(&q->head, chunk);

    // Validate: some or all slots may be past tail
    if (old_h >= t) {
        // Entire reservation is invalid — roll back
        atomic_fetch_add_i64(&q->head, -chunk);
        core_has_work[core_id] = 0;
        return 0;
    }
    // Clamp to actual available items
    int64_t actual = t - old_h;
    if (actual > chunk) actual = chunk;
    // Roll back unused portion of the reservation
    if (actual < chunk) {
        atomic_fetch_add_i64(&q->head, -(chunk - actual));
    }
    *out_begin = old_h;
    return actual;
}

// ────────────────────── BFS Shared State ──────────────────────
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

// Trace infrastructure
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
__l2sp__ volatile int32_t g_wq_trace_count = 0;
__l2sp__ volatile int32_t g_wq_trace_dropped = 0;

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

static void dump_traces() {
    auto emit = [&](FILE* out) {
        std::fprintf(out, "WQTRACE_DUMP_BEGIN,bench=%s,cores=%d,samples=%d,dropped=%d\n",
                     BENCH_LOG_PREFIX, g_total_cores, (int)g_wq_trace_count, (int)g_wq_trace_dropped);
        for (int32_t i = 0; i < g_wq_trace_count; i++) {
            const WQTraceSample& s = g_wq_trace_samples[i];
            std::fprintf(out, "WQTRACE,bench=%s,cores=%d,sample=%d,phase=%s,level=%d,depths=",
                         BENCH_LOG_PREFIX, g_total_cores, (int)i, wq_phase_name(s.phase), (int)s.level);
            for (int c = 0; c < g_total_cores; c++) {
                if (c > 0) std::fprintf(out, "|");
                std::fprintf(out, "%d", (int)g_wq_trace_depths[i][c]);
            }
            std::fprintf(out, "\n");
        }
        std::fprintf(out, "WQTRACE_DUMP_END,bench=%s\n", BENCH_LOG_PREFIX);
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

// ────────────────────── Graph Loading ──────────────────────
static bool load_graph_from_file() {
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

    g_row_ptr = (int32_t *)(buf + 20);
    g_col_idx = (int32_t *)(buf + 20 + (size_t)(hdr_N + 1) * sizeof(int32_t));

    // Use highest-degree vertex as BFS source
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
    uintptr_t l2sp_heap = align_up_uintptr((uintptr_t)l2sp_end, 8);
    const uintptr_t l2sp_base = 0x20000000;
    const uintptr_t l2sp_limit = l2sp_base + podL2SPSize();

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
        std::printf("ERROR: allocation failed for dynamic frontier buffers\n");
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

// ────────────────────── BFS Node Processing ──────────────────────
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

// ────────────────────── Victim Selection ──────────────────────
static inline uint32_t xorshift_victim(uint32_t seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static constexpr int RECENTLY_TRIED_SIZE = 4;

// ────────────────────── Fetcher Hart (hart 0 per core) ──────────────────────
// Dedicated to pulling work from L2SP queue into L1SP queue using FAA.
// Does NOT process BFS nodes itself - purely a data mover.
static void fetcher_loop(int tid, int32_t level) {
    const int harts_per_core = g_harts_per_core;
    const int total_cores = g_total_cores;
    const int my_core = tid / harts_per_core;

    WorkQueue* my_queue = &core_queues[my_core];

    uint64_t local_fetch_ops = 0;
    uint64_t local_fetch_items = 0;
    uint64_t local_stall_cycles = 0;
    uint64_t local_idle_cycles = 0;

    // Xorshift RNG for steal victim selection
    uint32_t rng_state = (uint32_t)(tid + 1) * 2654435761u;
    int recently_tried[RECENTLY_TRIED_SIZE];
    for (int i = 0; i < RECENTLY_TRIED_SIZE; i++) recently_tried[i] = -1;
    int rt_idx = 0;

    int64_t steal_backoff = 4;
    const int64_t steal_backoff_max = 256;

    int64_t tmp_buf[FETCHER_BATCH_SIZE];

    while (g_level_remaining.load(std::memory_order_acquire) > 0) {
        int64_t l1_depth = l1_queue_depth();

        // If L1 queue is below low watermark, fetch from L2SP queue
        if (l1_depth < FETCHER_LOW_WATERMARK) {
            int64_t begin_idx = 0;
            int64_t fetch_count = FETCHER_BATCH_SIZE;
            // Don't overfill
            int64_t room = FETCHER_HIGH_WATERMARK - l1_depth;
            if (fetch_count > room) fetch_count = room;
            if (fetch_count <= 0) fetch_count = FETCHER_BATCH_SIZE / 2;

            // FAA-based pop from L2SP queue (no CAS retry)
            int64_t got = queue_pop_chunk_faa(my_queue, my_core, &begin_idx, fetch_count);
            if (got > 0) {
                // Read items from frontier into temp buffer
                for (int64_t i = 0; i < got; i++) {
                    tmp_buf[i] = frontier_current_get(my_queue->start_idx + begin_idx + i);
                }

                // Push into L1SP queue
                int64_t pushed = l1_queue_push_batch(tmp_buf, got);

                local_fetch_ops++;
                local_fetch_items += pushed;
                core_has_work[my_core] = 1;
                steal_backoff = 4;
                for (int ri = 0; ri < RECENTLY_TRIED_SIZE; ri++) recently_tried[ri] = -1;
                continue;
            }

            // L2SP local queue empty - try stealing from other cores
            if (total_cores > 1 && thief_token_try_acquire()) {
                bool found = false;
                for (int k = 0; k < STEAL_VICTIMS; k++) {
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

                    begin_idx = 0;
                    // FAA-based steal from victim's L2SP queue
                    got = queue_pop_chunk_faa(&core_queues[victim], victim, &begin_idx, STEAL_K);
                    if (got > 0) {
                        found = true;
                        for (int64_t i = 0; i < got; i++) {
                            tmp_buf[i] = frontier_current_get(core_queues[victim].start_idx + begin_idx + i);
                        }
                        // Push stolen items into L1SP queue
                        int64_t pushed = l1_queue_push_batch(tmp_buf, got);
                        local_fetch_ops++;
                        local_fetch_items += pushed;
                        core_has_work[my_core] = 1;
                        steal_backoff = 4;
                        for (int ri = 0; ri < RECENTLY_TRIED_SIZE; ri++) recently_tried[ri] = -1;
                        break;
                    } else {
                        recently_tried[rt_idx] = victim;
                        rt_idx = (rt_idx + 1) % RECENTLY_TRIED_SIZE;
                    }
                }
                thief_token_release();

                if (!found) {
                    uint64_t ws = 0, we = 0;
                    asm volatile("rdcycle %0" : "=r"(ws));
                    wait_check_local(steal_backoff, my_core);
                    asm volatile("rdcycle %0" : "=r"(we));
                    local_idle_cycles += (we - ws);
                    if (steal_backoff < steal_backoff_max) steal_backoff <<= 1;
                }
            } else {
                // Can't steal (either single core or another hart has token)
                uint64_t ws = 0, we = 0;
                asm volatile("rdcycle %0" : "=r"(ws));
                spin_pause(8);
                asm volatile("rdcycle %0" : "=r"(we));
                local_idle_cycles += (we - ws);
            }
        } else {
            // L1 queue has enough work - short pause then check again
            spin_pause(4);
        }
    }

    stat_fetcher_ops[my_core] += (int64_t)local_fetch_ops;
    stat_fetcher_items[my_core] += (int64_t)local_fetch_items;
    stat_fetcher_stall_cycles[my_core] += (int64_t)local_stall_cycles;
    stat_fetcher_idle_cycles[my_core] += (int64_t)local_idle_cycles;
}

// ────────────────────── Compute Hart BFS Level Processing ──────────────────────
// Harts 1..N-1 process BFS nodes using tiered queues with FAA:
//   1. Pop from L1SP queue (fast, ~1 cycle, FAA-based)
//   2. If L1SP empty, pop directly from L2SP queue (FAA-based fallback)
//   3. If both empty, wait for fetcher to refill L1SP or level to end
static void compute_hart_bfs_level(int tid, int32_t level) {
    const int harts_per_core = g_harts_per_core;
    const int my_core = tid / harts_per_core;
    const int total_cores = g_total_cores;

    WorkQueue* my_queue = &core_queues[my_core];

    int64_t local_processed = 0;
    int64_t local_l1_pops = 0;
    int64_t local_l2_pops = 0;
    int64_t local_stolen_items = 0;
    int64_t local_steal_attempts = 0;
    int64_t local_steal_success = 0;
    int64_t local_idle_cycles = 0;

    int64_t local_buf[BFS_CHUNK_SIZE];
    int64_t stolen_buf[STEAL_K];

    int empty_streak = 0;
    int64_t backoff = 4;
    const int64_t backoff_max = 128;

    // Xorshift RNG for emergency direct stealing (rarely used)
    uint32_t rng_state = (uint32_t)(tid + 1) * 2654435761u;

    while (g_level_remaining.load(std::memory_order_acquire) > 0) {
        // Tier 1: Pop from L1SP queue (fast path, FAA-based)
        int64_t count = l1_queue_pop_chunk(local_buf, BFS_CHUNK_SIZE);
        if (count > 0) {
            for (int64_t i = 0; i < count; i++) {
                local_processed++;
                local_l1_pops++;
                process_single_node(local_buf[i], level);
                g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
            }
            empty_streak = 0;
            backoff = 4;
            continue;
        }

        // Tier 2: Pop directly from L2SP queue (FAA-based fallback)
        {
            int64_t begin_idx = 0;
            count = queue_pop_chunk_faa(my_queue, my_core, &begin_idx, BFS_CHUNK_SIZE);
            if (count > 0) {
                for (int64_t i = 0; i < count; i++) {
                    int64_t u = frontier_current_get(my_queue->start_idx + begin_idx + i);
                    local_processed++;
                    local_l2_pops++;
                    process_single_node(u, level);
                    g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
                }
                empty_streak = 0;
                backoff = 4;
                continue;
            }
        }

        // Both tiers empty — wait and retry
        empty_streak++;

        // Compute harts also attempt stealing if truly idle for long enough
        if (empty_streak >= 4 && total_cores > 1 && thief_token_try_acquire()) {
            bool found = false;
            for (int k = 0; k < STEAL_VICTIMS; k++) {
                rng_state = xorshift_victim(rng_state);
                int victim = (int)(rng_state % (uint32_t)total_cores);
                if (victim == my_core || core_has_work[victim] == 0) continue;

                local_steal_attempts++;
                int64_t begin_idx = 0;
                // FAA-based steal
                count = queue_pop_chunk_faa(&core_queues[victim], victim, &begin_idx, STEAL_K);
                if (count > 0) {
                    local_steal_success++;
                    found = true;
                    for (int64_t i = 0; i < count; i++) {
                        stolen_buf[i] = frontier_current_get(core_queues[victim].start_idx + begin_idx + i);
                    }
                    for (int64_t i = 0; i < count; i++) {
                        local_processed++;
                        local_stolen_items++;
                        process_single_node(stolen_buf[i], level);
                        g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
                    }
                    empty_streak = 0;
                    backoff = 4;
                    break;
                }
            }
            thief_token_release();

            if (!found) {
                uint64_t ws = 0, we = 0;
                asm volatile("rdcycle %0" : "=r"(ws));
                wait_check_local(backoff, my_core);
                asm volatile("rdcycle %0" : "=r"(we));
                local_idle_cycles += (int64_t)(we - ws);
                if (backoff < backoff_max) backoff <<= 1;
            }
        } else {
            uint64_t ws = 0, we = 0;
            asm volatile("rdcycle %0" : "=r"(ws));
            wait_check_local(backoff, my_core);
            asm volatile("rdcycle %0" : "=r"(we));
            local_idle_cycles += (int64_t)(we - ws);
            if (backoff < backoff_max) backoff <<= 1;
        }
    }

    stat_nodes_processed[tid] += local_processed;
    stat_l1_pops[tid] += local_l1_pops;
    stat_l2_pops[tid] += local_l2_pops;
    stat_stolen_items[tid] += local_stolen_items;
    stat_steal_attempts[tid] += local_steal_attempts;
    stat_steal_success[tid] += local_steal_success;
    stat_idle_wait_cycles[tid] += local_idle_cycles;
}

// ────────────────────── Compute Hart with Queue Depth Sampling ──────────────────────
// Same as compute_hart_bfs_level but hart 1 per core samples L1/L2SP depth
static void compute_hart_bfs_level_sampled(int tid, int32_t level,
                                            int my_core, WorkQueue* my_queue) {
    const int harts_per_core = g_harts_per_core;
    const int total_cores = g_total_cores;

    int64_t local_processed = 0;
    int64_t local_l1_pops = 0;
    int64_t local_l2_pops = 0;
    int64_t local_stolen_items = 0;
    int64_t local_steal_attempts = 0;
    int64_t local_steal_success = 0;
    int64_t local_idle_cycles = 0;

    int64_t local_buf[BFS_CHUNK_SIZE];
    int64_t stolen_buf[STEAL_K];

    int empty_streak = 0;
    int64_t backoff = 4;
    const int64_t backoff_max = 128;
    int64_t sample_counter = 0;

    uint32_t rng_state = (uint32_t)(tid + 1) * 2654435761u;

    while (g_level_remaining.load(std::memory_order_acquire) > 0) {
        // Periodic queue depth sampling
        if ((sample_counter++ % QDEPTH_SAMPLE_INTERVAL) == 0) {
            int64_t l1d = l1_queue_depth();
            int64_t l2d = queue_depth(my_queue);
            qdepth_sample(my_core, level, l1d, l2d);
        }

        // Tier 1: Pop from L1SP queue (fast path, FAA-based)
        int64_t count = l1_queue_pop_chunk(local_buf, BFS_CHUNK_SIZE);
        if (count > 0) {
            for (int64_t i = 0; i < count; i++) {
                local_processed++;
                local_l1_pops++;
                process_single_node(local_buf[i], level);
                g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
            }
            empty_streak = 0;
            backoff = 4;
            continue;
        }

        // Tier 2: Pop directly from L2SP queue (FAA-based fallback)
        {
            int64_t begin_idx = 0;
            count = queue_pop_chunk_faa(my_queue, my_core, &begin_idx, BFS_CHUNK_SIZE);
            if (count > 0) {
                for (int64_t i = 0; i < count; i++) {
                    int64_t u = frontier_current_get(my_queue->start_idx + begin_idx + i);
                    local_processed++;
                    local_l2_pops++;
                    process_single_node(u, level);
                    g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
                }
                empty_streak = 0;
                backoff = 4;
                continue;
            }
        }

        // Both tiers empty — wait and retry
        empty_streak++;

        if (empty_streak >= 4 && total_cores > 1 && thief_token_try_acquire()) {
            bool found = false;
            for (int k = 0; k < STEAL_VICTIMS; k++) {
                rng_state = xorshift_victim(rng_state);
                int victim = (int)(rng_state % (uint32_t)total_cores);
                if (victim == my_core || core_has_work[victim] == 0) continue;

                local_steal_attempts++;
                int64_t begin_idx = 0;
                count = queue_pop_chunk_faa(&core_queues[victim], victim, &begin_idx, STEAL_K);
                if (count > 0) {
                    local_steal_success++;
                    found = true;
                    for (int64_t i = 0; i < count; i++) {
                        stolen_buf[i] = frontier_current_get(core_queues[victim].start_idx + begin_idx + i);
                    }
                    for (int64_t i = 0; i < count; i++) {
                        local_processed++;
                        local_stolen_items++;
                        process_single_node(stolen_buf[i], level);
                        g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
                    }
                    empty_streak = 0;
                    backoff = 4;
                    break;
                }
            }
            thief_token_release();

            if (!found) {
                uint64_t ws = 0, we = 0;
                asm volatile("rdcycle %0" : "=r"(ws));
                wait_check_local(backoff, my_core);
                asm volatile("rdcycle %0" : "=r"(we));
                local_idle_cycles += (int64_t)(we - ws);
                if (backoff < backoff_max) backoff <<= 1;
            }
        } else {
            uint64_t ws = 0, we = 0;
            asm volatile("rdcycle %0" : "=r"(ws));
            wait_check_local(backoff, my_core);
            asm volatile("rdcycle %0" : "=r"(we));
            local_idle_cycles += (int64_t)(we - ws);
            if (backoff < backoff_max) backoff <<= 1;
        }
    }

    stat_nodes_processed[tid] += local_processed;
    stat_l1_pops[tid] += local_l1_pops;
    stat_l2_pops[tid] += local_l2_pops;
    stat_stolen_items[tid] += local_stolen_items;
    stat_steal_attempts[tid] += local_steal_attempts;
    stat_steal_success[tid] += local_steal_success;
    stat_idle_wait_cycles[tid] += local_idle_cycles;
}

// ────────────────────── BFS Level Dispatch ──────────────────────
static void process_bfs_level(int tid, int32_t level) {
    const int my_local_id = tid % g_harts_per_core;

    if (my_local_id == 0) {
        // Hart 0: dedicated fetcher
        fetcher_loop(tid, level);
    } else if (my_local_id == 1) {
        // Hart 1 on each core: compute + queue depth sampling
        const int my_core = tid / g_harts_per_core;
        WorkQueue* my_queue = &core_queues[my_core];
        qdepth_reset(my_core, level);
        compute_hart_bfs_level_sampled(tid, level, my_core, my_queue);
    } else {
        // Harts 2..N-1: compute harts
        compute_hart_bfs_level(tid, level);
    }
}

// ────────────────────── Level Advance ──────────────────────
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

    int64_t per_core = total_nodes / total_cores;
    int64_t remainder = total_nodes % total_cores;

    int64_t base = 0;
    for (int c = 0; c < total_cores; c++) {
        int64_t count = per_core + (c < remainder ? 1 : 0);
        queue_assign_slice(&core_queues[c], base, count);
        core_has_work[c] = (count > 0) ? 1 : 0;
        base += count;
    }

    for (int64_t i = 0; i < total_nodes; i++) {
        frontier_current_set(i, frontier_next_item_get(i));
    }

    g_next_frontier.tail = 0;
}

// ────────────────────── Work Count ──────────────────────
static int64_t count_total_work() {
    int64_t total = 0;
    for (int c = 0; c < g_total_cores; c++) {
        int64_t count = core_queues[c].tail - core_queues[c].head;
        if (count > 0) total += count;  // FAA may push head past tail
    }
    return total;
}

// ────────────────────── BFS Driver ──────────────────────
static void bfs() {
    const int tid = get_thread_id();

    if (tid == 0) {
        g_wq_trace_count = 0;
        g_wq_trace_dropped = 0;

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
            stat_l1_pops[i] = 0;
            stat_l2_pops[i] = 0;
            stat_stolen_items[i] = 0;
            stat_idle_wait_cycles[i] = 0;
            g_hart_stack_peak_bytes[i] = 0;
        }

        for (int c = 0; c < g_total_cores; c++) {
            queue_init(&core_queues[c]);
            core_has_work[c] = 0;
            stat_fetcher_ops[c] = 0;
            stat_fetcher_items[c] = 0;
            stat_fetcher_stall_cycles[c] = 0;
            stat_fetcher_idle_cycles[c] = 0;
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

        g_core_l1sp_bytes = coreL1SPSize();
        g_hart_stack_capacity_bytes = g_core_l1sp_bytes / (uint64_t)g_harts_per_core;

        std::printf("=== %s ===\n", BENCH_TITLE);
        std::printf("Graph: N=%d E=%d (RMAT CSR from rmat_r16.bin)\n",
                    g_num_vertices, g_num_edges);
        std::printf("Graph storage: row_ptr/col_idx in DRAM (malloc)\n");
        std::printf("Hot state: visited in %s, dist_arr in %s\n",
                    g_visited_in_l2sp ? "L2SP" : "DRAM",
                    g_dist_in_l2sp ? "L2SP" : "DRAM");
        const char* frontier_storage =
            (g_next_frontier.l2sp_capacity == 0) ? "DRAM" :
            (g_next_frontier.l2sp_capacity == g_next_frontier.capacity) ? "L2SP" :
            "L2SP+DRAM";
        std::printf("Frontiers: dynamic %s buffers with balanced repartition "
                    "(capacity=%d vertices, l2sp_vertices=%ld)\n",
                    frontier_storage,
                    g_num_vertices,
                    (long)g_next_frontier.l2sp_capacity);
        std::printf("Hardware: %d cores x %d harts = %d total harts\n",
                    g_total_cores, g_harts_per_core, g_total_harts);
        std::printf("  Fetcher harts: %d (hart 0 per core)\n", g_total_cores);
        std::printf("  Compute harts: %d (harts 1..%d per core)\n",
                    g_total_cores * (g_harts_per_core - 1), g_harts_per_core - 1);
        std::printf("Source: node %ld (highest-degree vertex)\n", (long)source_id);
        std::printf("L1SP: per-core=%lu bytes, global=%lu bytes\n",
                    (unsigned long)g_core_l1sp_bytes,
                    (unsigned long)(g_core_l1sp_bytes * (uint64_t)g_total_cores));
        std::printf("Queue ops: FAA (fetch-and-add) on L1SP and L2SP heads\n");
        std::printf("Tiered queue: L1SP (fetcher batch=%d, low_wm=%d, high_wm=%d) -> L2SP -> steal\n",
                    FETCHER_BATCH_SIZE, FETCHER_LOW_WATERMARK, FETCHER_HIGH_WATERMARK);
        std::printf("\n");
        record_wq_trace(WQ_PHASE_INIT, 0);

        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }

    // Hart 0 per core: init L1SP structures
    if (myThreadId() == 0) {
        thief_token_init();
        const uint64_t l1sp_total = g_core_l1sp_bytes;
        int64_t queue_bytes = (int64_t)l1sp_total - (int64_t)L1_QUEUE_ITEMS_OFFSET
                              - (int64_t)L1_QUEUE_GUARD_BYTES;
        if (queue_bytes < 0) queue_bytes = 0;
        int64_t capacity = queue_bytes / (int64_t)sizeof(int64_t);
        if (capacity > L1_QUEUE_MAX_ITEMS) capacity = L1_QUEUE_MAX_ITEMS;
        l1_queue_init(capacity);
    }

    barrier();

    uint64_t start_cycles = 0;
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(start_cycles));
    }

    int32_t level = 0;

    while (true) {
        int64_t total_work = count_total_work();

        if (tid == 0) {
            g_level_remaining.store(total_work, std::memory_order_release);
        }

        barrier();

        if (total_work <= 0) break;

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

        // Reset L1 queues at level start (fetcher will refill)
        if (myThreadId() == 0) {
            L1QueueHeader* hdr = l1_queue_header_ptr();
            hdr->head = 0;
            hdr->tail = 0;
        }

        barrier();
        ph_stat_phase(1);
        process_bfs_level(tid, level);
        ph_stat_phase(0);
        barrier();

        if (tid == 0) {
            record_wq_trace(WQ_PHASE_POST_PROCESS, level);
        }

        barrier();
        advance_to_next_level_balanced(tid);
        barrier();

        if (tid == 0) {
            record_wq_trace(WQ_PHASE_POST_REDISTRIBUTE, level);
        }

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

    if (tid == 0) {
        std::printf("\n=== BFS Complete ===\n");
        std::printf("Levels traversed: %d\n", level);
        std::printf("Nodes discovered: %ld / %d\n", (long)discovered, g_num_vertices);
        std::printf("dist[source=%d] = %d (expected 0)\n",
                    g_bfs_source, dist_arr[g_bfs_source]);

        int64_t total_processed = 0;
        int64_t total_l1 = 0;
        int64_t total_l2 = 0;
        int64_t total_stolen = 0;
        int64_t total_steal_attempts = 0;
        int64_t total_steal_success = 0;
        int64_t total_idle = 0;

        std::printf("\nPer-hart statistics:\n");
        std::printf("Hart | Role     | Processed | L1 Pops | L2 Pops | Stolen | Steal Att | Steal OK | Idle Cyc\n");
        std::printf("-----|----------|-----------|---------|---------|--------|-----------|----------|----------\n");

        for (int h = 0; h < g_total_harts; h++) {
            const char* role = (h % g_harts_per_core == 0) ? "fetcher" : "compute";
            std::printf("%4d | %-8s | %9ld | %7ld | %7ld | %6ld | %9ld | %8ld | %8ld\n",
                        h, role,
                        (long)stat_nodes_processed[h],
                        (long)stat_l1_pops[h],
                        (long)stat_l2_pops[h],
                        (long)stat_stolen_items[h],
                        (long)stat_steal_attempts[h],
                        (long)stat_steal_success[h],
                        (long)stat_idle_wait_cycles[h]);

            total_processed += stat_nodes_processed[h];
            total_l1 += stat_l1_pops[h];
            total_l2 += stat_l2_pops[h];
            total_stolen += stat_stolen_items[h];
            total_steal_attempts += stat_steal_attempts[h];
            total_steal_success += stat_steal_success[h];
            total_idle += stat_idle_wait_cycles[h];
        }

        std::printf("\nPer-core fetcher statistics:\n");
        std::printf("Core | Fetch Ops | Items Fetched | Idle Cycles\n");
        std::printf("-----|-----------|---------------|------------\n");
        int64_t grand_fetch_ops = 0, grand_fetch_items = 0, grand_fetch_idle = 0;
        for (int c = 0; c < g_total_cores; c++) {
            std::printf("%4d | %9ld | %13ld | %10ld\n",
                        c, (long)stat_fetcher_ops[c], (long)stat_fetcher_items[c],
                        (long)stat_fetcher_idle_cycles[c]);
            grand_fetch_ops += stat_fetcher_ops[c];
            grand_fetch_items += stat_fetcher_items[c];
            grand_fetch_idle += stat_fetcher_idle_cycles[c];
        }

        std::printf("\nSummary:\n");
        std::printf("  Total nodes processed: %ld\n", (long)total_processed);
        std::printf("  Total edges traversed: %ld\n", (long)stat_edges_traversed);
        std::printf("  From L1SP queue:       %ld\n", (long)total_l1);
        std::printf("  From L2SP queue:       %ld\n", (long)total_l2);
        std::printf("  From stealing:         %ld\n", (long)total_stolen);
        std::printf("  L1 hit rate:           ");
        if (total_processed > 0) {
            std::printf("%ld%%\n", (long)(100 * total_l1 / total_processed));
        } else {
            std::printf("N/A\n");
        }
        std::printf("  Steal attempts:        %ld\n", (long)total_steal_attempts);
        std::printf("  Steal success:         %ld\n", (long)total_steal_success);
        std::printf("  Fetcher ops total:     %ld\n", (long)grand_fetch_ops);
        std::printf("  Fetcher items total:   %ld\n", (long)grand_fetch_items);
        std::printf("  Compute idle cycles:   %ld\n", (long)total_idle);
        std::printf("  Fetcher idle cycles:   %ld\n", (long)grand_fetch_idle);

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
        std::printf("TIERED_FAA_STATS: l1_pops=%ld l2_pops=%ld stolen=%ld fetch_ops=%ld fetch_items=%ld\n",
                    (long)total_l1, (long)total_l2, (long)total_stolen,
                    (long)grand_fetch_ops, (long)grand_fetch_items);

        // Queue depth report
        std::printf("\n=== Queue Depth per Level per Core ===\n");
        std::printf("Core | Level | L1avg | L1min | L1max | L2avg | L2min | L2max | Samples | Idle(%%) \n");
        std::printf("-----|-------|-------|-------|-------|-------|-------|-------|---------|--------\n");
        for (int c = 0; c < g_total_cores; c++) {
            for (int lv = 0; lv < level && lv < QDEPTH_MAX_LEVELS; lv++) {
                QDepthStats* s = &qdepth[c][lv];
                if (s->samples == 0) continue;
                int64_t l1avg = s->l1_sum / s->samples;
                int64_t l2avg = s->l2_sum / s->samples;
                int64_t idle_pct = 100 * s->idle_samples / s->samples;
                std::printf("%4d | %5d | %5ld | %5ld | %5ld | %5ld | %5ld | %5ld | %7ld | %5ld%%\n",
                            c, lv, (long)l1avg,
                            (long)(s->l1_min == INT64_MAX ? 0 : s->l1_min), (long)s->l1_max,
                            (long)l2avg,
                            (long)(s->l2_min == INT64_MAX ? 0 : s->l2_min), (long)s->l2_max,
                            (long)s->samples, (long)idle_pct);
            }
        }

        // Aggregate idle summary
        std::printf("\nQDEPTH_SUMMARY:");
        for (int lv = 0; lv < level && lv < QDEPTH_MAX_LEVELS; lv++) {
            int64_t total_samples = 0, total_idle_samples = 0;
            int64_t total_l1_sum = 0, total_l2_sum = 0;
            for (int c = 0; c < g_total_cores; c++) {
                QDepthStats* s = &qdepth[c][lv];
                total_samples += s->samples;
                total_idle_samples += s->idle_samples;
                total_l1_sum += s->l1_sum;
                total_l2_sum += s->l2_sum;
            }
            if (total_samples > 0) {
                std::printf(" lv%d:L1avg=%ld,L2avg=%ld,idle=%ld%%",
                            lv, (long)(total_l1_sum / total_samples),
                            (long)(total_l2_sum / total_samples),
                            (long)(100 * total_idle_samples / total_samples));
            }
        }
        std::printf("\n");

        dump_traces();

        // Release dynamically-allocated memory
        std::free(g_file_buffer);
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
    // Every hart sets globals before the first barrier to avoid a race
    // where other harts read g_total_harts=0 inside barrier().
    // All harts query the same hardware values, so writes are idempotent.
    g_total_cores = numPodCores();
    g_harts_per_core = myCoreThreads();
    g_total_harts = g_total_cores * g_harts_per_core;

    const int tid = get_thread_id();

    barrier();
    bfs();
    barrier();

    return 0;
}
