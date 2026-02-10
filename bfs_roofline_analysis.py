#!/usr/bin/env python3
"""
BFS Workload-Specific Roofline Analysis for PANDOHammer

This script helps you:
1. Calculate THEORETICAL (ideal) roofline bounds for BFS
2. Extract MEASURED performance from simulation stats
3. Compare ideal vs actual to identify bottlenecks
4. Generate experiment configurations for systematic analysis

Usage:
    # Analyze a completed simulation
    python bfs_roofline_analysis.py --stats-file build_stampede/drvr/drvr-run-bfs_multi_sw_l2sp/stats.csv

    # Calculate theoretical bounds for a given grid size
    python bfs_roofline_analysis.py --theoretical --grid-r 64 --grid-c 64

    # Generate experiment configurations
    python bfs_roofline_analysis.py --generate-experiments
"""

import os
import csv
import argparse
import math
from collections import defaultdict
from dataclasses import dataclass
from typing import Dict, List, Tuple, Optional

# Handle headless environments
import matplotlib
if os.environ.get('DISPLAY') is None:
    matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np


# ============================================================================
# Architecture Parameters (from pandohammer.py)
# ============================================================================
@dataclass
class ArchConfig:
    """PANDOHammer architecture configuration."""
    clock_ghz: float = 1.0
    threads_per_core: int = 16
    cores_per_pod: int = 4
    pods_per_pxn: int = 1
    bw_per_core_gbs: float = 24.0  # GB/s

    # Memory latencies (cycles at 1GHz = ns)
    l1sp_latency_cycles: int = 1
    l2sp_latency_cycles: int = 4
    dram_latency_cycles: int = 70

    # Memory sizes
    l1sp_size_bytes: int = 128 * 1024  # 128KB per core
    l2sp_size_bytes: int = 1024 * 1024  # 1MB per pod

    def total_threads(self) -> int:
        return self.threads_per_core * self.cores_per_pod * self.pods_per_pxn

    def l1sp_bw_gbs(self) -> float:
        return self.bw_per_core_gbs

    def l2sp_bw_gbs(self) -> float:
        return self.bw_per_core_gbs * self.cores_per_pod

    def dram_bw_gbs(self) -> float:
        return self.bw_per_core_gbs * self.cores_per_pod * self.pods_per_pxn


