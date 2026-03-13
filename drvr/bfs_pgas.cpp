// bfs_pgas.cpp
// Multi-PXN BFS using PGAS virtual address translation.
// Each PXN owns a contiguous vertex partition. Cross-PXN dist[] access
// and frontier exchange use PGAS virtual addresses.
//
// Memory layout per PXN:
//   DRAM  – CSR graph (full copy), dist[N_local], remote frontier exchange,
//           coordination struct (PXN 0 only, accessed via PGAS from others)
//   L2SP  – frontier bitmaps (2x), barrier, control variables
//
// PGAS setup:
//   SIT slot 0: VSID=1 (3 bits at 61:59), VGID 8 bits at 55:48
//   PTT: VGID k -> PXN k
//
// Run with >=2 PXNs, 1 pod/PXN, configurable cores/threads.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>
#include <pandohammer/mmio.h>
#include <pandohammer/address.h>
#include <pandohammer/staticdecl.h>

#ifndef DEFAULT_VTX_PER_THREAD
#define DEFAULT_VTX_PER_THREAD 1024
#endif
#ifndef DEFAULT_DEGREE
#define DEFAULT_DEGREE 16
#endif

static const int DO_FULL_VERIFY = 0;

#define __l2sp__ __attribute__((section(".l2sp")))

// ---------- Blocking atomic OR ----------
static inline int32_t atomic_or_i32(volatile int32_t *ptr, int32_t val)
{
    int32_t ret;
    asm volatile("amoor.w %0, %2, 0(%1)"
                 : "=r"(ret) : "r"(ptr), "r"(val) : "memory");
    return ret;
}

// ---------- Helpers ----------
static int parse_i(const char *s, int d)
{
    if (!s) return d;
    char *e = nullptr;
    long v = strtol(s, &e, 10);
    return (e && *e == 0) ? (int)v : d;
}

static inline void wait(volatile int x)
{
    for (int i = 0; i < x; i++)
        asm volatile("nop");
}

// ---------- PGAS virtual address construction ----------
// VSID=1, 3 bits at 61:59. VGID=target_pxn, 8 bits at 55:48. Offset at 47:0.
static inline uint64_t pgas_addr(int target_pxn, uint64_t dram_offset)
{
    return (3ULL << 62)
         | (1ULL << 59)
         | (((uint64_t)target_pxn & 0xFF) << 48)
         | (dram_offset & ((1ULL << 48) - 1));
}

// Get DRAM offset from a local (relative) DRAM pointer
static inline uint64_t dram_offset_of(const void *ptr)
{
    return ph_address_relative_dram_offset((uintptr_t)ptr);
}

// ---------- Intra-PXN Barrier (L2SP) ----------
struct barrier_data {
    int count;
    int signal;
    int num_threads;
};

__l2sp__ barrier_data g_barrier_data = {0, 0, 0};

class barrier_ref {
public:
    barrier_ref(barrier_data *ptr) : ptr_(ptr) {}
    barrier_data *ptr_;

    int &count()       { return ptr_->count; }
    int &signal()      { return ptr_->signal; }
    int &num_threads() { return ptr_->num_threads; }

    void sync() { sync([](){}); }

    template <typename F>
    void sync(F f) {
        int signal_ = signal();
        int count_  = atomic_fetch_add_i32(&count(), 1);
        if (count_ == num_threads() - 1) {
            count() = 0;
            f();
            signal() = !signal_;
        } else {
            static constexpr int backoff_limit = 1000;
            int backoff_counter = 8;
            while (signal() == signal_) {
                wait(backoff_counter);
                backoff_counter = std::min(backoff_counter * 2, backoff_limit);
            }
        }
    }
};

