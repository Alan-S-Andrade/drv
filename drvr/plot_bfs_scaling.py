#!/usr/bin/env python3
import argparse
import os
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from bfs_plot_utils import ensure_dir, parse_output_summary


CORE_COUNTS = [2, 4, 8, 16, 32, 64]

RUN_LAYOUTS = {
    "baseline": {
        2: "/users/alanandr/drv/baseline/drvr/drvr-run-bfs_nosteal_baseline",
        4: "/users/alanandr/drv/baseline_4c/drvr/drvr-run-bfs_nosteal_baseline",
        8: "/users/alanandr/drv/baseline_8c/drvr/drvr-run-bfs_nosteal_baseline",
        16: "/users/alanandr/drv/baseline_16c/drvr/drvr-run-bfs_nosteal_baseline",
        32: "/users/alanandr/drv/baselinle_32c/drvr/drvr-run-bfs_nosteal_baseline",
        64: "/users/alanandr/drv/baseline_64c/drvr/drvr-run-bfs_nosteal_baseline",
    },
    "adaptive": {
        2: "/users/alanandr/drv/build/drvr/drvr-run-bfs_work_stealing_adaptive",
        4: "/users/alanandr/drv/bfs_4c/drvr/drvr-run-bfs_work_stealing_adaptive",
        8: "/users/alanandr/drv/bfs_8c/drvr/drvr-run-bfs_work_stealing_adaptive",
        16: "/users/alanandr/drv/bfs_16c/drvr/drvr-run-bfs_work_stealing_adaptive",
        32: "/users/alanandr/drv/bfs_32c/drvr/drvr-run-bfs_work_stealing_adaptive",
        64: "/users/alanandr/drv/bfs_64c/drvr/drvr-run-bfs_work_stealing_adaptive",
    },
    "l1sp_cache": {
        2: "/users/alanandr/drv/build_l1sp/drvr/drvr-run-bfs_work_stealing_l1sp_cache",
        4: "/users/alanandr/drv/build_l1sp_4c/drvr/drvr-run-bfs_work_stealing_l1sp_cache",
        8: "/users/alanandr/drv/build_l1sp_8c/drvr/drvr-run-bfs_work_stealing_l1sp_cache",
        16: "/users/alanandr/drv/build_l1sp_16c/drvr/drvr-run-bfs_work_stealing_l1sp_cache",
        32: "/users/alanandr/drv/build_l1sp_32c/drvr/drvr-run-bfs_work_stealing_l1sp_cache",
        64: "/users/alanandr/drv/build_l1sp_64c/drvr/drvr-run-bfs_work_stealing_l1sp_cache",
    },
}


@dataclass
class ScalingPoint:
    variant: str
    cores: int
    run_dir: str
    cycles_elapsed: Optional[float]
    cycles_per_node: Optional[float]
    total_nodes_processed: Optional[float]
    nodes_discovered: Optional[float]
    complete: bool
    l1_max_occ_avg: Optional[float] = None
    l1_max_occ_max: Optional[float] = None
    l1_batch_avg: Optional[float] = None
    l1_probes_avg: Optional[float] = None


def _extract_discovered(summary) -> Optional[float]:
    raw = summary.summary_raw.get("Nodes discovered")
    if not raw:
        return None
    try:
        return float(raw.split("/", 1)[0].strip())
    except Exception:
        return None


