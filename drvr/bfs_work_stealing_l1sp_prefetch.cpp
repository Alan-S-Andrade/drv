// Level-synchronous BFS with per-core work stealing + L1SP graph prefetching
//
// Key idea: The roofline shows BFS is DRAM-latency-bound. Per-core L1SP is
// only ~50% used by hart stacks.  We dedicate hart-0 on each core as a
// "prefetch hart" that reads CSR row_ptr / col_idx from DRAM into a shared
// L1SP prefetch ring buffer.  Compute harts (1..N-1) consume prefetched
// entries from L1SP (single-cycle access) instead of going to DRAM.
//
// The prefetch ring buffer lives in the unused L1SP space below the work
// cache and above the tokens.  Each entry holds:
//   - vertex id, row_start, row_end
//   - up to PREFETCH_MAX_EDGES_PER_NODE col_idx values
//
// Double-buffering: prefetch hart writes into ring slots, sets per-slot
// ready flag; compute harts spin-wait on ready flag then read locally.

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

static constexpr const char* BENCH_TITLE = "BFS with L1SP Graph Prefetch + Work Stealing";
static constexpr const char* BENCH_LOG_PREFIX = "run_l1sp_prefetch";

// -------------------- Tuning Constants --------------------
static constexpr int MAX_HARTS  = 1024;
static constexpr int MAX_CORES  = 64;
static constexpr int MAX_HARTS_PER_CORE = 16;
static constexpr int BFS_CHUNK_SIZE = 64;
static constexpr int STEAL_K_MIN = 64;
static constexpr int STEAL_K_MAX = 256;
static constexpr int STEAL_VICTIMS_MIN = 2;
static constexpr int STEAL_VICTIMS_MAX = 8;
static constexpr int INITIAL_SKEW_WEIGHT = 20;

// Prefetch parameters
static constexpr int PREFETCH_MAX_EDGES_PER_NODE = 48;  // max edges cached per node
static constexpr int PREFETCH_RING_GUARD_BYTES = 2048;   // guard between ring and stacks

// -------------------- Global State --------------------
__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

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

__l2sp__ volatile int32_t core_has_work[MAX_CORES];
__l2sp__ volatile int32_t core_steal_batch_size[MAX_CORES];
__l2sp__ volatile int32_t core_steal_victim_probes[MAX_CORES];
__l2sp__ volatile int32_t core_last_steal_feedback[MAX_CORES];
__l2sp__ volatile uint64_t g_core_l1sp_bytes = 0;
__l2sp__ volatile uint64_t g_hart_stack_capacity_bytes = 0;
__l2sp__ volatile uint64_t g_hart_stack_live_bytes[MAX_HARTS];
__l2sp__ volatile uint64_t g_hart_stack_peak_bytes[MAX_HARTS];

