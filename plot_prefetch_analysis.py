#!/usr/bin/env python3
"""
L1SP Graph Prefetch Analysis & Visualization

Plots:
  1. Performance comparison (cycles): ws vs l1sp_cache vs l1sp_prefetch
  2. Speedup of prefetch vs baselines
  3. Roofline model with prefetch data points
  4. L1SP usage breakdown (prefetch ring vs stacks vs guard)
  5. Prefetch hit rate and efficiency
  6. DRAM traffic reduction estimate

Usage:
  python3 plot_prefetch_analysis.py [--dir prefetch_sweep]
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
from matplotlib.lines import Line2D

# Architecture constants
CLOCK_GHZ = 1.0
BW_L2SP_PER_POD = 64.0   # GB/s
BW_DRAM_EFFECTIVE = 24.0  # GB/s
OPS_PER_EDGE = 5
DRAM_BYTES_PER_EDGE = 4
L2SP_BYTES_PER_EDGE = 16
L1SP_PER_CORE = 262144    # 256 KB default

# Self-prefetch layout per hart (must match C++ constants)
STACK_RESERVE = 4096
PREFETCH_GUARD = 256
PREFETCH_BUF_OFFSET_H0 = 64  # hart 0 has tokens
PREFETCH_BUF_OFFSET_HN = 0   # other harts start at base


def parse_csv(path):
    """Parse prefetch_sweep_results.csv."""
    rows = []
    with open(path) as f:
        reader = csv.DictReader(f)
        for r in reader:
            if r['cycles_elapsed'] in ('FAIL', '0', ''):
                continue
            for k in ['cores', 'harts_per_core', 'total_harts', 'cycles_elapsed',
                       'nodes_discovered', 'edges_traversed',
                       'pf_hits', 'pf_misses', 'pf_hitrate',
                       'pf_nodes', 'pf_edges', 'pf_cycles']:
                try:
                    r[k] = int(r[k])
                except (ValueError, KeyError):
                    r[k] = 0
            rows.append(r)
    return rows


def parse_ramulator_stats(stats_file):
    """Parse key DRAM stats from ramulator output."""
    stats = {}
    if not os.path.exists(stats_file):
        return stats
    with open(stats_file) as f:
        for line in f:
            line = line.strip()
            if ':' in line:
                parts = line.split(':', 1)
                key = parts[0].strip()
                val = parts[1].strip().split()[0] if parts[1].strip() else '0'
                try:
                    stats[key] = float(val)
                except ValueError:
                    pass
    return stats


def parse_output_edges(sweep_dir):
    """Parse total edges from any output.txt."""
    for f in glob.glob(os.path.join(sweep_dir, '*/output.txt')):
        with open(f) as fp:
            for line in fp:
                m = re.search(r'E=(\d+)', line)
                if m:
                    return int(m.group(1))
    return 21090  # default


def parse_l1sp_info(sweep_dir):
    """Parse L1SP size and self-prefetch buffer capacity from prefetch output."""
    info = {'l1sp_per_core': L1SP_PER_CORE, 'harts_per_core': 16,
            'buf_cap_h0': 0, 'buf_cap_hn': 0, 'pf_pct': 0}
    for f in sorted(glob.glob(os.path.join(sweep_dir, 'l1sp_prefetch_*/output.txt'))):
        with open(f) as fp:
            for line in fp:
                m = re.search(r'L1SP: per-core=(\d+) bytes', line)
                if m:
                    info['l1sp_per_core'] = int(m.group(1))
                m = re.search(r'Hardware: (\d+) cores x (\d+) harts', line)
                if m:
                    info['harts_per_core'] = int(m.group(2))
                m = re.search(r'Self-prefetch buffer: hart0=(\d+) edges, other harts=(\d+) edges', line)
                if m:
                    info['buf_cap_h0'] = int(m.group(1))
                    info['buf_cap_hn'] = int(m.group(2))
                m = re.search(r'Total prefetch L1SP per core: (\d+) / (\d+) bytes \((\d+)%\)', line)
                if m:
                    info['pf_pct'] = int(m.group(3))
        if info['buf_cap_h0'] > 0:
            break
    return info


def filter_rows(rows, variant):
    """Filter rows by variant, sort by cores."""
    result = [r for r in rows if r['variant'] == variant]
    result.sort(key=lambda r: r['cores'])
    return result


def plot_performance_comparison(rows, out_dir):
    """Plot 1: End-to-end cycles comparison across variants."""
    fig, ax = plt.subplots(figsize=(12, 7))

    variants = [
        ('ws',            'Work Stealing',         '#4e79a7', ''),
        ('l1sp_cache',    'L1SP Work Cache',       '#59a14f', '//'),
        ('l1sp_prefetch', 'L1SP Graph Prefetch',   '#e15759', 'xx'),
    ]

    all_cores = sorted(set(r['cores'] for r in rows))
    x = np.arange(len(all_cores))
    n_variants = len([v for v, _, _, _ in variants if filter_rows(rows, v)])
    width = 0.8 / max(n_variants, 1)

    vi = 0
    for variant, label, color, hatch in variants:
        vrows = filter_rows(rows, variant)
        if not vrows:
            continue
        cores = [r['cores'] for r in vrows]
        cycles = [r['cycles_elapsed'] / 1e6 for r in vrows]
        # Map to x positions
        xpos = [x[all_cores.index(c)] for c in cores if c in all_cores]
        cycles_mapped = [cy for c, cy in zip(cores, cycles) if c in all_cores]
        bars = ax.bar(np.array(xpos) + vi * width - (n_variants - 1) * width / 2,
                      cycles_mapped, width * 0.9,
                      label=label, color=color, hatch=hatch,
                      edgecolor='black', linewidth=0.5)
        for bar, cyc in zip(bars, cycles_mapped):
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                    f'{cyc:.1f}M', ha='center', va='bottom', fontsize=7)
        vi += 1

    ax.set_xlabel('Cores', fontsize=12)
    ax.set_ylabel('Cycles (millions)', fontsize=12)
    ax.set_xticks(x)
    ax.set_xticklabels(all_cores)
    ax.set_title('BFS Performance: L1SP Graph Prefetch vs Baselines\n'
                 'RMAT Power-Law Graph, 16 harts/core',
                 fontsize=13, fontweight='bold')
    ax.legend(fontsize=10)
    ax.grid(True, axis='y', alpha=0.3)

    plt.tight_layout()
    path = os.path.join(out_dir, 'prefetch_performance.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved: {path}')


def plot_speedup(rows, out_dir):
    """Plot 2: Speedup of prefetch vs other variants."""
    fig, ax = plt.subplots(figsize=(10, 7))

    ws_rows = {r['cores']: r for r in filter_rows(rows, 'ws')}
    cache_rows = {r['cores']: r for r in filter_rows(rows, 'l1sp_cache')}
    pf_rows = {r['cores']: r for r in filter_rows(rows, 'l1sp_prefetch')}

    all_cores = sorted(pf_rows.keys())

    if ws_rows:
        common = sorted(set(all_cores) & set(ws_rows.keys()))
        if common:
            speedups = [ws_rows[c]['cycles_elapsed'] / pf_rows[c]['cycles_elapsed']
                        for c in common]
            ax.plot(common, speedups, '-o', color='#4e79a7', linewidth=2,
                    markersize=8, label='vs Work Stealing')
            for c, s in zip(common, speedups):
                ax.annotate(f'{s:.2f}x', (c, s), textcoords='offset points',
                            xytext=(5, 8), fontsize=9, color='#4e79a7')

    if cache_rows:
        common = sorted(set(all_cores) & set(cache_rows.keys()))
        if common:
            speedups = [cache_rows[c]['cycles_elapsed'] / pf_rows[c]['cycles_elapsed']
                        for c in common]
            ax.plot(common, speedups, '-s', color='#59a14f', linewidth=2,
                    markersize=8, label='vs L1SP Work Cache')
            for c, s in zip(common, speedups):
                ax.annotate(f'{s:.2f}x', (c, s), textcoords='offset points',
                            xytext=(5, -12), fontsize=9, color='#59a14f')

    ax.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5, label='Break-even')
    ax.set_xlabel('Cores', fontsize=12)
    ax.set_ylabel('Speedup (higher = prefetch is faster)', fontsize=12)
    ax.set_title('L1SP Graph Prefetch Speedup\nvs Baseline Variants',
                 fontsize=13, fontweight='bold')
    ax.legend(fontsize=10)
    ax.grid(True, alpha=0.3)
    if all_cores:
        ax.set_xticks(all_cores)

    plt.tight_layout()
    path = os.path.join(out_dir, 'prefetch_speedup.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved: {path}')


def plot_roofline(rows, total_edges, out_dir):
    """Plot 3: Roofline with prefetch data points."""
    total_ops = total_edges * OPS_PER_EDGE
    total_dram_bytes = total_edges * DRAM_BYTES_PER_EDGE
    total_l2sp_bytes = total_edges * L2SP_BYTES_PER_EDGE
    total_bytes = total_dram_bytes + total_l2sp_bytes

    oi_dram = total_ops / total_dram_bytes
    oi_total = total_ops / total_bytes

    fig, ax = plt.subplots(figsize=(13, 9))
    oi_range = np.logspace(-3, 2, 500)

    # Roofline ceilings
    for ncores, color, ls in [(2, '#4e79a7', '-'), (4, '#76b7b2', '-'),
                                (8, '#59a14f', '-'), (16, '#e15759', '-')]:
        peak = ncores * 16 * CLOCK_GHZ
        roof = np.minimum(peak, BW_DRAM_EFFECTIVE * oi_range)
        ax.loglog(oi_range, roof, ls, color=color, linewidth=1.5,
                  label=f'{ncores}-core (peak={peak:.0f} GOPs)', alpha=0.7)

    # DRAM ceiling
    ax.loglog(oi_range, np.minimum(10000, BW_DRAM_EFFECTIVE * oi_range),
              ':', color='gray', linewidth=2,
              label=f'DRAM BW ceiling (~{BW_DRAM_EFFECTIVE:.0f} GB/s)')

    # Plot measured points for each variant
    variant_style = {
        'ws':            ('o', '#4e79a7', 'WS'),
        'l1sp_cache':    ('s', '#59a14f', 'L1Cache'),
        'l1sp_prefetch': ('D', '#e15759', 'Prefetch'),
    }

    for variant, (marker, color, vlabel) in variant_style.items():
        vrows = filter_rows(rows, variant)
        for r in vrows:
            edges = r['edges_traversed'] if r['edges_traversed'] > 0 else total_edges
            ops = edges * OPS_PER_EDGE
            achieved_gops = ops / r['cycles_elapsed']

            # For prefetch variant, effective OI is higher because L1SP reads
            # replaced some DRAM reads
            if variant == 'l1sp_prefetch' and r['pf_hits'] > 0:
                # Estimate: prefetch hits serve from L1SP, misses from DRAM
                total_accesses = r['pf_hits'] + r['pf_misses']
                if total_accesses > 0:
                    dram_fraction = r['pf_misses'] / total_accesses
                    eff_dram_bytes = total_dram_bytes * dram_fraction
                    eff_total_bytes = eff_dram_bytes + total_l2sp_bytes
                    eff_oi = ops / max(eff_total_bytes, 1)
                else:
                    eff_oi = oi_dram
            else:
                eff_oi = oi_dram

            ax.scatter([eff_oi], [achieved_gops], s=120, marker=marker,
                       color=color, edgecolors='black', linewidth=0.6, zorder=5)
            ax.annotate(f"{r['cores']}c {vlabel}",
                        (eff_oi, achieved_gops),
                        textcoords='offset points', xytext=(6, 4),
                        fontsize=7, color=color, fontweight='bold')

    # OI reference lines
    ax.axvline(x=oi_dram, color='orange', linestyle=':', linewidth=1.5, alpha=0.7,
               label=f'BFS OI (DRAM) = {oi_dram:.2f} ops/B')
    ax.axvline(x=oi_total, color='purple', linestyle=':', linewidth=1.5, alpha=0.5,
               label=f'BFS OI (total) = {oi_total:.2f} ops/B')

    # Region annotations
    ax.text(0.003, 0.015, 'Memory\nBound', fontsize=14, alpha=0.3,
            ha='center', va='center', color='#555')
    ax.text(50, 0.015, 'Compute\nBound', fontsize=14, alpha=0.3,
            ha='center', va='center', color='#555')

    ax.set_xlabel('Operational Intensity (ops / byte)', fontsize=12)
    ax.set_ylabel('Attainable Performance (GOPs)', fontsize=12)
    ax.set_title('Roofline Model: L1SP Graph Prefetch Effect\n'
                 'RMAT Power-Law Graph (E=%d)' % total_edges,
                 fontsize=13, fontweight='bold')

    # Custom legend
    extra = [
        Line2D([0], [0], marker='o', color='#4e79a7', linestyle='', markersize=8, label='WS'),
        Line2D([0], [0], marker='s', color='#59a14f', linestyle='', markersize=8, label='L1 Cache'),
        Line2D([0], [0], marker='D', color='#e15759', linestyle='', markersize=8, label='Prefetch'),
    ]
    handles, labels = ax.get_legend_handles_labels()
    ax.legend(handles=handles + extra, fontsize=7, loc='upper left', ncol=2)

    ax.set_xlim(1e-3, 100)
    ax.set_ylim(1e-3, 1e3)

    plt.tight_layout()
    path = os.path.join(out_dir, 'prefetch_roofline.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved: {path}')


def plot_l1sp_usage(rows, l1sp_info, out_dir):
    """Plot 4: L1SP usage breakdown per core (self-prefetch layout)."""
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))

    pf_rows = filter_rows(rows, 'l1sp_prefetch')
    if not pf_rows:
        print("No prefetch data, skipping L1SP usage plot")
        plt.close(fig)
        return

    l1sp_total = l1sp_info['l1sp_per_core']
    hpc = l1sp_info['harts_per_core']
    slot = l1sp_total // hpc  # bytes per hart
    buf_h0_bytes = l1sp_info['buf_cap_h0'] * 4  # int32 edges -> bytes
    buf_hn_bytes = l1sp_info['buf_cap_hn'] * 4
    total_pf_bytes = buf_h0_bytes + buf_hn_bytes * (hpc - 1)
    total_stack_bytes = STACK_RESERVE * hpc
    total_guard_bytes = PREFETCH_GUARD * hpc
    tokens_bytes = PREFETCH_BUF_OFFSET_H0  # tokens only on hart 0
    free_bytes = l1sp_total - total_pf_bytes - total_stack_bytes - total_guard_bytes - tokens_bytes

    # Pie chart: L1SP partition per core
    ax = axes[0]
    sizes = [tokens_bytes, total_pf_bytes, total_guard_bytes, total_stack_bytes, max(0, free_bytes)]
    labels_pie = [
        f'Tokens\n({tokens_bytes}B)',
        f'Prefetch Bufs\n({total_pf_bytes/1024:.1f}KB)',
        f'Guard\n({total_guard_bytes/1024:.1f}KB)',
        f'Stacks\n({total_stack_bytes/1024:.1f}KB)',
        f'Free\n({max(0,free_bytes)/1024:.1f}KB)',
    ]
    colors = ['#bab0ac', '#f28e2b', '#edc948', '#4e79a7', '#cccccc']
    explode = [0, 0.05, 0, 0, 0]
    # Remove zero-sized segments
    nonzero = [(s, l, c, e) for s, l, c, e in zip(sizes, labels_pie, colors, explode) if s > 0]
    if nonzero:
        sizes_nz, labels_nz, colors_nz, explode_nz = zip(*nonzero)
        ax.pie(sizes_nz, labels=labels_nz, colors=colors_nz,
               explode=explode_nz, autopct='%1.1f%%',
               startangle=90, textprops={'fontsize': 9})
    ax.set_title(f'L1SP Layout per Core ({l1sp_total/1024:.0f} KB, {hpc} harts)',
                 fontsize=12, fontweight='bold')

    # Per-hart bar chart showing layout
    ax = axes[1]
    hart_labels = ['Hart 0'] + [f'Hart {i}' for i in range(1, min(hpc, 8))]
    if hpc > 8:
        hart_labels[-1] = f'Hart {hpc-1}'
    n_show = len(hart_labels)
    x = np.arange(n_show)

    buf_sizes_kb = [buf_h0_bytes / 1024] + [buf_hn_bytes / 1024] * (n_show - 1)
    stack_kb = [STACK_RESERVE / 1024] * n_show
    guard_kb = [PREFETCH_GUARD / 1024] * n_show
    tok_kb = [tokens_bytes / 1024] + [0] * (n_show - 1)

    ax.bar(x, buf_sizes_kb, 0.6, label=f'Prefetch Buffer', color='#f28e2b')
    ax.bar(x, stack_kb, 0.6, bottom=buf_sizes_kb, label=f'Stack ({STACK_RESERVE}B)', color='#4e79a7')
    ax.bar(x, guard_kb, 0.6, bottom=[b+s for b,s in zip(buf_sizes_kb, stack_kb)],
           label=f'Guard ({PREFETCH_GUARD}B)', color='#edc948')
    ax.bar(x, tok_kb, 0.6, bottom=[b+s+g for b,s,g in zip(buf_sizes_kb, stack_kb, guard_kb)],
           label='Tokens', color='#bab0ac')

    ax.axhline(y=slot/1024, color='red', linestyle='--', alpha=0.7,
               label=f'Slot ({slot/1024:.0f} KB/hart)')
    ax.set_xlabel('Hart', fontsize=11)
    ax.set_ylabel('L1SP per Hart (KB)', fontsize=11)
    ax.set_xticks(x)
    ax.set_xticklabels(hart_labels, rotation=30, ha='right', fontsize=8)
    ax.legend(fontsize=8, loc='upper right')
    ax.set_title(f'Per-Hart L1SP Layout (Self-Prefetch, {l1sp_info["pf_pct"]}% utilization)',
                 fontsize=12, fontweight='bold')
    ax.grid(True, axis='y', alpha=0.3)

    plt.tight_layout()
    path = os.path.join(out_dir, 'prefetch_l1sp_usage.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved: {path}')


def plot_prefetch_efficiency(rows, out_dir):
    """Plot 5: Prefetch hit rate and efficiency metrics."""
    fig, axes = plt.subplots(1, 3, figsize=(16, 5))

    pf_rows = filter_rows(rows, 'l1sp_prefetch')
    if not pf_rows:
        print("No prefetch data, skipping efficiency plot")
        plt.close(fig)
        return

    cores = [r['cores'] for r in pf_rows]

    # Hit rate
    ax = axes[0]
    hitrates = [r['pf_hitrate'] for r in pf_rows]
    bars = ax.bar(range(len(cores)), hitrates, color='#59a14f', edgecolor='black')
    for bar, hr in zip(bars, hitrates):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height(),
                f'{hr}%', ha='center', va='bottom', fontsize=10, fontweight='bold')
    ax.set_xlabel('Cores', fontsize=11)
    ax.set_ylabel('Hit Rate (%)', fontsize=11)
    ax.set_xticks(range(len(cores)))
    ax.set_xticklabels(cores)
    ax.set_ylim(0, 105)
    ax.set_title('Prefetch Hit Rate', fontsize=12, fontweight='bold')
    ax.grid(True, axis='y', alpha=0.3)

    # Prefetch cycles vs total cycles
    ax = axes[1]
    pf_cyc_frac = [100 * r['pf_cycles'] / max(r['cycles_elapsed'], 1) for r in pf_rows]
    bars = ax.bar(range(len(cores)), pf_cyc_frac, color='#f28e2b', edgecolor='black')
    for bar, frac in zip(bars, pf_cyc_frac):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height(),
                f'{frac:.1f}%', ha='center', va='bottom', fontsize=10)
    ax.set_xlabel('Cores', fontsize=11)
    ax.set_ylabel('Prefetch Overhead (%)', fontsize=11)
    ax.set_xticks(range(len(cores)))
    ax.set_xticklabels(cores)
    ax.set_title('Prefetch Hart Overhead\n(cycles/total cycles)', fontsize=12, fontweight='bold')
    ax.grid(True, axis='y', alpha=0.3)

    # Edges served from L1SP vs DRAM
    ax = axes[2]
    hits_vals = [r['pf_hits'] for r in pf_rows]
    miss_vals = [r['pf_misses'] for r in pf_rows]
    x2 = np.arange(len(cores))
    w2 = 0.35
    ax.bar(x2 - w2/2, [h/1000 for h in hits_vals], w2,
           label='L1SP (prefetched)', color='#59a14f', edgecolor='black')
    ax.bar(x2 + w2/2, [m/1000 for m in miss_vals], w2,
           label='DRAM (fallback)', color='#e15759', edgecolor='black')
    for i, (h, m) in enumerate(zip(hits_vals, miss_vals)):
        total = h + m
        pct = 100 * h / total if total > 0 else 0
        ax.text(i, max(h, m) / 1000 + 0.2, f'{pct:.0f}% L1SP',
                ha='center', va='bottom', fontsize=9, fontweight='bold')
    ax.set_ylabel('Edges (thousands)', fontsize=11)
    ax.set_title('Edge Access Source\n(L1SP prefetch vs DRAM fallback)',
                 fontsize=12, fontweight='bold')
    ax.set_xlabel('Cores', fontsize=11)
    ax.set_xticks(range(len(cores)))
    ax.set_xticklabels(cores)
    ax.legend(fontsize=9)
    ax.grid(True, axis='y', alpha=0.3)

    fig.suptitle('L1SP Graph Prefetch Efficiency', fontsize=14, fontweight='bold')
    plt.tight_layout()
    path = os.path.join(out_dir, 'prefetch_efficiency.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved: {path}')


def plot_dram_traffic(rows, total_edges, out_dir):
    """Plot 6: Estimated DRAM col_idx traffic with prefetch reduction."""
    fig, ax = plt.subplots(figsize=(10, 7))

    variants_data = {}
    for variant in ['ws', 'l1sp_cache', 'l1sp_prefetch']:
        vrows = filter_rows(rows, variant)
        for r in vrows:
            c = r['cores']
            if c not in variants_data:
                variants_data[c] = {}
            edges = r['edges_traversed'] if r['edges_traversed'] > 0 else total_edges

            if variant == 'l1sp_prefetch' and (r['pf_hits'] + r['pf_misses']) > 0:
                total_accesses = r['pf_hits'] + r['pf_misses']
                dram_fraction = r['pf_misses'] / total_accesses
                est_dram = edges * DRAM_BYTES_PER_EDGE * dram_fraction
            else:
                est_dram = edges * DRAM_BYTES_PER_EDGE

            variants_data[c][variant] = est_dram / 1024  # KB

    cores = sorted(variants_data.keys())
    x = np.arange(len(cores))
    width = 0.25

    colors = {'ws': '#4e79a7', 'l1sp_cache': '#59a14f', 'l1sp_prefetch': '#e15759'}
    labels = {'ws': 'Work Stealing', 'l1sp_cache': 'L1SP Cache', 'l1sp_prefetch': 'L1SP Prefetch'}

    vi = 0
    for variant in ['ws', 'l1sp_cache', 'l1sp_prefetch']:
        vals = [variants_data[c].get(variant, None) for c in cores]
        present = [v is not None for v in vals]
        if not any(present):
            continue
        vals_clean = [v if v is not None else 0 for v in vals]
        bars = ax.bar(x + vi * width - width, vals_clean, width * 0.9,
                      label=labels[variant], color=colors[variant],
                      edgecolor='black', linewidth=0.5)
        # Annotate zero bars
        for i, (bar, v, p) in enumerate(zip(bars, vals_clean, present)):
            if p and v < 0.1:
                ax.annotate('0 KB\n(100% L1SP)', (bar.get_x() + bar.get_width()/2, 1),
                            ha='center', va='bottom', fontsize=8, fontweight='bold',
                            color=colors[variant])
        vi += 1

    ax.set_xlabel('Cores', fontsize=12)
    ax.set_ylabel('Estimated DRAM col_idx Traffic (KB)', fontsize=12)
    ax.set_xticks(x)
    ax.set_xticklabels(cores)
    ax.set_title('DRAM Traffic for CSR col_idx Reads\n'
                 'Prefetch serves 100%% from L1SP, eliminating random DRAM reads',
                 fontsize=13, fontweight='bold')
    ax.legend(fontsize=10)
    ax.grid(True, axis='y', alpha=0.3)
    plt.tight_layout()
    path = os.path.join(out_dir, 'prefetch_dram_traffic.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved: {path}')


def print_summary_table(rows, total_edges):
    """Print comparative summary table."""
    print("\n" + "="*90)
    print("  L1SP Graph Prefetch Experiment Summary")
    print("="*90)

    header = f"{'Variant':<18} {'Cores':<6} {'Cycles':<12} {'Nodes':<8} {'Edges':<8} " \
             f"{'PF Hits':<8} {'PF Miss':<8} {'Hit%':<6} {'cyc/edge':<10}"
    print(header)
    print("-"*90)

    for variant in ['ws', 'l1sp_cache', 'l1sp_prefetch']:
        vrows = filter_rows(rows, variant)
        for r in vrows:
            edges = r['edges_traversed'] if r['edges_traversed'] > 0 else total_edges
            cpe = r['cycles_elapsed'] / edges if edges > 0 else 0
            print(f"{r['variant']:<18} {r['cores']:<6} {r['cycles_elapsed']:<12} "
                  f"{r['nodes_discovered']:<8} {edges:<8} "
                  f"{r['pf_hits']:<8} {r['pf_misses']:<8} {r['pf_hitrate']:<6} "
                  f"{cpe:<10.1f}")
        if vrows:
            print()

    # Speedup summary
    print("\nSpeedup (Prefetch vs others):")
    pf = {r['cores']: r for r in filter_rows(rows, 'l1sp_prefetch') if r['cycles_elapsed'] > 0}
    for variant, label in [('ws', 'Work Stealing'), ('l1sp_cache', 'L1SP Cache')]:
        vrows = {r['cores']: r for r in filter_rows(rows, variant) if r['cycles_elapsed'] > 0}
        for c in sorted(pf.keys()):
            if c in vrows and pf[c]['cycles_elapsed'] > 0:
                speedup = vrows[c]['cycles_elapsed'] / pf[c]['cycles_elapsed']
                print(f"  {c}-core: Prefetch vs {label}: {speedup:.2f}x")
    print("="*90)


def main():
    parser = argparse.ArgumentParser(description='L1SP Graph Prefetch Analysis')
    parser.add_argument('--dir', default='prefetch_sweep',
                        help='Directory with sweep results')
    args = parser.parse_args()

    sweep_dir = args.dir
    if not os.path.isabs(sweep_dir):
        sweep_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), sweep_dir)

    csv_path = os.path.join(sweep_dir, 'prefetch_sweep_results.csv')
    if not os.path.exists(csv_path):
        print(f"ERROR: {csv_path} not found. Run sweep_prefetch_experiment.sh first.")
        return

    rows = parse_csv(csv_path)
    if not rows:
        print("ERROR: No valid data rows found")
        return

    total_edges = parse_output_edges(sweep_dir)
    l1sp_info = parse_l1sp_info(sweep_dir)

    print(f"Loaded {len(rows)} data points from {csv_path}")
    print(f"Total edges: {total_edges}")
    print(f"L1SP per core: {l1sp_info['l1sp_per_core']} bytes")
    if l1sp_info['buf_cap_h0'] > 0:
        print(f"Self-prefetch: hart0={l1sp_info['buf_cap_h0']} edges, "
              f"other={l1sp_info['buf_cap_hn']} edges, "
              f"L1SP utilization={l1sp_info['pf_pct']}%")

    print_summary_table(rows, total_edges)

    # Generate all plots
    plot_performance_comparison(rows, sweep_dir)
    plot_speedup(rows, sweep_dir)
    plot_roofline(rows, total_edges, sweep_dir)
    plot_l1sp_usage(rows, l1sp_info, sweep_dir)
    plot_prefetch_efficiency(rows, sweep_dir)
    plot_dram_traffic(rows, total_edges, sweep_dir)

    print(f"\nAll plots saved to {sweep_dir}/")


if __name__ == '__main__':
    main()
