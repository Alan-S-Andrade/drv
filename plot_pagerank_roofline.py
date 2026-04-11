#!/usr/bin/env python3
"""Roofline model comparison: PageRank vs BFS on DRV/PANDOHammer.

Shows that PageRank on a power-law RMAT graph exhibits the same
latency-bound behavior as BFS, far below the roofline ceiling.

Hardware: 1 GHz cores, 16 harts/core, in-order barrel-threaded.
Memory: L1SP (1 cycle), L2SP (10 cycles, 8 banks/pod), DRAM (~24 GB/s effective).
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# ─── Hardware ceilings (same as BFS roofline) ───
clock_ghz = 1.0

bw_l2sp_per_pod  = 64.0   # GB/s (8 banks × 8 GB/s)
bw_dram_effective = 24.0   # GB/s (network cap)

# ─── BFS reference data (from plot_roofline.py) ───
# RMAT 65K graph (N=65536, E=1,818,338)
bfs_total_edges = 1_818_338
bfs_ops_per_edge = 5   # CAS + compare + add + write + branch
bfs_total_ops = bfs_total_edges * bfs_ops_per_edge

bfs_dram_bytes_per_edge = 4    # col_idx read
bfs_l2sp_bytes_per_edge = 16   # visited CAS (8B read + 8B write)
bfs_total_bytes = bfs_total_edges * (bfs_dram_bytes_per_edge + bfs_l2sp_bytes_per_edge)
bfs_oi = bfs_total_ops / bfs_total_bytes  # ~0.25 ops/byte

# BFS measured (from existing runs on 65K RMAT)
bfs_measured = {
    '2c':  218_278_516,
    '16c': 157_838_265,
}

# ─── PageRank measured data ───
# RMAT 1K graph (N=1024, E=21090), 10 iterations, pull-based
pr_N = 1024
pr_E = 21090
pr_iters = 10
pr_avg_deg = pr_E / pr_N  # ~20.6

# PageRank operational intensity
# Per vertex per iteration:
#   row_ptr[v], row_ptr[v+1]: 2 × int32 reads = 8 bytes DRAM
#   rank_new[v] write: 1 × int64 = 8 bytes DRAM
#   Per neighbor (avg 20.6):
#     col_idx[ei]:    int32 read = 4 bytes DRAM
#     rank_old[u]:    int64 read = 8 bytes DRAM
#     degree[u]:      int32 read = 4 bytes DRAM
#   Total bytes/vertex = 16 + 20.6 × 16 = 345.6 bytes
#
#   Compute: base_rank calc ~5 ops + per neighbor: div + add + load = ~3 ops
#   Total ops/vertex = 5 + 20.6 × 3 = 66.8 ops
#
#   OI = 66.8 / 345.6 ≈ 0.19 ops/byte

pr_ops_per_vertex = 5 + pr_avg_deg * 3
pr_bytes_per_vertex = 16 + pr_avg_deg * 16  # 8 (row_ptr pair) + 8 (rank_new) + deg * (4+8+4)
pr_total_ops = pr_N * pr_ops_per_vertex * pr_iters
pr_total_bytes = pr_N * pr_bytes_per_vertex * pr_iters
pr_oi = pr_ops_per_vertex / pr_bytes_per_vertex  # per-vertex OI

# PageRank measured cycle counts
pr_measured = {
    '1c_4h':  14_580_671,  # 1 core × 4 harts
    '2c_16h':  8_097_480,  # 2 cores × 16 harts
}

print("=" * 70)
print("PAGERANK vs BFS ROOFLINE COMPARISON")
print("=" * 70)
print(f"\nPageRank: RMAT N={pr_N}, E={pr_E}, {pr_iters} iters")
print(f"  OI = {pr_oi:.4f} ops/byte")
print(f"  Total ops = {pr_total_ops:,.0f}")
print(f"  Total bytes = {pr_total_bytes:,.0f}")
print(f"\nBFS: RMAT N=65536, E=1,818,338")
print(f"  OI = {bfs_oi:.4f} ops/byte")
print(f"  Total ops = {bfs_total_ops:,}")
print(f"  Total bytes = {bfs_total_bytes:,}")

print("\n--- Measured Performance ---")
measured_gops = []
for label, cycles in pr_measured.items():
    time_s = cycles / (clock_ghz * 1e9)
    achieved_gops = (pr_total_ops / 1e9) / time_s
    measured_gops.append(achieved_gops)
    pr_edge_traversals = pr_E * pr_iters
    mteps = pr_edge_traversals / (time_s * 1e6)
    print(f"  PR {label}: {cycles:>12,} cyc = {time_s*1e3:.3f} ms, "
          f"{achieved_gops:.6f} GOPs, {mteps:.1f} MTEPS")

for label, cycles in bfs_measured.items():
    time_s = cycles / (clock_ghz * 1e9)
    achieved_gops = (bfs_total_ops / 1e9) / time_s
    measured_gops.append(achieved_gops)
    mteps = bfs_total_edges / (time_s * 1e6)
    print(f"  BFS {label}: {cycles:>12,} cyc = {time_s*1e3:.3f} ms, "
          f"{achieved_gops:.6f} GOPs, {mteps:.1f} MTEPS")

# ─── Generate roofline plot ───
fig, ax = plt.subplots(figsize=(12, 8))

oi_range = np.logspace(-3, 2, 500)

# Draw rooflines for different core configs
roof_configs = [
    ('2-core',   2,  '#4e79a7', '-'),
    ('16-core', 16,  '#59a14f', '-'),
    ('64-core', 64,  '#e15759', '-'),
]

for label, ncores, color, ls in roof_configs:
    peak = ncores * clock_ghz  # GOPs
    bw_eff = bw_l2sp_per_pod   # shared per-pod
    roof = np.minimum(peak, bw_eff * oi_range)
    ax.loglog(oi_range, roof, ls, color=color, linewidth=2,
              label=f'{label} (L2SP {bw_eff:.0f} GB/s)', alpha=0.9)
    ridge = peak / bw_eff
    ax.plot(ridge, peak, '*', color=color, markersize=12, zorder=4, alpha=0.6)

# DRAM ceiling
ax.loglog(oi_range, np.minimum(1000, bw_dram_effective * oi_range),
          ':', color='gray', linewidth=2,
          label=f'DRAM BW ceiling (~{bw_dram_effective:.0f} GB/s)')

# ── Plot BFS points ──
for label, cycles in bfs_measured.items():
    time_s = cycles / (clock_ghz * 1e9)
    achieved_gops = (bfs_total_ops / 1e9) / time_s
    ax.scatter([bfs_oi], [achieved_gops], s=200, marker='o',
               color='#4e79a7', edgecolors='black', linewidth=1.2, zorder=5)
    ax.annotate(f"BFS {label}", (bfs_oi, achieved_gops),
                textcoords='offset points', xytext=(12, 5), fontsize=10,
                color='#4e79a7', fontweight='bold')

# ── Plot PageRank points ──
pr_color = '#d62728'
pr_marker = 'D'
pr_label_map = {'1c_4h': 'PR 1c', '2c_16h': 'PR 2c', '16c_16h': 'PR 16c'}

for label, cycles in pr_measured.items():
    time_s = cycles / (clock_ghz * 1e9)
    achieved_gops = (pr_total_ops / 1e9) / time_s
    display = pr_label_map.get(label, label)
    ax.scatter([pr_oi], [achieved_gops], s=200, marker=pr_marker,
               color=pr_color, edgecolors='black', linewidth=1.2, zorder=5)
    ax.annotate(display, (pr_oi, achieved_gops),
                textcoords='offset points', xytext=(12, -10), fontsize=10,
                color=pr_color, fontweight='bold')

# OI reference lines
ax.axvline(x=bfs_oi, color='#4e79a7', linestyle=':', linewidth=1.5, alpha=0.5,
           label=f'BFS OI = {bfs_oi:.2f} ops/B')
ax.axvline(x=pr_oi, color='#d62728', linestyle=':', linewidth=1.5, alpha=0.5,
           label=f'PageRank OI = {pr_oi:.2f} ops/B')

ax.set_xlabel('Operational Intensity (ops / byte)', fontsize=12)
ax.set_ylabel('Attainable Performance (GOPs)', fontsize=12)
ax.set_title('Roofline Model: PageRank vs BFS on Panther\n'
             'Both on RMAT power-law graphs, L2SP 1MB',
             fontsize=13, fontweight='bold')
ax.set_xlim(1e-3, 100)
ax.set_ylim(1e-4, 100)
ax.legend(fontsize=8, loc='upper left', ncol=1)
ax.grid(True, which='both', alpha=0.2)

memory_text = ax.text(0.005, 0.0005, 'Memory\nBound', fontsize=14, color='gray', alpha=0.4,
              fontweight='bold', ha='center')
compute_text = ax.text(30, 0.05, 'Compute\nBound', fontsize=14, color='gray', alpha=0.4,
               fontweight='bold', ha='center')

plt.tight_layout()
out = '/users/alanandr/drv/pagerank_roofline.png'
plt.savefig(out, dpi=150, bbox_inches='tight')
print(f'\nSaved: {out}')

measured_oi = [bfs_oi, pr_oi]
zoom_xlim = (min(measured_oi) * 0.85, max(measured_oi) * 1.35)
zoom_ylim = (min(measured_gops) * 0.75, max(measured_gops) * 1.35)
ax.set_xlim(*zoom_xlim)
ax.set_ylim(*zoom_ylim)
memory_text.set_visible(False)
compute_text.set_visible(False)

zoom_out = '/users/alanandr/drv/pagerank_roofline_zoom.png'
plt.savefig(zoom_out, dpi=200, bbox_inches='tight')
print(f'Saved: {zoom_out}')
