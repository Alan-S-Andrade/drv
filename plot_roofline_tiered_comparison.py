#!/usr/bin/env python3
"""Roofline comparison: Baseline vs Tiered Work Queues.

Plots both BFS variants on the PANDOHammer roofline model.
The tiered WQ variant dedicates 1 of 16 harts per core as a fetcher,
so it has 15/16 = 93.75% of compute capacity but reduces L2SP contention
by staging work through the L1SP queue (~1 cycle vs ~10 cycle access).

Usage:
  # With measured data from sweep:
  python3 plot_roofline_tiered_comparison.py --csv tiered_wq_results/summary.csv

  # With estimated data (no simulation):
  python3 plot_roofline_tiered_comparison.py
"""

import argparse
import csv
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# ─── Hardware parameters ───
clock_ghz = 1.0
harts_per_core = 16

# Bandwidth ceilings (GB/s)
bw_l1sp_per_core = 8.0   # 1 req/cycle × 8 bytes
bw_l2sp_per_pod  = 64.0  # 8 banks × 8 GB/s
bw_dram_eff      = 24.0  # network cap

# ─── BFS workload characteristics (65K RMAT) ───
total_edges = 1_818_338
ops_per_edge = 5
total_ops = total_edges * ops_per_edge
dram_bytes_per_edge = 4   # col_idx read
l2sp_bytes_per_edge = 16  # visited CAS
total_dram_bytes = total_edges * dram_bytes_per_edge
total_l2sp_bytes = total_edges * l2sp_bytes_per_edge
total_bytes = total_dram_bytes + total_l2sp_bytes

oi_dram  = total_ops / total_dram_bytes
oi_l2sp  = total_ops / total_l2sp_bytes
oi_total = total_ops / total_bytes

# For the tiered variant, effective OI shifts because many L2SP accesses
# become L1SP accesses (lower latency, higher BW).
# Assume ~70% of frontier/queue L2SP accesses are served from L1SP instead.
l1sp_capture_rate = 0.70
tiered_l2sp_bytes = total_l2sp_bytes * (1.0 - l1sp_capture_rate)
tiered_l1sp_bytes = total_l2sp_bytes * l1sp_capture_rate
tiered_total_bytes = total_dram_bytes + tiered_l2sp_bytes + tiered_l1sp_bytes
oi_tiered_total = total_ops / tiered_total_bytes
# Effective BW is weighted average of L1SP and L2SP bandwidths
# For a 16-core pod: L1SP contributes 16*8=128 GB/s, L2SP 64 GB/s
# Weighted by byte fraction

# ─── Measured data (65K RMAT, scale=16, ef=16, seed=42, cusp=16) ───
# Baseline work stealing: pure L2SP work queues
measured_baseline = {
    '1c':  {'cycles': 312_724_401, 'cores': 1},
    '2c':  {'cycles': 218_278_516, 'cores': 2},
    '4c':  {'cycles': 196_397_562, 'cores': 4},
    '8c':  {'cycles': 194_981_865, 'cores': 8},
    '16c': {'cycles': 157_838_265, 'cores': 16},
}

# Tiered work queues (estimated — replaced by CSV data when available)
measured_tiered = {}

