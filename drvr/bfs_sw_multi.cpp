// parallel_bfs.cpp
#include <vector>
#include <atomic>
#include <thread>
#include <iostream>
#include <queue>
#include <chrono>

// Simple CSR graph
struct CSRGraph {
    int n;                          // number of vertices
    std::vector<int> offsets;       // size n+1
    std::vector<int> edges;         // size m
};

// Sequential BFS for baseline
std::vector<int> bfs_sequential(const CSRGraph& g, int source) {
    std::vector<int> dist(g.n, -1);
    std::queue<int> q;
    dist[source] = 0;
    q.push(source);

    while (!q.empty()) {
        int u = q.front(); q.pop();
        int du = dist[u];
        for (int ei = g.offsets[u]; ei < g.offsets[u+1]; ++ei) {
            int v = g.edges[ei];
            if (dist[v] == -1) {
                dist[v] = du + 1;
                q.push(v);
            }
        }
    }
    return dist;
}

// Parallel BFS using std::thread
std::vector<int> bfs_parallel(const CSRGraph& g, int source, int num_threads) {
    // distance array as atomics so multiple threads can race on v
    std::vector<std::atomic<int>> dist(g.n);
    for (int i = 0; i < g.n; ++i) dist[i].store(-1, std::memory_order_relaxed);

    std::vector<int> frontier;
    std::vector<int> next_frontier;

    dist[source].store(0, std::memory_order_relaxed);
    frontier.push_back(source);

    int cur_level = 0;

    while (!frontier.empty()) {
        next_frontier.clear();
        // per-thread local buffers to reduce contention on next_frontier
        std::vector<std::vector<int>> local_next(num_threads);

        auto worker = [&](int tid) {
            int sz = (int)frontier.size();
            // simple 1D block partition
            int chunk = (sz + num_threads - 1) / num_threads;
            int start = tid * chunk;
            int end   = std::min(sz, start + chunk);
            if (start >= end) return;

            for (int i = start; i < end; ++i) {
                int u = frontier[i];
                int du = dist[u].load(std::memory_order_relaxed);

                for (int ei = g.offsets[u]; ei < g.offsets[u+1]; ++ei) {
                    int v = g.edges[ei];
                    int expected = -1;
                    // first thread that flips -1 → du+1 wins and enqueues v
                    if (dist[v].compare_exchange_strong(
                            expected, du + 1,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
                        local_next[tid].push_back(v);
                    }
                }
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back(worker, t);
        }
        for (auto& th : threads) th.join();

        // merge local_next into global next_frontier
        size_t total_size = 0;
        for (int t = 0; t < num_threads; ++t) total_size += local_next[t].size();
        next_frontier.reserve(total_size);
        for (int t = 0; t < num_threads; ++t) {
            next_frontier.insert(
                next_frontier.end(),
                local_next[t].begin(),
                local_next[t].end());
        }

        frontier.swap(next_frontier);
        ++cur_level;
    }

    // convert atomic<int> → plain int
    std::vector<int> dist_out(g.n);
    for (int i = 0; i < g.n; ++i) {
        dist_out[i] = dist[i].load(std::memory_order_relaxed);
    }
    return dist_out;
}

// Example main: just a grid graph like your DRVR BFS
CSRGraph make_grid(int R, int C) {
    CSRGraph g;
    g.n = R * C;
    g.offsets.resize(g.n + 1);
    std::vector<int> edges;

    auto id = [C](int r, int c) { return r*C + c; };
    const int dr[4] = {-1, 1, 0, 0};
    const int dc[4] = {0, 0,-1, 1};

    int edge_count = 0;
    for (int u = 0; u < g.n; ++u) {
        g.offsets[u] = edge_count;
        int ur = u / C, uc = u % C;
        for (int k = 0; k < 4; ++k) {
            int vr = ur + dr[k];
            int vc = uc + dc[k];
            if (vr >= 0 && vr < R && vc >= 0 && vc < C) {
                edges.push_back(id(vr, vc));
                ++edge_count;
            }
        }
    }
    g.offsets[g.n] = edge_count;
    g.edges.swap(edges);
    return g;
}

int main(int argc, char** argv) {
    int R = 1020;
    int C = 102;
    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    CSRGraph g = make_grid(R, C);
    int source = 0;

    // Sequential baseline
    auto t0 = std::chrono::high_resolution_clock::now();
    auto dist_seq = bfs_sequential(g, source);
    auto t1 = std::chrono::high_resolution_clock::now();

    // Parallel BFS
    auto t2 = std::chrono::high_resolution_clock::now();
    auto dist_par = bfs_parallel(g, source, num_threads);
    auto t3 = std::chrono::high_resolution_clock::now();

    // Quick correctness check: distances should match
    bool ok = true;
    for (int i = 0; i < g.n; ++i) {
        if (dist_seq[i] != dist_par[i]) {
            std::cerr << "Mismatch at " << i
                      << ": seq=" << dist_seq[i]
                      << " par=" << dist_par[i] << "\n";
            ok = false;
            break;
        }
    }

    auto seq_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    auto par_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    std::cout << "Sequential BFS: " << seq_ms << " ms\n";
    std::cout << "Parallel BFS (" << num_threads << " threads): "
              << par_ms << " ms\n";
    std::cout << "Distances " << (ok ? "MATCH" : "DO NOT MATCH") << "\n";
    return ok ? 0 : 1;
}

