#!/usr/bin/env python3
"""
PANDOHammer Roofline Model Generator

This script generates roofline models for different hierarchy levels:
- Single thread → L1SP
- Single core → L1SP
- Pod → L2SP
- PXN → DRAM

Based on parameters from model/pandohammer.py

Usage:
    # Generate plots with default config (single core)
    python roofline_model.py --output-prefix roofline --no-show

    # Multi-core pod configuration
    python roofline_model.py --cores-per-pod 4 --pods-per-pxn 2 --output-prefix roofline --no-show

    # Just print the summary
    python roofline_model.py --summary-only
"""

import os
import argparse

# Use non-interactive backend if no display is available (for cluster/headless use)
import matplotlib
if os.environ.get('DISPLAY') is None:
    matplotlib.use('Agg')

import matplotlib.pyplot as plt
import numpy as np


class PANDOHammerConfig:
    """Configuration class holding PANDOHammer architecture parameters."""

    def __init__(
        self,
        clock_ghz: float = 1.0,
        threads_per_core: int = 16,
        cores_per_pod: int = 1,
        pods_per_pxn: int = 1,
        num_pxn: int = 1,
        # Bandwidth in GB/s (from pandohammer.py: 24e9 bytes/s = 24 GB/s)
        bw_per_core_gbs: float = 24.0,
        # Memory sizes
        l1sp_size_kb: int = 128,
        l2sp_size_mb: int = 1,
        dram_size_gb: int = 2,
        # Access latencies in ns
        l1sp_latency_ns: float = 1.0,
        l2sp_latency_ns: float = 4.0,
        dram_latency_ns: float = 70.0,
        # FP operations per cycle per thread (1 for simple, 2 for FMA)
        fp_ops_per_cycle: int = 1,
    ):
        self.clock_ghz = clock_ghz
        self.threads_per_core = threads_per_core
        self.cores_per_pod = cores_per_pod
        self.pods_per_pxn = pods_per_pxn
        self.num_pxn = num_pxn
        self.bw_per_core_gbs = bw_per_core_gbs
        self.l1sp_size_kb = l1sp_size_kb
        self.l2sp_size_mb = l2sp_size_mb
        self.dram_size_gb = dram_size_gb
        self.l1sp_latency_ns = l1sp_latency_ns
        self.l2sp_latency_ns = l2sp_latency_ns
        self.dram_latency_ns = dram_latency_ns
        self.fp_ops_per_cycle = fp_ops_per_cycle

    def __repr__(self):
        return (
            f"PANDOHammerConfig(\n"
            f"  clock={self.clock_ghz} GHz,\n"
            f"  threads_per_core={self.threads_per_core},\n"
            f"  cores_per_pod={self.cores_per_pod},\n"
            f"  pods_per_pxn={self.pods_per_pxn},\n"
            f"  bw_per_core={self.bw_per_core_gbs} GB/s,\n"
            f"  L1SP: {self.l1sp_size_kb} KB, {self.l1sp_latency_ns} ns,\n"
            f"  L2SP: {self.l2sp_size_mb} MB, {self.l2sp_latency_ns} ns,\n"
            f"  DRAM: {self.dram_size_gb} GB, {self.dram_latency_ns} ns\n"
            f")"
        )


