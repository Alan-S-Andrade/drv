#!/usr/bin/env python3
"""
Hart Sweep Analysis: 16 vs 32 harts/core on PANDOHammer BFS

Generates:
  1. End-to-end time comparison (cycles & wall time)
  2. Speedup vs 2-core baseline
  3. L2SP / L1SP utilization
  4. Roofline analysis (both hart configs)

Usage:
  python3 plot_hart_sweep.py [--dir hart_sweep]
"""

import os
import csv
import argparse
import glob
import re

import matplotlib
if os.environ.get('DISPLAY') is None:
    matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

# ─── Architecture constants ───
CLOCK_GHZ = 1.0
BW_L2SP_PER_POD = 64.0   # GB/s (8 banks × 8 GB/s)
BW_DRAM_EFFECTIVE = 24.0  # GB/s (network cap)

# BFS workload constants (from plot_roofline.py for RMAT r16)
TOTAL_EDGES = None   # will be parsed from output
OPS_PER_EDGE = 5
DRAM_BYTES_PER_EDGE = 4
L2SP_BYTES_PER_EDGE = 16


def parse_perf_csv(path):
    """Parse hart_sweep_results.csv → list of dicts (skip FAIL rows)."""
    rows = []
    with open(path) as f:
        reader = csv.DictReader(f)
        for r in reader:
            if r['cycles_elapsed'] == 'FAIL':
                continue
            r['cores'] = int(r['cores'])
            r['harts_per_core'] = int(r['harts_per_core'])
            r['total_harts'] = int(r['total_harts'])
            r['cycles_elapsed'] = int(r['cycles_elapsed'])
            r['nodes_discovered'] = int(r['nodes_discovered'])
            rows.append(r)
    return rows


def parse_utilization_csv(path):
    """Parse utilization_results.csv → list of dicts."""
    rows = []
    with open(path) as f:
        reader = csv.DictReader(f)
        for r in reader:
            for k in ['cores', 'harts_per_core', 'l2sp_total', 'l2sp_used',
                       'l1sp_per_core', 'l1sp_data_used',
                       'nodes_processed', 'edges_traversed', 'imbalance_pct']:
                try:
                    r[k] = int(r[k])
                except (ValueError, KeyError):
                    r[k] = 0
            try:
                r['l2sp_pct'] = float(r['l2sp_pct'])
            except (ValueError, KeyError):
                r['l2sp_pct'] = 0.0
            rows.append(r)
    return rows


def parse_output_edges(sweep_dir):
    """Parse total edges from any output.txt."""
    for f in glob.glob(os.path.join(sweep_dir, '*/output.txt')):
        with open(f) as fp:
            for line in fp:
                m = re.search(r'E=(\d+)', line)
                if m:
                    return int(m.group(1))
    return 0


def filter_rows(rows, variant, harts):
    """Filter and sort by cores."""
    result = [r for r in rows if r['variant'] == variant
              and r['harts_per_core'] == harts]
    result.sort(key=lambda r: r['cores'])
    return result