def collect_scaling_points(variants: Sequence[str]) -> Dict[str, List[ScalingPoint]]:
    collected: Dict[str, List[ScalingPoint]] = {}
    for variant in variants:
        points: List[ScalingPoint] = []
        for cores in CORE_COUNTS:
            run_dir = RUN_LAYOUTS[variant].get(cores)
            if not run_dir:
                continue
            output_path = os.path.join(run_dir, "output.txt")
            if not os.path.exists(output_path):
                points.append(ScalingPoint(variant, cores, run_dir, None, None, None, None, False))
                continue
            summary = parse_output_summary(output_path)
            cycles_elapsed = summary.summary_numeric.get("Cycles elapsed")
            cycles_per_node = summary.summary_numeric.get("Cycles per node")
            total_nodes_processed = summary.summary_numeric.get("Total nodes processed")
            nodes_discovered = _extract_discovered(summary)
            complete = cycles_elapsed is not None
            l1_max_occ_avg = None
            l1_max_occ_max = None
            l1_batch_avg = None
            l1_probes_avg = None
            if summary.per_core_headers and summary.per_core_rows:
                header_index = {name: idx for idx, name in enumerate(summary.per_core_headers)}
                def avg_metric(name: str) -> Optional[float]:
                    idx = header_index.get(name)
                    if idx is None:
                        return None
                    values = [row[idx] for row in summary.per_core_rows]
                    return sum(values) / len(values) if values else None
                def max_metric(name: str) -> Optional[float]:
                    idx = header_index.get(name)
                    if idx is None:
                        return None
                    values = [row[idx] for row in summary.per_core_rows]
                    return float(max(values)) if values else None
                l1_max_occ_avg = avg_metric("Max Occ")
                l1_max_occ_max = max_metric("Max Occ")
                l1_batch_avg = avg_metric("Batch")
                l1_probes_avg = avg_metric("Probes")
            points.append(
                ScalingPoint(
                    variant=variant,
                    cores=cores,
                    run_dir=run_dir,
                    cycles_elapsed=cycles_elapsed,
                    cycles_per_node=cycles_per_node,
                    total_nodes_processed=total_nodes_processed,
                    nodes_discovered=nodes_discovered,
                    complete=complete,
                    l1_max_occ_avg=l1_max_occ_avg,
                    l1_max_occ_max=l1_max_occ_max,
                    l1_batch_avg=l1_batch_avg,
                    l1_probes_avg=l1_probes_avg,
                )
            )
        collected[variant] = points
    return collected


def plot_runtime_and_speedup(points_by_variant: Dict[str, List[ScalingPoint]], outdir: str) -> int:
    ensure_dir(outdir)
    colors = {
        "baseline": "#2563eb",
        "adaptive": "#dc2626",
        "l1sp_cache": "#059669",
    }
    labels = {
        "baseline": "baseline",
        "adaptive": "adaptive",
        "l1sp_cache": "l1sp_cache",
    }

    fig, axes = plt.subplots(2, 1, figsize=(12, 10))
    ax_runtime, ax_speedup = axes

    for variant, points in points_by_variant.items():
        valid = [point for point in points if point.complete and point.cycles_elapsed is not None]
        if not valid:
            continue
        x_vals = [point.cores for point in valid]
        runtimes = [point.cycles_elapsed for point in valid]
        ax_runtime.plot(x_vals, runtimes, marker="o", linewidth=2.2, color=colors[variant], label=labels[variant])

        baseline_2core = next((point.cycles_elapsed for point in points if point.cores == 2 and point.complete), None)
        if baseline_2core is not None:
            speedups = [baseline_2core / point.cycles_elapsed for point in valid]
            ax_speedup.plot(x_vals, speedups, marker="o", linewidth=2.2, color=colors[variant], label=labels[variant])

        missing = [point.cores for point in points if not point.complete]
        if missing:
            ax_runtime.scatter(missing, [np.nan] * len(missing), color=colors[variant], marker="x")

    ax_runtime.set_title("BFS runtime scaling by variant")
    ax_runtime.set_xlabel("Cores")
    ax_runtime.set_ylabel("Cycles elapsed")
    ax_runtime.set_xticks(CORE_COUNTS)
    ax_runtime.grid(True, alpha=0.25)
    ax_runtime.legend()

    ax_speedup.set_title("Speedup relative to the 2-core run")
    ax_speedup.set_xlabel("Cores")
    ax_speedup.set_ylabel("Speedup")
    ax_speedup.set_xticks(CORE_COUNTS)
    ax_speedup.grid(True, alpha=0.25)
    ax_speedup.legend()

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "bfs_scaling_runtime_speedup.png"), dpi=150)
    plt.close(fig)
    return 1