class RooflineModel:
    """Roofline model calculator for PANDOHammer architecture."""

    def __init__(self, config: PANDOHammerConfig):
        self.config = config
        self._calculate_peaks()

    def _calculate_peaks(self):
        """Calculate peak performance and bandwidth at each level."""
        cfg = self.config

        # Peak compute (GFLOP/s) at each level
        # Single thread: clock * ops_per_cycle
        self.peak_thread_gflops = cfg.clock_ghz * cfg.fp_ops_per_cycle

        # Single core: all threads
        self.peak_core_gflops = self.peak_thread_gflops * cfg.threads_per_core

        # Pod: all cores in pod
        self.peak_pod_gflops = self.peak_core_gflops * cfg.cores_per_pod

        # PXN: all pods in PXN
        self.peak_pxn_gflops = self.peak_pod_gflops * cfg.pods_per_pxn

        # Peak bandwidth (GB/s) at each level
        # Thread to L1SP: share of core's bandwidth (simplified model)
        # In reality, threads share the same L1SP, so effective BW depends on contention
        self.bw_thread_l1sp = cfg.bw_per_core_gbs / cfg.threads_per_core

        # Core to L1SP: full core bandwidth
        self.bw_core_l1sp = cfg.bw_per_core_gbs

        # Pod to L2SP: aggregate of all cores
        self.bw_pod_l2sp = cfg.bw_per_core_gbs * cfg.cores_per_pod

        # PXN to DRAM: aggregate of all pods
        self.bw_pxn_dram = cfg.bw_per_core_gbs * cfg.cores_per_pod * cfg.pods_per_pxn

        # Calculate ridge points (FLOP/Byte)
        self.ridge_thread_l1sp = self.peak_thread_gflops / self.bw_thread_l1sp
        self.ridge_core_l1sp = self.peak_core_gflops / self.bw_core_l1sp
        self.ridge_pod_l2sp = self.peak_pod_gflops / self.bw_pod_l2sp
        self.ridge_pxn_dram = self.peak_pxn_gflops / self.bw_pxn_dram

    def roofline(self, ai: np.ndarray, peak_gflops: float, bandwidth_gbs: float) -> np.ndarray:
        """
        Calculate roofline performance for given arithmetic intensity.

        Args:
            ai: Arithmetic intensity array (FLOP/Byte)
            peak_gflops: Peak compute performance (GFLOP/s)
            bandwidth_gbs: Peak memory bandwidth (GB/s)

        Returns:
            Attainable performance (GFLOP/s)
        """
        return np.minimum(peak_gflops, ai * bandwidth_gbs)

    def get_all_rooflines(self):
        """Return a dictionary of all roofline parameters."""
        return {
            'thread_l1sp': {
                'peak_gflops': self.peak_thread_gflops,
                'bandwidth_gbs': self.bw_thread_l1sp,
                'ridge_point': self.ridge_thread_l1sp,
                'label': 'Single Thread → L1SP',
                'color': 'blue',
                'linestyle': '-',
            },
            'core_l1sp': {
                'peak_gflops': self.peak_core_gflops,
                'bandwidth_gbs': self.bw_core_l1sp,
                'ridge_point': self.ridge_core_l1sp,
                'label': 'Single Core → L1SP',
                'color': 'green',
                'linestyle': '-',
            },
            'pod_l2sp': {
                'peak_gflops': self.peak_pod_gflops,
                'bandwidth_gbs': self.bw_pod_l2sp,
                'ridge_point': self.ridge_pod_l2sp,
                'label': 'Pod → L2SP',
                'color': 'orange',
                'linestyle': '-',
            },
            'pxn_dram': {
                'peak_gflops': self.peak_pxn_gflops,
                'bandwidth_gbs': self.bw_pxn_dram,
                'ridge_point': self.ridge_pxn_dram,
                'label': 'PXN → DRAM',
                'color': 'red',
                'linestyle': '-',
            },
        }

    def print_summary(self):
        """Print a summary of all roofline parameters."""
        print("=" * 70)
        print("PANDOHammer Roofline Model Summary")
        print("=" * 70)
        print(f"\nConfiguration:")
        print(f"  Clock: {self.config.clock_ghz} GHz")
        print(f"  Threads/Core: {self.config.threads_per_core}")
        print(f"  Cores/Pod: {self.config.cores_per_pod}")
        print(f"  Pods/PXN: {self.config.pods_per_pxn}")
        print(f"  BW/Core: {self.config.bw_per_core_gbs} GB/s")
        print(f"  FP ops/cycle: {self.config.fp_ops_per_cycle}")

        print(f"\n{'Level':<25} {'Peak GFLOP/s':>15} {'BW (GB/s)':>12} {'Ridge (F/B)':>12}")
        print("-" * 70)

        rooflines = self.get_all_rooflines()
        for name, params in rooflines.items():
            print(f"{params['label']:<25} {params['peak_gflops']:>15.2f} "
                  f"{params['bandwidth_gbs']:>12.2f} {params['ridge_point']:>12.4f}")

        print("=" * 70)