// ---------- Inter-PXN coordination struct (lives in DRAM on each PXN) ----------
// Only PXN 0's copy is used for coordination; accessed via PGAS from remote PXNs.
struct alignas(8) pxn_coord {
    volatile int64_t barrier_count;   // offset 0
    volatile int64_t barrier_phase;   // offset 8
    volatile int32_t global_done;     // offset 16
    volatile int32_t global_any_work; // offset 20
    volatile int32_t global_bfs_iters;// offset 24
    volatile int32_t pad0;            // offset 28
    volatile int64_t global_sum_dist; // offset 32
    volatile int32_t global_reached;  // offset 40
    volatile int32_t global_max_dist; // offset 44
    volatile int32_t global_sim_exit; // offset 48
    volatile int32_t pad1;            // offset 52
};

static pxn_coord g_coord;
static volatile int32_t g_pxn_exit = 0;  // per-PXN exit flag for parked threads

// ---------- DRAM arrays ----------
static char    *g_file_buffer = nullptr;
static int32_t *g_csr_offsets_dram = nullptr;
static int32_t *g_csr_edges_dram = nullptr;
static int32_t *g_dist_dram = nullptr;         // dist[N_local] local partition
static volatile int32_t *g_rf_buf = nullptr;   // remote frontier [num_pxn * bm_words]

static volatile int32_t g_init_ok = 0;        // 1 = success, -1 = error

// ---------- L2SP globals ----------
__l2sp__ int32_t g_N;
__l2sp__ int32_t g_N_local;
__l2sp__ int32_t g_vtx_offset;
__l2sp__ int32_t g_degree;
__l2sp__ int32_t g_total_threads;
__l2sp__ int32_t g_bitmap_words;
__l2sp__ int32_t g_my_pxn;
__l2sp__ int32_t g_num_pxn;
__l2sp__ int32_t g_source;

__l2sp__ volatile int g_bfs_done;
__l2sp__ volatile int g_sim_exit;
__l2sp__ volatile int32_t g_any_work;

__l2sp__ volatile int64_t g_sum_dist;
__l2sp__ volatile int32_t g_reached;
__l2sp__ volatile int32_t g_max_dist;

// Cached DRAM pointers in L2SP
__l2sp__ int32_t *g_csr_offsets;
__l2sp__ int32_t *g_csr_edges;
__l2sp__ int32_t *g_dist;

// Frontier bitmaps (L2SP heap)
__l2sp__ volatile int32_t *g_frontier;
__l2sp__ volatile int32_t *g_next_frontier;

// DRAM offsets for PGAS (same on all PXNs due to identical malloc order)
__l2sp__ uint64_t g_dist_dram_offset;
__l2sp__ uint64_t g_rf_dram_offset;
__l2sp__ uint64_t g_coord_dram_offset;

extern "C" char l2sp_end[];

// ---------- Inter-PXN barrier ----------
static int64_t g_local_barrier_phase = 0;

