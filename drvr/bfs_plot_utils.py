#!/usr/bin/env python3
import csv
import math
import os
import re
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


RUN_DIR_VARIANTS = {
    "drvr-run-bfs_work_stealing_l1sp_cache": "bfs_work_stealing_l1sp_cache.cpp",
    "drvr-run-bfs_work_stealing_adaptive": "bfs_work_stealing_adaptive.cpp",
    "drvr-run-bfs_nosteal_baseline": "bfs_work_stealing_nosteal_baseline.cpp",
}

TRACE_STOP_PREFIXES = (
    "WQTRACE_DUMP_BEGIN,",
    "L1SPTRACE_DUMP_BEGIN,",
    "WQSNAP_DUMP_BEGIN,",
    "L1SPSNAP_DUMP_BEGIN,",
)


@dataclass
class LevelRecord:
    level: int
    total_work: int
    discovered: int
    distribution: List[int]


@dataclass
class OutputSummary:
    banner: Optional[str] = None
    graph_lines: List[str] = field(default_factory=list)
    level_records: List[LevelRecord] = field(default_factory=list)
    per_hart_headers: List[str] = field(default_factory=list)
    per_hart_rows: List[List[int]] = field(default_factory=list)
    per_core_headers: List[str] = field(default_factory=list)
    per_core_rows: List[List[int]] = field(default_factory=list)
    summary_numeric: Dict[str, float] = field(default_factory=dict)
    summary_raw: Dict[str, str] = field(default_factory=dict)


@dataclass
class TraceRun:
    bench: str
    cores: int
    harts: Optional[int] = None
    wq_samples: List[Tuple[int, List[int]]] = field(default_factory=list)
    wq_meta: Dict[int, Tuple[str, int]] = field(default_factory=dict)
    wq_snaps: List[Tuple[int, int, str, int, List[int]]] = field(default_factory=list)
    l1sp_global: List[Tuple[int, str, int, int]] = field(default_factory=list)
    l1sp_hart_samples: List[Tuple[int, int, int, int, int]] = field(default_factory=list)
    l1sp_core0_hart_samples: List[Tuple[int, int, int, int]] = field(default_factory=list)
    l1sp_snap_core: List[Tuple[int, int, int, int]] = field(default_factory=list)
    l1sp_snap_hart: List[Tuple[int, int, int, int, int, int]] = field(default_factory=list)


@dataclass
class StatsCsvData:
    components: List[str]
    stat_names: List[str]
    component_stat_values: Dict[str, Dict[str, float]]
    stat_totals: Dict[str, float]
    core_stat_values: Dict[int, Dict[str, float]]
    hart_stat_values: Dict[int, Dict[str, float]]


@dataclass
class RamulatorData:
    names: List[str]
    values: List[float]


def discover_run_dirs(paths: Sequence[str]) -> List[str]:
    found: List[str] = []
    seen = set()
    for path in paths:
        abs_path = os.path.abspath(path)
        if os.path.isdir(abs_path) and os.path.basename(abs_path) in RUN_DIR_VARIANTS:
            if os.path.exists(os.path.join(abs_path, "output.txt")) and abs_path not in seen:
                found.append(abs_path)
                seen.add(abs_path)
            continue

        for root, dirs, _files in os.walk(abs_path):
            for name in dirs:
                if name not in RUN_DIR_VARIANTS:
                    continue
                run_dir = os.path.join(root, name)
                if not os.path.exists(os.path.join(run_dir, "output.txt")):
                    continue
                if run_dir in seen:
                    continue
                found.append(run_dir)
                seen.add(run_dir)
    found.sort()
    return found


def source_name_for_run_dir(run_dir: str) -> str:
    return RUN_DIR_VARIANTS.get(os.path.basename(run_dir), os.path.basename(run_dir))


def parse_fields(line: str, prefix: str) -> Optional[Dict[str, str]]:
    if not line.startswith(prefix):
        return None
    out: Dict[str, str] = {}
    for token in line.strip().split(",")[1:]:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        out[key] = value
    return out


