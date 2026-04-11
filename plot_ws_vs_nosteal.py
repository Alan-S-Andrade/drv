#!/usr/bin/env python3
"""Plot speedup: perfect-load-balance work-stealing BFS vs no-stealing baseline
on RMAT power-law graphs (N=32768)."""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import os, re, sys

DRV = os.path.dirname(os.path.abspath(__file__))
SWEEP = os.path.join(DRV, 'speedup_sweep')
OUT   = os.path.join(DRV, 'bfs_ws_vs_nosteal.png')

# ---------- collect data from output.txt files ----------
def parse_cycles(outfile):
    """Return (cycles, nodes) from an output.txt, or (None, None)."""
    if not os.path.isfile(outfile):
        return None, None
    with open(outfile) as f:
        text = f.read()
    cm = re.search(r'Cycles elapsed:\s+(\d+)', text)
    nm = re.search(r'Nodes discovered:\s+(\d+)', text)
    if cm:
        return int(cm.group(1)), int(nm.group(1)) if nm else None
    return None, None

ws_data   = {}  # cores -> cycles
bl_data   = {}  # cores -> cycles

for d in sorted(os.listdir(SWEEP)):
    fp = os.path.join(SWEEP, d, 'output.txt')
    cycles, nodes = parse_cycles(fp)
    if cycles is None:
        continue
    m = re.match(r'(perfect_ws|nosteal)_(\d+)c', d)
    if not m:
        continue
    variant, cores = m.group(1), int(m.group(2))
    if variant == 'perfect_ws':
        ws_data[cores] = cycles
    elif variant == 'nosteal':
        bl_data[cores] = cycles

print("Perfect WS data:", {c: f"{v:,}" for c,v in sorted(ws_data.items())})
print("Nosteal BL data:", {c: f"{v:,}" for c,v in sorted(bl_data.items())})

# Core counts where we have BOTH variants
common = sorted(set(ws_data) & set(bl_data))
if not common:
    print("ERROR: no matching core counts for both variants")
    sys.exit(1)

# ---------- plot ----------
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5.5))
fig.suptitle('BFS on RMAT Power-Law Graph (N=32768, 16 harts/core)\n'
             'Perfect Load Balancing vs No-Stealing Baseline',
             fontsize=13, fontweight='bold')

C_WS = '#2ca02c'
C_BL = '#d62728'

# --- Left: Total cycles at each core count ---
x = np.arange(len(common))
w = 0.35
ws_cycles = [ws_data[c] / 1e6 for c in common]
bl_cycles = [bl_data[c] / 1e6 for c in common]

bars_bl = ax1.bar(x - w/2, bl_cycles, w, label='No-Stealing Baseline', color=C_BL,
                  edgecolor='black', linewidth=0.5)
bars_ws = ax1.bar(x + w/2, ws_cycles, w, label='Perfect Load Balancing', color=C_WS,
                  edgecolor='black', linewidth=0.5)

for bars in [bars_bl, bars_ws]:
    for bar in bars:
        h = bar.get_height()
        ax1.text(bar.get_x() + bar.get_width()/2, h + 0.5,
                 f'{h:.1f}M', ha='center', va='bottom', fontsize=8)

ax1.set_xlabel('Number of Cores', fontsize=11)
ax1.set_ylabel('Total Cycles (millions)', fontsize=11)
ax1.set_title('Execution Time (lower is better)', fontsize=11)
ax1.set_xticks(x)
ax1.set_xticklabels(common)
ax1.legend(fontsize=9, loc='upper right')
ax1.grid(axis='y', alpha=0.3)
ax1.set_ylim(bottom=0)

# --- Right: Speedup ---
speedup = [bl_data[c] / ws_data[c] for c in common]
ax2.plot(common, speedup, 's-', color=C_WS, markersize=9, linewidth=2,
         label='Speedup (baseline / WS)')
ax2.axhline(1.0, color='gray', ls='--', lw=0.8, label='1x (break even)')
for c, s in zip(common, speedup):
    ax2.annotate(f'{s:.2f}x', (c, s), textcoords='offset points',
                 xytext=(0, 12), ha='center', fontsize=10, fontweight='bold')

ax2.set_xlabel('Number of Cores', fontsize=11)
ax2.set_ylabel('Speedup over No-Stealing Baseline', fontsize=11)
ax2.set_title('Speedup from Perfect Load Balancing', fontsize=11)
ax2.set_xticks(common)
ax2.grid(alpha=0.3)
ax2.set_ylim(bottom=0)
ax2.legend(fontsize=9, loc='upper left')

plt.tight_layout(rect=[0, 0, 1, 0.92])
plt.savefig(OUT, dpi=180, bbox_inches='tight')
print(f"\nPlot saved to {OUT}")
