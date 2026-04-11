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
    TRACE_STOP_PREFIXES,
    aggregate_l1sp_core_samples,
    aggregate_l1sp_snapshots,
    chunked_metrics,
    discover_run_dirs,
    ensure_dir,
    find_run_log,
    parse_output_summary,
    parse_trace_log,
    sanitize_filename,
    source_name_for_run_dir,
)

l2sp_size_bytes = 1024 * 1024
l1sp_size_bytes = 256 * 1024


def _save(fig: plt.Figure, out_path: str) -> None:
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def plot_levels(run_dir: str, outdir: str) -> int:
    summary = parse_output_summary(os.path.join(run_dir, "output.txt"))
    if not summary.level_records:
        return 0

    levels = [record.level for record in summary.level_records]
    total_work = [record.total_work for record in summary.level_records]
    discovered = [record.discovered for record in summary.level_records]
    distributions = [record.distribution for record in summary.level_records if record.distribution]

    fig, axes = plt.subplots(2, 1, figsize=(14, 10))
    ax0, ax1 = axes

    ax0.plot(levels, total_work, marker="o", linewidth=2.2, label="Total work")
    ax0.plot(levels, discovered, marker="s", linewidth=2.2, label="Nodes discovered")
    ax0.set_title(summary.banner or source_name_for_run_dir(run_dir))
    ax0.set_xlabel("BFS level")
    ax0.set_ylabel("Vertices")
    ax0.grid(True, alpha=0.25)
    ax0.legend()

    if distributions:
        heat = np.array(distributions, dtype=float).T
        im = ax1.imshow(heat, origin="lower", aspect="auto", cmap="YlOrRd")
        ax1.set_title("Per-core frontier distribution by BFS level")
        ax1.set_xlabel("BFS level")
        ax1.set_ylabel("Core")
        ax1.set_xticks(range(len(levels)))
        ax1.set_xticklabels([str(level) for level in levels])
        ax1.set_yticks(range(heat.shape[0]))
        fig.colorbar(im, ax=ax1, fraction=0.025, pad=0.02, label="Vertices")
    else:
        ax1.text(0.02, 0.5, "No per-core distribution found", transform=ax1.transAxes)
        ax1.set_axis_off()

    out_path = os.path.join(outdir, "bfs_levels.png")
    _save(fig, out_path)
    return 1


def _plot_table_chunks(
    rows: List[List[int]],
    headers: Sequence[str],
    entity_label: str,
    title: str,
    out_prefix: str,
) -> int:
    if not rows or len(headers) < 2:
        return 0

    entity_ids = [row[0] for row in rows]
    metric_names = list(headers[1:])
    plot_count = 0

    for chunk_idx, metrics in enumerate(chunked_metrics(metric_names, max_per_fig=6)):
        n_plots = len(metrics)
        ncols = 2
        nrows = math.ceil(n_plots / ncols)
        fig, axes = plt.subplots(nrows, ncols, figsize=(16, 4.5 * nrows), squeeze=False)
        axes_flat = axes.flatten()

        for ax, metric_name in zip(axes_flat, metrics):
            metric_idx = headers.index(metric_name)
            values = [row[metric_idx] for row in rows]
            ax.bar(entity_ids, values, color="#2f6db0")
            ax.set_title(metric_name)
            ax.set_xlabel(entity_label)
            ax.set_ylabel(metric_name)
            ax.grid(True, axis="y", alpha=0.25)

        for ax in axes_flat[n_plots:]:
            ax.set_axis_off()

        fig.suptitle(title, fontsize=13)
        suffix = f"_{chunk_idx + 1}" if chunk_idx > 0 else ""
        _save(fig, f"{out_prefix}{suffix}.png")
        plot_count += 1

    return plot_count


def plot_output_tables(run_dir: str, outdir: str) -> int:
    summary = parse_output_summary(os.path.join(run_dir, "output.txt"))
    plot_count = 0
    plot_count += _plot_table_chunks(
        summary.per_hart_rows,
        summary.per_hart_headers,
        entity_label=summary.per_hart_headers[0] if summary.per_hart_headers else "Hart",
        title="Per-hart benchmark statistics",
        out_prefix=os.path.join(outdir, "per_hart_stats"),
    )
    plot_count += _plot_table_chunks(
        summary.per_core_rows,
        summary.per_core_headers,
        entity_label=summary.per_core_headers[0] if summary.per_core_headers else "Core",
        title="Per-core L1 cache statistics",
        out_prefix=os.path.join(outdir, "per_core_l1_stats"),
    )
    return plot_count


