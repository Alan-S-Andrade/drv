#!/usr/bin/env python3
"""
Emit CACTI-ready SRAM parameter blocks for a DRV pod.

Examples:
  python3 model/cacti_pod_params.py
  python3 model/cacti_pod_params.py --pod-cores-x 4 --pod-cores-y 4
  python3 model/cacti_pod_params.py --pod-cores-x 4 --pod-cores-y 4 --without-pxn-dram-cache
"""

from __future__ import annotations

import argparse


def make_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Generate CACTI parameter blocks for a DRV pod")
    p.add_argument("--pod-cores-x", type=int, default=4)
    p.add_argument("--pod-cores-y", type=int, default=4)
    p.add_argument("--core-threads", type=int, default=16)
    p.add_argument("--core-l1sp-size", type=int, default=256 * 1024)
    p.add_argument("--pod-l2sp-size", type=int, default=1024 * 1024)
    p.add_argument("--pod-l2sp-banks", type=int, default=1)
    p.add_argument("--pxn-dram-banks", type=int, default=1)
    p.add_argument("--without-pxn-dram-cache", action="store_true")
    p.add_argument("--dram-cache-size", type=int, default=64 * 1024)
    p.add_argument("--dram-cache-assoc", type=int, default=8)
    p.add_argument("--dram-cache-line-size", type=int, default=64)
    p.add_argument("--tech-node-nm", type=int, default=22)
    p.add_argument("--clock-ghz", type=float, default=1.0)
    p.add_argument("--l1sp-line-size", type=int, default=16)
    p.add_argument("--l2sp-line-size", type=int, default=16)
    p.add_argument("--noc-flit-bytes", type=int, default=8)
    p.add_argument("--noc-input-buffer-bytes", type=int, default=1024)
    p.add_argument("--noc-output-buffer-bytes", type=int, default=1024)
    return p


def fmt_kib(value: int) -> str:
    if value % 1024 == 0:
        return f"{value // 1024} KiB"
    return f"{value} B"


def emit_block(name: str, rows: list[tuple[str, str]]) -> None:
    print(name)
    print("-" * len(name))
    for key, value in rows:
        print(f"{key:24} {value}")
    print()


def main() -> None:
    args = make_parser().parse_args()

    pod_cores = args.pod_cores_x * args.pod_cores_y
    router_ports = pod_cores + args.pod_l2sp_banks + 1
    l2sp_bank_bytes = args.pod_l2sp_size // args.pod_l2sp_banks
    dram_cache_enabled = not args.without_pxn_dram_cache

    print("DRV Pod CACTI Parameters")
    print("========================")
    print(f"Pod cores               {args.pod_cores_x} x {args.pod_cores_y} = {pod_cores}")
    print(f"Threads per core        {args.core_threads}")
    print(f"Technology node         {args.tech_node_nm} nm")
    print(f"Clock target            {args.clock_ghz:.3f} GHz")
    print(f"Router ports            {router_ports}")
    print()

    emit_block("L1SP Per Core", [
        ("capacity", fmt_kib(args.core_l1sp_size)),
        ("banks", "1"),
        ("associativity", "1"),
        ("line size", f"{args.l1sp_line_size} B"),
        ("ports", "1RW or 1R/1W"),
        ("technology", f"{args.tech_node_nm} nm"),
        ("target clock", f"{args.clock_ghz:.3f} GHz"),
        ("instances", str(pod_cores)),
        ("total capacity", fmt_kib(args.core_l1sp_size * pod_cores)),
    ])

    emit_block("L2SP Per Bank", [
        ("capacity", fmt_kib(l2sp_bank_bytes)),
        ("banks", "1"),
        ("associativity", "1"),
        ("line size", f"{args.l2sp_line_size} B"),
        ("ports", "1RW or 1R/1W"),
        ("technology", f"{args.tech_node_nm} nm"),
        ("target clock", f"{args.clock_ghz:.3f} GHz"),
        ("instances", str(args.pod_l2sp_banks)),
        ("total capacity", fmt_kib(args.pod_l2sp_size)),
    ])

    emit_block("NoC Input Buffer Per Port", [
        ("capacity", fmt_kib(args.noc_input_buffer_bytes)),
        ("banks", "1"),
        ("associativity", "1"),
        ("line size", f"{args.noc_flit_bytes} B"),
        ("ports", "1RW"),
        ("technology", f"{args.tech_node_nm} nm"),
        ("target clock", f"{args.clock_ghz:.3f} GHz"),
        ("instances", str(router_ports)),
        ("total capacity", fmt_kib(args.noc_input_buffer_bytes * router_ports)),
    ])

    emit_block("NoC Output Buffer Per Port", [
        ("capacity", fmt_kib(args.noc_output_buffer_bytes)),
        ("banks", "1"),
        ("associativity", "1"),
        ("line size", f"{args.noc_flit_bytes} B"),
        ("ports", "1RW"),
        ("technology", f"{args.tech_node_nm} nm"),
        ("target clock", f"{args.clock_ghz:.3f} GHz"),
        ("instances", str(router_ports)),
        ("total capacity", fmt_kib(args.noc_output_buffer_bytes * router_ports)),
    ])

    if dram_cache_enabled:
        emit_block("DRAM Cache Per Bank", [
            ("capacity", fmt_kib(args.dram_cache_size)),
            ("banks", "1"),
            ("associativity", str(args.dram_cache_assoc)),
            ("line size", f"{args.dram_cache_line_size} B"),
            ("ports", "1RW or 1R/1W"),
            ("technology", f"{args.tech_node_nm} nm"),
            ("target clock", f"{args.clock_ghz:.3f} GHz"),
            ("instances", str(args.pxn_dram_banks)),
            ("total capacity", fmt_kib(args.dram_cache_size * args.pxn_dram_banks)),
        ])
    else:
        print("DRAM Cache Per Bank")
        print("-------------------")
        print("disabled")
        print()

    print("CACTI Notes")
    print("-----------")
    print("Use associativity=1 for scratchpads.")
    print("Use line size 16 B for scratchpad-like modeling or 64 B if matching cache-array assumptions.")
    print("Multiply per-instance CACTI area/power by the instance count shown above.")


if __name__ == "__main__":
    main()