static inline void inter_pxn_barrier(int my_pxn, int num_pxn)
{
    uint64_t coord_off = g_coord_dram_offset;
    uint64_t count_off = coord_off + __builtin_offsetof(pxn_coord, barrier_count);
    uint64_t phase_off = coord_off + __builtin_offsetof(pxn_coord, barrier_phase);

    volatile int64_t *count_ptr = (volatile int64_t *)pgas_addr(0, count_off);
    volatile int64_t *phase_ptr = (volatile int64_t *)pgas_addr(0, phase_off);

    int64_t my_phase = g_local_barrier_phase;

    int64_t old = atomic_fetch_add_i64(count_ptr, 1);
    if (old == num_pxn - 1) {
        atomic_fetch_add_i64(count_ptr, -num_pxn);
        atomic_fetch_add_i64(phase_ptr, 1);
    } else {
        long w = 1, wmax = 8 * 1024;
        while (atomic_load_i64(phase_ptr) == my_phase) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }
    g_local_barrier_phase = my_phase + 1;
}

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
    }

    // ---- Hardware topology ----
    const int hart_in_core   = myThreadId();
    const int core_in_pod    = myCoreId();
    const int pod_in_pxn     = myPodId();
    const int pxn_id         = myPXNId();
    const int harts_per_core = myCoreThreads();
    const int cores_per_pod  = numPodCores();
    const int num_pxn        = numPXN();

    const int tid = core_in_pod * harts_per_core + hart_in_core;
    const int threads_per_pod = cores_per_pod * harts_per_core;
    const int total_threads = threads_per_pod;

    // Park non-pod-0 threads
    if (pod_in_pxn != 0) {
        while (g_pxn_exit == 0) hartsleep(1000);
        return 0;
    }

    // Park excess threads (shouldn't happen if config matches)
    if (tid >= total_threads) {
        while (g_pxn_exit == 0) hartsleep(1000);
        return 0;
    }

    // Vertex partitioning
    const int N_local = vtx_per_thread * total_threads;
    const int N_total = N_local * num_pxn;
    const int bm_words_local = (N_local + 31) / 32;

    barrier_ref barrier(&g_barrier_data);

    // ================================================================
    // Phase 0a: tid 0 on each PXN — bulk-load graph, allocate arrays
    // ================================================================
    if (tid == 0) {
        const int64_t total_edges = (int64_t)N_total * degree;
        size_t file_size = 20
            + (size_t)(N_total + 1) * sizeof(int32_t)
            + (size_t)total_edges * sizeof(int32_t)
            + (size_t)N_total * sizeof(int32_t);

        g_file_buffer = (char *)std::malloc(file_size);
        if (!g_file_buffer) {
            std::printf("PXN%d: ERROR malloc file buffer (%lu)\n", pxn_id,
                        (unsigned long)file_size);
            g_init_ok = -1;
            goto init_done;
        }

        // Bulk load graph
        char fname[] = "uniform_graph.bin";
        {
            struct ph_bulk_load_desc desc;
            desc.filename_addr = (long)fname;
            desc.dest_addr     = (long)g_file_buffer;
            desc.size          = (long)file_size;
            desc.result        = 0;
            ph_bulk_load_file(&desc);

            if (desc.result <= 0) {
                std::printf("PXN%d: ERROR bulk load (result=%ld)\n", pxn_id, desc.result);
                std::free(g_file_buffer);
                g_init_ok = -1;
                goto init_done;
            }
        }

        // Parse header
        {
            int32_t *h = (int32_t *)g_file_buffer;
            if (h[0] != N_total || h[2] != degree) {
                std::printf("PXN%d: ERROR graph mismatch N=%d!=%d D=%d!=%d\n",
                            pxn_id, h[0], N_total, h[2], degree);
                std::free(g_file_buffer);
                g_init_ok = -1;
                goto init_done;
            }
            g_csr_offsets_dram = (int32_t *)(g_file_buffer + 20);
            g_csr_edges_dram   = (int32_t *)(g_file_buffer + 20
                                  + (size_t)(N_total + 1) * sizeof(int32_t));
        }

        // Allocate dist[N_local] (only this PXN's partition)
        g_dist_dram = (int32_t *)std::malloc((size_t)N_local * sizeof(int32_t));
        if (!g_dist_dram) {
            std::printf("PXN%d: ERROR malloc dist\n", pxn_id);
            std::free(g_file_buffer);
            g_init_ok = -1;
            goto init_done;
        }

        // Allocate remote frontier exchange buffer
        {
            size_t rf_size = (size_t)num_pxn * bm_words_local * sizeof(int32_t);
            g_rf_buf = (volatile int32_t *)std::malloc(rf_size);
            if (!g_rf_buf) {
                std::printf("PXN%d: ERROR malloc rf_buf\n", pxn_id);
                std::free(g_dist_dram);
                std::free(g_file_buffer);
                g_init_ok = -1;
                goto init_done;
            }
            std::memset((void *)g_rf_buf, 0, rf_size);
        }

        // Initialize coordination struct (PXN 0 only — others leave theirs zeroed)
        if (pxn_id == 0) {
            std::memset((void *)&g_coord, 0, sizeof(g_coord));
        }

        // Compute DRAM offsets for PGAS addressing
        uint64_t dist_off  = dram_offset_of(g_dist_dram);
        uint64_t rf_off    = dram_offset_of((void *)g_rf_buf);
        uint64_t coord_off = dram_offset_of(&g_coord);

        // NOTE: SIT/PTT programming moved below — must be done per-core
        // since each RISCVCore has its own PGASTranslator.

        // Set up L2SP parameters
        int source = ((int32_t *)g_file_buffer)[4];

        barrier.num_threads() = total_threads;
        g_N              = N_total;
        g_N_local        = N_local;
        g_vtx_offset     = pxn_id * N_local;
        g_degree         = degree;
        g_total_threads  = total_threads;
        g_bitmap_words   = bm_words_local;
        g_my_pxn         = pxn_id;
        g_num_pxn        = num_pxn;
        g_source         = source;
        g_bfs_done       = 0;
        g_sim_exit       = 0;
        g_any_work       = 0;
        g_sum_dist       = 0;
        g_reached        = 0;
        g_max_dist       = 0;
        g_local_barrier_phase = 0;

        g_csr_offsets     = g_csr_offsets_dram;
        g_csr_edges       = g_csr_edges_dram;
        g_dist            = g_dist_dram;
        g_dist_dram_offset = dist_off;
        g_rf_dram_offset   = rf_off;
        g_coord_dram_offset = coord_off;

        // Allocate L2SP frontier bitmaps
        uintptr_t heap = ((uintptr_t)l2sp_end + 7) & ~(uintptr_t)7;
        g_frontier      = (volatile int32_t *)heap;
        heap += (size_t)bm_words_local * sizeof(int32_t);
        heap = (heap + 7) & ~(uintptr_t)7;
        g_next_frontier = (volatile int32_t *)heap;

        if (pxn_id == 0) {
            std::printf("PGAS BFS: N=%d (%d/PXN), %d PXNs, degree=%d, source=%d\n",
                        N_total, N_local, num_pxn, degree, source);
            std::printf("HW: cores/pod=%d harts/core=%d threads/PXN=%d\n",
                        cores_per_pod, harts_per_core, total_threads);
            std::printf("dist_dram_offset=0x%lx rf_dram_offset=0x%lx coord_dram_offset=0x%lx\n",
                        (unsigned long)dist_off, (unsigned long)rf_off, (unsigned long)coord_off);
        }

        g_init_ok = 1;
    }