def plot_end_to_end_times(perf, out_prefix):
    """Plot 1: End-to-end cycle counts for all configurations."""
    fig, axes = plt.subplots(1, 2, figsize=(14, 6), sharey=True)

    for ax_idx, (variant, vlabel) in enumerate([('ws', 'Work Stealing'),
                                                  ('baseline', 'Imbalanced Baseline')]):
        ax = axes[ax_idx]
        # Find common core counts that both 16h and 32h have for this variant
        cores_16 = set(r['cores'] for r in filter_rows(perf, variant, 16))
        cores_32 = set(r['cores'] for r in filter_rows(perf, variant, 32))
        common_cores = sorted(cores_16 & cores_32)
        if not common_cores:
            common_cores = sorted(cores_16 | cores_32)

        x = np.arange(len(common_cores))
        width = 0.35

        for hi, (harts, hcolor, hatch) in enumerate([(16, '#4e79a7', ''),
                                                       (32, '#e15759', '//')]):
            rows = filter_rows(perf, variant, harts)
            rows = [r for r in rows if r['cores'] in common_cores]
            if not rows:
                continue
            cycles = [r['cycles_elapsed'] for r in rows]
            bars = ax.bar(x[:len(cycles)] + hi * width - width / 2,
                          [c / 1e6 for c in cycles],
                          width, label=f'{harts} harts/core',
                          color=hcolor, hatch=hatch, edgecolor='black', linewidth=0.5)
            for bar, cyc in zip(bars, cycles):
                ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                        f'{cyc / 1e6:.1f}M', ha='center', va='bottom', fontsize=7)

        ax.set_xlabel('Cores', fontsize=11)
        ax.set_xticks(x)
        ax.set_xticklabels(common_cores)
        ax.set_title(vlabel, fontsize=12, fontweight='bold')
        ax.legend(fontsize=9)
        ax.grid(True, axis='y', alpha=0.3)

    axes[0].set_ylabel('Cycles (millions)', fontsize=11)
    fig.suptitle('BFS End-to-End Time: 16 vs 32 Harts/Core\nRMAT Power-Law Graph',
                 fontsize=13, fontweight='bold')
    plt.tight_layout()
    path = f'{out_prefix}_times.png'
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved: {path}')


def plot_speedup(perf, out_prefix):
    """Plot 2: Speedup vs 2-core same-hart baseline."""
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))

    for ax_idx, (variant, vlabel) in enumerate([('ws', 'Work Stealing'),
                                                  ('baseline', 'Imbalanced Baseline')]):
        ax = axes[ax_idx]
        for harts, color, marker in [(16, '#4e79a7', 'o'), (32, '#e15759', 's')]:
            rows = filter_rows(perf, variant, harts)
            if len(rows) < 2:
                continue
            base_cycles = rows[0]['cycles_elapsed']  # 2-core baseline
            cores = [r['cores'] for r in rows]
            speedups = [base_cycles / r['cycles_elapsed'] for r in rows]
            ax.plot(cores, speedups, f'-{marker}', color=color, linewidth=2,
                    markersize=8, label=f'{harts} harts/core')
            for c, s in zip(cores, speedups):
                ax.annotate(f'{s:.2f}x', (c, s), textcoords='offset points',
                            xytext=(5, 5), fontsize=8, color=color)

        # ideal line based on available cores
        all_cores = sorted(set(r['cores'] for r in perf if r['variant'] == variant))
        if all_cores:
            ax.plot(all_cores, [c / all_cores[0] for c in all_cores],
                    '--', color='gray', alpha=0.5, label='Ideal scaling')
        ax.set_xlabel('Cores', fontsize=11)
        ax.set_ylabel('Speedup (vs 2-core same harts)', fontsize=11)
        ax.set_title(vlabel, fontsize=12, fontweight='bold')
        ax.legend(fontsize=9)
        ax.grid(True, alpha=0.3)
        if all_cores:
            ax.set_xticks(all_cores)

    fig.suptitle('BFS Speedup: 16 vs 32 Harts/Core\nRMAT Power-Law Graph',
                 fontsize=13, fontweight='bold')
    plt.tight_layout()
    path = f'{out_prefix}_speedup.png'
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved: {path}')