def plot_combined_roofline(model: RooflineModel, output_file: str = None,
                           show_plot: bool = True, application_points: dict = None):
    """
    Plot all rooflines on a single figure.

    Args:
        model: RooflineModel instance
        output_file: Path to save the plot (optional)
        show_plot: Whether to display the plot
        application_points: Dict of {name: (ai, measured_gflops)} for application markers
    """
    fig, ax = plt.subplots(figsize=(12, 8))

    # Arithmetic intensity range
    ai = np.logspace(-3, 3, 500)

    rooflines = model.get_all_rooflines()

    for name, params in rooflines.items():
        perf = model.roofline(ai, params['peak_gflops'], params['bandwidth_gbs'])
        ax.loglog(ai, perf,
                  color=params['color'],
                  linestyle=params['linestyle'],
                  linewidth=2.5,
                  label=f"{params['label']} (Peak={params['peak_gflops']:.1f} GFLOP/s, BW={params['bandwidth_gbs']:.1f} GB/s)")

        # Mark ridge point
        ax.axvline(x=params['ridge_point'], color=params['color'],
                   linestyle=':', alpha=0.5, linewidth=1)
        ax.plot(params['ridge_point'], params['peak_gflops'],
                marker='o', color=params['color'], markersize=8)

    # Plot application points if provided
    if application_points:
        for app_name, (app_ai, app_perf) in application_points.items():
            ax.plot(app_ai, app_perf, marker='*', markersize=15,
                    label=f'{app_name} (AI={app_ai:.3f})')

    ax.set_xlabel('Arithmetic Intensity (FLOP/Byte)', fontsize=12)
    ax.set_ylabel('Performance (GFLOP/s)', fontsize=12)
    ax.set_title('PANDOHammer Roofline Model - All Hierarchy Levels', fontsize=14)
    ax.legend(loc='lower right', fontsize=9)
    ax.grid(True, which='both', alpha=0.3)
    ax.set_xlim([1e-3, 1e3])

    # Set y-axis limits based on peak performance
    max_peak = max(p['peak_gflops'] for p in rooflines.values())
    ax.set_ylim([1e-3, max_peak * 2])

    plt.tight_layout()

    if output_file:
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"Saved combined roofline to: {output_file}")

    if show_plot:
        plt.show()

    return fig, ax


def plot_separate_rooflines(model: RooflineModel, output_file: str = None,
                            show_plot: bool = True):
    """
    Plot each roofline level on a separate subplot.

    Args:
        model: RooflineModel instance
        output_file: Path to save the plot (optional)
        show_plot: Whether to display the plot
    """
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    axes = axes.flatten()

    ai = np.logspace(-3, 3, 500)
    rooflines = model.get_all_rooflines()

    for idx, (name, params) in enumerate(rooflines.items()):
        ax = axes[idx]

        # Calculate roofline
        perf = model.roofline(ai, params['peak_gflops'], params['bandwidth_gbs'])

        # Plot roofline
        ax.loglog(ai, perf, color=params['color'], linewidth=2.5)

        # Plot memory-bound slope
        mem_bound = ai * params['bandwidth_gbs']
        ax.loglog(ai[mem_bound < params['peak_gflops']],
                  mem_bound[mem_bound < params['peak_gflops']],
                  color=params['color'], linestyle='--', alpha=0.5, linewidth=1.5,
                  label=f"Memory-bound: {params['bandwidth_gbs']:.1f} GB/s")

        # Plot compute-bound ceiling
        ax.axhline(y=params['peak_gflops'], color=params['color'],
                   linestyle=':', alpha=0.5, linewidth=1.5,
                   label=f"Compute-bound: {params['peak_gflops']:.1f} GFLOP/s")

        # Mark ridge point
        ax.axvline(x=params['ridge_point'], color='gray',
                   linestyle='--', alpha=0.7, linewidth=1)
        ax.plot(params['ridge_point'], params['peak_gflops'],
                marker='o', color=params['color'], markersize=10)
        ax.annotate(f"Ridge: {params['ridge_point']:.3f} F/B",
                    xy=(params['ridge_point'], params['peak_gflops']),
                    xytext=(params['ridge_point'] * 2, params['peak_gflops'] * 0.5),
                    fontsize=9, arrowprops=dict(arrowstyle='->', color='gray'))

        # Shade regions
        ax.fill_between(ai[ai < params['ridge_point']],
                        1e-4, perf[ai < params['ridge_point']],
                        alpha=0.1, color='blue', label='Memory-bound region')
        ax.fill_between(ai[ai >= params['ridge_point']],
                        1e-4, perf[ai >= params['ridge_point']],
                        alpha=0.1, color='green', label='Compute-bound region')

        ax.set_xlabel('Arithmetic Intensity (FLOP/Byte)', fontsize=10)
        ax.set_ylabel('Performance (GFLOP/s)', fontsize=10)
        ax.set_title(params['label'], fontsize=12, fontweight='bold')
        ax.legend(loc='lower right', fontsize=8)
        ax.grid(True, which='both', alpha=0.3)
        ax.set_xlim([1e-3, 1e3])
        ax.set_ylim([1e-3, params['peak_gflops'] * 2])

    plt.suptitle('PANDOHammer Roofline Model - Hierarchy Levels', fontsize=14, fontweight='bold')
    plt.tight_layout()

    if output_file:
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"Saved separate rooflines to: {output_file}")

    if show_plot:
        plt.show()

    return fig, axes


