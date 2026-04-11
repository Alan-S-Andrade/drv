// Level-synchronous BFS with L1SP visited-set bitmap filter
//
// Key idea: Use unused L1SP space for a per-core visited bitmap that
// REDUCES TOTAL WORK rather than hiding latency.  Before each expensive
// L2SP atomic_swap on visited[v], a single-cycle L1SP bitmap check
// filters out already-visited vertices.  This eliminates redundant
// L2SP atomics, especially in later BFS levels where most vertices
// are already visited.
//
// For N=1024 vertices, bitmap = 128 bytes per core (<0.05% of L1SP).
// The bitmap is a best-effort filter: false negatives possible across
// cores but correctness is guaranteed by the authoritative L2SP atomic.

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
static constexpr int BFS_CHUNK_SIZE = 64;  // nodes per batch local pop
static constexpr int STEAL_K = 64;         // nodes per batch steal (tunable independently)

// -------------------- L1SP Visited Bitmap --------------------
// Per-core bitmap placed at a fixed L1SP offset.  All 16 harts on a
// core share the same bitmap.  Size = ceil(N/8) bytes.
// Placed right after the 8-byte thief token at offset 16 (aligned).
static constexpr uintptr_t BITMAP_L1SP_OFFSET = 16;

// Filter statistics — per-core to avoid false-sharing
__l2sp__ volatile int64_t stat_filter_checks[MAX_CORES];   // total neighbor edges checked
__l2sp__ volatile int64_t stat_filter_hits[MAX_CORES];     // skipped by L1SP bitmap
__l2sp__ volatile int64_t stat_filter_misses[MAX_CORES];   // fell through to L2SP atomic
__l2sp__ volatile int64_t stat_edges_traversed = 0;

// Get pointer to this core's visited bitmap in L1SP (64-bit word array)
static inline volatile int64_t* core_bitmap_ptr() {
    uintptr_t l1sp_base;
    asm volatile("csrr %0, " __stringify(MCSR_L1SPBASE) : "=r"(l1sp_base));
    return (volatile int64_t*)(l1sp_base + BITMAP_L1SP_OFFSET);
}

// Test whether vertex v is marked in the local L1SP bitmap (64-bit words)
static inline bool bitmap_test(volatile int64_t* bitmap, int64_t v) {
    return (bitmap[v >> 6] & ((int64_t)1 << (v & 63))) != 0;
}

// Set vertex v in the local L1SP bitmap (64-bit word read-modify-write)
static inline void bitmap_set(volatile int64_t* bitmap, int64_t v) {
    bitmap[v >> 6] |= ((int64_t)1 << (v & 63));
}

// Initialize this core's bitmap to all zeros (called by hart 0 of each core)
static inline void bitmap_init(int32_t num_vertices) {
    volatile int64_t* bitmap = core_bitmap_ptr();
    const int32_t bitmap_words = (num_vertices + 63) / 64;
    for (int32_t i = 0; i < bitmap_words; i++) {
        bitmap[i] = 0;
    }
}

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

static inline bool next_frontier_push_atomic(int64_t work,
                                              int64_t &l2sp_wq, int64_t &l2sp_frontier) {
    l2sp_wq++;  // atomic on g_next_frontier.tail (in L2SP)
    int64_t idx = atomic_fetch_add_i64(&g_next_frontier.tail, 1);
    if (idx >= g_next_frontier.capacity) {
        return false;
    }
    if (idx < g_next_frontier.l2sp_capacity) {
        l2sp_frontier += 2;  // item write + owner write in L2SP
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
    WQ_SNAP_POP        = 0,
    WQ_SNAP_STEAL      = 1,
    WQ_SNAP_GOT_STOLEN = 2,
};

struct WQSnapMeta {
    int32_t level;
    int32_t event;       // WQSnapEvent
    int32_t actor_core;  // core that popped / stole
    int32_t victim_core; // core stolen from (-1 if N/A)
};

__dram__ WQSnapMeta  g_wq_snap_meta[MAX_WQ_SNAPS];
__dram__ int32_t     g_wq_snap_depths[MAX_WQ_SNAPS][MAX_CORES];
__dram__ uint32_t    g_wq_snap_stack_bytes[MAX_WQ_SNAPS][MAX_HARTS]; // per-hart stack usage at each snap
__l2sp__ volatile int32_t g_wq_snap_count = 0;

// ---- Per-hart L2SP access counters (written at end of each level, not in hot loop) ----
__dram__ int64_t stat_l2sp_wq_accesses[MAX_HARTS];      // queue/control metadata in L2SP
__dram__ int64_t stat_l2sp_frontier_accesses[MAX_HARTS]; // frontier data in L2SP
__dram__ int64_t stat_l2sp_graph_accesses[MAX_HARTS];    // visited/dist in L2SP

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
    (void)phase; (void)level;  // disabled for simulation speed
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
                                  int32_t actor_core, int32_t victim_core, int tid) {
    (void)level; (void)event; (void)actor_core; (void)victim_core; (void)tid;
    // disabled for simulation speed
}