def plot_efficiency_and_cpn(points_by_variant: Dict[str, List[ScalingPoint]], outdir: str) -> int:
    ensure_dir(outdir)
    colors = {
        "baseline": "#2563eb",
        "adaptive": "#dc2626",
        "l1sp_cache": "#059669",
    }
    fig, axes = plt.subplots(2, 1, figsize=(12, 10))
    ax_eff, ax_cpn = axes

    for variant, points in points_by_variant.items():
        valid = [point for point in points if point.complete and point.cycles_elapsed is not None]
        if not valid:
            continue
        x_vals = [point.cores for point in valid]

        baseline_2core = next((point.cycles_elapsed for point in points if point.cores == 2 and point.complete), None)
        if baseline_2core is not None:
            efficiency = [(baseline_2core / point.cycles_elapsed) / (point.cores / 2.0) for point in valid]
            ax_eff.plot(x_vals, efficiency, marker="o", linewidth=2.2, color=colors[variant], label=variant)

        cpn_valid = [point for point in valid if point.cycles_per_node is not None]
        if cpn_valid:
            ax_cpn.plot(
                [point.cores for point in cpn_valid],
                [point.cycles_per_node for point in cpn_valid],
                marker="o",
                linewidth=2.2,
                color=colors[variant],
                label=variant,
            )

    ax_eff.set_title("Parallel efficiency relative to the 2-core run")
    ax_eff.set_xlabel("Cores")
    ax_eff.set_ylabel("Efficiency")
    ax_eff.set_xticks(CORE_COUNTS)
    ax_eff.grid(True, alpha=0.25)
    ax_eff.legend()

    ax_cpn.set_title("Cycles per node by core count")
    ax_cpn.set_xlabel("Cores")
    ax_cpn.set_ylabel("Cycles per node")
    ax_cpn.set_xticks(CORE_COUNTS)
    ax_cpn.grid(True, alpha=0.25)
    ax_cpn.legend()

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "bfs_scaling_efficiency_cpn.png"), dpi=150)
    plt.close(fig)
    return 1


def write_scaling_csv(points_by_variant: Dict[str, List[ScalingPoint]], outdir: str) -> int:
    ensure_dir(outdir)
    path = os.path.join(outdir, "bfs_scaling_summary.csv")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(
            "variant,cores,complete,cycles_elapsed,cycles_per_node,total_nodes_processed,"
            "nodes_discovered,l1_max_occ_avg,l1_max_occ_max,l1_batch_avg,l1_probes_avg,run_dir\n"
        )
        for variant, points in points_by_variant.items():
            for point in points:
                handle.write(
                    f"{variant},{point.cores},{int(point.complete)},"
                    f"{'' if point.cycles_elapsed is None else point.cycles_elapsed},"
                    f"{'' if point.cycles_per_node is None else point.cycles_per_node},"
                    f"{'' if point.total_nodes_processed is None else point.total_nodes_processed},"
                    f"{'' if point.nodes_discovered is None else point.nodes_discovered},"
                    f"{'' if point.l1_max_occ_avg is None else point.l1_max_occ_avg},"
                    f"{'' if point.l1_max_occ_max is None else point.l1_max_occ_max},"
                    f"{'' if point.l1_batch_avg is None else point.l1_batch_avg},"
                    f"{'' if point.l1_probes_avg is None else point.l1_probes_avg},"
                    f"{point.run_dir}\n"
                )
    return 1


def plot_l1_cache_scaling(points_by_variant: Dict[str, List[ScalingPoint]], outdir: str) -> int:
    ensure_dir(outdir)
    points = points_by_variant.get("l1sp_cache", [])
    valid = [point for point in points if point.complete and point.l1_max_occ_avg is not None]
    if not valid:
        return 0

    fig, axes = plt.subplots(3, 1, figsize=(12, 12))
    ax_occ, ax_batch, ax_probes = axes

    x_vals = [point.cores for point in valid]
    ax_occ.plot(x_vals, [point.l1_max_occ_avg for point in valid], marker="o", linewidth=2.2, label="Avg Max Occ")
    ax_occ.plot(x_vals, [point.l1_max_occ_max for point in valid], marker="s", linewidth=2.2, label="Max Max Occ")
    ax_occ.set_title("L1 cache occupancy scaling for l1sp_cache")
    ax_occ.set_xlabel("Cores")
    ax_occ.set_ylabel("Occupancy")
    ax_occ.set_xticks(CORE_COUNTS)
    ax_occ.grid(True, alpha=0.25)
    ax_occ.legend()

    batch_valid = [point for point in valid if point.l1_batch_avg is not None]
    ax_batch.plot([point.cores for point in batch_valid], [point.l1_batch_avg for point in batch_valid],
                  marker="o", linewidth=2.2, color="#2563eb")
    ax_batch.set_title("Average adaptive batch size")
    ax_batch.set_xlabel("Cores")
    ax_batch.set_ylabel("Batch")
    ax_batch.set_xticks(CORE_COUNTS)
    ax_batch.grid(True, alpha=0.25)

    probes_valid = [point for point in valid if point.l1_probes_avg is not None]
    ax_probes.plot([point.cores for point in probes_valid], [point.l1_probes_avg for point in probes_valid],
                   marker="o", linewidth=2.2, color="#059669")
    ax_probes.set_title("Average probe count")
    ax_probes.set_xlabel("Cores")
    ax_probes.set_ylabel("Probes")
    ax_probes.set_xticks(CORE_COUNTS)
    ax_probes.grid(True, alpha=0.25)

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "bfs_scaling_l1_cache_controls.png"), dpi=150)
    plt.close(fig)
    return 1


