#include "vread.hpp"
#include <cstdio>
#include "pandohammer/atomic.h"
#include "pandohammer/cpuinfo.h"
#include "pandohammer/mmio.h"
#ifndef CORES
#define CORES (numPodCores())
#endif

#ifndef THREADS
#define THREADS (myCoreThreads()*numPodCores())
#endif

#ifndef BLOCK_SIZE
#define BLOCK_SIZE 8
#endif

#define ID (myCoreId()*myCoreThreads()+myThreadId())

__attribute__((section(".dram")))
idx_type sync = 0;

int main(int argc, char *argv[])
{
    int64_t* a = (int64_t*)DRAM_BASE;

    for (idx_type i = BLOCK_SIZE*ID; i < vsize; i+=BLOCK_SIZE*THREADS) {
        for (idx_type j = 0; j < BLOCK_SIZE; j++) {
            atomic_load_i64(&a[i+j]);
        }
    }

    atomic_fetch_add_i64(&sync, 1);
    while (atomic_load_i64(&sync) != THREADS) {
        // spin
    }
    return 0;
}