static void record_l1sp_trace_sample(int tid) {
    (void)tid;  // disabled for simulation speed
}

static void dump_wq_trace() {
    // disabled for simulation speed
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
// Uses L1SP visited-bitmap to skip redundant L2SP atomics on already-visited vertices.
static inline void process_single_node(int64_t u, int32_t level,
                                        int64_t &l2sp_wq, int64_t &l2sp_frontier,
                                        int64_t &l2sp_graph,
                                        volatile int64_t* bitmap, int my_core,
                                        int64_t &filter_hits, int64_t &filter_misses) {
    const int32_t row_start = g_row_ptr[u];
    const int32_t row_end = g_row_ptr[u + 1];

    for (int32_t ei = row_start; ei < row_end; ei++) {
        const int64_t v = g_col_idx[ei];

        // L1SP bitmap check (single-cycle read)
        if (bitmap_test(bitmap, v)) {
            filter_hits++;
            continue;  // SKIP: already visited — no L2SP atomic needed
        }

        // Not in bitmap — must do the real L2SP atomic
        filter_misses++;
        if (g_visited_in_l2sp) l2sp_graph++;  // amoswap on visited[v]
        if (claim_node(v)) {
            // Successfully claimed: mark in bitmap and process
            bitmap_set(bitmap, v);
            if (g_dist_in_l2sp) l2sp_graph++;  // dist_arr[v] write
            dist_arr[v] = level + 1;
            if (!next_frontier_push_atomic(v, l2sp_wq, l2sp_frontier)) {
                std::printf("ERROR: next frontier overflow at level %d (capacity=%ld)\n",
                            level, (long)g_next_frontier.capacity);
                std::abort();
            }
            l2sp_wq++;  // atomic on discovered (in L2SP)
            atomic_fetch_add_i64(&discovered, 1);
        } else {
            // Claim failed: someone else visited it — mark in bitmap too
            bitmap_set(bitmap, v);
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
    const int STEAL_START = 1;      // wait for N local misses before starting steal episodes
    const int STEAL_VICTIMS = 2;    // probe this many victims per episode
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

    // Local L2SP access counters — accumulated into per-hart globals at end
    int64_t local_l2sp_wq = 0;       // queue/control metadata in L2SP
    int64_t local_l2sp_frontier = 0;  // frontier data in L2SP
    int64_t local_l2sp_graph = 0;     // visited/dist in L2SP

    int empty_streak = 0;

    // Local buffer for stolen items — avoid re-touching victim's cache lines
    int64_t stolen_buf[STEAL_K];

    int64_t local_backoff = 4;
    const int64_t local_backoff_max = 128;

    // Get pointer to this core's L1SP visited bitmap (shared by all harts on core)
    volatile int64_t* bitmap = core_bitmap_ptr();

    // Local filter stats — accumulated into per-core globals at end
    int64_t local_filter_hits = 0;
    int64_t local_filter_misses = 0;

    while (g_level_remaining.load(std::memory_order_acquire) > 0) {
        local_l2sp_wq++;  // g_level_remaining load (in L2SP)

        // Batch pop: grab up to BFS_CHUNK_SIZE nodes at once
        int64_t begin_idx;
        local_l2sp_wq++;  // CAS on queue head/tail (in L2SP)
        int64_t count = queue_pop_chunk(my_queue, my_core, &begin_idx, BFS_CHUNK_SIZE);

        if (count > 0) {
            for (int64_t i = 0; i < count; i++) {
                int64_t abs_idx = my_queue->start_idx + begin_idx + i;
                if (abs_idx < g_next_frontier.l2sp_capacity) local_l2sp_frontier++;
                int64_t u = frontier_current_get(abs_idx);
                local_processed++;
                process_single_node(u, level, local_l2sp_wq, local_l2sp_frontier, local_l2sp_graph, bitmap, my_core, local_filter_hits, local_filter_misses);
                local_l2sp_wq++;  // fetch_sub on g_level_remaining
                g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
            }

            // Periodic snapshot of all queue depths
            if (++local_snap_counter % SNAP_EVERY == 0) {
                record_wq_snap(level, WQ_SNAP_POP, my_core, -1, tid);
            }

            empty_streak = 0;
            steal_backoff = 4;
            local_backoff = 4;
        } else {
            // Pop returned 0 — could be CAS contention, not truly empty.
            // Re-check: if local queue still has items, retry immediately.
            {
                local_l2sp_wq++;  // head/tail re-check
                int64_t h = atomic_load_i64(&my_queue->head);
                int64_t t = atomic_load_i64(&my_queue->tail);
                if (h < t) {
                    continue;  // CAS contention, queue still has work
                }
            }

            // Queue is truly empty.
            local_l2sp_wq++;  // core_has_work write
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
            for (int k = 0; k < STEAL_VICTIMS; k++) {
                // Pick a random victim, skip self, empty hints, and recently-failed
                int victim;
                int pick_tries = 0;
                do {
                    rng_state = xorshift_victim(rng_state);
                    victim = (int)(rng_state % (uint32_t)total_cores);
                    bool in_recent = false;
                    if (victim != my_core && core_has_work[victim] != 0) {
                        local_l2sp_wq++;  // core_has_work[victim] read
                        for (int ri = 0; ri < RECENTLY_TRIED_SIZE; ri++) {
                            if (recently_tried[ri] == victim) { in_recent = true; break; }
                        }
                        if (!in_recent) break;
                    }
                    pick_tries++;
                } while (pick_tries < total_cores);
                if (victim == my_core || core_has_work[victim] == 0) continue;

                local_steal_attempts++;
                local_l2sp_wq++;  // CAS on victim queue head/tail
                count = queue_pop_chunk(&core_queues[victim], victim, &begin_idx, STEAL_K);
                if (count > 0) {
                    local_steal_success++;
                    found = true;

                    // Copy stolen items to local buffer, then release victim's cache lines
                    for (int64_t i = 0; i < count; i++) {
                        int64_t abs_idx = core_queues[victim].start_idx + begin_idx + i;
                        if (abs_idx < g_next_frontier.l2sp_capacity) local_l2sp_frontier++;
                        stolen_buf[i] = frontier_current_get(abs_idx);
                    }
                    // Thief processes ALL stolen items itself — no pushing
                    // back into shared queue where siblings would compete.
                    for (int64_t i = 0; i < count; i++) {
                        local_processed++;
                        process_single_node(stolen_buf[i], level,
                                            local_l2sp_wq, local_l2sp_frontier, local_l2sp_graph, bitmap, my_core, local_filter_hits, local_filter_misses);
                        local_l2sp_wq++;  // fetch_sub on g_level_remaining
                        g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
                    }

                    // Snapshot on every successful steal (thief + victim perspectives)
                    record_wq_snap(level, WQ_SNAP_STEAL, my_core, victim, tid);
                    record_wq_snap(level, WQ_SNAP_GOT_STOLEN, victim, my_core, tid);

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
    stat_l2sp_wq_accesses[tid] += local_l2sp_wq;
    stat_l2sp_frontier_accesses[tid] += local_l2sp_frontier;
    stat_l2sp_graph_accesses[tid] += local_l2sp_graph;

    // Accumulate filter stats into per-core globals (non-atomic, single write per level per hart)
    atomic_fetch_add_i64((volatile int64_t*)&stat_filter_hits[my_core], local_filter_hits);
    atomic_fetch_add_i64((volatile int64_t*)&stat_filter_misses[my_core], local_filter_misses);
    atomic_fetch_add_i64((volatile int64_t*)&stat_filter_checks[my_core], local_filter_hits + local_filter_misses);
}

// -------------------- Level Advance (round-robin balanced) --------------------
static void advance_to_next_level_locality_preserving(int tid) {
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

    // Round-robin: each core gets floor(total_nodes / total_cores) nodes;
    // the first (total_nodes % total_cores) cores get one extra.
    int64_t per_core = total_nodes / total_cores;
    int64_t remainder = total_nodes % total_cores;

    int64_t base = 0;
    for (int c = 0; c < total_cores; c++) {
        int64_t count = per_core + (c < remainder ? 1 : 0);
        queue_assign_slice(&core_queues[c], base, count);
        core_has_work[c] = (count > 0) ? 1 : 0;
        base += count;
    }

    // Copy next-frontier items into current-frontier in order;
    // the queue slices already map each position to a core round-robin.
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

        for (int i = 0; i < g_total_harts; i++) {
            g_local_sense[i] = 0;
            stat_nodes_processed[i] = 0;
            stat_steal_attempts[i] = 0;
            stat_steal_success[i] = 0;
            stat_l2sp_wq_accesses[i] = 0;
            stat_l2sp_frontier_accesses[i] = 0;
            stat_l2sp_graph_accesses[i] = 0;
        }

        for (int c = 0; c < g_total_cores; c++) {
            queue_init(&core_queues[c]);
            core_has_work[c] = 0;
            stat_filter_checks[c] = 0;
            stat_filter_hits[c] = 0;
            stat_filter_misses[c] = 0;
        }
        stat_edges_traversed = 0;
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

        std::printf("=== BFS with L1SP Visited-Bitmap Filter + Work Stealing ===\n");
        std::printf("Graph: N=%d E=%d (RMAT CSR from rmat.bin)\n",
                    g_num_vertices, g_num_edges);
        std::printf("Graph storage: row_ptr/col_idx in DRAM (malloc)\n");
        std::printf("L1SP bitmap: %d bytes per core (%d vertices)\n",
                    (g_num_vertices + 7) / 8, g_num_vertices);
        std::printf("Hot state: visited in %s, dist_arr in %s\n",
                    g_visited_in_l2sp ? "L2SP" : "DRAM",
                    g_dist_in_l2sp ? "L2SP" : "DRAM");
        const char* frontier_storage =
            (g_next_frontier.l2sp_capacity == 0) ? "DRAM" :
            (g_next_frontier.l2sp_capacity == g_next_frontier.capacity) ? "L2SP" :
            "L2SP+DRAM";
        std::printf("Frontiers: dynamic %s buffers preserving producer-core ownership "
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
        std::printf("\n");
        record_wq_trace(WQ_PHASE_INIT, 0);

        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }

    // Each core's first hart initializes its L1SP thief token and visited bitmap.
    // All harts on the core share the same L1SP, so only one write needed.
    if (myThreadId() == 0) {
        thief_token_init();
        bitmap_init(g_num_vertices);
        // Mark the source vertex in this core's bitmap
        volatile int64_t* bm = core_bitmap_ptr();
        bitmap_set(bm, g_bfs_source);
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
        advance_to_next_level_locality_preserving(tid);
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
        std::printf("Nodes discovered: %ld / %d\n", (long)discovered, g_num_vertices);

        // ---- L1SP Visited-Bitmap Filter Statistics ----
        int64_t total_checks = 0, total_hits = 0, total_misses = 0;
        std::printf("\n=== L1SP Visited-Bitmap Filter Statistics ===\n");
        std::printf("Core | Checks | Bitmap Hits | L2SP Atomics | Hit Rate\n");
        std::printf("-----|--------|-------------|--------------|----------\n");
        for (int c = 0; c < g_total_cores; c++) {
            int64_t checks = stat_filter_checks[c];
            int64_t hits = stat_filter_hits[c];
            int64_t misses = stat_filter_misses[c];
            int64_t pct = (checks > 0) ? (hits * 100 / checks) : 0;
            std::printf(" %3d | %6ld | %11ld | %12ld |    %ld%%\n",
                        c, (long)checks, (long)hits, (long)misses, (long)pct);
            total_checks += checks;
            total_hits += hits;
            total_misses += misses;
        }
        const int32_t bitmap_bytes = (g_num_vertices + 7) / 8;
        const int64_t total_hitrate = (total_checks > 0) ? (total_hits * 100 / total_checks) : 0;
        const int64_t l2sp_atomics_saved_pct = (total_checks > 0) ? (total_hits * 100 / total_checks) : 0;
        std::printf("\nFilter summary:\n");
        std::printf("  Total edge checks:     %ld\n", (long)total_checks);
        std::printf("  Bitmap hits (skipped): %ld (%ld%%)\n", (long)total_hits, (long)total_hitrate);
        std::printf("  L2SP atomics needed:   %ld (%ld%%)\n", (long)total_misses,
                    (long)(total_checks > 0 ? total_misses * 100 / total_checks : 0));
        std::printf("  L2SP atomics saved:    %ld (%ld%%)\n", (long)total_hits, (long)l2sp_atomics_saved_pct);
        std::printf("  Bitmap size per core:  %d bytes (%.2f%% of L1SP)\n",
                    bitmap_bytes, (double)bitmap_bytes * 100.0 / (double)g_core_l1sp_bytes);
        std::printf("  Total edges traversed: %ld\n", (long)total_checks);

        // Machine-readable line for sweep scripts
        std::printf("FILTER_STATS: checks=%ld,hits=%ld,misses=%ld,hitrate=%ld,bitmap_bytes=%d,l1sp_pct=%.4f\n",
                    (long)total_checks, (long)total_hits, (long)total_misses,
                    (long)total_hitrate, bitmap_bytes,
                    (double)bitmap_bytes * 100.0 / (double)g_core_l1sp_bytes);

        // ---- Per-core L2SP access statistics ----
        std::printf("\nPer-core L2SP access statistics:\n");
        std::printf("Core | WQ Metadata | Frontier Data | Graph State |    Total | Accesses/Cycle\n");
        std::printf("-----|-------------|---------------|-------------|----------|---------------\n");

        int64_t grand_l2sp_total = 0;
        for (int c = 0; c < g_total_cores; c++) {
            int64_t core_wq = 0, core_frontier = 0, core_graph = 0;
            for (int t = 0; t < g_harts_per_core; t++) {
                int h = c * g_harts_per_core + t;
                core_wq       += stat_l2sp_wq_accesses[h];
                core_frontier += stat_l2sp_frontier_accesses[h];
                core_graph    += stat_l2sp_graph_accesses[h];
            }
            int64_t core_total = core_wq + core_frontier + core_graph;
            grand_l2sp_total += core_total;
            // Integer fixed-point: accesses/cycle * 10000
            int64_t per_cycle_x10000 = (elapsed > 0) ? (core_total * 10000) / (int64_t)elapsed : 0;
            std::printf("%4d | %11ld | %13ld | %11ld | %8ld |    %ld.%04ld\n",
                        c, (long)core_wq, (long)core_frontier, (long)core_graph,
                        (long)core_total, (long)(per_cycle_x10000 / 10000), (long)(per_cycle_x10000 % 10000));
        }
        std::printf("  Grand total L2SP accesses: %ld\n", (long)grand_l2sp_total);
        if (elapsed > 0) {
            int64_t agg_x10000 = (grand_l2sp_total * 10000) / (int64_t)elapsed;
            std::printf("  Aggregate L2SP accesses/cycle: %ld.%04ld\n",
                        (long)(agg_x10000 / 10000), (long)(agg_x10000 % 10000));
        }

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
