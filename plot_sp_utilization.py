#!/usr/bin/env python3
"""L2SP & L1SP utilization comparison: without vs with L1SP caching.

Shows where BFS data arrays (visited, dist, frontier) are placed
for a 65K RMAT graph on 16 cores under two allocation strategies:
  1. L2SP → DRAM  (baseline, no L1SP caching)
  2. L2SP → L1SP → DRAM  (with L1SP overflow tier)
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# ─── Hardware constants ───
L2SP_TOTAL     = 1_048_576        # 1 MB per pod
L1SP_PER_CORE  = 262_144          # 256 KB per core
HARTS_PER_CORE = 16
HART_STACK     = 16_384           # bytes per-hart stack slot
L1SP_DATA_START = 16              # after thief token (8 B aligned)
L1SP_STACK_GUARD = 5_120          # 5 KB guard zone
STATIC_L2SP    = 29_680           # measured from 32K/4c output (compile-time sized arrays)

NCORES = 16
N = 65_536                        # 65K RMAT vertices

# ─── Array sizes ───
visited_bytes  = N * 8            # int64_t per vertex
dist_bytes     = N * 4            # int32_t per vertex
frontier_bytes = N * 16           # 2 × int64_t (current + next) per vertex

# ─── L2SP allocation (same for both modes) ───
l2sp_avail = L2SP_TOTAL - STATIC_L2SP
# visited: all fits
vis_l2sp = min(N, l2sp_avail // 8) * 8
l2sp_avail -= vis_l2sp
# dist: all fits
dist_l2sp = min(N, l2sp_avail // 4) * 4
l2sp_avail -= dist_l2sp
# frontier: whatever's left
frontier_l2sp_verts = l2sp_avail // 16
frontier_l2sp = frontier_l2sp_verts * 16
frontier_remaining_verts = N - frontier_l2sp_verts

# ─── L1SP data region ───
hart_cap = L1SP_PER_CORE // HARTS_PER_CORE     # 16384
l1sp_data_per_core = hart_cap - L1SP_STACK_GUARD - L1SP_DATA_START  # 11248

def floor_pow2(x):
    if x <= 0: return 0
    p = 1
    while p * 2 <= x:
        p *= 2
    return p

# Frontier in L1SP (visited/dist all fit in L2SP, so all L1SP data space → frontier)
entries_raw = l1sp_data_per_core // 16   # 16 bytes per frontier vertex (cur + next)
epc = floor_pow2(entries_raw)            # entries per core (power of 2)
if epc * NCORES > frontier_remaining_verts:
    epc = floor_pow2(frontier_remaining_verts // NCORES)
frontier_l1sp_verts = epc * NCORES
frontier_l1sp_bytes = frontier_l1sp_verts * 16
frontier_l1sp_per_core = epc * 16        # bytes per core used for frontier data

# ─── Mode 1: Without L1SP caching (L2SP → DRAM) ───
mode1_l2sp_static   = STATIC_L2SP
mode1_l2sp_visited  = vis_l2sp
mode1_l2sp_dist     = dist_l2sp
mode1_l2sp_frontier = frontier_l2sp
mode1_l2sp_free     = L2SP_TOTAL - mode1_l2sp_static - mode1_l2sp_visited - mode1_l2sp_dist - mode1_l2sp_frontier
mode1_dram_frontier = frontier_remaining_verts * 16
mode1_l1sp_thief    = 8
mode1_l1sp_stacks   = HART_STACK * HARTS_PER_CORE  # = L1SP_PER_CORE
mode1_l1sp_data     = 0      # no data caching
mode1_l1sp_guard    = 0      # no guard needed without data

# ─── Mode 2: With L1SP caching (L2SP → L1SP → DRAM) ───
mode2_l2sp_static   = STATIC_L2SP
mode2_l2sp_visited  = vis_l2sp
mode2_l2sp_dist     = dist_l2sp
mode2_l2sp_frontier = frontier_l2sp
mode2_l2sp_free     = L2SP_TOTAL - mode2_l2sp_static - mode2_l2sp_visited - mode2_l2sp_dist - mode2_l2sp_frontier
mode2_dram_frontier = (frontier_remaining_verts - frontier_l1sp_verts) * 16
mode2_l1sp_thief    = 8
mode2_l1sp_data     = frontier_l1sp_per_core   # frontier data per core
mode2_l1sp_guard    = L1SP_STACK_GUARD
mode2_l1sp_stacks   = HART_STACK * HARTS_PER_CORE  # still same stack space

# ─── Plot ───
fig, axes = plt.subplots(1, 3, figsize=(15, 6))

colors = {
    'static':   '#bdbdbd',
    'visited':  '#4e79a7',
    'dist':     '#59a14f',
    'frontier': '#e15759',
    'free':     '#f0f0f0',
    'thief':    '#ff9da7',
    'data':     '#f28e2b',
    'guard':    '#b07aa1',
    'stacks':   '#76b7b2',
    'unused':   '#f0f0f0',
    'dram':     '#edc948',
}

def kb(b):
    return b / 1024

# ── Panel 1: L2SP Utilization ──
ax = axes[0]
labels = ['Baseline\n(no L1SP cache)', 'With\nL1SP cache']
x = np.arange(len(labels))
w = 0.5

for i, (static, vis, dist, frt, free) in enumerate([
    (mode1_l2sp_static, mode1_l2sp_visited, mode1_l2sp_dist, mode1_l2sp_frontier, mode1_l2sp_free),
    (mode2_l2sp_static, mode2_l2sp_visited, mode2_l2sp_dist, mode2_l2sp_frontier, mode2_l2sp_free),
]):
    bottom = 0
    for val, color, lbl in [
        (static, colors['static'],   'Static globals'),
        (vis,    colors['visited'],  'visited[]'),
        (dist,   colors['dist'],     'dist_arr[]'),
        (frt,    colors['frontier'], 'Frontier bufs'),
        (free,   colors['free'],     'Free'),
    ]:
        bar = ax.bar(x[i], kb(val), w, bottom=kb(bottom), color=color,
                      edgecolor='white', linewidth=0.5,
                      label=lbl if i == 0 else None)
        if kb(val) > 20:
            ax.text(x[i], kb(bottom + val/2), f'{kb(val):.0f} KB',
                    ha='center', va='center', fontsize=8, fontweight='bold')
        bottom += val

ax.set_ylabel('KB', fontsize=11)
ax.set_title('L2SP Utilization (1 MB pod)', fontsize=12, fontweight='bold')
ax.set_xticks(x)
ax.set_xticklabels(labels, fontsize=10)
ax.set_ylim(0, kb(L2SP_TOTAL) * 1.05)
ax.axhline(y=kb(L2SP_TOTAL), color='black', linestyle='--', linewidth=1, alpha=0.5)
ax.text(0.95, kb(L2SP_TOTAL)+5, '1024 KB', ha='right', va='bottom', fontsize=8, alpha=0.6)
ax.legend(fontsize=7, loc='upper left', bbox_to_anchor=(0, 0.88))

# ── Panel 2: L1SP Utilization per core ──
ax = axes[1]

# Measured: 8 of 16 harts are active at peak → 50% stack utilization
ACTIVE_HARTS = 8
peak_stack_per_core = ACTIVE_HARTS * HART_STACK  # actual peak stack usage

for i, (thief, data, guard) in enumerate([
    (mode1_l1sp_thief, mode1_l1sp_data, mode1_l1sp_guard),
    (mode2_l1sp_thief, mode2_l1sp_data, mode2_l1sp_guard),
]):
    # L1SP layout: [thief 8B][pad to 16B][data region][guard][stacks...]
    overhead = L1SP_DATA_START + data + guard
    allocated_stack = L1SP_PER_CORE - overhead
    used_stack = min(peak_stack_per_core, allocated_stack)
    wasted_stack = allocated_stack - used_stack  # allocated but unused stack space

    bottom = 0
    for val, color, lbl in [
        (thief, colors['thief'],  'Thief token'),
        (data,  colors['data'],   'Frontier data'),
        (guard, colors['guard'],  'Stack guard'),
        (used_stack,    colors['stacks'],  'Used stacks (8/16 harts)'),
        (wasted_stack,  colors['unused'],  'Unused (idle hart slots)'),
    ]:
        bar = ax.bar(x[i], kb(val), w, bottom=kb(bottom), color=color,
                      edgecolor='white' if color != colors['unused'] else '#999999',
                      linewidth=0.5,
                      label=lbl if i == 0 else None)
        if kb(val) > 8:
            ax.text(x[i], kb(bottom + val/2), f'{kb(val):.0f} KB',
                    ha='center', va='center', fontsize=8, fontweight='bold')
        bottom += val

ax.set_ylabel('KB', fontsize=11)
ax.set_title('L1SP Utilization (per core)', fontsize=12, fontweight='bold')
ax.set_xticks(x)
ax.set_xticklabels(labels, fontsize=10)
ax.set_ylim(0, kb(L1SP_PER_CORE) * 1.05)
ax.axhline(y=kb(L1SP_PER_CORE), color='black', linestyle='--', linewidth=1, alpha=0.5)
ax.text(0.95, kb(L1SP_PER_CORE)+3, '256 KB', ha='right', va='bottom', fontsize=8, alpha=0.6)
ax.legend(fontsize=7, loc='upper left', bbox_to_anchor=(0, 0.88))

# ── Panel 3: DRAM Overflow ──
ax = axes[2]
dram_vals = [kb(mode1_dram_frontier), kb(mode2_dram_frontier)]
bars = ax.bar(x, dram_vals, w, color=colors['dram'], edgecolor='black', linewidth=0.5)
for i, v in enumerate(dram_vals):
    ax.text(x[i], v + 10, f'{v:.0f} KB', ha='center', va='bottom', fontsize=10, fontweight='bold')

reduction_pct = (1 - mode2_dram_frontier / mode1_dram_frontier) * 100
ax.annotate(f'{reduction_pct:.0f}% less\nDRAM overflow',
            xy=(x[1], dram_vals[1]), xytext=(x[1]+0.3, dram_vals[0]*0.8),
            fontsize=9, color='#e15759', fontweight='bold',
            arrowprops=dict(arrowstyle='->', color='#e15759', lw=1.5))

ax.set_ylabel('KB', fontsize=11)
ax.set_title('DRAM Frontier Overflow', fontsize=12, fontweight='bold')
ax.set_xticks(x)
ax.set_xticklabels(labels, fontsize=10)
ax.set_ylim(0, max(dram_vals) * 1.3)

fig.suptitle(f'Panther BFS Memory Utilization: RMAT 65K Graph, {NCORES} Cores\n'
             f'N={N:,}  E=1,818,338  L1SP data region={l1sp_data_per_core:,} B/core  '
             f'L1SP frontier: {frontier_l1sp_verts:,} vertices ({epc}/core × {NCORES} cores)',
             fontsize=11, fontweight='bold', y=1.02)

plt.tight_layout()
out = '/users/alanandr/drv/sp_utilization.png'
plt.savefig(out, dpi=150, bbox_inches='tight')
print(f'Saved: {out}')
print(f'\n--- Summary (N={N}, {NCORES} cores) ---')
print(f'L2SP: {STATIC_L2SP} static + {vis_l2sp} visited + {dist_l2sp} dist + {frontier_l2sp} frontier = {STATIC_L2SP+vis_l2sp+dist_l2sp+frontier_l2sp} / {L2SP_TOTAL}')
print(f'Frontier L2SP: {frontier_l2sp_verts} vertices ({frontier_l2sp} bytes)')
print(f'Frontier remaining: {frontier_remaining_verts} vertices')
print(f'L1SP data/core: {l1sp_data_per_core} bytes usable, {frontier_l1sp_per_core} used for frontier')
print(f'Frontier L1SP: {frontier_l1sp_verts} vertices ({frontier_l1sp_bytes} bytes total)')
print(f'DRAM baseline: {mode1_dram_frontier} bytes ({frontier_remaining_verts} vertices)')
print(f'DRAM with L1SP: {mode2_dram_frontier} bytes ({frontier_remaining_verts - frontier_l1sp_verts} vertices)')
print(f'DRAM reduction: {reduction_pct:.1f}%')
