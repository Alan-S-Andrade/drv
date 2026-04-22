// bfs_csr_shared_queue.cpp
// Level-synchronous CSR BFS with L1SP queues, L2SP overflow, and work stealing.
//
// Architecture:
//   1. At level start, hart 0 distributes frontier round-robin into per-core
//      L1SP queues (bounded). Excess goes to shared L2SP queue.
//   2. Harts pop from their core's L1SP queue (CAS, 16-way contention).
//   3. When L1SP empty, one hart refills from shared L2SP queue (fetch-add).
//   4. When L2SP also empty, steal from the fullest L1SP queue of another core.
//   5. Discoveries go to a separate next-frontier buffer (level-synchronous).
//
// Memory layout:
//   L1SP: per-core local queue items (fast pop, absolute addressing)
//   L2SP: per-core queue control (head/tail/lock), shared queue, frontier, stats
//   DRAM: CSR graph (row_ptr, col_idx), dist[], frontier overflow
//
// Graph bulk-loaded from file via MMIO (same as bfs_csr_work_stealing.cpp).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>

#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>
#include <pandohammer/mmio.h>
#include <pandohammer/address.h>
#include <pandohammer/staticdecl.h>

#include "work_stealing.h"   // reuse barrier from ws::

// =============================================================================
// PART 1 — CONFIGURATION CONSTANTS
// =============================================================================
// Compile-time knobs that shape the algorithm:
//   * DEFAULT_* — fallbacks when --V / --D are not passed.
//   * MAX_HARTS / MAX_CORES — upper bounds for statically-sized arrays below.
//   * BFS_CHUNK_SIZE  — vertices a hart pops per local-queue grab.
//   * LOCAL_BUF_SIZE  — per-hart discovery buffer before a batched flush to the
//                       next-level frontier (amortises the fetch-add tail bump).
//   * CORE_Q_CAP      — per-core L1SP circular buffer depth (power of 2).
//   * REFILL_SIZE     — items moved L2SP-shared-queue -> L1SP-core-queue each
//                       refill.
//   * STEAL_SIZE      — items taken when one core steals from another's L1SP.
//   * L1SP_ALIGN_PAD / CORE_Q_L1SP_OFFSET / L1SP_DATA_START / L1SP_STACK_GUARD
//     — layout within each core's L1SP: low bytes reserved, then the circular
//       queue items, then the frontier L1SP tier, with a stack guard at top.
// -----------------------------------------------------------------------------
// ---------- Configuration ----------
#ifndef DEFAULT_VTX_PER_THREAD
#define DEFAULT_VTX_PER_THREAD 1024
#endif
#ifndef DEFAULT_DEGREE
#define DEFAULT_DEGREE 16
#endif

static constexpr int MAX_HARTS = 2048;
static constexpr int MAX_CORES = 64;
static constexpr int BFS_CHUNK_SIZE = 8;      // Vertices per local queue pop
static constexpr int LOCAL_BUF_SIZE = 32;     // Per-hart discovery buffer (flushed in batch)

// Per-core local queue in L1SP
static constexpr int CORE_Q_CAP = 256;        // Power-of-2 circular buffer capacity
static constexpr int CORE_Q_MASK = CORE_Q_CAP - 1;
static constexpr int REFILL_SIZE = 128;       // Vertices taken from shared queue per refill
static constexpr int STEAL_SIZE = 64;         // Vertices stolen from another core's L1SP queue

// L1SP layout: [0..16) padding, [16..16+CORE_Q_CAP*8) queue items, rest for frontier
static constexpr uintptr_t L1SP_ALIGN_PAD = 16;
static constexpr uintptr_t CORE_Q_L1SP_OFFSET = L1SP_ALIGN_PAD;
static constexpr uintptr_t L1SP_DATA_START = CORE_Q_L1SP_OFFSET + CORE_Q_CAP * sizeof(int64_t);
static constexpr uintptr_t L1SP_STACK_GUARD = 5120;  // 5KB guard for stack growth

// =============================================================================
// PART 2 — DATA STRUCTURES
// =============================================================================
// Two small structs define the work-distribution machinery:
//
//   CoreQueueCtrl — one per core, lives in L2SP. Holds the head/tail/lock of
//                   the core's L1SP circular item buffer. Cache-line padded so
//                   writes from different cores don't false-share.
//                   * head       : next item to pop (CAS-advanced).
//                   * read_head  : slots safely readable by cross-core thieves;
//                                  refill must not overwrite past this.
//                   * tail       : one past last valid item.
//                   * refill_lock: only one hart per core refills at a time.
//
//   SharedQueue   — the pod-wide overflow pool in L2SP. Cores fetch-add their
//                   way through it to refill their L1SP queue. Head/tail on
//                   separate cache lines (alignas(64)) to avoid false sharing
//                   between producer and consumer sides.
// -----------------------------------------------------------------------------
// ---------- Per-Core Queue Control (in L2SP) ----------
// Padded to 64 bytes to avoid false sharing between cores.
// Items array lives in L1SP (via absolute addressing).
struct alignas(64) CoreQueueCtrl {
    volatile int64_t head;        // next to pop (monotonically increasing, CAS-advanced)
    volatile int64_t read_head;   // advanced after items are fully read (safe-to-overwrite boundary)
    volatile int64_t tail;        // end of valid entries (monotonically increasing)
    volatile int32_t refill_lock; // 0=unlocked, 1=refilling
    int32_t _pad0;
    int64_t _pad1[2];             // pad to 64 bytes total
};

// ---------- Shared Queue ----------
// Global work pool in L2SP. Cores steal chunks via fetch-add.
// Head and tail on separate cache lines to avoid false sharing.
struct SharedQueue {
    alignas(64) volatile int64_t head;
    alignas(64) volatile int64_t tail;
};

// =============================================================================
// PART 3 — SMALL HELPERS
// =============================================================================
// Utility routines used during startup and accounting:
//   * parse_i       — argv integer parse with default.
//   * bulk_load     — host-file -> simulated DRAM transfer via MMIO
//                     (ph_bulk_load_file). Used to load the CSR graph and the
//                     pre-computed bfs_dist_init.bin.
//   * floor_pow2    — largest power of two <= n (used for L1SP per-core sizing).
//   * ilog2_pow2    — log2 of a power of two (used as the shift amount when
//                     indexing the per-core L1SP frontier tier).
// -----------------------------------------------------------------------------
// ---------- Helpers ----------
static int parse_i(const char *s, int d)
{
    if (!s) return d;
    char *e = nullptr;
    long v = strtol(s, &e, 10);
    return (e && *e == 0) ? (int)v : d;
}

