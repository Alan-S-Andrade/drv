#!/usr/bin/env python3
import argparse
import csv
import os
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot per-core dynamic work queue depths from work_queue_depth_trace CSV output."
    )
    parser.add_argument("csv", help="Path to run_work_queue_depth_trace_<cores>cores.csv")
    parser.add_argument(
        "--x-axis",
        choices=("cycle", "event"),
        default="cycle",
        help="Use hardware cycle or event index on the x-axis",
    )
    parser.add_argument(
        "--include-init",
        action="store_true",
        help="Include initial zero-depth init events in the plot",
    )
    parser.add_argument(
        "--out",
        default=None,
        help="Output PNG path. Defaults to <csv_basename>_<x-axis>.png",
    )
    return parser.parse_args()


def default_output_path(csv_path: str, x_axis: str) -> str:
    base, _ = os.path.splitext(csv_path)
    return f"{base}_{x_axis}.png"


def load_rows(path: str, include_init: bool) -> dict[int, list[dict[str, int | str]]]:
    rows_by_core: dict[int, list[dict[str, int | str]]] = defaultdict(list)

    with open(path, newline="") as f:
        filtered = (line for line in f if not line.startswith("#"))
        reader = csv.DictReader(filtered)
        required = {"event", "cycle", "target_core", "depth", "op"}
        if reader.fieldnames is None:
            raise SystemExit("CSV appears empty")
        missing = required.difference(reader.fieldnames)
        if missing:
            raise SystemExit(f"missing required columns: {', '.join(sorted(missing))}")

        for row in reader:
            if not include_init and row["op"] == "init":
                continue
            core = int(row["target_core"])
            rows_by_core[core].append(
                {
                    "event": int(row["event"]),
                    "cycle": int(row["cycle"]),
                    "depth": int(row["depth"]),
                    "op": row["op"],
                }
            )

    if not rows_by_core:
        raise SystemExit("no rows left to plot")

    for core_rows in rows_by_core.values():
        core_rows.sort(key=lambda row: (int(row["cycle"]), int(row["event"])))

    return rows_by_core


def main() -> None:
    args = parse_args()
    rows_by_core = load_rows(args.csv, args.include_init)

    x_col = args.x_axis
    out_path = args.out if args.out else default_output_path(args.csv, x_col)

    core_count = len(rows_by_core)
    fig_width = 10
    fig_height = max(4.5, min(12.0, 3.5 + 0.22 * core_count))
    fig, ax = plt.subplots(figsize=(fig_width, fig_height))

    for core in sorted(rows_by_core):
        rows = rows_by_core[core]
        x_vals = [0]
        y_vals = [0]
        for row in rows:
            x_vals.append(int(row[x_col]))
            y_vals.append(int(row["depth"]))
        ax.step(x_vals, y_vals, where="post", linewidth=1.8, label=f"core {core}")

    ax.set_title("Per-core dynamic work queue depth")
    ax.set_xlabel("cycle" if x_col == "cycle" else "event index")
    ax.set_ylabel("queue depth")
    ax.grid(True, alpha=0.25)
    ax.legend(ncol=2 if core_count <= 8 else 4, fontsize=8)

    fig.tight_layout()
    fig.savefig(out_path, dpi=200)
    plt.close(fig)

    print(f"Wrote {out_path}")


if __name__ == "__main__":
    main()
