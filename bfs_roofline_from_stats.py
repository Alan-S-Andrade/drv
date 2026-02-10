#!/usr/bin/env python3
"""
BFS Roofline Analysis from PANDOHammer Text Output

Analyzes BFS performance from the PANDOHammer memory access summary output.
"""

import os
import argparse

import matplotlib
if os.environ.get('DISPLAY') is None:
    matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np


def analyze_bfs_roofline(
    # Grid parameters
    grid_r: int = 64,
    grid_c: int = 64,

    # Architecture parameters (single core with 16 threads)
    clock_ghz: float = 1.0,
    threads_per_core: int = 16,
    cores_used: int = 1,
    bw_per_core_gbs: float = 24.0,

    # Measured stats from PANDOHammer output
    l1sp_loads: int = 0,
    l1sp_stores: int = 0,
    l2sp_loads: int = 0,
    l2sp_stores: int = 0,
    dram_loads: int = 0,
    dram_stores: int = 0,
    dram_cache_hits: int = 0,
    dram_cache_misses: int = 0,
    dram_cache_hit_latency: float = 14.0,   # Core-perspective (includes ~12 cycle interconnect)
    dram_cache_miss_latency: float = 123.0, # Core-perspective (includes ~12 cycle interconnect)

    # Simulation time in milliseconds
    sim_time_ms: float = 0,

    output_prefix: str = "bfs_roofline"
):
    """
    Perform roofline analysis for BFS workload.
    """

    num_vertices = grid_r * grid_c
    num_edges = 2 * (grid_r - 1) * grid_c + 2 * grid_r * (grid_c - 1)
    num_iterations = grid_r + grid_c - 2  # BFS levels

    print("=" * 80)
    print("BFS ROOFLINE ANALYSIS")
    print("=" * 80)

    # -------------------------------------------------------------------------
    # 1. Graph Properties
    # -------------------------------------------------------------------------
    print(f"\n[1] GRAPH PROPERTIES")
    print(f"    Grid Size:        {grid_r} x {grid_c}")
    print(f"    Vertices (N):     {num_vertices:,}")
    print(f"    Edges (E):        {num_edges:,}")
    print(f"    BFS Iterations:   {num_iterations}")

    # -------------------------------------------------------------------------
    # 2. Architecture Configuration
    # -------------------------------------------------------------------------
    print(f"\n[2] ARCHITECTURE CONFIGURATION")
    print(f"    Clock:            {clock_ghz} GHz")
    print(f"    Cores Used:       {cores_used}")
    print(f"    Threads/Core:     {threads_per_core}")
    print(f"    Total Threads:    {cores_used * threads_per_core}")
    print(f"    BW per Core:      {bw_per_core_gbs} GB/s")

    # Peak theoretical performance
    # Integer ops: 1 op/cycle/thread
    peak_int_ops_per_sec = clock_ghz * 1e9 * threads_per_core * cores_used
    peak_gops = peak_int_ops_per_sec / 1e9

    # Peak bandwidth
    peak_bw_gbs = bw_per_core_gbs * cores_used

    print(f"    Peak INT Ops:     {peak_gops:.1f} GOPs/s")
    print(f"    Peak Bandwidth:   {peak_bw_gbs:.1f} GB/s")

    # -------------------------------------------------------------------------
    # 3. Memory Access Summary
    # -------------------------------------------------------------------------
    print(f"\n[3] MEMORY ACCESS SUMMARY")

    total_l1sp = l1sp_loads + l1sp_stores
    total_l2sp = l2sp_loads + l2sp_stores
    total_dram = dram_loads + dram_stores
    total_accesses = total_l1sp + total_l2sp + total_dram

    if total_accesses > 0:
        print(f"    L1SP Accesses:    {total_l1sp:>15,}  ({100*total_l1sp/total_accesses:>5.1f}%)")
        print(f"      - Loads:        {l1sp_loads:>15,}")
        print(f"      - Stores:       {l1sp_stores:>15,}")
        print(f"    L2SP Accesses:    {total_l2sp:>15,}  ({100*total_l2sp/total_accesses:>5.1f}%)")
        print(f"      - Loads:        {l2sp_loads:>15,}")
        print(f"      - Stores:       {l2sp_stores:>15,}")
        print(f"    DRAM Accesses:    {total_dram:>15,}  ({100*total_dram/total_accesses:>5.1f}%)")
        print(f"      - Loads:        {dram_loads:>15,}")
        print(f"      - Stores:       {dram_stores:>15,}")
        print(f"    DRAM Cache Hits:  {dram_cache_hits:>15,}")
        print(f"    DRAM Cache Miss:  {dram_cache_misses:>15,}")

        if dram_cache_hits + dram_cache_misses > 0:
            hit_rate = 100 * dram_cache_hits / (dram_cache_hits + dram_cache_misses)
            print(f"    DRAM Hit Rate:    {hit_rate:>14.1f}%")

    # -------------------------------------------------------------------------
    # 4. Data Transfer Analysis
    # -------------------------------------------------------------------------
    print(f"\n[4] DATA TRANSFER ANALYSIS")

    # Assume 8 bytes per access (64-bit loads/stores typical for RISC-V)
    bytes_per_access = 8

    # Data transferred from each level
    bytes_l1sp = total_l1sp * bytes_per_access
    bytes_l2sp = total_l2sp * bytes_per_access
    bytes_dram_cached = dram_cache_hits * bytes_per_access  # Hits come from DRAM cache
    bytes_dram_actual = dram_cache_misses * bytes_per_access  # Actual DRAM traffic

    total_bytes = bytes_l1sp + bytes_l2sp + bytes_dram_cached + bytes_dram_actual

    print(f"    L1SP Traffic:     {bytes_l1sp/1e6:>12.2f} MB  (latency: ~1 cycle)")
    print(f"    L2SP Traffic:     {bytes_l2sp/1e6:>12.2f} MB  (latency: ~4 cycles)")
    print(f"    DRAM Cache Hit:   {bytes_dram_cached/1e6:>12.2f} MB  (latency: {dram_cache_hit_latency:.1f} cycles)")
    print(f"    DRAM Actual:      {bytes_dram_actual/1e6:>12.2f} MB  (latency: {dram_cache_miss_latency:.1f} cycles)")
    print(f"    Total Traffic:    {total_bytes/1e6:>12.2f} MB")

    # -------------------------------------------------------------------------
    # 5. Performance Analysis
    # -------------------------------------------------------------------------
    print(f"\n[5] PERFORMANCE ANALYSIS")

    sim_time_s = sim_time_ms / 1000.0
    sim_time_cycles = sim_time_ms * 1e6 * clock_ghz  # ms -> ns -> cycles

    print(f"    Simulation Time:  {sim_time_ms:.3f} ms ({sim_time_cycles:,.0f} cycles)")

    if sim_time_s > 0:
        # Throughput metrics
        vertices_per_sec = num_vertices / sim_time_s
        edges_per_sec = num_edges / sim_time_s
        mteps = edges_per_sec / 1e6

        print(f"    Vertices/sec:     {vertices_per_sec:,.0f}")
        print(f"    Edges/sec:        {edges_per_sec:,.0f}")
        print(f"    MTEPS:            {mteps:.2f}")

        # Achieved bandwidth
        achieved_bw_gbs = (total_bytes / 1e9) / sim_time_s
        bw_efficiency = 100 * achieved_bw_gbs / peak_bw_gbs

        print(f"    Achieved BW:      {achieved_bw_gbs:.2f} GB/s")
        print(f"    BW Efficiency:    {bw_efficiency:.1f}%")

        # Effective memory latency (weighted average)
        total_dram_accesses = dram_cache_hits + dram_cache_misses
        if total_dram_accesses > 0:
            eff_dram_latency = (dram_cache_hits * dram_cache_hit_latency +
                               dram_cache_misses * dram_cache_miss_latency) / total_dram_accesses
            print(f"    Eff DRAM Latency: {eff_dram_latency:.1f} cycles")

    # -------------------------------------------------------------------------
    # 6. Arithmetic Intensity (for Roofline)
    # -------------------------------------------------------------------------
    print(f"\n[6] ARITHMETIC INTENSITY")

    # BFS operations per vertex (integer ops, not FLOPs):
    # Per frontier vertex expansion (examining 4 neighbors):
    #   - Index calc per neighbor: 2 mul + 2 add = 4 ops
    #   - Bounds checks: 4 comparisons per neighbor = 4 ops
    #   - Total for 4 neighbors: 4 * (4 + 4) = 32 ops
    # Plus:
    #   - frontier check: 1 load + compare
    #   - dist read: 1 load
    #   - atomic CAS attempts: 4 (one per neighbor)
    # Approximate: ~50 int ops per frontier vertex

    # But with level-sync, each vertex is in frontier once, so:
    int_ops_per_vertex = 50
    # Plus iteration overhead (frontier scan by thread 0)
    frontier_swap_ops = num_iterations * num_vertices * 3  # read, write, clear
    total_int_ops = num_vertices * int_ops_per_vertex + frontier_swap_ops

    if total_bytes > 0:
        arithmetic_intensity = total_int_ops / total_bytes
        print(f"    Estimated INT ops:  {total_int_ops:,}")
        print(f"    Total Bytes:        {total_bytes:,}")
        print(f"    AI (INT ops/Byte):  {arithmetic_intensity:.4f}")
        print(f"    (Note: BFS has ~0 FLOPs, AI refers to integer operations)")

    # Ridge point
    ridge_point = peak_gops / peak_bw_gbs
    print(f"    Ridge Point:        {ridge_point:.4f} ops/byte")

    if arithmetic_intensity < ridge_point:
        print(f"    Status:             MEMORY BOUND (AI < ridge point)")
    else:
        print(f"    Status:             COMPUTE BOUND (AI >= ridge point)")

    # -------------------------------------------------------------------------
    # 7. Generate Roofline Plot
    # -------------------------------------------------------------------------
    print(f"\n[7] GENERATING ROOFLINE PLOT...")

    fig, axes = plt.subplots(1, 2, figsize=(14, 6))

    # Left: Classic Roofline
    ax1 = axes[0]
    ai_range = np.logspace(-3, 2, 500)

    # Roofline for different memory levels
    rooflines = [
        (peak_bw_gbs, 'L1SP/L2SP BW', 'green', '-'),
        (peak_bw_gbs * 0.5, 'Effective BW (50%)', 'orange', '--'),
    ]

    for bw, label, color, ls in rooflines:
        perf = np.minimum(peak_gops, ai_range * bw)
        ax1.loglog(ai_range, perf, color=color, linestyle=ls, linewidth=2, label=f'{label}: {bw:.0f} GB/s')

    # Compute ceiling
    ax1.axhline(y=peak_gops, color='red', linestyle=':', linewidth=2,
                label=f'Peak Compute: {peak_gops:.0f} GOPs/s')

    # Mark BFS point
    if total_bytes > 0 and sim_time_s > 0:
        achieved_gops = (total_int_ops / 1e9) / sim_time_s
        ax1.plot(arithmetic_intensity, achieved_gops, 'r*', markersize=20,
                label=f'BFS: AI={arithmetic_intensity:.4f}, {achieved_gops:.2f} GOPs/s')

        # Show where BFS would be at peak BW
        theoretical_gops_at_peak_bw = arithmetic_intensity * peak_bw_gbs
        ax1.plot(arithmetic_intensity, theoretical_gops_at_peak_bw, 'b^', markersize=12,
                label=f'BFS at Peak BW: {theoretical_gops_at_peak_bw:.2f} GOPs/s')

    ax1.axvline(x=arithmetic_intensity, color='purple', linestyle='--', alpha=0.5)
    ax1.axvline(x=ridge_point, color='gray', linestyle=':', alpha=0.5)
    ax1.text(ridge_point, peak_gops * 1.2, f'Ridge: {ridge_point:.3f}', fontsize=9, ha='center')

    ax1.set_xlabel('Arithmetic Intensity (INT ops/Byte)', fontsize=11)
    ax1.set_ylabel('Performance (GOPs/s)', fontsize=11)
    ax1.set_title(f'BFS Roofline ({grid_r}x{grid_c}, {cores_used}C x {threads_per_core}T)', fontsize=12)
    ax1.legend(loc='lower right', fontsize=8)
    ax1.grid(True, which='both', alpha=0.3)
    ax1.set_xlim([1e-3, 100])
    ax1.set_ylim([0.1, peak_gops * 3])

    # Right: Memory Hierarchy Breakdown
    ax2 = axes[1]

    categories = ['L1SP', 'L2SP', 'DRAM\nCache', 'DRAM\nActual']
    values = [bytes_l1sp/1e6, bytes_l2sp/1e6, bytes_dram_cached/1e6, bytes_dram_actual/1e6]
    colors = ['#2ecc71', '#3498db', '#f39c12', '#e74c3c']

    bars = ax2.bar(categories, values, color=colors, alpha=0.8)

    for bar, val in zip(bars, values):
        if val > 0:
            ax2.text(bar.get_x() + bar.get_width()/2, bar.get_height() + max(values)*0.02,
                    f'{val:.1f} MB', ha='center', fontsize=9)

    ax2.set_ylabel('Data Transferred (MB)', fontsize=11)
    ax2.set_title('Memory Hierarchy Data Traffic', fontsize=12)
    ax2.grid(True, axis='y', alpha=0.3)

    plt.tight_layout()

    output_file = f"{output_prefix}_analysis.png"
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"    Saved plot to: {output_file}")

    plt.close()

    # -------------------------------------------------------------------------
    # 8. Summary
    # -------------------------------------------------------------------------
    print(f"\n" + "=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print(f"    Grid:             {grid_r}x{grid_c} ({num_vertices} vertices)")
    print(f"    Config:           {cores_used} core(s) x {threads_per_core} threads")
    print(f"    Time:             {sim_time_ms:.3f} ms")
    if sim_time_s > 0:
        print(f"    MTEPS:            {mteps:.2f}")
        print(f"    Achieved BW:      {achieved_bw_gbs:.2f} GB/s ({bw_efficiency:.1f}% of peak)")
        print(f"    AI:               {arithmetic_intensity:.4f} (memory-bound)")
    print("=" * 80)

    return {
        'mteps': mteps if sim_time_s > 0 else 0,
        'achieved_bw_gbs': achieved_bw_gbs if sim_time_s > 0 else 0,
        'arithmetic_intensity': arithmetic_intensity if total_bytes > 0 else 0,
        'bw_efficiency': bw_efficiency if sim_time_s > 0 else 0,
    }


def main():
    parser = argparse.ArgumentParser(description='BFS Roofline Analysis from PANDOHammer stats')

    # Grid
    parser.add_argument('--grid-r', type=int, default=64)
    parser.add_argument('--grid-c', type=int, default=64)

    # Architecture
    parser.add_argument('--cores', type=int, default=1)
    parser.add_argument('--threads-per-core', type=int, default=16)
    parser.add_argument('--bw-per-core', type=float, default=24.0)

    # Memory stats
    parser.add_argument('--l1sp-loads', type=int, default=0)
    parser.add_argument('--l1sp-stores', type=int, default=0)
    parser.add_argument('--l2sp-loads', type=int, default=0)
    parser.add_argument('--l2sp-stores', type=int, default=0)
    parser.add_argument('--dram-loads', type=int, default=0)
    parser.add_argument('--dram-stores', type=int, default=0)
    parser.add_argument('--dram-cache-hits', type=int, default=0)
    parser.add_argument('--dram-cache-misses', type=int, default=0)
    parser.add_argument('--dram-hit-latency', type=float, default=14.0,
                        help='Core-perspective cache hit latency (includes interconnect)')
    parser.add_argument('--dram-miss-latency', type=float, default=123.0,
                        help='Core-perspective cache miss latency (includes interconnect)')

    # Time
    parser.add_argument('--time-ms', type=float, default=0)

    # Output
    parser.add_argument('--output-prefix', type=str, default='bfs_roofline')

    args = parser.parse_args()

    analyze_bfs_roofline(
        grid_r=args.grid_r,
        grid_c=args.grid_c,
        cores_used=args.cores,
        threads_per_core=args.threads_per_core,
        bw_per_core_gbs=args.bw_per_core,
        l1sp_loads=args.l1sp_loads,
        l1sp_stores=args.l1sp_stores,
        l2sp_loads=args.l2sp_loads,
        l2sp_stores=args.l2sp_stores,
        dram_loads=args.dram_loads,
        dram_stores=args.dram_stores,
        dram_cache_hits=args.dram_cache_hits,
        dram_cache_misses=args.dram_cache_misses,
        dram_cache_hit_latency=args.dram_hit_latency,
        dram_cache_miss_latency=args.dram_miss_latency,
        sim_time_ms=args.time_ms,
        output_prefix=args.output_prefix,
    )


if __name__ == '__main__':
    main()
