# DRV Usage Guide

This document is a hands-on guide to running the DRV (SST-based PANDOHammer)
simulator on Stampede3, collecting stats with `summarize.py` /
`summarize_ramulator.py`, and reading/adapting the main `*.sbatch`
drivers in this directory.

---

## 1. What the codebase contains

```
drv/
├── element/                SST C++ components (SSTRISCVCore/Hart/Simulator)
├── drvr/                   RISC-V driver programs (*.cpp / *.c → drvr_<name>)
├── model/                  SST python configuration + stat post-processors
│   ├── drvr.py             top-level SST model used by every sbatch
│   ├── cmdline.py          argparse for --pxn-*/--pod-*/--core-* flags
│   ├── pxn.py / pod.py
│   ├── compute.py          per-core wiring, enables statistics
│   ├── memory.py           L1SP/L2SP/DRAM cache + ramulator wiring
│   ├── summarize.py        post-processes stats.csv (SST stats)
│   └── summarize_ramulator.py  post-processes ramulator_*.stats
├── pandohammer/            RISC-V helper headers (mmio.h, atomic.h, …)
├── ramulator-configs/      HBM-pando*.cfg, DDR*, GDDR5, …
├── build_stampede/         CMake build tree + per-run result directories
├── gen_uniform_csr.py      uniform random CSR graph generator
├── gen_rmat_csr.py         RMAT + CUSP graph generator
├── gen_bfs_init_state.py   writes bfs_dist_init.bin next to graph
├── gen_pr_init_state.py    PageRank init
├── gen_sssp_init_state.py  SSSP init
└── *.sbatch                Slurm drivers (build + run + summarize)
```

Everything runs inside the Apptainer container
`/work2/10238/vineeth_architect/stampede3/drv_latest.sif`. The container
holds the SST install at `/install` and the codebase is bind-mounted at
`/work`.

---

## 2. Anatomy of an sbatch script

Every driver in this directory follows the same three-phase layout. The
blocks below are copied from `bfs_csr_weak_roi.sbatch` but apply to all
of them.

### 2a. Slurm header

```bash
#SBATCH -J bfs_csr_weak_roi
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err
#SBATCH -A CCR22006
#SBATCH -p spr           # spr or pvc
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -c 112
#SBATCH -t 48:00:00
```

### 2b. Build phase (STEP 1)

Runs `cmake` + `make -j rv64 Drv pandocommand_loader` inside the
container. The ramulator library is bind-mounted in from the pre-built
tree at `/work2/.../drv_copy/ramulator-build` so it is **not** rebuilt
every job.

### 2c. MemHierarchy library (STEP 1b)

The scripts use a pre-built `libmemHierarchy.so` with coherence-ALU
tagging from:

```
/work2/10238/vineeth_architect/stampede3/drv-stack/sst-elements/lib/sst-elements-library/libmemHierarchy.so
```

If you changed the memory hierarchy code, rebuild it with
`rebuild_memhierarchy.sh` first.

### 2d. Run phase (STEP 2)

A bash array of `CONFIGS=("PODS CX CY THR DRAM_CACHE_KIB" …)` is
iterated. For each config the script:

1. Computes `N = VTX_PER_THREAD * CORES * THR * PODS`.
2. Creates a per-config directory
   `build_stampede/drvr/<sweep>/<run_name>/` and copies
   `summarize.py` + `summarize_ramulator.py` into it.
3. Generates the graph (`gen_uniform_csr.py` or `gen_rmat_csr.py`) and
   any init files (e.g. `gen_bfs_init_state.py`).
4. `cd`s into the run dir and invokes SST:

```bash
PYTHONPATH=/work/py::/work/model \
  /install/bin/sst -n 1 /work/model/drvr.py \
  -- \
  --with-command-processor=<libpandocommand_loader.so> \
  --num-pxn=1 --pxn-pods=${PODS} \
  --pod-cores-x=${CX} --pod-cores-y=${CY} \
  --core-threads=${THR} \
  --pod-l2sp-banks=4 --pod-l2sp-interleave=64 \
  --pxn-dram-banks=1 \
  --pxn-dram-cache-size=${DRAM_CACHE_SIZE} \
  --pxn-dram-cache-slices=4 \
  --dram-backend-config-sliced=/work/ramulator-configs/HBM-pando-16ch.cfg \
  --pxn-dram-cache-alu=0 \
  "$BINARY" --V ${VTX_PER_THREAD} --D ${RUN_DEGREE} \
  | tee "$RUN_DIR/output.txt"
```

5. After the run, both summarizers are executed in the run dir:

