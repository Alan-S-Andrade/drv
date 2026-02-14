#!/usr/bin/env python3
"""Compute pod-level L2SP interarrival times from per-core timestamp dumps.

Merges all core timestamp files for a pod, sorts them, computes consecutive
deltas, and bins into a histogram CSV for plotting.

Usage:
    python histogram_iat.py <run_directory> [bin_width]
    python histogram_iat.py <run_directory> --log2

    run_directory: directory containing l2sp_timestamps_*.csv files
    bin_width:     size of each bin in cycles (default: 1)
    --log2:        use power-of-2 bins

Example:
    python histogram_iat.py . 1
    python histogram_iat.py /path/to/drvr-run-stream_bw_l2sp --log2
"""
import csv
import sys
import math
import os
import glob
from collections import defaultdict

def main():
    if len(sys.argv) < 2:
        print("Usage: python histogram_iat.py <run_directory> [bin_width]")
        print("       python histogram_iat.py <run_directory> --log2")
        print("  bin_width: size of each bin in cycles (default: 1)")
        print("  --log2:    use power-of-2 bins")
        sys.exit(1)

    run_dir = sys.argv[1]
    use_log2 = "--log2" in sys.argv
    bin_width = 1
    if not use_log2 and len(sys.argv) > 2 and sys.argv[2] != "--log2":
        bin_width = int(sys.argv[2])

    # Find all timestamp files
    pattern = os.path.join(run_dir, "l2sp_timestamps_*.csv")
    files = sorted(glob.glob(pattern))
    if not files:
        print(f"No l2sp_timestamps_*.csv files found in {run_dir}")
        sys.exit(1)

    print(f"Found {len(files)} core timestamp files")

    # Read and merge all timestamps
    all_timestamps = []
    for f in files:
        core_name = os.path.basename(f).replace("l2sp_timestamps_", "").replace(".csv", "")
        count = 0
        with open(f, "r") as fh:
            reader = csv.DictReader(fh)
            for row in reader:
                all_timestamps.append(int(row["cycle"]))
                count += 1
        print(f"  {core_name}: {count} accesses")

    if len(all_timestamps) < 2:
        print("Not enough data to compute interarrival times.")
        sys.exit(1)

    print(f"\nTotal L2SP accesses across pod: {len(all_timestamps)}")

    # Sort all timestamps and compute interarrival deltas
    all_timestamps.sort()
    deltas = []
    for i in range(1, len(all_timestamps)):
        deltas.append(all_timestamps[i] - all_timestamps[i - 1])

    total = len(deltas)
    min_val = min(deltas)
    max_val = max(deltas)
    mean_val = sum(deltas) / total

    print(f"Interarrival samples: {total}")
    print(f"Min     : {min_val} cycles")
    print(f"Max     : {max_val} cycles")
    print(f"Mean    : {mean_val:.2f} cycles")

    # Bin the data
    if use_log2:
        print("Binning : log2")
        bins = [[0, 0, 0]]
        exp = 0
        while (1 << exp) <= max_val:
            lo = 1 << exp
            hi = (1 << (exp + 1)) - 1
            bins.append([lo, hi, 0])
            exp += 1

        for v in deltas:
            if v == 0:
                bins[0][2] += 1
            else:
                idx = int(math.log2(v)) + 1
                if idx < len(bins):
                    bins[idx][2] += 1
                else:
                    bins[-1][2] += 1
    else:
        print(f"Bin width: {bin_width} cycles")
        num_bins = (max_val // bin_width) + 1
        bins = []
        for i in range(num_bins):
            lo = i * bin_width
            hi = (i + 1) * bin_width - 1
            bins.append([lo, hi, 0])
        for v in deltas:
            idx = v // bin_width
            if idx < len(bins):
                bins[idx][2] += 1
            else:
                bins[-1][2] += 1

    # Output histogram CSV
    outfile = os.path.join(run_dir, "l2sp_interarrival_histogram.csv")
    with open(outfile, "w") as f:
        f.write("cycles,count,pct\n")
        for lo, hi, count in bins:
            if count > 0:
                center = (lo + hi) // 2
                pct = count / total * 100
                f.write(f"{center},{count},{pct:.2f}\n")

    print(f"\nHistogram written to: {outfile}")
    print(f"Non-empty bins: {sum(1 for _,_,c in bins if c > 0)}")

if __name__ == "__main__":
    main()
