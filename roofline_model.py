#!/usr/bin/env python3
"""
Roofline Model: PANDOHammer vs GPU vs CPU

Demonstrates why PANDOHammer's high machine balance (bytes/FLOP)
makes it superior for bandwidth-bound kernels.

Architecture parameters from model/pandohammer.py and element/:
  - 1 GHz clock, in-order single-issue, barrel-threaded (16 harts/core)
  - 64 cores per pod
  - L2SP: 8 GB/s per bank (1 req/cycle x 8 bytes x 1 GHz)
  - DRAM:  8 GB/s per bank

Usage:
    python3 roofline_model.py
    python3 roofline_model.py --l2sp-banks 4 --dram-banks 1
"""
import os
import argparse
import matplotlib
if os.environ.get('DISPLAY') is None:
    matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# =====================================================================
# Architecture Definitions
# =====================================================================

def pando_config(cores=64, l2sp_banks=64, dram_banks=1,
                 clock_ghz=1.0, bw_per_bank_gbs=8.0):
    """Return PANDOHammer architecture configs as a list of dicts."""
    peak = cores * clock_ghz  # 1 op/cycle/core (scalar)
    configs = []
    if l2sp_banks > 0:
        configs.append({
            'name': f'PANDO {cores}c (L2SP, {l2sp_banks} banks)',
            'peak': peak,
            'bw': l2sp_banks * bw_per_bank_gbs,
            'color': '#0055AA',
            'ls': '-',
            'lw': 3.0,
            'marker': 's',
        })
    if l2sp_banks > 1:
        configs.append({
            'name': f'PANDO {cores}c (L2SP, 1 bank)',
            'peak': peak,
            'bw': 1 * bw_per_bank_gbs,
            'color': '#88BBDD',
            'ls': ':',
            'lw': 2.0,
            'marker': 's',
        })
    configs.append({
        'name': f'PANDO {cores}c (DRAM, {dram_banks} bank{"s" if dram_banks>1 else ""})',
        'peak': peak,
        'bw': dram_banks * bw_per_bank_gbs,
        'color': '#5599CC',
        'ls': '--',
        'lw': 2.0,
        'marker': 'D',
    })
    return configs

def comparison_archs():
    """Return GPU and CPU architectures for comparison."""
    return [
        {
            'name': 'NVIDIA A100 (FP64, HBM2e)',
            'peak': 9700,    # GFLOP/s FP64
            'bw': 2039,      # GB/s HBM2e
            'color': '#76B900',
            'ls': '-',
            'lw': 2.5,
            'marker': '^',
        },
        {
            'name': 'NVIDIA H100 (FP64, HBM3)',
            'peak': 33500,   # GFLOP/s FP64
            'bw': 3350,      # GB/s HBM3
            'color': '#FF6600',
            'ls': '-',
            'lw': 2.5,
            'marker': 'v',
        },
        {
            'name': 'Intel Xeon 8380 (FP64, DDR4)',
            'peak': 2000,    # GFLOP/s FP64 (AVX-512)
            'bw': 205,       # GB/s DDR4-3200 8ch
            'color': '#0071C5',
            'ls': '-',
            'lw': 2.5,
            'marker': 'o',
        },
    ]

# Representative kernel operational intensities
KERNELS = [
    ('BFS / Graph',    0.03),
    ('SpMV',           0.17),
    ('Stencil',        0.50),
    ('Dense GEMM',    16.00),
]

# =====================================================================
# Plotting
# =====================================================================

