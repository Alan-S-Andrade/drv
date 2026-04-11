#!/usr/bin/env python3
import argparse
import os
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import matplotlib.pyplot as plt
from matplotlib import cm

l1sp_size_bytes: int = 256 * 1024  # 256KB per core
l2sp_size_bytes: int = 1024 * 1024  # 1MB per pod


def parse_fields(line: str, prefix: str) -> Optional[Dict[str, str]]:
    if not line.startswith(prefix):
        return None
    out: Dict[str, str] = {}
    for tok in line.strip().split(",")[1:]:
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        out[k] = v
    return out


@dataclass
class RunData:
    bench: str
    cores: int
    run_idx: int
    harts: Optional[int] = None
    wq_samples: List[Tuple[int, List[int]]] = field(default_factory=list)
    wq_sample_meta: List[Tuple[int, str, int]] = field(default_factory=list)
    wq_snaps: List[Tuple[int, int, str, int, List[int]]] = field(default_factory=list)
    l1sp_global: List[Tuple[int, int]] = field(default_factory=list)
    l1sp_core_harts: Dict[int, List[Tuple[int, int]]] = field(default_factory=dict)


class ParserState:
    def __init__(self) -> None:
        self.runs: Dict[Tuple[str, int, int], RunData] = {}
        self.wq_run_idx: Dict[Tuple[str, int], int] = {}
        self.wqsnap_run_idx: Dict[Tuple[str, int], int] = {}
        self.l1_run_idx: Dict[Tuple[str, int], int] = {}
        self.active_wq: Optional[Tuple[str, int, int]] = None
        self.active_wqsnap: Optional[Tuple[str, int, int]] = None
        self.active_l1: Optional[Tuple[str, int, int]] = None

    def ensure_run(self, bench: str, cores: int, run_idx: int) -> RunData:
        key = (bench, cores, run_idx)
        if key not in self.runs:
            self.runs[key] = RunData(bench=bench, cores=cores, run_idx=run_idx)
        return self.runs[key]


def parse_log_file(path: str) -> Dict[Tuple[str, int, int], RunData]:
    s = ParserState()

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.strip()

            fields = parse_fields(line, "WQTRACE_DUMP_BEGIN,")
            if fields is not None:
                if "bench" in fields and "cores" in fields:
                    bench = fields["bench"]
                    cores = int(fields["cores"])
                    run_idx = s.wq_run_idx.get((bench, cores), 0)
                    s.active_wq = (bench, cores, run_idx)
                    s.ensure_run(bench, cores, run_idx)
                continue

            fields = parse_fields(line, "WQTRACE_DUMP_END,")
            if fields is not None:
                if s.active_wq is not None:
                    bench, cores, _ = s.active_wq
                    s.wq_run_idx[(bench, cores)] = s.wq_run_idx.get((bench, cores), 0) + 1
                    s.active_wq = None
                continue

            fields = parse_fields(line, "WQTRACE,")
            if fields is not None and s.active_wq is not None:
                if fields.get("queue") != "core" or "sample" not in fields or "depths" not in fields:
                    continue
                sample = int(fields["sample"])
                depths = [int(x) for x in fields["depths"].split("|") if x]
                bench, cores, run_idx = s.active_wq
                if len(depths) != cores:
                    continue
                run = s.ensure_run(bench, cores, run_idx)
                run.wq_samples.append((sample, depths))
                run.wq_sample_meta.append((sample, fields.get("phase", "unknown"),
                                           int(fields.get("level", "-1"))))
                continue

            fields = parse_fields(line, "WQSNAP_DUMP_BEGIN,")
            if fields is not None:
                if "bench" in fields and "cores" in fields:
                    bench = fields["bench"]
                    cores = int(fields["cores"])
                    run_idx = s.wqsnap_run_idx.get((bench, cores), 0)
                    s.active_wqsnap = (bench, cores, run_idx)
                    s.ensure_run(bench, cores, run_idx)
                continue

            fields = parse_fields(line, "WQSNAP_DUMP_END,")
            if fields is not None:
                if s.active_wqsnap is not None:
                    bench, cores, _ = s.active_wqsnap
                    s.wqsnap_run_idx[(bench, cores)] = s.wqsnap_run_idx.get((bench, cores), 0) + 1
                    s.active_wqsnap = None
                continue

            fields = parse_fields(line, "WQSNAP,")
            if fields is not None and s.active_wqsnap is not None:
                if "idx" not in fields or "depths" not in fields:
                    continue
                snap_idx = int(fields["idx"])
                depths = [int(x) for x in fields["depths"].split("|") if x]
                bench, cores, run_idx = s.active_wqsnap
                if len(depths) != cores:
                    continue
                run = s.ensure_run(bench, cores, run_idx)
                run.wq_snaps.append((
                    snap_idx,
                    int(fields.get("level", "-1")),
                    fields.get("event", "unknown"),
                    int(fields.get("actor_core", "-1")),
                    depths,
                ))
                continue

            fields = parse_fields(line, "L1SPTRACE_DUMP_BEGIN,")
            if fields is not None:
                if "bench" in fields and "cores" in fields:
                    bench = fields["bench"]
                    cores = int(fields["cores"])
                    run_idx = s.l1_run_idx.get((bench, cores), 0)
                    s.active_l1 = (bench, cores, run_idx)
                    run = s.ensure_run(bench, cores, run_idx)
                    if "harts" in fields:
                        run.harts = int(fields["harts"])
                continue

            fields = parse_fields(line, "L1SPTRACE_DUMP_END,")
            if fields is not None:
                if s.active_l1 is not None:
                    bench, cores, _ = s.active_l1
                    s.l1_run_idx[(bench, cores)] = s.l1_run_idx.get((bench, cores), 0) + 1
                    s.active_l1 = None
                continue

            fields = parse_fields(line, "L1SPTRACE_GLOBAL,")
            if fields is not None and s.active_l1 is not None:
                if "sample" not in fields or "bytes" not in fields:
                    continue
                sample = int(fields["sample"])
                size_bytes = int(fields["bytes"])
                bench, cores, run_idx = s.active_l1
                s.ensure_run(bench, cores, run_idx).l1sp_global.append((sample, size_bytes))
                continue

            fields = parse_fields(line, "L1SPTRACE_CORE_HART,")
            if fields is not None and s.active_l1 is not None:
                if "sample" not in fields or "thread" not in fields or "bytes" not in fields:
                    continue
                sample = int(fields["sample"])
                thread = int(fields["thread"])
                size_bytes = int(fields["bytes"])
                bench, cores, run_idx = s.active_l1
                run = s.ensure_run(bench, cores, run_idx)
                run.l1sp_core_harts.setdefault(thread, []).append((sample, size_bytes))
                continue

    return s.runs


