#!/usr/bin/env python3
"""
compare_results.py
Compare DRV/PANDO BFS vs Ligra BFS_PushOnly.

Produces:
  - MTEPS comparison
  - DRAM bandwidth utilization comparison (weak scaling)
  - DRAM cache efficiency analysis
  - Thread/core scaling analysis

DRV results: output.txt + stats.csv from bfs_csr_weak_results_3/
Ligra results:
  - Strong scaling: ligra_results/ligra_bfs_results.csv
  - Weak scaling + BW: ligra_results_weak/weak_scaling_bw.csv

DRAM bandwidth on CPU is measured via inline perf_event_open() IMC counters
bracketing ONLY the BFS while-loop (not graph loading, init, or stats).
This gives BFS-kernel-only DRAM traffic, comparable to DRV's useful phase.

Usage:
  python3 compare_results.py [options]
"""

import argparse
import csv
import os
import re
import sys
from collections import defaultdict

# =============================================================================
# Architecture constants
# =============================================================================

# DRV/PANDO
DRV_CLOCK_GHZ = 1.0              # 1 GHz => 1 cycle = 1 ns
DRV_DRAM_BANKS = 5               # DRAM banks per PXN
DRV_DRAM_ACCESS_TIME_NS = 40     # SimpleMemory backend latency
DRV_DRAM_INTERLEAVE_BYTES = 64   # Cache line / interleave size
DRV_DRAM_MSHRS_PER_BANK = 32    # MSHRs per DRAM cache bank
# Peak BW limited by MSHRs: 32 outstanding × 64B / 40ns per bank
# Backend is pipelined (1 req/cycle), but MSHRs cap outstanding to 32
DRV_PEAK_BW_PER_BANK_GBS = DRV_DRAM_MSHRS_PER_BANK * DRV_DRAM_INTERLEAVE_BYTES / DRV_DRAM_ACCESS_TIME_NS
# = 32 * 64 / 40 = 51.2 GB/s per bank
DRV_PEAK_BW_GBS = DRV_DRAM_BANKS * DRV_PEAK_BW_PER_BANK_GBS
# = 5 * 51.2 = 256 GB/s

# Stampede3 SKX (Xeon Platinum 8160)
SKX_PEAK_BW_GBS = 128.0          # 1 socket x 6 ch x DDR4-2666 (21.33 GB/s/ch)
                                   # Only socket 0's 6 IMC channels exposed in sysfs
SKX_CORES = 48                    # 2 x 24 cores


# =============================================================================
# DRV Parsing
# =============================================================================

def parse_drv_results(drv_dir):
    """Parse DRV BFS output.txt + stats.csv files.

    Returns list of dicts with BFS timing, DRAM access counts, cache stats,
    useful-phase metrics, and bandwidth.
    """
    results = []
    if not os.path.isdir(drv_dir):
        print(f"WARNING: DRV results directory not found: {drv_dir}", file=sys.stderr)
        return results

    for dirname in sorted(os.listdir(drv_dir)):
        dirpath = os.path.join(drv_dir, dirname)
        outpath = os.path.join(dirpath, "output.txt")
        statpath = os.path.join(dirpath, "stats.csv")
        if not os.path.isfile(outpath):
            continue

        m = re.match(r'bfs_csr_c(\d+)_t(\d+)', dirname)
        if not m:
            continue

        cores = int(m.group(1))
        threads_per_core = int(m.group(2))

        with open(outpath, 'r') as f:
            text = f.read()

        rec = {
            'config': dirname,
            'cores': cores,
            'threads_per_core': threads_per_core,
        }

        # Parse output.txt
        m2 = re.search(r'N=(\d+)\s+E=(\d+)\s+degree=(\d+)', text)
        if m2:
            rec['vertices'] = int(m2.group(1))
            rec['edges'] = int(m2.group(2))
            rec['degree'] = int(m2.group(3))
        else:
            continue

        m3 = re.search(r'Using total_threads=(\d+)', text)
        rec['total_threads'] = int(m3.group(1)) if m3 else cores * threads_per_core

        m4 = re.search(r'BFS done in (\d+) iterations \((\d+) cycles\)', text)
        if m4:
            rec['iterations'] = int(m4.group(1))
            rec['cycles'] = int(m4.group(2))
        else:
            rec['iterations'] = rec['cycles'] = None

        m5 = re.search(r'Reached:\s*(\d+)/(\d+)', text)
        rec['reached'] = int(m5.group(1)) if m5 else None

        m6 = re.search(r'max_dist=(\d+)\s+sum_dist=(\d+)', text)
        if m6:
            rec['max_dist'] = int(m6.group(1))
            rec['sum_dist'] = int(m6.group(2))
        else:
            rec['max_dist'] = rec['sum_dist'] = None

        m7 = re.search(r'simulated time:\s*([\d.]+)\s*ms', text)
        rec['sim_time_ms'] = float(m7.group(1)) if m7 else None

        # Compute BFS cycle time (includes graph gen overhead)
        if rec['cycles'] and rec['edges']:
            rec['bfs_time_sec'] = rec['cycles'] * 1e-9  # 1 GHz
        else:
            rec['bfs_time_sec'] = None

        # Parse stats.csv for detailed memory stats
        _parse_stats_csv(rec, statpath)

        # Compute MTEPS from useful-phase time (BFS traversal only)
        if rec.get('useful_phase_sec') and rec['useful_phase_sec'] > 0 and rec['edges']:
            rec['mteps'] = (rec['edges'] / rec['useful_phase_sec']) / 1e6
        elif rec.get('bfs_time_sec') and rec['edges']:
            # Fallback to total BFS time if useful phase not available
            rec['mteps'] = (rec['edges'] / rec['bfs_time_sec']) / 1e6
        else:
            rec['mteps'] = None

        results.append(rec)

    return results


