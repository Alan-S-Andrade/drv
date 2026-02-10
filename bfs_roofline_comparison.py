#!/usr/bin/env python3
"""
BFS Roofline Comparison - Multiple Configurations
"""

import os
import matplotlib
if os.environ.get('DISPLAY') is None:
    matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

def generate_roofline_comparison():
    """Generate roofline comparison for BFS experiments."""

    # Architecture parameters
    clock_ghz = 1.0
    threads_per_core = 16
    bw_per_core_gbs = 24.0

    # Grid parameters
    R, C = 64, 64
    N = R * C  # 4096 vertices
    E = 2 * (R-1) * C + 2 * R * (C-1)  # 16128 edges
    iterations = R + C - 2  # 126

    # =========================================================================
    # Configuration 1: 1 core (16 threads)
    # =========================================================================
    config1 = {
        'name': '1 Core (16 threads)',
        'cores': 1,
        'threads': 16,
        'l1sp_loads': 101_223_480,
        'l1sp_stores': 0,
        'dram_loads': 3_334_063,
        'dram_stores': 10_941,
        'dram_cache_hits': 3_326_565,
        'dram_cache_misses': 18_450,
        'time_ms': 253.552,
    }

    # =========================================================================
    # Configuration 2: 4 cores (64 threads) - if you have the time, update here
    # =========================================================================
    config2 = {
        'name': '4 Cores (64 threads)',
        'cores': 4,
        'threads': 64,
        'l1sp_loads': 369_065_158,
        'l1sp_stores': 0,
        'dram_loads': 3_695_672,
        'dram_stores': 10_941,
        'dram_cache_hits': 3_687_048,
        'dram_cache_misses': 19_576,
        'time_ms': None,  # UPDATE THIS when you have the value
    }

    configs = [config1]
    if config2['time_ms'] is not None:
        configs.append(config2)

    # =========================================================================
    # Calculate metrics for each configuration
    # =========================================================================
    results = []
    for cfg in configs:
        cores = cfg['cores']
        peak_gops = clock_ghz * threads_per_core * cores  # 1 int op/cycle/thread
        peak_bw = bw_per_core_gbs * cores

        total_l1sp = cfg['l1sp_loads'] + cfg['l1sp_stores']
        total_dram = cfg['dram_loads'] + cfg['dram_stores']
        total_accesses = total_l1sp + total_dram

        bytes_per_access = 8
        total_bytes = total_accesses * bytes_per_access

        time_s = cfg['time_ms'] / 1000.0 if cfg['time_ms'] else None

        # Arithmetic intensity (using measured bytes)
        # BFS int ops estimate: ~50 ops per vertex + iteration overhead
        int_ops_per_vertex = 50
        frontier_swap_ops = iterations * N * 3
        total_int_ops = N * int_ops_per_vertex + frontier_swap_ops

        ai = total_int_ops / total_bytes if total_bytes > 0 else 0

        achieved_gops = None
        achieved_bw = None
        mteps = None

        if time_s:
            achieved_gops = (total_int_ops / 1e9) / time_s
            achieved_bw = (total_bytes / 1e9) / time_s
            mteps = (E / 1e6) / time_s

        results.append({
            'name': cfg['name'],
            'cores': cores,
            'peak_gops': peak_gops,
            'peak_bw': peak_bw,
            'total_bytes': total_bytes,
            'ai': ai,
            'achieved_gops': achieved_gops,
            'achieved_bw': achieved_bw,
            'mteps': mteps,
            'time_ms': cfg['time_ms'],
        })

    # =========================================================================
    # Print summary
    # =========================================================================
    print("=" * 80)
    print("BFS ROOFLINE COMPARISON - 64x64 Grid")
    print("=" * 80)

    for r in results:
        print(f"\n{r['name']}:")
        print(f"  Peak Compute:    {r['peak_gops']:.0f} GOPs/s")
        print(f"  Peak Bandwidth:  {r['peak_bw']:.0f} GB/s")
        print(f"  Total Bytes:     {r['total_bytes']/1e6:.1f} MB")
        print(f"  Arith. Intens.:  {r['ai']:.5f} ops/byte")
        if r['time_ms']:
            print(f"  Time:            {r['time_ms']:.3f} ms")
            print(f"  Achieved GOPs/s: {r['achieved_gops']:.3f}")
            print(f"  Achieved BW:     {r['achieved_bw']:.2f} GB/s ({100*r['achieved_bw']/r['peak_bw']:.1f}% of peak)")
            print(f"  MTEPS:           {r['mteps']:.3f}")

    # =========================================================================
    # Generate roofline plot
    # =========================================================================
    fig, axes = plt.subplots(1, 2, figsize=(16, 6))

    # Left: Roofline Model
    ax1 = axes[0]
    ai_range = np.logspace(-4, 2, 500)

    # Colors for different core counts
    colors = ['#3498db', '#e74c3c', '#2ecc71', '#9b59b6']
    markers = ['o', 's', '^', 'D']

    for i, r in enumerate(results):
        # Plot roofline
        roofline = np.minimum(r['peak_gops'], ai_range * r['peak_bw'])
        ax1.loglog(ai_range, roofline, color=colors[i], linewidth=2,
                   label=f"{r['name']} Roof (Peak={r['peak_gops']:.0f} GOPs/s, BW={r['peak_bw']:.0f} GB/s)")

        # Plot achieved point
        if r['achieved_gops']:
            ax1.plot(r['ai'], r['achieved_gops'], marker=markers[i], markersize=15,
                    color=colors[i], markeredgecolor='black', markeredgewidth=1.5,
                    label=f"{r['name']} Achieved ({r['achieved_gops']:.3f} GOPs/s)")

    # Ridge point annotation (using first config)
    ridge = results[0]['peak_gops'] / results[0]['peak_bw']
    ax1.axvline(x=ridge, color='gray', linestyle=':', alpha=0.5)
    ax1.text(ridge*1.1, results[0]['peak_gops']*0.1, f'Ridge: {ridge:.3f}',
             fontsize=9, rotation=90, va='bottom')

    # BFS region annotation
    ax1.axvspan(1e-4, 0.01, alpha=0.1, color='blue', label='BFS region')

    ax1.set_xlabel('Arithmetic Intensity (INT ops/Byte)', fontsize=11)
    ax1.set_ylabel('Performance (GOPs/s)', fontsize=11)
    ax1.set_title(f'BFS Roofline Model ({R}x{C} Grid)', fontsize=12, fontweight='bold')
    ax1.legend(loc='lower right', fontsize=8)
    ax1.grid(True, which='both', alpha=0.3)
    ax1.set_xlim([1e-4, 100])
    ax1.set_ylim([1e-3, 200])

    # Right: Bandwidth efficiency comparison
    ax2 = axes[1]

    if len(results) > 0:
        names = [r['name'] for r in results]
        peak_bws = [r['peak_bw'] for r in results]
        achieved_bws = [r['achieved_bw'] if r['achieved_bw'] else 0 for r in results]

        x = np.arange(len(names))
        width = 0.35

        bars1 = ax2.bar(x - width/2, peak_bws, width, label='Peak BW', color='lightblue', edgecolor='black')
        bars2 = ax2.bar(x + width/2, achieved_bws, width, label='Achieved BW', color='steelblue', edgecolor='black')

        # Add efficiency percentage
        for i, (peak, achieved) in enumerate(zip(peak_bws, achieved_bws)):
            if achieved > 0:
                eff = 100 * achieved / peak
                ax2.annotate(f'{eff:.1f}%', xy=(i + width/2, achieved), ha='center', va='bottom',
                            fontsize=10, fontweight='bold')

        ax2.set_ylabel('Bandwidth (GB/s)', fontsize=11)
        ax2.set_title('Bandwidth Efficiency', fontsize=12, fontweight='bold')
        ax2.set_xticks(x)
        ax2.set_xticklabels(names)
        ax2.legend()
        ax2.grid(True, axis='y', alpha=0.3)

    plt.tight_layout()
    output_file = 'bfs_roofline_comparison.png'
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"\nSaved plot to: {output_file}")
    plt.close()

    # =========================================================================
    # Memory hierarchy breakdown
    # =========================================================================
    fig2, ax = plt.subplots(figsize=(10, 6))

    categories = ['L1SP\nLoads', 'DRAM\nLoads', 'DRAM\nStores', 'DRAM Cache\nHits', 'DRAM Cache\nMisses']

    for i, cfg in enumerate(configs):
        values = [
            cfg['l1sp_loads'] / 1e6,
            cfg['dram_loads'] / 1e6,
            cfg['dram_stores'] / 1e3,  # Note: in thousands for stores
            cfg['dram_cache_hits'] / 1e6,
            cfg['dram_cache_misses'] / 1e3,  # In thousands
        ]

        x = np.arange(len(categories))
        width = 0.35
        offset = (i - len(configs)/2 + 0.5) * width
        bars = ax.bar(x + offset, values, width, label=cfg['name'], alpha=0.8)

    ax.set_ylabel('Count (Millions, except Stores/Misses in Thousands)', fontsize=10)
    ax.set_title('Memory Access Breakdown', fontsize=12, fontweight='bold')
    ax.set_xticks(np.arange(len(categories)))
    ax.set_xticklabels(categories)
    ax.legend()
    ax.set_yscale('log')
    ax.grid(True, axis='y', alpha=0.3)

    plt.tight_layout()
    output_file2 = 'bfs_memory_breakdown.png'
    plt.savefig(output_file2, dpi=150, bbox_inches='tight')
    print(f"Saved plot to: {output_file2}")
    plt.close()

    # =========================================================================
    # Key insights
    # =========================================================================
    print("\n" + "=" * 80)
    print("KEY INSIGHTS")
    print("=" * 80)
    print("""
1. BFS is EXTREMELY memory-bound (AI << 1)
   - Arithmetic intensity ~0.0003 ops/byte
   - Ridge point ~0.67 ops/byte
   - BFS is ~2000x below the ridge point

2. Bandwidth utilization is low (~13%)
   - Achieved: ~3.3 GB/s vs Peak: 24 GB/s
   - Causes: Random access patterns, barrier synchronization,
     each thread scanning entire vertex array

3. High memory access amplification (66x)
   - Theoretical: ~1.6M accesses
   - Measured: ~104M accesses
   - Due to: All threads scanning entire array each iteration

4. Optimization opportunities:
   - Work partitioning: Each thread should only check its vertices
   - Frontier compaction: Use a queue instead of dense array scan
   - Reduce barrier frequency or use async barriers
""")
    print("=" * 80)


if __name__ == '__main__':
    generate_roofline_comparison()
