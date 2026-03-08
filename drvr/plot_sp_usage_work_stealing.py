#!/usr/bin/env python3
import argparse
import os
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import matplotlib.pyplot as plt


# Queue capacities are from benchmark source constants (entries per core queue).
QUEUE_CAPACITY_ENTRIES = {
    "bfs_work_stealing": 8192,
    "pagerank_work_stealing": 512,
    "work_stealing_benchmark": 4096,
    "work_stealing_batch": 4096,
}


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
    wq_samples: List[Tuple[int, List[int]]] = field(default_factory=list)
    l1sp_global: List[Tuple[int, int]] = field(default_factory=list)
    l1sp_core_bytes: Optional[int] = None
    l1sp_global_bytes: Optional[int] = None


class ParserState:
    def __init__(self) -> None:
        self.runs: Dict[Tuple[str, int, int], RunData] = {}
        self.wq_run_idx: Dict[Tuple[str, int], int] = {}
        self.l1_run_idx: Dict[Tuple[str, int], int] = {}
        self.active_wq: Optional[Tuple[str, int, int]] = None
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
                    rk = (bench, cores)
                    run_idx = s.wq_run_idx.get(rk, 0)
                    s.active_wq = (bench, cores, run_idx)
                    s.ensure_run(bench, cores, run_idx)
                continue

            fields = parse_fields(line, "WQTRACE_DUMP_END,")
            if fields is not None:
                if s.active_wq is not None:
                    bench, cores, run_idx = s.active_wq
                    _ = run_idx
                    rk = (bench, cores)
                    s.wq_run_idx[rk] = s.wq_run_idx.get(rk, 0) + 1
                    s.active_wq = None
                continue

            fields = parse_fields(line, "WQTRACE,")
            if fields is not None and s.active_wq is not None:
                if fields.get("queue") != "core":
                    continue
                if "sample" not in fields or "depths" not in fields:
                    continue
                sample = int(fields["sample"])
                depths = [int(x) for x in fields["depths"].split("|") if x != ""]
                bench, cores, run_idx = s.active_wq
                if len(depths) != cores:
                    continue
                run = s.ensure_run(bench, cores, run_idx)
                run.wq_samples.append((sample, depths))
                continue

            fields = parse_fields(line, "L1SPTRACE_DUMP_BEGIN,")
            if fields is not None:
                if "bench" in fields and "cores" in fields:
                    bench = fields["bench"]
                    cores = int(fields["cores"])
                    rk = (bench, cores)
                    run_idx = s.l1_run_idx.get(rk, 0)
                    s.active_l1 = (bench, cores, run_idx)
                    s.ensure_run(bench, cores, run_idx)
                continue

            fields = parse_fields(line, "L1SPTRACE_DUMP_END,")
            if fields is not None:
                if s.active_l1 is not None:
                    bench, cores, run_idx = s.active_l1
                    _ = run_idx
                    rk = (bench, cores)
                    s.l1_run_idx[rk] = s.l1_run_idx.get(rk, 0) + 1
                    s.active_l1 = None
                continue

            fields = parse_fields(line, "L1SPTRACE_CONFIG,")
            if fields is not None and s.active_l1 is not None:
                bench, cores, run_idx = s.active_l1
                run = s.ensure_run(bench, cores, run_idx)
                if "core_bytes" in fields:
                    run.l1sp_core_bytes = int(fields["core_bytes"])
                if "global_bytes" in fields:
                    run.l1sp_global_bytes = int(fields["global_bytes"])
                continue

            fields = parse_fields(line, "L1SPTRACE_GLOBAL,")
            if fields is not None and s.active_l1 is not None:
                if "sample" not in fields or "bytes" not in fields:
                    continue
                sample = int(fields["sample"])
                size_bytes = int(fields["bytes"])
                bench, cores, run_idx = s.active_l1
                run = s.ensure_run(bench, cores, run_idx)
                run.l1sp_global.append((sample, size_bytes))
                continue

    return s.runs


def is_work_stealing_bench(bench: str) -> bool:
    return "work_stealing" in bench or "work_stealing" == bench


def to_mib(bytes_value: int) -> float:
    return float(bytes_value) / (1024.0 * 1024.0)