# ============================================================================
# BFS Theoretical Analysis
# ============================================================================
@dataclass
class BFSWorkload:
    """BFS workload characteristics for a 2D grid."""
    rows: int
    cols: int

    @property
    def num_vertices(self) -> int:
        return self.rows * self.cols

    @property
    def num_edges(self) -> int:
        """4-connected grid: each vertex has up to 4 neighbors."""
        # Interior: 4 edges, edges: 3 edges, corners: 2 edges
        # Total directed edges = 2 * (R-1) * C + 2 * R * (C-1)
        return 2 * (self.rows - 1) * self.cols + 2 * self.rows * (self.cols - 1)

    @property
    def num_iterations(self) -> int:
        """BFS from (0,0) on grid takes R+C-2 iterations."""
        return self.rows + self.cols - 2

    def memory_per_vertex_bytes(self) -> Dict[str, int]:
        """Memory footprint per vertex."""
        return {
            'dist': 4,           # int32_t g_dist[N]
            'frontier': 1,       # uint8_t g_frontier[N]
            'next_frontier': 1,  # uint8_t g_next_frontier[N]
        }

    def total_memory_bytes(self) -> int:
        """Total memory footprint."""
        per_vertex = sum(self.memory_per_vertex_bytes().values())
        return self.num_vertices * per_vertex

    def theoretical_memory_traffic(self) -> Dict[str, int]:
        """
        Calculate theoretical memory traffic for BFS.

        In level-synchronous BFS on a grid:
        - Each vertex is visited exactly once
        - When visited, we check all 4 neighbors
        - We read: frontier[v], dist[v], and dist[neighbor] for each neighbor
        - We write: dist[neighbor] and next_frontier[neighbor] if not visited

        Returns bytes for best-case (all from L2SP) scenario.
        """
        N = self.num_vertices

        # Per-vertex reads (when in frontier):
        # - frontier[v]: 1 byte
        # - dist[v]: 4 bytes
        # - dist[neighbor] for 4 neighbors: 4 * 4 = 16 bytes (with some boundary cases)
        reads_per_vertex = 1 + 4 + 4 * 4  # 21 bytes

        # Per-vertex writes (when discovered):
        # - dist[v]: 4 bytes (atomic CAS)
        # - next_frontier[v]: 1 byte
        writes_per_vertex = 4 + 1  # 5 bytes

        # Barrier/swap overhead per iteration
        # - Read all frontier: N bytes
        # - Write next_frontier: N bytes
        # - Clear next_frontier: N bytes
        barrier_overhead = 3 * N * self.num_iterations

        total_reads = N * reads_per_vertex
        total_writes = N * writes_per_vertex

        return {
            'total_bytes': total_reads + total_writes + barrier_overhead,
            'read_bytes': total_reads + barrier_overhead // 2,
            'write_bytes': total_writes + barrier_overhead // 2,
            'per_vertex_bytes': reads_per_vertex + writes_per_vertex,
        }

    def theoretical_operations(self) -> Dict[str, int]:
        """
        Calculate theoretical operations for BFS.

        BFS is integer-heavy, not FP-heavy:
        - Integer comparisons (bounds checking)
        - Integer arithmetic (index calculation)
        - Atomic CAS operations
        """
        N = self.num_vertices

        # Per vertex when in frontier:
        # - Index calculations: ~10 int ops
        # - Bounds checks: ~8 comparisons
        # - Atomic CAS attempts: ~4
        ops_per_vertex = 10 + 8 + 4

        # Total integer operations
        total_int_ops = N * ops_per_vertex

        # Barrier overhead: iteration count checks, etc.
        barrier_ops = self.num_iterations * N

        return {
            'total_int_ops': total_int_ops + barrier_ops,
            'total_fp_ops': 0,  # BFS has no FP operations
            'int_ops_per_vertex': ops_per_vertex,
        }

    def arithmetic_intensity(self) -> float:
        """
        Calculate arithmetic intensity (ops/byte).

        For BFS, this is INTEGER ops / bytes, not FLOP/byte.
        This gives a "pseudo-AI" for comparison purposes.
        """
        ops = self.theoretical_operations()
        mem = self.theoretical_memory_traffic()
        return ops['total_int_ops'] / mem['total_bytes']

    def print_analysis(self):
        """Print detailed workload analysis."""
        print("=" * 70)
        print(f"BFS WORKLOAD ANALYSIS: {self.rows}x{self.cols} Grid")
        print("=" * 70)

        print(f"\nGraph Properties:")
        print(f"  Vertices (N):     {self.num_vertices:,}")
        print(f"  Edges:            {self.num_edges:,}")
        print(f"  BFS Iterations:   {self.num_iterations}")

        mem = self.memory_per_vertex_bytes()
        print(f"\nMemory Layout (per vertex):")
        for name, size in mem.items():
            print(f"  {name}: {size} bytes")
        print(f"  Total: {sum(mem.values())} bytes/vertex")
        print(f"  Total footprint: {self.total_memory_bytes() / 1024:.1f} KB")

        traffic = self.theoretical_memory_traffic()
        print(f"\nTheoretical Memory Traffic:")
        print(f"  Read bytes:       {traffic['read_bytes']:,} ({traffic['read_bytes']/1024:.1f} KB)")
        print(f"  Write bytes:      {traffic['write_bytes']:,} ({traffic['write_bytes']/1024:.1f} KB)")
        print(f"  Total bytes:      {traffic['total_bytes']:,} ({traffic['total_bytes']/1024:.1f} KB)")
        print(f"  Bytes per vertex: {traffic['per_vertex_bytes']}")

        ops = self.theoretical_operations()
        print(f"\nTheoretical Operations:")
        print(f"  Integer ops:      {ops['total_int_ops']:,}")
        print(f"  FP ops:           {ops['total_fp_ops']}")
        print(f"  Int ops/vertex:   {ops['int_ops_per_vertex']}")

        ai = self.arithmetic_intensity()
        print(f"\nArithmetic Intensity (INT ops/byte): {ai:.4f}")
        print(f"  (Note: BFS has ~0 FLOP/byte, so it's always memory-bound)")

        print("=" * 70)


