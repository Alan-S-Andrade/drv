#!/usr/bin/env python3
"""Plot CACTI and McPAT energy/area results for Panther single pod."""

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import os
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "energy_area_results_panther")

# ── Data from CACTI (per instance, 22 nm) ──────────────────────────
cacti = {
    "L1SP\n(128 KiB×64)": {
        "instances": 64,
        "access_time_ns": 0.43105,
        "read_energy_nJ": 0.0320924,
        "write_energy_nJ": 0.0432863,
        "leakage_mW": 39.0981,
        "gate_leakage_mW": 0.0764713,
        "area_mm2": 0.168259,
    },
    "L2SP Bank\n(128 KiB×8)": {
        "instances": 8,
        "access_time_ns": 0.604104,
        "read_energy_nJ": 0.0453196,
        "write_energy_nJ": 0.0595205,
        "leakage_mW": 48.9939,
        "gate_leakage_mW": 0.10268,
        "area_mm2": 0.337594,
    },
    "DRAM Cache\n(64 KiB×1)": {
        "instances": 1,
        "access_time_ns": 0.811729,
        "read_energy_nJ": 0.0835001,
        "write_energy_nJ": 0.0865676,
        "leakage_mW": 22.36,
        "gate_leakage_mW": 0.0470035,
        "area_mm2": 0.218832 + 0.00372938,  # data + tag
    },
}

# ── Data from McPAT (full pod, 22 nm, 1 GHz) ──────────────────────
mcpat = {
    "Cores (×64)": {
        "area_mm2": 150.80,
        "peak_dyn_W": 18.888,
        "runtime_dyn_W": 3.97124,
        "sub_leakage_W": 13.324,
        "gate_leakage_W": 0.131707,
    },
    "L2 Banks (×8)": {
        "area_mm2": 3.16953,
        "peak_dyn_W": 0.211273,
        "runtime_dyn_W": 0.00183384,
        "sub_leakage_W": 0.215135,
        "gate_leakage_W": 0.000562404,
    },
    "NoC (Bus)": {
        "area_mm2": 0.0790939,
        "peak_dyn_W": 0.102698,
        "runtime_dyn_W": 0.0026061,
        "sub_leakage_W": 0.0078612,
        "gate_leakage_W": 7.76667e-05,
    },
    "Mem Controller": {
        "area_mm2": 0.325032,
        "peak_dyn_W": 0.401869,
        "runtime_dyn_W": 0.06092,
        "sub_leakage_W": 0.00420039,
        "gate_leakage_W": 4.04871e-05,
    },
}

# McPAT per-core sub-components (single core)
core_sub = {
    "Instr Fetch": {"area": 0.174005, "peak_dyn": 0.145694, "runtime_dyn": 1.2532},
    "Load/Store": {"area": 0.368501, "peak_dyn": 0.0483029, "runtime_dyn": 0.552564},
    "MMU": {"area": 0.0397297, "peak_dyn": 0.0202895, "runtime_dyn": 0.904446},
    "Exec Unit": {"area": 0.746149, "peak_dyn": 0.0808396, "runtime_dyn": 1.26103},
}

# ── Color palette ──────────────────────────────────────────────────
PAL = ["#2196F3", "#4CAF50", "#FF9800", "#E91E63", "#9C27B0"]
PAL2 = ["#1565C0", "#2E7D32", "#E65100", "#880E4F"]

SUPTITLE = ("Panther Single Pod (22 nm, 1 GHz)\n"
            "64 cores × 16 harts, 128 KiB L1SP, 1 MiB L2SP (8 banks)")

names = list(cacti.keys())
x = np.arange(len(names))
mc_names = list(mcpat.keys())
total_area = sum(mcpat[n]["area_mm2"] for n in mc_names)

def save(fig, name):
    p = os.path.join(OUT_DIR, name)
    fig.savefig(p, dpi=150, bbox_inches="tight")
    print(f"Saved: {p}")
    plt.close(fig)