static void bulk_load(const char *name, void *dest, size_t size)
{
    char fname_buf[64];
    int fi = 0;
    while (name[fi] && fi < 63) { fname_buf[fi] = name[fi]; fi++; }
    fname_buf[fi] = '\0';

    struct ph_bulk_load_desc desc;
    desc.filename_addr = (long)fname_buf;
    desc.dest_addr     = (long)dest;
    desc.size          = (long)size;
    desc.result        = 0;
    ph_bulk_load_file(&desc);

    if (desc.result <= 0) {
        std::printf("ERROR: bulk load '%s' failed (result=%ld)\n", name, desc.result);
    }
}

static inline int32_t floor_pow2(int32_t n) {
    if (n <= 0) return 0;
    int32_t p = 1;
    while (p * 2 <= n) p *= 2;
    return p;
}

static inline int32_t ilog2_pow2(int32_t n) {
    int32_t k = 0;
    while ((1 << (k + 1)) <= n) k++;
    return k;
}

// =============================================================================
// PART 4 — L2SP GLOBALS (shared pod state)
// =============================================================================
// Everything tagged __l2sp__ lives in the pod's L2 scratchpad and is shared by
// all harts on the pod. Grouped by purpose:
//   * Runtime config      — topology (harts/cores), init flag.
//   * Barrier             — hart synchronisation between levels.
//   * Shared queue        — overflow pool cores refill from.
//   * Per-core queue ctrl — head/tail/lock for each core's L1SP queue.
//   * Graph parameters    — N, degree, source vertex.
//   * DRAM pointers       — CSR arrays + dist[] (actual arrays in DRAM, these
//                           pointers are in L2SP for fast deref).
//   * Control flags       — sim exit, frontier-overflow error.
//   * Quiescence counter  — g_active_harts, used by the termination check.
//   * Reduction accum.    — g_sum_dist / g_reached / g_max_dist (result stats).
//   * Per-hart/per-core   — stat_* arrays populated during the BFS loop and
//     stats                 printed at the end.
//   * Frontier tier state — cur_buf, frontier_tail[2] (double-buffered), plus
//                           the L2SP / L1SP / DRAM tier descriptors used by
//                           frontier_ptr() to route an index to the right tier.
//   * l2sp_end symbol     — provided by the linker; marks where the L2SP heap
//                           begins, so alloc_frontier_storage() can bump-alloc.
// -----------------------------------------------------------------------------
// ---------- L2SP Globals ----------

// Runtime config
__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

// Barrier
__l2sp__ ws::BarrierState<MAX_HARTS> g_barrier;

// Shared queue (indexes into current frontier buffer)
__l2sp__ SharedQueue shared_queue;

// Per-core local queue control (items in L1SP)
__l2sp__ CoreQueueCtrl core_q[MAX_CORES];

// Graph parameters
__l2sp__ int32_t g_N;
__l2sp__ int32_t g_degree;
__l2sp__ int32_t g_source;

// DRAM pointers (stored in L2SP for fast access)
__l2sp__ char    *g_file_buffer;
__l2sp__ int32_t *g_csr_offsets;
__l2sp__ int32_t *g_csr_edges;
__l2sp__ int32_t *g_dist;

// Control
__l2sp__ volatile int g_sim_exit;
__l2sp__ volatile int32_t g_frontier_error;   // set to 1 on frontier overflow

// Quiescence: tracks harts actively processing work (for safe termination)
__l2sp__ volatile int64_t g_active_harts;

// Reduction accumulators
__l2sp__ volatile int64_t g_sum_dist;
__l2sp__ volatile int32_t g_reached;
__l2sp__ volatile int32_t g_max_dist;

// Per-hart / per-core stats
__l2sp__ volatile int64_t stat_nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_edges_per_core[MAX_CORES];
__l2sp__ volatile int64_t stat_nodes_per_core[MAX_CORES];
__l2sp__ volatile int64_t stat_steals_per_core[MAX_CORES];

// ---------- Double-buffered frontier (L2SP -> L1SP -> DRAM) ----------
// Two buffers: cur_buf and cur_buf^1.  Swapped each level (no data copy).
__l2sp__ volatile int cur_buf;                  // 0 or 1
// Each tail on its own cache line to avoid false sharing under atomic fetch-add
struct alignas(64) FrontierTail { volatile int64_t val; };
__l2sp__ FrontierTail frontier_tail[2];

// L2SP tier
__l2sp__ int64_t *frontier_l2sp[2];
__l2sp__ int64_t frontier_l2sp_cap;            // entries per buffer in L2SP

// L1SP tier (distributed across cores via absolute addressing)
__l2sp__ uintptr_t g_l1sp_abs_base[MAX_CORES]; // absolute base per core
__l2sp__ int32_t g_l1sp_data_bytes_per_core;
__l2sp__ int64_t frontier_l1sp_cap;            // entries per buffer total
__l2sp__ int32_t frontier_l1sp_per_core;       // entries per core (power of 2)
__l2sp__ int32_t frontier_l1sp_shift;          // log2(per_core)
__l2sp__ uintptr_t frontier_l1sp_offset[2];    // byte offset within each core's L1SP

// DRAM tier
__l2sp__ int64_t *frontier_dram[2];
__l2sp__ int64_t frontier_total_cap;           // total entries per buffer (L2SP+L1SP+DRAM)

// L2SP heap boundary
extern "C" char l2sp_end[];

// =============================================================================
// PART 5 — THREAD ID + BARRIER
// =============================================================================
// Thin wrappers over the hardware topology:
//   * get_thread_id() — flat pod-wide hart id (core*harts_per_core + hart).
//   * barrier()       — pod-wide level-sync barrier (reuses ws::barrier from
//                       work_stealing.h). Called between BFS levels so the
//                       frontier swap and distribute_frontier happen atomically
//                       with respect to all harts.
// -----------------------------------------------------------------------------
// ---------- Thread ID + Barrier ----------
static inline int get_thread_id() {
    return myCoreId() * (int)g_harts_per_core + myThreadId();
}

static inline void barrier() {
    ws::barrier(&g_barrier, get_thread_id(), g_total_harts);
}

