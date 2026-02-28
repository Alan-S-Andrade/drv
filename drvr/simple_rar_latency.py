import sys
import re
from collections import defaultdict
import matplotlib.pyplot as plt

def parse_lines(lines):
    core_vals = defaultdict(list)
    for line in lines:
        line = line.strip()
        if not line or line.startswith("---") or line.startswith("Core,"):
            continue

        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 4:
            continue

        core_s = re.sub(r"[^\d\-]+", "", parts[0])
        lat_s  = re.sub(r"[^\d\-\.]+", "", parts[3])

        if core_s == "" or lat_s == "":
            continue

        try:
            core = int(core_s)
            lat = float(lat_s)
        except ValueError:
            continue

        core_vals[core].append(lat)

    return core_vals

def main():
    if len(sys.argv) > 1 and sys.argv[1] != "-":
        with open(sys.argv[1], "r", encoding="utf-8") as f:
            lines = f.readlines()
    else:
        lines = sys.stdin.read().splitlines()

    core_vals = parse_lines(lines)
    if not core_vals:
        print("No data parsed.", file=sys.stderr)
        sys.exit(1)

    cores = sorted(core_vals.keys())
    avg_latency = [sum(core_vals[c]) / len(core_vals[c]) for c in cores]

    plt.figure(figsize=(14, 8))
    plt.bar(cores, avg_latency)
    plt.xlabel("Core")
    plt.ylabel("Average cycles")
    plt.title("Average L2sp latency per core")
    plt.xticks(cores)
    plt.tight_layout()
    plt.savefig("average_latency_per_core.png", dpi=150)

if __name__ == "__main__":
    main()
