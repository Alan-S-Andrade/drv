#!/usr/bin/env python3
"""Roofline comparison: Baseline vs Tiered WQ (CAS) vs Tiered WQ + FAA.

Three-variant comparison on the PANDOHammer roofline model.
The tiered variants dedicate 1 of 16 harts per core as a fetcher.
The FAA variant additionally replaces all CAS-based queue ops with
fetch-and-add for lower-latency, contention-free reservations.

Usage:
  # With measured data from sweep:
  python3 plot_roofline_tiered_faa_comparison.py --csv tiered_faa_results/summary.csv

  # With estimated data (no simulation):
  python3 plot_roofline_tiered_faa_comparison.py
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

# ─── BFS workload characteristics (65K RMAT, CUSP=16) ───
total_edges = 1_818_338
ops_per_edge = 5
total_ops = total_edges * ops_per_edge
dram_bytes_per_edge = 4   # col_idx read
l2sp_bytes_per_edge = 16  # visited CAS
total_dram_bytes = total_edges * dram_bytes_per_edge
total_l2sp_bytes = total_edges * l2sp_bytes_per_edge
total_bytes = total_dram_bytes + total_l2sp_bytes

oi_dram  = total_ops / total_dram_bytes
oi_total = total_ops / total_bytes

# For tiered variants, effective OI shifts because L1SP serves queue accesses
l1sp_capture_rate = 0.70
tiered_l1sp_bw_16c = 16 * bw_l1sp_per_core  # 128 GB/s
tiered_eff_bw = l1sp_capture_rate * tiered_l1sp_bw_16c + (1 - l1sp_capture_rate) * bw_l2sp_per_pod

# ─── Measured baseline data (65K RMAT, scale=16, ef=16, seed=42, cusp=16) ───
measured_baseline = {
    '1c':  {'cycles': 312_724_401, 'cores': 1},
    '2c':  {'cycles': 218_278_516, 'cores': 2},
    '4c':  {'cycles': 196_397_562, 'cores': 4},
    '8c':  {'cycles': 194_981_865, 'cores': 8},
    '16c': {'cycles': 157_838_265, 'cores': 16},
}

measured_tiered_cas = {}
measured_tiered_faa = {}


def load_csv_results(csv_path):
    """Load results from the sweep summary CSV.

    CSV format: variant,cores,cycles,nodes_discovered,edges_traversed
    """
    baseline = {}
    tiered_cas = {}
    tiered_faa = {}
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
            if 'tiered_wq_faa' in variant:
                tiered_faa[key] = entry
            elif 'tiered' in variant:
                tiered_cas[key] = entry
            else:
                baseline[key] = entry
    return baseline, tiered_cas, tiered_faa


def estimate_tiered_cas_cycles(baseline_cycles, ncores):
    """Estimate tiered WQ (CAS) cycles from baseline."""
    if ncores <= 2:
        overhead = 1.05   # 5% slower (fetcher hart waste)
    elif ncores <= 8:
        overhead = 0.92   # 8% faster (reduced contention)
    else:
        overhead = 0.85   # 15% faster (L2SP contention relief)
    return int(baseline_cycles * overhead)


def estimate_tiered_faa_cycles(baseline_cycles, ncores):
    """Estimate tiered WQ + FAA cycles from baseline.

    FAA removes CAS retry loops on both L1SP and L2SP queue heads,
    giving additional improvement over tiered-CAS especially at high core counts.
    """
    if ncores <= 2:
        overhead = 1.02   # 2% slower (fetcher waste, but FAA offsets some)
    elif ncores <= 8:
        overhead = 0.88   # 12% faster (reduced contention + no CAS retry)
    else:
        overhead = 0.78   # 22% faster (FAA + tiered synergy at scale)
    return int(baseline_cycles * overhead)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--csv', type=str, default=None,
                        help='Path to sweep summary.csv with measured results')
    args = parser.parse_args()

    global measured_baseline, measured_tiered_cas, measured_tiered_faa

    if args.csv and os.path.exists(args.csv):
        print(f'Loading measured data from {args.csv}')
        measured_baseline, measured_tiered_cas, measured_tiered_faa = load_csv_results(args.csv)
        data_source = 'measured'
    else:
        if args.csv:
            print(f'Warning: {args.csv} not found, using estimates')
        data_source = 'estimated'
        # Build estimates from baseline
        for key, val in measured_baseline.items():
            measured_tiered_cas[key] = {
                'cycles': estimate_tiered_cas_cycles(val['cycles'], val['cores']),
                'cores': val['cores'],
            }
            measured_tiered_faa[key] = {
                'cycles': estimate_tiered_faa_cycles(val['cycles'], val['cores']),
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

    # Tiered effective roofline
    ncores_16 = 16
    tiered_peak = ncores_16 * clock_ghz * (harts_per_core - 1) / harts_per_core
    roof_tiered = np.minimum(tiered_peak, tiered_eff_bw * oi_range)
    ax.loglog(oi_range, roof_tiered, '--', color='#f28e2b', linewidth=2.5,
              label=f'16c Tiered (eff BW {tiered_eff_bw:.0f} GB/s)', alpha=0.85)

    # Plot measured data points at 16 cores
    datasets = [
        (measured_baseline,    'o', '#4e79a7', 'Baseline (CAS)'),
        (measured_tiered_cas,  's', '#f28e2b', 'Tiered WQ (CAS)'),
        (measured_tiered_faa,  'D', '#e15759', 'Tiered WQ + FAA'),
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
    ax.set_title(f'Roofline: Baseline vs Tiered WQ vs Tiered+FAA ({data_source} data)\n'
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
    tiered_cas_gops = []
    tiered_faa_gops = []

    for key in ['1c', '2c', '4c', '8c', '16c']:
        if key in measured_baseline:
            ncores = measured_baseline[key]['cores']
            core_counts.append(ncores)
            baseline_gops.append(total_ops / measured_baseline[key]['cycles'])
            if key in measured_tiered_cas:
                tiered_cas_gops.append(total_ops / measured_tiered_cas[key]['cycles'])
            else:
                tiered_cas_gops.append(None)
            if key in measured_tiered_faa:
                tiered_faa_gops.append(total_ops / measured_tiered_faa[key]['cycles'])
            else:
                tiered_faa_gops.append(None)

    # Filter None values
    cas_cores = [c for c, g in zip(core_counts, tiered_cas_gops) if g is not None]
    cas_vals = [g for g in tiered_cas_gops if g is not None]
    faa_cores = [c for c, g in zip(core_counts, tiered_faa_gops) if g is not None]
    faa_vals = [g for g in tiered_faa_gops if g is not None]

    ax2.semilogx(core_counts, baseline_gops, 'o-', color='#4e79a7', linewidth=2,
                 markersize=8, label='Baseline (CAS)')
    if cas_vals:
        ax2.semilogx(cas_cores, cas_vals, 's-.', color='#f28e2b', linewidth=2,
                     markersize=8, label='Tiered WQ (CAS)')
    if faa_vals:
        ax2.semilogx(faa_cores, faa_vals, 'D--', color='#e15759', linewidth=2,
                     markersize=8, label='Tiered WQ + FAA')

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
    for data, color, lbl in [
        (measured_tiered_cas, '#f28e2b', 'CAS'),
        (measured_tiered_faa, '#e15759', 'FAA'),
    ]:
        if '16c' in measured_baseline and '16c' in data:
            baseline_16c = measured_baseline['16c']['cycles']
            variant_16c = data['16c']['cycles']
            speedup = baseline_16c / variant_16c
            gops = total_ops / variant_16c
            y_offset = -15 if lbl == 'CAS' else 10
            ax2.annotate(f'{speedup:.2f}x ({lbl})',
                         (16, gops),
                         textcoords='offset points', xytext=(15, y_offset),
                         fontsize=8, color=color, fontweight='bold',
                         arrowprops=dict(arrowstyle='->', color=color, lw=0.8))

    plt.tight_layout()
    out = '/users/alanandr/drv/bfs_roofline_tiered_faa_comparison.png'
    plt.savefig(out, dpi=150, bbox_inches='tight')
    print(f'Saved: {out}')

    # Print summary table
    print(f'\n=== Performance Summary ({data_source} data, 65K RMAT CUSP=16) ===')
    print(f'{"Variant":<30} {"Cores":>5} {"Cycles":>12} {"GOPs":>8} {"Speedup":>8}')
    print('-' * 65)

    for key in ['1c', '2c', '4c', '8c', '16c']:
        if key not in measured_baseline:
            continue
        ncores = measured_baseline[key]['cores']
        for data, lbl in [
            (measured_baseline,    'Baseline (CAS)'),
            (measured_tiered_cas,  'Tiered WQ (CAS)'),
            (measured_tiered_faa,  'Tiered WQ + FAA'),
        ]:
            if key in data:
                cyc = data[key]['cycles']
                gops = total_ops / cyc
                ref_cyc = measured_baseline[key]['cycles']
                speedup = ref_cyc / cyc
                print(f'{lbl:<30} {ncores:>5} {cyc:>12,} {gops:>8.4f} {speedup:>7.2f}x')


if __name__ == '__main__':
    main()