def plot_summary(run_dir: str, outdir: str) -> int:
    summary = parse_output_summary(os.path.join(run_dir, "output.txt"))
    if not summary.summary_numeric:
        return 0

    items = sorted(summary.summary_numeric.items(), key=lambda item: item[1], reverse=True)
    labels = [item[0] for item in items]
    values = [item[1] for item in items]

    fig, ax = plt.subplots(figsize=(16, max(6, 0.35 * len(items))))
    positions = np.arange(len(labels))
    ax.barh(positions, values, color="#6c8d3c")
    ax.set_yticks(positions)
    ax.set_yticklabels(labels, fontsize=8)
    ax.invert_yaxis()
    ax.set_xlabel("Value")
    ax.set_title("Benchmark summary values parsed from output.txt")
    ax.grid(True, axis="x", alpha=0.25)
    _save(fig, os.path.join(outdir, "summary_metrics.png"))
    return 1


def plot_trace_overview(run_dir: str, outdir: str, item_bytes: int) -> int:
    log_path = find_run_log(run_dir)
    if log_path is None:
        return 0
    trace = parse_trace_log(log_path)
    if trace is None:
        return 0

    fig, axes = plt.subplots(5, 1, figsize=(16, 22))
    ax_wq_line, ax_wq_heat, ax_l1_samples, ax_l1_snaps, ax_l2 = axes

    if trace.wq_samples:
        samples = sorted(trace.wq_samples, key=lambda item: item[0])
        x_vals = [sample for sample, _depths in samples]
        colors = plt.cm.get_cmap("tab20", max(trace.cores, 1))
        max_depth = 0
        for core in range(trace.cores):
            y_vals = [depths[core] if core < len(depths) else 0 for _sample, depths in samples]
            max_depth = max(max_depth, max(y_vals) if y_vals else 0)
            ax_wq_line.plot(x_vals, y_vals, linewidth=1.8, color=colors(core), label=f"Core {core}")
        for sample in x_vals:
            phase, level = trace.wq_meta.get(sample, ("unknown", -1))
            ax_wq_line.axvline(sample, color="0.88", linewidth=0.8, zorder=0)
            ax_wq_line.text(sample, max_depth, f"{phase}\nL{level}", fontsize=7, rotation=90,
                            va="top", ha="right", color="0.35")
        ax_wq_line.set_title("WQTRACE queue depths by core")
        ax_wq_line.set_ylabel("Depth")
        ax_wq_line.grid(True, alpha=0.25)
        if trace.cores <= 16:
            ax_wq_line.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0), fontsize=8)

        heat = np.array([depths for _sample, depths in samples], dtype=float).T
        im = ax_wq_heat.imshow(heat, origin="lower", aspect="auto", cmap="YlOrRd")
        ax_wq_heat.set_title("WQTRACE heatmap with WQSNAP overlay")
        ax_wq_heat.set_ylabel("Core")
        ax_wq_heat.set_xticks(range(len(x_vals)))
        ax_wq_heat.set_xticklabels([str(sample) for sample in x_vals])
        fig.colorbar(im, ax=ax_wq_heat, fraction=0.02, pad=0.02, label="Depth")
        if trace.wq_snaps:
            snap_x = []
            snap_y = []
            snap_c = []
            for snap_idx, _level, event, actor_core, _depths in trace.wq_snaps:
                if actor_core < 0:
                    continue
                snap_x.append(snap_idx)
                snap_y.append(actor_core)
                snap_c.append("#14b8a6" if event == "steal" else "#2563eb")
            if snap_x:
                ax_wq_heat.scatter(snap_x, snap_y, c=snap_c, s=24, edgecolors="black", linewidths=0.3)
    else:
        ax_wq_line.text(0.02, 0.5, "No WQTRACE samples found", transform=ax_wq_line.transAxes)
        ax_wq_heat.text(0.02, 0.5, "No WQTRACE samples found", transform=ax_wq_heat.transAxes)

    per_sample_core = aggregate_l1sp_core_samples(trace)
    if per_sample_core:
        sample_ids = sorted(per_sample_core)
        colors = plt.cm.get_cmap("tab20", max(trace.cores, 1))
        for core in range(trace.cores):
            values = [per_sample_core[sample].get(core, 0) / 1024.0 for sample in sample_ids]
            ax_l1_samples.plot(sample_ids, values, linewidth=1.8, color=colors(core), label=f"Core {core}")
        if trace.l1sp_global:
            global_samples = sorted(trace.l1sp_global, key=lambda item: item[0])
            ax_l1_samples.plot(
                [sample for sample, _phase, _level, _bytes in global_samples],
                [bytes_used / 1024.0 for _sample, _phase, _level, bytes_used in global_samples],
                linewidth=2.2,
                linestyle="--",
                color="black",
                label="Global",
            )
        ax_l1_samples.set_title("L1SPTRACE per-core sampled bytes")
        ax_l1_samples.set_ylabel("KiB")
        ax_l1_samples.grid(True, alpha=0.25)
        if trace.cores <= 16:
            ax_l1_samples.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0), fontsize=8)
    else:
        ax_l1_samples.text(0.02, 0.5, "No L1SPTRACE samples found", transform=ax_l1_samples.transAxes)

    snap_map = aggregate_l1sp_snapshots(trace)
    if snap_map:
        snap_ids = sorted(snap_map)
        heat = np.array(
            [[snap_map[snap].get(core, 0) / 1024.0 for snap in snap_ids] for core in range(trace.cores)],
            dtype=float,
        )
        im = ax_l1_snaps.imshow(heat, origin="lower", aspect="auto", cmap="viridis")
        ax_l1_snaps.set_title("L1SPSNAP per-core snapshot bytes")
        ax_l1_snaps.set_ylabel("Core")
        stride = max(1, len(snap_ids) // 24)
        tick_idx = list(range(0, len(snap_ids), stride))
        ax_l1_snaps.set_xticks(tick_idx)
        ax_l1_snaps.set_xticklabels([str(snap_ids[idx]) for idx in tick_idx], rotation=90)
        fig.colorbar(im, ax=ax_l1_snaps, fraction=0.02, pad=0.02, label="KiB")
    else:
        ax_l1_snaps.text(0.02, 0.5, "No L1SPSNAP data found", transform=ax_l1_snaps.transAxes)

    if trace.wq_samples:
        samples = sorted(trace.wq_samples, key=lambda item: item[0])
        x_vals = [sample for sample, _depths in samples]
        total_bytes = [sum(depths) * item_bytes / 1024.0 for _sample, depths in samples]
        avg_bytes = [value / max(trace.cores, 1) for value in total_bytes]
        ax_l2.plot(x_vals, total_bytes, linewidth=2.2, label="Total queue footprint")
        ax_l2.plot(x_vals, avg_bytes, linewidth=2.0, linestyle="--", label="Average queue footprint per core")
        ax_l2.set_title("Queue-driven L2SP footprint estimate")
        ax_l2.set_xlabel("Trace sample")
        ax_l2.set_ylabel("KiB")
        ax_l2.grid(True, alpha=0.25)
        ax_l2.legend()
    else:
        ax_l2.text(0.02, 0.5, "No WQTRACE data available", transform=ax_l2.transAxes)

    fig.suptitle(trace.bench, fontsize=14)
    _save(fig, os.path.join(outdir, "trace_overview.png"))
    return 1


def plot_total_sp_usage(run_dir: str, outdir: str, item_bytes: int) -> int:
    log_path = find_run_log(run_dir)
    if log_path is None:
        return 0
    trace = parse_trace_log(log_path)
    if trace is None:
        return 0

    per_sample_core = aggregate_l1sp_core_samples(trace)
    if not per_sample_core and not trace.wq_samples:
        return 0

    fig, axes = plt.subplots(2, 1, figsize=(14, 10))
    ax_l1, ax_l2 = axes

    if per_sample_core:
        sample_ids = sorted(per_sample_core)
        total_l1_kib = [sum(per_sample_core[sample].values()) / 1024.0 for sample in sample_ids]
        ax_l1.plot(sample_ids, total_l1_kib, marker="o", linewidth=2.2, color="#2563eb",
                   label="Summed per-core L1SP trace bytes")
        if trace.l1sp_global:
            global_samples = sorted(trace.l1sp_global, key=lambda item: item[0])
            ax_l1.plot(
                [sample for sample, _phase, _level, _bytes in global_samples],
                [bytes_used / 1024.0 for _sample, _phase, _level, bytes_used in global_samples],
                linestyle="--",
                linewidth=2.0,
                color="black",
                label="L1SPTRACE_GLOBAL bytes",
            )
        ax_l1.set_title("Total L1SP usage across execution")
        ax_l1.set_xlabel("Trace sample")
        ax_l1.set_ylabel("KiB")
        ax_l1.grid(True, alpha=0.25)
        ax_l1.legend()
    else:
        ax_l1.text(0.02, 0.5, "No L1SP trace samples available", transform=ax_l1.transAxes)

    if trace.wq_samples:
        samples = sorted(trace.wq_samples, key=lambda item: item[0])
        x_vals = [sample for sample, _depths in samples]
        total_l2_kib = [sum(depths) * item_bytes / 1024.0 for _sample, depths in samples]
        ax_l2.plot(x_vals, total_l2_kib, marker="o", linewidth=2.2, color="#059669")
        ax_l2.set_title("Total L2SP workqueue footprint across execution")
        ax_l2.set_xlabel("Trace sample")
        ax_l2.set_ylabel("KiB")
        ax_l2.grid(True, alpha=0.25)
        ax_l2.text(
            0.01,
            0.98,
            "Trace-derived estimate from WQTRACE queue depths",
            transform=ax_l2.transAxes,
            va="top",
            ha="left",
            fontsize=9,
            color="0.35",
        )
    else:
        ax_l2.text(0.02, 0.5, "No WQTRACE samples available", transform=ax_l2.transAxes)

    fig.suptitle(f"{trace.bench} scratchpad usage over execution", fontsize=14)
    _save(fig, os.path.join(outdir, "total_sp_usage_over_time.png"))
    return 1


def plot_l2sp_usage_by_core(run_dir: str, outdir: str, item_bytes: int) -> int:
    log_path = find_run_log(run_dir)
    if log_path is None:
        return 0
    trace = parse_trace_log(log_path)
    if trace is None or not trace.wq_samples:
        return 0

    samples = sorted(trace.wq_samples, key=lambda item: item[0])
    x_vals = [sample for sample, _depths in samples]
    total_capacity_kib = l2sp_size_bytes / 1024.0
    per_core_capacity_kib = total_capacity_kib / max(trace.cores, 1)

    fig, axes = plt.subplots(3, 1, figsize=(14, 13))
    ax0, ax1, ax2 = axes
    colors = plt.cm.get_cmap("tab20", max(trace.cores, 1))

    total_usage = []
    avg_usage = []
    for core in range(trace.cores):
        y_vals = [((depths[core] if core < len(depths) else 0) * item_bytes) / 1024.0 for _sample, depths in samples]
        ax0.plot(x_vals, y_vals, linewidth=1.8, color=colors(core), label=f"Core {core}")
        total_usage.append(y_vals)

    total_usage_series = [sum(core_vals[idx] for core_vals in total_usage) for idx in range(len(x_vals))]
    avg_usage_series = [value / trace.cores for value in total_usage_series]

    ax0.plot(x_vals, [per_core_capacity_kib] * len(x_vals), linewidth=1.8, color="#dc2626",
             linestyle="-.", label="Per-core share of total capacity")
    ax0.plot(x_vals, avg_usage_series, linewidth=2.0, color="#7c3aed", linestyle=":", label="Average per-core usage")
    ax0.set_title("Per-core L2SP workqueue footprint")
    ax0.set_xlabel("Trace sample")
    ax0.set_ylabel("KiB")
    ax0.grid(True, alpha=0.25)
    ax0.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0), fontsize=8)
    per_core_peak = max((max(core_vals) for core_vals in total_usage), default=0.0)
    if per_core_peak > 0.0:
        ax0.set_ylim(0.0, max(per_core_capacity_kib, per_core_peak) * 1.15)

    ax1.plot(x_vals, total_usage_series, linewidth=2.4, color="black", label="Total usage")
    ax1.plot(x_vals, [total_capacity_kib] * len(x_vals), linewidth=2.0, color="#dc2626",
             linestyle="--", label="Total L2SP capacity")
    ax1.plot(x_vals, avg_usage_series, linewidth=2.0, color="#2563eb", label="Average per-core usage")
    ax1.set_title("Total L2SP usage compared to capacity")
    ax1.set_xlabel("Trace sample")
    ax1.set_ylabel("KiB")
    ax1.grid(True, alpha=0.25)
    ax1.legend()
    ax1.text(
        0.01,
        0.95,
        "Capacity line is total pod L2SP: 1024 KiB",
        transform=ax1.transAxes,
        va="top",
        ha="left",
        fontsize=9,
        color="0.35",
    )

    capacity_pct = [(value / total_capacity_kib) * 100.0 for value in total_usage_series]
    ax2.plot(x_vals, capacity_pct, linewidth=2.2, color="#059669")
    ax2.set_title("Total L2SP usage as a percentage of total capacity")
    ax2.set_xlabel("Trace sample")
    ax2.set_ylabel("% of total L2SP capacity")
    ax2.grid(True, alpha=0.25)

    fig.suptitle(f"{trace.bench} L2SP usage by core", fontsize=14)
    _save(fig, os.path.join(outdir, "l2sp_usage_by_core.png"))
    return 1


