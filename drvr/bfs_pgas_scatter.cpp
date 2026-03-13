// bfs_pgas_scatter.cpp
// Single-PXN BFS with PGAS-based dist[] scatter across DRAM banks.
// Over-decomposes vertices into num_vgids virtual groups, each placed
// at a different DRAM offset via offset_base so traffic spreads across
// DRAM channels. Designed for power-law (RMAT) graphs.
//
// Compare num_vgids=1 (baseline, no scatter) vs higher values to see
// the effect of distributing hub vertex traffic across DRAM banks.
//
// Memory layout:
//   DRAM  – CSR graph (offsets+edges), dist[] arrays (one per VGID)
//   L2SP  – frontier bitmaps (2x), barrier, control variables

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

static const int MAX_VGIDS = 256;

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

// ---------- PGAS virtual address ----------
// VSID=1 (3 bits at 61:59), VGID (8 bits at 55:48), offset (48 bits at 47:0)
static inline uint64_t pgas_addr(int vgid, uint64_t offset)
{
    return (3ULL << 62)
         | (1ULL << 59)
         | (((uint64_t)vgid & 0xFF) << 48)
         | (offset & ((1ULL << 48) - 1));
}

// Get DRAM offset from a local (relative) DRAM pointer
static inline uint64_t dram_offset_of(const void *ptr)
{
    return ph_address_relative_dram_offset((uintptr_t)ptr);
}

// ---------- Barrier ----------
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

// ---------- L2SP globals ----------
__l2sp__ int32_t g_N;                  // total vertices
__l2sp__ int32_t g_E;                  // total edges
__l2sp__ int32_t g_total_threads;
__l2sp__ int32_t g_bitmap_words;
__l2sp__ int32_t g_num_vgids;          // over-decomposition count
__l2sp__ int32_t g_group_size;         // vertices per VGID = N / num_vgids

__l2sp__ volatile int g_bfs_done;
__l2sp__ volatile int g_sim_exit;
__l2sp__ int32_t g_bfs_iters;
__l2sp__ volatile int32_t g_any_work;

__l2sp__ volatile int64_t g_sum_dist;
__l2sp__ volatile int32_t g_reached;
__l2sp__ volatile int32_t g_max_dist;

__l2sp__ int32_t *g_csr_offsets;
__l2sp__ int32_t *g_csr_edges;
__l2sp__ int32_t  g_source;

// dist[] arrays — one per VGID, stored in L2SP for fast pointer lookup
__l2sp__ int32_t *g_dist_ptrs[MAX_VGIDS];  // local pointers for init/verify
__l2sp__ uint64_t g_dist_offsets[MAX_VGIDS]; // DRAM offsets for PTT programming

__l2sp__ volatile int32_t *g_frontier;
__l2sp__ volatile int32_t *g_next_frontier;

extern "C" char l2sp_end[];