# ============================================================================
# Simulation Statistics Parser
# ============================================================================
@dataclass
class SimulationStats:
    """Parsed simulation statistics."""
    sim_time_ns: float
    total_cycles: int

    # Memory access counts
    load_l1sp: int
    store_l1sp: int
    atomic_l1sp: int
    load_l2sp: int
    store_l2sp: int
    atomic_l2sp: int
    load_dram: int
    store_dram: int
    atomic_dram: int

    # Cycle breakdown
    busy_cycles: int
    memory_wait_cycles: int
    active_idle_cycles: int

    # Per-core stats (optional)
    per_core_stats: Optional[Dict] = None


def parse_stats_csv(filepath: str) -> SimulationStats:
    """Parse SST simulation statistics CSV file."""
    stats = defaultdict(int)
    sim_time = 0
    per_core = defaultdict(lambda: defaultdict(int))

    with open(filepath, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            component = row['ComponentName']
            stat_name = row['StatisticName']
            value = int(row['Sum.u64'])
            sim_time = int(row['SimTime'])

            # Aggregate stats across all cores/harts
            if 'core' in component and '_cache' not in component:
                stats[stat_name] += value

                # Also track per-core
                core_name = component.split('_core')[0]
                per_core[core_name][stat_name] += value

    # Convert sim_time from picoseconds to nanoseconds
    sim_time_ns = sim_time / 1000.0

    return SimulationStats(
        sim_time_ns=sim_time_ns,
        total_cycles=int(sim_time_ns),  # 1GHz clock means 1 cycle = 1 ns
        load_l1sp=stats.get('load_l1sp', 0),
        store_l1sp=stats.get('store_l1sp', 0),
        atomic_l1sp=stats.get('atomic_l1sp', 0),
        load_l2sp=stats.get('load_l2sp', 0),
        store_l2sp=stats.get('store_l2sp', 0),
        atomic_l2sp=stats.get('atomic_l2sp', 0),
        load_dram=stats.get('load_dram', 0),
        store_dram=stats.get('store_dram', 0),
        atomic_dram=stats.get('atomic_dram', 0),
        busy_cycles=stats.get('busy_cycles', 0),
        memory_wait_cycles=stats.get('memory_wait_cycles', 0),
        active_idle_cycles=stats.get('active_idle_cycles', 0),
        per_core_stats=dict(per_core) if per_core else None,
    )


def analyze_measured_performance(stats: SimulationStats, arch: ArchConfig, workload: BFSWorkload):
    """Analyze measured performance vs theoretical bounds."""
    print("=" * 70)
    print("MEASURED PERFORMANCE ANALYSIS")
    print("=" * 70)

    # Memory access summary
    total_l1sp = stats.load_l1sp + stats.store_l1sp + stats.atomic_l1sp
    total_l2sp = stats.load_l2sp + stats.store_l2sp + stats.atomic_l2sp
    total_dram = stats.load_dram + stats.store_dram + stats.atomic_dram
    total_accesses = total_l1sp + total_l2sp + total_dram

    print(f"\nSimulation Time: {stats.sim_time_ns / 1e6:.3f} ms ({stats.total_cycles:,} cycles)")

    print(f"\nMemory Access Distribution:")
    if total_accesses > 0:
        print(f"  L1SP:  {total_l1sp:>12,} ({100*total_l1sp/total_accesses:>5.1f}%)")
        print(f"  L2SP:  {total_l2sp:>12,} ({100*total_l2sp/total_accesses:>5.1f}%)")
        print(f"  DRAM:  {total_dram:>12,} ({100*total_dram/total_accesses:>5.1f}%)")
        print(f"  Total: {total_accesses:>12,}")

    # Estimate bytes transferred (assuming 8 bytes per access average)
    bytes_per_access = 8
    measured_bytes = total_accesses * bytes_per_access

    # Calculate achieved bandwidth
    time_seconds = stats.sim_time_ns / 1e9
    achieved_bw_gbs = (measured_bytes / 1e9) / time_seconds if time_seconds > 0 else 0

    print(f"\nBandwidth Analysis:")
    print(f"  Estimated bytes transferred: {measured_bytes:,} ({measured_bytes/1024/1024:.2f} MB)")
    print(f"  Achieved bandwidth: {achieved_bw_gbs:.2f} GB/s")
    print(f"  Peak L2SP bandwidth: {arch.l2sp_bw_gbs():.2f} GB/s")
    print(f"  Bandwidth efficiency: {100*achieved_bw_gbs/arch.l2sp_bw_gbs():.1f}%")

    # Cycle breakdown
    total_active = stats.busy_cycles + stats.memory_wait_cycles + stats.active_idle_cycles
    if total_active > 0:
        print(f"\nCycle Breakdown:")
        print(f"  Busy cycles:       {stats.busy_cycles:>12,} ({100*stats.busy_cycles/total_active:>5.1f}%)")
        print(f"  Memory wait:       {stats.memory_wait_cycles:>12,} ({100*stats.memory_wait_cycles/total_active:>5.1f}%)")
        print(f"  Idle cycles:       {stats.active_idle_cycles:>12,} ({100*stats.active_idle_cycles/total_active:>5.1f}%)")

    # Compare with theoretical
    theoretical = workload.theoretical_memory_traffic()
    print(f"\nTheoretical vs Measured:")
    print(f"  Theoretical bytes: {theoretical['total_bytes']:,}")
    print(f"  Measured bytes:    {measured_bytes:,}")
    print(f"  Ratio:             {measured_bytes/theoretical['total_bytes']:.2f}x")

    # Performance metrics
    vertices_per_second = workload.num_vertices / time_seconds if time_seconds > 0 else 0
    edges_per_second = workload.num_edges / time_seconds if time_seconds > 0 else 0

    print(f"\nThroughput Metrics:")
    print(f"  Vertices/second:   {vertices_per_second:,.0f}")
    print(f"  Edges/second:      {edges_per_second:,.0f}")
    print(f"  MTEPS:             {edges_per_second/1e6:.2f}")

    print("=" * 70)

    return {
        'achieved_bw_gbs': achieved_bw_gbs,
        'bw_efficiency': achieved_bw_gbs / arch.l2sp_bw_gbs(),
        'busy_fraction': stats.busy_cycles / total_active if total_active > 0 else 0,
        'memory_wait_fraction': stats.memory_wait_cycles / total_active if total_active > 0 else 0,
        'mteps': edges_per_second / 1e6,
    }


# ============================================================================
# Roofline Visualization
# ============================================================================
def plot_bfs_roofline(arch: ArchConfig, workload: BFSWorkload,
                      measured_perf: Optional[Dict] = None,
                      output_file: str = None):
    """
    Plot BFS-specific roofline model.

    Since BFS has ~0 FLOPs, we use "Integer Ops/Byte" as the x-axis
    and "MTEPS" or "GOPs/s" as y-axis alternatives.
    """
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))

    # Left plot: Traditional roofline (with BFS point marked)
    ax1 = axes[0]
    ai = np.logspace(-3, 2, 500)

    # Memory-bound lines (using integer ops as proxy for "performance")
    peak_gops = arch.clock_ghz * arch.total_threads()  # 1 int op/cycle/thread

    for bw, label, color in [
        (arch.l1sp_bw_gbs(), 'L1SP BW', 'blue'),
        (arch.l2sp_bw_gbs(), 'L2SP BW', 'green'),
        (arch.dram_bw_gbs(), 'DRAM BW', 'red'),
    ]:
        perf = np.minimum(peak_gops, ai * bw)
        ax1.loglog(ai, perf, color=color, linewidth=2, label=f'{label}: {bw:.0f} GB/s')

    ax1.axhline(y=peak_gops, color='black', linestyle=':', label=f'Peak: {peak_gops:.0f} GOPs/s')

    # Mark BFS arithmetic intensity
    bfs_ai = workload.arithmetic_intensity()
    ax1.axvline(x=bfs_ai, color='purple', linestyle='--', alpha=0.7,
                label=f'BFS AI: {bfs_ai:.3f}')

    # Mark measured point if available
    if measured_perf:
        measured_ai = bfs_ai  # Same AI, just different achieved perf
        measured_gops = measured_perf['achieved_bw_gbs'] * bfs_ai
        ax1.plot(measured_ai, measured_gops, 'r*', markersize=15,
                label=f'Measured: {measured_gops:.2f} GOPs/s')

    ax1.set_xlabel('Arithmetic Intensity (Int Ops/Byte)', fontsize=11)
    ax1.set_ylabel('Performance (GOPs/s)', fontsize=11)
    ax1.set_title('BFS Roofline Model (Integer Operations)', fontsize=12)
    ax1.legend(loc='lower right', fontsize=9)
    ax1.grid(True, which='both', alpha=0.3)
    ax1.set_xlim([1e-3, 100])
    ax1.set_ylim([0.1, peak_gops * 2])

    # Right plot: Memory-centric view (bandwidth utilization)
    ax2 = axes[1]

    # This shows: given memory access pattern, what's the limiting bandwidth
    categories = ['L1SP\n(Best)', 'L2SP\n(Target)', 'DRAM\n(Worst)']
    peak_bws = [arch.l1sp_bw_gbs(), arch.l2sp_bw_gbs(), arch.dram_bw_gbs()]
    colors = ['blue', 'green', 'red']

    x = np.arange(len(categories))
    bars = ax2.bar(x, peak_bws, color=colors, alpha=0.7, label='Peak BW')

    if measured_perf:
        achieved = [measured_perf['achieved_bw_gbs']] * 3
        ax2.bar(x, achieved, color='purple', alpha=0.5, label='Achieved BW')

    ax2.set_ylabel('Bandwidth (GB/s)', fontsize=11)
    ax2.set_title('Memory Hierarchy Bandwidth', fontsize=12)
    ax2.set_xticks(x)
    ax2.set_xticklabels(categories)
    ax2.legend()

    # Add annotations
    for i, (cat, bw) in enumerate(zip(categories, peak_bws)):
        ax2.annotate(f'{bw:.0f} GB/s', xy=(i, bw), ha='center', va='bottom')

    plt.tight_layout()

    if output_file:
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"Saved BFS roofline to: {output_file}")
    else:
        plt.show()

    return fig