# =====================================================================
# 1) CACTI: Per-instance access time
# =====================================================================
fig1, ax1 = plt.subplots(figsize=(6, 4.5))
fig1.suptitle(SUPTITLE, fontsize=10, fontweight="bold")
access_t = [cacti[n]["access_time_ns"] for n in names]
bars = ax1.bar(x, access_t, color=PAL[:3], edgecolor="black", linewidth=0.5)
for b, v in zip(bars, access_t):
    ax1.text(b.get_x() + b.get_width() / 2, b.get_height() + 0.01,
             f"{v:.3f}", ha="center", va="bottom", fontsize=10)
ax1.set_ylabel("Access Time (ns)")
ax1.set_title("CACTI: Access Latency (per instance)", fontsize=12)
ax1.set_xticks(x)
ax1.set_xticklabels(names, fontsize=9)
ax1.set_ylim(0, max(access_t) * 1.25)
fig1.tight_layout(rect=[0, 0, 1, 0.90])
save(fig1, "1_cacti_access_latency.png")

# =====================================================================
# 2) CACTI: Per-access energy (read vs write)
# =====================================================================
fig2, ax2 = plt.subplots(figsize=(6, 4.5))
fig2.suptitle(SUPTITLE, fontsize=10, fontweight="bold")
read_e = [cacti[n]["read_energy_nJ"] for n in names]
write_e = [cacti[n]["write_energy_nJ"] for n in names]
w = 0.35
bars_r = ax2.bar(x - w / 2, read_e, w, label="Read", color="#2196F3", edgecolor="black", linewidth=0.5)
bars_w = ax2.bar(x + w / 2, write_e, w, label="Write", color="#FF9800", edgecolor="black", linewidth=0.5)
for b, v in zip(bars_r, read_e):
    ax2.text(b.get_x() + b.get_width() / 2, b.get_height() + 0.001,
             f"{v:.4f}", ha="center", va="bottom", fontsize=8)
for b, v in zip(bars_w, write_e):
    ax2.text(b.get_x() + b.get_width() / 2, b.get_height() + 0.001,
             f"{v:.4f}", ha="center", va="bottom", fontsize=8)
ax2.set_ylabel("Energy per Access (nJ)")
ax2.set_title("CACTI: Read/Write Energy (per instance)", fontsize=12)
ax2.set_xticks(x)
ax2.set_xticklabels(names, fontsize=9)
ax2.legend(fontsize=9)
ax2.set_ylim(0, max(write_e) * 1.35)
fig2.tight_layout(rect=[0, 0, 1, 0.90])
save(fig2, "2_cacti_rw_energy.png")

# =====================================================================
# 3) CACTI: Pod-total area & leakage
# =====================================================================
fig3, ax3 = plt.subplots(figsize=(6, 4.5))
fig3.suptitle(SUPTITLE, fontsize=10, fontweight="bold")
pod_area = [cacti[n]["area_mm2"] * cacti[n]["instances"] for n in names]
pod_leak = [cacti[n]["leakage_mW"] * cacti[n]["instances"] for n in names]

bars_a = ax3.bar(x - w / 2, pod_area, w, label="Area (mm²)", color="#4CAF50", edgecolor="black", linewidth=0.5)
ax3b = ax3.twinx()
bars_l = ax3b.bar(x + w / 2, pod_leak, w, label="Leakage (mW)", color="#E91E63", edgecolor="black", linewidth=0.5)

for b, v in zip(bars_a, pod_area):
    ax3.text(b.get_x() + b.get_width() / 2, b.get_height() + 0.1,
             f"{v:.2f}", ha="center", va="bottom", fontsize=9)
for b, v in zip(bars_l, pod_leak):
    ax3b.text(b.get_x() + b.get_width() / 2, b.get_height() + 5,
              f"{v:.0f}", ha="center", va="bottom", fontsize=9)

ax3.set_ylabel("Area (mm²)", color="#4CAF50")
ax3b.set_ylabel("Leakage (mW)", color="#E91E63")
ax3.set_title("CACTI: Pod-Total Area & Leakage (instances × per-unit)", fontsize=11)
ax3.set_xticks(x)
ax3.set_xticklabels(names, fontsize=9)

