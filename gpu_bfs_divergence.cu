/**
 * gpu_bfs_divergence.cu — GPU BFS demonstrating SIMD warp divergence penalty
 *
 * Level-synchronous BFS on CUDA.  Each warp processes 32 frontier vertices.
 * On a **regular** (constant-degree) graph every lane follows the same branch
 * pattern; on a **power-law RMAT** graph, lanes diverge heavily:
 *   - high-degree vertices loop many times (long inner loop)
 *   - low-degree / already-visited vertices exit early
 * The warp can only retire when its slowest lane finishes, so the
 * imbalance shows up directly as wasted SIMD cycles.
 *
 * Build:
 *   nvcc -O3 -arch=sm_80 gpu_bfs_divergence.cu -o gpu_bfs_divergence
 *
 * Usage:
 *   ./gpu_bfs_divergence <graph.bin>
 *
 * Binary CSR format (same as gen_rmat.py / gen_uniform_csr.py):
 *   header:  5 x int32 (N, num_edges, degree, unused, source)
 *   offsets: (N+1) x int32
 *   edges:   num_edges x int32
 *   degrees: N x int32
 */

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <chrono>

/* ------------------------------------------------------------------ */
/* Graph data (host + device)                                         */
/* ------------------------------------------------------------------ */
struct CSRGraph {
    int32_t  N;
    int32_t  num_edges;
    int32_t  avg_degree;
    int32_t  source;
    int32_t *offsets;   // device
    int32_t *edges;     // device
    int32_t *degrees;   // device
};