```bash
python3 summarize.py
python3 summarize_ramulator.py
```

### 2e. Submitting

```
sbatch bfs_csr_weak_roi.sbatch
squeue -u $USER                   # watch the queue
tail -f logs/<jobname>_<jobid>.out
```

Each sweep writes into `build_stampede/drvr/<sweep_dir>/<run_name>/`.

---

## 3. Key `drvr.py` command-line flags

Full list: `model/cmdline.py`. The ones you will actually touch:

| Flag | Meaning |
|---|---|
| `--num-pxn` | number of PXNs (nodes) |
| `--pxn-pods` | pods per PXN |
| `--pod-cores-x` / `--pod-cores-y` | 2-D pod core grid (cores = CX*CY) |
| `--core-threads` | hardware threads per core (typ. 16) |
| `--core-l1sp-size` | bytes, default 128 KiB |
| `--core-l1-cache-size` / `--core-l1-cache-assoc` | per-core L1 cache (0 = disabled) |
| `--pod-l2sp-banks` | L2SP banks per pod |
| `--pod-l2sp-interleave` | L2SP address interleave (bytes, 0 = none) |
| `--pod-l2sp-size` | L2SP bytes per pod (max 1 MiB) |
| `--pxn-dram-banks` | DRAM banks per PXN |
| `--pxn-dram-size` | bytes, up to 8 GiB |
| `--pxn-dram-cache-size` | bytes |
| `--pxn-dram-cache-slices` | # independent cache+memctrl slices |
| `--pxn-dram-cache-alu` | near-cache ALU latency in cycles, 0 = through Ramulator |
| `--pxn-dram-link-latency` | e.g. `50ns` — sweep target in the latency sweeps |
| `--dram-backend` | `ramulator` / `simple` / `dramsim3` |
| `--dram-backend-config-sliced` | ramulator config per slice |
| `--with-command-processor` | path to `libpandocommand_loader.so` |

After the flags comes the **binary path** then **program argv**
(e.g. `--V 1024 --D 16` for BFS).

---

## 4. Getting stats

Two post-processors live side-by-side in `model/`:

### 4a. `summarize.py` — reads `stats.csv` (SST built-ins + custom)

SST writes `stats.csv` in the run directory when
`statLoadLevel=1` is set (already done by `drvr.py`). `summarize.py`
groups stats by component name:

* **Core statistics** (from `SSTRISCVCore::ELI_getStatistics`):
  * `busy_cycles`, `memory_wait_cycles`, `active_idle_cycles`
  * `load_dram / load_l1sp / load_l2sp` (and store/atomic variants)
  * `load_latency_total`, `load_request_count`, `dram_load_*`
  * `outstanding_requests_sum`, `dram_outstanding_requests_sum`
  * `useful_*` counterparts — gated by the `stat_phase_` MMIO flag
    (set via `ph_stat_phase(1)` around the ROI).
* **DRAM cache** (`*_cache` / `victim_cache` components):
  * `CacheHits`, `CacheMisses`, `latency_Get{S,X}_{hit,miss}`.
* **MemController** (L1SP, L2SP, DRAM slices):
  * `outstanding_requests`, `cycles_with_issue`, `requests_received_{GetS,GetX,Write,PutM}`, `latency_{GetS,GetX,Write,PutM}`.

Usage:

```
cd build_stampede/drvr/<sweep>/<run_name>
python3 summarize.py                          # prints full report
python3 summarize.py --edges 1048576          # override edge count
python3 summarize.py --graph uniform_graph.bin
```

Report sections printed:

1. **MEMORY ACCESS SUMMARY** — L1SP/L2SP/DRAM totals + useful-phase
   breakdown (the `stat_phase` filter discriminates ROI traffic from
   startup/teardown).
2. **DRAM cache** — hit rate, cache-local vs core-perspective latencies,
   interconnect overhead estimate.
3. **Useful-phase memory performance** — phase duration (max across
   cores), avg load/DRAM latencies in the ROI.
4. **Outstanding requests** — per-core and system-wide averages
   (useful phase and total phase, load-only and DRAM-only).
5. **DRAM bandwidth utilization** — cache-miss bytes, PutM eviction
   bytes, atomic DRAM bytes; MSHR- or HBM-spec-limited peak; util%.
6. **Bytes/edge + TEPS** — needs `uniform_graph.bin` (auto-detected)
   or `--edges N`.
7. **Per-core table** — busy / memwait / idle percentages (total and
   useful) + load counts + i-cache misses per core.
