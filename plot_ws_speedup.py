#!/usr/bin/env python3
"""Plot work-stealing speedup over imbalanced baseline.

Reads ws_speedup_results.csv (from sweep_ws_speedup.sh) and produces:
  1. Speedup bar chart (baseline_cycles / ws_cycles)
  2. Cycle comparison bar chart
  3. Roofline overlay (if graph stats available)

Output: ws_speedup.png
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import csv
import os
import sys

csv_file = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        'ws_speedup_results.csv')
if len(sys.argv) > 1:
    csv_file = sys.argv[1]

# Parse results
baseline = {}   # cores → cycles
ws = {}
baseline_imbal = {}
ws_imbal = {}
baseline_edges = {}
ws_edges = {}

with open(csv_file) as f:
    reader = csv.DictReader(f)
    for row in reader:
        cores = int(row['cores'])
        cycles = int(row['cycles'])
        imbal = int(row.get('imbalance_pct', 0) or 0)
        edges = int(row.get('total_edges', 0) or 0)
        if row['variant'] == 'baseline':
            baseline[cores] = cycles
            baseline_imbal[cores] = imbal
            baseline_edges[cores] = edges
        else:
            ws[cores] = cycles
            ws_imbal[cores] = imbal
            ws_edges[cores] = edges

cores_list = sorted(set(baseline.keys()) & set(ws.keys()))
if not cores_list:
    print("ERROR: no matching core counts between baseline and ws")
    sys.exit(1)

speedups = [baseline[c] / ws[c] for c in cores_list]

# ─── Figure: 3-panel ───
fig, axes = plt.subplots(1, 3, figsize=(18, 6))

# Panel 1: Speedup
ax1 = axes[0]
colors1 = ['#e15759' if s > 1 else '#4e79a7' for s in speedups]
bars1 = ax1.bar(range(len(cores_list)), speedups, color=colors1, edgecolor='black', linewidth=0.5)
ax1.set_xticks(range(len(cores_list)))
ax1.set_xticklabels([f'{c}c' for c in cores_list])
ax1.set_ylabel('Speedup (baseline / work-stealing)')
ax1.set_xlabel('Cores')
ax1.set_title('Work Stealing Speedup\nover Imbalanced Baseline')
ax1.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5, linewidth=1)
for i, (s, c) in enumerate(zip(speedups, cores_list)):
    ax1.annotate(f'{s:.2f}x', (i, s), textcoords='offset points',
                 xytext=(0, 5), ha='center', fontsize=11, fontweight='bold',
                 color='#e15759' if s > 1 else '#4e79a7')

# Panel 2: Cycle comparison
ax2 = axes[1]
x = np.arange(len(cores_list))
width = 0.35
b_cycles = [baseline[c] / 1e6 for c in cores_list]
w_cycles = [ws[c] / 1e6 for c in cores_list]
ax2.bar(x - width/2, b_cycles, width, label='Imbalanced Baseline',
        color='#4e79a7', edgecolor='black', linewidth=0.5)
ax2.bar(x + width/2, w_cycles, width, label='Work Stealing',
        color='#e15759', edgecolor='black', linewidth=0.5)
ax2.set_xticks(x)
ax2.set_xticklabels([f'{c}c' for c in cores_list])
ax2.set_ylabel('Cycles (millions)')
ax2.set_xlabel('Cores')
ax2.set_title('BFS Execution Time\n(lower is better)')
ax2.legend(fontsize=9)

# Panel 3: Imbalance comparison
ax3 = axes[2]
b_imbal = [baseline_imbal.get(c, 0) for c in cores_list]
w_imbal = [ws_imbal.get(c, 0) for c in cores_list]
ax3.bar(x - width/2, b_imbal, width, label='Imbalanced Baseline',
        color='#4e79a7', edgecolor='black', linewidth=0.5)
ax3.bar(x + width/2, w_imbal, width, label='Work Stealing',
        color='#e15759', edgecolor='black', linewidth=0.5)
ax3.set_xticks(x)
ax3.set_xticklabels([f'{c}c' for c in cores_list])
ax3.set_ylabel('Edge Imbalance %')
ax3.set_xlabel('Cores')
ax3.set_title('Load Imbalance\n(max−min)/max edges per core')
ax3.legend(fontsize=9)
for i, (bi, wi) in enumerate(zip(b_imbal, w_imbal)):
    ax3.annotate(f'{bi}%', (i - width/2, bi), textcoords='offset points',
                 xytext=(0, 3), ha='center', fontsize=9, color='#4e79a7')
    ax3.annotate(f'{wi}%', (i + width/2, wi), textcoords='offset points',
                 xytext=(0, 3), ha='center', fontsize=9, color='#e15759')

plt.suptitle('RMAT Power-Law BFS: Vertex-Ownership Imbalance vs Work Stealing',
             fontsize=14, fontweight='bold', y=1.02)
plt.tight_layout()

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ws_speedup.png')
plt.savefig(out, dpi=150, bbox_inches='tight')
print(f'Saved: {out}')

# Also print summary table
print("\nSummary:")
print(f"{'Cores':>5} | {'Baseline':>12} | {'WS':>12} | {'Speedup':>8} | {'Baseline Imbal':>14} | {'WS Imbal':>8}")
print("-" * 70)
for c in cores_list:
    print(f"{c:>5} | {baseline[c]:>12,} | {ws[c]:>12,} | {speedups[cores_list.index(c)]:>8.2f}x | {baseline_imbal.get(c,0):>13}% | {ws_imbal.get(c,0):>7}%")
