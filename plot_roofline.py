#!/usr/bin/env python3
"""Roofline model for DRV/PANDOHammer BFS workload.

Hardware: 1 GHz cores, 16 harts/core, in-order barrel-threaded.
Memory: L1SP (1 cycle), L2SP (10 cycles, 8 banks/pod), HBM (1024 GB/s peak).
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# ─── Hardware ceilings ───
clock_ghz = 1.0

# Peak compute (integer ops/sec) — 1 op/cycle/hart
configs = {
    '2c':  {'cores': 2,  'harts': 32,  'peak_gops': 2},
    '4c':  {'cores': 4,  'harts': 64,  'peak_gops': 4},
    '16c': {'cores': 16, 'harts': 256, 'peak_gops': 16},
    '64c': {'cores': 64, 'harts': 1024,'peak_gops': 64},
}

# Bandwidth ceilings (GB/s)
# L1SP: 8 GB/s per core (1 req/cycle × 8 bytes)
# L2SP: 8 GB/s per bank, 8 banks default → 64 GB/s per pod
# DRAM: network-limited to 24 GB/s per core (not 1024 GB/s HBM peak)
# The actual bottleneck is per-core network BW of 24 GB/s

bw_l1sp_per_core = 8.0   # GB/s
bw_l2sp_per_pod  = 64.0  # GB/s (8 banks × 8 GB/s)
bw_dram_per_core = 24.0  # GB/s (network cap)

# ─── BFS operational intensity ───
# Per frontier vertex:
#   Compute: ~5 int ops (CAS visited, write dist, atomic push, loop control, compare)
#   Memory:  read row_ptr[u] + row_ptr[u+1] = 8 bytes (DRAM)
#            per neighbor: read col_idx[ei] = 4 bytes (DRAM)
#                          read+CAS visited[v] = 16 bytes (L2SP)
#                          write dist[v] = 4 bytes (L2SP, conditional)
#                          write frontier = 8 bytes (L2SP, conditional)
# Average degree ~27 edges/vertex for the 65K RMAT
# Most edge reads are DRAM, visited/dist are L2SP

# Operational intensity estimates by memory level
# DRAM OI: ops / DRAM bytes
#   ops per vertex ≈ 5 + 27*(3 int ops per neighbor) ≈ 86 ops
#   DRAM bytes per vertex ≈ 8 (row_ptr pair) + 27*4 (col_idx) ≈ 116 bytes
#   OI_dram ≈ 86/116 ≈ 0.74 ops/byte

# L2SP OI: ops / L2SP bytes
#   L2SP bytes per vertex ≈ 27*16 (visited CAS) + avg_claim*4 (dist) + avg_claim*8 (frontier)
#   ~27 neighbors, ~0.5 claim rate → 27*16 + 13*12 ≈ 588 bytes
#   OI_l2sp ≈ 86/588 ≈ 0.15 ops/byte

# Total OI (all bytes): 86 / (116 + 588) ≈ 0.12 ops/byte

# Measured from actual runs:
# 65K graph, 2-core baseline: 218M cycles, 46551 nodes discovered, 1.8M edges
# Each edge traversal: ~1 col_idx read (4B DRAM) + 1 visited CAS (16B L2SP)
#   Total bytes moved ≈ 1.8M * 4 (DRAM) + 1.8M * 16 (L2SP) ≈ 7.2 + 28.8 = 36 MB
#   Useful ops ≈ 1.8M edges * ~5 ops/edge = 9M ops
#   OI ≈ 9M / 36M ≈ 0.25 ops/byte
#   Achieved throughput = 9M ops / 0.218s = 0.041 GOPs

# Per config measured data (65K RMAT, baseline)
measured = {
    '1c':  {'cycles': 312_724_401, 'cores': 1},
    '2c':  {'cycles': 218_278_516, 'cores': 2},
    '4c':  {'cycles': 196_397_562, 'cores': 4},
    '8c':  {'cycles': 194_981_865, 'cores': 8},
    '16c': {'cycles': 157_838_265, 'cores': 16},
    '32c': {'cycles': 199_416_680, 'cores': 32},
    '64c': {'cycles': 319_928_113, 'cores': 64},
}
# L1SP model
measured_l1sp = {
    '2c':  {'cycles': 234_198_321, 'cores': 2},
    '4c':  {'cycles': 206_325_147, 'cores': 4},
    '8c':  {'cycles': 200_486_714, 'cores': 8},
    '16c': {'cycles': 189_512_430, 'cores': 16},
    '32c': {'cycles': 208_618_214, 'cores': 32},
    '64c': {'cycles': 245_627_260, 'cores': 64},
}

total_edges = 1_818_338
ops_per_edge = 5  # CAS + compare + add + write + branch
total_ops = total_edges * ops_per_edge
dram_bytes_per_edge = 4  # col_idx read
l2sp_bytes_per_edge = 16  # visited CAS (8B read + 8B write)
total_dram_bytes = total_edges * dram_bytes_per_edge
total_l2sp_bytes = total_edges * l2sp_bytes_per_edge
total_bytes = total_dram_bytes + total_l2sp_bytes

oi_dram = total_ops / total_dram_bytes    # ops per DRAM byte
oi_l2sp = total_ops / total_l2sp_bytes    # ops per L2SP byte
oi_total = total_ops / total_bytes        # ops per total byte

# ─── Build roofline ───
fig, ax = plt.subplots(figsize=(12, 8))

oi_range = np.logspace(-3, 2, 500)

# Draw rooflines for key configs
roof_configs = [
    ('2-core',   2,  bw_l2sp_per_pod, '#4e79a7', '-'),
    ('16-core', 16,  bw_l2sp_per_pod, '#59a14f', '-'),
    ('64-core', 64,  bw_l2sp_per_pod, '#e15759', '-'),
]

for label, ncores, bw, color, ls in roof_configs:
    peak = ncores * clock_ghz  # GOPs
    # L2SP-limited roofline (shared pod BW)
    bw_eff = bw  # L2SP BW doesn't scale with cores (shared)
    roof = np.minimum(peak, bw_eff * oi_range)
    ax.loglog(oi_range, roof, ls, color=color, linewidth=2, label=f'{label} (L2SP {bw_eff:.0f} GB/s)', alpha=0.9)

# DRAM-limited roofline (per-core network BW × cores, but shared channel)
# Effective DRAM BW is min(ncores * 24, 1024) but realistically ~24 GB/s total
# because of single-channel queuing
for ncores, color, marker in [(2, '#4e79a7', 'o'), (16, '#59a14f', 's'), (64, '#e15759', '^')]:
    peak = ncores * clock_ghz
    # Actual DRAM bandwidth is bottlenecked by single channel + queuing
    # Measured: ~24 GB/s effective (network cap), degrades at high core count
    dram_bw_eff = min(ncores * 24.0, 24.0)  # saturated at ~24 GB/s
    roof_dram = np.minimum(peak, dram_bw_eff * oi_range)
    ax.loglog(oi_range, roof_dram, '--', color=color, linewidth=1.5, alpha=0.5)

# Label DRAM ceiling
ax.loglog(oi_range, np.minimum(1000, 24.0 * oi_range), ':', color='gray', linewidth=2, label='DRAM BW ceiling (~24 GB/s)')

# Plot two measured BFS points at 16 cores: baseline vs work-stealing
for data, marker, color, lbl in [
    (measured,      'o', '#4e79a7', 'Baseline (no steal) — 16c'),
    (measured_l1sp, 's', '#e15759', 'Work Stealing — 16c'),
]:
    v = data['16c']
    achieved_gops = total_ops / v['cycles']
    ax.scatter([oi_dram], [achieved_gops], s=120, marker=marker, color=color,
               edgecolors='black', linewidth=0.8, zorder=5, label=lbl)
    ax.annotate(f"{v['cores']}c", (oi_dram, achieved_gops),
                textcoords='offset points', xytext=(8, 5), fontsize=9,
                color=color, fontweight='bold')

# Mark the ridge points
for ncores, color in [(2, '#4e79a7'), (16, '#59a14f'), (64, '#e15759')]:
    peak = ncores * clock_ghz
    ridge = peak / bw_l2sp_per_pod  # ops/byte where compute meets BW
    ax.plot(ridge, peak, '*', color=color, markersize=12, zorder=4, alpha=0.6)

# Annotations
ax.axvline(x=oi_dram, color='orange', linestyle=':', linewidth=1.5, alpha=0.7, label=f'BFS OI (DRAM) = {oi_dram:.2f} ops/B')
ax.axvline(x=oi_total, color='purple', linestyle=':', linewidth=1.5, alpha=0.5, label=f'BFS OI (total) = {oi_total:.2f} ops/B')

ax.set_xlabel('Operational Intensity (ops / byte)', fontsize=12)
ax.set_ylabel('Attainable Performance (GOPs)', fontsize=12)
ax.set_title('Roofline Model on Panther: BFS\n'
             'RMAT 65K graph (N=65536, E=1.8M), L2SP 1MB',
             fontsize=13, fontweight='bold')
ax.set_xlim(1e-3, 100)
ax.set_ylim(1e-3, 100)
ax.legend(fontsize=8, loc='upper left', ncol=1)
ax.grid(True, which='both', alpha=0.2)

# Add zone labels
ax.text(0.005, 0.003, 'Memory\nBound', fontsize=14, color='gray', alpha=0.4,
        fontweight='bold', ha='center')
ax.text(30, 0.5, 'Compute\nBound', fontsize=14, color='gray', alpha=0.4,
        fontweight='bold', ha='center')

plt.tight_layout()
out = '/users/alanandr/drv/bfs_roofline.png'
plt.savefig(out, dpi=150, bbox_inches='tight')
print(f'Saved: {out}')