def _parse_int_or_float(text: str) -> float:
    if "." in text:
        return float(text)
    return float(int(text))


def _parse_first_number(text: str) -> Optional[float]:
    match = re.search(r"-?\d+(?:\.\d+)?", text)
    if not match:
        return None
    return _parse_int_or_float(match.group(0))


def _parse_distribution(line: str) -> List[int]:
    values: Dict[int, int] = {}
    for core, count in re.findall(r"C(\d+):(\d+)", line):
        values[int(core)] = int(count)
    if not values:
        return []
    max_core = max(values)
    return [values.get(core, 0) for core in range(max_core + 1)]


def _parse_pipe_table(lines: Sequence[str], start_idx: int) -> Tuple[List[str], List[List[int]], int]:
    header_line = lines[start_idx].strip()
    headers = [part.strip() for part in header_line.split("|")]
    rows: List[List[int]] = []
    idx = start_idx + 2
    while idx < len(lines):
        stripped = lines[idx].strip()
        if not stripped:
            break
        if "|" not in stripped:
            break
        if stripped.endswith(":"):
            break
        raw_values = [part.strip() for part in stripped.split("|")]
        try:
            rows.append([int(value) for value in raw_values])
        except ValueError:
            break
        idx += 1
    return headers, rows, idx


def parse_output_summary(path: str) -> OutputSummary:
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()

    out = OutputSummary()
    idx = 0
    while idx < len(lines):
        line = lines[idx].rstrip("\n")
        stripped = line.strip()

        if stripped.startswith("=== BFS ") and stripped.endswith("==="):
            out.banner = stripped
        elif stripped.startswith("Graph ") or stripped.startswith("Hot state:") or stripped.startswith("Frontiers:") \
                or stripped.startswith("Hardware:") or stripped.startswith("Source:") or stripped.startswith("L1SP:") \
                or stripped.startswith("L1SP stack slot:") or stripped.startswith("Stealing:") \
                or stripped.startswith("L1 work cache:"):
            out.graph_lines.append(stripped)
        else:
            match = re.match(r"Level (\d+): total_work=(\d+), discovered=(\d+)", stripped)
            if match:
                distribution: List[int] = []
                if idx + 1 < len(lines):
                    distribution = _parse_distribution(lines[idx + 1].strip())
                    if distribution:
                        idx += 1
                out.level_records.append(
                    LevelRecord(
                        level=int(match.group(1)),
                        total_work=int(match.group(2)),
                        discovered=int(match.group(3)),
                        distribution=distribution,
                    )
                )
            elif stripped == "Per-hart statistics:" and idx + 2 < len(lines):
                headers, rows, idx = _parse_pipe_table(lines, idx + 1)
                out.per_hart_headers = headers
                out.per_hart_rows = rows
                continue
            elif stripped == "Per-core L1 cache statistics:" and idx + 2 < len(lines):
                headers, rows, idx = _parse_pipe_table(lines, idx + 1)
                out.per_core_headers = headers
                out.per_core_rows = rows
                continue
            elif stripped == "Summary:":
                idx += 1
                while idx < len(lines):
                    summary_line = lines[idx].strip()
                    if not summary_line:
                        idx += 1
                        continue
                    if summary_line.startswith(TRACE_STOP_PREFIXES):
                        return out
                    if ":" not in summary_line:
                        idx += 1
                        continue
                    key, value = summary_line.split(":", 1)
                    key = key.strip()
                    value = value.strip()
                    if key in ("WORKSRC", "L1CACHE"):
                        for token in value.split():
                            if "=" not in token:
                                continue
                            sub_key, sub_value = token.split("=", 1)
                            full_key = f"{key}.{sub_key}"
                            out.summary_raw[full_key] = sub_value
                            numeric = _parse_first_number(sub_value)
                            if numeric is not None:
                                out.summary_numeric[full_key] = numeric
                    else:
                        out.summary_raw[key] = value
                        numeric = _parse_first_number(value)
                        if numeric is not None:
                            out.summary_numeric[key] = numeric
                    idx += 1
                return out

        idx += 1

    return out


