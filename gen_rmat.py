#!/usr/bin/env python3
"""Generate Graph500-standard RMAT (Kronecker) graphs.

Graph500 RMAT parameters: a=0.57, b=0.19, c=0.19, d=0.05
  - Scale S: N = 2^S vertices
  - Edge factor EF: total generated edge pairs = EF * N (default 16)
  - Undirected: edges are symmetrized (both directions stored)
  - Self-loops removed, duplicates removed

Output formats:
  --format drv    : DRV binary CSR (matches gen_uniform_csr.py)
  --format ligra  : Ligra AdjacencyGraph text (matches gen_uniform_graph.cpp)
  --format both   : writes both files

Usage examples:
  # Scale 20 (1M vertices), edge factor 16, Ligra format
  python3 gen_rmat_graph.py --scale 20 --ef 16 --format ligra -o rmat_s20.adj

  # Scale 14, DRV binary format
  python3 gen_rmat_graph.py --scale 14 --ef 16 --format drv -o rmat_s14.bin

  # Both formats
  python3 gen_rmat_graph.py --scale 16 --ef 16 --format both -o rmat_s16
"""
import argparse
import struct
import array
import sys
import random
import time


def generate_rmat_edges(scale, ef, seed=42, a=0.57, b=0.19, c=0.19, d=0.05):
    """Generate Graph500-standard RMAT edge list using Kronecker method.

    Returns a list of (src, dst) tuples after:
      - symmetrizing (adding reverse edges)
      - removing self-loops
      - removing duplicates
    """
    N = 1 << scale
    M = ef * N  # number of raw edge pairs to generate
    rng = random.Random(seed)

    ab = a + b
    abc = a + b + c

    print(f"Generating {M} raw RMAT edges (scale={scale}, N={N}, ef={ef})...",
          file=sys.stderr)
    t0 = time.time()

    edges = set()
    for _ in range(M):
        u, v = 0, 0
        for level in range(scale):
            r = rng.random()
            if r < a:
                pass  # quadrant (0,0)
            elif r < ab:
                v |= (1 << level)  # quadrant (0,1)
            elif r < abc:
                u |= (1 << level)  # quadrant (1,0)
            else:
                u |= (1 << level)  # quadrant (1,1)
                v |= (1 << level)

        # Permute vertices (Graph500 spec: random permutation)
        # We skip global permutation for reproducibility/simplicity,
        # but apply noise via bit-level scrambling below

        if u != v:  # remove self-loops
            if u > v:
                u, v = v, u
            edges.add((u, v))

    t1 = time.time()
    print(f"  Raw generation: {t1 - t0:.1f}s, {len(edges)} unique undirected edges",
          file=sys.stderr)

    return N, edges


def apply_permutation(N, edges, seed=12345):
    """Apply a random vertex permutation (Graph500 spec)."""
    rng = random.Random(seed)
    perm = list(range(N))
    rng.shuffle(perm)

    permuted = set()
    for u, v in edges:
        pu, pv = perm[u], perm[v]
        if pu > pv:
            pu, pv = pv, pu
        permuted.add((pu, pv))

    return permuted


def cusp_remap(N, edges, P):
    """CUSP partitioning: remap vertex IDs by degree-ranked round-robin.

    1. Rank all N vertices by degree (descending).
    2. Vertex ranked #r goes to partition r % P, position r // P.
       New ID = (r % P) * (N // P) + (r // P).
    3. Rebuild edge set with remapped IDs.
    """
    # Compute degrees from edge set
    deg = [0] * N
    for u, v in edges:
        deg[u] += 1
        deg[v] += 1

    # Rank vertices by degree descending (stable by vertex ID for ties)
    ranked = sorted(range(N), key=lambda v: (-deg[v], v))

    # Build remap: ranked[r] -> new_id
    partition_size = N // P
    remap = [0] * N
    for r, v in enumerate(ranked):
        part = r % P
        pos = r // P
        remap[v] = part * partition_size + pos

    # Apply remap to edges
    remapped = set()
    for u, v in edges:
        nu, nv = remap[u], remap[v]
        if nu > nv:
            nu, nv = nv, nu
        remapped.add((nu, nv))

    print(f"  CUSP: {P} partitions, {partition_size} vertices/partition", file=sys.stderr)
    return remapped, remap


def edges_to_csr(N, edges):
    """Convert undirected edge set to symmetric CSR (both directions stored)."""
    # Build adjacency lists
    adj = [[] for _ in range(N)]
    for u, v in edges:
        adj[u].append(v)
        adj[v].append(u)

    # Sort each adjacency list
    for v in range(N):
        adj[v].sort()

    # Build CSR arrays
    offsets = array.array("i")
    columns = array.array("i")
    degrees = array.array("i")

    offset = 0
    for v in range(N):
        offsets.append(offset)
        deg = len(adj[v])
        degrees.append(deg)
        for u in adj[v]:
            columns.append(u)
        offset += deg

    offsets.append(offset)  # sentinel
    total_edges = offset

    return offsets, columns, degrees, total_edges


