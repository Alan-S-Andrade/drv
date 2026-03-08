// Per-Core Work Stealing Benchmark with Tunable Steal Batch Size
// - Allows tuning the number of work units stolen in one attempt (steal_batch_size)
// - Use to measure network interconnect effects of batch stealing
//
// Based on work_stealing_benchmark.cpp

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>

#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>
#include <pandohammer/address.h>
#include <pandohammer/staticdecl.h>

static constexpr int QUEUE_SIZE = 4096;
static constexpr int64_t WORK_UNIT_ITERS = 10000;
static constexpr int MAX_HARTS = 1024;
static constexpr int MAX_CORES = 64;
static constexpr int g_total_work = 1536;

// Steal batch size (can be tuned via command line or macro)
int g_steal_batch_size = 32;

__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

struct WorkQueue {
    volatile int64_t head;
    volatile int64_t tail;
    volatile int64_t items[QUEUE_SIZE];
};

__l2sp__ WorkQueue core_queues[MAX_CORES];
__l2sp__ int64_t g_local_sense[MAX_HARTS];
__l2sp__ volatile int64_t stat_work_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_attempts[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_success[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_failures[MAX_HARTS];

__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;
// Remaining work units (decremented only when a hart actually processes work).
__l2sp__ std::atomic<int64_t> g_work_remaining = 0;

static inline int get_thread_id() {
    int tid = (myCoreId() << 4) + myThreadId();
    return tid;
}

static inline void spin_pause(int64_t iters) {
    for (int64_t i = 0; i < iters; i++) {
        asm volatile("" ::: "memory");
    }
}

static inline void wait_no_sleep_if_work(int64_t backoff) {
    if (g_work_remaining.load(std::memory_order_acquire) > 0) {
        spin_pause(backoff);
    } else {
        hartsleep(backoff);
    }
}

static inline void barrier() {
    int tid = get_thread_id();
    int total = g_total_harts;
    int64_t local = g_local_sense[tid];
    local ^= 1;
    g_local_sense[tid] = local;
    int64_t old = g_count.fetch_add(1, std::memory_order_acq_rel);
    if (old == total - 1) {
        g_count.store(0, std::memory_order_relaxed);
        g_sense.store(local, std::memory_order_release);
    } else {
        long w = 1;
        long wmax = 64 * total;
        while (g_sense.load(std::memory_order_acquire) != local) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }
}

static inline void queue_init(WorkQueue* q) {
    q->head = 0;
    q->tail = 0;
}

static inline bool queue_push(WorkQueue* q, int64_t work) {
    int64_t t = q->tail;
    if (t >= QUEUE_SIZE) return false;
    q->items[t] = work;
    q->tail = t + 1;
    return true;
}

static inline int64_t queue_pop(WorkQueue* q) {
    int64_t backoff = 1;
    const int64_t max_backoff = 32;
    const int max_retries = 16;
    int retries = 0;
    while (retries < max_retries) {
        int64_t t = atomic_load_i64(&q->tail);
        if (t == 0) return -1;
        int64_t h = atomic_load_i64(&q->head);
        if (h >= t) return -1;
        int64_t new_t = t - 1;
        int64_t old_t = atomic_compare_and_swap_i64(&q->tail, t, new_t);
        if (old_t != t) {
            retries++;
            wait_no_sleep_if_work(backoff);
            if (backoff < max_backoff) backoff <<= 1;
            continue;
        }
        backoff = 1;
        h = atomic_load_i64(&q->head);
        if (h <= new_t) {
            return q->items[new_t];
        }
        int64_t old_head = atomic_compare_and_swap_i64(&q->head, h, h + 1);
        if (old_head == h && h == new_t) {
            atomic_compare_and_swap_i64(&q->tail, new_t, 0);
            atomic_compare_and_swap_i64(&q->head, h + 1, 0);
            return q->items[new_t];
        }
        atomic_compare_and_swap_i64(&q->tail, new_t, 0);
        atomic_compare_and_swap_i64(&q->head, h + 1, 0);
        atomic_compare_and_swap_i64(&q->head, h, 0);
        return -1;
    }
    return -1;
}

// Steal up to batch_size work items from another core's queue
static int64_t queue_steal_batch(WorkQueue* q, int64_t* batch, int batch_size) {
    int64_t stolen = 0;
    int64_t backoff = 1;
    const int64_t max_backoff = 32;
    const int max_retries = 8;
    int retries = 0;
    while (retries < max_retries && stolen < batch_size) {
        int64_t h = atomic_load_i64(&q->head);
        int64_t t = q->tail;
        if (h >= t) break;
        int64_t available = t - h;
        int64_t to_steal = (available < (batch_size - stolen)) ? available : (batch_size - stolen);
        if (to_steal <= 0) break;
        // Try to claim a batch by advancing head
        int64_t old_head = atomic_compare_and_swap_i64(&q->head, h, h + to_steal);
        if (old_head == h) {
            for (int64_t i = 0; i < to_steal; i++) {
                batch[stolen++] = q->items[h + i];
            }
            break;
        }
        retries++;
        wait_no_sleep_if_work(backoff);
        if (backoff < max_backoff) backoff <<= 1;
    }
    return stolen;
}

// Steal work item from another hart's queue (use CAS on head)
// Returns -1 if steal failed
static inline int64_t queue_steal(WorkQueue* q) {
    int64_t backoff = 1;
    const int64_t max_backoff = 32;
    const int max_retries = 8;  // Don't spin forever on one victim
    int retries = 0;
    while (retries < max_retries) {
        int64_t h = atomic_load_i64(&q->head);
        int64_t t = q->tail;  // Volatile read is ok here (approximate)
        if (h >= t) return -1;  // Empty
        // Read the item before CAS
        int64_t work = q->items[h];
        // Try to advance head using CAS
        int64_t old_head = atomic_compare_and_swap_i64(&q->head, h, h + 1);
        if (old_head == h) {
            // Successful steal
            return work;
        }
        // CAS failed - retry with backoff
        retries++;
        wait_no_sleep_if_work(backoff);
        if (backoff < max_backoff) backoff <<= 1;
    }
    return -1;  // Too much contention, give up on this victim
}

static inline void do_work(int64_t work_amount) {
    volatile int64_t sum = 0;
    for (int64_t i = 0; i < work_amount * WORK_UNIT_ITERS; i++) {
        sum += i;
    }
    sum = 5;
}

static void work_steal_loop(int tid, int total_harts) {
    std::atomic_thread_fence(std::memory_order_acquire);
    int harts_per_core = g_harts_per_core;
    int total_cores = g_total_cores;
    int my_core = tid / harts_per_core;
    WorkQueue* my_queue = &core_queues[my_core];
    int steal_target_core = (my_core + 1) % total_cores;
    int64_t local_processed = 0;
    int64_t local_steal_attempts = 0;
    int64_t local_steal_success = 0;
    int64_t local_steal_failures = 0;
    int64_t backoff = 1;
    const int64_t max_backoff = 64;
    while (true) {
        if (g_work_remaining.load(std::memory_order_acquire) <= 0) break;
        // Try to get work from own core's queue first
        int64_t work = queue_pop(my_queue);
        if (work >= 0) {
            do_work(work);
            local_processed += work;
            g_work_remaining.fetch_sub(work, std::memory_order_acq_rel);
            backoff = 1;
            continue;
        }
        // Local core queue empty - try to steal from other cores
        // Round-robin through other cores
        bool found_work = false;
        for (int rounds = 0; rounds < total_cores - 1; rounds++) {
            if (steal_target_core == my_core) {
                steal_target_core = (steal_target_core + 1) % total_cores;
            }
            local_steal_attempts++;
            work = queue_steal(&core_queues[steal_target_core]);
            if (work >= 0) {
                // Successful steal
                local_steal_success++;
                do_work(work);
                local_processed += work;
                g_work_remaining.fetch_sub(work, std::memory_order_acq_rel);
                found_work = true;
                backoff = 1;
                // Move to next target for fairness
                steal_target_core = (steal_target_core + 1) % total_cores;
                break;
            } else {
                local_steal_failures++;
                // Brief backoff between steal attempts to reduce contention
                wait_no_sleep_if_work(backoff);
            }
            // Try next victim core
            steal_target_core = (steal_target_core + 1) % total_cores;
        }
        if (!found_work) {
            if (g_work_remaining.load(std::memory_order_acquire) <= 0) break;
            // Exponential backoff before retry
            wait_no_sleep_if_work(backoff);
            if (backoff < max_backoff) backoff <<= 1;
        }
    }
    // Record statistics
    stat_work_processed[tid] = local_processed;
    stat_steal_attempts[tid] = local_steal_attempts;
    stat_steal_success[tid] = local_steal_success;
    stat_steal_failures[tid] = local_steal_failures;
}

static void distribute_work_imbalanced(int tid, int total_harts, int total_work) {
    if (tid == 0) {
        int odd_cores = g_total_cores / 2;
        int even_cores = g_total_cores - odd_cores;
        int base_work = total_work / (even_cores + 2 * odd_cores);
        int64_t total_pushed_units = 0;
        for (int c = 0; c < g_total_cores; c++) {
            int my_work = (c % 2 == 0) ? base_work : base_work * 2;
            if (my_work > QUEUE_SIZE) my_work = QUEUE_SIZE;
            for (int i = 0; i < my_work; i++) {
                queue_push(&core_queues[c], 1);
            }
            total_pushed_units += my_work;
        }
        g_work_remaining.store(total_pushed_units, std::memory_order_release);
    }
}

int main(int argc, char** argv) {
    const int hart_in_core = myThreadId();
    const int core_id = myCoreId();
    const int harts_per_core = myCoreThreads();
    const int total_cores = numPodCores();
    const int max_hw_harts = total_cores * harts_per_core;
    const int tid = get_thread_id();
    if (tid == 0) {
        g_total_cores = total_cores;
        g_harts_per_core = harts_per_core;
        g_total_harts = max_hw_harts;
        for (int i = 0; i < g_total_harts; i++) {
            g_local_sense[i] = 0;
            stat_work_processed[i] = 0;
            stat_steal_attempts[i] = 0;
            stat_steal_success[i] = 0;
            stat_steal_failures[i] = 0;
        }
        for (int i = 0; i < g_total_cores; i++) {
            queue_init(&core_queues[i]);
        }
        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }
    barrier();
    // Parse batch size from command line if provided
    if (argc > 1) {
        int val = atoi(argv[1]);
        if (val > 0 && val <= 64) g_steal_batch_size = val;
    }
    distribute_work_imbalanced(tid, g_total_harts, g_total_work);
    barrier();
    if (tid == 0) {
        std::printf("\nInitial work distribution:\n");
        for (int c = 0; c < g_total_cores; c++) {
            int64_t work_count = core_queues[c].tail - core_queues[c].head;
            std::printf("  Core %2d: %ld items\n", c, (long)work_count);
        }
        std::printf("\nStarting work stealing (batch size = %d)...\n", g_steal_batch_size);
    }
    barrier();
    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(start_cycles));
    }
    work_steal_loop(tid, g_total_harts);
    if (tid == 0) {
        std::printf("Work stealing complete!\n");
    }
    barrier();
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(end_cycles));
    }
    barrier();
    if (tid == 0) {
        std::printf("\n=== Results ===\n");
        int64_t total_processed = 0;
        int64_t total_steal_attempts = 0;
        int64_t total_steal_success = 0;
        int64_t total_steal_failures = 0;
        int64_t max_processed = 0;
        int64_t min_processed = 0;
        std::printf("\nPer-hart statistics:\n");
        std::printf("Hart | Processed | Steal Attempts | Steals OK | Steals Fail\n");
        std::printf("-----|-----------|----------------|-----------|------------\n");
        for (int h = 0; h < g_total_harts; h++) {
            int64_t processed = stat_work_processed[h];
            int64_t attempts = stat_steal_attempts[h];
            int64_t success = stat_steal_success[h];
            int64_t failures = stat_steal_failures[h];
            std::printf("%4d | %9ld | %14ld | %9ld | %10ld\n", h, (long)processed, (long)attempts, (long)success, (long)failures);
            total_processed += processed;
            total_steal_attempts += attempts;
            total_steal_success += success;
            total_steal_failures += failures;
            if (processed > max_processed) max_processed = processed;
            if (processed < min_processed) min_processed = processed;
        }
        std::printf("\nSummary:\n");
        std::printf("  Total work processed: %ld\n", (long)total_processed);
        std::printf("  Total steal attempts: %ld\n", (long)total_steal_attempts);
        if (total_steal_attempts > 0) {
            std::printf("  Successful steals:    %ld (%ld%%)\n", (long)total_steal_success, (long)(100 * total_steal_success / total_steal_attempts));
        } else {
            std::printf("  Successful steals:    %ld\n", (long)total_steal_success);
        }
        std::printf("  Failed steals:        %ld\n", (long)total_steal_failures);
        std::printf("  Load balance:\n");
        std::printf("    Min processed:      %ld\n", (long)min_processed);
        std::printf("    Max processed:      %ld\n", (long)max_processed);
        uint64_t elapsed = end_cycles - start_cycles;
        std::printf("  Cycles elapsed:       %lu\n", (unsigned long)elapsed);
        if (total_processed > 0) {
            std::printf("  Cycles per work unit: %lu\n", (unsigned long)(elapsed / total_processed));
        }
    }
    barrier();
    return 0;
}
