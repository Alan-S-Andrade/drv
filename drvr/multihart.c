// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>    // atomic_fetch_add_i64, atomic_fetch_add_i32, atomic_compare_and_swap_i32
#include <pandohammer/hartsleep.h> // hartsleep()

#define HARTS 16
static int64_t thread_phase_counter[HARTS];

// Shared barrier state
static volatile int64_t global_barrier_count = 0;
static volatile int64_t global_barrier_phase = 0;

static inline void barrier(int total_threads) {
    int hid = myThreadId();
    int64_t threads_cur_phase = thread_phase_counter[hid];
    printf("THREAD %d entering barrier phase %ld\n", hid, threads_cur_phase);

    // AMO increment on count
    int64_t old = atomic_fetch_add_i64(&global_barrier_count, 1);
    printf("OLD: %ld\n", old);
    if (old == total_threads - 1) {
        // Last thread: reset and advance phase
        atomic_swap_i64(&global_barrier_count, 0);
        atomic_fetch_add_i64(&global_barrier_phase, 1);
    } else {
        // Spin / sleep until phase advances
        long w = 1;
        long wmax = 8 * 1024;
        while (atomic_load_i64(&global_barrier_phase) == threads_cur_phase) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }

    thread_phase_counter[hid] = threads_cur_phase + 1;
}

static inline int hartid() {
    int hart;
    asm volatile ("csrr %0, mhartid" : "=r" (hart));
    return hart;
}

static inline int64_t amoswap(int64_t w, int64_t *p) {
    int64_t r;
    asm volatile ("amoswap.d %0, %1, 0(%2)"
                  : "=r" (r)
                  : "r" (w), "r" (p)
                  : "memory");
    return r;
}

static inline int64_t amoadd(int64_t w, int64_t *p) {
    int64_t r;
    asm volatile ("amoadd.d %0, %1, 0(%2)"
                  : "=r" (r)
                  : "r" (w), "r" (p)
                  : "memory");
    return r;
}

int64_t x = -1;
int64_t y =  0;

int main() {
    barrier(HARTS);
    printf("Hello from multihart test!\n");
    printf("L2SP Size: %lu\n", podL2SPSize());
    printf("Thread ID: %d\n", myThreadId());
    printf("final barrier phase: %ld\n", global_barrier_phase);
    ph_print_int(myCoreId());
    ph_print_int(myPodId());
    ph_print_int(myPXNId());
    barrier(HARTS);
    printf("final barrier phase: %ld\n", global_barrier_phase);
    ph_print_int(myCoreThreads());
    ph_print_int(numPXN());
    ph_print_int(numPodCores());
    ph_print_int(numPXNPods());
    ph_print_int(coreL1SPSize());
    ph_print_int(podL2SPSize());
    ph_print_int(pxnDRAMSize());
    int64_t id  = hartid();
    printf("Hart ID: %ld\n", id);
    ph_print_int(id);
    // swap id with x
    ph_print_int(amoswap(id, &x));
    ph_print_int(amoadd(1, &y));
    return 0;
}