8. **Per-DRAM-cache table** — hits, misses, hit/miss latency.
9. **Per-bank MemController tables** — one each for L2SP, L1SP, DRAM
   with queue depth, read/write latency, inter-arrival time, and a
   useful-phase utilization estimate.

### 4b. `summarize_ramulator.py` — reads `ramulator_*.stats`

Ramulator dumps `ramulator_system_pxn0_dram0.stats` (or
`..._s<slice>.stats` when sliced) next to `stats.csv`. The summarizer:

```
python3 summarize_ramulator.py                          # auto-detect file
python3 summarize_ramulator.py my_file.stats
python3 summarize_ramulator.py --clock 500MHz ...       # override DRAM clock
python3 summarize_ramulator.py --graph uniform_graph.bin  # for bytes/edge
```

It transparently sums stats across multiple ramulator instances (one
per DRAM cache slice) and reports:

* **Achieved bandwidth** — read/write/total GB/s, peak-BW utilization %,
  and the fraction of cycles any channel was active.
* **Per-channel bandwidth** — read/write/total MB/s per channel.
* **Row buffer hit rate** — aggregate and per-channel, split by
  read/write.
* **Queue occupancy** — global + per-channel averages (reqs/cycle).
* **Read latency** — weighted avg in DRAM cycles, per channel.
* **KEY METRICS SUMMARY** box at the bottom with the headline numbers.
* **DRAM bytes / edge** — if the graph is present or `--edges` is
  supplied.

### 4c. Typical combined workflow

```bash
cd build_stampede/drvr/<sweep>/<run_name>
python3 summarize.py            | tee summary_sst.txt
python3 summarize_ramulator.py  | tee summary_ramulator.txt
```

The two tools are complementary:
`summarize.py` tells you what the **cores** saw (after cache
filtering, atomics, coherence traffic); `summarize_ramulator.py`
tells you what the **DRAM device** saw (after the cache).
Disagreements between them usually point at cache filtering or
eviction traffic you forgot about.

### 4d. Instrumenting new ROIs

The `useful_*` stats only accumulate while `stat_phase_` is 1 on that
hart. From RISC-V code:

```c
#include <pandohammer/mmio.h>
ph_stat_phase(1);   // MMIO 0xFFFFFFFFFFFF0020 = 1
/* ... ROI ... */
ph_stat_phase(0);
```

Put it around the hot kernel, **before** any global barrier. Without
these calls the whole `useful_*` block will be zero and the useful-
phase metrics in `summarize.py` will read 0 cycles / 0 accesses.

---

## 5. The main `*.sbatch` scripts

All scripts build once then loop over a `CONFIGS` array. To reduce
a sweep to a single point, comment out all lines in `CONFIGS` except
the one you want. Numbers below are the format used by each script's
array.

### 5a. BFS on CSR graphs

| Script | Binary / purpose |
|---|---|
| `bfs_csr_weak_roi.sbatch` ROI-only BFS, `stat_phase` instrumented. Configs: `"PODS CX CY THR DRAM_CACHE_KIB"`
| `bfs_csr_weak_roi_multi.sbatch` | Same ROI-only kernel, sweeps over multi-pod configs. Configs: `"PODS CX CY THR"`. |
| `bfs_csr_weak_multi.sbatch` | `drvr_bfs_csr_weak_multi` — non-ROI multi-pod. |
| `bfs_csr_shared_queue.sbatch` | `drvr_bfs_csr_shared_queue_baseline` — single-queue-per-pod baseline (on the Stampede `pvc` partition). |
| `bfs_csr_shared_queue_baseline.sbatch` / `bfs_csr_shared_queue_multi.sbatch` | Same kernel, different sweep shapes. `_multi` version adds a per-config `SPILL_PCT` for inter-pod stealing. |
| `bfs_csr_sq_compare.sbatch` | Side-by-side comparison of shared-queue variants. |
| `bfs_csr_sq_cache_sweep.sbatch` | Shared-queue kernel × DRAM cache-size sweep. |
| `bfs_csr_sq_latency_sweep.sbatch` | Fixed 8×8 shared-queue run, sweeps `--pxn-dram-link-latency` (e.g. `50ns`, `200ns`, `400ns`). Single graph is generated once and symlinked into each run dir. |
| `bfs_csr_sq_l1cache.sbatch` | CURRENTLY DOESNT WORK - Shared-queue kernel with **per-core L1 cache enabled** (`--core-l1-cache-size=8192`) and **no DRAM cache** — used to study whether L1 caches alone are enough. |
| `bfs_tiered_wq_faa.sbatch` | `drvr_bfs_tiered_wq_faa` — CURRENTLY DOESNT WORK - tiered work-queue with fetch-and-add, needs `rmat_r16.bin`. |