# ============================================================================
# Experiment Configuration Generator
# ============================================================================
def generate_experiment_configs():
    """Generate recommended experiment configurations for BFS roofline analysis."""

    print("=" * 70)
    print("RECOMMENDED EXPERIMENT CONFIGURATIONS")
    print("=" * 70)

    print("""
To build a complete BFS roofline, run experiments varying:
1. Grid sizes (to see scaling behavior)
2. Thread counts (to see parallelism effects)
3. Memory placement (L2SP vs DRAM)

GRID SIZE EXPERIMENTS:
======================
These test how performance scales with problem size.

""")

    # Grid sizes that fit in L2SP (1MB = 1,048,576 bytes)
    # BFS needs ~6 bytes/vertex (dist:4 + frontier:1 + next_frontier:1)
    l2sp_size = 1024 * 1024
    bytes_per_vertex = 6
    max_vertices_l2sp = l2sp_size // bytes_per_vertex  # ~174,000 vertices

    grid_configs = [
        (32, 32, "Tiny - baseline"),
        (64, 64, "Small - fits L2SP easily"),
        (128, 128, "Medium - fits L2SP"),
        (256, 256, "Large - L2SP boundary (~65K vertices)"),
        (512, 512, "XL - exceeds L2SP, needs DRAM"),
    ]

    for r, c, desc in grid_configs:
        n = r * c
        mem = n * bytes_per_vertex
        fits_l2sp = "YES" if mem <= l2sp_size else "NO"
        print(f"  --R {r:>4} --C {c:>4}  # {desc}")
        print(f"      N={n:>6}, Mem={mem/1024:.0f}KB, Fits L2SP: {fits_l2sp}")
        print()

    print("""
THREAD COUNT EXPERIMENTS:
=========================
Test parallelism scaling (keep grid size constant).

""")

    thread_configs = [1, 4, 16, 32, 64]
    for t in thread_configs:
        print(f"  --T {t:>3}  # {t} threads")

    print("""
COMPLETE EXPERIMENT SCRIPT:
===========================
Run this to generate data for roofline plots:

# Create results directory
mkdir -p bfs_roofline_results

# Small grid (L2SP)
for T in 1 4 16 32 64; do
    make drvr-run-bfs_multi_sw_l2sp DRV_RUN_OPTIONS="--R 64 --C 64 --T $T"
    cp build_stampede/drvr/drvr-run-bfs_multi_sw_l2sp/stats.csv \\
       bfs_roofline_results/bfs_64x64_T${T}.csv
done

# Medium grid (L2SP boundary)
for T in 16 32 64; do
    make drvr-run-bfs_multi_sw_l2sp DRV_RUN_OPTIONS="--R 128 --C 128 --T $T"
    cp build_stampede/drvr/drvr-run-bfs_multi_sw_l2sp/stats.csv \\
       bfs_roofline_results/bfs_128x128_T${T}.csv
done

# Large grid (DRAM)
for T in 16 32 64; do
    make drvr-run-bfs_multi_sw_l2sp DRV_RUN_OPTIONS="--R 256 --C 256 --T $T"
    cp build_stampede/drvr/drvr-run-bfs_multi_sw_l2sp/stats.csv \\
       bfs_roofline_results/bfs_256x256_T${T}.csv
done

# Then analyze:
python bfs_roofline_analysis.py --batch-analyze bfs_roofline_results/
""")

    print("=" * 70)