def plot_l1sp_usage_by_hart(run_dir: str, outdir: str) -> int:
    log_path = find_run_log(run_dir)
    if log_path is None:
        return 0
    trace = parse_trace_log(log_path)
    if trace is None:
        return 0

    # Prefer fine-grained snapshots because they carry a real timeline for all harts.
    target_core = 0

    if trace.l1sp_snap_hart:
        filtered = [
            (snap_idx, level, core, thread, hart, bytes_used)
            for snap_idx, level, core, thread, hart, bytes_used in trace.l1sp_snap_hart
            if core == target_core
        ]
        hart_ids = sorted({hart for _idx, _level, _core, _thread, hart, _bytes in filtered})
        per_hart: dict[int, list[tuple[int, float]]] = {hart: [] for hart in hart_ids}
        total_by_sample: dict[int, float] = {}
        for snap_idx, _level, _core, _thread, hart, bytes_used in sorted(filtered):
            kib = bytes_used / 1024.0
            per_hart[hart].append((snap_idx, kib))
            total_by_sample[snap_idx] = total_by_sample.get(snap_idx, 0.0) + kib
    elif trace.l1sp_core0_hart_samples:
        hart_ids = sorted({hart for _sample, _thread, hart, _bytes in trace.l1sp_core0_hart_samples})
        per_hart = {hart: [] for hart in hart_ids}
        total_by_sample = {}
        for sample, _thread, hart, bytes_used in sorted(trace.l1sp_core0_hart_samples):
            kib = bytes_used / 1024.0
            per_hart[hart].append((sample, kib))
            total_by_sample[sample] = total_by_sample.get(sample, 0.0) + kib
    elif trace.l1sp_hart_samples:
        # Last fallback: these records are aggregate state dumps and may only provide one point.
        hart_ids = sorted({hart for _sample, _core, _thread, hart, _bytes in trace.l1sp_hart_samples})
        per_hart = {hart: [] for hart in hart_ids}
        total_by_sample = {}
        for sample, _core, _thread, hart, bytes_used in sorted(trace.l1sp_hart_samples):
            kib = bytes_used / 1024.0
            per_hart[hart].append((sample, kib))
            total_by_sample[sample] = total_by_sample.get(sample, 0.0) + kib
    else:
        return 0

    if not hart_ids:
        return 0

    total_capacity_kib = l1sp_size_bytes / 1024.0

    fig, axes = plt.subplots(3, 1, figsize=(15, 13))
    ax_harts, ax_total, ax_compare = axes
    colors = plt.cm.get_cmap("tab20", max(len(hart_ids), 1))

    for idx, hart in enumerate(hart_ids):
        samples = per_hart[hart]
        if not samples:
            continue
        ax_harts.plot(
            [sample for sample, _kib in samples],
            [kib for _sample, kib in samples],
            linewidth=1.6,
            color=colors(idx),
            label=f"Hart {hart}",
        )

    sample_ids = sorted(total_by_sample)
    total_usage = [total_by_sample[sample] for sample in sample_ids]
    ax_harts.plot(sample_ids, total_usage, linewidth=2.4, color="black", linestyle="--", label="Total traced L1SP usage")
    ax_harts.set_title(f"Per-hart L1SP usage over execution for core {target_core}")
    ax_harts.set_xlabel("Trace sample")
    ax_harts.set_ylabel("KiB")
    ax_harts.grid(True, alpha=0.25)
    ax_harts.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0), fontsize=8)
    data_max = max(total_usage) if total_usage else 0.0
    if data_max > 0.0:
        ax_harts.set_ylim(0.0, data_max * 1.15)

    ax_total.plot(sample_ids, total_usage, linewidth=2.4, color="black", label="Total L1SP usage")
    ax_total.plot(
        sample_ids,
        [total_capacity_kib] * len(sample_ids),
        linewidth=2.0,
        color="#dc2626",
        linestyle="--",
        label="Total L1SP capacity",
    )
    ax_total.set_title(f"Total L1SP usage vs total capacity for core {target_core}")
    ax_total.set_xlabel("Trace sample")
    ax_total.set_ylabel("KiB")
    ax_total.grid(True, alpha=0.25)
    ax_total.legend()
    ax_total.text(
        0.01,
        0.95,
        "Capacity line is one core's L1SP: 256 KiB",
        transform=ax_total.transAxes,
        va="top",
        ha="left",
        fontsize=9,
        color="0.35",
    )

    capacity_pct = [(value / total_capacity_kib) * 100.0 for value in total_usage]
    ax_compare.plot(sample_ids, capacity_pct, linewidth=2.2, color="#7c3aed")
    ax_compare.set_title(f"Total traced L1SP usage as a percentage of core {target_core} L1SP capacity")
    ax_compare.set_xlabel("Trace sample")
    ax_compare.set_ylabel("% of total L1SP capacity")
    ax_compare.grid(True, alpha=0.25)

    fig.suptitle(f"{trace.bench} per-hart L1SP usage", fontsize=14)
    _save(fig, os.path.join(outdir, "l1sp_usage_by_hart.png"))
    return 1