def load_csv_results(csv_path):
    """Load results from the sweep summary CSV.

    CSV format: variant,cores,cycles,nodes_discovered,edges_traversed
    """
    baseline = {}
    tiered = {}
    with open(csv_path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            cores = int(row['cores'])
            cycles = int(row['cycles'])
            key = f'{cores}c'
            entry = {'cycles': cycles, 'cores': cores}
            if 'edges_traversed' in row and row['edges_traversed'] != '0':
                entry['edges'] = int(row['edges_traversed'])
            variant = row['variant']
            if 'tiered' in variant:
                tiered[key] = entry
            else:
                baseline[key] = entry
    return baseline, tiered


def estimate_tiered_cycles(baseline_cycles, ncores):
    """Estimate tiered WQ cycles from baseline.

    The tiered approach trades 1/16 of compute capacity for:
    - Reduced L2SP contention (fetcher batches requests)
    - L1SP-speed work access for compute harts (~1 cycle vs ~10 cycles)
    - Reduced CAS contention on L2SP queue heads
    """
    if ncores <= 2:
        overhead = 1.05  # 5% slower (fetcher hart waste)
    elif ncores <= 8:
        overhead = 0.92  # 8% faster (reduced contention)
    else:
        overhead = 0.85  # 15% faster (L2SP contention relief dominates)
    return int(baseline_cycles * overhead)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--csv', type=str, default=None,
                        help='Path to sweep summary.csv with measured results')
    args = parser.parse_args()

    global measured_baseline, measured_tiered

    if args.csv and os.path.exists(args.csv):
        print(f'Loading measured data from {args.csv}')
        measured_baseline, measured_tiered = load_csv_results(args.csv)
        data_source = 'measured'
    else:
        if args.csv:
            print(f'Warning: {args.csv} not found, using estimates')
        data_source = 'estimated'
        # Build tiered estimates from baseline
        for key, val in measured_baseline.items():
            measured_tiered[key] = {
                'cycles': estimate_tiered_cycles(val['cycles'], val['cores']),
                'cores': val['cores'],
            }

    # ─── Build roofline ───
    fig, axes = plt.subplots(1, 2, figsize=(18, 8))

    # === Left panel: Roofline model ===
    ax = axes[0]
    oi_range = np.logspace(-3, 2, 500)

    # Draw rooflines for key configs
    roof_configs = [
        ('2-core',   2,  '#4e79a7', '-'),
        ('16-core', 16,  '#59a14f', '-'),
        ('64-core', 64,  '#e15759', '-'),
    ]

    for label, ncores, color, ls in roof_configs:
        peak = ncores * clock_ghz
        roof = np.minimum(peak, bw_l2sp_per_pod * oi_range)
        ax.loglog(oi_range, roof, ls, color=color, linewidth=2,
                  label=f'{label} (L2SP {bw_l2sp_per_pod:.0f} GB/s)', alpha=0.9)

    # DRAM ceiling
    ax.loglog(oi_range, np.minimum(1000, bw_dram_eff * oi_range),
              ':', color='gray', linewidth=2, label=f'DRAM BW (~{bw_dram_eff:.0f} GB/s)')

    # Tiered effective roofline: blend of L1SP and L2SP BW
    # With tiered WQ, effective pod BW increases because L1SP serves most queue accesses
    ncores_16 = 16
    tiered_l1sp_bw = ncores_16 * bw_l1sp_per_core  # 128 GB/s
    # Weighted effective BW: 70% from L1SP + 30% from L2SP
    tiered_eff_bw = l1sp_capture_rate * tiered_l1sp_bw + (1 - l1sp_capture_rate) * bw_l2sp_per_pod
    peak_16 = ncores_16 * clock_ghz
    # Effective compute: 15/16 harts are computing
    tiered_peak = peak_16 * (harts_per_core - 1) / harts_per_core
    roof_tiered = np.minimum(tiered_peak, tiered_eff_bw * oi_range)
    ax.loglog(oi_range, roof_tiered, '--', color='#f28e2b', linewidth=2.5,
              label=f'16c Tiered (eff BW {tiered_eff_bw:.0f} GB/s)', alpha=0.85)

    # Plot measured data points at 16 cores
    datasets = [
        (measured_baseline, 'o', '#4e79a7', 'Baseline (work stealing)'),
        (measured_tiered,   'D', '#f28e2b', 'Tiered WQ (dedicated fetcher)'),
    ]

    for data, marker, color, lbl in datasets:
        if '16c' not in data:
            continue
        v = data['16c']
        achieved_gops = total_ops / v['cycles']
        suffix = '' if data_source == 'measured' else ' (est.)'
        ax.scatter([oi_dram], [achieved_gops], s=140, marker=marker, color=color,
                   edgecolors='black', linewidth=0.8, zorder=5,
                   label=f'{lbl} — 16c{suffix}')
        ax.annotate(f"{v['cycles']/1e6:.0f}M cyc",
                    (oi_dram, achieved_gops),
                    textcoords='offset points', xytext=(10, -8), fontsize=8,
                    color=color, fontweight='bold')

    # OI reference lines
    ax.axvline(x=oi_dram, color='orange', linestyle=':', linewidth=1.5, alpha=0.7,
               label=f'BFS OI (DRAM) = {oi_dram:.2f}')
    ax.axvline(x=oi_total, color='purple', linestyle=':', linewidth=1.5, alpha=0.5,
               label=f'BFS OI (total) = {oi_total:.2f}')

    ax.set_xlabel('Operational Intensity (ops / byte)', fontsize=12)
    ax.set_ylabel('Attainable Performance (GOPs)', fontsize=12)
    ax.set_title(f'Roofline: Baseline vs Tiered WQ ({data_source} data)\n'
                 'RMAT 65K (scale=16, ef=16, seed=42, cusp=16), L2SP 1MB, 16 harts/core',
                 fontsize=12, fontweight='bold')
    ax.set_xlim(1e-3, 100)
    ax.set_ylim(1e-3, 100)
    ax.legend(fontsize=7, loc='upper left', ncol=1)
    ax.grid(True, which='both', alpha=0.2)

    ax.text(0.005, 0.003, 'Memory\nBound', fontsize=14, color='gray', alpha=0.4,
            fontweight='bold', ha='center')
    ax.text(30, 0.5, 'Compute\nBound', fontsize=14, color='gray', alpha=0.4,
            fontweight='bold', ha='center')

    # === Right panel: Core scaling comparison ===
    ax2 = axes[1]

    core_counts = []
    baseline_gops = []
    tiered_gops = []

    for key in ['1c', '2c', '4c', '8c', '16c']:
        if key in measured_baseline:
            ncores = measured_baseline[key]['cores']
            core_counts.append(ncores)
            baseline_gops.append(total_ops / measured_baseline[key]['cycles'])
            if key in measured_tiered:
                tiered_gops.append(total_ops / measured_tiered[key]['cycles'])
            else:
                tiered_gops.append(None)

    # Filter None values for tiered
    tiered_cores = [c for c, g in zip(core_counts, tiered_gops) if g is not None]
    tiered_vals = [g for g in tiered_gops if g is not None]

    ax2.semilogx(core_counts, baseline_gops, 'o-', color='#4e79a7', linewidth=2,
                 markersize=8, label='Baseline (work stealing)')
    if tiered_vals:
        ax2.semilogx(tiered_cores, tiered_vals, 'D-.', color='#f28e2b', linewidth=2,
                     markersize=8, label='Tiered WQ (dedicated fetcher)')

    # Add ideal scaling line
    if baseline_gops:
        base_perf = baseline_gops[0]
        ideal_x = core_counts
        ideal_y = [base_perf * c / core_counts[0] for c in core_counts]
        ax2.semilogx(ideal_x, ideal_y, ':', color='gray', linewidth=1.5,
                     alpha=0.5, label='Ideal linear scaling')

    ax2.set_xlabel('Number of Cores', fontsize=12)
    ax2.set_ylabel('Throughput (GOPs)', fontsize=12)
    ax2.set_title(f'Core Scaling: BFS Throughput ({data_source} data)\n'
                  'RMAT 65K (scale=16, ef=16, cusp=16), 16 harts/core',
                  fontsize=12, fontweight='bold')
    ax2.set_xticks(core_counts)
    ax2.set_xticklabels([str(c) for c in core_counts])
    ax2.legend(fontsize=9, loc='upper left')
    ax2.grid(True, alpha=0.3)

    # Add speedup annotations on the 16-core points
    if '16c' in measured_baseline and '16c' in measured_tiered:
        baseline_16c = measured_baseline['16c']['cycles']
        tiered_16c = measured_tiered['16c']['cycles']
        speedup = baseline_16c / tiered_16c
        gops = total_ops / tiered_16c
        ax2.annotate(f'{speedup:.2f}x vs base',
                     (16, gops),
                     textcoords='offset points', xytext=(15, -15),
                     fontsize=8, color='#f28e2b', fontweight='bold',
                     arrowprops=dict(arrowstyle='->', color='#f28e2b', lw=0.8))

    plt.tight_layout()
    out = '/users/alanandr/drv/bfs_roofline_tiered_comparison.png'
    plt.savefig(out, dpi=150, bbox_inches='tight')
    print(f'Saved: {out}')

    # Print summary table
    print(f'\n=== Performance Summary ({data_source} data, 65K RMAT) ===')
    print(f'{"Variant":<35} {"Cores":>5} {"Cycles":>12} {"GOPs":>8} {"Speedup":>8}')
    print('-' * 70)

    for key in ['1c', '2c', '4c', '8c', '16c']:
        if key not in measured_baseline:
            continue
        ncores = measured_baseline[key]['cores']
        for data, lbl in [
            (measured_baseline, 'Baseline (work stealing)'),
            (measured_tiered, 'Tiered WQ (dedicated fetcher)'),
        ]:
            if key in data:
                cyc = data[key]['cycles']
                gops = total_ops / cyc
                ref_cyc = measured_baseline[key]['cycles']
                speedup = ref_cyc / cyc
                print(f'{lbl:<35} {ncores:>5} {cyc:>12,} {gops:>8.4f} {speedup:>7.2f}x')


if __name__ == '__main__':
    main()
