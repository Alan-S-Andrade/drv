// shared_read_l2sp.cpp
// Shared-read L2SP microbenchmark for PANDOHammer.
// All harts read from the SAME shared array in L2SP.
// Measures L2SP bank conflict overhead vs. disjoint-access stream_bw_l2sp.
// Read-only -- no synchronization needed.
// Use SST stats (summarize.py) for detailed memory access breakdown.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pandohammer/cpuinfo.h>
#include <pandohammer/mmio.h>

#define __l2sp__ __attribute__((section(".l2sp")))

// Max shared array size (64KB). Default usage is 128 words (1KB),
// matching the per-hart working set in stream_bw_l2sp.
static const int MAX_WORDS = 8 * 1024;

__l2sp__ uint64_t g_shared[MAX_WORDS];

static int parse_i(const char* s, int d) {
    if (!s) return d;
    char* e = nullptr;
    long v = strtol(s, &e, 10);
    return (e && *e == 0) ? (int)v : d;
}

extern "C" int main(int argc, char** argv) {
    int num_words = 128;    // 1KB shared array (matches per-hart slice in stream_bw)
    int iters = 160;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--N") && i + 1 < argc)
            num_words = parse_i(argv[++i], num_words);
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc)
            iters = parse_i(argv[++i], iters);
        else if (!strcmp(argv[i], "--help")) {
            std::printf("Usage: %s [--N num_words] [--iters iterations]\n", argv[0]);
            return 0;
        }
    }

    if (num_words > MAX_WORDS) num_words = MAX_WORDS;

    const int hpc = myCoreThreads();
    const int cpp = numPodCores();
    const int tid = myCoreId() * hpc + myThreadId();
    const int total = cpp * hpc;

    // Only run on pod 0, pxn 0 (L2SP is pod-local)
    if (myPXNId() != 0 || myPodId() != 0) return 0;

    // Hart 0 initializes the shared array
    if (tid == 0) {
        std::printf("SHARED_READ_L2SP: %d harts, %d shared words (%d B), %d iters\n",
                    total, num_words, num_words * 8, iters);
        for (int i = 0; i < num_words; i++)
            g_shared[i] = (uint64_t)(i + 1);
    }

    // === READ phase (all harts read the same shared array) ===
    ph_stat_phase(1);
    uint64_t t0 = cycle();
    uint64_t sink = 0;
    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < num_words; i++) {
            sink += g_shared[i];
        }
    }
    uint64_t t1 = cycle();
    ph_stat_phase(0);

    if (tid == 0) {
        uint64_t bytes = (uint64_t)num_words * 8 * iters;
        std::printf("Hart 0: read %llu cyc (%llu bytes)\n",
                    (unsigned long long)(t1 - t0),
                    (unsigned long long)bytes);
        std::printf("sink=%llu\nDone.\n", (unsigned long long)sink);
    }

    return 0;
}