def plot_run_directory(run_dir: str, outdir: str, item_bytes: int) -> int:
    ensure_dir(outdir)
    plot_count = 0
    plot_count += plot_levels(run_dir, outdir)
    plot_count += plot_output_tables(run_dir, outdir)
    plot_count += plot_summary(run_dir, outdir)
    plot_count += plot_trace_overview(run_dir, outdir, item_bytes=item_bytes)
    plot_count += plot_total_sp_usage(run_dir, outdir, item_bytes=item_bytes)
    plot_count += plot_l2sp_usage_by_core(run_dir, outdir, item_bytes=item_bytes)
    plot_count += plot_l1sp_usage_by_hart(run_dir, outdir)
    return plot_count


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot BFS benchmark-side statistics and trace dumps.")
    parser.add_argument("paths", nargs="*", default=["."], help="Run directories or roots to search")
    parser.add_argument("--outdir", default="bfs_plots/run_stats", help="Output directory root")
    parser.add_argument("--item-bytes", type=int, default=8, help="Bytes per workqueue item")
    args = parser.parse_args()

    run_dirs = discover_run_dirs(args.paths)
    if not run_dirs:
        print("No BFS run directories found.")
        return

    total_plots = 0
    for run_dir in run_dirs:
        label = sanitize_filename(os.path.relpath(run_dir, os.getcwd()))
        target = os.path.join(args.outdir, label)
        count = plot_run_directory(run_dir, target, item_bytes=args.item_bytes)
        total_plots += count
        print(f"{run_dir}: wrote {count} plot(s) to {target}")

    print(f"Generated {total_plots} benchmark-side plot(s).")


if __name__ == "__main__":
    main()
