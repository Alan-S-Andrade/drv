// atomic_bw.cpp
// Scatter-add bandwidth microbenchmark: posted vs blocking atomics.
// Each hart does ITERS atomic adds to a shared array of counters in DRAM,
// distributing across N_COUNTERS bins (mimics histogram / PageRank updates).
//
// Compile with -DUSE_POSTED for posted (fire-and-forget) atomics.
// Default is blocking atomics.
//
// Build targets:
//   atomic_bw_blocking  (blocking, hart stalls on every add)
//   atomic_bw_posted    (posted, hart fires and moves on)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pandohammer/cpuinfo.h>
#include <pandohammer/mmio.h>
#include <pandohammer/atomic.h>

#define __l2sp__ __attribute__((section(".l2sp")))

#ifndef N_COUNTERS
#define N_COUNTERS 256
#endif

#ifndef ITERS
#define ITERS 512
#endif

// Counters live in L2SP
__l2sp__ volatile int64_t g_counters[N_COUNTERS];

// Two-phase barrier (all harts must wait)
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
            int backoff = 8;
            while (signal() == signal_) {
                for (int i = 0; i < backoff; i++)
                    asm volatile("nop");
                backoff = std::min(backoff * 2, 1000);
            }
        }
    }
};

static int parse_i(const char* s, int d) {
    if (!s) return d;
    char* e = nullptr;
    long v = strtol(s, &e, 10);
    return (e && *e == 0) ? (int)v : d;
}

extern "C" int main(int argc, char** argv) {
    int iters = ITERS;
    int n_counters = N_COUNTERS;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--iters") && i + 1 < argc)
            iters = parse_i(argv[++i], iters);
        else if (!strcmp(argv[i], "--counters") && i + 1 < argc)
            n_counters = parse_i(argv[++i], n_counters);
    }
    if (n_counters > N_COUNTERS) n_counters = N_COUNTERS;

    const int hpc   = myCoreThreads();
    const int cpp   = numPodCores();
    const int tid   = myCoreId() * hpc + myThreadId();
    const int total = cpp * hpc;

    // Only run on pod 0, pxn 0
    if (myPXNId() != 0 || myPodId() != 0) return 0;

    // Initialize barrier
    barrier_ref barrier(&g_barrier_data);
    if (tid == 0) {
        barrier.num_threads() = total;
    }

    // Hart 0 zeroes the counter array
    if (tid == 0) {
        for (int i = 0; i < n_counters; i++)
            g_counters[i] = 0;
#ifdef USE_POSTED
        std::printf("ATOMIC_BW [POSTED]: %d harts, %d counters, %d iters/hart\n",
                    total, n_counters, iters);
#else
        std::printf("ATOMIC_BW [BLOCKING]: %d harts, %d counters, %d iters/hart\n",
                    total, n_counters, iters);
#endif
    }

    // Sync all harts before starting timed phase
    barrier.sync();

    // === Timed scatter-add phase ===
    ph_stat_phase(1);
    uint64_t t0 = cycle();

#ifdef USE_POSTED
    for (int i = 0; i < iters; i++) {
        int idx = (tid + i) % n_counters;
        atomic_fetch_add_i64_posted(&g_counters[idx], 1);
    }
#else
    int64_t sink = 0;
    for (int i = 0; i < iters; i++) {
        int idx = (tid + i) % n_counters;
        sink += atomic_fetch_add_i64(&g_counters[idx], 1);
    }
#endif

    uint64_t t1 = cycle();
    ph_stat_phase(0);

    // Barrier so all harts finish before verification
    barrier.sync();

    if (tid == 0) {
        // Fence: blocking atomic add 0 to ensure all posted ops landed
        atomic_fetch_add_i64(&g_counters[0], 0);

        // Verify: total increments = total_harts * iters
        int64_t sum = 0;
        for (int i = 0; i < n_counters; i++)
            sum += atomic_load_i64(&g_counters[i]);

        int64_t expected = (int64_t)total * iters;
        uint64_t cyc = t1 - t0;

        std::printf("Hart 0: %llu cycles for %d atomic adds (%llu cycles/atomic)\n",
                    (unsigned long long)cyc, iters,
                    (unsigned long long)(cyc / iters));
        std::printf("  total increments: %lld (expected %lld) %s\n",
                    (long long)sum, (long long)expected,
                    sum == expected ? "PASS" : "FAIL");
#ifndef USE_POSTED
        std::printf("  sink=%lld\n", (long long)sink);
#endif
    }

    // Final barrier: keep non-zero harts alive until hart 0 finishes verification
    barrier.sync();

    return 0;
}