def plot_roofline(archs, kernels, save_prefix='roofline_comparison'):
    oi = np.logspace(-3, 2.5, 3000)

    fig = plt.figure(figsize=(20, 14))
    gs = fig.add_gridspec(2, 2, hspace=0.38, wspace=0.30)
    ax1 = fig.add_subplot(gs[0, :])    # roofline
    ax2 = fig.add_subplot(gs[1, 0])    # utilization
    ax3 = fig.add_subplot(gs[1, 1])    # machine balance

    # ---- Panel 1: Absolute Roofline (log-log) ----
    for a in archs:
        perf = np.minimum(a['bw'] * oi, a['peak'])
        ax1.loglog(oi, perf, label=a['name'], color=a['color'],
                   linestyle=a['ls'], linewidth=a['lw'])
        ridge = a['peak'] / a['bw']
        ax1.plot(ridge, a['peak'], marker=a.get('marker','o'),
                 color=a['color'], markersize=9, zorder=5)
        # ridge annotation
        ax1.annotate(f'{ridge:.2f}',
                     xy=(ridge, a['peak']),
                     xytext=(ridge * 1.8, a['peak'] * 1.4),
                     fontsize=7, color=a['color'], alpha=0.85,
                     arrowprops=dict(arrowstyle='->', color=a['color'], alpha=0.5))

    for kname, koi in kernels:
        ax1.axvline(x=koi, color='gray', linestyle=':', alpha=0.4)
        ax1.text(koi * 0.82, 0.012, kname, rotation=90, va='bottom',
                 ha='right', fontsize=10, color='dimgray', fontweight='bold')

    ax1.axvspan(1e-3, 1.0, alpha=0.04, color='red')
    ax1.text(0.003, 4e4, 'Bandwidth-bound\nregion', fontsize=11,
             color='red', alpha=0.35, fontstyle='italic')

    ax1.set_xlabel('Operational Intensity (FLOP/byte)', fontsize=14)
    ax1.set_ylabel('Attainable Performance (GFLOP/s)', fontsize=14)
    ax1.set_title('Roofline Model: PANDOHammer vs GPU vs CPU', fontsize=16,
                  fontweight='bold')
    ax1.legend(loc='upper left', fontsize=9, ncol=2)
    ax1.grid(True, which='both', alpha=0.15)
    ax1.set_xlim(1e-3, 300)
    ax1.set_ylim(1e-2, 1e5)

    # ---- Panel 2: Compute Utilization ----
    for a in archs:
        util = np.minimum(a['bw'] * oi / a['peak'], 1.0) * 100
        ax2.semilogx(oi, util, label=a['name'], color=a['color'],
                     linestyle=a['ls'], linewidth=a['lw'])

    for kname, koi in kernels:
        ax2.axvline(x=koi, color='gray', linestyle=':', alpha=0.4)
        ax2.text(koi * 0.82, 3, kname, rotation=90, va='bottom',
                 ha='right', fontsize=9, color='dimgray', fontweight='bold')

    # annotate utilization at BFS OI for key architectures
    bfs_oi = 0.03
    annotations_done = set()
    for a in archs:
        u = min(a['bw'] * bfs_oi / a['peak'], 1.0) * 100
        key = a['name'].split('(')[0].strip()
        if key in annotations_done:
            continue
        annotations_done.add(key)
        if 'L2SP, 64' in a['name'] or 'L2SP, 4' in a['name']:
            label = f'{u:.0f}%'
            offset = (3.5, min(u + 12, 95))
        elif 'A100' in a['name']:
            label = f'{u:.1f}%'
            offset = (4, u + 18)
        elif 'H100' in a['name']:
            label = f'{u:.2f}%'
            offset = (6, u + 12)
        elif 'Xeon' in a['name']:
            label = f'{u:.2f}%'
            offset = (8, u + 8)
        else:
            continue
        ax2.annotate(label, xy=(bfs_oi, u),
                     xytext=(bfs_oi * offset[0], offset[1]),
                     fontsize=9, fontweight='bold', color=a['color'],
                     arrowprops=dict(arrowstyle='->', color=a['color'], lw=1.2))

    ax2.set_xlabel('Operational Intensity (FLOP/byte)', fontsize=13)
    ax2.set_ylabel('Compute Utilization (% of Peak)', fontsize=13)
    ax2.set_title('How Much Compute Is Actually Used?', fontsize=14,
                  fontweight='bold')
    ax2.legend(loc='upper left', fontsize=7.5)
    ax2.grid(True, which='both', alpha=0.15)
    ax2.set_xlim(1e-3, 300)
    ax2.set_ylim(0, 110)
    ax2.axhline(y=100, color='black', linestyle='-', alpha=0.15)

    # ---- Panel 3: Machine Balance Bar Chart ----
    names_short = [a['name'].replace('\n', ' ') for a in archs]
    balances = [a['bw'] / a['peak'] for a in archs]
    colors = [a['color'] for a in archs]

    y_pos = range(len(names_short))
    bars = ax3.barh(y_pos, balances, color=colors, edgecolor='white', height=0.55)
    ax3.set_yticks(list(y_pos))
    ax3.set_yticklabels(names_short, fontsize=9)
    ax3.set_xlabel('Machine Balance (Bytes / FLOP)', fontsize=13)
    ax3.set_title('Bandwidth per Unit of Compute', fontsize=14, fontweight='bold')
    ax3.set_xscale('log')
    ax3.grid(True, which='both', alpha=0.15, axis='x')

    for bar, bal in zip(bars, balances):
        ax3.text(bal * 1.4, bar.get_y() + bar.get_height()/2,
                 f'{bal:.2f} B/F', va='center', fontsize=9, fontweight='bold')

    ax3.text(0.95, 0.05,
             'Higher = more BW per FLOP\n= better for BW-bound kernels',
             transform=ax3.transAxes, fontsize=9, va='bottom', ha='right',
             bbox=dict(boxstyle='round,pad=0.3', facecolor='lightyellow', alpha=0.8))

    for ext in ['png', 'pdf']:
        path = f'{save_prefix}.{ext}'
        fig.savefig(path, dpi=150, bbox_inches='tight')
    print(f"Saved: {save_prefix}.png, {save_prefix}.pdf")
    plt.close(fig)