// =============================================================================
// PART 6 — FRONTIER STORAGE (tiered: L2SP -> L1SP -> DRAM)
// =============================================================================
// The BFS frontier is double-buffered (two buffers, swapped each level via
// cur_buf ^= 1). Each buffer is spread across three tiers, fastest first:
//
//   Tier 1 — L2SP : pod-shared, up to what fits in the L2SP heap.
//   Tier 2 — L1SP : distributed across cores; each core holds an equal
//                   power-of-two slice accessed via absolute addressing.
//                   Index -> (core, local) via frontier_l1sp_shift / mask.
//   Tier 3 — DRAM : overflow via plain malloc().
//
// init_l1sp_data_regions()  sets up the absolute-address base per core and
//                           computes how many bytes of each core's L1SP are
//                           free for data (after queue items + stack guard).
// frontier_ptr / get / set  route a logical index [0..N) to the right tier.
// (distribute_frontier, flush_discoveries, and alloc_frontier_storage further
//  down are also part of this frontier layer.)
// -----------------------------------------------------------------------------
// ---------- L1SP Data Region Setup ----------
static inline void init_l1sp_data_regions() {
    for (int c = 0; c < g_total_cores; c++) {
        uintptr_t addr = 0;
        addr = ph_address_set_absolute(addr, 1);
        addr = ph_address_absolute_set_core(addr, (uintptr_t)c);
        addr = ph_address_absolute_set_l1sp_offset(addr, 0);
        g_l1sp_abs_base[c] = addr;
    }

    uint64_t core_l1sp = coreL1SPSize();
    uint64_t hart_cap = core_l1sp / (uint64_t)g_harts_per_core;
    if (hart_cap > L1SP_DATA_START + L1SP_STACK_GUARD) {
        g_l1sp_data_bytes_per_core = (int32_t)(hart_cap - L1SP_STACK_GUARD - L1SP_DATA_START);
    } else {
        g_l1sp_data_bytes_per_core = 0;
    }
}

// ---------- Frontier Accessors (L2SP -> L1SP -> DRAM) ----------
static inline int64_t* frontier_ptr(int buf, int64_t idx) {
    if (idx < frontier_l2sp_cap)
        return frontier_l2sp[buf] + idx;
    int64_t r = idx - frontier_l2sp_cap;
    if (r < frontier_l1sp_cap) {
        int core = (int)(r >> frontier_l1sp_shift);
        int local = (int)(r & ((1 << frontier_l1sp_shift) - 1));
        return (int64_t*)(g_l1sp_abs_base[core] + frontier_l1sp_offset[buf]) + local;
    }
    return frontier_dram[buf] + (r - frontier_l1sp_cap);
}

static inline int64_t frontier_get(int buf, int64_t idx) {
    return *frontier_ptr(buf, idx);
}

static inline void frontier_set(int buf, int64_t idx, int64_t val) {
    *frontier_ptr(buf, idx) = val;
}

// =============================================================================
// PART 7 — PER-CORE LOCAL QUEUE OPERATIONS (producer/consumer on L1SP)
// =============================================================================
// Each core has a circular buffer in its own L1SP holding vertex ids ready to
// be processed. Head/tail/lock live in L2SP (see CoreQueueCtrl). Two-level
// queueing: pop is almost always fast because only the 16 harts on that core
// contend for head. When the local queue is empty, ONE hart per core refills
// it from the shared L2SP queue (protected by refill_lock).
//
//   core_q_items()        — absolute-address pointer to a core's L1SP items.
//   core_q_pop_chunk()    — TTAS + CAS to atomically grab up to max_k items.
//                           Uses a fence r,r so readers see item writes that
//                           preceded the publishing tail bump.
//   core_q_try_refill()   — one hart acquires refill_lock, fetch-adds on the
//                           shared queue, copies items L2SP frontier -> L1SP
//                           circular buffer, publishes via tail update.
//                           Returns: 1 = got/has work, 0 = retry, -1 = shared
//                           queue globally empty (caller should try stealing).
//
// The read_head field is what makes this safe against concurrent cross-core
// stealers: refill won't overwrite slots a thief may still be reading.
// -----------------------------------------------------------------------------
// ---------- Per-Core Local Queue Operations ----------
// Items stored in L1SP circular buffer, control (head/tail/lock) in L2SP.
// Only 16 harts per core contend on each core's queue — CAS nearly always succeeds.

// Get pointer to core's queue items in L1SP (via absolute addressing)
static inline volatile int64_t* core_q_items(int core_id) {
    return (volatile int64_t*)(g_l1sp_abs_base[core_id] + CORE_Q_L1SP_OFFSET);
}

// Batch pop from core's local queue. Returns count (0 if empty or contention).
static inline int64_t core_q_pop_chunk(int my_core, int64_t *out_buf, int64_t max_k) {
    CoreQueueCtrl* q = &core_q[my_core];

    // TTAS: volatile pre-check
    int64_t h = q->head;
    int64_t t = q->tail;
    if (h >= t) return 0;

    // CAS phase (16-way contention on L2SP — nearly always succeeds)
    h = atomic_load_i64(&q->head);
    t = q->tail;
    if (h >= t) return 0;

    int64_t avail = t - h;
    int64_t k = (avail < max_k) ? avail : max_k;

    int64_t old_h = atomic_compare_and_swap_i64(&q->head, h, h + k);
    if (old_h != h) return 0;  // contention, caller retries

    // Fence: ensure L1SP item reads see writes that preceded the tail update
    asm volatile("fence r,r" ::: "memory");

    // Read items from L1SP circular buffer
    volatile int64_t* items = core_q_items(my_core);
    for (int64_t i = 0; i < k; i++) {
        out_buf[i] = items[(old_h + i) & CORE_Q_MASK];
    }

    // Advance read_head to signal these slots are safe to overwrite.
    // This prevents a concurrent refiller from overwriting slots we just read.
    // Use CAS loop since multiple harts may advance read_head concurrently.
    int64_t rh = q->read_head;
    while (rh < old_h + k) {
        int64_t new_rh = old_h + k;
        int64_t prev = atomic_compare_and_swap_i64(&q->read_head, rh, new_rh);
        if (prev == rh) break;
        rh = prev;
    }
    return k;
}

// Try to refill core's local queue from the shared L2SP queue.
// Returns: 1 = refilled (or already has work), 0 = busy (another hart refilling),
//         -1 = globally empty (no more work in shared queue)
static inline int core_q_try_refill(int my_core) {
    CoreQueueCtrl* q = &core_q[my_core];

    // Try to acquire refill lock
    if (atomic_compare_and_swap_i32(&q->refill_lock, 0, 1) != 0)
        return 0;  // another hart is refilling

    // Double-check: queue might have been refilled between our pop and lock
    if (q->tail > atomic_load_i64(&q->head)) {
        q->refill_lock = 0;
        return 1;  // already has work
    }

    // Compute available buffer space using read_head (not head), so we don't
    // overwrite slots that a cross-core thief may still be reading from.
    volatile int64_t* items = core_q_items(my_core);
    int64_t base = q->tail;
    int64_t safe_head = atomic_load_i64(&q->read_head);
    int64_t space = CORE_Q_CAP - (base - safe_head);
    int64_t refill_amt = (space < REFILL_SIZE) ? space : (int64_t)REFILL_SIZE;
    if (refill_amt <= 0) {
        q->refill_lock = 0;
        return 0;  // no space, retry later
    }

    // Claim items from the shared L2SP queue via fetch-add
    int64_t old_h = atomic_fetch_add_i64(&shared_queue.head, refill_amt);
    int64_t t = atomic_load_i64(&shared_queue.tail);

    if (old_h >= t) {
        q->refill_lock = 0;
        return -1;  // globally empty
    }

    int64_t actual = refill_amt;
    if (old_h + actual > t) actual = t - old_h;

    // Copy vertex IDs from frontier storage to local L1SP queue
    for (int64_t i = 0; i < actual; i++) {
        items[(base + i) & CORE_Q_MASK] = frontier_get(cur_buf, old_h + i);
    }

    // Fence: ensure L1SP item writes are globally visible before tail update
    asm volatile("fence w,w" ::: "memory");

    // Publish: update tail makes items visible to other harts
    q->tail = base + actual;
    q->refill_lock = 0;
    return 1;
}

