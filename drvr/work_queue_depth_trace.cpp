#include <atomic>
#include <cstdint>
#include <cstdio>

#include <pandohammer/address.h>
#include <pandohammer/atomic.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/hartsleep.h>
#include <pandohammer/mmio.h>
#include <pandohammer/staticdecl.h>

#define __dram__ __attribute__((section(".dram")))

static constexpr int WORK_CHUNK_SIZE = 16;
static constexpr int WORK_UNIT_ITERS = 1024;
static constexpr int INITIAL_TARGET_CHUNKS_PER_CORE = 24;
static constexpr int STEAL_BATCH = 32;
static constexpr int MAX_CORES = 64;
static constexpr int MAX_HARTS = 1024;
static constexpr int MAX_TRACE_EVENTS = 8192;
static constexpr uintptr_t L2SP_GUARD_BYTES = 4096;

enum TraceOp : int32_t {
    TRACE_INIT = 0,
    TRACE_PUSH = 1,
    TRACE_POP = 2,
    TRACE_STEAL = 3,
    TRACE_REFILL = 4,
};

struct WorkQueue {
    volatile int64_t head;
    volatile int64_t tail;
    int64_t capacity;
    volatile int64_t* items;
};

struct DepthTraceEvent {
    uint64_t cycle;
    int32_t actor_core;
    int32_t actor_thread;
    int32_t target_core;
    int32_t depth;
    int32_t op;
    int32_t peer_core;
};

struct alignas(64) CoreLocalSum {
    volatile int64_t value;
};

__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ volatile int g_total_work_units = 0;
__l2sp__ volatile int g_queue_capacity = 0;
__l2sp__ volatile int g_seed_core0_chunks = 0;
__l2sp__ volatile int g_target_chunks_per_core = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

__l2sp__ WorkQueue core_queues[MAX_CORES];
__l2sp__ CoreLocalSum g_core_sum[MAX_CORES];
__l2sp__ std::atomic<int> core_thief[MAX_CORES];

__l2sp__ int64_t g_local_sense[MAX_HARTS];
__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;