def plot_hart_comparison_speedup(perf, out_prefix):
    """Plot 3: Direct 32h vs 16h speedup at each core count."""
    fig, ax = plt.subplots(figsize=(10, 6))

    core_counts = sorted(set(r['cores'] for r in perf))

    for variant, color, marker in [('ws', '#59a14f', 'D'),
                                    ('baseline', '#f28e2b', '^')]:
        r16 = filter_rows(perf, variant, 16)
        r32 = filter_rows(perf, variant, 32)
        if not r16 or not r32:
            continue
        c16 = {r['cores']: r['cycles_elapsed'] for r in r16}
        c32 = {r['cores']: r['cycles_elapsed'] for r in r32}
        cores = sorted(set(c16.keys()) & set(c32.keys()))
        speedups = [c16[c] / c32[c] for c in cores]
        ax.plot(cores, speedups, f'-{marker}', color=color, linewidth=2,
                markersize=10, label=variant.replace('ws', 'Work Stealing').replace('baseline', 'Baseline'))
        for c, s in zip(cores, speedups):
            ax.annotate(f'{s:.2f}x', (c, s), textcoords='offset points',
                        xytext=(5, 8), fontsize=9, color=color, fontweight='bold')

    ax.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5, label='Break-even')
    ax.set_xlabel('Cores', fontsize=12)
    ax.set_ylabel('Speedup (32h / 16h)', fontsize=12)
    ax.set_title('32 Harts/Core vs 16 Harts/Core Speedup\n(>1 = 32h is faster)',
                 fontsize=13, fontweight='bold')
    ax.legend(fontsize=10)
    ax.grid(True, alpha=0.3)
    ax.set_xticks(core_counts)

    plt.tight_layout()
    path = f'{out_prefix}_32v16_speedup.png'
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved: {path}')


def plot_utilization(util, out_prefix):
    """Plot 4: L2SP and L1SP utilization."""
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # Use only ws rows that have data
    ws_rows = [r for r in util if r['variant'] == 'ws' and r['l2sp_total'] > 0]
    core_counts = sorted(set(r['cores'] for r in ws_rows))
    if not core_counts:
        print("No utilization data available, skipping utilization plot")
        plt.close(fig)
        return
    x = np.arange(len(core_counts))
    width = 0.3

    # --- L2SP usage % ---
    ax = axes[0][0]
    for harts, color, hatch in [(16, '#4e79a7', ''), (32, '#e15759', '//')]:
        rows = [r for r in ws_rows if r['harts_per_core'] == harts]
        rows = [r for r in rows if r['cores'] in core_counts]
        rows.sort(key=lambda r: r['cores'])
        if not rows:
            continue
        pcts = [r['l2sp_pct'] for r in rows]
        offset = 0 if harts == 16 else width
        ax.bar(x[:len(pcts)] - width/2 + offset, pcts, width * 0.9,
               label=f'WS {harts}h', color=color,
               hatch=hatch, edgecolor='black', linewidth=0.5, alpha=0.8)
    ax.set_ylabel('L2SP Used (%)', fontsize=10)
    ax.set_xticks(x)
    ax.set_xticklabels(core_counts)
    ax.set_xlabel('Cores')
    ax.set_title('L2SP Utilization', fontsize=11, fontweight='bold')
    ax.legend(fontsize=8)
    ax.grid(True, axis='y', alpha=0.3)

    # --- L2SP bytes used (absolute) ---
    ax = axes[0][1]
    for harts, color, hatch in [(16, '#4e79a7', ''), (32, '#e15759', '//')]:
        rows = [r for r in ws_rows if r['harts_per_core'] == harts]
        rows = [r for r in rows if r['cores'] in core_counts]
        rows.sort(key=lambda r: r['cores'])
        if not rows:
            continue
        used = [r['l2sp_used'] / 1024 for r in rows]
        offset = 0 if harts == 16 else width
        ax.bar(x[:len(used)] - width/2 + offset, used, width * 0.9,
               label=f'WS {harts}h', color=color,
               hatch=hatch, edgecolor='black', linewidth=0.5, alpha=0.8)
    ax.set_ylabel('L2SP Used (KB)', fontsize=10)
    ax.set_xticks(x)
    ax.set_xticklabels(core_counts)
    ax.set_xlabel('Cores')
    ax.set_title('L2SP Absolute Usage', fontsize=11, fontweight='bold')
    ax.legend(fontsize=8)
    ax.grid(True, axis='y', alpha=0.3)

    # --- Work imbalance % ---
    ax = axes[1][0]
    for harts, color, hatch in [(16, '#4e79a7', ''), (32, '#e15759', '//')]:
        rows = [r for r in ws_rows if r['harts_per_core'] == harts]
        rows = [r for r in rows if r['cores'] in core_counts]
        rows.sort(key=lambda r: r['cores'])
        if not rows:
            continue
        imbal = [r['imbalance_pct'] for r in rows]
        offset = 0 if harts == 16 else width
        ax.bar(x[:len(imbal)] - width/2 + offset, imbal, width * 0.9,
               label=f'WS {harts}h', color=color,
               hatch=hatch, edgecolor='black', linewidth=0.5, alpha=0.8)
    ax.set_ylabel('Imbalance (%)', fontsize=10)
    ax.set_xticks(x)
    ax.set_xticklabels(core_counts)
    ax.set_xlabel('Cores')
    ax.set_title('Work Imbalance (max−min)/max', fontsize=11, fontweight='bold')
    ax.legend(fontsize=8)
    ax.grid(True, axis='y', alpha=0.3)

    # --- Cycles comparison (using perf data for WS only) ---
    ax = axes[1][1]
    ax.text(0.5, 0.5,
            'L2SP utilization is constant (5.6%)\n'
            'because this small graph (1K vertices)\n'
            'fits entirely in L2SP.\n\n'
            'L1SP data tier: unused (no overflow).\n'
            'L1SP stacks: 16KB/hart @16h, 8KB/hart @32h.',
            transform=ax.transAxes, fontsize=11,
            va='center', ha='center',
            bbox=dict(boxstyle='round,pad=0.5', facecolor='lightyellow', alpha=0.8))
    ax.set_title('Memory Tier Summary', fontsize=11, fontweight='bold')
    ax.set_xticks([])
    ax.set_yticks([])

    fig.suptitle('Memory & Work Utilization: 16 vs 32 Harts/Core',
                 fontsize=13, fontweight='bold')
    plt.tight_layout()
    path = f'{out_prefix}_utilization.png'
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved: {path}')


