// gen_uniform_graph.cpp
// Generates a uniform random graph in Ligra's AdjacencyGraph text format.
// Matches the graph generation in DRV's bfs_csr_weak.cpp:
//   - Fixed out-degree per vertex (default 16)
//   - Uniform random edge targets using xorshift32 PRNG
//   - Same seed formula: seed = v * degree + e + 1
//
// Usage: gen_uniform_graph [-d <degree>] [-s] <N> <outfile>
//   -d <degree>  : out-degree per vertex (default 16)
//   -s           : symmetric (add reverse edges, deduplicate)
//   N            : number of vertices
//   outfile      : output file path
//
// Output: Ligra AdjacencyGraph format (text)
//   Line 1: "AdjacencyGraph"
//   Line 2: N
//   Line 3: M (total edges)
//   Lines 4..N+3: CSR offsets
//   Lines N+4..N+M+3: Edge destinations

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <set>

static inline uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(int argc, char *argv[]) {
    int degree = 16;
    bool symmetric = false;
    char *outfile = nullptr;
    long N = 0;

    // Parse arguments
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            degree = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0) {
            symmetric = true;
        } else {
            if (positional == 0) {
                N = atol(argv[i]);
                positional++;
            } else if (positional == 1) {
                outfile = argv[i];
                positional++;
            }
        }
    }

    if (N <= 0 || !outfile) {
        fprintf(stderr, "Usage: %s [-d <degree>] [-s] <N> <outfile>\n", argv[0]);
        return 1;
    }

    fprintf(stderr, "Generating uniform random graph: N=%ld, degree=%d, symmetric=%d\n",
            N, degree, symmetric);

    if (!symmetric) {
        // Directed graph: exactly N*degree edges, fixed out-degree
        long M = (long)N * degree;
        FILE *fp = fopen(outfile, "w");
        if (!fp) { perror("fopen"); return 1; }

        fprintf(fp, "AdjacencyGraph\n");
        fprintf(fp, "%ld\n", N);
        fprintf(fp, "%ld\n", M);

        // Write offsets
        for (long v = 0; v < N; v++) {
            fprintf(fp, "%ld\n", v * degree);
        }

        // Write edges (same PRNG as DRV's bfs_csr_weak.cpp)
        for (long v = 0; v < N; v++) {
            for (int e = 0; e < degree; e++) {
                uint32_t seed = (uint32_t)(v * degree + e + 1);
                xorshift32(&seed);
                long target = (long)(seed % (uint32_t)N);
                fprintf(fp, "%ld\n", target);
            }
        }

        fclose(fp);
        fprintf(stderr, "Wrote directed graph: %ld vertices, %ld edges\n", N, M);
    } else {
        // Symmetric: generate forward edges, then add reverse edges, deduplicate
        // Build adjacency lists
        std::vector<std::set<long>> adj(N);

        for (long v = 0; v < N; v++) {
            for (int e = 0; e < degree; e++) {
                uint32_t seed = (uint32_t)(v * degree + e + 1);
                xorshift32(&seed);
                long target = (long)(seed % (uint32_t)N);
                adj[v].insert(target);
                adj[target].insert(v);  // reverse edge
            }
        }

        // Count total edges
        long M = 0;
        for (long v = 0; v < N; v++) {
            M += (long)adj[v].size();
        }

        FILE *fp = fopen(outfile, "w");
        if (!fp) { perror("fopen"); return 1; }

        fprintf(fp, "AdjacencyGraph\n");
        fprintf(fp, "%ld\n", N);
        fprintf(fp, "%ld\n", M);

        // Write offsets
        long offset = 0;
        for (long v = 0; v < N; v++) {
            fprintf(fp, "%ld\n", offset);
            offset += (long)adj[v].size();
        }

        // Write edges
        for (long v = 0; v < N; v++) {
            for (long u : adj[v]) {
                fprintf(fp, "%ld\n", u);
            }
        }

        fclose(fp);
        fprintf(stderr, "Wrote symmetric graph: %ld vertices, %ld edges\n", N, M);
    }

    return 0;
}