def parse_trace_log(path: str) -> Optional[TraceRun]:
    run: Optional[TraceRun] = None
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            fields = parse_fields(line, "WQTRACE_DUMP_BEGIN,")
            if fields is not None and "bench" in fields and "cores" in fields:
                run = TraceRun(fields["bench"], int(fields["cores"]))
                continue

            if run is None:
                continue

            fields = parse_fields(line, "L1SPTRACE_DUMP_BEGIN,")
            if fields is not None:
                if "harts" in fields:
                    run.harts = int(fields["harts"])
                continue

            fields = parse_fields(line, "WQTRACE,")
            if fields is not None and fields.get("queue") == "core":
                depths = [int(value) for value in fields.get("depths", "").split("|") if value]
                sample = int(fields.get("sample", "-1"))
                run.wq_samples.append((sample, depths))
                run.wq_meta[sample] = (fields.get("phase", "unknown"), int(fields.get("level", "-1")))
                continue

            fields = parse_fields(line, "WQSNAP,")
            if fields is not None:
                depths = [int(value) for value in fields.get("depths", "").split("|") if value]
                run.wq_snaps.append((
                    int(fields.get("idx", "-1")),
                    int(fields.get("level", "-1")),
                    fields.get("event", "unknown"),
                    int(fields.get("actor_core", "-1")),
                    depths,
                ))
                continue

            fields = parse_fields(line, "L1SPTRACE_GLOBAL,")
            if fields is not None:
                run.l1sp_global.append((
                    int(fields.get("sample", "-1")),
                    fields.get("phase", "unknown"),
                    int(fields.get("level", "-1")),
                    int(fields.get("bytes", "0")),
                ))
                continue

            fields = parse_fields(line, "L1SPTRACE_HART,")
            if fields is not None:
                run.l1sp_hart_samples.append((
                    int(fields.get("sample", "-1")),
                    int(fields.get("core", "-1")),
                    int(fields.get("thread", "-1")),
                    int(fields.get("hart", "-1")),
                    int(fields.get("bytes", "0")),
                ))
                continue

            fields = parse_fields(line, "L1SPTRACE_CORE_HART,")
            if fields is not None:
                run.l1sp_core0_hart_samples.append((
                    int(fields.get("sample", "-1")),
                    int(fields.get("thread", "-1")),
                    int(fields.get("hart", "-1")),
                    int(fields.get("bytes", "0")),
                ))
                continue

            fields = parse_fields(line, "L1SPSNAP_CORE,")
            if fields is not None:
                run.l1sp_snap_core.append((
                    int(fields.get("idx", "-1")),
                    int(fields.get("level", "-1")),
                    int(fields.get("core", "-1")),
                    int(fields.get("bytes", "0")),
                ))
                continue

            fields = parse_fields(line, "L1SPSNAP_HART,")
            if fields is not None:
                run.l1sp_snap_hart.append((
                    int(fields.get("idx", "-1")),
                    int(fields.get("level", "-1")),
                    int(fields.get("core", "-1")),
                    int(fields.get("thread", "-1")),
                    int(fields.get("hart", "-1")),
                    int(fields.get("bytes", "0")),
                ))
                continue
    return run


def aggregate_l1sp_core_samples(run: TraceRun) -> Dict[int, Dict[int, int]]:
    per_sample: Dict[int, Dict[int, int]] = {}
    source = run.l1sp_hart_samples
    if source:
        for sample, core, _thread, _hart, bytes_used in source:
            per_sample.setdefault(sample, {})
            per_sample[sample][core] = per_sample[sample].get(core, 0) + bytes_used
        return per_sample

    for sample, thread, _hart, bytes_used in run.l1sp_core0_hart_samples:
        per_sample.setdefault(sample, {})
        per_sample[sample][thread] = bytes_used
    return per_sample


