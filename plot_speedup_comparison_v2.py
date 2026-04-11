#!/usr/bin/env python3
"""Plot speedup: shared-queue work-stealing BFS vs all-to-core-0 imbalanced baseline."""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import csv
import sys
import os

CSV_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'speedup_sweep', 'speedup_results.csv')
OUT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'bfs_speedup_comparison.png')

if len(sys.argv) > 1:
    CSV_PATH = sys.argv[1]
if len(sys.argv) > 2:
    OUT_PATH = sys.argv[2]

# Parse CSV
data = {}  # variant -> {cores: cycles}
with open(CSV_PATH) as f:
    reader = csv.DictReader(f, fieldnames=['variant','cores','cycles_elapsed','nodes_discovered'])
    for row in reader:
        v = row['variant']
        if v not in ('perfect_ws', 'imbalanced'):
            continue
        c = int(row['cores'])
        if not row['cycles_elapsed']:
            continue
        cy = int(row['cycles_elapsed'])
        data.setdefault(v, {}).setdefault(c, cy)  # keep first occurrence only

print("Parsed data:")
for v, vd in data.items():
    for c in sorted(vd):
        print(f"  {v} @ {c}c: {vd[c]:,} cycles")

variants = {
    'perfect_ws':  {'label': 'Shared-Queue Work Stealing', 'color': '#e15759', 'marker': 's'},
    'imbalanced':  {'label': 'Imbalanced Baseline (all-to-core-0)', 'color': '#4e79a7', 'marker': 'o'},
}

fig, axes = plt.subplots(1, 2, figsize=(14, 5.5))
fig.suptitle('BFS: Shared-Queue Work Stealing vs Imbalanced Baseline\n'
             'RMAT Power-Law Graph, All Frontier Assigned to Core 0, 16 Harts/Core',
             fontsize=12, fontweight='bold')

# ---- Left: Total Cycles (bar chart) ----
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
            ax1.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.02,
                     f'{val:.2f}M', ha='center', va='bottom', fontsize=7)

ax1.set_xlabel('Number of Cores')
ax1.set_ylabel('Total Cycles (millions)')
ax1.set_title('Execution Time (lower is better)')
ax1.set_xticks(x)
ax1.set_xticklabels(all_cores)
ax1.legend(fontsize=8, loc='upper right')
ax1.grid(axis='y', alpha=0.3)

# ---- Right: Speedup vs 1-core imbalanced baseline ----
ax2 = axes[1]
ref_cycles = data['imbalanced'][1]  # 1-core imbalanced = serial reference

for vname, style in variants.items():
    if vname not in data:
        continue
    vd = data[vname]
    cores_sorted = sorted(vd.keys())
    speedup = [ref_cycles / vd[c] for c in cores_sorted]
    ax2.plot(cores_sorted, speedup, f'{style["marker"]}-',
             color=style['color'], linewidth=2, markersize=8, label=style['label'])
    for c, sp in zip(cores_sorted, speedup):
        ax2.annotate(f'{sp:.2f}x', (c, sp), textcoords='offset points',
                     xytext=(0, 8), ha='center', fontsize=7)

# Reference lines
ax2.plot(all_cores, all_cores, '--', color='lightgray', linewidth=1.5, label='Ideal linear')
ax2.axhline(y=1, color='gray', linestyle=':', linewidth=0.8, alpha=0.5)
ax2.set_xlabel('Number of Cores')
ax2.set_ylabel('Speedup (vs 1-core baseline)')
ax2.set_title('Speedup Relative to Imbalanced 1-Core')
ax2.set_xscale('log', base=2)
ax2.set_xticks(all_cores)
ax2.set_xticklabels(all_cores)
ax2.legend(fontsize=8)
ax2.grid(alpha=0.3)

plt.tight_layout(rect=[0, 0, 1, 0.88])
plt.savefig(OUT_PATH, dpi=150, bbox_inches='tight')
print(f'\nSaved: {OUT_PATH}')