def plot_runtime_grouped_bars(points_by_variant: Dict[str, List[ScalingPoint]], outdir: str) -> int:
    ensure_dir(outdir)
    variants = ["baseline", "adaptive", "l1sp_cache"]
    colors = {
        "baseline": "#2563eb",
        "adaptive": "#dc2626",
        "l1sp_cache": "#059669",
    }

    available_cores = [
        core for core in CORE_COUNTS
        if any(
            point.cores == core and point.complete and point.cycles_elapsed is not None
            for variant in variants
            for point in points_by_variant.get(variant, [])
        )
    ]
    if not available_cores:
        return 0

    fig, ax = plt.subplots(figsize=(13, 7))
    x = np.arange(len(available_cores), dtype=float)
    width = 0.24

    for idx, variant in enumerate(variants):
        heights = []
        for core in available_cores:
            point = next((p for p in points_by_variant.get(variant, []) if p.cores == core), None)
            heights.append(point.cycles_elapsed if point and point.complete and point.cycles_elapsed is not None else np.nan)
        offset = (idx - 1) * width
        bars = ax.bar(x + offset, heights, width=width, color=colors[variant], label=variant)
        for bar, height in zip(bars, heights):
            if np.isnan(height):
                ax.text(bar.get_x() + bar.get_width() / 2.0, 0.0, "missing",
                        rotation=90, ha="center", va="bottom", fontsize=8, color="0.4")

    ax.set_title("Execution time by BFS variant for the same core count")
    ax.set_xlabel("Cores")
    ax.set_ylabel("Cycles elapsed")
    ax.set_xticks(x)
    ax.set_xticklabels([str(core) for core in available_cores])
    ax.grid(True, axis="y", alpha=0.25)
    ax.legend()

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "bfs_runtime_grouped_by_cores.png"), dpi=150)
    plt.close(fig)
    return 1


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot BFS runtime scaling and speedup for baseline, adaptive, and l1sp_cache.")
    parser.add_argument("--outdir", default="bfs_plots/scaling", help="Output directory")
    parser.add_argument(
        "--variant",
        action="append",
        choices=sorted(RUN_LAYOUTS),
        help="Optional variant filter. Default: all variants.",
    )
    args = parser.parse_args()

    variants = args.variant if args.variant else list(RUN_LAYOUTS)
    points_by_variant = collect_scaling_points(variants)

    plot_count = 0
    plot_count += write_scaling_csv(points_by_variant, args.outdir)
    plot_count += plot_runtime_and_speedup(points_by_variant, args.outdir)
    plot_count += plot_efficiency_and_cpn(points_by_variant, args.outdir)
    plot_count += plot_l1_cache_scaling(points_by_variant, args.outdir)
    plot_count += plot_runtime_grouped_bars(points_by_variant, args.outdir)

    for variant, points in points_by_variant.items():
        complete = [point.cores for point in points if point.complete]
        missing = [point.cores for point in points if not point.complete]
        print(f"{variant}: complete={complete} incomplete={missing}")
    print(f"Generated {plot_count} scaling artifact(s) in {args.outdir}")


if __name__ == "__main__":
    main()