def aggregate_l1sp_snapshots(run: TraceRun) -> Dict[int, Dict[int, int]]:
    per_snap: Dict[int, Dict[int, int]] = {}
    if run.l1sp_snap_core:
        for snap_idx, _level, core, bytes_used in run.l1sp_snap_core:
            per_snap.setdefault(snap_idx, {})
            per_snap[snap_idx][core] = bytes_used
        return per_snap

    for snap_idx, _level, core, _thread, _hart, bytes_used in run.l1sp_snap_hart:
        per_snap.setdefault(snap_idx, {})
        per_snap[snap_idx][core] = per_snap[snap_idx].get(core, 0) + bytes_used
    return per_snap


def parse_stats_csv(path: str) -> StatsCsvData:
    component_stat_values: Dict[str, Dict[str, float]] = {}
    stat_totals: Dict[str, float] = {}
    core_stat_values: Dict[int, Dict[str, float]] = {}
    hart_stat_values: Dict[int, Dict[str, float]] = {}

    core_re = re.compile(r"core(\d+)_core$")
    hart_re = re.compile(r"hart_(\d+)$")

    with open(path, "r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            component = row["ComponentName"]
            stat = row["StatisticName"]
            value = float(row["Sum.u64"])
            component_stat_values.setdefault(component, {})
            component_stat_values[component][stat] = component_stat_values[component].get(stat, 0.0) + value
            stat_totals[stat] = stat_totals.get(stat, 0.0) + value

            core_match = core_re.search(component)
            if core_match:
                core_id = int(core_match.group(1))
                core_stat_values.setdefault(core_id, {})
                core_stat_values[core_id][stat] = core_stat_values[core_id].get(stat, 0.0) + value

                hart_match = hart_re.match(row["StatisticSubId"])
                if hart_match:
                    hart_id = int(hart_match.group(1))
                    hart_stat_values.setdefault(hart_id, {})
                    hart_stat_values[hart_id][stat] = hart_stat_values[hart_id].get(stat, 0.0) + value

    components = sorted(component_stat_values)
    stat_names = sorted(stat_totals, key=lambda name: (-stat_totals[name], name))
    return StatsCsvData(
        components=components,
        stat_names=stat_names,
        component_stat_values=component_stat_values,
        stat_totals=stat_totals,
        core_stat_values=core_stat_values,
        hart_stat_values=hart_stat_values,
    )


def parse_l2sp_timestamps(run_dir: str) -> Dict[int, List[int]]:
    out: Dict[int, List[int]] = {}
    for name in sorted(os.listdir(run_dir)):
        match = re.match(r"l2sp_timestamps_pxn\d+_pod\d+_core(\d+)\.csv$", name)
        if not match:
            continue
        core = int(match.group(1))
        path = os.path.join(run_dir, name)
        with open(path, "r", encoding="utf-8", newline="") as handle:
            reader = csv.reader(handle)
            next(reader, None)
            out[core] = [int(row[0]) for row in reader if row]
    return out


def parse_ramulator_stats(path: str) -> RamulatorData:
    names: List[str] = []
    values: List[float] = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            content = raw.split("#", 1)[0].strip()
            if not content:
                continue
            parts = content.split()
            if len(parts) < 2:
                continue
            try:
                value = float(parts[1])
            except ValueError:
                continue
            names.append(parts[0])
            values.append(value)
    return RamulatorData(names=names, values=values)


def find_run_log(run_dir: str) -> Optional[str]:
    candidates = [
        os.path.join(run_dir, name)
        for name in sorted(os.listdir(run_dir))
        if name.startswith("run_") and name.endswith(".log")
    ]
    return candidates[0] if candidates else None


def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def sanitize_filename(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", text).strip("_")


def chunked_metrics(metric_names: Sequence[str], max_per_fig: int) -> Iterable[Sequence[str]]:
    for idx in range(0, len(metric_names), max_per_fig):
        yield metric_names[idx:idx + max_per_fig]


def to_log10_matrix(values: Sequence[Sequence[float]]) -> List[List[float]]:
    return [[math.log10(value + 1.0) for value in row] for row in values]