// Per-hart and per-core stats
__l2sp__ volatile int64_t stat_nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_attempts[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_success[MAX_HARTS];
__l2sp__ volatile int64_t discovered = 0;
__l2sp__ volatile int64_t stat_steal_items[MAX_HARTS];
__l2sp__ volatile int64_t stat_idle_wait_cycles[MAX_HARTS];
__l2sp__ volatile int64_t stat_edges_traversed = 0;

// Prefetch stats
__l2sp__ volatile int64_t stat_prefetch_nodes[MAX_CORES];     // nodes prefetched
__l2sp__ volatile int64_t stat_prefetch_edges[MAX_CORES];     // edges prefetched
__l2sp__ volatile int64_t stat_prefetch_hits[MAX_HARTS];      // L1SP cache hits
__l2sp__ volatile int64_t stat_prefetch_misses[MAX_HARTS];    // DRAM fallback
__l2sp__ volatile int64_t stat_prefetch_cycles[MAX_CORES];    // cycles spent prefetching
__l2sp__ volatile int64_t stat_prefetch_stall_cycles[MAX_HARTS]; // cycles compute waits for prefetch
__l2sp__ volatile int64_t stat_prefetch_edge_overflow[MAX_CORES]; // nodes with > MAX edges

// Per-core steal token in L1SP
static constexpr uintptr_t THIEF_TOKEN_L1SP_OFFSET = 0;
static constexpr uintptr_t REFILL_TOKEN_L1SP_OFFSET = 8;

// -------------------- Prefetch Ring Buffer --------------------
// Ring METADATA lives in L2SP (per-core arrays) because L2SP atomics
// are reliable across harts.  Ring DATA lives in L1SP for fast access
// by compute harts on the same core.
//
// L1SP layout per core:
//   [0x00]  thief_token       (8 bytes)
//   [0x08]  refill_token      (8 bytes)
//   [0x40]  PrefetchEntry[ring_capacity]  (ring data)
//   [...]   guard
//   [...]   hart stacks (from top of L1SP, growing down)

struct PrefetchEntry {
    volatile int32_t ready;     // 0=empty, 1=prefetched
    int32_t vertex;             // vertex id
    int32_t row_start;          // g_row_ptr[vertex]
    int32_t row_end;            // g_row_ptr[vertex+1]
    int32_t edge_count;         // min(row_end - row_start, PREFETCH_MAX_EDGES_PER_NODE)
    int32_t pad;
    int32_t edges[PREFETCH_MAX_EDGES_PER_NODE]; // col_idx values
};

// Ring metadata in L2SP — one per core, indexed by core_id
__l2sp__ volatile int64_t pf_write_idx[MAX_CORES];
__l2sp__ volatile int64_t pf_read_idx[MAX_CORES];
__l2sp__ volatile int64_t pf_ring_capacity[MAX_CORES];
__l2sp__ volatile int64_t pf_done[MAX_CORES];

static constexpr uintptr_t PREFETCH_ENTRIES_OFFSET = 64; // 0x40 (moved up since no header in L1SP)

static inline PrefetchEntry* prefetch_entries_ptr() {
    uintptr_t l1sp_base;
    asm volatile("csrr %0, " __stringify(MCSR_L1SPBASE) : "=r"(l1sp_base));
    return (PrefetchEntry *)(l1sp_base + PREFETCH_ENTRIES_OFFSET);
}

static inline int64_t compute_ring_capacity() {
    const uint64_t l1sp_size = g_core_l1sp_bytes;
    const int hpc = g_harts_per_core;
    // The SST model divides L1SP equally among harts for stacks:
    //   Hart i stack region = [l1sp_base + i*slot, l1sp_base + (i+1)*slot)
    //   where slot = l1sp_size / harts_per_core
    // Hart 0 is repurposed as prefetch hart.  Its stack pointer starts at
    // the TOP of its region and grows down.  We place the ring buffer at
    // the BOTTOM of Hart 0's region, with a guard to prevent collision.
    const uint64_t hart0_region = l1sp_size / (uint64_t)hpc;
    // Reserve space for Hart 0's actual stack (grows from top down) + guard
    const uint64_t HART0_STACK_RESERVE = 4096;
    const uint64_t reserved = HART0_STACK_RESERVE + PREFETCH_RING_GUARD_BYTES;
    if (hart0_region <= PREFETCH_ENTRIES_OFFSET + reserved) return 4;
    const uint64_t available = hart0_region - PREFETCH_ENTRIES_OFFSET - reserved;
    int64_t cap = (int64_t)(available / sizeof(PrefetchEntry));
    if (cap < 4) cap = 4;
    if (cap > 256) cap = 256;
    return cap;
}

static inline void prefetch_ring_init() {
    int core = myCoreId();
    int64_t cap = compute_ring_capacity();
    pf_write_idx[core] = 0;
    pf_read_idx[core] = 0;
    pf_ring_capacity[core] = cap;
    pf_done[core] = 0;

    PrefetchEntry* entries = prefetch_entries_ptr();
    for (int64_t i = 0; i < cap; i++) {
        entries[i].ready = 0;
        entries[i].vertex = -1;
        entries[i].edge_count = 0;
    }
}

// -------------------- Barrier --------------------
__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;

static inline int get_thread_id() {
    return (myCoreId() << 4) + myThreadId();
}

static inline void spin_pause(int64_t iters) {
    for (int64_t i = 0; i < iters; i++) {
        asm volatile("" ::: "memory");
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

// -------------------- Work Queue Ops --------------------
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

// -------------------- Steal Policy --------------------
static inline uint32_t xorshift_victim(uint32_t seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static inline void adapt_steal_policy(int my_core, int64_t victim_depth_before, int64_t stolen_count) {
    int32_t batch = core_steal_batch_size[my_core];
    int32_t probes = core_steal_victim_probes[my_core];
    int32_t feedback = 0;

    if (victim_depth_before >= 4 * stolen_count && stolen_count == batch) {
        feedback = 1;
    } else if (victim_depth_before <= stolen_count + (stolen_count >> 1)) {
        feedback = -1;
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

// Thief token
static inline volatile int64_t* thief_token_ptr() {
    uintptr_t l1sp_base;
    asm volatile("csrr %0, " __stringify(MCSR_L1SPBASE) : "=r"(l1sp_base));
    return (volatile int64_t *)(l1sp_base + THIEF_TOKEN_L1SP_OFFSET);
}

static inline bool thief_token_try_acquire() {
    volatile int64_t *tok = thief_token_ptr();
    return atomic_compare_and_swap_i64(tok, 0, 1) == 0;
}

static inline void thief_token_release() {
    volatile int64_t *tok = thief_token_ptr();
    *tok = 0;
}

static inline void thief_token_init() {
    volatile int64_t *tok = thief_token_ptr();
    *tok = 0;
    volatile int64_t *rtok = (volatile int64_t *)((uintptr_t)tok + 8); // refill token
    *rtok = 0;
}

// Global remaining-work counter
__l2sp__ volatile int64_t g_level_remaining = 0;

static inline void wait_check_local(int64_t backoff, int my_core) {
    if (core_has_work[my_core]) {
        return;
    } else if (atomic_load_i64(&g_level_remaining) > 0) {
        spin_pause(backoff);
    } else {
        hartsleep(backoff);
    }
}

// -------------------- BFS Graph State --------------------
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

// -------------------- Graph Loading --------------------
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
        std::printf("ERROR: bulk load header failed (result=%ld)\n", header_desc.result);
        return false;
    }

    int hdr_N      = header[0];
    int hdr_E      = header[1];
    int hdr_D      = header[2];
    int hdr_source = header[4];

    if (hdr_N <= 0 || hdr_E < 0) {
        std::printf("ERROR: invalid CSR header: N=%d E=%d\n", hdr_N, hdr_E);
        return false;
    }

    const size_t file_size = 20
        + (size_t)(hdr_N + 1) * sizeof(int32_t)
        + (size_t)hdr_E * sizeof(int32_t)
        + (size_t)hdr_N * sizeof(int32_t);

    char *buf = (char *)std::malloc(file_size);
    if (!buf) {
        std::printf("ERROR: malloc failed (%lu bytes)\n", (unsigned long)file_size);
        return false;
    }

    struct ph_bulk_load_desc desc;
    desc.filename_addr = (long)fname;
    desc.dest_addr     = (long)buf;
    desc.size          = (long)file_size;
    desc.result        = 0;
    ph_bulk_load_file(&desc);

    if (desc.result <= 0) {
        std::printf("ERROR: bulk load failed (result=%ld)\n", desc.result);
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

    // Allocate visited/dist in L2SP where possible
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
        return false;
    }

    // Frontier buffers
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
        std::printf("ERROR: allocation failed for frontier buffers\n");
        std::free(buf);
        if (!g_visited_in_l2sp) std::free((void *)visited);
        if (!g_dist_in_l2sp) std::free(dist_arr);
        std::free(g_current_frontier_dram_storage);
        std::free(g_next_frontier.dram_items);
        std::free(g_next_frontier.dram_owner_core);
        return false;
    }

    std::printf("Graph loaded: N=%d E=%d source=%d max_deg=%d (%lu bytes)\n",
                hdr_N, hdr_E, g_bfs_source, max_deg, (unsigned long)file_size);
    return true;
}

// -------------------- Self-Prefetch Logic --------------------
// Each hart uses its own L1SP stack region to batch-read col_idx from DRAM
// before processing edges.  This reduces DRAM latency by turning scattered
// reads into sequential reads that benefit from DRAM row buffer hits.
//
// Per-hart L1SP layout:
//   [hart_base + 0]              thief token / refill token (hart 0 only, first 16B)
//   [hart_base + PREFETCH_BUF_OFFSET]  prefetch buffer (col_idx batch)
//   [hart_top - STACK_RESERVE]   stack pointer (grows down)
//
// Each hart's prefetch buffer can hold up to PREFETCH_BUF_ENTRIES int32_t values.

static constexpr int PREFETCH_BUF_OFFSET_H0 = 64;  // hart 0 has tokens at 0-15
static constexpr int PREFETCH_BUF_OFFSET_HN = 0;    // other harts start at base
static constexpr int STACK_RESERVE = 4096;           // stack space (grows from top down)
static constexpr int PREFETCH_GUARD = 256;           // guard between buf and stack

static inline int32_t* my_prefetch_buf(int hart_local_id) {
    uintptr_t l1sp_base;
    asm volatile("csrr %0, " __stringify(MCSR_L1SPBASE) : "=r"(l1sp_base));
    const uint64_t slot = g_core_l1sp_bytes / (uint64_t)g_harts_per_core;
    uintptr_t my_base = l1sp_base + (uint64_t)hart_local_id * slot;
    int offset = (hart_local_id == 0) ? PREFETCH_BUF_OFFSET_H0 : PREFETCH_BUF_OFFSET_HN;
    return (int32_t*)(my_base + offset);
}

static inline int32_t my_prefetch_buf_capacity(int hart_local_id) {
    const uint64_t slot = g_core_l1sp_bytes / (uint64_t)g_harts_per_core;
    int offset = (hart_local_id == 0) ? PREFETCH_BUF_OFFSET_H0 : PREFETCH_BUF_OFFSET_HN;
    uint64_t available = slot - offset - STACK_RESERVE - PREFETCH_GUARD;
    return (int32_t)(available / sizeof(int32_t));
}

// Process a node using L1SP prefetch buffer for col_idx edges
static inline void process_node_with_prefetch(int64_t u, int32_t level,
                                               int32_t* pf_buf, int32_t pf_capacity,
                                               int tid) {
    const int32_t row_start = g_row_ptr[u];
    const int32_t row_end = g_row_ptr[u + 1];
    const int32_t degree = row_end - row_start;
    atomic_fetch_add_i64((volatile int64_t *)&stat_edges_traversed, (int64_t)degree);

    // Batch-read col_idx into L1SP prefetch buffer
    int32_t batch_size = (degree < pf_capacity) ? degree : pf_capacity;
    for (int32_t i = 0; i < batch_size; i++) {
        pf_buf[i] = g_col_idx[row_start + i];  // sequential DRAM reads → L1SP
    }

    // Process from L1SP (fast single-cycle reads)
    for (int32_t i = 0; i < batch_size; i++) {
        const int64_t v = pf_buf[i];  // L1SP read!
        if (claim_node(v)) {
            dist_arr[v] = level + 1;
            if (!next_frontier_push_atomic(v)) {
                std::printf("ERROR: frontier overflow\n");
                std::abort();
            }
            atomic_fetch_add_i64(&discovered, 1);
        }
    }

    // Handle overflow edges directly from DRAM (if degree > buffer capacity)
    for (int32_t ei = row_start + batch_size; ei < row_end; ei++) {
        const int64_t v = g_col_idx[ei];
        if (claim_node(v)) {
            dist_arr[v] = level + 1;
            if (!next_frontier_push_atomic(v)) {
                std::printf("ERROR: frontier overflow\n");
                std::abort();
            }
            atomic_fetch_add_i64(&discovered, 1);
        }
    }

    stat_prefetch_hits[tid] += batch_size;
    stat_prefetch_misses[tid] += (degree > batch_size) ? (degree - batch_size) : 0;
    if (degree > pf_capacity) {
        stat_prefetch_edge_overflow[myCoreId()]++;
    }
}

// Direct DRAM processing (used for stolen nodes / when no prefetch available)
static inline void process_single_node(int64_t u, int32_t level) {
    const int32_t row_start = g_row_ptr[u];
    const int32_t row_end = g_row_ptr[u + 1];
    atomic_fetch_add_i64((volatile int64_t *)&stat_edges_traversed, (int64_t)(row_end - row_start));
    for (int32_t ei = row_start; ei < row_end; ei++) {
        const int64_t v = g_col_idx[ei];
        if (claim_node(v)) {
            dist_arr[v] = level + 1;
            if (!next_frontier_push_atomic(v)) {
                std::printf("ERROR: next frontier overflow\n");
                std::abort();
            }
            atomic_fetch_add_i64(&discovered, 1);
        }
    }
}

// -------------------- BFS Level Processing --------------------
static void process_bfs_level(int tid, int32_t level) {
    const int harts_per_core = g_harts_per_core;
    const int total_cores = g_total_cores;
    const int my_core = myCoreId();
    const int my_local_id = myThreadId();

    WorkQueue* my_queue = &core_queues[my_core];

    // Set up per-hart L1SP prefetch buffer
    int32_t* pf_buf = my_prefetch_buf(my_local_id);
    int32_t pf_capacity = my_prefetch_buf_capacity(my_local_id);


    // Compute harts (1..N-1): consume from prefetch ring, then steal
    const int32_t steal_batch = core_steal_batch_size[my_core];
    const int32_t steal_victims = core_steal_victim_probes[my_core];
    int64_t steal_backoff = 4;
    const int64_t steal_backoff_max = 512;
    int64_t local_backoff = 4;
    const int64_t local_backoff_max = 128;

    uint32_t rng_state = (uint32_t)(tid + 1) * 2654435761u;
    static constexpr int RECENTLY_TRIED_SIZE = 4;
    int recently_tried[RECENTLY_TRIED_SIZE];
    for (int i = 0; i < RECENTLY_TRIED_SIZE; i++) recently_tried[i] = -1;
    int rt_idx = 0;

    int64_t local_processed = 0;
    int64_t local_steal_attempts = 0;
    int64_t local_steal_success = 0;
    int64_t local_steal_items = 0;
    int64_t local_idle_wait_cycles = 0;

    int64_t stolen_buf[STEAL_K_MAX];
    int empty_streak = 0;
    while (atomic_load_i64(&g_level_remaining) > 0) {
        // Phase 1: Pop from local queue and process with L1SP prefetch
        bool processed_local = false;
        {
            int64_t begin_idx = 0;
            int64_t count = queue_pop_chunk(my_queue, my_core, &begin_idx, BFS_CHUNK_SIZE);
            if (count > 0) {
                for (int64_t i = 0; i < count; i++) {
                    int64_t u = frontier_current_get(my_queue->start_idx + begin_idx + i);
                    process_node_with_prefetch(u, level, pf_buf, pf_capacity, tid);
                    local_processed++;
                    atomic_fetch_add_i64(&g_level_remaining, -1);
                }
                processed_local = true;
                empty_streak = 0;
                local_backoff = 4;
                steal_backoff = 4;
            }
        }

        if (processed_local) continue;

        // Phase 3: Work stealing from other cores
        empty_streak++;
        if (empty_streak < 2) {
            uint64_t wait_start = 0, wait_end = 0;
            asm volatile("rdcycle %0" : "=r"(wait_start));
            wait_check_local(local_backoff, my_core);
            asm volatile("rdcycle %0" : "=r"(wait_end));
            local_idle_wait_cycles += (int64_t)(wait_end - wait_start);
            if (local_backoff < local_backoff_max) local_backoff <<= 1;
            continue;
        }

        if (!thief_token_try_acquire()) {
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
            int64_t count = queue_pop_chunk(&core_queues[victim], victim, &begin_idx, steal_batch);
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
                    process_node_with_prefetch(stolen_buf[i], level, pf_buf, pf_capacity, tid);
                    atomic_fetch_add_i64(&g_level_remaining, -1);
                }
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
        }
    }

    stat_nodes_processed[tid] += local_processed;
    stat_steal_attempts[tid] += local_steal_attempts;
    stat_steal_success[tid] += local_steal_success;
    stat_steal_items[tid] += local_steal_items;
    stat_idle_wait_cycles[tid] += local_idle_wait_cycles;
}

// -------------------- Level Advance --------------------
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

        // Init per-hart arrays using actual sparse TIDs: (core << 4) + thread
        for (int c = 0; c < g_total_cores; c++) {
            for (int h = 0; h < g_harts_per_core; h++) {
                int actual_tid = (c << 4) + h;
                g_local_sense[actual_tid] = 0;
                stat_nodes_processed[actual_tid] = 0;
                stat_steal_attempts[actual_tid] = 0;
                stat_steal_success[actual_tid] = 0;
                stat_steal_items[actual_tid] = 0;
                stat_idle_wait_cycles[actual_tid] = 0;
                stat_prefetch_hits[actual_tid] = 0;
                stat_prefetch_misses[actual_tid] = 0;
                stat_prefetch_stall_cycles[actual_tid] = 0;
                g_hart_stack_live_bytes[actual_tid] = 0;
                g_hart_stack_peak_bytes[actual_tid] = 0;
            }
        }

        for (int c = 0; c < g_total_cores; c++) {
            queue_init(&core_queues[c]);
            core_has_work[c] = 0;
            core_steal_batch_size[c] = STEAL_K_MIN;
            core_steal_victim_probes[c] = STEAL_VICTIMS_MIN;
            core_last_steal_feedback[c] = 0;
            stat_prefetch_nodes[c] = 0;
            stat_prefetch_edges[c] = 0;
            stat_prefetch_cycles[c] = 0;
            stat_prefetch_edge_overflow[c] = 0;
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
        std::printf("Graph: N=%d E=%d (RMAT CSR from rmat.bin)\n",
                    g_num_vertices, g_num_edges);
        std::printf("Hot state: visited in %s, dist_arr in %s\n",
                    g_visited_in_l2sp ? "L2SP" : "DRAM",
                    g_dist_in_l2sp ? "L2SP" : "DRAM");
        std::printf("Hardware: %d cores x %d harts = %d total harts\n",
                    g_total_cores, g_harts_per_core, g_total_harts);
        std::printf("Source: node %ld (highest-degree)\n", (long)source_id);
        std::printf("L1SP: per-core=%lu bytes, global=%lu bytes\n",
                    (unsigned long)g_core_l1sp_bytes,
                    (unsigned long)(g_core_l1sp_bytes * (uint64_t)g_total_cores));
        std::printf("L1SP stack slot: per-hart=%lu bytes\n",
                    (unsigned long)g_hart_stack_capacity_bytes);
        std::printf("Prefetch: per-hart self-prefetch of col_idx into L1SP\n");
        std::printf("Stealing: adaptive batch/probe sizing\n\n");

        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }

    // Each core's hart-0 initializes tokens
    if (myThreadId() == 0) {
        thief_token_init();
    }

    barrier();

    // Print prefetch buffer capacity (from hart 0)
    if (tid == 0) {
        int32_t cap0 = my_prefetch_buf_capacity(0);
        int32_t cap1 = my_prefetch_buf_capacity(1);
        std::printf("Self-prefetch buffer: hart0=%d edges, other harts=%d edges\n",
                    cap0, cap1);
    }

    // NOTE: no extra barrier here — go directly to BFS loop

    uint64_t start_cycles = 0;
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(start_cycles));
    }

    int32_t level = 0;

    while (true) {
        int64_t total_work = count_total_work();

        if (tid == 0) {
            atomic_swap_i64(&g_level_remaining, total_work);
        }

        barrier();

        if (total_work == 0) break;

        if (tid == 0) {
            std::printf("Level %d: total_work=%ld, discovered=%ld\n",
                        level, (long)total_work, (long)discovered);
        }

        // No per-level ring reset needed (self-prefetch uses local buffers)

        barrier();
        ph_stat_phase(1);
        process_bfs_level(tid, level);
        ph_stat_phase(0);
        barrier();
        advance_to_next_level_balanced(tid);
        barrier();
        level++;
    }

    uint64_t end_cycles = 0;
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(end_cycles));
    }

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
        int64_t total_steal_items_sum = 0;
        int64_t total_idle_cycles = 0;
        int64_t total_pf_hits = 0;
        int64_t total_pf_misses = 0;
        int64_t total_pf_nodes = 0;
        int64_t total_pf_edges = 0;
        int64_t total_pf_cycles = 0;
        int64_t total_pf_overflows = 0;

        std::printf("\nPer-hart statistics:\n");
        std::printf("Hart | Processed | PF Hits | PF Miss | Steal OK/Attempts | Idle Cycles\n");
        std::printf("-----|-----------|---------|---------|-------------------|------------\n");

        for (int c = 0; c < g_total_cores; c++) {
            for (int ht = 0; ht < g_harts_per_core; ht++) {
                int h = (c << 4) + ht;
                std::printf("%4d | %9ld | %7ld | %7ld | %6ld/%6ld     | %10ld\n",
                            h,
                            (long)stat_nodes_processed[h],
                            (long)stat_prefetch_hits[h],
                            (long)stat_prefetch_misses[h],
                            (long)stat_steal_success[h],
                            (long)stat_steal_attempts[h],
                            (long)stat_idle_wait_cycles[h]);

                total_processed += stat_nodes_processed[h];
                total_attempts += stat_steal_attempts[h];
                total_success += stat_steal_success[h];
                total_steal_items_sum += stat_steal_items[h];
                total_idle_cycles += stat_idle_wait_cycles[h];
                total_pf_hits += stat_prefetch_hits[h];
                total_pf_misses += stat_prefetch_misses[h];
            }
        }

        std::printf("\nPer-core prefetch statistics:\n");
        std::printf("Core | PF Nodes | PF Edges | PF Cycles | Edge Overflow\n");
        std::printf("-----|----------|----------|-----------|-------------\n");
        for (int c = 0; c < g_total_cores; c++) {
            std::printf("%4d | %8ld | %8ld | %9ld | %12ld\n",
                        c,
                        (long)stat_prefetch_nodes[c],
                        (long)stat_prefetch_edges[c],
                        (long)stat_prefetch_cycles[c],
                        (long)stat_prefetch_edge_overflow[c]);
            total_pf_nodes += stat_prefetch_nodes[c];
            total_pf_edges += stat_prefetch_edges[c];
            total_pf_cycles += stat_prefetch_cycles[c];
            total_pf_overflows += stat_prefetch_edge_overflow[c];
        }

        std::printf("\nSummary:\n");
        std::printf("  Total nodes processed:  %ld\n", (long)total_processed);
        std::printf("  Total edges traversed:  %ld\n", (long)stat_edges_traversed);
        std::printf("  Total steal attempts:   %ld\n", (long)total_attempts);
        if (total_attempts > 0) {
            std::printf("  Successful steals:      %ld (%ld%%)\n",
                        (long)total_success,
                        (long)(100 * total_success / total_attempts));
        }
        std::printf("  Total steal items:      %ld\n", (long)total_steal_items_sum);
        std::printf("  Idle wait cycles:       %ld\n", (long)total_idle_cycles);

        std::printf("\nPrefetch Summary:\n");
        std::printf("  Nodes prefetched:       %ld\n", (long)total_pf_nodes);
        std::printf("  Edges prefetched:       %ld\n", (long)total_pf_edges);
        std::printf("  Prefetch cycles:        %ld\n", (long)total_pf_cycles);
        std::printf("  L1SP hits:              %ld\n", (long)total_pf_hits);
        std::printf("  DRAM fallback:          %ld\n", (long)total_pf_misses);
        if (total_pf_hits + total_pf_misses > 0) {
            std::printf("  Hit rate:               %ld%%\n",
                        (long)(100 * total_pf_hits / (total_pf_hits + total_pf_misses)));
        }
        std::printf("  Edge overflows:         %ld\n", (long)total_pf_overflows);

        // L1SP usage summary
        std::printf("\n========== L1SP UTILIZATION ==========\n");
        std::printf("L1SP per-core:        %lu bytes\n", (unsigned long)g_core_l1sp_bytes);
        std::printf("L1SP global:          %lu bytes (%d cores)\n",
                    (unsigned long)(g_core_l1sp_bytes * (uint64_t)g_total_cores),
                    g_total_cores);
        {
            uint64_t slot = g_core_l1sp_bytes / (uint64_t)g_harts_per_core;
            int32_t cap_h0 = my_prefetch_buf_capacity(0);
            int32_t cap_hn = my_prefetch_buf_capacity(1);
            uint64_t buf_bytes_h0 = (uint64_t)cap_h0 * sizeof(int32_t);
            uint64_t buf_bytes_hn = (uint64_t)cap_hn * sizeof(int32_t);
            uint64_t total_pf_bytes = buf_bytes_h0 + buf_bytes_hn * (uint64_t)(g_harts_per_core - 1);
            std::printf("L1SP layout per hart (%lu bytes/hart):\n",
                        (unsigned long)slot);
            std::printf("  Hart 0: tokens=16B, prefetch buf=%lu B (%d edges), stack=%d B\n",
                        (unsigned long)buf_bytes_h0, cap_h0, STACK_RESERVE);
            std::printf("  Hart N: prefetch buf=%lu B (%d edges), stack=%d B\n",
                        (unsigned long)buf_bytes_hn, cap_hn, STACK_RESERVE);
            std::printf("  Total prefetch L1SP per core: %lu / %lu bytes (%lu%%)\n",
                        (unsigned long)total_pf_bytes,
                        (unsigned long)g_core_l1sp_bytes,
                        (unsigned long)(100 * total_pf_bytes / g_core_l1sp_bytes));
        }
        std::printf("======================================\n");

        uint64_t elapsed = end_cycles - start_cycles;
        std::printf("\nCycles elapsed:        %lu\n", (unsigned long)elapsed);
        if (total_processed > 0) {
            std::printf("Cycles per node:       %lu\n",
                        (unsigned long)(elapsed / total_processed));
        }
        if (stat_edges_traversed > 0) {
            std::printf("Cycles per edge:       %lu\n",
                        (unsigned long)(elapsed / (uint64_t)stat_edges_traversed));
        }

        // Machine-parseable summary lines for experiment scripts
        std::printf("\nPREFETCH_STATS: nodes=%ld edges=%ld hits=%ld misses=%ld hitrate=%ld cycles=%ld overflow=%ld\n",
                    (long)total_pf_nodes, (long)total_pf_edges,
                    (long)total_pf_hits, (long)total_pf_misses,
                    (total_pf_hits + total_pf_misses > 0)
                        ? (long)(100 * total_pf_hits / (total_pf_hits + total_pf_misses)) : 0L,
                    (long)total_pf_cycles, (long)total_pf_overflows);

        // Release memory
        std::free(g_file_buffer);
        if (!g_visited_in_l2sp) std::free((void *)visited);
        if (!g_dist_in_l2sp) std::free(dist_arr);
        std::free(g_current_frontier_dram_storage);
        std::free(g_next_frontier.dram_items);
        std::free(g_next_frontier.dram_owner_core);
        g_file_buffer = nullptr;
        g_row_ptr = nullptr;
        g_col_idx = nullptr;
        visited = nullptr;
        dist_arr = nullptr;
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