// =============================================================================
// PART 8 — NEXT-FRONTIER BATCH PUSH
// =============================================================================
// Harts accumulate newly-discovered vertices in a small per-hart local buffer
// and push them into the next-level frontier in bursts (here, LOCAL_BUF_SIZE
// items at a time). A single fetch-add reserves a contiguous slot range on
// frontier_tail[next_buf], then items are written via frontier_set() which
// routes each write to L2SP / L1SP / DRAM by index. Overflow is detected
// against frontier_total_cap (sets g_frontier_error so the level loop aborts).
// -----------------------------------------------------------------------------
// ---------- Next Frontier Batch Push ----------
// Flush local discovery buffer to next frontier in one atomic tail bump.
static inline void flush_discoveries(int64_t *buf, int count, int next_buf) {
    if (count <= 0) return;
    int64_t base = atomic_fetch_add_i64(&frontier_tail[next_buf].val, (int64_t)count);
    if (base + count > frontier_total_cap) {
        std::printf("ERROR: frontier overflow at index %ld (cap=%ld)\n",
                    (long)(base + count), (long)frontier_total_cap);
        g_frontier_error = 1;
        return;
    }
    for (int i = 0; i < count; i++) {
        frontier_set(next_buf, base + i, buf[i]);
    }
}

// =============================================================================
// PART 9 — LEVEL-START FRONTIER DISTRIBUTION
// =============================================================================
// Called by hart 0 at the top of each BFS level. Takes the current frontier
// (size = frontier_tail[cur_buf]) and seeds each core's L1SP circular queue
// with an equal slice (ceiling-div so small frontiers still land in L1SP).
// Anything that doesn't fit in the L1SP queues stays in the frontier buffer
// and is accessed through the shared L2SP queue's head/tail range. This is
// what gives the algorithm its "L1SP fast path, L2SP overflow" character.
// -----------------------------------------------------------------------------
// ---------- Distribute Frontier into L1SP Queues ----------
// Hart 0 calls this at level start. Distributes frontier entries round-robin
// into per-core L1SP queues (up to CORE_Q_CAP each). Overflow stays in the
// frontier buffer, accessible via the shared L2SP queue.
static void distribute_frontier() {
    int64_t fsize = frontier_tail[cur_buf].val;
    int nc = g_total_cores;

    // Distribute frontier to L1SP queues. Use ceiling division so small
    // frontiers still land in fast L1SP rather than falling through to L2SP.
    int64_t per_core = (fsize + nc - 1) / nc;  // ceiling division
    if (per_core > CORE_Q_CAP) per_core = CORE_Q_CAP;

    int64_t distributed = 0;
    for (int c = 0; c < nc; c++) {
        int64_t remaining = fsize - distributed;
        int64_t count = (per_core < remaining) ? per_core : remaining;
        if (count < 0) count = 0;

        if (count > 0) {
            volatile int64_t* items = core_q_items(c);
            for (int64_t i = 0; i < count; i++) {
                items[i] = frontier_get(cur_buf, distributed + i);
            }
        }
        asm volatile("fence w,w" ::: "memory");
        core_q[c].head = 0;
        core_q[c].read_head = 0;
        core_q[c].tail = count;
        core_q[c].refill_lock = 0;
        distributed += count;
    }

    // Remaining frontier entries accessible via shared L2SP queue
    shared_queue.head = distributed;
    shared_queue.tail = fsize;
}

// =============================================================================
// PART 10 — CROSS-CORE WORK STEALING
// =============================================================================
// Last-resort work acquisition when BOTH the local L1SP queue AND the shared
// L2SP queue are empty. One hart per stealing core (serialised via the core's
// own refill_lock) scans every other core's CoreQueueCtrl, picks the one with
// the largest head..tail gap, and CAS-claims up to STEAL_SIZE items from the
// victim's head. Items are copied from the victim's L1SP circular buffer into
// the stealer's L1SP circular buffer, then the victim's read_head is advanced
// so its next refill knows those slots are safe to overwrite.
//
// Return codes: 1 = stole work, 0 = contention (caller retries), -1 = nothing
// worth stealing (caller moves to the termination check).
// -----------------------------------------------------------------------------
// ---------- Work Stealing from Another Core's L1SP Queue ----------
// Scans all cores, finds the one with the most items, steals a fixed chunk.
// Returns: 1 = stole work, 0 = contention (retry), -1 = nothing to steal.
static inline int steal_from_fullest(int my_core) {
    CoreQueueCtrl* myq = &core_q[my_core];

    // Check shared queue BEFORE acquiring lock — prefer L2SP refill over stealing
    if (atomic_load_i64(&shared_queue.head) < atomic_load_i64(&shared_queue.tail))
        return 0;  // let normal refill path handle it

    // Acquire our refill lock (only one hart per core attempts steal)
    if (atomic_compare_and_swap_i32(&myq->refill_lock, 0, 1) != 0)
        return 0;

    // Double-check: queue might have work now
    if (myq->tail > atomic_load_i64(&myq->head)) {
        myq->refill_lock = 0;
        return 1;
    }

    // Scan all cores to find the fullest queue
    int best_core = -1;
    int64_t best_avail = 0;
    for (int c = 0; c < g_total_cores; c++) {
        if (c == my_core) continue;
        int64_t avail = core_q[c].tail - atomic_load_i64(&core_q[c].head);
        if (avail > best_avail) {
            best_avail = avail;
            best_core = c;
        }
    }

    if (best_core < 0 || best_avail < 2) {
        myq->refill_lock = 0;
        return -1;  // nothing worth stealing
    }

    // Compute how many items we can receive (check our buffer capacity first)
    volatile int64_t* my_items = core_q_items(my_core);
    int64_t base = myq->tail;
    int64_t my_head = atomic_load_i64(&myq->head);
    int64_t my_space = CORE_Q_CAP - (base - my_head);
    if (my_space <= 0) {
        myq->refill_lock = 0;
        return 0;
    }

    int64_t to_steal = (best_avail < STEAL_SIZE) ? best_avail : (int64_t)STEAL_SIZE;
    if (to_steal > my_space) to_steal = my_space;

    // CAS on victim's head to claim exactly what we can receive
    CoreQueueCtrl* victim = &core_q[best_core];
    int64_t h = atomic_load_i64(&victim->head);
    int64_t t = victim->tail;
    int64_t actual_avail = t - h;
    if (actual_avail < 2) {
        myq->refill_lock = 0;
        return -1;
    }
    if (to_steal > actual_avail) to_steal = actual_avail;

    int64_t old_h = atomic_compare_and_swap_i64(&victim->head, h, h + to_steal);
    if (old_h != h) {
        myq->refill_lock = 0;
        return 0;  // contention, caller retries
    }

    // Read items from victim's L1SP circular buffer
    asm volatile("fence r,r" ::: "memory");
    volatile int64_t* victim_items = core_q_items(best_core);

    for (int64_t i = 0; i < to_steal; i++) {
        my_items[(base + i) & CORE_Q_MASK] = victim_items[(old_h + i) & CORE_Q_MASK];
    }

    // Signal victim that these slots are safe to overwrite
    int64_t rh = victim->read_head;
    while (rh < old_h + to_steal) {
        int64_t prev = atomic_compare_and_swap_i64(&victim->read_head, rh, old_h + to_steal);
        if (prev == rh) break;
        rh = prev;
    }

    // Publish stolen items
    asm volatile("fence w,w" ::: "memory");
    myq->tail = base + to_steal;
    myq->refill_lock = 0;

    atomic_fetch_add_i64(&stat_steals_per_core[my_core], 1);
    return 1;
}

