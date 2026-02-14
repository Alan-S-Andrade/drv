// stream_bw_l2sp.cpp
// Disjoint-read streaming bandwidth microbenchmark for PANDOHammer.
// Each hart independently reads its own chunk of L2SP.
// No barriers, no synchronization -- measures raw memory bandwidth.
// Use SST stats (summarize.py) for detailed memory access breakdown.
//
// Compile with -DENABLE_WRITE_PHASE to also benchmark writes.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pandohammer/cpuinfo.h>
#include <pandohammer/mmio.h>

#define __l2sp__ __attribute__((section(".l2sp")))

// 512KB of L2SP storage (64K x 8-byte words).
// 64 cores x 8 threads x 128 words/hart = 64K words = 512KB.
static const int MAX_WORDS = 64 * 1024;

__l2sp__ uint64_t g_array[MAX_WORDS];

static int parse_i(const char* s, int d) {
    if (!s) return d;
    char* e = nullptr;
    long v = strtol(s, &e, 10);
    return (e && *e == 0) ? (int)v : d;
}

extern "C" int main(int argc, char** argv) {
    int words_per_hart = 64;   // 1KB per hart (64 cores x 128 = 64KB total)
    int iters = 160;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--N") && i + 1 < argc)
            words_per_hart = parse_i(argv[++i], words_per_hart);
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc)
            iters = parse_i(argv[++i], iters);
        else if (!strcmp(argv[i], "--help")) {
            std::printf("Usage: %s [--N words_per_hart] [--iters iterations]\n", argv[0]);
            return 0;
        }
    }

    const int hpc = myCoreThreads();
    const int cpp = numPodCores();
    const int tid = myCoreId() * hpc + myThreadId();
    const int total = cpp * hpc;

    // Only run on pod 0, pxn 0 (L2SP is pod-local)
    if (myPXNId() != 0 || myPodId() != 0) return 0;

    int total_words = words_per_hart * total;
    if (total_words > MAX_WORDS) {
        if (tid == 0)
            std::printf("ERROR: need %d words but MAX_WORDS=%d\n",
                        total_words, MAX_WORDS);
        return 1;
    }

    int base = tid * words_per_hart;

    if (tid == 0) {
        std::printf("STREAM_BW_L2SP: %d harts, %d words/hart (%d B), %d iters\n",
                    total, words_per_hart, words_per_hart * 8, iters);
        std::printf("Total footprint: %d KB in L2SP\n", total_words * 8 / 1024);
    }

    // Initialize each hart's slice (untimed, outside stat_phase)
    for (int i = 0; i < words_per_hart; i++)
        g_array[base + i] = (uint64_t)(i + 1);

#ifdef ENABLE_WRITE_PHASE
    // === WRITE phase (disabled by default, compile with -DENABLE_WRITE_PHASE) ===
    ph_stat_phase(1);
    uint64_t t0 = cycle();
    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < words_per_hart; i++) {
            g_array[base + i] = (uint64_t)(i ^ it);
        }
    }
    uint64_t t1 = cycle();
    ph_stat_phase(0);
#endif

    // === READ phase ===
    ph_stat_phase(1);
    uint64_t t2 = cycle();
    uint64_t sink = 0;
    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < words_per_hart; i++) {
            sink += g_array[base + i];
        }
    }
    uint64_t t3 = cycle();
    ph_stat_phase(0);

    // Only hart 0 prints timing
    if (tid == 0) {
        uint64_t bytes = (uint64_t)words_per_hart * 8 * iters;
#ifdef ENABLE_WRITE_PHASE
        std::printf("Hart 0: write %llu cyc, read %llu cyc (%llu bytes each)\n",
                    (unsigned long long)(t1 - t0),
                    (unsigned long long)(t3 - t2),
                    (unsigned long long)bytes);
#else
        std::printf("Hart 0: read %llu cyc (%llu bytes)\n",
                    (unsigned long long)(t3 - t2),
                    (unsigned long long)bytes);
#endif
        std::printf("sink=%llu\nDone.\n", (unsigned long long)sink);
    }

    return 0;
}
