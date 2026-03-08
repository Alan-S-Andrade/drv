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

static constexpr int QUEUE_SIZE = 4096;        // Max work items per core
static constexpr int64_t WORK_UNIT_ITERS = 10000; // Iterations per work unit
static constexpr int MAX_HARTS = 1024;         // Maximum array size
static constexpr int MAX_CORES = 64;           // Maximum cores
static constexpr int g_total_work = 1524;      // Total work units to distribute
static constexpr int WORK_CHUNK_SIZE = 32;     // Work units per queue item (range)
static constexpr int STEAL_K = 4;              // Max items to steal per episode (tunable)
static constexpr int MAX_WQ_TRACE_SAMPLES = 512;  // Per-core queue-depth trace samples
static constexpr int WQ_TRACE_SAMPLE_PERIOD = 32; // Sample every N loop iterations (core leader only)

// Pack a (begin, end) range into a single int64_t queue item
static inline int64_t pack_range(int32_t begin, int32_t end) {
    return ((int64_t)(uint32_t)begin << 32) | (int64_t)(uint32_t)end;
}
static inline int32_t range_begin(int64_t packed) { return (int32_t)(packed >> 32); }
static inline int32_t range_end(int64_t packed) { return (int32_t)(packed & 0xFFFFFFFF); }

// Runtime values (set by hart 0 during initialization)
__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;  // Flag to signal initialization complete

// -------------------- Work Queue Structure --------------------
// Each core has a deque-like structure
// - Harts in core push/pop from tail (LIFO for locality)
// - Harts from other cores steal from head (FIFO) using CAS on head index

struct WorkQueue {
    volatile int64_t head;                  // Steal point (CAS protected)
    volatile int64_t tail;                  // Pop point (CAS protected for concurrent pops)
    volatile int64_t items[QUEUE_SIZE];     // Work items
};

struct WorkQueueTraceSample {
    uint64_t cycle;
    int64_t depth;
    int64_t head;
    int64_t tail;
};