def plot_run(run: RunData, out_png: str, item_bytes: int) -> Optional[str]:
    if not run.wq_samples and not run.l1sp_global:
        return "missing both WQTRACE and L1SPTRACE"

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 8), sharex=False)

    # L1SP size vs capacity
    l1_samples = sorted(run.l1sp_global, key=lambda x: x[0])
    if l1_samples:
        x_l1 = [s for s, _ in l1_samples]
        y_l1 = [to_mib(v) for _, v in l1_samples]
    elif run.wq_samples and run.l1sp_global_bytes is not None:
        x_l1 = [s for s, _ in sorted(run.wq_samples, key=lambda x: x[0])]
        y_l1 = [to_mib(run.l1sp_global_bytes)] * len(x_l1)
    else:
        x_l1 = []
        y_l1 = []

    if run.l1sp_global_bytes is not None:
        cap_l1_mib = to_mib(run.l1sp_global_bytes)
        if x_l1:
            ax1.plot(x_l1, [cap_l1_mib] * len(x_l1), "--", linewidth=1.8, label="L1SP capacity (global)")
        else:
            ax1.axhline(cap_l1_mib, linestyle="--", linewidth=1.8, label="L1SP capacity (global)")
    if x_l1:
        ax1.plot(x_l1, y_l1, linewidth=2.2, label="L1SP size (global)")
    else:
        ax1.text(0.02, 0.5, "No L1SPTRACE_GLOBAL data", transform=ax1.transAxes)

    ax1.set_ylabel("MiB")
    ax1.set_title("L1SP Global Size vs Capacity")
    ax1.grid(True, alpha=0.25)
    handles, labels = ax1.get_legend_handles_labels()
    if handles:
        ax1.legend(loc="best")

    # L2SP size vs capacity inferred from queue-depth trace
    wq = sorted(run.wq_samples, key=lambda x: x[0])
    if wq:
        x_l2 = [s for s, _ in wq]
        y_l2 = [to_mib(sum(depths) * item_bytes) for _, depths in wq]

        q_cap_entries = QUEUE_CAPACITY_ENTRIES.get(run.bench)
        if q_cap_entries is None:
            # Fallback: use max observed depth per core as best-effort lower-bound capacity.
            q_cap_entries = max(max(depths) for _, depths in wq)
            cap_note = "(estimated)"
        else:
            cap_note = ""

        cap_l2_bytes = run.cores * q_cap_entries * item_bytes
        cap_l2_mib = to_mib(cap_l2_bytes)

        ax2.plot(x_l2, y_l2, linewidth=2.2, label="L2SP size from queue depth")
        ax2.plot(x_l2, [cap_l2_mib] * len(x_l2), "--", linewidth=1.8,
                 label=f"L2SP queue capacity {cap_note}".strip())
    else:
        ax2.text(0.02, 0.5, "No WQTRACE queue-depth data", transform=ax2.transAxes)

    ax2.set_xlabel("Trace sample")
    ax2.set_ylabel("MiB")
    ax2.set_title("L2SP Queue Footprint vs Capacity")
    ax2.grid(True, alpha=0.25)
    handles, labels = ax2.get_legend_handles_labels()
    if handles:
        ax2.legend(loc="best")

    fig.suptitle(f"{run.bench} | {run.cores} cores | run {run.run_idx}", fontsize=13)
    plt.tight_layout(rect=[0, 0, 1, 0.97])
    plt.savefig(out_png, dpi=170)
    plt.close(fig)
    return None


def default_logs() -> List[str]:
    out = []
    for root, _, files in os.walk("."):
        for fn in files:
            if fn.startswith("run_") and fn.endswith("cores.log"):
                out.append(os.path.join(root, fn))
    out.sort()
    return out


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Plot L1SP/L2SP size vs capacity across execution for work-stealing benchmarks."
    )
    ap.add_argument("logs", nargs="*", help="Log files (default: discover run_*cores.log recursively)")
    ap.add_argument("--outdir", default="sp_plots", help="Output directory (default: sp_plots)")
    ap.add_argument("--item-bytes", type=int, default=8, help="Bytes per queue entry (default: 8)")
    ap.add_argument("--bench", action="append", default=[], help="Optional benchmark filter (repeatable)")
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