// =============================================================================
// PART 11 — BFS INNER LOOP (core algorithm for one level)
// =============================================================================
// This is THE BFS kernel. Every hart runs this function once per level. The
// control flow is a state machine over three sources of work:
//
//   [POP]    core_q_pop_chunk  — grab up to BFS_CHUNK_SIZE vertices from own
//                                core's L1SP queue; relax outgoing edges with
//                                CAS(dist[v], -1, level+1); push winners into
//                                the per-hart discovery buffer; flush to the
//                                next frontier when the buffer fills.
//   [REFILL] core_q_try_refill — when the local queue is empty, pull a REFILL
//                                chunk from the shared L2SP queue.
//   [STEAL]  steal_from_fullest — when shared is also empty, steal from
//                                another core's L1SP queue.
//
// Termination — "is_active" counter (g_active_harts). A hart only decrements
// it when it has nothing to pop, nothing to refill, AND nothing to steal.
// Then it verifies (a) no other hart is active, (b) shared queue is empty,
// (c) every core_q is empty. Only then does it exit the loop. This avoids
// premature termination when work is in flight between tiers.
//
// Final bookkeeping: flush remaining discoveries and update per-hart/per-core
// stats (stat_nodes_processed, stat_nodes_per_core, stat_edges_per_core).
// -----------------------------------------------------------------------------
// ---------- BFS Level Processing ----------
static void process_bfs_level(int tid, int32_t level)
{
    const int my_core = tid / g_harts_per_core;
    const int next_buf = cur_buf ^ 1;

    int32_t *offsets = g_csr_offsets;
    int32_t *edges   = g_csr_edges;
    int32_t *dist    = g_dist;

    int64_t local_processed = 0;
    int64_t local_edges = 0;
    int64_t chunk_buf[BFS_CHUNK_SIZE];

    // Per-hart local buffer for batching next-frontier pushes
    int64_t disc_buf[LOCAL_BUF_SIZE];
    int disc_count = 0;

    // Stateful quiescence: stay "active" while popping, refilling, or stealing.
    // Only drop active status when truly idle and about to check termination.
    bool is_active = false;

    while (true) {
        // Become active if not already (covers pop, refill, and steal attempts)
        if (!is_active) {
            atomic_fetch_add_i64(&g_active_harts, 1);
            is_active = true;
        }

        // Pop from core's local L1SP queue
        int64_t count = core_q_pop_chunk(my_core, chunk_buf, BFS_CHUNK_SIZE);

        if (count > 0) {
            for (int64_t i = 0; i < count; i++) {
                int32_t u = (int32_t)chunk_buf[i];
                local_processed++;

                int32_t edge_begin = offsets[u];
                int32_t edge_end   = offsets[u + 1];
                local_edges += (edge_end - edge_begin);

                for (int32_t ei = edge_begin; ei < edge_end; ei++) {
                    int32_t v = edges[ei];
                    if (atomic_compare_and_swap_i32(&dist[v], -1, level + 1) == -1) {
                        disc_buf[disc_count++] = (int64_t)v;
                        if (disc_count == LOCAL_BUF_SIZE) {
                            flush_discoveries(disc_buf, disc_count, next_buf);
                            disc_count = 0;
                        }
                    }
                }
            }
            // Stay active — loop back to pop immediately
        } else {
            // Local queue empty — try to refill from shared L2SP queue
            int refill = core_q_try_refill(my_core);
            if (refill == 1) {
                continue;  // refilled, stay active, go pop
            } else if (refill == 0) {
                hartsleep(1);  // another hart is refilling, stay active, retry
                continue;
            }

            // refill == -1: shared queue empty — try stealing from fullest core
            int steal = steal_from_fullest(my_core);
            if (steal == 1) {
                continue;  // stole work, stay active, go pop
            } else if (steal == 0) {
                hartsleep(1);  // contention, stay active, retry
                continue;
            }

            // steal == -1: nothing to steal — we are truly idle
            if (disc_count > 0) {
                flush_discoveries(disc_buf, disc_count, next_buf);
                disc_count = 0;
            }

            // Drop active status ONLY before checking global termination
            atomic_fetch_add_i64(&g_active_harts, -1);
            is_active = false;

            // Confirm everything is truly exhausted and no harts are mid-work
            bool all_empty = true;
            if (atomic_load_i64(&g_active_harts) > 0) {
                all_empty = false;
            } else if (atomic_load_i64(&shared_queue.head) < atomic_load_i64(&shared_queue.tail)) {
                all_empty = false;
            } else {
                for (int c = 0; c < g_total_cores; c++) {
                    if (core_q[c].tail > atomic_load_i64(&core_q[c].head)) {
                        all_empty = false;
                        break;
                    }
                }
            }
            if (all_empty) break;
            hartsleep(1);  // race window, retry
        }
    }

    // Safety: decrement if we broke out while still active
    if (is_active) {
        atomic_fetch_add_i64(&g_active_harts, -1);
    }

    // Final flush
    if (disc_count > 0) {
        flush_discoveries(disc_buf, disc_count, next_buf);
    }

    stat_nodes_processed[tid] += local_processed;
    atomic_fetch_add_i64(&stat_nodes_per_core[my_core], local_processed);
    atomic_fetch_add_i64(&stat_edges_per_core[my_core], local_edges);
}