def is_work_stealing_bench(bench: str) -> bool:
    return "work_stealing" in bench


def to_kib(bytes_value: float) -> float:
    return float(bytes_value) / 1024.0


def l2sp_static_bytes(run: RunData, item_bytes: int) -> int:
    if run.bench != "bfs_work_stealing":
        return 0

    max_harts = 1024
    max_cores = 64
    queue_size = 512

    # These match the __l2sp__ declarations in drvr/bfs_work_stealing.cpp.
    bytes_total = 0
    bytes_total += 3 * 4  # g_total_harts, g_harts_per_core, g_total_cores
    bytes_total += 4  # g_initialized
    bytes_total += 2 * max_cores * (16 + queue_size * item_bytes)  # core_queues + next_level_queues
    bytes_total += max_harts * 8  # g_local_sense
    bytes_total += max_cores * 4  # core_has_work
    bytes_total += 8  # g_level_remaining
    bytes_total += 8  # g_count
    bytes_total += 8  # g_sense
    bytes_total += 5 * 8  # g_row_ptr, g_col_idx, visited, dist_arr, g_file_buffer
    bytes_total += 3 * 4  # g_num_vertices, g_num_edges, g_bfs_source
    bytes_total += max_harts * 8  # stat_nodes_processed
    bytes_total += max_harts * 8  # stat_steal_attempts
    bytes_total += max_harts * 8  # stat_steal_success
    bytes_total += 8  # discovered
    bytes_total += 8  # g_core_l1sp_bytes
    bytes_total += 8  # g_hart_stack_capacity_bytes
    bytes_total += max_harts * 8  # g_hart_stack_peak_bytes
    bytes_total += 4  # g_wq_trace_count
    bytes_total += 4  # g_wq_trace_dropped
    bytes_total += 4  # g_wq_snap_count
    return bytes_total


