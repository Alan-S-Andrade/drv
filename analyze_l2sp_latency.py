#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Parse and analyze L2SP latency measurement output

import sys
import re
from collections import defaultdict

def parse_latency_output(filename):
    """Parse console output from l2sp_latency measurement."""
    
    results = {
        'single_access': {},
        'streaming': {},
        'alternating': {},
        'config': {}
    }
    
    current_section = None
    
    try:
        with open(filename, 'r') as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"Error: Cannot open {filename}")
        return results
    
    for line in lines:
        line = line.strip()
        
        # Detect sections
        if '--- Single Access Latency ---' in line:
            current_section = 'single_access'
            continue
        elif '--- Streaming Access' in line:
            current_section = 'streaming'
            continue
        elif '--- Cross-Core Alternating' in line:
            current_section = 'alternating'
            continue
        
        # Parse configuration
        if 'Harts per core:' in line:
            match = re.search(r'(\d+)', line)
            if match:
                results['config']['harts_per_core'] = int(match.group(1))
        elif 'Cores per pod:' in line:
            match = re.search(r'(\d+)', line)
            if match:
                results['config']['cores_per_pod'] = int(match.group(1))
        elif 'Total harts:' in line:
            match = re.search(r'(\d+)', line)
            if match:
                results['config']['total_harts'] = int(match.group(1))
        
        # Parse data lines (CSV format)
        if re.match(r'\d+,', line):
            parts = line.split(',')
            
            if current_section == 'single_access' and len(parts) >= 4:
                # Hart,Core,Thread,AvgLatency
                hart_id = int(parts[0])
                latency = int(parts[3])
                results['single_access'][hart_id] = latency
            
            elif current_section == 'streaming' and len(parts) >= 2:
                # Hart,AvgLatency
                hart_id = int(parts[0])
                latency = int(parts[1])
                results['streaming'][hart_id] = latency
            
            elif current_section == 'alternating' and len(parts) >= 2:
                # Hart,AvgLatency
                hart_id = int(parts[0])
                latency = int(parts[1])
                results['alternating'][hart_id] = latency
    
    return results

def analyze_latency_results(results):
    """Generate analysis and statistics."""
    
    print("\n" + "="*70)
    print("L2SP LATENCY ANALYSIS")
    print("="*70)
    
    config = results['config']
    if config:
        print("\nConfiguration:")
        print(f"  Harts per core: {config.get('harts_per_core', '?')}")
        print(f"  Cores per pod: {config.get('cores_per_pod', '?')}")
        print(f"  Total harts: {config.get('total_harts', '?')}")
    
    # Analyze single access latency
    single = results['single_access']
    if single:
        print("\n--- SINGLE ACCESS LATENCY ---")
        latencies = list(single.values())
        print(f"  Samples: {len(latencies)}")
        print(f"  Min: {min(latencies)} cycles")
        print(f"  Max: {max(latencies)} cycles")
        print(f"  Avg: {sum(latencies) / len(latencies):.1f} cycles")
        print(f"  Std Dev: {compute_stddev(latencies):.2f}")
        
        # Categorize by core
        by_core = defaultdict(list)
        for hart_id, lat in single.items():
            core_id = hart_id >> 4
            by_core[core_id].append(lat)
        
        print(f"\n  Per-core breakdown (assuming 16 threads/core):")
        for core_id in sorted(by_core.keys()):
            core_lats = by_core[core_id]
            print(f"    Core {core_id:2d}: min={min(core_lats):3d}, "
                  f"max={max(core_lats):3d}, "
                  f"avg={sum(core_lats)/len(core_lats):6.1f}")
    
    # Analyze streaming latency
    stream = results['streaming']
    if stream:
        print("\n--- STREAMING ACCESS LATENCY ---")
        latencies = list(stream.values())
        print(f"  Samples: {len(latencies)}")
        print(f"  Min: {min(latencies)} cycles")
        print(f"  Max: {max(latencies)} cycles")
        print(f"  Avg: {sum(latencies) / len(latencies):.1f} cycles")
        print(f"  Std Dev: {compute_stddev(latencies):.2f}")
        
        #Contention effect
        if single:
            single_avg = sum(single.values()) / len(single)
            stream_avg = sum(stream.values()) / len(stream)
            print(f"\n  Contention effect:")
            print(f"    Streaming vs Single: {stream_avg/single_avg:.2f}x")
    
    # Analyze alternating pattern
    alt = results['alternating']
    if alt:
        print("\n--- ALTERNATING ACCESS PATTERN ---")
        latencies = list(alt.values())
        print(f"  Samples: {len(latencies)}")
        if latencies:
            print(f"  Min: {min(latencies)} cycles")
            print(f"  Max: {max(latencies)} cycles")
            print(f"  Avg: {sum(latencies) / len(latencies):.1f} cycles")
            print(f"  Std Dev: {compute_stddev(latencies):.2f}")
    
    print("\n" + "="*70)

def compute_stddev(values):
    """Compute standard deviation."""
    if len(values) < 2:
        return 0.0
    mean = sum(values) / len(values)
    variance = sum((x - mean)**2 for x in values) / len(values)
    return variance ** 0.5

def generate_report(results, output_file=None):
    """Generate a detailed report."""
    
    lines = []
    lines.append("L2SP LATENCY MEASUREMENT REPORT")
    lines.append("="*70)
    
    # Configuration
    config = results['config']
    if config:
        lines.append("\n[CONFIGURATION]")
        lines.append(f"Harts per core: {config.get('harts_per_core', 'unknown')}")
        lines.append(f"Cores per pod: {config.get('cores_per_pod', 'unknown')}")
        lines.append(f"Total harts: {config.get('total_harts', 'unknown')}")
    
    # Data tables
    lines.append("\n[SINGLE ACCESS LATENCIES]")
    lines.append("Hart_ID, Core_ID, Thread_ID, Latency_Cycles")
    for hart_id in sorted(results['single_access'].keys()):
        core_id = hart_id >> 4
        thread_id = hart_id & 0xF
        latency = results['single_access'][hart_id]
        lines.append(f"{hart_id}, {core_id}, {thread_id}, {latency}")
    
    # Write to file or stdout
    text = "\n".join(lines)
    
    if output_file:
        with open(output_file, 'w') as f:
            f.write(text)
        print(f"Report written to {output_file}")
    else:
        print(text)

def main():
    if len(sys.argv) < 2:
        print("Usage: analyze_latency.py <output_file> [--report output.txt]")
        print("\nExample:")
        print("  python3 analyze_latency.py measurement.txt")
        print("  python3 analyze_latency.py measurement.txt --report report.txt")
        sys.exit(1)
    
    output_file = sys.argv[1]
    report_file = None
    
    if len(sys.argv) > 3 and sys.argv[2] == '--report':
        report_file = sys.argv[3]
    
    # Parse and analyze
    results = parse_latency_output(output_file)
    analyze_latency_results(results)
    
    if report_file:
        generate_report(results, report_file)

if __name__ == '__main__':
    main()
