#!/usr/bin/env python3
"""Plot L1SP model vs baseline BFS performance across core counts.
All runs: RMAT 65K graph (N=65536, E=1818338), 16 harts/core, 46551 nodes discovered."""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# --- Data: total_cycles by core count ---
cores = [2, 4, 8, 16, 32, 64]

# Baseline: nosteal, static balanced repartition
baseline_cycles = [
    218_278_516,
    196_397_562,
    194_981_865,
    157_838_265,
    199_416_680,
    319_928_113,
]
baseline_imbalance = [4.7, 7.2, 11.6, 6.7, 6.6, 8.8]

# L1SP work cache + adaptive stealing
l1sp_cycles = [
    234_198_321,
    206_325_147,
    200_486_714,
    189_512_430,
    208_618_214,
    245_627_260,
]
l1sp_imbalance = [4.5, 7.3, 11.3, 12.8, 17.2, 23.5]

# Adaptive stealing (no L1SP cache)
adaptive_cycles = [
    332_416_661,
    198_961_818,
    200_184_680,
    297_489_466,  # anomalous (different frontier strategy)
    198_959_864,
    210_076_810,
]
adaptive_imbalance = [None, 7.5, 12.2, 15.6, 16.2, 18.3]

# Single-core reference
single_core_cycles = 312_724_401

# Convert to millions
baseline_M = [c / 1e6 for c in baseline_cycles]
l1sp_M = [c / 1e6 for c in l1sp_cycles]
adaptive_M = [c / 1e6 for c in adaptive_cycles]
single_M = single_core_cycles / 1e6

# Speedup relative to single-core
baseline_speedup = [single_core_cycles / c for c in baseline_cycles]
l1sp_speedup = [single_core_cycles / c for c in l1sp_cycles]
adaptive_speedup = [single_core_cycles / c for c in adaptive_cycles]

# --- Figure: 2x2 layout ---
fig, axes = plt.subplots(2, 2, figsize=(14, 10))
fig.suptitle('BFS Performance: Baseline vs L1SP Model vs Adaptive Steal\n'
             'RMAT 65K (N=65536, E=1.8M), 16 harts/core', fontsize=14, fontweight='bold')

# --- Plot 1: Total Cycles ---
ax1 = axes[0, 0]
x = np.arange(len(cores))
w = 0.25
bars1 = ax1.bar(x - w, baseline_M, w, label='Baseline (no steal)', color='#4e79a7', edgecolor='black', linewidth=0.5)
bars2 = ax1.bar(x, l1sp_M, w, label='L1SP Cache + Steal', color='#e15759', edgecolor='black', linewidth=0.5)
bars3 = ax1.bar(x + w, adaptive_M, w, label='Adaptive Steal', color='#76b7b2', edgecolor='black', linewidth=0.5, alpha=0.7)
ax1.axhline(y=single_M, color='gray', linestyle='--', linewidth=1, alpha=0.6, label=f'1-core ref ({single_M:.0f}M)')
ax1.set_xlabel('Number of Cores')
ax1.set_ylabel('Total Cycles (millions)')
ax1.set_title('Total Execution Cycles')
ax1.set_xticks(x)
ax1.set_xticklabels(cores)
ax1.legend(fontsize=8, loc='upper right')
ax1.grid(axis='y', alpha=0.3)

# --- Plot 2: Speedup ---
ax2 = axes[0, 1]
ax2.plot(cores, baseline_speedup, 'o-', color='#4e79a7', linewidth=2, markersize=7, label='Baseline (no steal)')
ax2.plot(cores, l1sp_speedup, 's-', color='#e15759', linewidth=2, markersize=7, label='L1SP Cache + Steal')
ax2.plot(cores, adaptive_speedup, '^--', color='#76b7b2', linewidth=2, markersize=7, alpha=0.7, label='Adaptive Steal')
ax2.plot(cores, [1]*len(cores), ':', color='gray', linewidth=1, alpha=0.5)
# Ideal linear speedup from 1-core
ideal = [single_core_cycles / (single_core_cycles / c) for c in cores]
ax2.plot(cores, cores, '--', color='lightgray', linewidth=1, label='Ideal linear')
ax2.set_xlabel('Number of Cores')
ax2.set_ylabel('Speedup (vs 1-core)')
ax2.set_title('Speedup Scaling')
ax2.set_xscale('log', base=2)
ax2.set_xticks(cores)
ax2.set_xticklabels(cores)
ax2.legend(fontsize=8)
ax2.grid(alpha=0.3)

# --- Plot 3: Imbalance ---
ax3 = axes[1, 0]
ax3.plot(cores, baseline_imbalance, 'o-', color='#4e79a7', linewidth=2, markersize=7, label='Baseline (no steal)')
ax3.plot(cores, l1sp_imbalance, 's-', color='#e15759', linewidth=2, markersize=7, label='L1SP Cache + Steal')
# Adaptive has a None at index 0
adaptive_cores_valid = [cores[i] for i in range(len(cores)) if adaptive_imbalance[i] is not None]
adaptive_imb_valid = [v for v in adaptive_imbalance if v is not None]
ax3.plot(adaptive_cores_valid, adaptive_imb_valid, '^--', color='#76b7b2', linewidth=2, markersize=7, alpha=0.7, label='Adaptive Steal')
ax3.axhline(y=5, color='green', linestyle=':', linewidth=1.5, alpha=0.6, label='Perfect balance (<5%)')
ax3.set_xlabel('Number of Cores')
ax3.set_ylabel('Imbalance %  (max−min)/max')
ax3.set_title('Work Imbalance')
ax3.set_xticks(cores)
ax3.legend(fontsize=8)
ax3.grid(alpha=0.3)
ax3.set_ylim(0, 30)

# --- Plot 4: Cycles per discovered node ---
nodes_discovered = 46551
baseline_cpn = [c / nodes_discovered for c in baseline_cycles]
l1sp_cpn = [c / nodes_discovered for c in l1sp_cycles]
adaptive_cpn = [c / nodes_discovered for c in adaptive_cycles]
single_cpn = single_core_cycles / nodes_discovered

ax4 = axes[1, 1]
ax4.plot(cores, baseline_cpn, 'o-', color='#4e79a7', linewidth=2, markersize=7, label='Baseline (no steal)')
ax4.plot(cores, l1sp_cpn, 's-', color='#e15759', linewidth=2, markersize=7, label='L1SP Cache + Steal')
ax4.plot(cores, adaptive_cpn, '^--', color='#76b7b2', linewidth=2, markersize=7, alpha=0.7, label='Adaptive Steal')
ax4.axhline(y=single_cpn, color='gray', linestyle='--', linewidth=1, alpha=0.6, label=f'1-core ({single_cpn:.0f})')
ax4.set_xlabel('Number of Cores')
ax4.set_ylabel('Cycles / Discovered Node')
ax4.set_title('Per-Node Efficiency')
ax4.set_xticks(cores)
ax4.legend(fontsize=8)
ax4.grid(alpha=0.3)

plt.tight_layout(rect=[0, 0, 1, 0.94])
out = '/users/alanandr/drv/bfs_model_vs_baseline.png'
plt.savefig(out, dpi=150, bbox_inches='tight')
print(f'Saved: {out}')
