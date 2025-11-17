# convert_adjlist_to_c_edges.py
# Usage examples:
#   python3 convert_adjlist_to_c_edges.py graph.txt -o edges_input.hpp
#   python3 convert_adjlist_to_c_edges.py graph.txt --symmetrize both --var-name edges_input
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
    mode='both'   -> include (u,v) and (v,u) in output (keep duplicates; stable-dedup exact (u,v))
    mode='simple' -> collapse to undirected unique (min(u,v), max(u,v))
    """
    out = []

    if mode == "both":
        for u, v in edges:
            if drop_self_loops and u == v:
                continue
            out.append((u, v))
            out.append((v, u))
        # dedup exact ordered pairs while preserving order
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

def to_cpp_pairs(edges, var_name="edges_input", add_max_nodes=True, static_const=True):
    maximum_nodes = max((max(u, v) for u, v in edges), default=0)

    lines = []
    lines.append("#pragma once")
    lines.append("#include <utility>")
    lines.append("")
    if add_max_nodes:
        lines.append(f"constexpr int maximum_nodes = {maximum_nodes};")
        lines.append("")
    qual = "static const " if static_const else ""
    lines.append(f"{qual}std::pair<int,int> {var_name}[] = {{")
    for i, (u, v) in enumerate(edges):
        comma = "," if i < len(edges) - 1 else ""
        lines.append(f"    std::make_pair({u}, {v}){comma}")
    lines.append("};")
    lines.append(f"constexpr std::size_t {var_name}_count = sizeof({var_name})/sizeof({var_name}[0]);")
    return "\n".join(lines)

def convert_to_cpp_pairs(input_file, output_file=None, sym_mode="both", keep_self_loops=False,
                         var_name="edges_input", add_max_nodes=True, static_const=True):
    edges = read_edges(input_file)
    edges_sym = symmetrize_edges(edges, mode=sym_mode, drop_self_loops=not keep_self_loops)
    cpp_code = to_cpp_pairs(edges_sym, var_name=var_name,
                            add_max_nodes=add_max_nodes, static_const=static_const)

    if output_file:
        with open(output_file, 'w') as f:
            f.write(cpp_code)
        print(f"✅ Output written to {output_file}")
    else:
        print(cpp_code)

if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description="Convert adjacency list to C++ std::pair<int,int> array (with symmetrization).")
    p.add_argument("input_file", help="Path to the adjacency list file (each line: 'u v').")
    p.add_argument("-o", "--output_file", help="Optional output file path (e.g., edges_input.hpp).")
    p.add_argument("--symmetrize", choices=["both", "simple"], default="both",
                   help="Symmetrization mode: 'both' adds reverse edges; 'simple' keeps unique undirected edges.")
    p.add_argument("--keep-self-loops", action="store_true", help="Keep self-loops (u==v).")
    p.add_argument("--var-name", default="edges_input", help="C++ variable name for the array.")
    p.add_argument("--no-max-nodes", action="store_true", help="Do not emit maximum_nodes constant.")
    p.add_argument("--no-static-const", action="store_true", help="Do not mark the array as static const.")
    args = p.parse_args()

    convert_to_cpp_pairs(
        args.input_file,
        args.output_file,
        sym_mode=args.symmetrize,
        keep_self_loops=args.keep_self_loops,
        var_name=args.var_name,
        add_max_nodes=not args.no_max_nodes,
        static_const=not args.no_static_const,
    )

