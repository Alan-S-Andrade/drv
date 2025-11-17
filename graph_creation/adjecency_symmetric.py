# convert_adjlist_to_c_edges.py
# Usage:
#   python3 convert_adjlist_to_c_edges.py graph.txt -o edges_output.c --symmetrize both
#   python3 convert_adjlist_to_c_edges.py graph.txt --symmetrize simple --keep-self-loops

from collections import OrderedDict

def read_edges(path):
    edges = []
    with open(path, 'r') as f:
        for ln in f:
            ln = ln.strip()
            if not ln or ln.startswith('#'):
                continue
            parts = ln.split()
            if len(parts) != 2:
                continue
            u, v = int(parts[0]), int(parts[1])
            edges.append((u, v))
    return edges

def symmetrize_edges(edges, mode="both", drop_self_loops=True):
    """
    mode='both'   -> include (u,v) and (v,u) in output
    mode='simple' -> collapse to undirected unique (min(u,v), max(u,v))
    """
    out = []

    if mode == "both":
        for u, v in edges:
            if drop_self_loops and u == v:
                continue
            out.append((u, v))
            out.append((v, u))
        # dedup while preserving order
        seen = set()
        dedup = []
        for e in out:
            if e not in seen:
                seen.add(e)
                dedup.append(e)
        return dedup

    elif mode == "simple":
        # normalize to u < v and dedup
        norm = set()
        for u, v in edges:
            if drop_self_loops and u == v:
                continue
            a, b = (u, v) if u < v else (v, u)
            norm.add((a, b))
        # stable order: keep first-seen order from input (normalized)
        order = OrderedDict()
        for u, v in edges:
            if drop_self_loops and u == v:
                continue
            a, b = (u, v) if u < v else (v, u)
            if (a, b) in norm and (a, b) not in order:
                order[(a, b)] = None
        return list(order.keys())

    else:
        raise ValueError("symmetrize mode must be 'both' or 'simple'")

def to_c(edges, maximum_nodes=None, add_define=True):
    if maximum_nodes is None:
        maximum_nodes = max(max(u, v) for u, v in edges) if edges else 0

    lines = []
    if add_define:
        lines.append(f"#define maximum_nodes {maximum_nodes}")
        lines.append("")

    lines.append("int edges[][2] = {")
    for i, (u, v) in enumerate(edges):
        comma = "," if i < len(edges) - 1 else ""
        lines.append(f"    {{{u},{v}}}{comma}")
    lines.append("};")
    lines.append("int edgeCount = sizeof(edges) / sizeof(edges[0]);")
    lines.append("")
    lines.append("int adj[maximum_nodes + 1][maximum_nodes + 1] = {0};")
    lines.append("")
    lines.append("// Build adjacency matrix")
    lines.append("for (int i = 0; i < edgeCount; i++) {")
    lines.append("    int u = edges[i][0];")
    lines.append("    int v = edges[i][1];")
    lines.append("    adj[u][v] = 1;")
    lines.append("    adj[v][u] = 1;")
    lines.append("}")
    return "\n".join(lines)

def convert_to_c_edges(input_file, output_file=None, sym_mode="both", keep_self_loops=False):
    edges = read_edges(input_file)
    edges_sym = symmetrize_edges(edges, mode=sym_mode, drop_self_loops=not keep_self_loops)
    c_code = to_c(edges_sym)

    if output_file:
        with open(output_file, 'w') as f:
            f.write(c_code)
        print(f"✅ Output written to {output_file}")
    else:
        print(c_code)

if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description="Convert adjacency list to C-style edge array (with symmetrization).")
    p.add_argument("input_file", help="Path to the adjacency list file (each line: 'u v').")
    p.add_argument("-o", "--output_file", help="Optional output file path.")
    p.add_argument("--symmetrize", choices=["both", "simple"], default="both",
                   help="Symmetrization mode: 'both' adds reverse edges; 'simple' keeps unique undirected edges.")
    p.add_argument("--keep-self-loops", action="store_true", help="Keep self-loops (u==v).")
    args = p.parse_args()

    convert_to_c_edges(
        args.input_file,
        args.output_file,
        sym_mode=args.symmetrize,
        keep_self_loops=args.keep_self_loops,
    )

