// gemm_int_l2sp.cpp
// Integer matrix multiply C = A * B with all matrices in L2SP.
// Designed to stress L2SP bandwidth: inner loop does 2 L2SP reads per multiply-add.
// No barriers during computation -- all threads work independently on different rows.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>
#include <pandohammer/mmio.h>
#include <pandohammer/staticdecl.h>

// ---------- Memory section attributes ----------
#define __l2sp__ __attribute__((section(".l2sp")))

// ---------- Helper functions ----------
static int parse_i(const char* s, int d) {
    if (!s) return d;
    char* e = nullptr;
    long v = strtol(s, &e, 10);
    return (e && *e == 0) ? (int)v : d;
}

static inline void wait(volatile int x) {
    for (int i = 0; i < x; i++) {
        asm volatile("nop");
    }
}

// ---------- Barrier (L2SP-based) ----------
struct barrier_data {
    int count;
    int signal;
    int num_threads;
};

__l2sp__ barrier_data g_barrier_data = {0, 0, 0};

class barrier_ref {
public:
    barrier_ref(barrier_data* ptr) : ptr_(ptr) {}
    barrier_data* ptr_;

    int& count() { return ptr_->count; }
    int& signal() { return ptr_->signal; }
    int& num_threads() { return ptr_->num_threads; }

    void sync() {
        sync([](){});
    }

