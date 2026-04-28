# Ligra BFS Comparison Harness

CPU-side baseline for the DRV/PANDO BFS work. Runs Ligra's push-only BFS on
the same uniform random graphs the DRV `bfs_csr_weak` driver consumes, so
results are directly comparable.

## Layout

- `BFS_PushOnly.C` — push-only BFS (mirrors the DRV algorithm; uses the
  `no_dense` flag). Drop into `ligra/apps/`.
- `gen_uniform_graph.cpp` — generates DRV-compatible uniform random graphs
  with the xorshift32 PRNG (`seed = v * degree + e + 1`). Drop into
  `ligra/utils/`.
- `gen_rmat_graph.py` — RMAT graph generator for the dir-optimizing variant.
- `ligra-upstream.patch` — small edits to upstream Ligra:
  - `apps/Makefile`: add `BFS_PushOnly` to the `ALL` target.
  - `ligra/utils.h`: add `#include <cstdint>` (needed for gcc 13).
- `ligra_bfs_weak_scaling.sbatch` — weak-scaling sweep matching the DRV
  c1..c64 sweep. Uses `perf uncore_imc` for DRAM bandwidth.
- `ligra_bfs_benchmark.sbatch` — strong-scaling sweep.
- `ligra_bfs_diropt_weak_scaling.sbatch` — weak scaling for direction-
  optimizing BFS on RMAT inputs.
- `compare_results.py` — parses both DRV (`bfs_csr_weak_results_2/`) and
  Ligra outputs and produces side-by-side scaling/BW/utilization plots.
  Supports `--drv-peak-bw` and `--cpu-peak-bw` overrides.

## Reproducing the Ligra side

Base commit: **jshun/ligra @ 8763202** ("fixing seg fault issue on small
graphs").

```sh
git clone https://github.com/jshun/ligra.git
cd ligra
git checkout 8763202

# apply upstream edits + drop in the new sources
git apply /path/to/drv/comparison/ligra/ligra-upstream.patch
cp /path/to/drv/comparison/ligra/BFS_PushOnly.C       apps/
cp /path/to/drv/comparison/ligra/gen_uniform_graph.cpp utils/

# Stampede3: MKLROOT from intel/24.0 makes the Makefile pick icpc
unset MKLROOT
cd apps && OPENMP=1 make BFS_PushOnly
cd ../utils && OPENMP=1 make gen_uniform_graph
```

## Algorithm equivalence

Verified identical `reached / max_dist / sum_dist` against DRV at c1..c32
on the uniform random inputs. See memory note `ligra_bfs_setup`.
