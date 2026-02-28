// SPDX-License-Identifier: MIT
// Test for posted (fire-and-forget) and blocking atomics
// 4 harts each do posted atomic adds to a shared counter,
// then a blocking atomic load verifies the final value.
#include <stdint.h>
#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>

#define ITERS 10

// Shared counter in DRAM (default section)
volatile int64_t posted_counter = 0;
volatile int64_t blocking_counter = 0;
volatile int64_t done_flag = 0;

int main()
{
    int tid = myThreadId();
    int nharts = myCoreThreads();

    // --- Test 1: Posted atomic adds ---
    // Each hart adds 1, ITERS times, using posted (fire-and-forget)
    for (int i = 0; i < ITERS; i++) {
        atomic_fetch_add_i64_posted((volatile int64_t *)&posted_counter, 1);
    }

    // --- Test 2: Blocking atomic adds ---
    // Each hart adds 1, ITERS times, using blocking atomics
    for (int i = 0; i < ITERS; i++) {
        atomic_fetch_add_i64((volatile int64_t *)&blocking_counter, 1);
    }

    // Simple barrier: each hart signals done, hart 0 waits for all
    atomic_fetch_add_i64((volatile int64_t *)&done_flag, 1);

    if (tid == 0) {
        // Spin until all harts are done
        while (atomic_load_i64((volatile int64_t *)&done_flag) < nharts) {
            // spin
        }

        // Need to wait a bit for any in-flight posted atomics to land.
        // Do a blocking atomic add of 0 to the same address to fence.
        atomic_fetch_add_i64((volatile int64_t *)&posted_counter, 0);

        int64_t posted_val = atomic_load_i64((volatile int64_t *)&posted_counter);
        int64_t blocking_val = atomic_load_i64((volatile int64_t *)&blocking_counter);
        int64_t expected = (int64_t)nharts * ITERS;

        ph_print_int(posted_val);
        ph_print_int(blocking_val);
        ph_print_int(expected);

        if (blocking_val != expected) {
            // blocking atomics broken
            return 1;
        }
        if (posted_val != expected) {
            // posted atomics broken
            return 2;
        }
    }
    return 0;
}