    template <typename F>
    void sync(F f) {
        int signal_ = signal();
        int count_ = atomic_fetch_add_i32(&count(), 1);
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

// ---------- Shared state in L2SP ----------
__l2sp__ int g_N;

// Pointers to matrices allocated from L2SP heap
__l2sp__ int32_t* g_A;
__l2sp__ int32_t* g_B;
__l2sp__ int32_t* g_C;

__l2sp__ volatile int g_sim_exit;

// Linker symbol: first free byte in L2SP after static data
extern "C" char l2sp_end[];

// ---------- Main ----------
extern "C" int main(int argc, char** argv) {
    int N = 128;
    int desired_threads = 1024;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--N") && i + 1 < argc) N = parse_i(argv[++i], N);
        else if (!strcmp(argv[i], "--T") && i + 1 < argc) desired_threads = parse_i(argv[++i], desired_threads);
        else if (!strcmp(argv[i], "--help")) {
            std::printf("Usage: %s --N <dim> --T <threads>\n", argv[0]);
            return 0;
        }
    }

    const int hart_in_core   = myThreadId();
    const int core_in_pod    = myCoreId();
    const int pod_in_pxn     = myPodId();
    const int pxn_id         = myPXNId();
    const int harts_per_core = myCoreThreads();
    const int cores_per_pod  = numPodCores();
    const int pods_per_pxn   = numPXNPods();

    const int total_harts_hw =
        numPXN() * pods_per_pxn * cores_per_pod * harts_per_core;

    // Pod-local thread ID (L2SP is pod-local)
    const int tid_local = core_in_pod * harts_per_core + hart_in_core;
    const int threads_per_pod = cores_per_pod * harts_per_core;

    int total_threads = desired_threads;
    if (total_threads > threads_per_pod) total_threads = threads_per_pod;

    // Park non-participating threads
    if (tid_local >= total_threads) {
        while (g_sim_exit == 0) hartsleep(1000);
        return 0;
    }

    // Only run on pod 0, pxn 0
    if (pxn_id != 0 || pod_in_pxn != 0) {
        while (g_sim_exit == 0) hartsleep(1000);
        return 0;
    }

    barrier_ref barrier(&g_barrier_data);

    if (tid_local == 0) {
        barrier.num_threads() = total_threads;
    }
    while (barrier.num_threads() == 0) {
        wait(10);
    }

    // Allocate matrices from L2SP heap (thread 0 only via barrier lambda)
    ph_stat_phase(0);
    barrier.sync([&]() {
        g_N = N;
        g_sim_exit = 0;

        uintptr_t heap = ((uintptr_t)l2sp_end + 7) & ~(uintptr_t)7;
        g_A = (int32_t*)heap;
        heap += (int64_t)N * N * sizeof(int32_t);
        heap = (heap + 7) & ~(uintptr_t)7;
        g_B = (int32_t*)heap;
        heap += (int64_t)N * N * sizeof(int32_t);
        heap = (heap + 7) & ~(uintptr_t)7;
        g_C = (int32_t*)heap;
        heap += (int64_t)N * N * sizeof(int32_t);

        uintptr_t l2sp_base = 0x20000000;
        if (heap - l2sp_base > podL2SPSize()) {
            std::printf("ERROR: N=%d needs %lu bytes, L2SP has %lu\n",
                        N, (unsigned long)(heap - l2sp_base), (unsigned long)podL2SPSize());
            g_N = 0;
        }
    });

    if (g_N == 0) {
        if (tid_local == 0) g_sim_exit = 1;
        while (g_sim_exit == 0) hartsleep(100);
        return 1;
    }

    if (tid_local == 0) {
        int64_t mat_bytes = (int64_t)g_N * g_N * 4;
        std::printf("GEMM_INT_L2SP: N=%d (%lld KB per matrix, %lld KB total)\n",
                    g_N, (long long)(mat_bytes / 1024), (long long)(3 * mat_bytes / 1024));
        std::printf("HW: cores/pod=%d harts/core=%d total_threads=%d\n",
                    cores_per_pod, harts_per_core, total_threads);
        std::printf("L2SP reads per output element: %d (2*N)\n", 2 * g_N);
        std::printf("Total L2SP reads for GEMM: %lld\n",
                    (long long)g_N * g_N * 2 * g_N);
    }

    // ---------- Initialize matrices (parallel) ----------
    ph_stat_phase(1);
    int size = g_N * g_N;
    for (int i = tid_local; i < size; i += total_threads) {
        int r = i / g_N;
        int c = i % g_N;
        g_A[i] = (int32_t)((r + c) % 128);
        g_B[i] = (int32_t)((r - c + 128) % 128);
        g_C[i] = 0;
    }
    ph_stat_phase(0);
    barrier.sync();

    // ---------- GEMM kernel: C = A * B (parallel by rows) ----------
    // Each thread computes rows row, row+total_threads, row+2*total_threads, ...
    // Inner loop: sum += A[row][k] * B[k][col]  →  2 L2SP reads per iteration
    ph_stat_phase(1);
    for (int row = tid_local; row < g_N; row += total_threads) {
        for (int col = 0; col < g_N; ++col) {
            int32_t sum = 0;
            for (int k = 0; k < g_N; ++k) {
                sum += g_A[row * g_N + k] * g_B[k * g_N + col];
            }
            g_C[row * g_N + col] = sum;
        }
    }
    ph_stat_phase(0);
    barrier.sync();

    // ---------- Verification (thread 0 only) ----------
    if (tid_local == 0) {
        std::printf("GEMM complete.\n");

        int error_count = 0;
        int max_print = 10;
        for (int r = 0; r < g_N; ++r) {
            for (int c = 0; c < g_N; ++c) {
                int32_t expected = 0;
                for (int k = 0; k < g_N; ++k) {
                    int32_t a = (int32_t)((r + k) % 128);
                    int32_t b = (int32_t)((k - c + 128) % 128);
                    expected += a * b;
                }
                int32_t actual = g_C[r * g_N + c];
                if (actual != expected) {
                    error_count++;
                    if (error_count <= max_print) {
                        std::printf("FAIL [%d,%d]: got %d exp %d\n",
                                    r, c, actual, expected);
                    }
                }
            }
        }

        if (error_count == 0) {
            int mid = g_N / 2;
            std::printf("C[%d][%d] = %d\n", mid, mid, g_C[mid * g_N + mid]);
            std::printf("PASS\n");
        } else {
            std::printf("FAIL: %d errors\n", error_count);
        }

        std::printf("Done, signaling exit.\n");
        std::fflush(stdout);
        g_sim_exit = 1;
    }

    while (g_sim_exit == 0) hartsleep(100);
    return 0;
}
