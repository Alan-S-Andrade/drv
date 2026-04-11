#!/usr/bin/env python3
"""Generate a block diagram of a single Panther core."""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

fig, ax = plt.subplots(figsize=(6.5, 7.5))
ax.set_xlim(0, 10)
ax.set_ylim(0, 11)
ax.set_aspect("equal")
ax.axis("off")

# ── Colours (matching reference diagram) ──
DARK_BLUE  = "#1a3a5c"
PURPLE     = "#8e24aa"
GREEN      = "#4caf50"
CYAN       = "#00bcd4"
WHITE      = "#ffffff"

# ── Outer frame — "Panther Core" ──
outer = mpatches.FancyBboxPatch(
    (0.5, 0.5), 9, 10, boxstyle="round,pad=0.15",
    facecolor=DARK_BLUE, edgecolor="none")
ax.add_patch(outer)
ax.text(5, 9.9, "Panther Core", color=WHITE, fontsize=16,
        fontweight="bold", ha="center", va="center")

# ── L1 Scratchpad (left block) ──
l1sp = mpatches.FancyBboxPatch(
    (0.8, 1.6), 4.0, 6.8, boxstyle="round,pad=0.12",
    facecolor=PURPLE, edgecolor="none")
ax.add_patch(l1sp)
ax.text(2.8, 5.8, "128 KiB", color=WHITE, fontsize=14, fontweight="bold",
        ha="center", va="center")
ax.text(2.8, 4.8, "pipelined\ndata\nscratchpad", color=WHITE, fontsize=12,
        ha="center", va="center", linespacing=1.3)

# ── Register File (top-right) ──
rf = mpatches.FancyBboxPatch(
    (5.1, 5.8), 4.1, 2.6, boxstyle="round,pad=0.12",
    facecolor=PURPLE, edgecolor="none")
ax.add_patch(rf)
ax.text(7.15, 7.65, "8 KiB SRAM", color=WHITE, fontsize=11,
        fontweight="bold", ha="center", va="center")
ax.text(7.15, 7.0, "Register file", color=WHITE, fontsize=11,
        ha="center", va="center")
ax.text(7.15, 6.35, "16T × (32i + 32f)", color=WHITE, fontsize=10,
        ha="center", va="center")

# ── RISC-V core label (green bar, middle-right) ──
rv = mpatches.FancyBboxPatch(
    (5.1, 4.5), 4.1, 1.05, boxstyle="round,pad=0.08",
    facecolor=GREEN, edgecolor="none")
ax.add_patch(rv)
ax.text(7.15, 5.03, "16T RISC-V 64", color=WHITE, fontsize=12,
        fontweight="bold", ha="center", va="center")

# ── I-Cache (bottom-right) ──
ic = mpatches.FancyBboxPatch(
    (5.1, 1.6), 4.1, 2.6, boxstyle="round,pad=0.12",
    facecolor=PURPLE, edgecolor="none")
ax.add_patch(ic)
ax.text(7.15, 2.9, "16 KiB I-cache", color=WHITE, fontsize=12,
        fontweight="bold", ha="center", va="center")

# ── NOC Link (bottom bar) ──
noc = mpatches.FancyBboxPatch(
    (0.8, 0.6), 8.4, 0.75, boxstyle="round,pad=0.08",
    facecolor=CYAN, edgecolor="none")
ax.add_patch(noc)
ax.text(5, 0.97, "NOC Link", color=WHITE, fontsize=13,
        fontweight="bold", ha="center", va="center")

fig.tight_layout()
fig.savefig("panther_core_diagram.png", dpi=200, bbox_inches="tight",
            facecolor="white")
print("Saved panther_core_diagram.png")