lines_a = matplotlib.patches.Patch(color="#4CAF50", label="Area (mm²)")
lines_l = matplotlib.patches.Patch(color="#E91E63", label="Leakage (mW)")
ax3.legend(handles=[lines_a, lines_l], fontsize=9, loc="upper right")
fig3.tight_layout(rect=[0, 0, 1, 0.90])
save(fig3, "3_cacti_pod_area_leakage.png")

# =====================================================================
# 4) McPAT: Area breakdown — two-panel: full pod + per-core zoom
# =====================================================================
fig4, (ax4a, ax4b) = plt.subplots(2, 1, figsize=(11, 7),
                                   gridspec_kw={"height_ratios": [1, 1.2]})
fig4.suptitle(SUPTITLE, fontsize=10, fontweight="bold", x=0.45)

# Per-core sub-component areas
core_area = 2.35624         # mm² per core (McPAT)
l1sp_area = 0.30883         # Data Cache = L1SP
icache_area = 0.15831       # Instruction Cache
regfile_area = 0.021898     # Register Files — CACTI 8 KB 2R+1W 22nm
noc_link_area = 0.0790939 / 64  # Pod NoC ÷ 64 cores
other_core = core_area - l1sp_area - icache_area - regfile_area - noc_link_area

N_CORES = 64

# ── Top panel: Pod-level area as individual bars ──
pod_components = [
    ("Cores (×64)",     core_area * N_CORES,                   "#1565C0"),
    ("L2 Banks (×8)",   mcpat["L2 Banks (×8)"]["area_mm2"],    "#4CAF50"),
    ("Mem Ctrl",        mcpat["Mem Controller"]["area_mm2"],    "#E91E63"),
    ("NoC Bus",         mcpat["NoC (Bus)"]["area_mm2"],         "#FF9800"),
]

pod_names  = [d[0] for d in pod_components]
pod_vals   = [d[1] for d in pod_components]
pod_colors = [d[2] for d in pod_components]
y4a = np.arange(len(pod_names))

bars4a = ax4a.barh(y4a, pod_vals, height=0.6, color=pod_colors,
                   edgecolor="black", linewidth=0.5)
for b, v in zip(bars4a, pod_vals):
    ax4a.text(b.get_width() + 0.5, b.get_y() + b.get_height() / 2,
              f"{v:.2f} mm²", va="center", fontsize=9)

ax4a.set_yticks(y4a)
ax4a.set_yticklabels(pod_names, fontsize=10)
ax4a.set_xlabel("Area (mm²)")
ax4a.set_title(f"Pod-Level Area Breakdown — Total = {total_area:.1f} mm²", fontsize=11)
ax4a.invert_yaxis()

# ── Bottom panel: Per-core sub-component breakdown (individual bars) ──
per_core_data = [
    ("L1 Scratchpad\n(128 KiB)",  l1sp_area,      "#2196F3"),
    ("I-Cache",                    icache_area,     "#42A5F5"),
    ("Register File\n(16h×64 regs×64b)", regfile_area, "#FF7043"),
    ("NoC Link",                   noc_link_area,   "#BBDEFB"),
    ("Other\n(ALU, FPU, MMU,\nLSQ, Sched)",  other_core,     "#1565C0"),
]

pc_names = [d[0] for d in per_core_data]
pc_vals  = [d[1] for d in per_core_data]
pc_colors = [d[2] for d in per_core_data]
y_pos = np.arange(len(pc_names))

bars_pc = ax4b.barh(y_pos, pc_vals, height=0.6, color=pc_colors,
                    edgecolor="black", linewidth=0.5)
for b, v in zip(bars_pc, pc_vals):
    ax4b.text(b.get_width() + 0.01, b.get_y() + b.get_height() / 2,
              f"{v:.4f} mm²  ({v/core_area*100:.1f}%)",
              va="center", fontsize=9)

ax4b.set_yticks(y_pos)
ax4b.set_yticklabels(pc_names, fontsize=9)
ax4b.set_xlabel("Area (mm² per core)")
ax4b.set_title(f"Per-Core Sub-Component Area — Total per core = {core_area:.3f} mm²", fontsize=11)
ax4b.invert_yaxis()

