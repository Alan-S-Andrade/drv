#!/usr/bin/env python3
"""Pre-generate initialized SSSP data structures for ROI-only simulation.

Reads uniform_graph.bin header to extract N and source vertex, then generates:
  - sssp_dist_init.bin:     N int32 values, all 0x7FFFFFFF except dist[source] = 0
  - sssp_frontier_init.bin: ceil(N/32) int32 words for frontier (bit set for source),
                           followed by ceil(N/32) zero words for next_frontier.

Format: raw little-endian int32 arrays, no header.
"""
import argparse
import struct
import array


def main():
    parser = argparse.ArgumentParser(
        description="Generate pre-initialized SSSP state for ROI-only simulation")
    parser.add_argument("--graph", required=True,
                        help="Path to uniform_graph.bin")
    parser.add_argument("-o", "--output-dir", required=True,
                        help="Output directory for generated files")
    args = parser.parse_args()

    # Read graph header: 5 x int32 (N, E, avg_degree, unused, source)
    with open(args.graph, "rb") as f:
        header = struct.unpack("<5i", f.read(20))

    N = header[0]
    source = header[4]
    bm_words = (N + 31) // 32

    print(f"Graph: N={N}, source={source}, bitmap_words={bm_words}")

    # Generate dist[] init: all INT32_MAX (0x7FFFFFFF), except dist[source] = 0
    SSSP_INF = 0x7FFFFFFF
    dist = array.array("i", [SSSP_INF] * N)
    dist[source] = 0

    dist_path = f"{args.output_dir}/sssp_dist_init.bin"
    with open(dist_path, "wb") as f:
        dist.tofile(f)
    print(f"Wrote {dist_path}: {N * 4} bytes")

    # Generate frontier bitmaps: frontier has source bit set, next_frontier all zero
    frontier = array.array("i", [0] * bm_words)
    frontier[source // 32] = 1 << (source % 32)

    next_frontier = array.array("i", [0] * bm_words)

    frontier_path = f"{args.output_dir}/sssp_frontier_init.bin"
    with open(frontier_path, "wb") as f:
        frontier.tofile(f)
        next_frontier.tofile(f)
    print(f"Wrote {frontier_path}: {2 * bm_words * 4} bytes")


if __name__ == "__main__":
    main()
