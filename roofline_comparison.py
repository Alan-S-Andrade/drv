#!/usr/bin/env python3
"""
Clean roofline comparison: PANDOHammer vs GPU vs CPU.
Just architectural ceilings — no workload annotations.
"""
import os, argparse
import matplotlib
if os.environ.get('DISPLAY') is None:
    matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

ARCHS = [
    # (name, peak GFLOP/s, BW GB/s, color, linestyle, linewidth)
    ('PANDO 64c  (L2SP 64 banks)',   64,    512,  '#0055AA', '-',  3.0),
    ('PANDO 64c  (L2SP 1 bank)',     64,      8,  '#88BBDD', ':',  2.0),
    ('PANDO 64c  (DRAM 1 bank)',     64,      8,  '#5599CC', '--', 2.0),
    ('NVIDIA A100  (FP64, HBM2e)',  9700,  2039,  '#76B900', '-',  2.5),
    ('NVIDIA H100  (FP64, HBM3)',  33500,  3350,  '#FF6600', '-',  2.5),
    ('Intel Xeon 8380 (FP64, DDR4)', 2000,  205,  '#0071C5', '-',  2.5),
]

# Workload operational intensities (FLOP/byte)
WORKLOADS = [
    # (name, OI, marker, color)
    ('BFS',              0.03,  'X', '#CC0000'),
    ('PageRank',         0.10,  'P', '#AA00AA'),
    ('MoE All-to-All',   0.005, 'D', '#FF4444'),
    ('Dense GEMM',      16.0,   '*', '#228B22'),
]

oi = np.logspace(-3, 2.5, 3000)

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(18, 7))

# --- Panel 1: absolute roofline ---
for name, peak, bw, color, ls, lw in ARCHS:
    perf = np.minimum(bw * oi, peak)
    ax1.loglog(oi, perf, label=name, color=color, ls=ls, lw=lw)
    ridge = peak / bw
    ax1.plot(ridge, peak, 'o', color=color, ms=8, zorder=5)

# workload vertical lines
for wname, woi, wmarker, wcolor in WORKLOADS:
    ax1.axvline(x=woi, color=wcolor, ls='--', alpha=0.35, lw=1.2)
    ax1.text(woi, 5e4, wname, rotation=90, va='top', ha='right',
             fontsize=9, color=wcolor, fontweight='bold', alpha=0.8)

ax1.set_xlabel('Operational Intensity (FLOP/byte)', fontsize=13)
ax1.set_ylabel('Attainable Performance (GFLOP/s)', fontsize=13)
ax1.set_title('Roofline Model', fontsize=15, fontweight='bold')
ax1.legend(loc='upper left', fontsize=9)
ax1.grid(True, which='both', alpha=0.15)
ax1.set_xlim(1e-3, 300)
ax1.set_ylim(1e-2, 1e5)

# --- Panel 2: utilization ---
for name, peak, bw, color, ls, lw in ARCHS:
    util = np.minimum(bw * oi / peak, 1.0) * 100
    ax2.semilogx(oi, util, label=name, color=color, ls=ls, lw=lw)

# workload vertical lines + utilization dots for PANDO 64-bank
for wname, woi, wmarker, wcolor in WORKLOADS:
    ax2.axvline(x=woi, color=wcolor, ls='--', alpha=0.35, lw=1.2)
    ax2.text(woi, 107, wname, rotation=90, va='top', ha='right',
             fontsize=9, color=wcolor, fontweight='bold', alpha=0.8)

ax2.set_xlabel('Operational Intensity (FLOP/byte)', fontsize=13)
ax2.set_ylabel('Compute Utilization (% of Peak)', fontsize=13)
ax2.set_title('Compute Utilization', fontsize=15, fontweight='bold')
ax2.legend(loc='center right', fontsize=8)
ax2.grid(True, which='both', alpha=0.15)
ax2.set_xlim(1e-3, 300)
ax2.set_ylim(0, 115)

plt.tight_layout()
for ext in ['png', 'pdf']:
    fig.savefig(f'roofline.{ext}', dpi=150, bbox_inches='tight')
print('Saved roofline.png and roofline.pdf')