# =====================================================================
# Tables
# =====================================================================

def print_tables(archs, kernels):
    print("\n" + "=" * 85)
    print("MACHINE BALANCE COMPARISON")
    print("=" * 85)
    print(f"{'Architecture':<38} {'Peak':>10} {'BW':>10} {'B/FLOP':>10} {'Ridge':>10}")
    print(f"{'':38} {'(GFLOP/s)':>10} {'(GB/s)':>10} {'':>10} {'(F/B)':>10}")
    print("-" * 85)
    for a in archs:
        bal = a['bw'] / a['peak']
        ridge = a['peak'] / a['bw']
        print(f"{a['name']:<38} {a['peak']:>10,.0f} {a['bw']:>10,.0f}"
              f" {bal:>10.3f} {ridge:>10.2f}")

    print("\n" + "=" * 85)
    print("COMPUTE UTILIZATION AT KEY OPERATIONAL INTENSITIES")
    print("=" * 85)
    hdr = f"{'Architecture':<38}"
    for kname, koi in kernels:
        hdr += f"  {kname:>12}"
    print(hdr)
    hdr2 = f"{'':38}"
    for _, koi in kernels:
        hdr2 += f"  {'OI='+str(koi):>12}"
    print(hdr2)
    print("-" * 85)
    for a in archs:
        row = f"{a['name']:<38}"
        for _, koi in kernels:
            u = min(a['bw'] * koi / a['peak'], 1.0) * 100
            row += f"  {u:>11.1f}%"
        print(row)

    # Insight
    print("\n" + "=" * 85)
    print("KEY INSIGHT: WHY PANDO WINS FOR BANDWIDTH-BOUND KERNELS")
    print("=" * 85)
    # find pando with highest balance and A100 for comparison
    pando_best = max((a for a in archs if 'PANDO' in a['name']),
                     key=lambda a: a['bw']/a['peak'])
    a100 = next((a for a in archs if 'A100' in a['name']), None)
    h100 = next((a for a in archs if 'H100' in a['name']), None)
    xeon = next((a for a in archs if 'Xeon' in a['name']), None)

    pb = pando_best['bw'] / pando_best['peak']
    print(f"""
PANDOHammer Machine Balance: {pb:.2f} bytes/FLOP""")
    for ref, label in [(a100, 'A100'), (h100, 'H100'), (xeon, 'Xeon')]:
        if ref:
            rb = ref['bw'] / ref['peak']
            ratio = pb / rb
            print(f"  vs {label}: {rb:.3f} B/FLOP  -> PANDO has {ratio:.0f}x more BW per FLOP")

    print(f"""
For bandwidth-bound kernels (OI < ridge point), performance = BW x OI.
Every FLOP of compute provisioned needs bandwidth to feed it.

  GPUs/CPUs: Provision massive compute (SIMD, tensor cores, OoO, etc.)
             but only ~0.1-0.2 bytes/FLOP of bandwidth.
             -> >97% of compute sits IDLE for BW-bound kernels.

  PANDOHammer: Simple scalar in-order cores, but {pb:.1f} bytes/FLOP.
             -> Compute is well-matched to available bandwidth.
             -> Higher utilization, less wasted silicon/power.

Silicon that GPUs spend on unused ALUs, PANDOHammer spends on:
  * More memory banks (scalable bandwidth)
  * More simple cores (thread-level parallelism)
  * Near-memory scratchpads (low-latency, guaranteed BW)
""")

# =====================================================================
# Main
# =====================================================================

def main():
    parser = argparse.ArgumentParser(
        description='PANDOHammer Roofline: comparison with GPU and CPU')
    parser.add_argument('--cores', type=int, default=64,
                        help='Cores per pod (default: 64)')
    parser.add_argument('--l2sp-banks', type=int, default=64,
                        help='Number of L2SP banks (default: 64)')
    parser.add_argument('--dram-banks', type=int, default=1,
                        help='Number of DRAM banks (default: 1)')
    parser.add_argument('--bw-per-bank', type=float, default=8.0,
                        help='Bandwidth per bank in GB/s (default: 8.0)')
    parser.add_argument('--output', type=str, default='roofline_comparison',
                        help='Output filename prefix (default: roofline_comparison)')
    args = parser.parse_args()

    # Build architecture list
    pando = pando_config(cores=args.cores,
                         l2sp_banks=args.l2sp_banks,
                         dram_banks=args.dram_banks,
                         bw_per_bank_gbs=args.bw_per_bank)
    gpus_cpus = comparison_archs()
    all_archs = pando + gpus_cpus

    # Print tables
    print_tables(all_archs, KERNELS)

    # Plot
    plot_roofline(all_archs, KERNELS, save_prefix=args.output)

if __name__ == '__main__':
    main()
