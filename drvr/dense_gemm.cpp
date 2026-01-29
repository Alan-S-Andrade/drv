#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>

// ---------- Configuration ----------

// Set to 1 to enable full O(N^3) verification by the master thread.
static const int DO_FULL_VERIFY = 1;

// Maximum Matrix Dimension (Square Matrix N x N)
// 256*256 * 4 bytes = 256KB per matrix.
static const int MAX_DIM = 256; 

// ---------- Helpers ----------

static int parse_i(const char* s, int d) {
    if (!s) return d;
    char* e = nullptr;
    long v = strtol(s, &e, 10);
    return (e && *e == 0) ? (int)v : d;
}

// Flattened 2D index: row * Width + col
static inline int idx(int r, int c, int width) { return r * width + c; }

// ---------- Barrier ----------

static const int MAX_THREADS = 1024;
static int64_t g_local_phase_arr[MAX_THREADS];

static volatile int64_t g_barrier_count = 0;
static volatile int64_t g_barrier_phase = 0;
static volatile int g_sim_exit = 0;

static inline void barrier(int tid, int total_threads) {
    int64_t my_phase = g_local_phase_arr[tid];

    int64_t old = atomic_fetch_add_i64(&g_barrier_count, 1);
    if (old == total_threads - 1) {
        atomic_fetch_add_i64(&g_barrier_count, -total_threads);
        atomic_fetch_add_i64(&g_barrier_phase, 1);
    } else {
        long w = 1, wmax = 8 * 1024;
        while (atomic_load_i64(&g_barrier_phase) == my_phase) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }
    g_local_phase_arr[tid] = my_phase + 1;
}

// ---------- Matrix State ----------

// Matrix Dimensions (N x N)
static int g_N; 
static int g_Size; // N * N

// Flattened Arrays: C = A * B
// Using int32_t to avoid FPU usage
static int32_t g_A[MAX_DIM * MAX_DIM];
static int32_t g_B[MAX_DIM * MAX_DIM];
static int32_t g_C[MAX_DIM * MAX_DIM];

extern "C" int main(int argc, char** argv) {
    int N = 64; // Default size
    int desired_threads = 16;

    // Argument Parsing
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--N") && i + 1 < argc) N = parse_i(argv[++i], N);
        else if (!strcmp(argv[i], "--T") && i + 1 < argc) desired_threads = parse_i(argv[++i], desired_threads);
        else if (!strcmp(argv[i], "--help")) {
            std::printf("Usage: %s --N <dim> --T <threads>\n", argv[0]);
            return 0;
        }
    }

    g_N = N;
    g_Size = N * N;

    if (g_N > MAX_DIM) {
        std::printf("Error: N=%d exceeds MAX_DIM=%d\n", g_N, MAX_DIM);
        return 1;
    }

    // ---------- Topology / Thread ID Calculation ----------

    const int hart_in_core    = myThreadId();    
    const int core_in_pod     = myCoreId();      
    const int pod_in_pxn      = myPodId();
    const int pxn_id          = myPXNId();
    const int harts_per_core  = myCoreThreads();
    const int cores_per_pod   = numPodCores();   
    const int pods_per_pxn    = numPXNPods();

    const int total_harts_hw =
        numPXN() * pods_per_pxn * cores_per_pod * harts_per_core;

    const int tid_global =
        ((((pxn_id * pods_per_pxn) + pod_in_pxn) * cores_per_pod + core_in_pod) * harts_per_core) + hart_in_core;

    int total_threads = desired_threads;
    if (total_threads > total_harts_hw) total_threads = total_harts_hw;
    if (total_threads > MAX_THREADS) total_threads = MAX_THREADS;

    // ---------- Thread Parking ----------
    // Park threads that are not participating in the calculation
    if (tid_global < 0 || tid_global >= total_threads) {
        while (g_sim_exit == 0) {
            hartsleep(1000);
        }
        return 0;
    }

    // Master Init Print
    if (tid_global == 0) {
        std::printf("Integer Matrix Mult: N=%d (Total elements per matrix: %d)\n", g_N, g_Size);
        std::printf("Hardware: Total Harts=%d\n", total_harts_hw);
        std::printf("Workload: Threads=%d\n", total_threads);
        std::printf("Verification: %s\n", DO_FULL_VERIFY ? "ENABLED" : "DISABLED");
    }

    // ---------- Initialization (Parallel) ----------
    // A[i,j] = (i + j) % 128
    // B[i,j] = (i - j) % 128
    // Modulo 128 keeps values small to prevent overflow during multiplication
    
    for (int i = tid_global; i < g_Size; i += total_threads) {
        int r = i / g_N;
        int c = i % g_N;
        
        g_A[i] = (r + c) % 128;      
        g_B[i] = (r - c) % 128;      
        g_C[i] = 0;                 
    }

    barrier(tid_global, total_threads);

    // ---------- Matrix Multiplication Kernel (Integer) ----------
    // Each thread computes a specific set of ROWS of C.

    for (int row = tid_global; row < g_N; row += total_threads) {
        for (int col = 0; col < g_N; ++col) {
            
            int32_t sum = 0;
            
            // Dot product of A[row] and B[col]
            for (int k = 0; k < g_N; ++k) {
                int32_t a_val = g_A[idx(row, k, g_N)];
                int32_t b_val = g_B[idx(k, col, g_N)];
                sum += a_val * b_val;
            }
            
            g_C[idx(row, col, g_N)] = sum;
        }
    }

    barrier(tid_global, total_threads);

    // ---------- Verification & Output ----------

    if (tid_global == 0) {
        std::printf("GEMM Calculation Complete.\n");
        
        if (DO_FULL_VERIFY) {
            std::printf("Running Verification (Serial Check)...\n");
            
            int error_count = 0;
            int max_print = 10;
            
            // Check full matrix
            for (int r = 0; r < g_N; ++r) {
                for (int c = 0; c < g_N; ++c) {
                    
                    // Recompute ground truth locally
                    int32_t expected = 0;
                    for (int k = 0; k < g_N; ++k) {
                        int32_t a = (r + k) % 128;
                        int32_t b = (k - c) % 128;
                        expected += a * b;
                    }
                    
                    int32_t actual = g_C[idx(r, c, g_N)];
                    
                    if (actual != expected) {
                        error_count++;
                        if (error_count <= max_print) {
                            std::printf("FAIL at [%d,%d]: Got %d, Expected %d\n", 
                                        r, c, actual, expected);
                        }
                    }
                }
            }

            if (error_count == 0) {
                int mid = g_N / 2;
                std::printf("Spot Check C[%d][%d] = %d\n", mid, mid, g_C[idx(mid, mid, g_N)]);
                std::printf("PASS\n");
            } else {
                std::printf("FAIL: Found %d errors.\n", error_count);
            }
        }

        // Trigger simulator exit
        std::printf("DBG: Master signaling exit.\n");
        std::fflush(stdout);
        g_sim_exit = 1;
    }

    // Wait for exit signal
    while (g_sim_exit == 0) {
        hartsleep(100);
    }

    return 0;
}