def batch_analyze(results_dir: str, arch: ArchConfig):
    """Analyze multiple experiment results and generate comparison plots."""
    import glob

    results = []

    for csv_file in sorted(glob.glob(os.path.join(results_dir, "*.csv"))):
        # Parse filename to get config: bfs_RxC_TN.csv
        basename = os.path.basename(csv_file)
        try:
            # Try to extract R, C, T from filename
            parts = basename.replace('.csv', '').split('_')
            grid_part = [p for p in parts if 'x' in p][0]
            r, c = map(int, grid_part.split('x'))
            t = int([p for p in parts if p.startswith('T')][0][1:])
        except:
            print(f"Skipping {basename} - couldn't parse config from filename")
            continue

        print(f"\nAnalyzing: {basename} (R={r}, C={c}, T={t})")

        stats = parse_stats_csv(csv_file)
        workload = BFSWorkload(rows=r, cols=c)

        # Calculate metrics
        total_accesses = (stats.load_l1sp + stats.store_l1sp + stats.atomic_l1sp +
                        stats.load_l2sp + stats.store_l2sp + stats.atomic_l2sp +
                        stats.load_dram + stats.store_dram + stats.atomic_dram)

        time_s = stats.sim_time_ns / 1e9
        measured_bytes = total_accesses * 8
        achieved_bw = (measured_bytes / 1e9) / time_s if time_s > 0 else 0
        mteps = (workload.num_edges / time_s / 1e6) if time_s > 0 else 0

        results.append({
            'R': r, 'C': c, 'T': t, 'N': r*c,
            'time_ms': stats.sim_time_ns / 1e6,
            'achieved_bw': achieved_bw,
            'mteps': mteps,
            'l2sp_frac': (stats.load_l2sp + stats.store_l2sp) / total_accesses if total_accesses > 0 else 0,
            'dram_frac': (stats.load_dram + stats.store_dram) / total_accesses if total_accesses > 0 else 0,
        })

    if not results:
        print("No results found!")
        return

    # Print summary table
    print("\n" + "=" * 90)
    print("BATCH ANALYSIS SUMMARY")
    print("=" * 90)
    print(f"{'Config':<15} {'Time (ms)':<12} {'BW (GB/s)':<12} {'MTEPS':<12} {'L2SP %':<10} {'DRAM %':<10}")
    print("-" * 90)

    for r in results:
        config = f"{r['R']}x{r['C']}_T{r['T']}"
        print(f"{config:<15} {r['time_ms']:<12.2f} {r['achieved_bw']:<12.2f} {r['mteps']:<12.2f} "
              f"{100*r['l2sp_frac']:<10.1f} {100*r['dram_frac']:<10.1f}")

    # Generate comparison plot
    fig, axes = plt.subplots(1, 3, figsize=(15, 5))

    # Group by grid size
    grid_sizes = sorted(set(r['N'] for r in results))

    # Plot 1: MTEPS vs Threads
    ax = axes[0]
    for n in grid_sizes:
        subset = [r for r in results if r['N'] == n]
        threads = [r['T'] for r in subset]
        mteps = [r['mteps'] for r in subset]
        ax.plot(threads, mteps, 'o-', label=f'{int(math.sqrt(n))}x{int(math.sqrt(n))}')
    ax.set_xlabel('Threads')
    ax.set_ylabel('MTEPS')
    ax.set_title('BFS Throughput vs Thread Count')
    ax.legend()
    ax.grid(True, alpha=0.3)

    # Plot 2: Bandwidth vs Problem Size
    ax = axes[1]
    # Get results with max threads for each grid size
    for n in grid_sizes:
        subset = [r for r in results if r['N'] == n]
        best = max(subset, key=lambda x: x['T'])
        ax.bar(str(n), best['achieved_bw'], alpha=0.7)
    ax.axhline(y=arch.l2sp_bw_gbs(), color='r', linestyle='--', label=f'Peak L2SP: {arch.l2sp_bw_gbs()} GB/s')
    ax.set_xlabel('Problem Size (vertices)')
    ax.set_ylabel('Achieved BW (GB/s)')
    ax.set_title('Bandwidth vs Problem Size')
    ax.legend()

    # Plot 3: Memory distribution
    ax = axes[2]
    for i, n in enumerate(grid_sizes):
        subset = [r for r in results if r['N'] == n]
        best = max(subset, key=lambda x: x['T'])
        ax.bar(i, best['l2sp_frac'], label='L2SP' if i == 0 else '', color='green', alpha=0.7)
        ax.bar(i, best['dram_frac'], bottom=best['l2sp_frac'], label='DRAM' if i == 0 else '', color='red', alpha=0.7)
    ax.set_xticks(range(len(grid_sizes)))
    ax.set_xticklabels([str(n) for n in grid_sizes])
    ax.set_xlabel('Problem Size (vertices)')
    ax.set_ylabel('Memory Access Fraction')
    ax.set_title('Memory Hierarchy Utilization')
    ax.legend()

    plt.tight_layout()
    output_file = os.path.join(results_dir, 'bfs_roofline_comparison.png')
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"\nSaved comparison plot to: {output_file}")