def plot_run(run: RunData, out_png: str, item_bytes: int) -> Optional[str]:
    if not run.wq_samples and not run.l1sp_core_harts:
        return "missing both WQTRACE and L1SPTRACE_CORE_HART"

    fig, axes = plt.subplots(5, 1, figsize=(15, 18), sharex=False)
    ax_l1_full, ax_l1_zoom, ax_wq_line, ax_wq_heat, ax_l2 = axes

    harts_per_core = (run.harts // run.cores) if run.harts and run.cores else 1
    per_hart_cap_kib = to_kib(l1sp_size_bytes / harts_per_core)
    core_cap_kib = to_kib(l1sp_size_bytes)

    # Panel 1 + 2: per-hart stack usage on core 0
    hart_threads = sorted(run.l1sp_core_harts.keys())
    hart_colors = cm.get_cmap("tab20", max(len(hart_threads), 1))
    l1_values_kib: List[float] = []
    x_l1: List[int] = []

    for idx, thread in enumerate(hart_threads):
        samples = sorted(run.l1sp_core_harts[thread], key=lambda x: x[0])
        if not samples:
            continue
        x_vals = [s for s, _ in samples]
        y_vals = [to_kib(v) for _, v in samples]
        color = hart_colors(idx)
        label = f"Hart t{thread}"
        ax_l1_full.plot(x_vals, y_vals, linewidth=1.4, color=color, label=label)
        ax_l1_zoom.plot(x_vals, y_vals, linewidth=1.8, color=color, label=label)
        l1_values_kib.extend(y_vals)
        if not x_l1:
            x_l1 = x_vals

    if x_l1:
        ax_l1_full.plot(x_l1, [per_hart_cap_kib] * len(x_l1), "--", linewidth=1.4,
                        color="red", label="Per-hart stack capacity")
        ax_l1_full.plot(x_l1, [core_cap_kib] * len(x_l1), ":", linewidth=1.8,
                        color="black", label="Total core L1SP capacity")
        ax_l1_zoom.plot(x_l1, [per_hart_cap_kib] * len(x_l1), "--", linewidth=1.4,
                        color="red", label="Per-hart stack capacity")
    else:
        ax_l1_full.text(0.02, 0.5, "No L1SPTRACE_CORE_HART data", transform=ax_l1_full.transAxes)
        ax_l1_zoom.text(0.02, 0.5, "No L1SPTRACE_CORE_HART data", transform=ax_l1_zoom.transAxes)

    if l1_values_kib:
        y_min = min(l1_values_kib)
        y_max = max(l1_values_kib)
        pad = max((y_max - y_min) * 0.25, 1.0)
        ax_l1_zoom.set_ylim(max(0.0, y_min - pad), y_max + pad)

    ax_l1_full.set_ylabel("KiB")
    ax_l1_full.set_title("L1SP Stack Usage by Hart (Core 0)")
    ax_l1_full.grid(True, alpha=0.25)
    handles, _ = ax_l1_full.get_legend_handles_labels()
    if handles:
        ax_l1_full.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0), borderaxespad=0.0, fontsize=8)

    ax_l1_zoom.set_ylabel("KiB")
    ax_l1_zoom.set_title("L1SP Stack Usage by Hart (Core 0, Zoomed)")
    ax_l1_zoom.grid(True, alpha=0.25)
    handles, _ = ax_l1_zoom.get_legend_handles_labels()
    if handles:
        ax_l1_zoom.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0),
                          borderaxespad=0.0, ncol=2, fontsize=8)

    # Panel 3: per-core workqueue depths across coarse execution samples
    wq = sorted(run.wq_samples, key=lambda x: x[0])
    meta = {sample: (phase, level) for sample, phase, level in run.wq_sample_meta}
    if wq:
        x_wq = [s for s, _ in wq]
        colors = cm.get_cmap("tab20", max(run.cores, 1))
        max_depth = 0
        for core in range(run.cores):
            y_vals = [depths[core] for _, depths in wq]
            max_depth = max(max_depth, max(y_vals) if y_vals else 0)
            ax_wq_line.plot(x_wq, y_vals, linewidth=1.8, color=colors(core), label=f"Core {core}")

        phase_boundaries = {"level_begin", "post_process", "post_redistribute", "final", "init"}
        for sample, _depths in wq:
            phase, level = meta.get(sample, ("unknown", -1))
            if phase in phase_boundaries:
                ax_wq_line.axvline(sample, color="0.85", linewidth=0.8, zorder=0)
                ax_wq_line.text(sample, max_depth, f"{phase}\nL{level}",
                                rotation=90, va="top", ha="right", fontsize=7, color="0.35")

        ax_wq_line.set_ylabel("Queue depth")
        ax_wq_line.set_title("Workqueue Depth by Core (WQTRACE)")
        ax_wq_line.grid(True, alpha=0.25)
        if run.cores <= 16:
            ax_wq_line.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0),
                              borderaxespad=0.0, fontsize=8, ncol=2)
    else:
        ax_wq_line.text(0.02, 0.5, "No WQTRACE data", transform=ax_wq_line.transAxes)

    # Panel 4: heatmap for queue depths, with WQSNAP overlays if present
    if wq:
        import numpy as np

        heat = np.array([depths for _, depths in wq], dtype=float).T
        im = ax_wq_heat.imshow(heat, aspect="auto", interpolation="nearest", origin="lower",
                               cmap="YlOrRd")
        ax_wq_heat.set_ylabel("Core")
        ax_wq_heat.set_title("Workqueue Depth Heatmap (WQTRACE)")
        ax_wq_heat.set_yticks(list(range(run.cores)))
        ax_wq_heat.set_xticks(x_wq)
        ax_wq_heat.set_xticklabels([str(x) for x in x_wq], rotation=0)
        cbar = fig.colorbar(im, ax=ax_wq_heat, fraction=0.018, pad=0.01)
        cbar.set_label("Queue depth")

        if run.wq_snaps:
            snap_x = []
            snap_y = []
            snap_colors = []
            for snap_idx, _level, event, actor_core, depths in sorted(run.wq_snaps, key=lambda x: x[0]):
                if actor_core < 0 or actor_core >= run.cores:
                    continue
                snap_x.append(snap_idx)
                snap_y.append(actor_core)
                snap_colors.append("cyan" if event == "steal" else "blue")
            if snap_x:
                ax_wq_heat.scatter(snap_x, snap_y, s=20, c=snap_colors, marker="o",
                                   edgecolors="black", linewidths=0.3, label="WQSNAP actor")
                ax_wq_heat.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0),
                                  borderaxespad=0.0, fontsize=8)
    else:
        ax_wq_heat.text(0.02, 0.5, "No WQTRACE data", transform=ax_wq_heat.transAxes)

    # Panel 5: L2SP queue usage
    if wq:
        x_l2 = x_wq
        static_l2sp_kib = to_kib(l2sp_static_bytes(run, item_bytes))
        y_l2_queue_total = [to_kib(sum(depths) * item_bytes) for _, depths in wq]
        y_l2_total = [static_l2sp_kib + value for value in y_l2_queue_total]
        y_l2_per_core = [value / run.cores for value in y_l2_total]
        cap_l2_kib = to_kib(l2sp_size_bytes)

        ax_l2.plot(x_l2, y_l2_per_core, linewidth=2.0, label="Average total L2SP usage per core")
        ax_l2.plot(x_l2, y_l2_total, linewidth=2.2, label="Total L2SP usage")
        if static_l2sp_kib > 0:
            ax_l2.plot(x_l2, [static_l2sp_kib] * len(x_l2), ":",
                       linewidth=1.8, label="Static non-queue L2SP baseline")
        ax_l2.plot(x_l2, [cap_l2_kib] * len(x_l2), "--", linewidth=1.8, label="Total L2SP capacity")
    else:
        ax_l2.text(0.02, 0.5, "No WQTRACE data", transform=ax_l2.transAxes)

    ax_l2.set_xlabel("Trace sample")
    ax_l2.set_ylabel("KiB")
    ax_l2.set_title("L2SP Footprint")
    ax_l2.grid(True, alpha=0.25)
    handles, _ = ax_l2.get_legend_handles_labels()
    if handles:
        ax_l2.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0), borderaxespad=0.0, fontsize=8)

    fig.suptitle(f"{run.bench} | {run.cores} cores | run {run.run_idx}", fontsize=13)
    plt.tight_layout(rect=[0, 0, 0.80, 0.97])
    plt.savefig(out_png, dpi=170)
    plt.close(fig)
    return None