def plot_with_ceilings(model: RooflineModel, output_file: str = None,
                       show_plot: bool = True):
    """
    Plot rooflines with additional performance ceilings showing
    the effect of various bottlenecks.

    Args:
        model: RooflineModel instance
        output_file: Path to save the plot (optional)
        show_plot: Whether to display the plot
    """
    fig, ax = plt.subplots(figsize=(14, 9))

    ai = np.logspace(-3, 3, 500)

    # Get peak values
    peak_pxn = model.peak_pxn_gflops
    peak_pod = model.peak_pod_gflops
    peak_core = model.peak_core_gflops
    peak_thread = model.peak_thread_gflops

    # Memory bandwidths
    bw_dram = model.bw_pxn_dram
    bw_l2sp = model.bw_pod_l2sp
    bw_l1sp = model.bw_core_l1sp

    # Plot compute ceilings (horizontal lines)
    ceilings = [
        (peak_pxn, 'PXN Peak', 'red', '-'),
        (peak_pod, 'Pod Peak', 'orange', '--'),
        (peak_core, 'Core Peak', 'green', '-.'),
        (peak_thread, 'Thread Peak', 'blue', ':'),
    ]

    for peak, label, color, ls in ceilings:
        ax.axhline(y=peak, color=color, linestyle=ls, linewidth=2,
                   label=f'{label}: {peak:.1f} GFLOP/s')

    # Plot memory bandwidth slopes
    bandwidths = [
        (bw_dram, 'DRAM BW', 'red'),
        (bw_l2sp, 'L2SP BW', 'orange'),
        (bw_l1sp, 'L1SP BW', 'green'),
    ]

    for bw, label, color in bandwidths:
        mem_perf = ai * bw
        # Only plot where it's below the max ceiling
        mask = mem_perf < peak_pxn * 1.5
        ax.loglog(ai[mask], mem_perf[mask], color=color, linestyle='-',
                  linewidth=1.5, alpha=0.7,
                  label=f'{label}: {bw:.1f} GB/s')

    # Plot the actual composite roofline for PXN→DRAM
    perf = model.roofline(ai, peak_pxn, bw_dram)
    ax.loglog(ai, perf, color='darkred', linewidth=3,
              label='PXN Roofline (composite)')

    ax.set_xlabel('Arithmetic Intensity (FLOP/Byte)', fontsize=12)
    ax.set_ylabel('Performance (GFLOP/s)', fontsize=12)
    ax.set_title('PANDOHammer Roofline with Performance Ceilings', fontsize=14)
    ax.legend(loc='lower right', fontsize=9, ncol=2)
    ax.grid(True, which='both', alpha=0.3)
    ax.set_xlim([1e-3, 1e3])
    ax.set_ylim([1e-2, peak_pxn * 3])

    # Add annotations for common workloads
    workloads = [
        (0.01, 'Sparse/Graph\n(BFS, PageRank)'),
        (0.1, 'Streaming\n(STREAM)'),
        (1.0, 'Dense LA\n(SpMV)'),
        (10.0, 'BLAS-3\n(GEMM)'),
        (100.0, 'Compute\nIntensive'),
    ]

    for ai_val, label in workloads:
        ax.axvline(x=ai_val, color='gray', linestyle=':', alpha=0.3)
        ax.text(ai_val, peak_pxn * 2, label, ha='center', fontsize=8,
                rotation=0, va='bottom')

    plt.tight_layout()

    if output_file:
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"Saved roofline with ceilings to: {output_file}")

    if show_plot:
        plt.show()

    return fig, ax


