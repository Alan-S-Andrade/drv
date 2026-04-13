#!/usr/bin/env python3
"""Roofline comparison: Tiered WQ + FAA at 16 harts/core vs 32 harts/core.

Operational intensity includes ALL instructions: compute harts + fetcher hart.
Fetcher hart ops: ~2 ops/fetched-item (1 L2SP read + 1 L1SP write) plus
amortized batch overhead (~1/128 FAA per item).

Usage:
  python3 plot_roofline_tiered_faa_comparison.py
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# ─── Hardware parameters ───
clock_ghz = 1.0

# Bandwidth ceilings (GB/s)
bw_l1sp_per_core = 8.0   # 1 req/cycle × 8 bytes
bw_l2sp_per_pod  = 64.0  # 8 banks × 8 GB/s
bw_dram_eff      = 24.0  # per-core network cap

# ─── BFS workload characteristics (65K RMAT, CUSP=16) ───
total_edges = 1_818_338
total_nodes_discovered = 65_536  # approximate vertices reached

# --- Per-edge ops (compute harts) ---
# 1) load g_col_idx[ei]        (DRAM read, 4B)
# 2) amoswap visited[v]        (L2SP atomic, 8B)
# Per claimed vertex (assume ~50% claim rate for RMAT):
# 3) store dist_arr[v]         (L2SP write, 8B)
# 4) FAA next_frontier.tail    (L2SP atomic, 8B)
# 5) store frontier item+owner (L2SP write, 16B)
# 6) FAA discovered counter    (L2SP atomic, 8B)
claim_rate = total_nodes_discovered / total_edges  # ~3.6%
compute_ops_per_edge = 2 + claim_rate * 4  # ~2.14 ops/edge

# --- Fetcher hart ops per discovered node ---
# Each discovered node is fetched from L2SP → L1SP:
# 1) L2SP load (read node from frontier)   8B
# 2) L1SP store (write to ring buffer)     8B
# Amortized per item: ~1/128 FAA + 1/128 head-read + 1/128 fence+tail-write ≈ 0.02
fetcher_ops_per_node = 2.02

# --- Total ops including fetcher ---
total_compute_ops = total_edges * compute_ops_per_edge
total_fetcher_ops = total_nodes_discovered * fetcher_ops_per_node
total_ops = total_compute_ops + total_fetcher_ops

# --- Total bytes transferred (all tiers) ---
# DRAM bytes: col_idx reads
dram_bytes_per_edge = 4
total_dram_bytes = total_edges * dram_bytes_per_edge

# L2SP bytes: visited atomics + dist writes + frontier writes + fetcher reads
l2sp_bytes_per_edge = 8  # amoswap visited (every edge)
l2sp_bytes_per_claimed = 8 + 8 + 16 + 8  # dist + FAA tail + frontier item+owner + FAA discovered
l2sp_fetcher_bytes_per_node = 8  # frontier read by fetcher
total_l2sp_bytes = (total_edges * l2sp_bytes_per_edge
                    + total_nodes_discovered * (l2sp_bytes_per_claimed + l2sp_fetcher_bytes_per_node))

# L1SP bytes: fetcher writes + compute hart reads from L1SP queue
l1sp_bytes_per_node = 8 + 8  # fetcher write + compute hart pop read
total_l1sp_bytes = total_nodes_discovered * l1sp_bytes_per_node

total_bytes = total_dram_bytes + total_l2sp_bytes + total_l1sp_bytes

# Operational intensities
oi_dram  = total_ops / total_dram_bytes
oi_total = total_ops / total_bytes

# Effective BW for tiered variants (L1SP captures queue traffic)
l1sp_capture_rate = 0.70

# ─── Measured baseline data (65K RMAT, scale=16, ef=16, seed=42, cusp=16) ───
# These are from SST simulation of the tiered FAA variant at 16 harts/core
measured_faa_16h = {
    '1c':  {'cycles': 318_978_889, 'cores': 1},
    '2c':  {'cycles': 222_644_086, 'cores': 2},
    '4c':  {'cycles': 172_829_854, 'cores': 4},
    '8c':  {'cycles': 171_584_041, 'cores': 8},
    '16c': {'cycles': 123_113_846, 'cores': 16},
}


def estimate_faa_32h_cycles(faa_16h_cycles, ncores):
    """Estimate tiered FAA cycles at 32 harts/core from 16-hart data.

    32 harts/core advantages:
    - 31 compute harts vs 15 → ~2x thread-level parallelism per core
    - Better latency hiding: more harts to cover L2SP/DRAM stalls
    - Fetcher hart cost is 1/32 vs 1/16 → less relative overhead
    32 harts/core disadvantages:
    - More L2SP contention from 2x the outstanding requests
    - L1SP stack space per hart halved (8KB vs 16KB at 256KB L1SP)
    - Diminishing returns if already memory-bound
    """
    if ncores <= 1:
        # Single core: pure latency hiding benefit, ~1.6x speedup
        # (more harts mask DRAM latency, but L2SP contention grows)
        factor = 0.62
    elif ncores <= 2:
        # 2 cores: good latency hiding, moderate contention
        factor = 0.60
    elif ncores <= 4:
        # 4 cores: diminishing returns, L2SP banks start to saturate
        factor = 0.58
    elif ncores <= 8:
        # 8 cores: memory-bound regime, contention offsets some gains
        factor = 0.62
    else:
        # 16 cores: heavily memory-bound, 32h helps less
        # More outstanding requests → more L2SP/DRAM queueing
        factor = 0.70
    return int(faa_16h_cycles * factor)


def main():
    measured_faa_32h = {}
    for key, val in measured_faa_16h.items():
        measured_faa_32h[key] = {
            'cycles': estimate_faa_32h_cycles(val['cycles'], val['cores']),
            'cores': val['cores'],
        }

    # ─── Build roofline ───
    fig, ax = plt.subplots(1, 1, figsize=(10, 8))
    oi_range = np.logspace(-3, 2, 500)

    # Draw rooflines for hardware configurations
    for hpc, color, ls, lbl_extra in [(16, '#4e79a7', '-', ''), (32, '#e15759', '--', '')]:
        for ncores in [2, 16]:
            peak = ncores * clock_ghz  # 1 op/cycle/hart in barrel threading
            roof = np.minimum(peak, bw_l2sp_per_pod * oi_range)
            ax.loglog(oi_range, roof, ls, color=color, linewidth=1.5,
                      label=f'{ncores}c/{hpc}h (peak {peak:.0f} GOP/s)', alpha=0.7)

    # Tiered effective roofline lines (blended BW)
    for hpc, color, ls in [(16, '#4e79a7', '-'), (32, '#e15759', '--')]:
        tiered_l1sp_bw = 16 * bw_l1sp_per_core  # 128 GB/s at 16 cores
        eff_bw = l1sp_capture_rate * tiered_l1sp_bw + (1 - l1sp_capture_rate) * bw_l2sp_per_pod
        compute_frac = (hpc - 1) / hpc
        peak_16c = 16 * clock_ghz * compute_frac
        roof_t = np.minimum(peak_16c, eff_bw * oi_range)
        ax.loglog(oi_range, roof_t, ls, color=color, linewidth=2.5, alpha=0.4,
                  label=f'16c/{hpc}h tiered eff BW={eff_bw:.0f} GB/s')

    # DRAM ceiling
    ax.loglog(oi_range, np.minimum(1000, bw_dram_eff * oi_range),
              ':', color='gray', linewidth=2, label=f'DRAM BW (~{bw_dram_eff:.0f} GB/s)')

    # Plot data points
    datasets = [
        (measured_faa_16h,  'o', '#4e79a7', 'Tiered FAA (16 harts/core)'),
        (measured_faa_32h,  'D', '#e15759', 'Tiered FAA (32 harts/core)'),
    ]

    all_gops = []
    for data, marker, color, lbl in datasets:
        first = True
        for key in ['1c', '2c', '4c', '8c', '16c']:
            if key not in data:
                continue
            v = data[key]
            achieved_gops = total_ops / v['cycles']
            all_gops.append(achieved_gops)
            label = lbl if first else None
            first = False
            ax.scatter([oi_total], [achieved_gops], s=120, marker=marker, color=color,
                       edgecolors='black', linewidth=0.8, zorder=5, label=label)
            ax.annotate(f"{v['cores']}c",
                        (oi_total, achieved_gops),
                        textcoords='offset points', xytext=(12, 0), fontsize=8,
                        color=color, fontweight='bold')

    # OI reference lines
    ax.axvline(x=oi_total, color='purple', linestyle=':', linewidth=1.5, alpha=0.7,
               label=f'BFS OI (all tiers) = {oi_total:.2f}')
    ax.axvline(x=oi_dram, color='orange', linestyle=':', linewidth=1.5, alpha=0.5,
               label=f'BFS OI (DRAM only) = {oi_dram:.2f}')

    ax.set_xlabel('Operational Intensity (ops / byte)', fontsize=12)
    ax.set_ylabel('Attainable Performance (GOPs)', fontsize=12)
    ax.set_title('Roofline: Tiered FAA — 16 vs 32 harts/core (estimated)\n'
                 f'OI includes fetcher ops · RMAT 65K · claim rate {claim_rate:.1%}',
                 fontsize=12, fontweight='bold')
    ax.set_xlim(1e-3, 100)
    ax.set_ylim(1e-3, 100)
    ax.legend(fontsize=7, loc='upper left', ncol=1)
    ax.grid(True, which='both', alpha=0.25)
    ax.text(0.95, 0.05, 'Memory\nBound', transform=ax.transAxes, fontsize=10,
            ha='right', va='bottom', color='gray', alpha=0.5, style='italic')
    ax.text(0.05, 0.95, 'Compute\nBound', transform=ax.transAxes, fontsize=10,
            ha='left', va='top', color='gray', alpha=0.5, style='italic')

    plt.tight_layout()
    out = '/users/alanandr/drv/bfs_roofline_tiered_faa_comparison.png'
    plt.savefig(out, dpi=150, bbox_inches='tight')
    print(f'Saved: {out}')

    # Print OI breakdown
    print(f'\n=== Operational Intensity Breakdown ===')
    print(f'Compute ops:    {total_compute_ops:,.0f}  ({compute_ops_per_edge:.2f} ops/edge × {total_edges:,} edges)')
    print(f'Fetcher ops:    {total_fetcher_ops:,.0f}  ({fetcher_ops_per_node:.2f} ops/node × {total_nodes_discovered:,} nodes)')
    print(f'Total ops:      {total_ops:,.0f}')
    print(f'DRAM bytes:     {total_dram_bytes:,.0f}')
    print(f'L2SP bytes:     {total_l2sp_bytes:,.0f}')
    print(f'L1SP bytes:     {total_l1sp_bytes:,.0f}')
    print(f'Total bytes:    {total_bytes:,.0f}')
    print(f'OI (DRAM only): {oi_dram:.3f}')
    print(f'OI (all tiers): {oi_total:.3f}')

    # Print performance summary
    print(f'\n=== Performance Summary (estimated, 65K RMAT CUSP=16) ===')
    print(f'{"Variant":<35} {"Cores":>5} {"Cycles":>12} {"GOPs":>8} {"Speedup":>8}')
    print('-' * 70)

    for key in ['1c', '2c', '4c', '8c', '16c']:
        if key not in measured_faa_16h:
            continue
        ncores = measured_faa_16h[key]['cores']
        for data, lbl in [
            (measured_faa_16h,  'Tiered FAA (16 harts/core)'),
            (measured_faa_32h,  'Tiered FAA (32 harts/core)'),
        ]:
            if key in data:
                cyc = data[key]['cycles']
                gops = total_ops / cyc
                ref_cyc = measured_faa_16h[key]['cycles']
                speedup = ref_cyc / cyc
                print(f'{lbl:<35} {ncores:>5} {cyc:>12,} {gops:>8.4f} {speedup:>7.2f}x')


if __name__ == '__main__':
    main()
