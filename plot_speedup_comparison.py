#!/usr/bin/env python3
"""Plot speedup: perfect work-stealing BFS vs no-steal baseline on RMAT power-law graph."""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import csv
import sys
import os

CSV_PATH = os.path.join(os.path.dirname(__file__), 'speedup_sweep', 'speedup_results.csv')
OUT_PATH = os.path.join(os.path.dirname(__file__), 'bfs_speedup_comparison.png')

if len(sys.argv) > 1:
    CSV_PATH = sys.argv[1]
if len(sys.argv) > 2:
    OUT_PATH = sys.argv[2]

# Parse CSV
data = {}  # variant -> {cores: cycles}
with open(CSV_PATH) as f:
    reader = csv.DictReader(f)
    for row in reader:
        v = row['variant']
        c = int(row['cores'])
        cy = int(row['cycles_elapsed'])
        data.setdefault(v, {})[c] = cy

# Determine single-core reference for each variant
variants = {
    'perfect_ws':  {'label': 'Work Stealing (vertex-ownership)', 'color': '#e15759', 'marker': 's'},
    'imbalanced':  {'label': 'Imbalanced Baseline (no steal)',   'color': '#4e79a7', 'marker': 'o'},
}

fig, axes = plt.subplots(1, 2, figsize=(13, 5.5))
fig.suptitle('BFS Speedup: Work Stealing vs Imbalanced Baseline\n'
             'RMAT Power-Law Graph (vertex-ownership distribution)', fontsize=13, fontweight='bold')

# ---- Left: Total Cycles ----
ax1 = axes[0]
all_cores = sorted(set(c for vd in data.values() for c in vd))
x = np.arange(len(all_cores))
w = 0.35
for i, (vname, style) in enumerate(variants.items()):
    if vname not in data:
        continue
    cycles_M = [data[vname].get(c, 0) / 1e6 for c in all_cores]
    offset = (i - 0.5) * w
    bars = ax1.bar(x + offset, cycles_M, w,
                   label=style['label'], color=style['color'],
                   edgecolor='black', linewidth=0.5)
    for bar, val in zip(bars, cycles_M):
        if val > 0:
            ax1.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.5,
                     f'{val:.1f}M', ha='center', va='bottom', fontsize=7)

ax1.set_xlabel('Number of Cores')
ax1.set_ylabel('Total Cycles (millions)')
ax1.set_title('Execution Time')
ax1.set_xticks(x)
ax1.set_xticklabels(all_cores)
ax1.legend(fontsize=9)
ax1.grid(axis='y', alpha=0.3)

# ---- Right: Speedup (relative to own 1-core) ----
ax2 = axes[1]
for vname, style in variants.items():
    if vname not in data:
        continue
    vd = data[vname]
    cores_sorted = sorted(vd.keys())
    ref = vd.get(1, vd[min(vd)])  # 1-core or smallest
    speedup = [ref / vd[c] for c in cores_sorted]
    ax2.plot(cores_sorted, speedup, f'{style["marker"]}-',
             color=style['color'], linewidth=2, markersize=8, label=style['label'])

# Ideal linear
ax2.plot(all_cores, all_cores, '--', color='lightgray', linewidth=1.5, label='Ideal linear')
ax2.set_xlabel('Number of Cores')
ax2.set_ylabel('Speedup (vs 1-core)')
ax2.set_title('Speedup Scaling')
ax2.set_xscale('log', base=2)
ax2.set_yscale('log', base=2)
ax2.set_xticks(all_cores)
ax2.set_xticklabels(all_cores)
ax2.set_yticks(all_cores)
ax2.set_yticklabels(all_cores)
ax2.legend(fontsize=9)
ax2.grid(alpha=0.3)

plt.tight_layout(rect=[0, 0, 1, 0.92])
plt.savefig(OUT_PATH, dpi=150, bbox_inches='tight')
print(f'Saved: {OUT_PATH}')