def write_drv_binary(path, N, offsets, columns, degrees, total_edges, source=0):
    """Write DRV binary CSR format."""
    avg_degree = total_edges // N if N > 0 else 0
    with open(path, "wb") as f:
        # Header: N, num_edges, degree(avg), 0, source
        f.write(struct.pack("<5i", N, total_edges, avg_degree, 0, source))
        offsets.tofile(f)
        columns.tofile(f)
        degrees.tofile(f)

    file_size = 20 + (N + 1) * 4 + total_edges * 4 + N * 4
    print(f"Wrote DRV binary: {path} ({file_size} bytes)", file=sys.stderr)
    print(f"  N={N}, edges={total_edges}, avg_degree={avg_degree}, source={source}",
          file=sys.stderr)


def write_ligra_text(path, N, offsets, columns, total_edges):
    """Write Ligra AdjacencyGraph text format."""
    with open(path, "w") as f:
        f.write("AdjacencyGraph\n")
        f.write(f"{N}\n")
        f.write(f"{total_edges}\n")
        for i in range(N):
            f.write(f"{offsets[i]}\n")
        for i in range(total_edges):
            f.write(f"{columns[i]}\n")

    print(f"Wrote Ligra text: {path}", file=sys.stderr)
    print(f"  N={N}, edges={total_edges}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(
        description="Generate Graph500-standard RMAT graphs")
    parser.add_argument("--scale", "-s", type=int, required=True,
                        help="Scale: N = 2^scale vertices")
    parser.add_argument("--ef", type=int, default=16,
                        help="Edge factor (default 16)")
    parser.add_argument("--seed", type=int, default=42,
                        help="RNG seed for edge generation")
    parser.add_argument("--format", "-f", choices=["drv", "ligra", "both"],
                        default="ligra", help="Output format (default: ligra)")
    parser.add_argument("-o", "--output", required=True,
                        help="Output file path (for 'both', used as prefix)")
    parser.add_argument("--source", type=int, default=0,
                        help="BFS source vertex for DRV header (default 0)")
    parser.add_argument("--no-permute", action="store_true",
                        help="Skip random vertex permutation")
    parser.add_argument("-a", type=float, default=0.57, help="RMAT a (default 0.57)")
    parser.add_argument("-b", type=float, default=0.19, help="RMAT b (default 0.19)")
    parser.add_argument("-c", type=float, default=0.19, help="RMAT c (default 0.19)")
    parser.add_argument("-d", type=float, default=0.05, help="RMAT d (default 0.05)")
    parser.add_argument("--cusp", type=int, default=0,
                        help="CUSP partitions P: degree-ranked round-robin remap across P partitions (0=disabled)")
    args = parser.parse_args()

    total = args.a + args.b + args.c + args.d
    if abs(total - 1.0) > 1e-6:
        print(f"Error: a+b+c+d = {total}, must sum to 1.0", file=sys.stderr)
        sys.exit(1)

    N, edges = generate_rmat_edges(
        args.scale, args.ef, args.seed, args.a, args.b, args.c, args.d)

    if not args.no_permute:
        print("Applying random vertex permutation...", file=sys.stderr)
        edges = apply_permutation(N, edges, seed=args.seed + 1)

    if args.cusp > 0:
        print(f"Applying CUSP partitioning (P={args.cusp})...", file=sys.stderr)
        edges, cusp_map = cusp_remap(N, edges, args.cusp)

    print("Building CSR...", file=sys.stderr)
    offsets, columns, degrees, total_edges = edges_to_csr(N, edges)

    # Degree distribution stats
    deg_list = list(degrees)
    deg_list.sort()
    max_deg = deg_list[-1] if deg_list else 0
    median_deg = deg_list[len(deg_list) // 2] if deg_list else 0
    avg_deg = total_edges / N if N > 0 else 0
    print(f"  Degree stats: avg={avg_deg:.1f}, median={median_deg}, max={max_deg}",
          file=sys.stderr)

    if args.format == "drv":
        write_drv_binary(args.output, N, offsets, columns, degrees,
                         total_edges, args.source)
    elif args.format == "ligra":
        write_ligra_text(args.output, N, offsets, columns, total_edges)
    else:  # both
        write_drv_binary(args.output + ".bin", N, offsets, columns, degrees,
                         total_edges, args.source)
        write_ligra_text(args.output + ".adj", N, offsets, columns, total_edges)


if __name__ == "__main__":
    main()