// =============================================================================
// PART 12 — FRONTIER STORAGE ALLOCATION
// =============================================================================
// One-time setup (hart 0 only) of the three frontier tiers for N vertices.
// Greedy top-down fill:
//   1. Consume as much L2SP heap as possible (up to N entries per buffer).
//   2. If entries remain, carve equal power-of-two slices out of each core's
//      L1SP data region (rounded down to keep indexing a shift).
//   3. malloc() any leftover into DRAM (both buffers).
// Writes into the L2SP globals described in PART 6 so the frontier_ptr()
// accessors know where each logical index lives. Returns false on malloc
// failure (main then reports an error and signals exit).
// -----------------------------------------------------------------------------
// ---------- Frontier Memory Allocation (L2SP -> L1SP -> DRAM) ----------
// Allocates double-buffered frontier storage across memory tiers.
// Returns false on allocation failure.
static bool alloc_frontier_storage(uintptr_t *l2sp_heap, uintptr_t l2sp_limit,
                                   uintptr_t *l1sp_heap_off, uintptr_t l1sp_data_end,
                                   int32_t N)
{
    frontier_l2sp_cap = 0;
    frontier_l1sp_cap = 0;
    frontier_l1sp_per_core = 0;
    frontier_l1sp_shift = 0;
    frontier_total_cap = N;
    frontier_l2sp[0] = nullptr;
    frontier_l2sp[1] = nullptr;
    frontier_dram[0] = nullptr;
    frontier_dram[1] = nullptr;

    // L2SP tier: as many entries as fit (split between 2 buffers)
    {
        uintptr_t heap = (*l2sp_heap + 7) & ~(uintptr_t)7;
        uintptr_t avail = (heap < l2sp_limit) ? (l2sp_limit - heap) : 0;
        // Each entry = int64_t, need 2 buffers
        int64_t max_per_buf = (int64_t)(avail / (2 * sizeof(int64_t)));
        if (max_per_buf > N) max_per_buf = N;

        if (max_per_buf > 0) {
            size_t bytes = (size_t)max_per_buf * sizeof(int64_t);
            frontier_l2sp[0] = (int64_t *)heap;
            heap += bytes;
            heap = (heap + 7) & ~(uintptr_t)7;
            frontier_l2sp[1] = (int64_t *)heap;
            heap += bytes;
            heap = (heap + 7) & ~(uintptr_t)7;
            frontier_l2sp_cap = max_per_buf;
            *l2sp_heap = heap;
        }
    }

    int64_t remaining = N - frontier_l2sp_cap;

    // L1SP tier: distributed across cores (power-of-2 per core for fast indexing)
    // Note: L1SP_DATA_START already accounts for per-core queue items region
    if (remaining > 0 && *l1sp_heap_off < l1sp_data_end) {
        int32_t avail_bytes = (int32_t)(l1sp_data_end - *l1sp_heap_off);
        // Need space for 2 buffers in each core's L1SP
        int32_t entries_raw = avail_bytes / (int32_t)(2 * sizeof(int64_t));
        int32_t epc = floor_pow2(entries_raw);  // entries per core per buffer
        if (epc > 0) {
            int64_t total = (int64_t)epc * g_total_cores;
            if (total > remaining) {
                epc = floor_pow2((int32_t)(remaining / g_total_cores));
                total = (int64_t)epc * g_total_cores;
            }
            if (epc > 0) {
                frontier_l1sp_cap = total;
                frontier_l1sp_per_core = epc;
                frontier_l1sp_shift = ilog2_pow2(epc);
                frontier_l1sp_offset[0] = *l1sp_heap_off;
                frontier_l1sp_offset[1] = *l1sp_heap_off + (uintptr_t)epc * sizeof(int64_t);
                *l1sp_heap_off += (uintptr_t)epc * 2 * sizeof(int64_t);
            }
        }
        remaining = N - frontier_l2sp_cap - frontier_l1sp_cap;
    }

    // DRAM tier: everything else
    if (remaining > 0) {
        size_t bytes = (size_t)remaining * sizeof(int64_t);
        frontier_dram[0] = (int64_t *)std::malloc(bytes);
        frontier_dram[1] = (int64_t *)std::malloc(bytes);
        if (!frontier_dram[0] || !frontier_dram[1]) {
            std::printf("ERROR: malloc failed for frontier DRAM (%lu bytes x 2)\n",
                        (unsigned long)bytes);
            std::free(frontier_dram[0]);
            std::free(frontier_dram[1]);
            return false;
        }
    }

    return true;
}

