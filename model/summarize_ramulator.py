"""
Summarize ramulator DRAM statistics from ramulator_system_pxn0_dram0.stats.

Reports:
  - Achieved bandwidth (read, write, total)
  - Bandwidth utilization %
  - Row buffer hit rate (per-channel and aggregate)
  - Queue occupancy (per-channel and aggregate)
  - Read latency (per-channel and aggregate)

Usage:
    python3 summarize_ramulator.py                       # default file
    python3 summarize_ramulator.py <stats_file>          # custom file
    python3 summarize_ramulator.py --clock 500MHz <file> # custom DRAM clock
"""
import sys
import re
import os
import struct


def parse_stats(filepath):
    """Parse ramulator stats file into dict of {stat_name: value}."""
    stats = {}
    with open(filepath, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            # Skip array index lines like "[0]  43234.0  #..."
            if line.startswith("["):
                continue
            # Format: ramulator.stat_name    value    # comment
            parts = line.split()
            if len(parts) < 2:
                continue
            key = parts[0]
            val_str = parts[1]
            if key.startswith("ramulator."):
                key = key[len("ramulator."):]
            try:
                if "." in val_str:
                    stats[key] = float(val_str)
                else:
                    stats[key] = int(val_str)
            except ValueError:
                stats[key] = val_str
    return stats


def detect_num_channels(stats):
    """Detect number of channels from read_transaction_bytes_N keys."""
    n = 0
    while f"read_transaction_bytes_{n}" in stats:
        n += 1
    return n


def parse_clock(clock_str):
    """Parse clock string like '1GHz' or '500MHz' to Hz."""
    clock_str = clock_str.strip().upper()
    m = re.match(r"([0-9.]+)\s*(GHZ|MHZ|KHZ|HZ)", clock_str)
    if not m:
        raise ValueError(f"Cannot parse clock: {clock_str}")
    val = float(m.group(1))
    unit = m.group(2)
    if unit == "GHZ":
        return val * 1e9
    elif unit == "MHZ":
        return val * 1e6
    elif unit == "KHZ":
        return val * 1e3
    return val


def fmt_bytes(b):
    """Format bytes as human-readable."""
    if b >= 1e9:
        return f"{b / 1e9:.2f} GB"
    elif b >= 1e6:
        return f"{b / 1e6:.2f} MB"
    elif b >= 1e3:
        return f"{b / 1e3:.2f} KB"
    return f"{b:.0f} B"


def read_graph_edges(graph_path):
    """Read number of edges from binary CSR graph header (5 x int32: N, E, avg_deg, 0, source)."""
    with open(graph_path, "rb") as f:
        header = f.read(20)
    if len(header) < 20:
        return None
    N, E, _, _, _ = struct.unpack("<5i", header)
    return E


def main():
    # Parse arguments
    stats_file = None
    clock_hz = 1e9  # default 1 GHz (SST drives ramulator at this clock)
    num_edges = None
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--clock" and i + 1 < len(args):
            clock_hz = parse_clock(args[i + 1])
            i += 2
        elif args[i] == "--edges" and i + 1 < len(args):
            num_edges = int(args[i + 1])
            i += 2
        elif args[i] == "--graph" and i + 1 < len(args):
            num_edges = read_graph_edges(args[i + 1])
            i += 2
        elif not args[i].startswith("-"):
            stats_file = args[i]
            i += 1
        else:
            i += 1

    # Auto-detect graph file for edge count
    if num_edges is None and os.path.exists("uniform_graph.bin"):
        num_edges = read_graph_edges("uniform_graph.bin")

    # Auto-detect stats file
    if stats_file is None:
        candidates = [
            "ramulator_system_pxn0_dram0.stats",
            "ramulator_system_pxn0_dram0_s0.stats",
        ]
        for c in candidates:
            if os.path.exists(c):
                stats_file = c
                break
        if stats_file is None:
            print("Error: No ramulator stats file found. Provide path as argument.")
            sys.exit(1)

    stats = parse_stats(stats_file)
    num_channels = detect_num_channels(stats)

    if num_channels == 0:
        print("Error: No channel data found in stats file.")
        sys.exit(1)

    # System-level stats
    dram_cycles = stats.get("dram_cycles", 0)
    total_requests = stats.get("incoming_requests", 0)
    read_requests = stats.get("read_requests", 0)
    write_requests = stats.get("write_requests", 0)
    max_bw_bps = stats.get("maximum_bandwidth", 0)
    active_cycles = stats.get("ramulator_active_cycles", 0)

    # Simulation time
    sim_time_sec = dram_cycles / clock_hz if dram_cycles > 0 else 0

    # Per-channel data
    ch_read_bytes = []
    ch_write_bytes = []
    ch_read_lat_avg = []
    ch_read_lat_sum = []
    ch_queue_avg = []
    ch_read_queue_avg = []
    ch_write_queue_avg = []
    ch_active = []

    # Row buffer stats per channel
    ch_row_hits = []
    ch_row_misses = []
    ch_row_conflicts = []
    ch_rd_row_hits = []
    ch_rd_row_misses = []
    ch_rd_row_conflicts = []
    ch_wr_row_hits = []
    ch_wr_row_misses = []
    ch_wr_row_conflicts = []

    for ch in range(num_channels):
        ch_read_bytes.append(stats.get(f"read_transaction_bytes_{ch}", 0))
        ch_write_bytes.append(stats.get(f"write_transaction_bytes_{ch}", 0))
        ch_read_lat_avg.append(stats.get(f"read_latency_avg_{ch}", 0))
        ch_read_lat_sum.append(stats.get(f"read_latency_sum_{ch}", 0))
        ch_queue_avg.append(stats.get(f"req_queue_length_avg_{ch}", 0))
        ch_read_queue_avg.append(stats.get(f"read_req_queue_length_avg_{ch}", 0))
        ch_write_queue_avg.append(stats.get(f"write_req_queue_length_avg_{ch}", 0))
        ch_active.append(stats.get(f"active_cycles_{ch}", 0))

        ch_row_hits.append(stats.get(f"row_hits_channel_{ch}_core", 0))
        ch_row_misses.append(stats.get(f"row_misses_channel_{ch}_core", 0))
        ch_row_conflicts.append(stats.get(f"row_conflicts_channel_{ch}_core", 0))

        ch_rd_row_hits.append(stats.get(f"read_row_hits_channel_{ch}_core", 0))
        ch_rd_row_misses.append(stats.get(f"read_row_misses_channel_{ch}_core", 0))
        ch_rd_row_conflicts.append(stats.get(f"read_row_conflicts_channel_{ch}_core", 0))

        ch_wr_row_hits.append(stats.get(f"write_row_hits_channel_{ch}_core", 0))
        ch_wr_row_misses.append(stats.get(f"write_row_misses_channel_{ch}_core", 0))
        ch_wr_row_conflicts.append(stats.get(f"write_row_conflicts_channel_{ch}_core", 0))

    # Aggregates
    total_read_bytes = sum(ch_read_bytes)
    total_write_bytes = sum(ch_write_bytes)
    total_bytes = total_read_bytes + total_write_bytes

    total_row_hits = sum(ch_row_hits)
    total_row_misses = sum(ch_row_misses)
    total_row_conflicts = sum(ch_row_conflicts)
    total_row_accesses = total_row_hits + total_row_misses + total_row_conflicts

    total_rd_row_hits = sum(ch_rd_row_hits)
    total_rd_row_misses = sum(ch_rd_row_misses)
    total_rd_row_conflicts = sum(ch_rd_row_conflicts)
    total_rd_row_accesses = total_rd_row_hits + total_rd_row_misses + total_rd_row_conflicts

    total_wr_row_hits = sum(ch_wr_row_hits)
    total_wr_row_misses = sum(ch_wr_row_misses)
    total_wr_row_conflicts = sum(ch_wr_row_conflicts)
    total_wr_row_accesses = total_wr_row_hits + total_wr_row_misses + total_wr_row_conflicts

    # Global queue stats
    global_queue_avg = stats.get("in_queue_req_num_avg", 0)
    global_read_queue_avg = stats.get("in_queue_read_req_num_avg", 0)
    global_write_queue_avg = stats.get("in_queue_write_req_num_avg", 0)

    # Bandwidth calculations
    achieved_read_bw = total_read_bytes / sim_time_sec if sim_time_sec > 0 else 0
    achieved_write_bw = total_write_bytes / sim_time_sec if sim_time_sec > 0 else 0
    achieved_total_bw = total_bytes / sim_time_sec if sim_time_sec > 0 else 0

    # Utilization
    bw_util_pct = (achieved_total_bw / max_bw_bps * 100) if max_bw_bps > 0 else 0
    read_bw_util_pct = (achieved_read_bw / max_bw_bps * 100) if max_bw_bps > 0 else 0

    # Active utilization (fraction of cycles any channel was active)
    active_util_pct = (active_cycles / (dram_cycles * num_channels) * 100) if dram_cycles > 0 else 0

    # Weighted average read latency (across channels)
    total_lat_sum = sum(ch_read_lat_sum)
    weighted_avg_lat = total_lat_sum / read_requests if read_requests > 0 else 0

    # ================================================================
    # PRINT REPORT
    # ================================================================
    W = 90
    print("=" * W)
    print("RAMULATOR DRAM STATISTICS SUMMARY")
    print("=" * W)

    print(f"\n{'Stats file:':<40} {stats_file}")
    print(f"{'DRAM clock:':<40} {clock_hz / 1e9:.3f} GHz")
    print(f"{'Channels:':<40} {num_channels}")
    print(f"{'DRAM cycles:':<40} {dram_cycles:,}")
    print(f"{'Simulation time:':<40} {sim_time_sec * 1e3:.3f} ms")
    print(f"{'Total requests:':<40} {total_requests:,}  (read: {read_requests:,}  write: {write_requests:,})")
    print(f"{'Peak bandwidth (spec):':<40} {max_bw_bps / 1e9:.1f} GB/s")

    # --- BANDWIDTH ---
    print(f"\n{'-' * W}")
    print("ACHIEVED BANDWIDTH")
    print(f"{'-' * W}")
    print(f"{'  Read traffic:':<40} {fmt_bytes(total_read_bytes):<15} BW: {achieved_read_bw / 1e9:.4f} GB/s")
    print(f"{'  Write traffic:':<40} {fmt_bytes(total_write_bytes):<15} BW: {achieved_write_bw / 1e9:.4f} GB/s")
    print(f"{'  Total traffic:':<40} {fmt_bytes(total_bytes):<15} BW: {achieved_total_bw / 1e9:.4f} GB/s")
    print(f"\n{'  BW utilization (total/peak):':<40} {bw_util_pct:.4f}%")
    print(f"{'  Read BW utilization:':<40} {read_bw_util_pct:.4f}%")
    print(f"{'  Active cycle utilization:':<40} {active_util_pct:.2f}%  ({active_cycles:,} / {dram_cycles * num_channels:,})")

    # --- Per-channel bandwidth ---
    print(f"\n{'-' * W}")
    print("PER-CHANNEL BANDWIDTH")
    print(f"{'-' * W}")
    hdr = f"{'Ch':<5} {'Read Bytes':<15} {'Write Bytes':<15} {'Total Bytes':<15} {'Read BW':<12} {'Write BW':<12} {'Total BW':<12}"
    print(hdr)
    print("-" * len(hdr))
    for ch in range(num_channels):
        ch_total = ch_read_bytes[ch] + ch_write_bytes[ch]
        ch_rbw = ch_read_bytes[ch] / sim_time_sec if sim_time_sec > 0 else 0
        ch_wbw = ch_write_bytes[ch] / sim_time_sec if sim_time_sec > 0 else 0
        ch_tbw = ch_total / sim_time_sec if sim_time_sec > 0 else 0
        print(f"{ch:<5} {ch_read_bytes[ch]:<15,} {ch_write_bytes[ch]:<15,} {ch_total:<15,} "
              f"{ch_rbw / 1e6:<12,.1f} {ch_wbw / 1e6:<12,.1f} {ch_tbw / 1e6:<12,.1f}")
    print(f"{'(MB/s)':<5} {'':>15} {'':>15} {'':>15}")

    # --- ROW BUFFER HIT RATE ---
    print(f"\n{'-' * W}")
    print("ROW BUFFER HIT RATE")
    print(f"{'-' * W}")

    def hit_rate(hits, total):
        return (hits / total * 100) if total > 0 else 0.0

    print(f"\n  {'Aggregate (all channels):'}")
    print(f"{'    Combined:':<40} {hit_rate(total_row_hits, total_row_accesses):.2f}%  "
          f"(hits: {total_row_hits:,}  misses: {total_row_misses:,}  conflicts: {total_row_conflicts:,})")
    print(f"{'    Read:':<40} {hit_rate(total_rd_row_hits, total_rd_row_accesses):.2f}%  "
          f"(hits: {total_rd_row_hits:,}  misses: {total_rd_row_misses:,}  conflicts: {total_rd_row_conflicts:,})")
    print(f"{'    Write:':<40} {hit_rate(total_wr_row_hits, total_wr_row_accesses):.2f}%  "
          f"(hits: {total_wr_row_hits:,}  misses: {total_wr_row_misses:,}  conflicts: {total_wr_row_conflicts:,})")

    print(f"\n  {'Per-channel:'}")
    hdr = f"  {'Ch':<5} {'Hit Rate %':<12} {'Hits':<12} {'Misses':<12} {'Conflicts':<12} {'Rd Hit %':<12} {'Wr Hit %':<12}"
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    for ch in range(num_channels):
        ch_total_acc = ch_row_hits[ch] + ch_row_misses[ch] + ch_row_conflicts[ch]
        ch_hr = hit_rate(ch_row_hits[ch], ch_total_acc)

        ch_rd_total = ch_rd_row_hits[ch] + ch_rd_row_misses[ch] + ch_rd_row_conflicts[ch]
        ch_rd_hr = hit_rate(ch_rd_row_hits[ch], ch_rd_total)

        ch_wr_total = ch_wr_row_hits[ch] + ch_wr_row_misses[ch] + ch_wr_row_conflicts[ch]
        ch_wr_hr = hit_rate(ch_wr_row_hits[ch], ch_wr_total)

        print(f"  {ch:<5} {ch_hr:<12.2f} {ch_row_hits[ch]:<12,} {ch_row_misses[ch]:<12,} "
              f"{ch_row_conflicts[ch]:<12,} {ch_rd_hr:<12.2f} {ch_wr_hr:<12.2f}")

    # --- QUEUE OCCUPANCY ---
    print(f"\n{'-' * W}")
    print("QUEUE OCCUPANCY (average requests in queue per DRAM cycle)")
    print(f"{'-' * W}")

    print(f"\n  {'Global (across all channels):'}")
    print(f"{'    Total queue:':<40} {global_queue_avg:.3f}")
    print(f"{'    Read queue:':<40} {global_read_queue_avg:.3f}")
    print(f"{'    Write queue:':<40} {global_write_queue_avg:.3f}")

    print(f"\n  {'Per-channel:'}")
    hdr = f"  {'Ch':<5} {'Total Q':<12} {'Read Q':<12} {'Write Q':<12}"
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    for ch in range(num_channels):
        print(f"  {ch:<5} {ch_queue_avg[ch]:<12.3f} {ch_read_queue_avg[ch]:<12.6f} {ch_write_queue_avg[ch]:<12.3f}")

    # --- READ LATENCY ---
    print(f"\n{'-' * W}")
    print("READ LATENCY (DRAM cycles, in memory clock domain)")
    print(f"{'-' * W}")

    print(f"\n{'  Weighted avg (all channels):':<40} {weighted_avg_lat:.2f} cycles")

    hdr = f"  {'Ch':<5} {'Avg Latency':<15} {'Lat Sum':<15}"
    print(f"\n  {'Per-channel:'}")
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    for ch in range(num_channels):
        print(f"  {ch:<5} {ch_read_lat_avg[ch]:<15.3f} {int(ch_read_lat_sum[ch]):<15,}")

    # --- SUMMARY BOX ---
    print(f"\n{'=' * W}")
    print("KEY METRICS SUMMARY")
    print(f"{'=' * W}")
    print(f"  {'Achieved BW (total):':<35} {achieved_total_bw / 1e9:.4f} GB/s")
    print(f"  {'Achieved BW (read):':<35} {achieved_read_bw / 1e9:.4f} GB/s")
    print(f"  {'Achieved BW (write):':<35} {achieved_write_bw / 1e9:.4f} GB/s")
    print(f"  {'Peak BW:':<35} {max_bw_bps / 1e9:.1f} GB/s")
    print(f"  {'BW Utilization:':<35} {bw_util_pct:.4f}%")
    print(f"  {'Row Buffer Hit Rate:':<35} {hit_rate(total_row_hits, total_row_accesses):.2f}%")
    print(f"  {'  Read Hit Rate:':<35} {hit_rate(total_rd_row_hits, total_rd_row_accesses):.2f}%")
    print(f"  {'  Write Hit Rate:':<35} {hit_rate(total_wr_row_hits, total_wr_row_accesses):.2f}%")
    print(f"  {'Avg Queue Occupancy:':<35} {global_queue_avg:.3f} reqs")
    print(f"  {'  Read Queue:':<35} {global_read_queue_avg:.3f} reqs")
    print(f"  {'  Write Queue:':<35} {global_write_queue_avg:.3f} reqs")
    print(f"  {'Avg Read Latency:':<35} {weighted_avg_lat:.2f} DRAM cycles")

    # --- DRAM Bytes per Edge ---
    if num_edges is not None and num_edges > 0:
        total_bytes_per_edge = total_bytes / num_edges
        read_bytes_per_edge = total_read_bytes / num_edges
        write_bytes_per_edge = total_write_bytes / num_edges
        print(f"\n  {'--- DRAM Bytes per Edge ---'}")
        print(f"  {'Graph Edges:':<35} {num_edges:,}")
        print(f"  {'Total DRAM Bytes / Edge:':<35} {total_bytes_per_edge:.2f} B")
        print(f"  {'Read DRAM Bytes / Edge:':<35} {read_bytes_per_edge:.2f} B")
        print(f"  {'Write DRAM Bytes / Edge:':<35} {write_bytes_per_edge:.2f} B")
    elif num_edges is not None:
        print(f"\n  {'Graph Edges:':<35} {num_edges} (zero — skipping bytes/edge)")
    else:
        print(f"\n  {'DRAM Bytes/Edge:':<35} N/A (no --edges/--graph and no uniform_graph.bin found)")

    print(f"{'=' * W}")


if __name__ == "__main__":
    main()