__l2sp__ volatile int64_t stat_work_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_attempts[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_success[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_failures[MAX_HARTS];

__dram__ DepthTraceEvent g_trace_events[MAX_TRACE_EVENTS];
__dram__ std::atomic<int32_t> g_trace_count = 0;
__dram__ std::atomic<int32_t> g_trace_dropped = 0;

extern "C" char l2sp_end[];

static inline int64_t pack_range(int32_t begin, int32_t end) {
    return ((int64_t)(uint32_t)begin << 32) | (int64_t)(uint32_t)end;
}

static inline int32_t range_begin(int64_t packed) {
    return (int32_t)(packed >> 32);
}

static inline int32_t range_end(int64_t packed) {
    return (int32_t)(packed & 0xFFFFFFFF);
}

static inline int get_thread_id() {
    return myCoreId() * myCoreThreads() + myThreadId();
}

static inline uintptr_t align_up_uintptr(uintptr_t value, uintptr_t align) {
    return (value + align - 1) & ~(align - 1);
}

static inline uint64_t read_cycle() {
    uint64_t cycle = 0;
    asm volatile("rdcycle %0" : "=r"(cycle));
    return cycle;
}

static inline const char* trace_op_name(int32_t op) {
    switch (op) {
        case TRACE_INIT:
            return "init";
        case TRACE_PUSH:
            return "push";
        case TRACE_POP:
            return "pop";
        case TRACE_STEAL:
            return "steal";
        case TRACE_REFILL:
            return "refill";
        default:
            return "unknown";
    }
}

static inline void barrier() {
    const int tid = get_thread_id();
    const int total = g_total_harts;
    int64_t local = g_local_sense[tid];
    local ^= 1;
    g_local_sense[tid] = local;

    const int64_t old = g_count.fetch_add(1, std::memory_order_acq_rel);
    if (old == total - 1) {
        g_count.store(0, std::memory_order_relaxed);
        g_sense.store(local, std::memory_order_release);
        return;
    }

    long backoff = 1;
    const long max_backoff = 64 * total;
    while (g_sense.load(std::memory_order_acquire) != local) {
        hartsleep(backoff);
        if (backoff < max_backoff) {
            backoff <<= 1;
        }
    }
}

static inline void queue_init(WorkQueue* q, volatile int64_t* items, int64_t capacity) {
    q->head = 0;
    q->tail = 0;
    q->capacity = capacity;
    q->items = items;
}

static inline int32_t queue_depth(WorkQueue* q) {
    const int64_t head = atomic_load_i64(&q->head);
    const int64_t tail = atomic_load_i64(&q->tail);
    if (tail <= head) {
        return 0;
    }
    return (int32_t)(tail - head);
}

static inline void record_depth_change(int target_core, TraceOp op, int actor_tid, int peer_core) {
    const int32_t idx = g_trace_count.fetch_add(1, std::memory_order_acq_rel);
    if (idx >= MAX_TRACE_EVENTS) {
        g_trace_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    DepthTraceEvent& event = g_trace_events[idx];
    event.cycle = (op == TRACE_INIT) ? 0 : read_cycle();
    if (actor_tid >= 0) {
        event.actor_core = actor_tid / g_harts_per_core;
        event.actor_thread = actor_tid % g_harts_per_core;
    } else {
        event.actor_core = -1;
        event.actor_thread = -1;
    }
    event.target_core = target_core;
    event.depth = queue_depth(&core_queues[target_core]);
    event.op = op;
    event.peer_core = peer_core;
}

static inline bool queue_push(WorkQueue* q, int core_id, int actor_tid, int64_t work, TraceOp op, int peer_core) {
    const int64_t tail = q->tail;
    if (tail >= q->capacity) {
        return false;
    }
    q->items[tail] = work;
    q->tail = tail + 1;
    record_depth_change(core_id, op, actor_tid, peer_core);
    return true;
}

static inline bool queue_push_atomic(WorkQueue* q, int core_id, int actor_tid, int64_t work, int peer_core) {
    while (true) {
        const int64_t tail = atomic_load_i64(&q->tail);
        if (tail >= q->capacity) {
            return false;
        }
        const int64_t old_tail = atomic_compare_and_swap_i64(&q->tail, tail, tail + 1);
        if (old_tail == tail) {
            q->items[tail] = work;
            record_depth_change(core_id, TRACE_REFILL, actor_tid, peer_core);
            return true;
        }
        hartsleep(1);
    }
}

static inline int64_t queue_pop(WorkQueue* q, int core_id, int actor_tid) {
    int64_t backoff = 1;
    const int64_t max_backoff = 32;
    int retries = 0;

    while (retries < 16) {
        const int64_t tail = atomic_load_i64(&q->tail);
        if (tail == 0) {
            return -1;
        }

        int64_t head = atomic_load_i64(&q->head);
        if (head >= tail) {
            return -1;
        }

        const int64_t new_tail = tail - 1;
        const int64_t old_tail = atomic_compare_and_swap_i64(&q->tail, tail, new_tail);
        if (old_tail != tail) {
            retries++;
            hartsleep(backoff);
            if (backoff < max_backoff) {
                backoff <<= 1;
            }
            continue;
        }

        head = atomic_load_i64(&q->head);
        if (head <= new_tail) {
            const int64_t work = q->items[new_tail];
            record_depth_change(core_id, TRACE_POP, actor_tid, -1);
            return work;
        }

        const int64_t old_head = atomic_compare_and_swap_i64(&q->head, head, head + 1);
        if (old_head == head && head == new_tail) {
            atomic_compare_and_swap_i64(&q->tail, new_tail, 0);
            atomic_compare_and_swap_i64(&q->head, head + 1, 0);
            record_depth_change(core_id, TRACE_POP, actor_tid, -1);
            return q->items[new_tail];
        }

        atomic_compare_and_swap_i64(&q->tail, new_tail, 0);
        atomic_compare_and_swap_i64(&q->head, head + 1, 0);
        atomic_compare_and_swap_i64(&q->head, head, 0);
        return -1;
    }

    return -1;
}

static inline int64_t queue_steal_k(WorkQueue* q, int victim_core, int actor_tid, int64_t* out_begin, int64_t max_k) {
    int64_t backoff = 1;
    const int64_t max_backoff = 32;
    int retries = 0;

    while (retries < 8) {
        const int64_t head = atomic_load_i64(&q->head);
        const int64_t tail = atomic_load_i64(&q->tail);
        if (head >= tail) {
            return 0;
        }

        const int64_t available = tail - head;
        const int64_t stolen = (available < max_k) ? available : max_k;
        const int64_t old_head = atomic_compare_and_swap_i64(&q->head, head, head + stolen);
        if (old_head == head) {
            *out_begin = head;
            record_depth_change(victim_core, TRACE_STEAL, actor_tid, actor_tid / g_harts_per_core);
            return stolen;
        }

        retries++;
        hartsleep(backoff);
        if (backoff < max_backoff) {
            backoff <<= 1;
        }
    }

    return 0;
}

static inline uint32_t xorshift_victim(uint32_t seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static inline void do_work_range(int32_t begin, int32_t end, int core_id) {
    volatile int64_t* sum = &g_core_sum[core_id].value;
    for (int32_t work = begin; work < end; work++) {
        for (int64_t i = 0; i < WORK_UNIT_ITERS; i++) {
            *sum += ((int64_t)work + i) & 0xF;
        }
    }
}

static void distribute_work_imbalanced(int tid) {
    if (tid != 0) {
        return;
    }

    int32_t next_work_unit = 0;
    for (int chunk = 0; chunk < g_seed_core0_chunks; chunk++) {
        const int32_t begin = next_work_unit;
        const int32_t end = begin + WORK_CHUNK_SIZE;
        next_work_unit = end;
        queue_push(&core_queues[0], 0, tid, pack_range(begin, end), TRACE_PUSH, -1);
    }

    g_total_work_units = next_work_unit;
}

static inline int pick_busiest_victim(int my_core, int* victim_depth) {
    int best_core = -1;
    int best_depth = 0;
    for (int core = 0; core < g_total_cores; core++) {
        if (core == my_core) {
            continue;
        }
        const int depth = queue_depth(&core_queues[core]);
        if (depth > best_depth) {
            best_depth = depth;
            best_core = core;
        }
    }
    *victim_depth = best_depth;
    return best_core;
}

static void work_steal_loop(int tid) {
    std::atomic_thread_fence(std::memory_order_acquire);

    const int my_core = tid / g_harts_per_core;
    WorkQueue* my_queue = &core_queues[my_core];

    int64_t local_processed = 0;
    int64_t local_steal_attempts = 0;
    int64_t local_steal_success = 0;
    int64_t local_steal_failures = 0;
    int64_t stolen_buf[STEAL_BATCH];

    int consecutive_empty = 0;
    const int max_empty_rounds = g_total_cores * 8 + 16;
    int64_t backoff = 1;

    while (consecutive_empty < max_empty_rounds) {
        const int64_t packed = queue_pop(my_queue, my_core, tid);
        if (packed >= 0) {
            do_work_range(range_begin(packed), range_end(packed), my_core);
            local_processed += (range_end(packed) - range_begin(packed));
            consecutive_empty = 0;
            backoff = 1;
            continue;
        }

        if (core_thief[my_core].exchange(1, std::memory_order_acquire) == 0) {
            bool found_work = false;
            const int my_depth = queue_depth(my_queue);
            int victim_depth = 0;
            const int victim = pick_busiest_victim(my_core, &victim_depth);
            if (victim >= 0 && victim_depth > my_depth + 1) {
                local_steal_attempts++;
                int64_t steal_begin = 0;
                int64_t desired = (victim_depth - my_depth) / 2;
                if (desired < 1) {
                    desired = 1;
                }
                const int64_t local_room = my_queue->capacity - queue_depth(my_queue);
                if (desired > local_room) {
                    desired = local_room;
                }
                if (desired > STEAL_BATCH) {
                    desired = STEAL_BATCH;
                }
                const int64_t stolen = (desired > 0)
                    ? queue_steal_k(&core_queues[victim], victim, tid, &steal_begin, desired)
                    : 0;
                if (stolen == 0) {
                    local_steal_failures++;
                    hartsleep(backoff);
                } else {
                    local_steal_success++;
                    for (int64_t i = 0; i < stolen; i++) {
                        stolen_buf[i] = core_queues[victim].items[steal_begin + i];
                    }
                    for (int64_t i = 1; i < stolen; i++) {
                        queue_push_atomic(my_queue, my_core, tid, stolen_buf[i], victim);
                    }

                    do_work_range(range_begin(stolen_buf[0]), range_end(stolen_buf[0]), my_core);
                    local_processed += (range_end(stolen_buf[0]) - range_begin(stolen_buf[0]));
                    consecutive_empty = 0;
                    backoff = 1;
                    found_work = true;
                }
            }

            core_thief[my_core].store(0, std::memory_order_release);
            if (!found_work) {
                consecutive_empty++;
                hartsleep(backoff);
                if (backoff < 64) {
                    backoff <<= 1;
                }
            }
            continue;
        }

        consecutive_empty++;
        hartsleep(backoff * 4);
        if (backoff < 64) {
            backoff <<= 1;
        }
    }

    stat_work_processed[tid] = local_processed;
    stat_steal_attempts[tid] = local_steal_attempts;
    stat_steal_success[tid] = local_steal_success;
    stat_steal_failures[tid] = local_steal_failures;
}

static void dump_depth_trace_csv() {
    const int32_t total_events = g_trace_count.load(std::memory_order_acquire);
    const int32_t emitted = (total_events > MAX_TRACE_EVENTS) ? MAX_TRACE_EVENTS : total_events;
    const int32_t dropped = g_trace_dropped.load(std::memory_order_acquire);

    char path[96];
    std::snprintf(path, sizeof(path), "run_work_queue_depth_trace_%dcores.csv", g_total_cores);

    FILE* fp = std::fopen(path, "w");
    if (fp == nullptr) {
        std::printf("WQDEPTH_TRACE_FILE_ERROR,path=%s\n", path);
        return;
    }

    std::fprintf(fp,
                 "# bench=work_queue_depth_trace,cores=%d,harts_per_core=%d,total_harts=%d,total_work_units=%d,events=%d,dropped=%d\n",
                 g_total_cores,
                 g_harts_per_core,
                 g_total_harts,
                 g_total_work_units,
                 emitted,
                 dropped);
    std::fprintf(fp, "event,cycle,actor_core,actor_thread,target_core,depth,op,peer_core\n");
    for (int32_t i = 0; i < emitted; i++) {
        const DepthTraceEvent& event = g_trace_events[i];
        std::fprintf(fp,
                     "%d,%llu,%d,%d,%d,%d,%s,%d\n",
                     (int)i,
                     (unsigned long long)event.cycle,
                     event.actor_core,
                     event.actor_thread,
                     event.target_core,
                     event.depth,
                     trace_op_name(event.op),
                     event.peer_core);
    }

    std::fclose(fp);
    std::printf("WQDEPTH_TRACE_FILE_WRITTEN,path=%s,events=%d,dropped=%d\n",
                path,
                emitted,
                dropped);
}

int main(int argc, char** argv) {
    const int total_cores = numPodCores();
    const int hw_harts_per_core = myCoreThreads();

    if (myThreadId() != 0) {
        return 0;
    }

    const int harts_per_core = 1;
    const int total_harts = total_cores;
    const int tid = myCoreId();

    if (tid == 0) {
        if (total_cores > MAX_CORES || total_harts > MAX_HARTS) {
            std::printf("ERROR: hardware topology exceeds benchmark limits (cores=%d/%d, harts=%d/%d)\n",
                        total_cores,
                        MAX_CORES,
                        total_harts,
                        MAX_HARTS);
            return 1;
        }

        g_total_cores = total_cores;
        g_harts_per_core = harts_per_core;
        g_total_harts = total_harts;
        g_total_work_units = 0;
        g_queue_capacity = 0;
        g_seed_core0_chunks = 0;
        g_target_chunks_per_core = 0;

        for (int hart = 0; hart < g_total_harts; hart++) {
            g_local_sense[hart] = 0;
            stat_work_processed[hart] = 0;
            stat_steal_attempts[hart] = 0;
            stat_steal_success[hart] = 0;
            stat_steal_failures[hart] = 0;
        }

        g_trace_count.store(0, std::memory_order_relaxed);
        g_trace_dropped.store(0, std::memory_order_relaxed);

        uintptr_t heap = align_up_uintptr((uintptr_t)l2sp_end, alignof(int64_t));
        const uintptr_t l2sp_base = 0x20000000;
        const uintptr_t l2sp_limit = l2sp_base + podL2SPSize();
        const uintptr_t usable_limit = (l2sp_limit > L2SP_GUARD_BYTES)
            ? (l2sp_limit - L2SP_GUARD_BYTES)
            : l2sp_limit;

        if (heap >= usable_limit) {
            std::printf("ERROR: no dynamic L2SP space left for work queues\n");
            return 1;
        }

        const uintptr_t total_queue_bytes = usable_limit - heap;
        const int64_t bytes_per_queue = (uintptr_t)g_total_cores > 0
            ? (int64_t)(total_queue_bytes / (uintptr_t)g_total_cores)
            : 0;
        const int64_t queue_capacity = bytes_per_queue / (int64_t)sizeof(int64_t);

        if (queue_capacity < STEAL_BATCH + 1) {
            std::printf("ERROR: dynamic queue capacity too small (%ld entries/core)\n",
                        (long)queue_capacity);
            return 1;
        }

        g_queue_capacity = (int)queue_capacity;
        g_target_chunks_per_core = INITIAL_TARGET_CHUNKS_PER_CORE;
        if (g_target_chunks_per_core < 1) {
            g_target_chunks_per_core = 1;
        }
        g_seed_core0_chunks = g_target_chunks_per_core * g_total_cores;
        if (g_seed_core0_chunks > g_queue_capacity) {
            g_seed_core0_chunks = g_queue_capacity;
            g_target_chunks_per_core = g_seed_core0_chunks / g_total_cores;
        }
        if (g_target_chunks_per_core < 1) {
            g_target_chunks_per_core = 1;
        }

        for (int core = 0; core < g_total_cores; core++) {
            queue_init(&core_queues[core], (volatile int64_t*)heap, queue_capacity);
            heap += (uintptr_t)queue_capacity * sizeof(int64_t);
            g_core_sum[core].value = 0;
            core_thief[core].store(0, std::memory_order_relaxed);
            record_depth_change(core, TRACE_INIT, tid, -1);
        }

        std::printf("=== Work Queue Depth Trace ===\n");
        std::printf("Hardware: %d cores x %d harts/core, using %d active hart/core = %d active harts\n",
                    g_total_cores,
                    hw_harts_per_core,
                    g_harts_per_core,
                    g_total_harts);
        std::printf("Work queue trace: queue capacity=%d chunks/core, seeded core0=%d, target/core=%d, chunk size=%d\n",
                    g_queue_capacity,
                    g_seed_core0_chunks,
                    g_target_chunks_per_core,
                    WORK_CHUNK_SIZE);

        std::atomic_thread_fence(std::memory_order_release);
        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }

    barrier();
    distribute_work_imbalanced(tid);
    barrier();

    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;
    if (tid == 0) {
        start_cycles = read_cycle();
    }

    work_steal_loop(tid);

    barrier();
    if (tid == 0) {
        end_cycles = read_cycle();
    }
    barrier();

    if (tid == 0) {
        int64_t total_processed = 0;
        int64_t total_attempts = 0;
        int64_t total_success = 0;
        int64_t total_failures = 0;

        for (int hart = 0; hart < g_total_harts; hart++) {
            total_processed += stat_work_processed[hart];
            total_attempts += stat_steal_attempts[hart];
            total_success += stat_steal_success[hart];
            total_failures += stat_steal_failures[hart];
        }

        std::printf("Processed work units: %ld / %d\n",
                    (long)total_processed,
                    g_total_work_units);
        std::printf("Steal attempts: %ld, successes: %ld, failures: %ld\n",
                    (long)total_attempts,
                    (long)total_success,
                    (long)total_failures);
        std::printf("Elapsed cycles: %llu\n", (unsigned long long)(end_cycles - start_cycles));

        dump_depth_trace_csv();
    }

    barrier();
    return 0;
}
