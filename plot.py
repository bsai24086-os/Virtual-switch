#!/usr/bin/env python3
"""Draws bench_results.png from bench_results.csv (run ./bench.sh first)."""
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

rows = list(csv.DictReader(open("bench_results.csv")))
modes = ["global", "striped", "rwlock"]
labels = {"global": "one global lock", "striped": "striped locks (64)",
          "rwlock": "read-write lock"}
styles = {"global": "o-", "striped": "s-", "rwlock": "^-"}

fig, axes = plt.subplots(1, 2, figsize=(10, 4), sharey=True)
for ax, pct in zip(axes, ["95", "50"]):
    for m in modes:
        pts = sorted((int(r["threads"]), float(r["mops"]))
                     for r in rows if r["mode"] == m and r["read_pct"] == pct)
        ax.plot([p[0] for p in pts], [p[1] for p in pts], styles[m], label=labels[m])
    ax.set_title(f"{pct}% lookups, {100 - int(pct)}% learns")
    ax.set_xlabel("threads")
    ax.set_xscale("log", base=2)
    ax.set_xticks([1, 2, 4, 8, 16])
    ax.set_xticklabels(["1", "2", "4", "8", "16"])
    ax.grid(True, alpha=0.3)
axes[0].set_ylabel("throughput (million ops/s)")
h, l = axes[0].get_legend_handles_labels()
fig.legend(h, l, loc="lower center", ncol=3, frameon=False)
fig.suptitle("MAC table throughput by lock design")
fig.tight_layout(rect=(0, 0.07, 1, 1))
fig.savefig("bench_results.png", dpi=150)
print("wrote bench_results.png")