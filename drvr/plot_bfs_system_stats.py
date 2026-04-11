#!/usr/bin/env python3
import argparse
import math
import os
import sys
from typing import Dict, List, Sequence

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from bfs_plot_utils import (
    chunked_metrics,
    discover_run_dirs,
    ensure_dir,
    parse_l2sp_timestamps,
    parse_ramulator_stats,
    parse_stats_csv,
    sanitize_filename,
)


SELECTED_CORE_STATS = [
    "busy_cycles",
    "stall_cycles",
    "memory_wait_cycles",
    "load_l1sp",
    "store_l1sp",
    "atomic_l1sp",
    "load_l2sp",
    "store_l2sp",
    "atomic_l2sp",
    "load_dram",
    "store_dram",
    "atomic_dram",
    "useful_load_l1sp",
    "useful_store_l1sp",
    "useful_load_l2sp",
    "useful_store_l2sp",
    "useful_load_dram",
    "useful_store_dram",
]


def _save(fig: plt.Figure, out_path: str) -> None:
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def plot_component_heatmap(run_dir: str, outdir: str) -> int:
    path = os.path.join(run_dir, "stats.csv")
    if not os.path.exists(path):
        return 0

    stats = parse_stats_csv(path)
    if not stats.components or not stats.stat_names:
        return 0
    matrix = np.array(
        [
            [math.log10(stats.component_stat_values.get(component, {}).get(stat_name, 0.0) + 1.0)
             for component in stats.components]
            for stat_name in stats.stat_names
        ],
        dtype=float,
    )

    fig, ax = plt.subplots(figsize=(max(12, 0.55 * len(stats.components)), 18))
    im = ax.imshow(matrix, origin="lower", aspect="auto", cmap="magma")
    ax.set_title("All stats.csv counters by component")
    ax.set_xlabel("Component")
    ax.set_ylabel("Statistic name")
    ax.set_xticks(range(len(stats.components)))
    ax.set_xticklabels(stats.components, rotation=45, ha="right", fontsize=8)
    stride = max(1, len(stats.stat_names) // 40)
    tick_idx = list(range(0, len(stats.stat_names), stride))
    ax.set_yticks(tick_idx)
    ax.set_yticklabels([stats.stat_names[idx] for idx in tick_idx], fontsize=7)
    fig.colorbar(im, ax=ax, fraction=0.02, pad=0.02, label="log10(sum + 1)")
    _save(fig, os.path.join(outdir, "stats_component_heatmap.png"))
    return 1


def plot_top_stats(run_dir: str, outdir: str, top_n: int) -> int:
    path = os.path.join(run_dir, "stats.csv")
    if not os.path.exists(path):
        return 0

    stats = parse_stats_csv(path)
    if not stats.stat_totals:
        return 0
    items = sorted(stats.stat_totals.items(), key=lambda item: item[1], reverse=True)[:top_n]
    labels = [item[0] for item in items]
    values = [item[1] for item in items]

    fig, ax = plt.subplots(figsize=(15, max(6, 0.35 * len(labels))))
    positions = np.arange(len(labels))
    ax.barh(positions, values, color="#8b5cf6")
    ax.set_yticks(positions)
    ax.set_yticklabels(labels, fontsize=8)
    ax.invert_yaxis()
    ax.set_xlabel("Total Sum.u64 across all components/subids")
    ax.set_title(f"Top {len(labels)} stats.csv counters")
    ax.grid(True, axis="x", alpha=0.25)
    _save(fig, os.path.join(outdir, "stats_top_totals.png"))
    return 1


def plot_core_selected_stats(run_dir: str, outdir: str) -> int:
    path = os.path.join(run_dir, "stats.csv")
    if not os.path.exists(path):
        return 0

    stats = parse_stats_csv(path)
    if not stats.core_stat_values or not stats.stat_totals:
        return 0
    cores = sorted(stats.core_stat_values)
    if not cores:
        return 0

    metrics = [name for name in SELECTED_CORE_STATS if name in stats.stat_totals]
    if not metrics:
        return 0

    matrix = np.array(
        [[math.log10(stats.core_stat_values[core].get(metric, 0.0) + 1.0) for core in cores] for metric in metrics],
        dtype=float,
    )

    fig, ax = plt.subplots(figsize=(max(10, 0.8 * len(cores)), max(8, 0.45 * len(metrics))))
    im = ax.imshow(matrix, origin="lower", aspect="auto", cmap="viridis")
    ax.set_title("Selected core-local counters from stats.csv")
    ax.set_xlabel("Core")
    ax.set_ylabel("Statistic name")
    ax.set_xticks(range(len(cores)))
    ax.set_xticklabels([str(core) for core in cores])
    ax.set_yticks(range(len(metrics)))
    ax.set_yticklabels(metrics, fontsize=8)
    fig.colorbar(im, ax=ax, fraction=0.02, pad=0.02, label="log10(sum + 1)")
    _save(fig, os.path.join(outdir, "stats_selected_core_heatmap.png"))
    return 1


def plot_l2sp_timestamps(run_dir: str, outdir: str) -> int:
    series = parse_l2sp_timestamps(run_dir)
    if not series:
        return 0

    ensure_dir(outdir)
    cores = sorted(series)
    counts: List[int] = []
    max_rel_cycle = 0.0
    rel_series: Dict[int, np.ndarray] = {}
    for core in cores:
        values = np.array(series[core], dtype=float)
        counts.append(int(values.size))
        if values.size == 0:
            rel = values
        else:
            rel = values - values.min()
            max_rel_cycle = max(max_rel_cycle, float(rel.max()))
        rel_series[core] = rel

    num_bins = 128
    bins = np.linspace(0.0, max_rel_cycle if max_rel_cycle > 0.0 else 1.0, num_bins + 1)
    heat_rows: List[np.ndarray] = []
    for core in cores:
        hist, _edges = np.histogram(rel_series[core], bins=bins)
        heat_rows.append(hist)

    fig, axes = plt.subplots(2, 1, figsize=(16, 10))
    ax0, ax1 = axes
    heat = np.array(heat_rows, dtype=float)
    im = ax0.imshow(heat, origin="lower", aspect="auto", cmap="cividis")
    ax0.set_title("Binned L2SP timestamp density by core")
    ax0.set_xlabel("Relative-cycle bin")
    ax0.set_ylabel("Core")
    ax0.set_yticks(range(len(cores)))
    ax0.set_yticklabels([str(core) for core in cores])
    fig.colorbar(im, ax=ax0, fraction=0.02, pad=0.02, label="Events per bin")

    ax1.bar(cores, counts, color="#0f766e")
    ax1.set_title("Total L2SP timestamp count by core")
    ax1.set_xlabel("Core")
    ax1.set_ylabel("Events")
    ax1.grid(True, axis="y", alpha=0.25)

    _save(fig, os.path.join(outdir, "l2sp_timestamps.png"))
    return 1


def plot_ramulator(run_dir: str, outdir: str, top_n: int) -> int:
    path = os.path.join(run_dir, "ramulator_system_pxn0_dram0.stats")
    if not os.path.exists(path):
        return 0

    ensure_dir(outdir)
    data = parse_ramulator_stats(path)
    if not data.names:
        return 0

    indexed = sorted(zip(data.names, data.values), key=lambda item: item[1], reverse=True)
    top = indexed[:top_n]

    fig, axes = plt.subplots(2, 1, figsize=(18, 12))
    ax0, ax1 = axes

    ax0.bar(range(len(top)), [value for _name, value in top], color="#c2410c")
    ax0.set_yscale("log")
    ax0.set_title(f"Top {len(top)} Ramulator counters")
    ax0.set_ylabel("Value")
    ax0.set_xticks(range(len(top)))
    ax0.set_xticklabels([name for name, _value in top], rotation=75, ha="right", fontsize=8)
    ax0.grid(True, axis="y", alpha=0.25)

    ax1.plot(range(len(indexed)), [value for _name, value in indexed], marker=".", linewidth=1.2, color="#2563eb")
    ax1.set_yscale("log")
    ax1.set_title("All Ramulator counters sorted by value")
    ax1.set_xlabel("Sorted counter index")
    ax1.set_ylabel("Value")
    ax1.grid(True, alpha=0.25)

    _save(fig, os.path.join(outdir, "ramulator_stats.png"))
    return 1


def plot_run_directory(run_dir: str, outdir: str, top_n: int) -> int:
    ensure_dir(outdir)
    plot_count = 0
    plot_count += plot_component_heatmap(run_dir, outdir)
    plot_count += plot_top_stats(run_dir, outdir, top_n=top_n)
    plot_count += plot_core_selected_stats(run_dir, outdir)
    plot_count += plot_l2sp_timestamps(run_dir, outdir)
    plot_count += plot_ramulator(run_dir, outdir, top_n=top_n)
    return plot_count


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot simulator-side stats for BFS run directories.")
    parser.add_argument("paths", nargs="*", default=["."], help="Run directories or roots to search")
    parser.add_argument("--outdir", default="bfs_plots/system_stats", help="Output directory root")
    parser.add_argument("--top-n", type=int, default=24, help="Top-N counters to show in summary plots")
    args = parser.parse_args()

    run_dirs = discover_run_dirs(args.paths)
    if not run_dirs:
        print("No BFS run directories found.")
        return

    total_plots = 0
    for run_dir in run_dirs:
        label = sanitize_filename(os.path.relpath(run_dir, os.getcwd()))
        target = os.path.join(args.outdir, label)
        count = plot_run_directory(run_dir, target, top_n=args.top_n)
        total_plots += count
        print(f"{run_dir}: wrote {count} plot(s) to {target}")

    print(f"Generated {total_plots} simulator-side plot(s).")


if __name__ == "__main__":
    main()