fig4.tight_layout(rect=[0, 0, 1, 0.90])
save(fig4, "4_mcpat_area_breakdown.png")

# =====================================================================
# 5) McPAT: Power breakdown (stacked bar)
# =====================================================================
fig5, ax5 = plt.subplots(figsize=(7, 5))
fig5.suptitle(SUPTITLE, fontsize=10, fontweight="bold")
x5 = np.arange(len(mc_names))
runtime_d = [mcpat[n]["runtime_dyn_W"] for n in mc_names]
sub_leak = [mcpat[n]["sub_leakage_W"] for n in mc_names]
gate_leak = [mcpat[n]["gate_leakage_W"] for n in mc_names]

ax5.bar(x5, runtime_d, label="Runtime Dynamic", color="#2196F3", edgecolor="black", linewidth=0.5)
ax5.bar(x5, sub_leak, bottom=runtime_d, label="Subthreshold Leakage", color="#FF9800", edgecolor="black", linewidth=0.5)
ax5.bar(x5, gate_leak,
        bottom=[r + s for r, s in zip(runtime_d, sub_leak)],
        label="Gate Leakage", color="#E91E63", edgecolor="black", linewidth=0.5)

for i in range(len(mc_names)):
    tot = runtime_d[i] + sub_leak[i] + gate_leak[i]
    ax5.text(i, tot + 0.1, f"{tot:.2f} W", ha="center", va="bottom", fontsize=9)

ax5.set_ylabel("Power (W)")
ax5.set_title("McPAT: Power Breakdown (Runtime BFS Workload)", fontsize=12)
ax5.set_xticks(x5)
ax5.set_xticklabels(mc_names, fontsize=9)
ax5.legend(fontsize=8, loc="upper right")
ax5.set_ylim(0, 18)

total_runtime = sum(runtime_d)
total_leakage = sum(s + g for s, g in zip(sub_leak, gate_leak))
fig5.text(0.5, 0.01,
          f"Pod Total Runtime Power = {total_runtime + total_leakage:.2f} W  "
          f"(Dynamic {total_runtime:.2f} W + Leakage {total_leakage:.2f} W)",
          ha="center", fontsize=9,
          bbox=dict(boxstyle="round,pad=0.3", facecolor="#E3F2FD", edgecolor="#1565C0"))
fig5.tight_layout(rect=[0, 0.05, 1, 0.90])
save(fig5, "5_mcpat_power_breakdown.png")

# =====================================================================
# 6) McPAT: Core sub-component breakdown
# =====================================================================
fig6, ax6 = plt.subplots(figsize=(6, 4.5))
fig6.suptitle(SUPTITLE, fontsize=10, fontweight="bold")
sub_names = list(core_sub.keys())
sub_areas = [core_sub[n]["area"] for n in sub_names]
sub_rdyn = [core_sub[n]["runtime_dyn"] for n in sub_names]

x6 = np.arange(2)
w6 = 0.18

for i, sn in enumerate(sub_names):
    vals = [sub_areas[i], sub_rdyn[i]]
    bars6 = ax6.bar(x6 + i * w6, vals, w6, label=sn, color=PAL[:4][i],
                    edgecolor="black", linewidth=0.5)
    for b, v in zip(bars6, vals):
        ax6.text(b.get_x() + b.get_width() / 2, b.get_height() + 0.01,
                 f"{v:.3f}", ha="center", va="bottom", fontsize=7, rotation=45)

ax6.set_ylabel("Area (mm²) / Power (W)")
ax6.set_title("McPAT: Core Sub-Components (per core)", fontsize=12)
ax6.set_xticks([0 + 1.5 * w6, 1 + 1.5 * w6])
ax6.set_xticklabels(["Area (mm²)", "Runtime Dyn (W)"], fontsize=10)
ax6.legend(fontsize=8, loc="upper left")
fig6.tight_layout(rect=[0, 0, 1, 0.90])
save(fig6, "6_mcpat_core_subcomponents.png")