init_done:

    // Wait for tid 0 setup
    while (g_init_ok == 0) wait(100);

    if (g_init_ok < 0) {
        if (tid == 0) g_pxn_exit = 1;
        while (g_pxn_exit == 0) hartsleep(100);
        return 1;
    }

    while (barrier.num_threads() == 0) wait(10);

    // ================================================================
    // Program PGAS translator on EVERY core (translator is per-core)
    // ================================================================
    if (hart_in_core == 0) {
        struct ph_pgas_sit_desc sit;
        sit.index     = 0;
        sit.vsid      = 1;
        sit.vsid_bits = 3;
        sit.vgid_hi   = 55;
        sit.vgid_lo   = 48;
        ph_pgas_sit_write(&sit);

        for (int p = 0; p < num_pxn; p++) {
            struct ph_pgas_ptt_desc ptt;
            ptt.sit_index   = 0;
            ptt.vgid        = p;
            ptt.target_pxn  = p;
            ptt.offset_base = 0;
            ph_pgas_ptt_write(&ptt);
        }
    }

    barrier.sync();

    // ================================================================
    // Phase 1: Parallel initialization
    // ================================================================
    const int N_total_val = g_N;
    const int N_local_val = g_N_local;
    const int vtx_off     = g_vtx_offset;
    const int bm_words    = g_bitmap_words;
    const int source      = g_source;
    const int my_pxn      = g_my_pxn;
    const int npxn        = g_num_pxn;

    int32_t *const local_offsets         = g_csr_offsets;
    int32_t *const local_edges           = g_csr_edges;
    int32_t *const local_dist            = g_dist;
    volatile int32_t *const local_frontier      = g_frontier;
    volatile int32_t *const local_next_frontier = g_next_frontier;

    const uint64_t dist_doff = g_dist_dram_offset;
    const uint64_t rf_doff   = g_rf_dram_offset;

    // Work distribution
    const int vtx_per_thr = (N_local_val + total_threads - 1) / total_threads;
    const int v_lo = tid * vtx_per_thr;
    const int v_hi = std::min(v_lo + vtx_per_thr, N_local_val);

    const int words_per_thread = (bm_words + total_threads - 1) / total_threads;
    const int w_lo = tid * words_per_thread;
    const int w_hi = std::min(w_lo + words_per_thread, bm_words);

    // Init dist[] for this PXN's partition
    for (int vl = v_lo; vl < v_hi; ++vl)
        local_dist[vl] = -1;

    // Init frontier bitmaps
    for (int w = w_lo; w < w_hi; ++w) {
        local_frontier[w]      = 0;
        local_next_frontier[w] = 0;
    }

    barrier.sync();

    // Set BFS source
    int source_pxn = source / N_local_val;
    barrier.sync([&]() {
        if (my_pxn == source_pxn) {
            int source_local = source - vtx_off;
            local_dist[source_local] = 0;
            local_frontier[source_local / 32] = 1 << (source_local % 32);
        }
    });

    // Inter-PXN barrier: all PXNs initialized
    if (tid == 0)
        inter_pxn_barrier(my_pxn, npxn);
    barrier.sync();

    if (my_pxn == 0 && tid == 0)
        std::printf("BFS from source %d (PXN %d)\n", source, source_pxn);

    // ================================================================
    // Phase 2: Main BFS loop (level-synchronous, multi-PXN)
    // ================================================================
    uint64_t t_bfs_start = cycle();

    // Get iteration counter via PGAS to PXN 0
    uint64_t coord_off = g_coord_dram_offset;

    while (true) {
        // Read current iteration from PXN 0's coordination struct
        volatile int32_t *iters_ptr = (volatile int32_t *)pgas_addr(0,
            coord_off + __builtin_offsetof(pxn_coord, global_bfs_iters));
        int cur_level = *iters_ptr;

        // ---- Step 1: Expand frontier ----
        ph_stat_phase(1);

        for (int w = w_lo; w < w_hi; ++w) {
            int32_t word = local_frontier[w];
            if (word == 0) continue;

            while (word != 0) {
                int bit = __builtin_ctz(word);
                word &= word - 1;

                int vl = w * 32 + bit;
                if (vl >= N_local_val) break;

                int v = vtx_off + vl;
                int edge_begin = local_offsets[v];
                int edge_end   = local_offsets[v + 1];

                for (int ei = edge_begin; ei < edge_end; ++ei) {
                    int u = local_edges[ei];  // global neighbor

                    int target_pxn = u / N_local_val;
                    int u_local    = u - target_pxn * N_local_val;

                    // CAS dist[u_local] on target PXN via PGAS
                    uint64_t dist_addr_off = dist_doff + (uint64_t)u_local * sizeof(int32_t);
                    volatile int32_t *dist_ptr =
                        (volatile int32_t *)pgas_addr(target_pxn, dist_addr_off);

                    if (atomic_compare_and_swap_i32(dist_ptr, -1, cur_level + 1) == -1) {
                        // Discovered u — update frontier
                        int wi = u_local / 32;
                        int32_t mask = 1 << (u_local % 32);

                        if (target_pxn == my_pxn) {
                            // Local: atomic OR into L2SP next_frontier
                            atomic_or_i32(&local_next_frontier[wi], mask);
                        } else {
                            // Remote: posted atomic OR into target PXN's exchange buffer
                            // Target PXN's rf_buf layout: [src_pxn * bm_words + wi]
                            uint64_t rf_word_off = rf_doff
                                + ((uint64_t)my_pxn * bm_words + wi) * sizeof(int32_t);
                            volatile int32_t *rf_ptr =
                                (volatile int32_t *)pgas_addr(target_pxn, rf_word_off);
                            atomic_or_i32_posted(rf_ptr, mask);
                        }
                    }
                }
            }
        }

        ph_stat_phase(0);

        // ---- Step 2: Intra-PXN barrier ----
        barrier.sync();

        // ---- Step 3: Inter-PXN barrier ----
        if (tid == 0)
            inter_pxn_barrier(my_pxn, npxn);
        barrier.sync();

        // ---- Step 4: Merge remote frontier from all source PXNs ----
        // Each source PXN wrote into our rf_buf[src * bm_words .. (src+1) * bm_words)
        {
            volatile int32_t *rf_base = g_rf_buf;
            for (int src = 0; src < npxn; ++src) {
                if (src == my_pxn) continue;
                volatile int32_t *src_buf = rf_base + (int64_t)src * bm_words;
                for (int w = w_lo; w < w_hi; ++w) {
                    int32_t remote_word = src_buf[w];
                    if (remote_word != 0) {
                        atomic_or_i32(&local_next_frontier[w], remote_word);
                        src_buf[w] = 0;
                    }
                }
            }
        }

        // ---- Step 5: Intra-PXN barrier ----
        barrier.sync([&]() { g_any_work = 0; });

        // ---- Step 6: Swap frontiers, check local work ----
        int local_any = 0;
        for (int w = w_lo; w < w_hi; ++w) {
            int32_t val = local_next_frontier[w];
            local_frontier[w] = val;
            if (val) local_any = 1;
            local_next_frontier[w] = 0;
        }

        if (local_any)
            atomic_or_i32(&g_any_work, 1);

        barrier.sync();

        // ---- Step 7: PXN leaders coordinate global done via PGAS ----
        if (tid == 0) {
            // Report this PXN's work status to PXN 0
            if (g_any_work) {
                volatile int32_t *any_work_ptr = (volatile int32_t *)pgas_addr(0,
                    coord_off + __builtin_offsetof(pxn_coord, global_any_work));
                atomic_or_i32(any_work_ptr, 1);
            }

            inter_pxn_barrier(my_pxn, npxn);

            // PXN 0 decides if BFS is done
            if (my_pxn == 0) {
                if (g_coord.global_any_work) {
                    g_coord.global_bfs_iters++;
                    g_coord.global_any_work = 0;
                } else {
                    g_coord.global_done = 1;
                }
            }

            inter_pxn_barrier(my_pxn, npxn);

            // Read global done from PXN 0
            volatile int32_t *done_ptr = (volatile int32_t *)pgas_addr(0,
                coord_off + __builtin_offsetof(pxn_coord, global_done));
            g_bfs_done = *done_ptr;
        }

        // ---- Step 8: Propagate done ----
        barrier.sync();

        if (g_bfs_done) break;
    }

    uint64_t t_bfs_end = cycle();

    // ================================================================
    // Phase 3: Parallel reduction
    // ================================================================
    ph_stat_phase(1);

    int64_t my_sum = 0;
    int32_t my_cnt = 0;
    int32_t my_max = 0;

    for (int vl = v_lo; vl < v_hi; ++vl) {
        int d = local_dist[vl];
        if (d >= 0) {
            my_cnt++;
            my_sum += d;
            if (d > my_max) my_max = d;
        }
    }

    // Intra-PXN reduction (L2SP atomics)
    atomic_fetch_add_i64(&g_sum_dist, my_sum);
    atomic_fetch_add_i32(&g_reached, my_cnt);
    {
        int32_t old_max = g_max_dist;
        while (my_max > old_max) {
            int32_t prev = atomic_compare_and_swap_i32(&g_max_dist, old_max, my_max);
            if (prev == old_max) break;
            old_max = prev;
        }
    }

    ph_stat_phase(0);
    barrier.sync();

    // PXN leader: reduce across PXNs via PGAS to PXN 0
    if (tid == 0) {
        volatile int64_t *gsum = (volatile int64_t *)pgas_addr(0,
            coord_off + __builtin_offsetof(pxn_coord, global_sum_dist));
        volatile int32_t *greached = (volatile int32_t *)pgas_addr(0,
            coord_off + __builtin_offsetof(pxn_coord, global_reached));
        volatile int32_t *gmax = (volatile int32_t *)pgas_addr(0,
            coord_off + __builtin_offsetof(pxn_coord, global_max_dist));

        atomic_fetch_add_i64(gsum, g_sum_dist);
        atomic_fetch_add_i32(greached, (int32_t)g_reached);

        int32_t pxn_max = g_max_dist;
        int32_t old_gmax = *gmax;
        while (pxn_max > old_gmax) {
            int32_t prev = atomic_compare_and_swap_i32(gmax, old_gmax, pxn_max);
            if (prev == old_gmax) break;
            old_gmax = prev;
        }

        inter_pxn_barrier(my_pxn, npxn);
    }
    barrier.sync();

    // ================================================================
    // Phase 4: Results (PXN 0, tid 0)
    // ================================================================
    if (my_pxn == 0 && tid == 0) {
        uint64_t bfs_cycles = t_bfs_end - t_bfs_start;
        int iters = g_coord.global_bfs_iters;
        int reached = g_coord.global_reached;
        int64_t sum = g_coord.global_sum_dist;
        int maxd = g_coord.global_max_dist;

        std::printf("BFS done in %d iterations (%llu cycles)\n",
                    iters, (unsigned long long)bfs_cycles);
        int pct = reached > 0 ? (int)(100LL * reached / N_total_val) : 0;
        std::printf("Reached: %d/%d (%d%%)\n", reached, N_total_val, pct);
        long long avg_x10 = reached > 0 ? (10LL * sum / reached) : 0;
        std::printf("max_dist=%d  sum_dist=%lld  avg_dist=%lld.%lld\n",
                    maxd, (long long)sum, avg_x10 / 10, avg_x10 % 10);

        bool ok = true;
        if (source_pxn == 0 && local_dist[source - vtx_off] != 0) {
            std::printf("FAIL: dist[source]=%d (expected 0)\n",
                        local_dist[source - vtx_off]);
            ok = false;
        }
        if (reached < 1) {
            std::printf("FAIL: reached=%d\n", reached);
            ok = false;
        }

        std::printf(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
        std::printf("BFS complete, signaling exit.\n");
        std::fflush(stdout);
    }

    // ---- Cleanup and exit ----
    if (tid == 0) {
        if (my_pxn == 0) {
            // Set global exit flag via local access (PXN 0 owns g_coord)
            g_coord.global_sim_exit = 1;
        } else {
            // Wait for PXN 0 to signal exit
            volatile int32_t *exit_ptr = (volatile int32_t *)pgas_addr(0,
                coord_off + __builtin_offsetof(pxn_coord, global_sim_exit));
            while (*exit_ptr == 0) hartsleep(100);
        }

        // Free local allocations
        std::free((void *)g_rf_buf);
        std::free(g_dist_dram);
        std::free(g_file_buffer);

        // Signal parked threads on this PXN
        g_pxn_exit = 1;
        g_sim_exit = 1;
    }

    while (g_sim_exit == 0) hartsleep(100);
    return 0;
}