/* ------------------------------------------------------------------ */
/* BFS kernel — one thread per frontier vertex                        */
/*                                                                    */
/* Warp divergence happens inside the inner loop:                     */
/*   lane 0 may have 500 neighbors (RMAT hub vertex)                 */
/*   lane 1 may have 1 neighbor                                      */
/*   → lane 1 idles for 499 iterations of lane 0's loop              */
/*                                                                    */
/* When branches/conditionals differ across lanes, the warp           */
/* serialises both paths (predicated execution / divergent warps).    */
/* ------------------------------------------------------------------ */
__global__ void bfs_level_kernel(
    const int32_t *__restrict__ offsets,
    const int32_t *__restrict__ edges,
    const int32_t *frontier,        // current-level vertex list
    int32_t        frontier_size,
    int32_t       *visited,         // 1 = visited, 0 = unvisited
    int32_t       *dist,            // distance array
    int32_t        current_dist,
    int32_t       *next_frontier,   // output frontier
    int32_t       *next_frontier_size)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= frontier_size) return;

    int32_t v = frontier[tid];

    /* ---- inner loop: length varies wildly on power-law graphs ---- */
    int32_t start = offsets[v];
    int32_t end   = offsets[v + 1];

    for (int32_t e = start; e < end; ++e) {
        int32_t u = edges[e];

        /* Branch: only unvisited vertices proceed.                     *
         * On RMAT graphs many neighbors are already visited → some     *
         * lanes take the branch, others don't → warp divergence.       */
        int32_t old = atomicCAS(&visited[u], 0, 1);
        if (old == 0) {
            dist[u] = current_dist;
            int32_t pos = atomicAdd(next_frontier_size, 1);
            next_frontier[pos] = u;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Host helpers                                                       */
/* ------------------------------------------------------------------ */

#define CUDA_CHECK(call)                                               \
    do {                                                               \
        cudaError_t err = (call);                                      \
        if (err != cudaSuccess) {                                      \
            fprintf(stderr, "CUDA error %s:%d: %s\n",                  \
                    __FILE__, __LINE__, cudaGetErrorString(err));       \
            exit(EXIT_FAILURE);                                        \
        }                                                              \
    } while (0)

static CSRGraph load_graph(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }

    int32_t hdr[5];
    if (fread(hdr, sizeof(int32_t), 5, f) != 5) {
        fprintf(stderr, "Failed to read header from %s\n", path);
        exit(1);
    }

    CSRGraph g;
    g.N          = hdr[0];
    g.num_edges  = hdr[1];
    g.avg_degree = hdr[2];
    g.source     = hdr[4];

    printf("Graph: N=%d  edges=%d  avg_degree=%d  source=%d\n",
           g.N, g.num_edges, g.avg_degree, g.source);

    /* Read host arrays */
    std::vector<int32_t> h_offsets(g.N + 1);
    std::vector<int32_t> h_edges(g.num_edges);
    std::vector<int32_t> h_degrees(g.N);

    if (fread(h_offsets.data(), sizeof(int32_t), g.N + 1, f) != (size_t)(g.N + 1) ||
        fread(h_edges.data(),   sizeof(int32_t), g.num_edges, f) != (size_t)g.num_edges ||
        fread(h_degrees.data(), sizeof(int32_t), g.N, f) != (size_t)g.N) {
        fprintf(stderr, "Truncated graph file %s\n", path);
        exit(1);
    }
    fclose(f);

    /* Find highest-degree vertex for BFS source (matches drv convention) */
    int32_t max_deg = 0, best_src = 0;
    for (int32_t v = 0; v < g.N; ++v) {
        if (h_degrees[v] > max_deg) {
            max_deg = h_degrees[v];
            best_src = v;
        }
    }
    g.source = best_src;
    printf("BFS source: vertex %d (degree %d)\n", best_src, max_deg);

    /* Print degree distribution summary */
    int32_t min_deg = h_degrees[0], max_deg2 = h_degrees[0];
    double sum = 0;
    for (int32_t v = 0; v < g.N; ++v) {
        sum += h_degrees[v];
        if (h_degrees[v] < min_deg) min_deg = h_degrees[v];
        if (h_degrees[v] > max_deg2) max_deg2 = h_degrees[v];
    }
    printf("Degree distribution: min=%d  max=%d  avg=%.1f\n",
           min_deg, max_deg2, sum / g.N);

    /* Copy to device */
    CUDA_CHECK(cudaMalloc(&g.offsets, (g.N + 1) * sizeof(int32_t)));
    CUDA_CHECK(cudaMalloc(&g.edges,   g.num_edges * sizeof(int32_t)));
    CUDA_CHECK(cudaMalloc(&g.degrees, g.N * sizeof(int32_t)));

    CUDA_CHECK(cudaMemcpy(g.offsets, h_offsets.data(),
                           (g.N + 1) * sizeof(int32_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(g.edges, h_edges.data(),
                           g.num_edges * sizeof(int32_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(g.degrees, h_degrees.data(),
                           g.N * sizeof(int32_t), cudaMemcpyHostToDevice));
    return g;
}

/* ------------------------------------------------------------------ */
/* BFS driver (host)                                                  */
/* ------------------------------------------------------------------ */
struct BFSResult {
    double   elapsed_ms;
    int64_t  edges_traversed;
    int32_t  levels;
    int32_t  vertices_reached;
};

static BFSResult run_bfs(const CSRGraph &g) {
    const int BLOCK = 256;

    /* Allocate device arrays */
    int32_t *d_visited, *d_dist;
    int32_t *d_frontier[2], *d_frontier_size;
    CUDA_CHECK(cudaMalloc(&d_visited, g.N * sizeof(int32_t)));
    CUDA_CHECK(cudaMalloc(&d_dist,    g.N * sizeof(int32_t)));
    CUDA_CHECK(cudaMalloc(&d_frontier[0], g.N * sizeof(int32_t)));
    CUDA_CHECK(cudaMalloc(&d_frontier[1], g.N * sizeof(int32_t)));
    CUDA_CHECK(cudaMalloc(&d_frontier_size, sizeof(int32_t)));

    /* Initialise */
    CUDA_CHECK(cudaMemset(d_visited, 0, g.N * sizeof(int32_t)));
    std::vector<int32_t> h_dist(g.N, -1);
    CUDA_CHECK(cudaMemcpy(d_dist, h_dist.data(), g.N * sizeof(int32_t),
                           cudaMemcpyHostToDevice));

    /* Seed source */
    int32_t one = 1, zero = 0;
    CUDA_CHECK(cudaMemcpy(d_visited + g.source, &one, sizeof(int32_t),
                           cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_dist + g.source, &zero, sizeof(int32_t),
                           cudaMemcpyHostToDevice));
    int32_t src = g.source;
    CUDA_CHECK(cudaMemcpy(d_frontier[0], &src, sizeof(int32_t),
                           cudaMemcpyHostToDevice));

    int32_t h_frontier_size = 1;
    int     cur = 0;
    int32_t level = 1;
    int64_t total_edges = 0;

    cudaEvent_t t_start, t_stop;
    CUDA_CHECK(cudaEventCreate(&t_start));
    CUDA_CHECK(cudaEventCreate(&t_stop));
    CUDA_CHECK(cudaDeviceSynchronize());
    CUDA_CHECK(cudaEventRecord(t_start));

    while (h_frontier_size > 0) {
        int nxt = 1 - cur;
        CUDA_CHECK(cudaMemcpy(d_frontier_size, &zero, sizeof(int32_t),
                               cudaMemcpyHostToDevice));

        int grid = (h_frontier_size + BLOCK - 1) / BLOCK;
        bfs_level_kernel<<<grid, BLOCK>>>(
            g.offsets, g.edges,
            d_frontier[cur], h_frontier_size,
            d_visited, d_dist, level,
            d_frontier[nxt], d_frontier_size);
        CUDA_CHECK(cudaGetLastError());

        CUDA_CHECK(cudaMemcpy(&h_frontier_size, d_frontier_size,
                               sizeof(int32_t), cudaMemcpyDeviceToHost));

        total_edges += h_frontier_size;  // approximate
        cur = nxt;
        ++level;
    }

    CUDA_CHECK(cudaEventRecord(t_stop));
    CUDA_CHECK(cudaEventSynchronize(t_stop));

    float ms = 0;
    CUDA_CHECK(cudaEventElapsedTime(&ms, t_start, t_stop));

    /* Count reached vertices */
    std::vector<int32_t> h_visited(g.N);
    CUDA_CHECK(cudaMemcpy(h_visited.data(), d_visited, g.N * sizeof(int32_t),
                           cudaMemcpyDeviceToHost));
    int32_t reached = 0;
    for (int32_t v = 0; v < g.N; ++v) reached += h_visited[v];

    /* Count actual edges traversed (sum degrees of visited vertices on host) */
    std::vector<int32_t> h_off(g.N + 1);
    CUDA_CHECK(cudaMemcpy(h_off.data(), g.offsets, (g.N + 1) * sizeof(int32_t),
                           cudaMemcpyDeviceToHost));
    int64_t edges_traversed = 0;
    for (int32_t v = 0; v < g.N; ++v) {
        if (h_visited[v]) edges_traversed += (h_off[v + 1] - h_off[v]);
    }

    /* Cleanup */
    cudaFree(d_visited); cudaFree(d_dist);
    cudaFree(d_frontier[0]); cudaFree(d_frontier[1]);
    cudaFree(d_frontier_size);
    cudaEventDestroy(t_start); cudaEventDestroy(t_stop);

    BFSResult r;
    r.elapsed_ms      = ms;
    r.edges_traversed  = edges_traversed;
    r.levels           = level - 1;
    r.vertices_reached = reached;
    return r;
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <graph.bin> [num_runs]\n", argv[0]);
        return 1;
    }

    int num_runs = (argc >= 3) ? atoi(argv[2]) : 5;
    if (num_runs < 1) num_runs = 1;

    /* Print GPU info */
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    printf("GPU: %s  (SM %d.%d, %d SMs, warp size %d)\n",
           prop.name, prop.major, prop.minor,
           prop.multiProcessorCount, prop.warpSize);
    printf("Global memory: %.1f GB\n\n",
           prop.totalGlobalMem / (1024.0 * 1024.0 * 1024.0));

    CSRGraph g = load_graph(argv[1]);

    printf("\n=== BFS Benchmark (%d runs) ===\n", num_runs);

    double best_ms = 1e18;
    BFSResult best_r;
    for (int i = 0; i < num_runs; ++i) {
        BFSResult r = run_bfs(g);
        double mteps = r.edges_traversed / (r.elapsed_ms * 1000.0);
        printf("  Run %d: %.3f ms, %ld edges, %d levels, %d vertices reached, %.2f MTEPS\n",
               i, r.elapsed_ms, r.edges_traversed, r.levels, r.vertices_reached, mteps);
        if (r.elapsed_ms < best_ms) { best_ms = r.elapsed_ms; best_r = r; }
    }

    double best_mteps = best_r.edges_traversed / (best_ms * 1000.0);
    printf("\n--- BEST ---\n");
    printf("Time:           %.3f ms\n", best_ms);
    printf("Edges traversed: %ld\n", best_r.edges_traversed);
    printf("Levels:         %d\n", best_r.levels);
    printf("Vertices:       %d / %d reached\n", best_r.vertices_reached, g.N);
    printf("Throughput:     %.2f MTEPS\n", best_mteps);
    printf("Warp size:      %d (SIMD width — diverging warps waste these lanes)\n",
           prop.warpSize);

    cudaFree(g.offsets); cudaFree(g.edges); cudaFree(g.degrees);
    return 0;
}