// =============================================================================
// PART 13 — MAIN (per-hart entry point + BFS driver)
// =============================================================================
// Every hart on the simulated machine enters main(). The function splits into
// four phases; which ones a given hart runs depends on its topology id.
//
//   Phase A — Argument parsing & topology reads
//     Parse --V / --D. Query myCoreId / myThreadId / myPodId / myPXNId and
//     derive flat tid. Harts on non-(pxn0,pod0), or beyond threads_per_pod,
//     spin on g_sim_exit and return.
//
//   Phase B — Hart-0 init (hart 0 only)
//     Fill in the __l2sp__ globals (topology, counters, barrier state), set
//     up per-core queue control, call init_l1sp_data_regions(), malloc +
//     bulk_load the graph file and dist[] init file, validate header, call
//     alloc_frontier_storage(), print the banner describing the resulting
//     tier layout, seed frontier[0] with the source vertex, and release
//     g_initialized. Other harts spin on g_initialized until this completes.
//
//   Phase C — BFS level loop (all harts)
//     While frontier is non-empty and no overflow:
//       - hart 0 zeros the next-buffer tail and calls distribute_frontier()
//       - barrier()
//       - ph_stat_phase(1); process_bfs_level(); ph_stat_phase(0)
//         (The stat_phase window is what makes summarize.py's useful_* stats
//          measure only the ROI and not the distribute/barrier overhead.)
//       - barrier(); hart 0 flips cur_buf; barrier()
//
//   Phase D — Results
//     Every hart computes a local (count, sum, max) over its slice of dist[]
//     and atomically folds into g_reached / g_sum_dist / g_max_dist. Hart 0
//     then prints the BFS summary (levels, reached %, avg distance, PASS/FAIL
//     check, per-core work balance, frontier-tier utilisation), free()s all
//     DRAM, and sets g_sim_exit=1 to release the idle harts.
// -----------------------------------------------------------------------------
// ---------- Main ----------
extern "C" int main(int argc, char **argv)
{
    int vtx_per_thread = DEFAULT_VTX_PER_THREAD;
    int degree         = DEFAULT_DEGREE;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--V") && i + 1 < argc)
            vtx_per_thread = parse_i(argv[++i], vtx_per_thread);
        else if (!strcmp(argv[i], "--D") && i + 1 < argc)
            degree = parse_i(argv[++i], degree);
        else if (!strcmp(argv[i], "--help")) {
            std::printf("Usage: %s --V <vtx/thread> --D <degree>\n", argv[0]);
            return 0;
        }
    }

    // ---- Hardware topology ----
    const int hart_in_core   = myThreadId();
    const int core_in_pod    = myCoreId();
    const int pod_in_pxn     = myPodId();
    const int pxn_id         = myPXNId();
    const int harts_per_core = myCoreThreads();
    const int cores_per_pod  = numPodCores();
    const int pods_per_pxn   = numPXNPods();
    const int threads_per_pod = cores_per_pod * harts_per_core;

    const int total_harts_hw =
        numPXN() * pods_per_pxn * cores_per_pod * harts_per_core;

    // Only run on pod 0, pxn 0
    if (pxn_id != 0 || pod_in_pxn != 0) {
        while (g_sim_exit == 0) hartsleep(1000);
        return 0;
    }

    const int tid = core_in_pod * harts_per_core + hart_in_core;

    if (tid >= threads_per_pod) {
        while (g_sim_exit == 0) hartsleep(1000);
        return 0;
    }

    // ================================================================
    // Hart 0: load graph, allocate structures
    // ================================================================
    if (tid == 0) {
        g_total_cores    = cores_per_pod;
        g_harts_per_core = harts_per_core;
        g_total_harts    = threads_per_pod;

        const int N = vtx_per_thread * threads_per_pod;
        const int64_t total_edges = (int64_t)N * degree;

        g_N        = N;
        g_degree   = degree;
        g_sim_exit = 0;
        g_frontier_error = 0;
        g_sum_dist = 0;
        g_reached  = 0;
        g_max_dist = 0;
        g_active_harts = 0;
        cur_buf    = 0;
        frontier_tail[0].val = 0;
        frontier_tail[1].val = 0;
        shared_queue.head = 0;
        shared_queue.tail = 0;

        // Initialize per-core queue control
        for (int c = 0; c < cores_per_pod; c++) {
            core_q[c].head = 0;
            core_q[c].read_head = 0;
            core_q[c].tail = 0;
            core_q[c].refill_lock = 0;
        }

        ws::barrier_init(&g_barrier, threads_per_pod);

        for (int i = 0; i < threads_per_pod; i++)
            stat_nodes_processed[i] = 0;
        for (int c = 0; c < cores_per_pod; c++) {
            stat_nodes_per_core[c] = 0;
            stat_edges_per_core[c] = 0;
            stat_steals_per_core[c] = 0;
        }

        // Set up L1SP data regions
        init_l1sp_data_regions();

        // Allocate and bulk-load graph
        size_t file_size = 20
            + (size_t)(N + 1) * sizeof(int32_t)
            + (size_t)total_edges * sizeof(int32_t)
            + (size_t)N * sizeof(int32_t);

        g_file_buffer = (char *)std::malloc(file_size);
        if (!g_file_buffer) {
            std::printf("ERROR: malloc failed for file buffer (%lu bytes)\n",
                        (unsigned long)file_size);
            g_N = 0;
            g_initialized.store(1, std::memory_order_release);
            return 1;
        }

        bulk_load("uniform_graph.bin", g_file_buffer, file_size);

        int32_t *header = (int32_t *)g_file_buffer;
        int hdr_N      = header[0];
        int hdr_E      = header[1];
        int hdr_source = header[4];

        if (hdr_N != N || hdr_E > (int)total_edges) {
            std::printf("ERROR: graph header mismatch: file(N=%d E=%d) != expected(N=%d E<=%lld)\n",
                        hdr_N, hdr_E, N, (long long)total_edges);
            std::free(g_file_buffer);
            g_N = 0;
            g_initialized.store(1, std::memory_order_release);
            return 1;
        }

        g_source      = hdr_source;
        g_csr_offsets  = (int32_t *)(g_file_buffer + 20);
        g_csr_edges    = (int32_t *)(g_file_buffer + 20 + (size_t)(N + 1) * sizeof(int32_t));

        // Allocate dist[] in DRAM + bulk load
        size_t dist_bytes = (size_t)N * sizeof(int32_t);
        g_dist = (int32_t *)std::malloc(dist_bytes);
        if (!g_dist) {
            std::printf("ERROR: malloc failed for dist[]\n");
            std::free(g_file_buffer);
            g_N = 0;
            g_initialized.store(1, std::memory_order_release);
            return 1;
        }
        bulk_load("bfs_dist_init.bin", g_dist, dist_bytes);

        // Allocate frontier storage (L2SP -> L1SP -> DRAM)
        uintptr_t l2sp_heap = ((uintptr_t)l2sp_end + 7) & ~(uintptr_t)7;
        uintptr_t l2sp_base = 0x20000000;
        uintptr_t l2sp_limit = l2sp_base + podL2SPSize();
        uintptr_t l1sp_heap_off = L1SP_DATA_START;
        uintptr_t l1sp_data_end = L1SP_DATA_START + (uintptr_t)g_l1sp_data_bytes_per_core;

        if (!alloc_frontier_storage(&l2sp_heap, l2sp_limit,
                                    &l1sp_heap_off, l1sp_data_end, N)) {
            std::free(g_dist);
            std::free(g_file_buffer);
            g_N = 0;
            g_initialized.store(1, std::memory_order_release);
            return 1;
        }

        size_t l2sp_used = l2sp_heap - l2sp_base;
        int64_t dram_frontier_entries = N - frontier_l2sp_cap - frontier_l1sp_cap;

        std::printf("=== CSR BFS with L1SP Queues + L2SP Overflow + Work Stealing ===\n");
        std::printf("CSR BFS (bulk load): N=%d E=%d degree=%d source=%d\n",
                    N, hdr_E, degree, hdr_source);
        std::printf("HW: total_harts=%d, pxn=%d pods/pxn=%d cores/pod=%d harts/core=%d\n",
                    total_harts_hw, numPXN(), pods_per_pxn, cores_per_pod, harts_per_core);
        std::printf("Using: %d cores x %d harts = %d total\n",
                    cores_per_pod, harts_per_core, threads_per_pod);

        std::printf("\nLocal queue: cap=%d, refill=%d (items in L1SP, control in L2SP)\n",
                    CORE_Q_CAP, REFILL_SIZE);

        std::printf("\nFrontier tiers (per buffer):\n");
        std::printf("  L2SP: %ld entries (%lu bytes used / %lu total)\n",
                    (long)frontier_l2sp_cap, (unsigned long)l2sp_used,
                    (unsigned long)podL2SPSize());
        if (frontier_l1sp_cap > 0) {
            std::printf("  L1SP: %ld entries (%d/core x %d cores, %d bytes/core usable)\n",
                        (long)frontier_l1sp_cap, frontier_l1sp_per_core,
                        g_total_cores, g_l1sp_data_bytes_per_core);
        } else {
            std::printf("  L1SP: 0 entries (reserved for local queue)\n");
        }
        std::printf("  DRAM: %ld entries\n", (long)dram_frontier_entries);
        std::printf("  Total capacity: %ld entries (= N)\n", (long)frontier_total_cap);
        std::printf("  Pop chunk: %d, discovery buffer: %d\n\n", BFS_CHUNK_SIZE, LOCAL_BUF_SIZE);

        // Set up initial frontier: just the source vertex
        frontier_set(0, 0, (int64_t)g_source);
        frontier_tail[0].val = 1;

        std::atomic_thread_fence(std::memory_order_release);
        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0)
            hartsleep(10);
    }

    if (g_N == 0) {
        if (tid == 0) g_sim_exit = 1;
        while (g_sim_exit == 0) hartsleep(100);
        return 1;
    }

    const int N = g_N;

    // Vertex range for parallel stats reduction
    const int vtx_per_thr = (N + g_total_harts - 1) / g_total_harts;
    const int v_lo = tid * vtx_per_thr;
    const int v_hi = std::min(v_lo + vtx_per_thr, N);

    barrier();

    // ================================================================
    // BFS Loop
    // ================================================================
    uint64_t t_bfs_start = cycle();
    int32_t level = 0;

    while (true) {
        // Hart 0: distribute frontier into L1SP queues, overflow to L2SP
        if (tid == 0) {
            // Reset the next buffer's tail for this level's discoveries
            frontier_tail[cur_buf ^ 1].val = 0;
            // Distribute current frontier into per-core L1SP queues
            distribute_frontier();
        }
        barrier();

        int64_t total_work = frontier_tail[cur_buf].val;
        if (total_work == 0 || g_frontier_error) break;

        if (tid == 0) {
            std::printf("Level %d: frontier=%ld\n", level, (long)total_work);
        }

        // Process level
        ph_stat_phase(1);
        process_bfs_level(tid, level);
        ph_stat_phase(0);

        barrier();

        // Swap buffers (no data copy needed)
        if (tid == 0) {
            cur_buf ^= 1;
        }

        barrier();
        level++;
    }

    uint64_t t_bfs_end = cycle();

    // ================================================================
    // Parallel stats reduction
    // ================================================================
    ph_stat_phase(1);

    int64_t local_sum = 0;
    int32_t local_cnt = 0;
    int32_t local_max = 0;

    for (int v = v_lo; v < v_hi; v++) {
        int d = g_dist[v];
        if (d >= 0) {
            local_cnt++;
            local_sum += d;
            if (d > local_max) local_max = d;
        }
    }

    atomic_fetch_add_i64(&g_sum_dist, local_sum);
    atomic_fetch_add_i32(&g_reached, local_cnt);

    int32_t old_max = g_max_dist;
    while (local_max > old_max) {
        int32_t prev = atomic_compare_and_swap_i32(&g_max_dist, old_max, local_max);
        if (prev == old_max) break;
        old_max = prev;
    }

    ph_stat_phase(0);
    barrier();

    // ================================================================
    // Results (hart 0)
    // ================================================================
    if (tid == 0) {
        uint64_t bfs_cycles = t_bfs_end - t_bfs_start;

        std::printf("\n=== BFS Complete ===\n");
        std::printf("Levels: %d (%llu cycles)\n", level, (unsigned long long)bfs_cycles);

        int pct = g_reached > 0 ? (int)(100LL * g_reached / N) : 0;
        std::printf("Reached: %d/%d (%d%%)\n", (int)g_reached, N, pct);
        long long avg_x10 = g_reached > 0 ? (10LL * g_sum_dist / g_reached) : 0;
        std::printf("max_dist=%d  sum_dist=%lld  avg_dist=%lld.%lld\n",
                    (int)g_max_dist, (long long)g_sum_dist,
                    avg_x10 / 10, avg_x10 % 10);

        bool ok = true;
        if (g_dist[g_source] != 0) {
            std::printf("FAIL: dist[%d]=%d (expected 0)\n", g_source, g_dist[g_source]);
            ok = false;
        }
        if (g_reached < 1) {
            std::printf("FAIL: reached=%d (expected >= 1)\n", (int)g_reached);
            ok = false;
        }
        std::printf(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");

        // Per-core work balance
        int64_t total_processed = 0;
        int64_t total_edges_done = 0;
        int64_t total_steals = 0;
        int64_t min_nodes = 0x7FFFFFFFFFFFFFFFLL;
        int64_t max_nodes = 0;

        std::printf("\nPer-core statistics:\n");
        std::printf("Core | Processed |     Edges | Steals\n");
        std::printf("-----|-----------|-----------|-------\n");

        for (int c = 0; c < g_total_cores; c++) {
            int64_t cp = stat_nodes_per_core[c];
            int64_t ce = stat_edges_per_core[c];
            int64_t cs = stat_steals_per_core[c];
            total_processed += cp;
            total_edges_done += ce;
            total_steals += cs;
            if (cp < min_nodes) min_nodes = cp;
            if (cp > max_nodes) max_nodes = cp;
            std::printf("%4d | %9ld | %9ld | %5ld\n", c, (long)cp, (long)ce, (long)cs);
        }

        int64_t avg = (g_total_cores > 0) ? (total_processed / g_total_cores) : 0;
        int64_t imbalance_pct = (max_nodes > 0)
            ? ((max_nodes - min_nodes) * 100 / max_nodes) : 0;

        std::printf("\nSummary:\n");
        std::printf("  Total nodes processed: %ld\n", (long)total_processed);
        std::printf("  Total edges traversed: %ld\n", (long)total_edges_done);
        std::printf("  Total L1SP steals:     %ld\n", (long)total_steals);
        std::printf("  Average per core:      %ld nodes\n", (long)avg);
        std::printf("  Min/Max per core:      %ld / %ld\n", (long)min_nodes, (long)max_nodes);
        std::printf("  Imbalance (max-min)/max: %ld%%\n", (long)imbalance_pct);
        if (total_processed > 0) {
            std::printf("  Cycles per node:       %lu\n",
                        (unsigned long)(bfs_cycles / total_processed));
        }

        // Memory tier utilization
        std::printf("\nFrontier tier utilization:\n");
        std::printf("  L2SP: %ld / %ld entries\n",
                    (long)std::min((int64_t)g_reached, frontier_l2sp_cap),
                    (long)frontier_l2sp_cap);
        if (frontier_l1sp_cap > 0) {
            std::printf("  L1SP: up to %ld entries (%d/core)\n",
                        (long)frontier_l1sp_cap, frontier_l1sp_per_core);
        }
        int64_t dram_entries = frontier_total_cap - frontier_l2sp_cap - frontier_l1sp_cap;
        if (dram_entries > 0) {
            std::printf("  DRAM: up to %ld entries\n", (long)dram_entries);
        }

        std::free(g_dist);
        std::free(g_file_buffer);
        std::free(frontier_dram[0]);
        std::free(frontier_dram[1]);

        std::printf("\nBFS complete, signaling exit.\n");
        std::fflush(stdout);
        g_sim_exit = 1;
    }

    while (g_sim_exit == 0) hartsleep(100);
    return 0;
}
