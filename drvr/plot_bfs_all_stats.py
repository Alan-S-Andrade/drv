#!/usr/bin/env python3
import argparse
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from bfs_plot_utils import discover_run_dirs, ensure_dir, sanitize_filename
from plot_bfs_run_stats import plot_run_directory as plot_benchmark_side
from plot_bfs_system_stats import plot_run_directory as plot_system_side


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate the full BFS plotting suite for discovered run directories.")
    parser.add_argument("paths", nargs="*", default=["."], help="Run directories or roots to search")
    parser.add_argument("--outdir", default="bfs_plots", help="Output directory root")
    parser.add_argument("--item-bytes", type=int, default=8, help="Bytes per workqueue item")
    parser.add_argument("--top-n", type=int, default=24, help="Top-N counters for summary bar charts")
    args = parser.parse_args()

    run_dirs = discover_run_dirs(args.paths)
    if not run_dirs:
        print("No BFS run directories found.")
        return

    ensure_dir(args.outdir)
    total_plots = 0
    for run_dir in run_dirs:
        label = sanitize_filename(os.path.relpath(run_dir, os.getcwd()))
        target = os.path.join(args.outdir, label)
        benchmark_count = plot_benchmark_side(run_dir, os.path.join(target, "benchmark"), item_bytes=args.item_bytes)
        system_count = plot_system_side(run_dir, os.path.join(target, "system"), top_n=args.top_n)
        total_plots += benchmark_count + system_count
        print(f"{run_dir}: wrote {benchmark_count + system_count} plot(s) to {target}")

    print(f"Generated {total_plots} total plot(s).")


if __name__ == "__main__":
    main()