def _parse_stats_csv(rec, statpath):
    """Parse stats.csv for DRAM accesses, cache stats, and MemController stats."""

    # Core-side DRAM accesses (total phase)
    rec['dram_loads'] = 0
    rec['dram_stores'] = 0
    rec['dram_atomics'] = 0

    # Core-side DRAM accesses (useful phase only — stat_phase=1)
    rec['useful_dram_loads'] = 0
    rec['useful_dram_stores'] = 0
    rec['useful_dram_atomics'] = 0

    # Useful-phase cycle counts per core (to compute useful_phase_duration)
    useful_busy = defaultdict(int)
    useful_memwait = defaultdict(int)
    useful_idle = defaultdict(int)

    # DRAM cache hits/misses
    rec['cache_hits'] = 0
    rec['cache_misses'] = 0

    # MemController DRAM bank stats
    dram_mc_issue = {}    # bank -> cycles_with_issue
    dram_mc_total = {}    # bank -> total_cycles

    if not os.path.isfile(statpath):
        _compute_derived(rec)
        return

    with open(statpath, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            comp = row['ComponentName']
            name = row['StatisticName']
            val = int(row['Sum.u64'])

            # Core stats (exclude cache components)
            if 'core' in comp and '_cache' not in comp:
                # Total phase
                if name == 'load_dram':
                    rec['dram_loads'] += val
                elif name == 'store_dram':
                    rec['dram_stores'] += val
                elif name == 'atomic_dram':
                    rec['dram_atomics'] += val
                # Useful phase
                elif name == 'useful_load_dram':
                    rec['useful_dram_loads'] += val
                elif name == 'useful_store_dram':
                    rec['useful_dram_stores'] += val
                elif name == 'useful_atomic_dram':
                    rec['useful_dram_atomics'] += val
                # Useful-phase cycles (keyed by core component)
                elif name == 'useful_busy_cycles':
                    useful_busy[comp] += val
                elif name == 'useful_memory_wait_cycles':
                    useful_memwait[comp] += val
                elif name == 'useful_active_idle_cycles':
                    useful_idle[comp] += val

            # DRAM cache stats
            if ('dram' in comp and '_cache' in comp) or 'victim_cache' in comp:
                if name == 'CacheHits':
                    rec['cache_hits'] += val
                elif name == 'CacheMisses':
                    rec['cache_misses'] += val

            # MemController DRAM bank stats
            if 'dram' in comp and 'memctrl' in comp and 'l2sp' not in comp and 'l1sp' not in comp:
                if name == 'cycles_with_issue':
                    dram_mc_issue[comp] = val
                elif name == 'total_cycles':
                    dram_mc_total[comp] = val

    # Useful-phase duration = max across cores
    useful_phase_per_core = []
    for core in useful_busy:
        phase = useful_busy[core] + useful_memwait[core] + useful_idle[core]
        if phase > 0:
            useful_phase_per_core.append(phase)
    rec['useful_phase_cycles'] = max(useful_phase_per_core) if useful_phase_per_core else 0

    # Aggregate useful-phase busy/memwait across all cores (average per core)
    num_cores = len(useful_busy) if useful_busy else 1
    rec['useful_busy_cycles_avg'] = sum(useful_busy.values()) / num_cores if useful_busy else 0
    rec['useful_memwait_cycles_avg'] = sum(useful_memwait.values()) / num_cores if useful_busy else 0
    rec['useful_idle_cycles_avg'] = sum(useful_idle.values()) / num_cores if useful_busy else 0
    total_useful = rec['useful_busy_cycles_avg'] + rec['useful_memwait_cycles_avg'] + rec['useful_idle_cycles_avg']
    rec['useful_busy_pct'] = (rec['useful_busy_cycles_avg'] / total_useful * 100) if total_useful > 0 else 0
    rec['useful_memwait_pct'] = (rec['useful_memwait_cycles_avg'] / total_useful * 100) if total_useful > 0 else 0

    # MemController aggregated stats
    if dram_mc_issue and dram_mc_total:
        total_issue = sum(dram_mc_issue.values())
        total_cyc = sum(dram_mc_total.values())
        num_banks = len(dram_mc_issue)
        rec['num_dram_banks'] = num_banks
        # Average per-bank utilization
        utils = []
        for bank in dram_mc_issue:
            tc = dram_mc_total.get(bank, 1)
            utils.append(dram_mc_issue[bank] / tc * 100)
        rec['memctrl_util_pct'] = sum(utils) / len(utils) if utils else 0
    else:
        rec['num_dram_banks'] = DRV_DRAM_BANKS
        rec['memctrl_util_pct'] = 0

    _compute_derived(rec)


def _compute_derived(rec):
    """Compute derived metrics from parsed raw data."""

    # Total core-side DRAM accesses
    total_core_dram = rec['dram_loads'] + rec['dram_stores'] + rec['dram_atomics']
    rec['total_core_dram_accesses'] = total_core_dram

    # Useful-phase core-side DRAM accesses
    useful_core_dram = rec['useful_dram_loads'] + rec['useful_dram_stores'] + rec['useful_dram_atomics']
    rec['useful_core_dram_accesses'] = useful_core_dram

    # DRAM cache stats
    total_cache = rec['cache_hits'] + rec['cache_misses']
    rec['cache_hit_rate'] = (rec['cache_hits'] / total_cache * 100) if total_cache > 0 else 0

    # Useful-phase duration
    rec['useful_phase_sec'] = rec.get('useful_phase_cycles', 0) * 1e-9 if rec.get('useful_phase_cycles') else None

    # Actual DRAM bandwidth = cache misses * interleave_bytes / time
    # Cache misses represent actual DRAM bank accesses (64 bytes each)
    rec['actual_dram_bytes'] = rec['cache_misses'] * DRV_DRAM_INTERLEAVE_BYTES

    # Bandwidth over total sim time (from output.txt BFS cycles)
    if rec.get('bfs_time_sec') and rec['cache_misses'] > 0:
        rec['actual_dram_bw_gbs'] = rec['actual_dram_bytes'] / rec['bfs_time_sec'] / 1e9
    else:
        rec['actual_dram_bw_gbs'] = None

    # Bandwidth over useful phase (more accurate for BFS-only measurement)
    if rec.get('useful_phase_sec') and rec['useful_phase_sec'] > 0 and useful_core_dram > 0:
        # Estimate useful-phase cache misses using overall hit rate
        miss_rate = (1 - rec['cache_hit_rate'] / 100) if rec['cache_hit_rate'] < 100 else 1.0
        est_useful_misses = useful_core_dram * miss_rate
        rec['useful_dram_bw_gbs'] = (est_useful_misses * DRV_DRAM_INTERLEAVE_BYTES) / rec['useful_phase_sec'] / 1e9
    else:
        rec['useful_dram_bw_gbs'] = None

    # MemController useful-phase utilization estimate
    # (core-side useful DRAM requests per bank per cycle)
    num_banks = rec.get('num_dram_banks', DRV_DRAM_BANKS)
    if rec.get('useful_phase_cycles') and rec.get('useful_phase_cycles') > 0 and useful_core_dram > 0:
        reqs_per_bank = useful_core_dram / num_banks
        rec['memctrl_uutil_pct'] = reqs_per_bank / rec['useful_phase_cycles'] * 100
    else:
        rec['memctrl_uutil_pct'] = 0

    # DRV utilization = actual DRAM BW / peak BW
    rec['drv_peak_bw_gbs'] = DRV_PEAK_BW_GBS
    if rec.get('useful_dram_bw_gbs') and rec['useful_dram_bw_gbs'] > 0:
        rec['bw_utilization_pct'] = rec['useful_dram_bw_gbs'] / DRV_PEAK_BW_GBS * 100
    else:
        rec['bw_utilization_pct'] = None


# =============================================================================
# Ligra Parsing
# =============================================================================

def parse_ligra_strong_csv(csv_path):
    """Parse Ligra strong-scaling results CSV."""
    results = []
    if not os.path.isfile(csv_path):
        return results
    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                rec = {
                    'graph_type': row['graph_type'],
                    'vertices': int(row['vertices']),
                    'edges': int(row['edges']),
                    'degree': int(row['degree']),
                    'threads': int(row['threads']),
                    'time_sec': float(row['time_sec']),
                }
                rec['mteps'] = (rec['edges'] / rec['time_sec']) / 1e6 if rec['time_sec'] > 0 else 0
                results.append(rec)
            except (ValueError, KeyError):
                pass
    return results


def parse_ligra_weak_csv(csv_path):
    """Parse Ligra weak-scaling + bandwidth CSV from ligra_bfs_weak_scaling.sbatch.

    bfs_time_sec = BFS kernel only (while-loop), measured via clock_gettime
    dram_{read,write}_bytes = inline IMC counters bracketing BFS kernel only
    """
    results = []
    if not os.path.isfile(csv_path):
        return results
    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                # Support both old ('threads') and new ('cores') column names
                cores = int(row.get('cores') or row.get('threads', 0))
                rec = {
                    'cores': cores,
                    'threads': cores,  # SKX: 1 thread per core
                    'vertices': int(row['vertices']),
                    'edges': int(row['edges']),
                    'degree': int(row['degree']),
                    'vtx_per_core': int(row.get('vtx_per_core') or row.get('vtx_per_thread', 16384)),
                    'bfs_time_sec': float(row['bfs_time_sec']),
                    'dram_read_bytes': int(row['dram_read_bytes']),
                    'dram_write_bytes': int(row['dram_write_bytes']),
                    'dram_total_bytes': int(row['dram_total_bytes']),
                    'dram_bw_gbs': float(row['dram_bw_gbs']),
                    'bw_utilization_pct': float(row['bw_utilization_pct']),
                    'iterations': int(row.get('iterations', 0) or 0),
                    'reached': int(row.get('reached', 0) or 0),
                    'max_dist': int(row.get('max_dist', 0) or 0),
                    'sum_dist': int(row.get('sum_dist', 0) or 0),
                }
                rec['mteps'] = (rec['edges'] / rec['bfs_time_sec']) / 1e6 if rec['bfs_time_sec'] > 0 else 0
                results.append(rec)
            except (ValueError, KeyError) as e:
                print(f"WARNING: skipping row: {e}", file=sys.stderr)
    return results


def aggregate_strong(results):
    """Aggregate strong-scaling results: median time per (graph_type, vertices, threads)."""
    grouped = defaultdict(list)
    for r in results:
        key = (r['graph_type'], r['vertices'], r['threads'])
        grouped[key].append(r['time_sec'])

    agg = []
    for (gtype, vertices, threads), times in sorted(grouped.items()):
        times.sort()
        n = len(times)
        median = times[n // 2]
        edges = vertices * 16
        agg.append({
            'graph_type': gtype,
            'vertices': vertices,
            'threads': threads,
            'median_time': median,
            'min_time': times[0],
            'max_time': times[-1],
            'num_rounds': n,
            'mteps': (edges / median) / 1e6 if median > 0 else 0,
            'edges': edges,
        })
    return agg


# =============================================================================
# Pairing helpers
# =============================================================================

def build_pairings(drv, ligra_weak, drv_filter=None):
    """Build (drv_rec, ligra_rec, match_type) pairings for comparison.

    First matches by vertex count ('exact'), then pairs remaining DRV entries
    with the Ligra entry having the nearest core count ('nearest').
    Returns list sorted by DRV vertex count.
    """
    if drv_filter:
        drv_by_v = {r['vertices']: r for r in drv if drv_filter(r)}
    else:
        drv_by_v = {r['vertices']: r for r in drv if r.get('useful_phase_sec')}
    ligra_by_v = {r['vertices']: r for r in ligra_weak}
    ligra_by_c = {r['cores']: r for r in ligra_weak}

    pairs = []
    matched_drv = set()

    # 1) Exact vertex-count matches
    for v in sorted(set(drv_by_v.keys()) & set(ligra_by_v.keys())):
        pairs.append((drv_by_v[v], ligra_by_v[v], 'exact'))
        matched_drv.add(v)

    # 2) Unmatched DRV → nearest Ligra core count
    ligra_cores = sorted(ligra_by_c.keys())
    for v in sorted(drv_by_v.keys()):
        if v in matched_drv:
            continue
        d = drv_by_v[v]
        # Find nearest Ligra core count
        best = min(ligra_cores, key=lambda c: abs(c - d['cores']))
        pairs.append((d, ligra_by_c[best], 'nearest'))

    return pairs


# =============================================================================
# Display Functions
# =============================================================================

def print_drv_table(drv):
    """Print DRV results with DRAM bandwidth and cache stats."""
    print("\n" + "=" * 150)
    print(f"DRV/PANDO BFS Results (1 GHz, {DRV_DRAM_BANKS} DRAM banks, "
          f"{DRV_DRAM_ACCESS_TIME_NS}ns access, {DRV_DRAM_INTERLEAVE_BYTES}B interleave, "
          f"peak={DRV_PEAK_BW_GBS:.0f} GB/s)")
    print("=" * 150)
    print(f"{'Config':<18} {'Vertices':>10} {'Edges':>10} {'Cores':>6} "
          f"{'uPhase(ms)':>11} {'MTEPS':>8} "
          f"{'CacheHit%':>10} {'CacheMiss':>10} {'uDRAM BW':>10} {'uUtil%':>7} "
          f"{'MCUtil%':>8}")
    print("-" * 150)

    for r in sorted(drv, key=lambda x: x['vertices']):
        if not r['cycles']:
            print(f"{r['config']:<18} {r['vertices']:>10,} {r['edges']:>10,} "
                  f"{r['cores']:>6} {'(incomplete)':>9}")
            continue

        uphase_ms = r.get('useful_phase_cycles', 0) * 1e-6 if r.get('useful_phase_cycles') else 0
        ubw = r.get('useful_dram_bw_gbs') or 0
        uutil = r.get('memctrl_uutil_pct') or 0
        mcutil = r.get('memctrl_util_pct') or 0
        hit_rate = r.get('cache_hit_rate', 0)
        misses = r.get('cache_misses', 0)

        print(f"{r['config']:<18} {r['vertices']:>10,} {r['edges']:>10,} "
              f"{r['cores']:>6} {uphase_ms:>11.2f} {r['mteps']:>8.1f} "
              f"{hit_rate:>9.1f}% {misses:>10,} {ubw:>10.2f} {uutil:>6.1f}% "
              f"{mcutil:>7.1f}%")

    print()
    print("  uPhase(ms) = useful phase time (BFS traversal only, from stat_phase)")
    print("  MTEPS = edges / uPhase (useful-phase based)")
    print("  CacheHit% = DRAM cache hit rate | CacheMiss = actual DRAM backend accesses")
    print("  uDRAM BW = estimated DRAM BW during useful phase (GB/s)")
    print("  uUtil% = core-side DRAM request rate per bank (useful phase)")
    print("  MCUtil% = MemController avg bank utilization (total sim)")
    print()


def print_ligra_weak_table(ligra_weak):
    """Print Ligra weak-scaling results with bandwidth."""
    if not ligra_weak:
        return

    print("\n" + "=" * 120)
    print(f"Ligra BFS_PushOnly -- Weak Scaling (16384 vtx/core, degree=16, SKX peak={SKX_PEAK_BW_GBS:.0f} GB/s)")
    print(f"  DRAM: inline IMC counters (perf_event_open), BFS kernel only")
    print("=" * 120)
    print(f"{'Cores':>6} {'Vertices':>10} {'Edges':>10} "
          f"{'BFS(ms)':>10} {'MTEPS':>8} "
          f"{'DRAM_R(KB)':>11} {'DRAM_W(KB)':>11} {'BW(GB/s)':>10} {'Util%':>7} "
          f"{'Iters':>6} {'Reached':>8} {'MaxDist':>8}")
    print("-" * 120)

    for r in sorted(ligra_weak, key=lambda x: x['cores']):
        bfs_ms = r['bfs_time_sec'] * 1e3
        dr = r['dram_read_bytes'] / 1e6
        dw = r['dram_write_bytes'] / 1e6
        print(f"{r['cores']:>6} {r['vertices']:>10,} {r['edges']:>10,} "
              f"{bfs_ms:>10.3f} {r['mteps']:>8.1f} "
              f"{dr:>11.2f} {dw:>11.2f} {r['dram_bw_gbs']:>10.3f} {r['bw_utilization_pct']:>6.2f}% "
              f"{r['iterations']:>6} {r['reached']:>8,} {r['max_dist']:>8}")
    print()
    print("  BFS(ms) = BFS kernel only (while-loop wall time, excludes init/stats)")
    print("  DRAM_R/W = inline IMC cas_count_{read,write} x 64B, BFS kernel only")
    print(f"  BW = (read+write) / bfs_time | Util% = BW / {SKX_PEAK_BW_GBS:.0f} GB/s peak")
    print()


def print_ligra_strong_table(ligra_agg):
    """Print Ligra strong-scaling aggregated results."""
    if not ligra_agg:
        return

    print("\n" + "=" * 95)
    print("Ligra BFS_PushOnly -- Strong Scaling Results")
    print("=" * 95)
    print(f"{'Type':<8} {'Vertices':>10} {'Threads':>8} {'Rounds':>7} "
          f"{'Median(s)':>12} {'Min(s)':>10} {'Max(s)':>10} {'MTEPS':>10}")
    print("-" * 95)

    for r in ligra_agg:
        print(f"{r['graph_type']:<8} {r['vertices']:>10,} {r['threads']:>8} "
              f"{r['num_rounds']:>7} {r['median_time']:>12.6f} "
              f"{r['min_time']:>10.6f} {r['max_time']:>10.6f} {r['mteps']:>10.1f}")
    print()


def print_comprehensive_comparison(drv, ligra_weak):
    """Print comprehensive side-by-side DRV vs Ligra comparison table."""
    if not drv or not ligra_weak:
        return

    pairs = build_pairings(drv, ligra_weak)
    if not pairs:
        return

    W = 215
    print("\n" + "=" * W)
    print("Comprehensive Comparison: DRV/PANDO vs CPU (uniform random, degree=16, weak scaling)")
    print("=" * W)

    # Header row 1: group labels
    print(f"{'':>10} {'':>10} |"
          f"{'--- DRV/PANDO (1 GHz, simulated) ---':^62}|"
          f"{'--- CPU / Ligra (SKX 2x24c) ---':^52}|"
          f"{'--- Ratios (DRV/CPU) ---':^33}|"
          f"{'':>5}")

    # Header row 2: column names
    print(f"{'Vertices':>10} {'Edges':>10} |"
          f" {'Cores':>5} {'Time(ms)':>9} {'MTEPS':>8}"
          f" {'CacheHit':>9} {'Misses':>9} {'BW(GB/s)':>9} {'Util%':>6}"
          f" {'DRAM(MB)':>9} |"
          f" {'Cores':>5} {'Time(ms)':>9} {'MTEPS':>8}"
          f" {'BW(GB/s)':>9} {'Util%':>6}"
          f" {'DRAM(MB)':>9} |"
          f" {'MTEPS':>7} {'BW':>7} {'Util':>7} {'Time':>7} |"
          f" {'Match':>5}")
    print("-" * W)

    avg_mteps_ratio = []
    avg_bw_ratio = []
    avg_util_ratio = []
    avg_time_ratio = []

    for d, l, match_type in pairs:
        # DRV metrics
        d_ms = d.get('useful_phase_sec', 0) * 1e3
        d_mteps = d.get('mteps') or 0
        d_cache_hit = d.get('cache_hit_rate', 0)
        d_misses = d.get('cache_misses', 0)
        d_bw = d.get('useful_dram_bw_gbs') or 0
        d_util = d.get('bw_utilization_pct') or 0
        d_dram_mb = d.get('cache_misses', 0) * DRV_DRAM_INTERLEAVE_BYTES / 1e6

        # CPU metrics
        l_ms = l['bfs_time_sec'] * 1e3
        l_mteps = l.get('mteps') or 0
        l_bw = l['dram_bw_gbs']
        l_util = l['bw_utilization_pct']
        l_dram_mb = l['dram_total_bytes'] / 1e6

        # Ratios
        mteps_ratio = d_mteps / l_mteps if l_mteps > 0 else 0
        bw_ratio = d_bw / l_bw if l_bw > 0 else 0
        util_ratio = d_util / l_util if l_util > 0 else 0
        time_ratio = l_ms / d_ms if d_ms > 0 else 0

        match_mark = "" if match_type == 'exact' else "*"

        print(f"{d['vertices']:>10,} {d['edges']:>10,} |"
              f" {d['cores']:>5} {d_ms:>9.2f} {d_mteps:>8.1f}"
              f" {d_cache_hit:>8.1f}% {d_misses:>9,} {d_bw:>9.2f} {d_util:>5.1f}%"
              f" {d_dram_mb:>9.1f} |"
              f" {l['cores']:>5} {l_ms:>9.3f} {l_mteps:>8.1f}"
              f" {l_bw:>9.2f} {l_util:>5.1f}%"
              f" {l_dram_mb:>9.1f} |"
              f" {mteps_ratio:>6.2f}x {bw_ratio:>6.2f}x {util_ratio:>6.2f}x {time_ratio:>6.1f}x |"
              f" {match_mark:>5}")

        avg_mteps_ratio.append(mteps_ratio)
        avg_bw_ratio.append(bw_ratio)
        avg_util_ratio.append(util_ratio)
        avg_time_ratio.append(time_ratio)

    print("-" * W)

    def _avg(lst):
        return sum(lst) / len(lst) if lst else 0

    print(f"{'':>10} {'Avg Ratio':>10} |"
          f"{'':>62}|"
          f"{'':>52}|"
          f" {_avg(avg_mteps_ratio):>6.2f}x {_avg(avg_bw_ratio):>6.2f}x"
          f" {_avg(avg_util_ratio):>6.2f}x {_avg(avg_time_ratio):>6.1f}x |"
          f"{'':>6}")

    print()
    print("  DRV columns:")
    print(f"    Time(ms) = useful phase only (BFS traversal, stat_phase=1) | MTEPS = edges / useful_phase_time")
    print(f"    CacheHit = DRAM cache hit rate | Misses = actual DRAM backend accesses (64B each)")
    print(f"    BW = estimated DRAM bandwidth during useful phase (after cache) | Util% = BW / {DRV_PEAK_BW_GBS:.0f} GB/s peak")
    print(f"    DRAM(MB) = cache_misses x {DRV_DRAM_INTERLEAVE_BYTES}B = actual DRAM traffic")
    print(f"    Peak: {DRV_DRAM_BANKS} banks x {DRV_DRAM_MSHRS_PER_BANK} MSHRs x {DRV_DRAM_INTERLEAVE_BYTES}B / {DRV_DRAM_ACCESS_TIME_NS}ns = {DRV_PEAK_BW_GBS:.0f} GB/s")
    print("  CPU columns:")
    print(f"    Time(ms) = BFS kernel only (while-loop, excludes init/stats) | MTEPS = edges / bfs_time")
    print(f"    BW = inline IMC counters, BFS kernel only (read+write) | Util% = BW / {SKX_PEAK_BW_GBS:.0f} GB/s peak")
    print(f"    DRAM(MB) = total DRAM read + write traffic (BFS kernel only)")
    print("  Ratios:")
    print("    MTEPS = DRV/CPU (higher=DRV better) | BW = DRV/CPU DRAM bandwidth")
    print("    Util = DRV/CPU utilization | Time = CPU_time/DRV_time (higher=DRV slower)")
    print("  Match: * = nearest core count (different graph sizes); blank = exact vertex match")
    print()


def print_utilization_scaling(drv, ligra_weak):
    """Print side-by-side DRAM utilization comparison."""
    if not drv or not ligra_weak:
        return

    pairs = build_pairings(drv, ligra_weak)
    if not pairs:
        return

    W = 200
    print("\n" + "=" * W)
    print(f"DRAM Utilization Comparison: DRV/PANDO vs CPU (weak scaling, peak = {DRV_PEAK_BW_GBS:.0f} GB/s both)")
    print("=" * W)

    # Header
    print(f"{'':>10} {'':>6} {'':>6} |"
          f"{'--- DRV/PANDO DRAM (after cache) ---':^55}|"
          f"{'--- CPU DRAM (inline IMC, BFS kernel) ---':^52}|"
          f"{'--- DRV / CPU ---':^24}|")

    print(f"{'Vertices':>10} {'DRV c':>6} {'CPU c':>6} |"
          f" {'Traffic':>10} {'Read':>9} {'Write':>9} {'BW':>9} {'Util%':>6}"
          f" {'CacheHit':>9} {'B/Edge':>7} |"
          f" {'Traffic':>10} {'Read':>9} {'Write':>9} {'BW':>9} {'Util%':>6}"
          f" {'B/Edge':>7} |"
          f" {'Traffic':>8} {'Util':>6} {'B/E':>6} |")
    print(f"{'':>10} {'':>6} {'':>6} |"
          f" {'(MB)':>10} {'(MB)':>9} {'(MB)':>9} {'(GB/s)':>9} {'':>6}"
          f" {'':>9} {'':>7} |"
          f" {'(MB)':>10} {'(MB)':>9} {'(MB)':>9} {'(GB/s)':>9} {'':>6}"
          f" {'':>7} |"
          f" {'ratio':>8} {'ratio':>6} {'ratio':>6} |")
    print("-" * W)

    for d, l, match_type in pairs:
        # DRV: actual DRAM traffic after cache
        d_miss = d.get('cache_misses', 0)
        d_traffic_mb = d_miss * DRV_DRAM_INTERLEAVE_BYTES / 1e6
        d_read_mb = d_traffic_mb
        d_write_mb = 0
        d_bw = d.get('useful_dram_bw_gbs') or 0
        d_util = d.get('bw_utilization_pct') or 0
        d_cache_hit = d.get('cache_hit_rate', 0)
        d_bytes_per_edge = (d_traffic_mb * 1e6) / d['edges'] if d['edges'] > 0 else 0

        # CPU: measured DRAM traffic
        l_read_mb = l['dram_read_bytes'] / 1e6
        l_write_mb = l['dram_write_bytes'] / 1e6
        l_traffic_mb = l['dram_total_bytes'] / 1e6
        l_bw = l['dram_bw_gbs']
        l_util = l['bw_utilization_pct']
        l_bytes_per_edge = (l_traffic_mb * 1e6) / l['edges'] if l['edges'] > 0 else 0

        # Ratios
        traffic_ratio = d_traffic_mb / l_traffic_mb if l_traffic_mb > 0 else 0
        util_ratio = d_util / l_util if l_util > 0 else 0
        bpe_ratio = d_bytes_per_edge / l_bytes_per_edge if l_bytes_per_edge > 0 else 0

        mark = "*" if match_type == 'nearest' else ""
        print(f"{d['vertices']:>10,} {d['cores']:>5}{mark} {l['cores']:>6} |"
              f" {d_traffic_mb:>10.1f} {d_read_mb:>9.1f} {d_write_mb:>9.1f} {d_bw:>9.2f} {d_util:>5.1f}%"
              f" {d_cache_hit:>8.1f}% {d_bytes_per_edge:>7.1f} |"
              f" {l_traffic_mb:>10.1f} {l_read_mb:>9.1f} {l_write_mb:>9.1f} {l_bw:>9.2f} {l_util:>5.1f}%"
              f" {l_bytes_per_edge:>7.1f} |"
              f" {traffic_ratio:>7.2f}x {util_ratio:>5.2f}x {bpe_ratio:>5.2f}x |")

    print("-" * W)

    # Summary: compute bytes/MTEPS (DRAM efficiency)
    print()
    print("  Legend:")
    print(f"    Traffic = total DRAM data moved (MB) | BW = bandwidth (GB/s) | Util% = BW / {DRV_PEAK_BW_GBS:.0f} GB/s peak")
    print("    B/Edge = bytes of DRAM traffic per graph edge (lower = more cache-efficient)")
    print("    DRV: traffic = cache_misses x 64B (read-fills from DRAM backend, useful-phase estimate)")
    print("    CPU: traffic = inline IMC cas_count_{read,write} x 64B, BFS kernel only (perf_event_open)")
    print("    CacheHit = DRV DRAM cache hit rate (no equivalent on CPU — LLC stats not collected)")
    print("    Traffic ratio < 1 means DRV moves less data | Util ratio < 1 means DRV utilizes less BW")
    print("    * = nearest core-count match (different graph sizes)")
    print()


def print_cache_analysis(drv):
    """Print DRAM cache efficiency across core counts."""
    if not drv:
        return

    completed = [r for r in drv if r['cycles']]
    if not completed:
        return

    print("\n" + "=" * 100)
    print("DRV DRAM Cache Analysis (weak scaling)")
    print("=" * 100)
    print(f"  {'Cores':>6} {'Vertices':>10} {'CoreDRAM Reqs':>14} {'uCoreDRAM':>12} "
          f"{'CacheHits':>12} {'CacheMiss':>12} {'HitRate%':>9} "
          f"{'Amplification':>14}")
    print(f"  {'-' * 95}")

    for r in sorted(completed, key=lambda x: x['cores']):
        total_reqs = r.get('total_core_dram_accesses', 0)
        useful_reqs = r.get('useful_core_dram_accesses', 0)
        hits = r.get('cache_hits', 0)
        misses = r.get('cache_misses', 0)
        hit_rate = r.get('cache_hit_rate', 0)
        # Amplification: ratio of core-side requests to actual DRAM accesses
        amp = total_reqs / misses if misses > 0 else 0

        print(f"  {r['cores']:>6} {r['vertices']:>10,} {total_reqs:>14,} {useful_reqs:>12,} "
              f"{hits:>12,} {misses:>12,} {hit_rate:>8.1f}% "
              f"{amp:>13.1f}x")

    print()
    print("  CoreDRAM Reqs = total core-side DRAM address space requests (8B each)")
    print("  uCoreDRAM = useful-phase only (stat_phase=1)")
    print("  CacheMiss = actual DRAM bank accesses (64B each)")
    print("  Amplification = core requests / cache misses (higher = more cache reuse)")
    print()


def print_mteps_comparison(drv, ligra_weak):
    """Print MTEPS side-by-side with scaling efficiency."""
    if not drv or not ligra_weak:
        return

    pairs = build_pairings(drv, ligra_weak, drv_filter=lambda r: r.get('mteps'))
    if not pairs:
        return

    # Get single-core baselines for scaling efficiency
    drv_base_mteps = pairs[0][0]['mteps']
    drv_base_cores = pairs[0][0]['cores']
    ligra_base_mteps = pairs[0][1]['mteps']
    ligra_base_cores = pairs[0][1]['cores']

    print("\n" + "=" * 145)
    print("MTEPS & Weak Scaling Efficiency")
    print("=" * 145)
    print(f"{'DRV Vtx':>10} {'CPU Vtx':>10} {'Edges':>12} | "
          f"{'DRV c':>5} {'DRV ms':>9} {'DRV MTEPS':>10} {'DRV Eff%':>9} | "
          f"{'CPU c':>5} {'CPU ms':>9} {'CPU MTEPS':>10} {'CPU Eff%':>9} | "
          f"{'MTEPS':>7} {'Eff':>7}")
    print(f"{'':>10} {'':>10} {'':>12} | "
          f"{'':>5} {'':>9} {'':>10} {'(ideal=N)':>9} | "
          f"{'':>5} {'':>9} {'':>10} {'(ideal=N)':>9} | "
          f"{'DRV/CPU':>7} {'DRV/CPU':>7}")
    print("-" * 145)

    for d, l, match_type in pairs:
        drv_ms = d.get('useful_phase_sec', d['bfs_time_sec']) * 1e3

        d_core_ratio = d['cores'] / drv_base_cores
        l_core_ratio = l['cores'] / ligra_base_cores
        d_eff = (d['mteps'] / (drv_base_mteps * d_core_ratio) * 100) if drv_base_mteps > 0 and d_core_ratio > 0 else 0
        l_eff = (l['mteps'] / (ligra_base_mteps * l_core_ratio) * 100) if ligra_base_mteps > 0 and l_core_ratio > 0 else 0

        ratio = d['mteps'] / l['mteps'] if l['mteps'] > 0 else float('inf')
        eff_ratio = d_eff / l_eff if l_eff > 0 else float('inf')

        mark = "*" if match_type == 'nearest' else ""
        print(f"{d['vertices']:>10,} {l['vertices']:>10,} {d['edges']:>12,} | "
              f"{d['cores']:>4}{mark} {drv_ms:>9.2f} {d['mteps']:>10.1f} {d_eff:>8.1f}% | "
              f"{l['cores']:>5} {l['bfs_time_sec']*1e3:>9.3f} {l['mteps']:>10.1f} {l_eff:>8.1f}% | "
              f"{ratio:>6.2f}x {eff_ratio:>6.2f}x")

    print()
    print("  Scaling Eff% = actual MTEPS / (base_MTEPS x core_multiplier) x 100")
    print(f"  Base: DRV={drv_base_mteps:.1f} MTEPS @ {drv_base_cores} core | "
          f"CPU={ligra_base_mteps:.1f} MTEPS @ {ligra_base_cores} core")
    print("  Eff DRV/CPU = DRV efficiency / CPU efficiency (>1 means DRV scales better)")
    print("  * = nearest core-count match (different graph sizes)")
    print()


SKX_CLOCK_GHZ = 2.1  # Xeon Platinum 8160


def print_frequency_analysis(drv, ligra_weak):
    """Analyze the impact of clock frequency difference (DRV 1 GHz vs CPU 2.1 GHz)."""
    if not drv or not ligra_weak:
        return

    pairs = build_pairings(drv, ligra_weak,
                           drv_filter=lambda r: r.get('useful_phase_sec') and r.get('mteps'))
    if not pairs:
        return

    W = 180
    print("\n" + "=" * W)
    print(f"Frequency-Normalized Analysis: DRV ({DRV_CLOCK_GHZ} GHz) vs CPU ({SKX_CLOCK_GHZ} GHz)")
    print("=" * W)

    print(f"{'':>10} {'':>6} {'':>6} |"
          f"{'--- Measured ---':^30}|"
          f"{'--- Per-Cycle (freq-independent) ---':^40}|"
          f"{'--- DRV projected @ 2.1 GHz (Amdahl) ---':^45}|"
          f"{'--- vs CPU ---':^18}|")
    print(f"{'Vertices':>10} {'DRV c':>6} {'CPU c':>6} |"
          f" {'DRV ms':>8} {'CPU ms':>8} {'DRV MTEPS':>10} |"
          f" {'DRV E/cyc':>10} {'CPU E/cyc':>10} {'E/cyc ratio':>12} |"
          f" {'uBusy%':>7} {'uMemW%':>7} {'Proj ms':>8} {'Proj MTEPS':>11} {'Speedup':>8} |"
          f" {'Proj/CPU':>8} {'Gap':>6} |")
    print("-" * W)

    for d, l, match_type in pairs:
        d_ms = d['useful_phase_sec'] * 1e3
        l_ms = l['bfs_time_sec'] * 1e3
        d_mteps = d['mteps']
        l_mteps = l['mteps']
        edges = d['edges']

        # Per-cycle throughput (frequency-independent)
        d_cycles = d['useful_phase_cycles']
        l_cycles = l['bfs_time_sec'] * SKX_CLOCK_GHZ * 1e9
        d_edges_per_cycle = edges / d_cycles if d_cycles > 0 else 0
        l_edges_per_cycle = l['edges'] / l_cycles if l_cycles > 0 else 0
        epc_ratio = d_edges_per_cycle / l_edges_per_cycle if l_edges_per_cycle > 0 else 0

        # Amdahl's law projection
        busy_pct = d.get('useful_busy_pct', 0) / 100
        memw_pct = d.get('useful_memwait_pct', 0) / 100

        busy_time_ms = d_ms * busy_pct
        memw_time_ms = d_ms * memw_pct
        idle_time_ms = d_ms * (1 - busy_pct - memw_pct)

        freq_ratio = SKX_CLOCK_GHZ / DRV_CLOCK_GHZ
        proj_busy_ms = busy_time_ms / freq_ratio
        proj_memw_ms = memw_time_ms
        proj_idle_ms = idle_time_ms / freq_ratio
        proj_ms = proj_busy_ms + proj_memw_ms + proj_idle_ms
        proj_mteps = (edges / (proj_ms * 1e-3)) / 1e6 if proj_ms > 0 else 0
        speedup = d_ms / proj_ms if proj_ms > 0 else 0

        proj_vs_cpu = proj_mteps / l_mteps if l_mteps > 0 else 0

        mark = "*" if match_type == 'nearest' else ""
        print(f"{d['vertices']:>10,} {d['cores']:>5}{mark} {l['cores']:>6} |"
              f" {d_ms:>8.2f} {l_ms:>8.3f} {d_mteps:>10.1f} |"
              f" {d_edges_per_cycle:>10.4f} {l_edges_per_cycle:>10.4f} {epc_ratio:>11.2f}x |"
              f" {d.get('useful_busy_pct', 0):>6.1f}% {d.get('useful_memwait_pct', 0):>6.1f}%"
              f" {proj_ms:>8.2f} {proj_mteps:>11.1f} {speedup:>7.2f}x |"
              f" {proj_vs_cpu:>7.2f}x {'':>6}|")

    print("-" * W)
    print()
    print(f"  Per-Cycle: E/cyc = edges / cycles (DRV @ {DRV_CLOCK_GHZ} GHz, CPU @ {SKX_CLOCK_GHZ} GHz)")
    print(f"    CPU cycles estimated from BFS kernel time x {SKX_CLOCK_GHZ} GHz")
    print(f"    E/cyc ratio = DRV / CPU (>1 means DRV does more work per clock cycle)")
    print(f"  Amdahl Projection: what if DRV ran at {SKX_CLOCK_GHZ} GHz?")
    print(f"    Compute time scales with frequency (busy + idle portions)")
    print(f"    Memory stall time stays FIXED (DRAM latency = {DRV_DRAM_ACCESS_TIME_NS}ns, independent of core clock)")
    print(f"    uBusy% / uMemW% = fraction of useful-phase cycles spent computing vs waiting on memory")
    print(f"    Speedup = measured_time / projected_time")
    print(f"    Proj/CPU = projected DRV MTEPS / actual CPU MTEPS (>1 means DRV would be faster)")
    print()


def print_algorithm_verification(drv, ligra_weak):
    """Verify DRV and Ligra produce identical BFS results."""
    if not drv or not ligra_weak:
        return

    drv_by_v = {r['vertices']: r for r in drv if r['reached']}
    ligra_by_v = {r['vertices']: r for r in ligra_weak if r['reached']}
    common = sorted(set(drv_by_v.keys()) & set(ligra_by_v.keys()))

    if not common:
        return

    print("\n" + "=" * 80)
    print("Algorithm Verification (DRV vs Ligra distance distributions)")
    print("=" * 80)
    print(f"{'Vertices':>10} | {'DRV reached':>12} {'DRV max':>8} {'DRV sum':>12} | "
          f"{'Ligra reached':>14} {'Ligra max':>10} {'Ligra sum':>12} | {'Match':>6}")
    print("-" * 80)

    all_match = True
    for v in common:
        d = drv_by_v[v]
        l = ligra_by_v[v]
        match = (d['reached'] == l['reached'] and
                 d['max_dist'] == l['max_dist'] and
                 d['sum_dist'] == l['sum_dist'])
        if not match:
            all_match = False
        print(f"{v:>10,} | {d['reached']:>12,} {d['max_dist']:>8} {d['sum_dist']:>12,} | "
              f"{l['reached']:>14,} {l['max_dist']:>10} {l['sum_dist']:>12,} | "
              f"{'OK' if match else 'MISMATCH':>6}")

    print(f"\n  {'ALL MATCH' if all_match else 'SOME MISMATCHES FOUND'}")
    print()


# =============================================================================
# Main
# =============================================================================

def _update_peak_bw(drv_bw, cpu_bw):
    global DRV_PEAK_BW_GBS, SKX_PEAK_BW_GBS
    DRV_PEAK_BW_GBS = drv_bw
    SKX_PEAK_BW_GBS = cpu_bw


def main():
    base = '/work2/10238/vineeth_architect/stampede3/drv_copy'

    parser = argparse.ArgumentParser(description="Compare DRV and Ligra BFS results")
    parser.add_argument('--drv-dir',
        default=f'{base}/drv/build_stampede/drvr/bfs_csr_weak_results_3',
        help='DRV BFS results directory')
    parser.add_argument('--ligra-strong-csv',
        default=f'{base}/ligra_results/ligra_bfs_results.csv',
        help='Ligra strong-scaling CSV')
    parser.add_argument('--ligra-weak-csv',
        default=f'{base}/ligra_results_weak/weak_scaling_bw.csv',
        help='Ligra weak-scaling + bandwidth CSV')
    parser.add_argument('--drv-peak-bw',
        type=float, default=DRV_PEAK_BW_GBS,
        help=f'DRV peak DRAM BW in GB/s (default: {DRV_PEAK_BW_GBS})')
    parser.add_argument('--cpu-peak-bw',
        type=float, default=SKX_PEAK_BW_GBS,
        help=f'CPU peak DRAM BW in GB/s (default: {SKX_PEAK_BW_GBS})')
    args = parser.parse_args()

    # Allow overriding peak BW via module-level vars
    _update_peak_bw(args.drv_peak_bw, args.cpu_peak_bw)

    print("=" * 70)
    print("DRV/PANDO vs Ligra BFS Comparison")
    print(f"DRV: {DRV_DRAM_BANKS} banks, {DRV_DRAM_ACCESS_TIME_NS}ns, "
          f"{DRV_DRAM_INTERLEAVE_BYTES}B line, {DRV_DRAM_MSHRS_PER_BANK} MSHRs/bank, "
          f"peak={DRV_PEAK_BW_GBS:.0f} GB/s, {DRV_CLOCK_GHZ} GHz")
    print(f"CPU: SKX {SKX_PEAK_BW_GBS:.0f} GB/s peak ({SKX_CORES} cores)")
    print("=" * 70)

    # Parse all data sources
    drv = parse_drv_results(args.drv_dir)
    print(f"\nParsed {len(drv)} DRV configurations")

    ligra_strong = parse_ligra_strong_csv(args.ligra_strong_csv)
    print(f"Parsed {len(ligra_strong)} Ligra strong-scaling entries")

    ligra_weak = parse_ligra_weak_csv(args.ligra_weak_csv)
    print(f"Parsed {len(ligra_weak)} Ligra weak-scaling entries")

    # DRV results (always available)
    if drv:
        print_drv_table(drv)
        print_cache_analysis(drv)

    # Ligra weak-scaling with bandwidth (primary comparison)
    if ligra_weak:
        print_ligra_weak_table(ligra_weak)

    # Bandwidth utilization scaling
    if drv or ligra_weak:
        print_utilization_scaling(drv, ligra_weak)

    # Side-by-side comparisons
    if drv and ligra_weak:
        print_comprehensive_comparison(drv, ligra_weak)
        print_mteps_comparison(drv, ligra_weak)
        print_frequency_analysis(drv, ligra_weak)
        print_algorithm_verification(drv, ligra_weak)

    # Strong-scaling results (if available)
    if ligra_strong:
        ligra_agg = aggregate_strong(ligra_strong)
        print_ligra_strong_table(ligra_agg)

    if not ligra_weak and not ligra_strong:
        print("\nNo Ligra results found. Run one of:")
        print("  sbatch ligra_bfs_weak_scaling.sbatch   # weak scaling + bandwidth")
        print("  sbatch ligra_bfs_benchmark.sbatch      # strong scaling")


if __name__ == '__main__':
    main()