// ---------- Main ----------
extern "C" int main(int argc, char **argv)
{
    int scale     = 0;     // if >0, N = 1<<scale
    int ef        = 16;    // edge factor (for file size estimate)
    int num_vgids = 1;     // VGID count for scatter
    int arg_N     = 0;     // explicit N (if scale not used)
    int arg_E     = 0;     // explicit E

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = parse_i(argv[++i], scale);
        else if (!strcmp(argv[i], "--ef") && i + 1 < argc)
            ef = parse_i(argv[++i], ef);
        else if (!strcmp(argv[i], "--N") && i + 1 < argc)
            arg_N = parse_i(argv[++i], arg_N);
        else if (!strcmp(argv[i], "--E") && i + 1 < argc)
            arg_E = parse_i(argv[++i], arg_E);
        else if (!strcmp(argv[i], "--vgids") && i + 1 < argc)
            num_vgids = parse_i(argv[++i], num_vgids);
    }

    // ---- Hardware topology ----
    const int hart_in_core   = myThreadId();
    const int core_in_pod    = myCoreId();
    const int pod_in_pxn     = myPodId();
    const int pxn_id         = myPXNId();
    const int harts_per_core = myCoreThreads();
    const int cores_per_pod  = numPodCores();

    const int tid = core_in_pod * harts_per_core + hart_in_core;
    const int threads_per_pod = cores_per_pod * harts_per_core;
    const int total_threads = threads_per_pod;

    // Park non-participating threads
    if (pxn_id != 0 || pod_in_pxn != 0 || tid >= total_threads) {
        while (g_sim_exit == 0) hartsleep(1000);
        return 0;
    }

    barrier_ref barrier(&g_barrier_data);

    if (tid == 0) barrier.num_threads() = total_threads;
    while (barrier.num_threads() == 0) wait(10);

    // ================================================================
    // Phase 0: tid 0 loads graph and sets up PGAS scatter
    // ================================================================
    barrier.sync([&]() {
        // Determine N and max E for buffer sizing
        int N;
        int64_t E_max;
        if (scale > 0) {
            N = 1 << scale;
            E_max = (int64_t)ef * N * 2;  // upper bound (undirected, before dedup)
        } else if (arg_N > 0 && arg_E > 0) {
            N = arg_N;
            E_max = arg_E;
        } else {
            std::printf("ERROR: specify --scale/--ef or --N/--E\n");
            g_N = 0;
            return;
        }

        if (num_vgids < 1) num_vgids = 1;
        if (num_vgids > MAX_VGIDS) num_vgids = MAX_VGIDS;
        // Round group_size UP so all N vertices are covered
        int group_size = (N + num_vgids - 1) / num_vgids;

        // Allocate file buffer (upper bound)
        size_t file_size = 20
            + (size_t)(N + 1) * sizeof(int32_t)
            + (size_t)E_max * sizeof(int32_t)
            + (size_t)N * sizeof(int32_t);

        char *file_buffer = (char *)std::malloc(file_size);
        if (!file_buffer) {
            std::printf("ERROR: malloc file buffer (%lu)\n", (unsigned long)file_size);
            g_N = 0;
            return;
        }

        // Bulk load
        char fname[] = "graph.bin";
        {
            struct ph_bulk_load_desc desc;
            desc.filename_addr = (long)fname;
            desc.dest_addr     = (long)file_buffer;
            desc.size          = (long)file_size;
            desc.result        = 0;
            ph_bulk_load_file(&desc);

            if (desc.result <= 0) {
                std::printf("ERROR: bulk load failed (result=%ld)\n", desc.result);
                std::free(file_buffer);
                g_N = 0;
                return;
            }
        }

        // Parse header
        int32_t *header = (int32_t *)file_buffer;
        int hdr_N = header[0];
        int hdr_E = header[1];
        int hdr_source = header[4];

        if (hdr_N != N) {
            std::printf("ERROR: N mismatch file=%d expected=%d\n", hdr_N, N);
            std::free(file_buffer);
            g_N = 0;
            return;
        }

        int32_t *csr_offsets = (int32_t *)(file_buffer + 20);
        int32_t *csr_edges   = (int32_t *)(file_buffer + 20
                                + (size_t)(N + 1) * sizeof(int32_t));
        int E = hdr_E;

        // Allocate dist[] as a single pool with per-VGID stagger to
        // guarantee different HBM channel placement.
        // HBM: 64 channels, RoBaRaCoCh, 32B tx → channel stride = 2048B.
        // Each VGID's data is rounded up to the nearest multiple of
        // (channel_stride / num_vgids) so consecutive VGIDs start in
        // evenly-spaced channels across the 64-channel rotation.
        const size_t vgid_data_size = (size_t)group_size * sizeof(int32_t);
        const size_t channel_stride = 64 * 32;  // 2048B = full channel rotation
        // Stagger: round each VGID slot so its start offset differs by
        // an odd number of channel boundaries from the previous VGID.
        // Simplest: round up to channel_stride so data is naturally aligned,
        // then add per-VGID channel stagger of (g * channel_stride/num_vgids).
        const size_t vgid_aligned = ((vgid_data_size + channel_stride - 1)
                                     / channel_stride) * channel_stride;
        const size_t stagger = channel_stride / num_vgids;  // bytes between VGID starts
        size_t pool_size = vgid_aligned * num_vgids + stagger * (num_vgids - 1);
        int32_t *dist_pool = (int32_t *)std::malloc(pool_size);
        if (!dist_pool) {
            std::printf("ERROR: malloc dist pool (%lu)\n", (unsigned long)pool_size);
            g_N = 0;
            return;
        }
        uint64_t pool_dram_off = dram_offset_of(dist_pool);
        for (int g = 0; g < num_vgids; g++) {
            size_t off = vgid_aligned * g + stagger * g;
            g_dist_ptrs[g] = (int32_t *)((char *)dist_pool + off);
            g_dist_offsets[g] = pool_dram_off + off;
        }

        int bm_words = (N + 31) / 32;

        // Allocate L2SP bitmaps
        uintptr_t heap = ((uintptr_t)l2sp_end + 7) & ~(uintptr_t)7;
        g_frontier      = (volatile int32_t *)heap;
        heap += (size_t)bm_words * sizeof(int32_t);
        heap = (heap + 7) & ~(uintptr_t)7;
        g_next_frontier = (volatile int32_t *)heap;

        // Set L2SP globals
        g_N             = N;
        g_E             = E;
        g_total_threads = total_threads;
        g_bitmap_words  = bm_words;
        g_num_vgids     = num_vgids;
        g_group_size    = group_size;
        g_bfs_done      = 0;
        g_bfs_iters     = 0;
        g_sim_exit      = 0;
        g_any_work      = 0;
        g_sum_dist      = 0;
        g_reached       = 0;
        g_max_dist      = 0;
        g_source        = hdr_source;
        g_csr_offsets   = csr_offsets;
        g_csr_edges     = csr_edges;

        std::printf("PGAS Scatter BFS: N=%d E=%d num_vgids=%d group_size=%d source=%d\n",
                    N, E, num_vgids, group_size, hdr_source);
        std::printf("HW: cores=%d harts/core=%d threads=%d\n",
                    cores_per_pod, harts_per_core, total_threads);
        for (int g = 0; g < num_vgids; g++) {
            // Channel = (offset >> 5) & 63  (bits [10:5] in RoBaRaCoCh)
            int ch = (int)((g_dist_offsets[g] >> 5) & 63);
            std::printf("  VGID %d: offset_base=0x%lx  start_channel=%d\n",
                        g, (unsigned long)g_dist_offsets[g], ch);
        }
    });

    if (g_N == 0) {
        g_sim_exit = 1;
        return 1;
    }

    // ================================================================
    // Program PGAS translator on EVERY core (translator is per-core)
    // ================================================================
    const int nvgids      = g_num_vgids;

    if (hart_in_core == 0) {
        // Program SIT on this core
        struct ph_pgas_sit_desc sit;
        sit.index     = 0;
        sit.vsid      = 1;
        sit.vsid_bits = 3;
        sit.vgid_hi   = 55;
        sit.vgid_lo   = 48;
        ph_pgas_sit_write(&sit);

        // Program PTT on this core
        for (int g = 0; g < nvgids; g++) {
            struct ph_pgas_ptt_desc ptt;
            ptt.sit_index   = 0;
            ptt.vgid        = g;
            ptt.target_pxn  = 0;
            ptt.offset_base = g_dist_offsets[g];
            ph_pgas_ptt_write(&ptt);
        }
    }

    // Wait for all cores to finish programming their translators
    barrier.sync();

    // ================================================================
    // Phase 1: Parallel initialization
    // ================================================================
    const int N           = g_N;
    const int bm_words    = g_bitmap_words;
    const int source      = g_source;
    const int grp_size    = g_group_size;

    int32_t *const local_offsets         = g_csr_offsets;
    int32_t *const local_edges           = g_csr_edges;
    volatile int32_t *const local_frontier      = g_frontier;
    volatile int32_t *const local_next_frontier = g_next_frontier;

    // Work distribution
    const int vtx_per_thr = (N + total_threads - 1) / total_threads;
    const int v_lo = tid * vtx_per_thr;
    const int v_hi = std::min(v_lo + vtx_per_thr, N);

    const int words_per_thread = (bm_words + total_threads - 1) / total_threads;
    const int w_lo = tid * words_per_thread;
    const int w_hi = std::min(w_lo + words_per_thread, bm_words);

    // Init dist[] via PGAS (to exercise the scatter path)
    for (int v = v_lo; v < v_hi; ++v) {
        int vgid    = v / grp_size;
        int v_local = v % grp_size;
        volatile int32_t *p =
            (volatile int32_t *)pgas_addr(vgid, (uint64_t)v_local * sizeof(int32_t));
        *p = -1;
    }

    // Init frontier bitmaps
    for (int w = w_lo; w < w_hi; ++w) {
        local_frontier[w]      = 0;
        local_next_frontier[w] = 0;
    }

    barrier.sync();

    // Set BFS source
    barrier.sync([&]() {
        // Write dist[source] = 0 via PGAS
        int sg    = source / grp_size;
        int sl    = source % grp_size;
        volatile int32_t *sp =
            (volatile int32_t *)pgas_addr(sg, (uint64_t)sl * sizeof(int32_t));
        *sp = 0;
        local_frontier[source / 32] = 1 << (source % 32);
    });

    barrier.sync();

    std::printf("BFS from source %d\n", source);

    // ================================================================
    // Phase 2: BFS loop
    // ================================================================
    uint64_t t_bfs_start = cycle();

    while (true) {
        int cur_level = g_bfs_iters;

        // ---- Expand frontier ----
        ph_stat_phase(1);

        for (int w = w_lo; w < w_hi; ++w) {
            int32_t word = local_frontier[w];
            if (word == 0) continue;

            while (word != 0) {
                int bit = __builtin_ctz(word);
                word &= word - 1;

                int v = w * 32 + bit;
                if (v >= N) break;

                int edge_begin = local_offsets[v];
                int edge_end   = local_offsets[v + 1];

                for (int ei = edge_begin; ei < edge_end; ++ei) {
                    int u = local_edges[ei];

                    // Access dist[u] via PGAS virtual address
                    int u_vgid  = u / grp_size;
                    int u_local = u % grp_size;
                    volatile int32_t *dist_u =
                        (volatile int32_t *)pgas_addr(u_vgid,
                            (uint64_t)u_local * sizeof(int32_t));

                    if (atomic_compare_and_swap_i32(dist_u, -1, cur_level + 1) == -1) {
                        int wi = u / 32;
                        int32_t mask = 1 << (u % 32);
                        atomic_or_i32(&local_next_frontier[wi], mask);
                    }
                }
            }
        }

        ph_stat_phase(0);

        // ---- Swap frontiers, check work ----
        barrier.sync([&]() { g_any_work = 0; });

        int local_any = 0;
        for (int w = w_lo; w < w_hi; ++w) {
            int32_t val = local_next_frontier[w];
            local_frontier[w] = val;
            if (val) local_any = 1;
            local_next_frontier[w] = 0;
        }

        if (local_any)
            atomic_or_i32(&g_any_work, 1);

        barrier.sync([&]() {
            if (g_any_work) {
                g_bfs_iters++;
            } else {
                g_bfs_done = 1;
            }
        });

        if (g_bfs_done) break;
    }

    uint64_t t_bfs_end = cycle();

    // ================================================================
    // Phase 3: Reduction (read dist[] via PGAS)
    // ================================================================
    ph_stat_phase(1);

    int64_t my_sum = 0;
    int32_t my_cnt = 0;
    int32_t my_max = 0;

    for (int v = v_lo; v < v_hi; ++v) {
        int vgid    = v / grp_size;
        int v_local = v % grp_size;
        volatile int32_t *p =
            (volatile int32_t *)pgas_addr(vgid, (uint64_t)v_local * sizeof(int32_t));
        int d = *p;
        if (d >= 0) {
            my_cnt++;
            my_sum += d;
            if (d > my_max) my_max = d;
        }
    }

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

    // ================================================================
    // Phase 4: Results
    // ================================================================
    if (tid == 0) {
        uint64_t bfs_cycles = t_bfs_end - t_bfs_start;

        std::printf("BFS done in %d iterations (%llu cycles)\n",
                    g_bfs_iters, (unsigned long long)bfs_cycles);
        int pct = g_reached > 0 ? (int)(100LL * g_reached / N) : 0;
        std::printf("Reached: %d/%d (%d%%)\n", (int)g_reached, N, pct);
        long long avg_x10 = g_reached > 0 ? (10LL * g_sum_dist / g_reached) : 0;
        std::printf("max_dist=%d  sum_dist=%lld  avg_dist=%lld.%lld\n",
                    (int)g_max_dist, (long long)g_sum_dist,
                    avg_x10 / 10, avg_x10 % 10);

        // Sanity check source dist
        {
            int sg = source / grp_size;
            int sl = source % grp_size;
            volatile int32_t *sp =
                (volatile int32_t *)pgas_addr(sg, (uint64_t)sl * sizeof(int32_t));
            int sd = *sp;
            if (sd != 0) {
                std::printf("FAIL: dist[source=%d]=%d (expected 0)\n", source, sd);
            }
        }

        bool ok = (g_reached >= 1);
        std::printf(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");

        // Free dist pool (single allocation)
        std::free(g_dist_ptrs[0]);

        std::printf("BFS complete.\n");
        std::fflush(stdout);
        g_sim_exit = 1;
    }

    while (g_sim_exit == 0) hartsleep(100);
    return 0;
}
