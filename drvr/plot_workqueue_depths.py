#!/usr/bin/env python3
import argparse
import os
import math
from collections import defaultdict

import matplotlib.pyplot as plt


def parse_wqtrace_line(line):
    if not line.startswith("WQTRACE,"):
        return None

    parts = line.strip().split(",")
    fields = {}
    for token in parts[1:]:
        if "=" not in token:
            continue
        k, v = token.split("=", 1)
        fields[k] = v

    required = ("bench", "cores", "sample", "phase", "iter", "queue", "depths")
    for k in required:
        if k not in fields:
            return None

    try:
        cores = int(fields["cores"])
        sample = int(fields["sample"])
        iteration = int(fields["iter"])
        depths = [int(x) for x in fields["depths"].split("|") if x != ""]
    except ValueError:
        return None

    if len(depths) != cores:
        return None

    return {
        "bench": fields["bench"],
        "cores": cores,
        "sample": sample,
        "phase": fields["phase"],
        "iter": iteration,
        "queue": fields["queue"],
        "depths": depths,
    }


def load_runs_from_file(path, queue_filter):
    runs = defaultdict(list)
    run_index = defaultdict(int)

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            evt = parse_wqtrace_line(raw)
            if evt is None:
                continue
            if evt["queue"] != queue_filter:
                continue

            key = (evt["bench"], evt["cores"])
            # New run boundary heuristic: explicit init after data already exists.
            if evt["phase"] == "init" and runs[(key, run_index[key])]:
                run_index[key] += 1

            runs[(key, run_index[key])].append(evt)

    return runs


def aggregate_series(x, y, window):
    if window <= 1 or len(x) <= 1:
        return x, y, y, y

    out_x = []
    y_min = []
    y_avg = []
    y_max = []
    for i in range(0, len(x), window):
        xb = x[i : i + window]
        yb = y[i : i + window]
        if not xb:
            continue
        out_x.append(xb[len(xb) // 2])
        y_min.append(min(yb))
        y_max.append(max(yb))
        y_avg.append(sum(yb) / float(len(yb)))
    return out_x, y_min, y_avg, y_max


def plot_run(events, bench, cores, run_name, out_path, window):
    events = sorted(events, key=lambda e: e["sample"])
    x = [e["sample"] for e in events]

    per_core_series = []
    per_core_max = []
    for core in range(cores):
        y = [e["depths"][core] for e in events]
        per_core_series.append(y)
        per_core_max.append(max(y) if y else 0)

    if cores <= 4:
        ncols = 1
    elif cores <= 16:
        ncols = 2
    else:
        ncols = 4
    nrows = int(math.ceil(cores / ncols))
    fig_h = max(3.0, 2.2 * nrows + 1.5)
    fig, axes = plt.subplots(nrows, ncols, figsize=(12, fig_h), sharex=True, squeeze=False)

    for core in range(cores):
        r = core // ncols
        c = core % ncols
        ax = axes[r][c]
        y = per_core_series[core]
        ymax = per_core_max[core]
        cap = max(1, ymax)
        xw, ylo, ymid, yhi = aggregate_series(x, y, window)
        color = f"C{core % 10}"
        ax.fill_between(xw, ylo, yhi, color=color, alpha=0.20, linewidth=0)
        ax.plot(xw, ymid, linewidth=1.8, color=color)
        ax.set_ylim(0, cap * 1.1)
        ax.set_title(f"core {core} (max={ymax})", fontsize=9)
        ax.grid(True, alpha=0.25)
        ax.set_ylabel("depth", fontsize=9)

    for idx in range(cores, nrows * ncols):
        r = idx // ncols
        c = idx % ncols
        axes[r][c].set_visible(False)

    for c in range(ncols):
        axes[nrows - 1][c].set_xlabel("trace sample")

    fig.suptitle(f"{bench} queue depth trace ({cores} cores) [{run_name}]", fontsize=14)
    plt.tight_layout(rect=[0, 0, 1, 0.97])
    plt.savefig(out_path, dpi=170)
    plt.close()


def main():
    parser = argparse.ArgumentParser(
        description="Plot per-core workqueue depths from WQTRACE lines."
    )
    parser.add_argument("logs", nargs="+", help="Simulation output log files")
    parser.add_argument(
        "--outdir",
        default="wq_plots",
        help="Output directory for PNG graphs (default: wq_plots)",
    )
    parser.add_argument(
        "--queue",
        default="core",
        choices=["core", "next"],
        help="Queue trace type to plot (default: core)",
    )
    parser.add_argument(
        "--window",
        type=int,
        default=0,
        help=(
            "Samples per aggregation bucket. 0 = auto (~250 points wide), "
            "1 = raw samples, >1 = min/avg/max bucketed view."
        ),
    )
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    total = 0

    for log in args.logs:
        per_file_runs = load_runs_from_file(log, args.queue)
        base = os.path.splitext(os.path.basename(log))[0]

        for ((bench, cores), run_idx), events in per_file_runs.items():
            if not events:
                continue
            run_name = f"{base}_run{run_idx}"
            out_name = f"{bench}_{cores}cores_{run_name}.png"
            out_path = os.path.join(args.outdir, out_name)
            if args.window == 0:
                window = max(1, int(math.ceil(len(events) / 250.0)))
            else:
                window = max(1, args.window)
            plot_run(events, bench, cores, run_name, out_path, window)
            total += 1
            print(f"Wrote {out_path} (window={window})")

    if total == 0:
        print("No matching WQTRACE data found.")
    else:
        print(f"Generated {total} plot(s).")


if __name__ == "__main__":
    main()