def calculate_application_ai(flops: float, bytes_transferred: float) -> float:
    """
    Calculate arithmetic intensity for an application.

    Args:
        flops: Total floating point operations
        bytes_transferred: Total bytes read/written from memory

    Returns:
        Arithmetic intensity (FLOP/Byte)
    """
    return flops / bytes_transferred


def example_applications():
    """Return example application arithmetic intensities."""
    return {
        # BFS: mostly integer, very few FLOPs, lots of random memory access
        'BFS': (0.01, 'Graph traversal - memory bound'),
        # STREAM: simple copy/scale operations
        'STREAM Copy': (0.125, '1 load + 1 store per 8B = 1/8 FLOP/B (if counting as FLOP)'),
        # SpMV: y = A*x where A is sparse
        'SpMV': (0.25, 'Sparse matrix-vector multiply'),
        # Dense GEMM: C = A*B with good blocking
        'GEMM (blocked)': (10.0, 'Dense matrix multiply with cache blocking'),
        # FFT
        'FFT': (1.5, 'Fast Fourier Transform'),
        # Stencil
        '3D Stencil': (0.5, '3D stencil computation'),
    }


def main():
    parser = argparse.ArgumentParser(
        description='PANDOHammer Roofline Model Generator',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Default single-core configuration
  python roofline_model.py

  # 4-core pod configuration
  python roofline_model.py --cores-per-pod 4

  # Full PXN with 4 pods of 8 cores each
  python roofline_model.py --cores-per-pod 8 --pods-per-pxn 4

  # Save plots to files
  python roofline_model.py --output-prefix my_roofline --no-show
        """
    )

    # Architecture parameters
    parser.add_argument('--clock-ghz', type=float, default=1.0,
                        help='Clock frequency in GHz (default: 1.0)')
    parser.add_argument('--threads-per-core', type=int, default=16,
                        help='Threads per core (default: 16)')
    parser.add_argument('--cores-per-pod', type=int, default=1,
                        help='Cores per pod (default: 1)')
    parser.add_argument('--pods-per-pxn', type=int, default=1,
                        help='Pods per PXN (default: 1)')
    parser.add_argument('--bw-per-core', type=float, default=24.0,
                        help='Bandwidth per core in GB/s (default: 24.0)')
    parser.add_argument('--fp-ops-per-cycle', type=int, default=1,
                        help='FP ops per cycle per thread (default: 1, use 2 for FMA)')

    # Output options
    parser.add_argument('--output-prefix', type=str, default=None,
                        help='Prefix for output files (generates _combined.png, _separate.png, _ceilings.png)')
    parser.add_argument('--no-show', action='store_true',
                        help='Do not display plots (useful for batch processing)')
    parser.add_argument('--summary-only', action='store_true',
                        help='Only print summary, do not generate plots')

    args = parser.parse_args()

    # Create configuration
    config = PANDOHammerConfig(
        clock_ghz=args.clock_ghz,
        threads_per_core=args.threads_per_core,
        cores_per_pod=args.cores_per_pod,
        pods_per_pxn=args.pods_per_pxn,
        bw_per_core_gbs=args.bw_per_core,
        fp_ops_per_cycle=args.fp_ops_per_cycle,
    )

    # Create roofline model
    model = RooflineModel(config)

    # Print summary
    model.print_summary()

    # Print example applications
    print("\nExample Application Arithmetic Intensities:")
    print("-" * 50)
    for name, (ai, desc) in example_applications().items():
        print(f"  {name:<15}: AI = {ai:.3f} FLOP/Byte")
        print(f"                  {desc}")
    print()

    if args.summary_only:
        return

    show = not args.no_show

    # Generate plots
    if args.output_prefix:
        plot_combined_roofline(model, f"{args.output_prefix}_combined.png", show)
        plot_separate_rooflines(model, f"{args.output_prefix}_separate.png", show)
        plot_with_ceilings(model, f"{args.output_prefix}_ceilings.png", show)
    else:
        plot_combined_roofline(model, None, show)
        plot_separate_rooflines(model, None, show)
        plot_with_ceilings(model, None, show)


if __name__ == '__main__':
    main()
