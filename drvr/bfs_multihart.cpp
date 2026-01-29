// Level-synchronous BFS on a 1,000 x 1,000 graph (Fixed Global ID & Exit)
// - Fix 1: Calculates unique tid_global across all Cores/Pods
// - Fix 2: Parks extra threads safely
// - Fix 3: Adds safe exit synchronization

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>    
#include <pandohammer/hartsleep.h>

// Max active threads we want to use for the algorithm
static constexpr int ACTIVE_THREADS = 16; 

// Max threads supported by the barrier array (safe upper bound)
static constexpr int MAX_SYS_THREADS = 1024;

// Global Exit Flag (prevents zombie threads from hanging or killing sim early)
static volatile int32_t g_program_done = 0;

static int64_t thread_phase_counter[MAX_SYS_THREADS];
static volatile int64_t global_barrier_count = 0;
static volatile int64_t global_barrier_phase = 0;

// Barrier using Global TID
static inline void barrier(int tid, int total_participants) {
    int64_t threads_cur_phase = thread_phase_counter[tid];

    int64_t old = atomic_fetch_add_i64(&global_barrier_count, 1);

    if (old == total_participants - 1) {
        // Last: reset count and advance phase
        atomic_swap_i64(&global_barrier_count, 0);      
        atomic_fetch_add_i64(&global_barrier_phase, 1);   
    } else {
        long w = 1;
        long wmax = 8 * 1024;
        while (global_barrier_phase == threads_cur_phase) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }

    thread_phase_counter[tid] = threads_cur_phase + 1;
}

static constexpr int ROWS = 100;
static constexpr int COLS = 100;
static constexpr int64_t N = int64_t(ROWS) * int64_t(COLS);

static inline int64_t id_of(int r, int c) { return int64_t(r) * COLS + c; }
static inline int row_of(int64_t id) { return int(id / COLS); }
static inline int col_of(int64_t id) { return int(id % COLS); }

// -------------------- BFS shared arrays --------------------
static uint32_t frontier_a[N];
static uint32_t frontier_b[N];

static volatile int64_t frontier_size = 0;
static volatile int64_t next_size = 0;

static volatile int64_t visited[N];
static int32_t dist_arr[N];

static uint32_t* frontier = frontier_a;
static uint32_t* next_frontier = frontier_b;

static volatile int64_t discovered = 0;

static inline bool claim_node(int64_t v) {
    const int64_t old = atomic_swap_i64(&visited[v], 1);
    return old == 0;
}

// -------------------- BFS --------------------
static void bfs(int tid_global, int total_threads, int64_t source_id) {
    
    // One-time init by master
    if (tid_global == 0) {
        // Reset barrier phases for active threads
        for (int i = 0; i < total_threads; i++) thread_phase_counter[i] = 0;

        for (int64_t i = 0; i < N; i++) {
            visited[i] = 0;
            dist_arr[i] = -1;
        }

        visited[source_id] = 1;
        dist_arr[source_id] = 0;
        frontier[0] = (uint32_t) source_id;

        frontier_size = 1;
        next_size = 0;
        discovered = 1;

        std::printf("BFS start: source=%ld (r=%d,c=%d), N=%ld, threads=%d\n",
                    (long)source_id, row_of(source_id), col_of(source_id), (long)N, total_threads);
    }

    barrier(tid_global, total_threads);

    int32_t level = 0;

    while (true) {
        barrier(tid_global, total_threads);

        const int64_t fsz = frontier_size;
        if (fsz == 0) break;

        // Slice current frontier among active threads
        const int64_t begin = (fsz * tid_global) / total_threads;
        const int64_t end   = (fsz * (tid_global + 1)) / total_threads;

        for (int64_t i = begin; i < end; i++) {
            const int64_t u = frontier[i];
            const int ur = row_of(u);
            const int uc = col_of(u);

            // Neighbors: Up, Down, Left, Right
            // Helper lambda to reduce copy-paste
            auto process_neighbor = [&](int64_t v) {
                if (claim_node(v)) {
                    dist_arr[v] = level + 1;
                    const int64_t idx = atomic_fetch_add_i64(&next_size, 1);
                    next_frontier[idx] = (uint32_t) v;
                    atomic_fetch_add_i64(&discovered, 1);
                }
            };

            if (ur > 0) process_neighbor(u - COLS);
            if (ur + 1 < ROWS) process_neighbor(u + COLS);
            if (uc > 0) process_neighbor(u - 1);
            if (uc + 1 < COLS) process_neighbor(u + 1);
        }

        barrier(tid_global, total_threads);

        if (tid_global == 0) {
            const int64_t new_fsz = atomic_swap_i64(&next_size, 0);

            uint32_t* tmp = frontier;
            frontier = next_frontier;
            next_frontier = tmp;

            frontier_size = new_fsz;
            std::printf("level=%d next_frontier_size=%ld discovered=%ld\n", level, (long) new_fsz, (long) discovered);
            level++;
        }

        barrier(tid_global, total_threads);
    }

    barrier(tid_global, total_threads);

    if (tid_global == 0) {
        std::printf("BFS done. Levels=%d, discovered=%ld (grid should reach %ld)\n", level, (long) discovered, (long) N);
        const int64_t far = id_of(ROWS - 1, COLS - 1);
        std::printf("dist[(%d,%d)] = %d (expected %d)\n", ROWS - 1, COLS - 1, dist_arr[far], (ROWS - 1) + (COLS - 1));
    }
}

int main(int argc, char** argv) {
    // 1. Calculate Topology-Aware Global ID
    const int hart_in_core    = myThreadId();      
    const int core_in_pod     = myCoreId();        
    const int pod_in_pxn      = myPodId();
    const int pxn_id          = myPXNId();
    
    const int harts_per_core  = myCoreThreads();
    const int cores_per_pod   = numPodCores();     
    const int pods_per_pxn    = numPXNPods();

    // The unique global ID
    const int tid_global = 
        ((((pxn_id * pods_per_pxn) + pod_in_pxn) * cores_per_pod + core_in_pod) * harts_per_core) + hart_in_core;

    // 2. Filter Active vs Parked Threads
    // Only allow IDs 0 to ACTIVE_THREADS-1 to participate
    if (tid_global >= ACTIVE_THREADS) {
        // "Park" extra hardware threads safely so they don't exit early
        while (atomic_load_i32(&g_program_done) == 0) {
            hartsleep(1000); 
        }
        return 0; 
    }

    // 3. Run BFS (Only active threads reach here)
    bfs(tid_global, ACTIVE_THREADS, id_of(0, 0));

    // 4. Safe Exit Sequence (Fixes the "missing print" and "race to exit")
    if (tid_global == 0) {
        std::fflush(stdout);     // Force output to screen
        hartsleep(5000);         // Wait 5ms for I/O to propagate
        
        // Signal all parked/waiting threads to exit
        atomic_swap_i32(&g_program_done, 1); 
    } else {
        // Active threads wait here for master to finish printing
        while (atomic_load_i32(&g_program_done) == 0) {
            hartsleep(1000);
        }
    }

    return 0;
}