# ============================================================================
# Main
# ============================================================================
def main():
    parser = argparse.ArgumentParser(
        description='BFS Workload-Specific Roofline Analysis',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    # Analysis modes
    parser.add_argument('--theoretical', action='store_true',
                       help='Calculate theoretical BFS bounds')
    parser.add_argument('--stats-file', type=str,
                       help='Path to simulation stats.csv file')
    parser.add_argument('--generate-experiments', action='store_true',
                       help='Generate recommended experiment configurations')
    parser.add_argument('--batch-analyze', type=str,
                       help='Directory containing multiple stats.csv files')

    # Workload parameters
    parser.add_argument('--grid-r', type=int, default=64,
                       help='Grid rows (default: 64)')
    parser.add_argument('--grid-c', type=int, default=64,
                       help='Grid columns (default: 64)')

    # Architecture parameters
    parser.add_argument('--cores-per-pod', type=int, default=4,
                       help='Cores per pod (default: 4)')
    parser.add_argument('--threads-per-core', type=int, default=16,
                       help='Threads per core (default: 16)')

    # Output
    parser.add_argument('--output', type=str,
                       help='Output file for roofline plot')

    args = parser.parse_args()

    # Create arch config
    arch = ArchConfig(
        cores_per_pod=args.cores_per_pod,
        threads_per_core=args.threads_per_core,
    )

    # Create workload
    workload = BFSWorkload(rows=args.grid_r, cols=args.grid_c)

    if args.generate_experiments:
        generate_experiment_configs()
        return

    if args.batch_analyze:
        batch_analyze(args.batch_analyze, arch)
        return

    if args.theoretical:
        workload.print_analysis()
        plot_bfs_roofline(arch, workload, output_file=args.output or 'bfs_theoretical_roofline.png')
        return

    if args.stats_file:
        if not os.path.exists(args.stats_file):
            print(f"Error: Stats file not found: {args.stats_file}")
            return

        # Print theoretical analysis
        workload.print_analysis()

        # Parse and analyze measured stats
        stats = parse_stats_csv(args.stats_file)
        measured = analyze_measured_performance(stats, arch, workload)

        # Generate roofline plot
        plot_bfs_roofline(arch, workload, measured,
                         output_file=args.output or 'bfs_measured_roofline.png')
        return

    # Default: show help
    parser.print_help()


if __name__ == '__main__':
    main()