def plot_roofline(perf, total_edges, out_prefix):
    """Plot 5: Roofline model with both 16h and 32h measured points."""
    total_ops = total_edges * OPS_PER_EDGE
    total_dram_bytes = total_edges * DRAM_BYTES_PER_EDGE
    total_l2sp_bytes = total_edges * L2SP_BYTES_PER_EDGE
    total_bytes = total_dram_bytes + total_l2sp_bytes

    oi_dram = total_ops / total_dram_bytes
    oi_total = total_ops / total_bytes

    fig, ax = plt.subplots(figsize=(12, 8))
    oi_range = np.logspace(-3, 2, 500)

    # Roofline ceilings for key core counts
    roof_configs = [
        ('2-core',   2,  BW_L2SP_PER_POD, '#4e79a7', '-', 1.5),
        ('4-core',   4,  BW_L2SP_PER_POD, '#76b7b2', '-', 1.5),
        ('8-core',   8,  BW_L2SP_PER_POD, '#59a14f', '-', 1.5),
        ('16-core', 16,  BW_L2SP_PER_POD, '#e15759', '-', 2.0),
    ]

    for label, ncores, bw, color, ls, lw in roof_configs:
        # 16-hart peak compute
        peak_16h = ncores * 16 * CLOCK_GHZ  # GOPs (1 op/cycle/hart)
        peak_32h = ncores * 32 * CLOCK_GHZ
        roof_16 = np.minimum(peak_16h, bw * oi_range)
        roof_32 = np.minimum(peak_32h, bw * oi_range)
        ax.loglog(oi_range, roof_16, ls, color=color, linewidth=lw,
                  label=f'{label} 16h (peak={peak_16h:.0f} GOPs)', alpha=0.7)
        ax.loglog(oi_range, roof_32, '--', color=color, linewidth=lw,
                  label=f'{label} 32h (peak={peak_32h:.0f} GOPs)', alpha=0.5)

    # DRAM ceiling
    ax.loglog(oi_range, np.minimum(10000, BW_DRAM_EFFECTIVE * oi_range),
              ':', color='gray', linewidth=2, label=f'DRAM BW ceiling (~{BW_DRAM_EFFECTIVE:.0f} GB/s)')

    # Plot measured points
    markers_16h = {'ws': ('o', '#4e79a7'), 'baseline': ('^', '#76b7b2')}
    markers_32h = {'ws': ('s', '#e15759'), 'baseline': ('D', '#f28e2b')}

    for variant, vlabel in [('ws', 'WS'), ('baseline', 'BL')]:
        for harts, mdict, suffix in [(16, markers_16h, '16h'),
                                      (32, markers_32h, '32h')]:
            marker, color = mdict[variant]
            rows = filter_rows(perf, variant, harts)
            for r in rows:
                achieved_gops = total_ops / r['cycles_elapsed']
                ax.scatter([oi_dram], [achieved_gops], s=100, marker=marker,
                           color=color, edgecolors='black', linewidth=0.6, zorder=5)
                ax.annotate(f"{r['cores']}c {suffix}",
                            (oi_dram, achieved_gops),
                            textcoords='offset points', xytext=(6, 4),
                            fontsize=7, color=color, fontweight='bold')

    # Custom legend entries for measured points
    from matplotlib.lines import Line2D
    extra = [
        Line2D([0], [0], marker='o', color='#4e79a7', linestyle='', markersize=8, label='WS 16h'),
        Line2D([0], [0], marker='s', color='#e15759', linestyle='', markersize=8, label='WS 32h'),
        Line2D([0], [0], marker='^', color='#76b7b2', linestyle='', markersize=8, label='BL 16h'),
        Line2D([0], [0], marker='D', color='#f28e2b', linestyle='', markersize=8, label='BL 32h'),
    ]

    # OI lines
    ax.axvline(x=oi_dram, color='orange', linestyle=':', linewidth=1.5, alpha=0.7,
               label=f'BFS OI (DRAM) = {oi_dram:.2f} ops/B')
    ax.axvline(x=oi_total, color='purple', linestyle=':', linewidth=1.5, alpha=0.5,
               label=f'BFS OI (total) = {oi_total:.2f} ops/B')

    handles, labels = ax.get_legend_handles_labels()
    ax.legend(handles=handles + extra, fontsize=7, loc='upper left', ncol=2)

    ax.set_xlabel('Operational Intensity (ops / byte)', fontsize=12)
    ax.set_ylabel('Attainable Performance (GOPs)', fontsize=12)
    ax.set_title('Roofline Model: 16 vs 32 Harts/Core\n'
                 f'RMAT Power-Law Graph (E={total_edges:,})',
                 fontsize=13, fontweight='bold')
    ax.set_xlim(1e-3, 100)
    ax.set_ylim(1e-3, 1000)
    ax.grid(True, which='both', alpha=0.2)

    ax.text(0.005, 0.003, 'Memory\nBound', fontsize=14, color='gray', alpha=0.4,
            fontweight='bold', ha='center')
    ax.text(30, 0.5, 'Compute\nBound', fontsize=14, color='gray', alpha=0.4,
            fontweight='bold', ha='center')

    plt.tight_layout()
    path = f'{out_prefix}_roofline.png'
    fig.savefig(path, dpi=150, bbox_inches='tight')
    print(f'Saved: {path}')

    # Zoomed version focused on measured data points
    ax.set_xlim(0.8, 2.5)
    ax.set_ylim(0.02, 0.12)
    ax.texts[-1].set_visible(False)  # hide Compute Bound label
    ax.texts[-2].set_visible(False)  # hide Memory Bound label
    path_zoom = f'{out_prefix}_roofline_zoom.png'
    fig.savefig(path_zoom, dpi=150, bbox_inches='tight')
    print(f'Saved: {path_zoom}')

    # Extra-close zoom on the data cluster
    ax.set_xlim(1.1, 1.4)
    ax.set_ylim(0.035, 0.09)
    path_zoom2 = f'{out_prefix}_roofline_zoom2.png'
    fig.savefig(path_zoom2, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved: {path_zoom2}')


def print_summary_table(perf):
    """Print a text summary table."""
    print("\n" + "=" * 90)
    print("HART SWEEP RESULTS SUMMARY")
    print("=" * 90)
    print(f"{'Variant':<12} {'Cores':>5} {'Harts':>5} {'Total':>6} {'Cycles':>14} "
          f"{'Nodes':>8} {'Cyc/Node':>10} {'GOPs':>8}")
    print("-" * 90)

    total_edges_global = 0
    for r in perf:
        cyc_per_node = r['cycles_elapsed'] / max(r['nodes_discovered'], 1)
        # estimate total ops for GOPs calculation
        gops = '-'
        print(f"{r['variant']:<12} {r['cores']:>5} {r['harts_per_core']:>5} "
              f"{r['total_harts']:>6} {r['cycles_elapsed']:>14,} "
              f"{r['nodes_discovered']:>8,} {cyc_per_node:>10.1f} {gops:>8}")

    # 32h vs 16h comparison
    print("\n" + "=" * 90)
    print("32h vs 16h SPEEDUP")
    print("=" * 90)
    for variant in ['ws', 'baseline']:
        r16 = {r['cores']: r for r in perf
               if r['variant'] == variant and r['harts_per_core'] == 16}
        r32 = {r['cores']: r for r in perf
               if r['variant'] == variant and r['harts_per_core'] == 32}
        cores = sorted(set(r16.keys()) & set(r32.keys()))
        print(f"\n{variant}:")
        for c in cores:
            speedup = r16[c]['cycles_elapsed'] / r32[c]['cycles_elapsed']
            delta = r16[c]['cycles_elapsed'] - r32[c]['cycles_elapsed']
            pct = (1 - r32[c]['cycles_elapsed'] / r16[c]['cycles_elapsed']) * 100
            print(f"  {c:>2} cores: 16h={r16[c]['cycles_elapsed']:>12,}  "
                  f"32h={r32[c]['cycles_elapsed']:>12,}  "
                  f"speedup={speedup:.3f}x  ({pct:+.1f}%)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dir', default='/users/alanandr/drv/hart_sweep',
                        help='Hart sweep output directory')
    args = parser.parse_args()

    perf_csv = os.path.join(args.dir, 'hart_sweep_results.csv')
    util_csv = os.path.join(args.dir, 'utilization_results.csv')
    out_prefix = os.path.join(args.dir, 'hart_sweep')

    if not os.path.exists(perf_csv):
        print(f"ERROR: {perf_csv} not found. Run sweep_hart_experiment.sh first.")
        return

    perf = parse_perf_csv(perf_csv)
    print_summary_table(perf)

    total_edges = parse_output_edges(args.dir)
    if total_edges == 0:
        total_edges = 580_000  # fallback estimate from RMAT r16
        print(f"WARNING: Could not parse edges from output. Using estimate: {total_edges:,}")
    else:
        print(f"Total edges (from simulation): {total_edges:,}")

    # Generate plots
    plot_end_to_end_times(perf, out_prefix)
    plot_speedup(perf, out_prefix)
    plot_hart_comparison_speedup(perf, out_prefix)

    if os.path.exists(util_csv):
        util = parse_utilization_csv(util_csv)
        plot_utilization(util, out_prefix)

    plot_roofline(perf, total_edges, out_prefix)

    print(f"\nAll plots saved to: {args.dir}/hart_sweep_*.png")


if __name__ == '__main__':
    main()