def default_logs() -> List[str]:
    out: List[str] = []
    for root, _, files in os.walk("."):
        for fn in files:
            if fn.startswith("run_") and fn.endswith("cores.log"):
                out.append(os.path.join(root, fn))
    out.sort()
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description="Plot L1SP/L2SP usage for work-stealing BFS traces.")
    ap.add_argument("logs", nargs="*", help="Log files (default: discover run_*cores.log recursively)")
    ap.add_argument("--outdir", default="sp_plots", help="Output directory")
    ap.add_argument("--item-bytes", type=int, default=8, help="Bytes per queue entry")
    ap.add_argument("--bench", action="append", default=[], help="Optional benchmark filter")
    args = ap.parse_args()

    logs = args.logs if args.logs else default_logs()
    if not logs:
        print("No log files found.")
        return

    os.makedirs(args.outdir, exist_ok=True)
    total_plots = 0
    skipped = 0
    bench_filter = set(args.bench) if args.bench else None

    for log in logs:
        runs = parse_log_file(log)
        base = os.path.splitext(os.path.basename(log))[0]

        for key in sorted(runs.keys()):
            run = runs[key]
            if not is_work_stealing_bench(run.bench):
                continue
            if bench_filter is not None and run.bench not in bench_filter:
                continue

            out_name = f"{run.bench}_{run.cores}cores_{base}_run{run.run_idx}_sp_usage.png"
            out_path = os.path.join(args.outdir, out_name)
            err = plot_run(run, out_path, item_bytes=args.item_bytes)
            if err is not None:
                skipped += 1
                print(f"SKIP {log} [{run.bench} run={run.run_idx}]: {err}")
                continue

            total_plots += 1
            print(f"Wrote {out_path}")

    if total_plots == 0:
        print("No plots generated.")
    else:
        print(f"Generated {total_plots} plot(s).")
    if skipped > 0:
        print(f"Skipped {skipped} run(s) due to missing trace content.")


if __name__ == "__main__":
    main()
