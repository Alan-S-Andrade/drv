// BFS_PushOnly.C
// Push-only (top-down) level-synchronous BFS for Ligra.
// Matches the algorithm used in DRV's bfs_csr_weak.cpp:
//   - Push-only traversal (no direction-optimizing / pull phase)
//   - Tracks distance (level) rather than parent
//   - Uses atomic CAS for distance updates
//   - Level-synchronous with implicit barrier between levels
//
// Uses Ligra's edgeMap with the no_dense flag to force sparse/push-only mode.
//
// Usage: ./BFS_PushOnly [-s] [-r <root>] [-rounds <n>] <graphFile>

#include "ligra.h"
#include "imc_counters.h"
#include <time.h>

// Global IMC counters — initialized once, reused across Compute() rounds
static ImcCounters g_imc;
static bool g_imc_init = false;

struct BFS_PushOnly_F {
  uintE* Dist;
  uintE level;
  BFS_PushOnly_F(uintE* _Dist, uintE _level) : Dist(_Dist), level(_level) {}

  // Dense (pull) update -- should never be called due to no_dense flag
  inline bool update(uintE s, uintE d) {
    if (Dist[d] == UINT_E_MAX) {
      Dist[d] = level;
      return 1;
    }
    return 0;
  }

  // Sparse (push) update -- atomic CAS matching DRV's atomic_compare_and_swap
  inline bool updateAtomic(uintE s, uintE d) {
    return CAS(&Dist[d], (uintE)UINT_E_MAX, level);
  }

  // Condition: vertex not yet visited
  inline bool cond(uintE d) { return Dist[d] == UINT_E_MAX; }
};

template <class vertex>
void Compute(graph<vertex>& GA, commandLine P) {
  long start = P.getOptionLongValue("-r", 0);
  long n = GA.n;
  long m = GA.m;

  // One-time IMC counter initialization
  if (!g_imc_init) {
    g_imc.init();
    g_imc_init = true;
  }

  uintE* Dist = newA(uintE, n);
  parallel_for(long i = 0; i < n; i++) Dist[i] = UINT_E_MAX;
  Dist[start] = 0;

  vertexSubset Frontier(n, start);
  uintE level = 1;
  long totalEdgesTraversed = 0;

  // --- start IMC + wall-clock measurement around BFS kernel only ---
  struct timespec ts0, ts1;
  if (g_imc.available()) {
    g_imc.reset();
    g_imc.enable();
  }
  clock_gettime(CLOCK_MONOTONIC, &ts0);

  while (!Frontier.isEmpty()) {
    // Count edges traversed this level (sum of out-degrees of frontier vertices)
    long frontierSize = Frontier.numNonzeros();

    // Push-only edgeMap: no_dense flag prevents switching to pull/dense mode
    vertexSubset output = edgeMap(GA, Frontier, BFS_PushOnly_F(Dist, level),
                                  -1, no_dense);
    totalEdgesTraversed += frontierSize; // approximate: count frontier vertices
    Frontier.del();
    Frontier = output;
    level++;
  }

  clock_gettime(CLOCK_MONOTONIC, &ts1);
  if (g_imc.available()) g_imc.disable();

  double bfs_sec = (ts1.tv_sec - ts0.tv_sec)
                 + (ts1.tv_nsec - ts0.tv_nsec) * 1e-9;
  uint64_t imc_rb = 0, imc_wb = 0;
  uint64_t llc_miss = 0;
  uint64_t l1d_pending = 0, cpu_cycles = 0;
  uint64_t stalls_mem = 0, stalls_l3 = 0;
  uint64_t inst_ret = 0, uops_slots = 0, exe0 = 0;
  if (g_imc.available()) {
    imc_rb = g_imc.read_bytes();
    imc_wb = g_imc.write_bytes();
  }
  if (g_imc.llc_available()) {
    llc_miss = g_imc.llc_misses();
  }
  if (g_imc.concurrency_available()) {
    l1d_pending = g_imc.l1d_pending();
    cpu_cycles  = g_imc.cpu_cycles();
  }
  if (g_imc.stalls_available()) {
    stalls_mem = g_imc.stalls_mem_any();
    stalls_l3  = g_imc.stalls_l3_miss();
  }
  if (g_imc.core_util_available()) {
    inst_ret   = g_imc.inst_retired();
    uops_slots = g_imc.uops_retired_slots();
    exe0       = g_imc.exe_bound_0_ports();
  }
  // --- end IMC measurement ---

  Frontier.del();

  // Compute statistics matching DRV output format
  long reached = 0;
  long long sumDist = 0;
  uintE maxDist = 0;
  for (long i = 0; i < n; i++) {
    if (Dist[i] != UINT_E_MAX) {
      reached++;
      sumDist += Dist[i];
      if (Dist[i] > maxDist) maxDist = Dist[i];
    }
  }

  // Print results in a format parseable by compare_results.py
  int pct = reached > 0 ? (int)(100LL * reached / n) : 0;
  cout << "BFS_PushOnly from source " << start << endl;
  cout << "Vertices: " << n << " Edges: " << m << endl;
  cout << "BFS done in " << (level - 1) << " iterations" << endl;
  cout << "Reached: " << reached << "/" << n << " (" << pct << "%)" << endl;
  cout << "max_dist=" << maxDist
       << "  sum_dist=" << sumDist
       << "  avg_dist=" << (reached > 0 ? (double)sumDist / reached : 0.0) << endl;

  // IMC line: bfs_sec rb wb llc_misses l1d_pending cycles stalls_mem stalls_l3
  //           inst_retired uops_slots exe_bound_0_ports
  if (g_imc.available()) {
    cout << "IMC: " << bfs_sec << " " << imc_rb << " " << imc_wb
         << " " << llc_miss << " " << l1d_pending << " " << cpu_cycles
         << " " << stalls_mem << " " << stalls_l3
         << " " << inst_ret << " " << uops_slots << " " << exe0 << endl;
  } else {
    cout << "IMC: " << bfs_sec << " -1 -1 -1 0 0 0 0 0 0 0" << endl;
  }

  free(Dist);
}