__l2sp__ WorkQueue core_queues[MAX_CORES];     // Static array for core queues
__l2sp__ int64_t g_local_sense[MAX_HARTS];     // Static array for per-hart sense
__l2sp__ volatile int64_t stat_work_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_attempts[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_success[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_failures[MAX_HARTS];
__l2sp__ volatile uint64_t g_core_l1sp_bytes = 0;

__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;
// Remaining work units (decremented only when a hart actually processes units).
__l2sp__ std::atomic<int64_t> g_work_remaining = 0;

// Per-core sum variables — cache-line padded so each core hits its own L2SP line
struct alignas(64) CoreLocalSum {
    volatile int64_t value;
};
__l2sp__ CoreLocalSum g_core_sum[MAX_CORES];

// Per-core steal token: only one hart per core may steal at a time
__l2sp__ std::atomic<int> core_thief[MAX_CORES];
__l2sp__ WorkQueueTraceSample g_wq_trace[MAX_CORES][MAX_WQ_TRACE_SAMPLES];
__l2sp__ volatile int32_t g_wq_trace_count[MAX_CORES];

static inline uint64_t rdcycle_u64() {
    uint64_t c;
    asm volatile("rdcycle %0" : "=r"(c));
    return c;
}

// One writer per core (the core leader) records queue depth over time.
static inline void record_workqueue_depth_sample(int core_id, WorkQueue* q) {
    int32_t idx = g_wq_trace_count[core_id];
    if (idx < 0 || idx >= MAX_WQ_TRACE_SAMPLES) return;
    int64_t h = atomic_load_i64(&q->head);
    int64_t t = atomic_load_i64(&q->tail);
    int64_t d = t - h;
    if (d < 0) d = 0;
    if (d > QUEUE_SIZE) d = QUEUE_SIZE;
    g_wq_trace[core_id][idx].cycle = rdcycle_u64();
    g_wq_trace[core_id][idx].depth = d;
    g_wq_trace[core_id][idx].head = h;
    g_wq_trace[core_id][idx].tail = t;
    g_wq_trace_count[core_id] = idx + 1;
}

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
    
    // g_total_harts is now guaranteed to be initialized
    int total = g_total_harts;
    
    int64_t local = g_local_sense[tid];
    local ^= 1;
    g_local_sense[tid] = local;

    int64_t old = g_count.fetch_add(1, std::memory_order_acq_rel);

    if (old == total - 1) {
        // last hart: reset count for next round, then publish sense flip
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
    if (t >= QUEUE_SIZE) return false;  // Queue full
    q->items[t] = work;
    q->tail = t + 1;
    return true;
}

// Atomic push for refilling local queue (thief pushes stolen items for local harts)
static inline bool queue_push_atomic(WorkQueue* q, int64_t work) {
    while (true) {
        int64_t t = atomic_load_i64(&q->tail);
        if (t >= QUEUE_SIZE) return false;
        int64_t old_t = atomic_compare_and_swap_i64(&q->tail, t, t + 1);
        if (old_t == t) {
            q->items[t] = work;
            return true;
        }
        wait_no_sleep_if_work(1);
    }
}

// Pop work item (multiple harts per core may call this concurrently)
// Returns -1 if empty or too much contention
static inline int64_t queue_pop(WorkQueue* q) {
    int64_t backoff = 1;
    const int64_t max_backoff = 32;
    const int max_retries = 16;  // Limit retries to avoid livelock under heavy contention
    int retries = 0;
    
    while (retries < max_retries) {
        int64_t t = atomic_load_i64(&q->tail);
        if (t == 0) return -1;  // Empty
        
        int64_t h = atomic_load_i64(&q->head);
        if (h >= t) return -1;  // Empty (all stolen)
        
        // Try to atomically decrement tail using CAS
        int64_t new_t = t - 1;
        int64_t old_t = atomic_compare_and_swap_i64(&q->tail, t, new_t);
        if (old_t != t) {
            // Another hart modified tail, retry with backoff to reduce contention
            retries++;
            wait_no_sleep_if_work(backoff);
            if (backoff < max_backoff) backoff <<= 1;
            continue;
        }
        backoff = 1;  // Reset backoff on successful CAS
        
        // Successfully decremented tail, now check if we raced with a stealer
        h = atomic_load_i64(&q->head);
        if (h <= new_t) {
            // We got the item at index new_t
            return q->items[new_t];
        }
        
        // Race with stealer for the last item - try CAS on head
        int64_t old_head = atomic_compare_and_swap_i64(&q->head, h, h + 1);
        if (old_head == h && h == new_t) {
            // We won - got the item, reset queue using CAS
            atomic_compare_and_swap_i64(&q->tail, new_t, 0);
            atomic_compare_and_swap_i64(&q->head, h + 1, 0);
            return q->items[new_t];
        }
        
        // Lost race, queue is empty - best effort reset
        // tail is new_t, head is somewhere >= new_t
        atomic_compare_and_swap_i64(&q->tail, new_t, 0);
        // head could have been advanced by stealer, just try common cases
        atomic_compare_and_swap_i64(&q->head, h + 1, 0);
        atomic_compare_and_swap_i64(&q->head, h, 0);
        return -1;
    }
    return -1;  // Too much contention, give up
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

// Steal up to k items from another core's queue (CAS on head).
// Returns count stolen (0 if empty/contention). Sets *out_begin to first index.
static inline int64_t queue_steal_k(WorkQueue* q, int64_t* out_begin, int64_t max_k) {
    int64_t backoff = 1;
    const int64_t max_backoff = 32;
    const int max_retries = 8;
    int retries = 0;

    while (retries < max_retries) {
        int64_t h = atomic_load_i64(&q->head);
        int64_t t = q->tail;  // volatile read (approximate)
        if (h >= t) return 0;

        int64_t avail = t - h;
        int64_t k = (avail < max_k) ? avail : max_k;

        int64_t old_head = atomic_compare_and_swap_i64(&q->head, h, h + k);
        if (old_head == h) {
            *out_begin = h;
            return k;
        }
        retries++;
        wait_no_sleep_if_work(backoff);
        if (backoff < max_backoff) backoff <<= 1;
    }
    return 0;
}

// Fast xorshift PRNG for victim selection — mixes tid + counter to decorrelate thieves
static inline uint32_t xorshift_victim(uint32_t seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static constexpr int RECENTLY_TRIED_SIZE = 4; // small "recently failed" ring buffer

// Process a range [begin, end) of work units; each unit does WORK_UNIT_ITERS iterations
static inline void do_work_range(int32_t begin, int32_t end, int core_id) {
    volatile int64_t *sum = &g_core_sum[core_id].value;
    for (int32_t w = begin; w < end; w++) {
        for (int64_t i = 0; i < WORK_UNIT_ITERS; i++) {
            *sum += i;
        }
    }
}

static void work_steal_loop(int tid, int total_harts) {
    // Ensure we see the initialization
    std::atomic_thread_fence(std::memory_order_acquire);
    int harts_per_core = g_harts_per_core;
    int total_cores = g_total_cores;
    
    if (tid == 0) {
        std::printf("Hart 0 entering work_steal_loop, total_cores=%d, harts_per_core=%d\n", total_cores, harts_per_core);
        std::fflush(stdout);
    }
    
    int my_core = tid / harts_per_core;
    const bool is_core_leader = ((tid % harts_per_core) == 0);
    WorkQueue* my_queue = &core_queues[my_core];

    // Xorshift RNG state — seeded per-hart so thieves diverge
    uint32_t rng_state = (uint32_t)(tid + 1) * 2654435761u;

    // Small ring buffer of recently-failed victims to avoid immediate retries
    int recently_tried[RECENTLY_TRIED_SIZE];
    for (int i = 0; i < RECENTLY_TRIED_SIZE; i++) recently_tried[i] = -1;
    int rt_idx = 0;
    
    int64_t local_processed = 0;
    int64_t local_steal_attempts = 0;
    int64_t local_steal_success = 0;
    int64_t local_steal_failures = 0;
    
    // Local buffer for stolen items — avoids re-touching victim's cache lines
    int64_t stolen_buf[STEAL_K];
    
    if (tid == 0) {
        std::printf("Hart 0: my_core=%d\n", my_core);
        std::fflush(stdout);
    }
    
    int64_t backoff = 1;
    const int64_t max_backoff = 64;
    int loop_iters = 0;
    
    while (true) {
        if (g_work_remaining.load(std::memory_order_acquire) <= 0) break;
        if (is_core_leader && ((loop_iters % WQ_TRACE_SAMPLE_PERIOD) == 0)) {
            record_workqueue_depth_sample(my_core, my_queue);
        }

        // Try to get work from own core's queue first
        int64_t packed = queue_pop(my_queue);
        
        if (packed >= 0) {
            // Process local work range
            int32_t rb = range_begin(packed);
            int32_t re = range_end(packed);
            do_work_range(rb, re, my_core);
            int64_t processed_units = (re - rb);
            local_processed += processed_units;
            g_work_remaining.fetch_sub(processed_units, std::memory_order_acq_rel);
            backoff = 1;  // Reset backoff on success
            continue;
        }
        
        // Try to become this core's thief (only one hart per core steals at a time)
        if (core_thief[my_core].exchange(1, std::memory_order_acquire) == 0) {
            // Won the steal token — steal from other cores (random victim)
            bool found_work = false;
            for (int rounds = 0; rounds < total_cores - 1; rounds++) {
                // Pick a random victim, skip self and recently-failed cores
                int victim;
                int pick_tries = 0;
                do {
                    rng_state = xorshift_victim(rng_state);
                    victim = (int)(rng_state % (uint32_t)total_cores);
                    // Check recently-tried list
                    bool in_recent = false;
                    if (victim != my_core) {
                        for (int ri = 0; ri < RECENTLY_TRIED_SIZE; ri++) {
                            if (recently_tried[ri] == victim) { in_recent = true; break; }
                        }
                    }
                    if (victim != my_core && !in_recent) break;
                    pick_tries++;
                } while (pick_tries < total_cores);
                if (victim == my_core) continue;
                
                local_steal_attempts++;
                int64_t steal_begin;
                int64_t stolen = queue_steal_k(&core_queues[victim], &steal_begin, STEAL_K);
                
                if (stolen > 0) {
                    // Copy stolen items to local buffer, then release victim's cache lines
                    local_steal_success++;
                    for (int64_t s = 0; s < stolen; s++) {
                        stolen_buf[s] = core_queues[victim].items[steal_begin + s];
                    }
                    // Thief keeps first item; pushes rest into local queue for sibling harts
                    for (int64_t s = 1; s < stolen; s++) {
                        queue_push_atomic(my_queue, stolen_buf[s]);
                    }
                    // Process only the thief's own item
                    {
                        int32_t rb = range_begin(stolen_buf[0]);
                        int32_t re = range_end(stolen_buf[0]);
                        do_work_range(rb, re, my_core);
                        int64_t processed_units = (re - rb);
                        local_processed += processed_units;
                        g_work_remaining.fetch_sub(processed_units, std::memory_order_acq_rel);
                    }
                    found_work = true;
                    backoff = 1;  // Reset backoff on success
                    // Clear recently-tried on success
                    for (int ri = 0; ri < RECENTLY_TRIED_SIZE; ri++) recently_tried[ri] = -1;
                    break;
                } else {
                    local_steal_failures++;
                    // Record this victim as recently failed
                    recently_tried[rt_idx] = victim;
                    rt_idx = (rt_idx + 1) % RECENTLY_TRIED_SIZE;
                    // Brief backoff between steal attempts to reduce contention
                    wait_no_sleep_if_work(backoff);
                }
            }
            
            // Release steal token
            core_thief[my_core].store(0, std::memory_order_release);
            
            if (!found_work) {
                if (g_work_remaining.load(std::memory_order_acquire) <= 0) break;
                wait_no_sleep_if_work(backoff);
                if (backoff < max_backoff) backoff <<= 1;
            }
        } else {
            // Another hart on this core is already stealing — back off harder, retry local
            if (g_work_remaining.load(std::memory_order_acquire) <= 0) break;
            wait_no_sleep_if_work(backoff * 4);
            if (backoff < max_backoff) backoff <<= 1;
        }
        loop_iters++;
    }

    if (is_core_leader) {
        record_workqueue_depth_sample(my_core, my_queue);
    }
    
    // Record statistics
    stat_work_processed[tid] = local_processed;
    stat_steal_attempts[tid] = local_steal_attempts;
    stat_steal_success[tid] = local_steal_success;
    stat_steal_failures[tid] = local_steal_failures;
}

static void distribute_work_imbalanced(int tid, int total_harts, int total_work) {
    // Only hart 0 distributes work for all cores
    if (tid == 0) {
        std::printf("Hart 0 distributing work...\n");
        std::printf("  total_work=%d, g_total_cores=%d, work_per_core=%d\n",
                   total_work, g_total_cores, total_work / g_total_cores);
        
        // Count odd and even cores for proper distribution
        int odd_cores = g_total_cores / 2;
        int even_cores = g_total_cores - odd_cores;
        // Odd cores get 2x work, so total = even_cores * base + odd_cores * 2 * base
        // total = base * (even_cores + 2 * odd_cores)
        int base_work = total_work / (even_cores + 2 * odd_cores);
        int64_t total_pushed_units = 0;
        
        for (int c = 0; c < g_total_cores; c++) {
            int my_work = (c % 2 == 0) ? base_work : base_work * 2;

            if (my_work > QUEUE_SIZE) my_work = QUEUE_SIZE;
            
            std::printf("  Core %2d: distributing %d items... ", c, my_work);
            std::fflush(stdout);
            
            // Push work ranges to this core's queue
            int pushed_units = 0;
            int pushed_chunks = 0;
            for (int w = 0; w < my_work; w += WORK_CHUNK_SIZE) {
                int32_t end = (w + WORK_CHUNK_SIZE > my_work) ? my_work : w + WORK_CHUNK_SIZE;
                if (queue_push(&core_queues[c], pack_range((int32_t)w, (int32_t)end))) {
                    pushed_units += (end - w);
                    pushed_chunks++;
                } else {
                    std::printf("PUSH FAILED at chunk %d! ", pushed_chunks);
                    break;
                }
            }
            std::printf("pushed %d units in %d chunks, tail=%ld\n", pushed_units, pushed_chunks, (long)core_queues[c].tail);
            total_pushed_units += pushed_units;
        }
        g_work_remaining.store(total_pushed_units, std::memory_order_release);
        std::printf("Distribution complete.\n");
    }
}

static void dump_wqtrace(FILE* out, int total_cores) {
    int32_t max_samples = 0;
    for (int c = 0; c < total_cores; c++) {
        if (g_wq_trace_count[c] > max_samples) max_samples = g_wq_trace_count[c];
    }
    const uint64_t core_l1sp_bytes = g_core_l1sp_bytes;
    const uint64_t global_l1sp_bytes = core_l1sp_bytes * (uint64_t)total_cores;

    std::fprintf(out, "WQTRACE_DUMP_BEGIN,bench=work_stealing_benchmark,cores=%d,samples=%d,dropped=0\n",
                 total_cores, (int)max_samples);

    for (int32_t i = 0; i < max_samples; i++) {
        std::fprintf(out, "WQTRACE,bench=work_stealing_benchmark,cores=%d,sample=%d,phase=runtime,level=-1,iter=-1,queue=core,depths=",
                     total_cores, (int)i);
        for (int c = 0; c < total_cores; c++) {
            int32_t n = g_wq_trace_count[c];
            int64_t d = 0;
            if (n > 0) {
                int32_t idx = (i < n) ? i : (n - 1);
                d = g_wq_trace[c][idx].depth;
            }
            if (c > 0) std::fprintf(out, "|");
            std::fprintf(out, "%ld", (long)d);
        }
        std::fprintf(out, "\n");
    }

    std::fprintf(out, "WQTRACE_DUMP_END,bench=work_stealing_benchmark\n");

    std::fprintf(out,
                 "L1SPTRACE_DUMP_BEGIN,bench=work_stealing_benchmark,cores=%d,harts=%d,samples=%d\n",
                 total_cores, g_total_harts, (int)max_samples);
    std::fprintf(out,
                 "L1SPTRACE_CONFIG,bench=work_stealing_benchmark,core_bytes=%lu,global_bytes=%lu\n",
                 (unsigned long)core_l1sp_bytes, (unsigned long)global_l1sp_bytes);
    for (int32_t i = 0; i < max_samples; i++) {
        uint64_t cycle = 0;
        bool have_cycle = false;
        for (int c = 0; c < total_cores; c++) {
            int32_t n = g_wq_trace_count[c];
            if (n > 0) {
                int32_t idx = (i < n) ? i : (n - 1);
                cycle = g_wq_trace[c][idx].cycle;
                have_cycle = true;
                break;
            }
        }
        if (!have_cycle) cycle = 0;
        std::fprintf(out,
                     "L1SPTRACE_GLOBAL,bench=work_stealing_benchmark,sample=%d,phase=runtime,level=-1,iter=-1,cycle=%lu,bytes=%lu\n",
                     (int)i, (unsigned long)cycle, (unsigned long)global_l1sp_bytes);
    }
    for (int h = 0; h < g_total_harts; h++) {
        std::fprintf(out,
                     "L1SPTRACE_HART,bench=work_stealing_benchmark,hart=%d,core=%d,thread=%d,bytes=%lu\n",
                     h, h / g_harts_per_core, h % g_harts_per_core,
                     (unsigned long)core_l1sp_bytes);
    }
    std::fprintf(out, "L1SPTRACE_DUMP_END,bench=work_stealing_benchmark\n");
}

int main(int argc, char** argv) {
    // Get hardware topology
    const int hart_in_core = myThreadId();
    const int core_id = myCoreId();
    const int harts_per_core = myCoreThreads();
    const int total_cores = numPodCores();
    const int max_hw_harts = total_cores * harts_per_core;
    const int tid = get_thread_id();
    
    // Thread 0 initializes shared memory structures
    if (tid == 0) {
        // Set runtime configuration to match hardware
        g_total_cores = total_cores;
        g_harts_per_core = harts_per_core;
        g_total_harts = max_hw_harts;
        
        std::printf("=== Per-Core Work Stealing Benchmark ==\n");
        std::printf("Hardware: %d cores x %d harts = %d total harts\n", 
                   total_cores, harts_per_core, max_hw_harts);
        std::printf("Using: %d cores x %d harts = %d total harts\n",
                   g_total_cores, g_harts_per_core, g_total_harts);
        g_core_l1sp_bytes = coreL1SPSize();
        std::printf("L1SP: per-core=%lu bytes, global=%lu bytes\n",
                   (unsigned long)g_core_l1sp_bytes,
                   (unsigned long)(g_core_l1sp_bytes * (uint64_t)g_total_cores));
        
        // Initialize arrays
        for (int i = 0; i < g_total_harts; i++) {
            g_local_sense[i] = 0;
            stat_work_processed[i] = 0;
            stat_steal_attempts[i] = 0;
            stat_steal_success[i] = 0;
            stat_steal_failures[i] = 0;
        }
        
        // Initialize all core queues, per-core sums, and steal tokens
        for (int i = 0; i < g_total_cores; i++) {
            queue_init(&core_queues[i]);
            g_core_sum[i].value = 0;
            core_thief[i].store(0, std::memory_order_relaxed);
            g_wq_trace_count[i] = 0;
        }
        
        std::printf("Total work units: %d (chunk size: %d)\n", g_total_work, WORK_CHUNK_SIZE);
        std::printf("\n");
        
        // Signal initialization complete with memory fence
        std::atomic_thread_fence(std::memory_order_release);
        g_initialized.store(1, std::memory_order_release);
    } else {
        // Other harts: wait for initialization to complete
        while (g_initialized.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }
    
    barrier();
    
    distribute_work_imbalanced(tid, g_total_harts, g_total_work);
    
    barrier();

    if (hart_in_core == 0) {
        record_workqueue_depth_sample(core_id, &core_queues[core_id]);
    }
    
    if (tid == 0) {
        std::printf("\nInitial work distribution:\n");
        for (int c = 0; c < g_total_cores; c++) {
            int64_t chunk_count = core_queues[c].tail - core_queues[c].head;
            std::printf("  Core %2d: %ld chunks\n", c, (long)chunk_count);
        }
        std::printf("\nStarting work stealing...\n");
    }
    
    barrier();

    if (hart_in_core == 0) {
        record_workqueue_depth_sample(core_id, &core_queues[core_id]);
    }
    
    if (tid == 0) {
        std::printf("Past barrier 3, about to start work...\n");
        std::fflush(stdout);
    }
    
    // Record start time (using cycle counter if available)
    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(start_cycles));
    }
    
    // Phase 2: Work stealing execution
    work_steal_loop(tid, g_total_harts);
    
    if (tid == 0) {
        std::printf("Work stealing complete!\n");
        std::fflush(stdout);
    }
    
    barrier();
    
    // Record end time
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(end_cycles));
    }

    barrier();

    // Phase 3: Report statistics
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
        std::fflush(stdout);
        
        std::printf("\nSummary:\n");
        std::fflush(stdout);
        std::printf("  Total work processed: %ld\n", (long)total_processed);
        std::printf("  Total steal attempts: %ld\n", (long)total_steal_attempts);
        if (total_steal_attempts > 0) {
            std::printf("  Successful steals:    %ld (%ld%%)\n", 
                       (long)total_steal_success,
                       (long)(100 * total_steal_success / total_steal_attempts));
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

        std::printf("\nWorkqueue depth summary (sampled):\n");
        std::printf("Core | Samples | MinDepth | MaxDepth | AvgDepth | Capacity\n");
        std::printf("-----|---------|----------|----------|----------|---------\n");
        for (int c = 0; c < g_total_cores; c++) {
            int32_t n = g_wq_trace_count[c];
            int64_t min_d = QUEUE_SIZE;
            int64_t max_d = 0;
            int64_t sum_d = 0;
            if (n <= 0) {
                min_d = 0;
            }
            for (int32_t i = 0; i < n; i++) {
                int64_t d = g_wq_trace[c][i].depth;
                if (d < min_d) min_d = d;
                if (d > max_d) max_d = d;
                sum_d += d;
            }
            long avg_d = (n > 0) ? (long)(sum_d / n) : 0;
            std::printf("%4d | %7d | %8ld | %8ld | %8ld | %8d\n",
                        c, (int)n, (long)min_d, (long)max_d, avg_d, QUEUE_SIZE);
        }

        // Legacy machine-parseable trace block.
        std::printf("\nWORKQUEUE_TRACE_BEGIN\n");
        std::printf("core,sample,cycle,depth,capacity,head,tail\n");
        for (int c = 0; c < g_total_cores; c++) {
            int32_t n = g_wq_trace_count[c];
            for (int32_t i = 0; i < n; i++) {
                const WorkQueueTraceSample& s = g_wq_trace[c][i];
                std::printf("%d,%d,%lu,%ld,%d,%ld,%ld\n",
                            c, i, (unsigned long)s.cycle, (long)s.depth,
                            QUEUE_SIZE, (long)s.head, (long)s.tail);
            }
        }
        std::printf("WORKQUEUE_TRACE_END\n");

        // Unified WQTRACE dump (stdout + file), same format as BFS/PageRank work-stealing.
        dump_wqtrace(stdout, g_total_cores);

        char path[64];
        std::snprintf(path, sizeof(path), "run_%dcores.log", g_total_cores);
        FILE* fp = std::fopen(path, "w");
        if (fp != nullptr) {
            dump_wqtrace(fp, g_total_cores);
            std::fclose(fp);
            std::printf("WQTRACE_FILE_WRITTEN,bench=work_stealing_benchmark,path=%s\n", path);
        } else {
            std::printf("WQTRACE_FILE_ERROR,bench=work_stealing_benchmark,path=%s\n", path);
        }
    }

    barrier();
    
    return 0;
}
