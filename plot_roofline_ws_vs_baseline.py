#!/usr/bin/env python3
"""Roofline plot for BFS: work-stealing vs imbalanced baseline at different core counts."""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import os

OUT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'bfs_roofline_ws_vs_baseline.png')

# --- Architecture parameters (Panther/DRV) ---
CLOCK_GHZ = 1.0
HARTS_PER_CORE = 16
L2SP_BW_GBS = 64.0       # L2SP bandwidth per pod (shared)
DRAM_BW_GBS = 24.0        # DRAM bandwidth ceiling (~24 GB/s)
CACHE_LINE_BYTES = 64

# --- BFS workload ---
TOTAL_EDGES = 21090        # edges traversed
TOTAL_OPS = TOTAL_EDGES    # 1 op per edge traversal (comparison)

# --- Measured data ---
# (variant, cores, cycles, dram_requests)
runs = [
    ('imbalanced', 1, 2574697, 793),
    ('imbalanced', 2, 2261293, 786),
    ('imbalanced', 4, 2229196, 800),
    ('imbalanced', 8, 2247229, 800),
    ('ws',         1, 3157952, 790),  # estimated ~790
    ('ws',         2, 1921663, 798),
    ('ws',         4, 1460476, 774),
    ('ws',         8, 1299257, 781),
]

# --- Compute OI and GOPs ---
results = {}
for variant, cores, cycles, dram_reqs in runs:
    dram_bytes = dram_reqs * CACHE_LINE_BYTES
    oi = TOTAL_OPS / dram_bytes  # ops/byte
    gops = TOTAL_OPS / (cycles / (CLOCK_GHZ * 1e9))  # ops/sec -> GOPs
    gops /= 1e9  # convert to GOPs
    results.setdefault(variant, []).append({
        'cores': cores, 'cycles': cycles, 'oi': oi, 'gops': gops,
        'dram_bytes': dram_bytes
    })
    print(f"  {variant:12s} {cores}c: OI={oi:.3f} ops/B, GOPs={gops:.6f}, cycles={cycles:,}")

# --- Roofline ceilings ---
oi_range = np.logspace(-3, 2, 500)

core_configs = [2, 4, 8]
colors = {2: '#4e79a7', 4: '#59a14f', 8: '#e15759'}

fig, ax = plt.subplots(1, 1, figsize=(10, 7))

# Draw roofline ceilings for each core count
for c in core_configs:
    peak_gops = CLOCK_GHZ * HARTS_PER_CORE * c  # integer throughput ceiling
    bw_ceiling = DRAM_BW_GBS  # DRAM BW is shared, not per-core
    # Roofline: min(peak, bw * OI)
    roof = np.minimum(peak_gops, bw_ceiling * oi_range)
    ax.loglog(oi_range, roof, '-', color=colors[c], linewidth=1.5, alpha=0.5,
              label=f'{c}-core ceiling (peak={peak_gops:.0f} GOPs)')

# DRAM BW ceiling (shared)
ax.loglog(oi_range, DRAM_BW_GBS * oi_range, '--', color='brown',
          linewidth=1.2, alpha=0.5, label=f'DRAM BW ceiling ({DRAM_BW_GBS} GB/s)')

# --- Plot data points ---
marker_styles = {
    'imbalanced': {'marker': 'o', 'color': '#4e79a7', 'label': 'Imbalanced Baseline'},
    'ws':         {'marker': 's', 'color': '#e15759', 'label': 'Shared-Queue WS'},
}

for variant, runs_list in results.items():
    style = marker_styles[variant]
    for r in runs_list:
        if r['cores'] == 1:
            continue  # skip 1-core
        ax.plot(r['oi'], r['gops'], style['marker'], color=style['color'],
                markersize=12, markeredgecolor='black', markeredgewidth=1.0,
                zorder=10)

# Add labels for key points only (avoid clutter)
# WS 8c (best)
ws8 = [r for r in results['ws'] if r['cores'] == 8][0]
ax.annotate(f"WS 8c\n{ws8['gops']*1000:.1f} MOPs", (ws8['oi'], ws8['gops']),
            textcoords='offset points', xytext=(15, 5),
            fontsize=8, color='#e15759', fontweight='bold',
            arrowprops=dict(arrowstyle='->', color='#e15759', lw=0.8))

# Baseline 8c (best baseline)
bl8 = [r for r in results['imbalanced'] if r['cores'] == 8][0]
ax.annotate(f"Baseline 8c\n{bl8['gops']*1000:.1f} MOPs", (bl8['oi'], bl8['gops']),
            textcoords='offset points', xytext=(-70, -25),
            fontsize=8, color='#4e79a7', fontweight='bold',
            arrowprops=dict(arrowstyle='->', color='#4e79a7', lw=0.8))

# Gap annotation
ax.annotate('', xy=(ws8['oi']+0.01, ws8['gops']), xytext=(bl8['oi']+0.01, bl8['gops']),
            arrowprops=dict(arrowstyle='<->', color='green', lw=1.5))
mid_gops = (ws8['gops'] + bl8['gops']) / 2
ax.text(ws8['oi'] + 0.04, mid_gops, f"{ws8['gops']/bl8['gops']:.1f}x", fontsize=9,
        color='green', fontweight='bold', va='center')

# Add legend entries for data points
for variant, style in marker_styles.items():
    ax.plot([], [], style['marker'], color=style['color'], markersize=8,
            markeredgecolor='black', markeredgewidth=0.5, label=style['label'])

# --- Annotations ---
ax.set_xlabel('Operational Intensity (ops / byte)', fontsize=12)
ax.set_ylabel('Attainable Performance (GOPs)', fontsize=12)
ax.set_title('Roofline Model: BFS Work Stealing vs Imbalanced Baseline\n'
             'RMAT 1024 vertices, all frontier → core 0, 16 harts/core',
             fontsize=12, fontweight='bold')

ax.set_xlim(1e-2, 1e2)
ax.set_ylim(1e-3, 1e3)
ax.legend(fontsize=8, loc='upper left')
ax.grid(True, alpha=0.3, which='both')

# Add region labels
ax.text(0.015, 0.002, 'Memory\nBound', fontsize=14, fontstyle='italic',
        color='brown', alpha=0.5)
ax.text(20, 0.002, 'Compute\nBound', fontsize=14, fontstyle='italic',
        color='red', alpha=0.5)

plt.tight_layout()
plt.savefig(OUT_PATH, dpi=150, bbox_inches='tight')
print(f'Saved: {OUT_PATH}')