### 5b. Other BFS variants

| Script | Purpose |
|---|---|
| `bfs_pgas.sbatch` | `drvr_bfs_pgas` — multi-PXN BFS over the PGAS address space. Configs: `"NUM_PXN CX CY THR"`. |
| `bfs_pgas_scatter.sbatch` | `drvr_bfs_pgas_scatter` — scatter variant, sweeps VGID block sizes. |

### 5c. PageRank / SSSP

| Script | Binary |
|---|---|
| `pr_csr_weak_roi.sbatch` | `drvr_pr_csr_weak_roi` — PageRank ROI-only. Has a `PR_ITERS` knob. |
| `sssp_csr_weak_roi.sbatch` | `drvr_sssp_csr_weak_roi` — SSSP ROI-only. Same `"PODS CX CY THR"` sweep. |

### 5d. Microbenchmarks & infrastructure sweeps

| Script | Purpose |
|---|---|
| `atomic_bw_blocking.sbatch` | `drvr_atomic_bw_blocking` — atomic BW, blocking. Configs: `"CX CY THR"`. |
| `atomic_bw_posted.sbatch` | Posted atomics counterpart. |
| `stream_bw.sbatch` | Streaming DRAM BW baseline. |
| `shared_read.sbatch` | Shared-read pattern microbenchmark. |
| `pgas_test.sbatch` | Quick PGAS smoke test. |
| `test_banked_cache.sbatch` | DRAM cache banking test. |
| `test_cache_slices.sbatch` | Sweep over `--pxn-dram-cache-slices`. |
| `test_cache_alu.sbatch` | Compares `--pxn-dram-cache-alu=0` vs `=2` (coherence-only). |
| `test_barrier_tuning.sbatch` | Barrier parameter tuning. |
| `test_hol_fix.sbatch` | Head-of-line blocking fix regression test. |
| `verify.sbatch`, `v.sbatch` | Small verification / smoke runs. |
| `sweep_bfs.sbatch` | Older BFS sweep wrapper. |
| `drv_bfs_build_run.sbatch` | Simplest build + one BFS run — useful as a template. |

---

## 6. Picking a point in the sweep (`CONFIGS`)

For the weak-scaling BFS drivers the canonical ladder is

```
# PODS CX CY THR DRAM_CACHE_KIB
"1 1  1 16 8"
"1 2  1 16 16"
"1 4  1 16 32"
"1 8  1 16 64"
"1 8  2 16 128"
"1 8  4 16 256"
"1 8  8 16 512"   # 64-core single-pod
"2 8  8 16 4096"
"4 8  8 16 4096"
```

Rule of thumb: `N = 1024 * CX * CY * THR * PODS` vertices, DRAM cache
doubles with cores so per-thread working set stays roughly constant.

---

## 7. Troubleshooting checklist

* **`Error: 'stats.csv' not found.`** — the SST run crashed before stats
  were written. Look at `output.txt` in the run dir, then `logs/…err`.
* **All `useful_*` stats are zero** — the kernel isn't calling
  `ph_stat_phase(1)/0`. See §4d.
* **`summarize_ramulator.py` says `No channel data found`** — you ran
  with `--dram-backend=simple`; ramulator stats are only produced when
  `--dram-backend=ramulator` (the default).
* **`Ramulator instances (slices): N`** in the ramulator summary — one
  `.stats` file exists per slice, the summarizer sums them
  automatically (no action needed).
* **`libmemHierarchy.so not found`** — run
  `./rebuild_memhierarchy.sh` once; it populates the path expected by
  every sbatch script.
* **Graph not auto-detected by bytes/edge** — name the binary
  `uniform_graph.bin` in the run dir (that's the default), or pass
  `--graph` / `--edges` to the summarizers.

---

## 8. Quickstart — one-pod BFS from scratch

```bash
cd /work2/10238/vineeth_architect/stampede3/drv_copy/drv

# 1. Edit CONFIGS in bfs_csr_weak_roi.sbatch down to a single entry:
#       "1 1 1 16 8"
# 2. Submit:
sbatch bfs_csr_weak_roi.sbatch

# 3. When it finishes, stats are in:
cd build_stampede/drvr/bfs_csr_weak_roi_results_uniform_new/bfs_csr_roi_p1_c1_t16_dc0M
python3 summarize.py
python3 summarize_ramulator.py
```

The job's own log is at `logs/bfs_csr_weak_roi_<jobid>.out` and already
contains the summarizer output (the script tees into both the run
directory and stdout).
