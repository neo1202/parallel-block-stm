#!/usr/bin/env python3
"""plot PSC job results - throughput vs threads per version, contention curves, hot/cold."""

import csv
import os
import sys
from pathlib import Path

import matplotlib.pyplot as plt

VERSIONS = [
    ("V0-mutex-list", "V0 mutex+list", "tab:gray"),
    ("V0-mutex-tree", "V0 mutex+tree", "tab:brown"),
    ("V1-lfchain-nobackoff", "V1 LF (no backoff)", "tab:orange"),
    ("V2-lfchain-backoff", "V2 LF + backoff", "tab:red"),
    ("V3-lfsched", "V3 LF scheduler", "tab:purple"),
    ("V4-arena", "V4 + arena", "tab:green"),
]

def load(base, version, exp):
    """yield (threads, accounts, tps) rows."""
    path = Path(base) / f"psc_40118226_{version}" / f"exp_{exp}.csv"
    if not path.exists():
        return []
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("threads,"):
                continue
            parts = line.split(",")
            out.append((int(parts[0]), int(parts[1]), float(parts[3])))
    return out

def plot_scaling(base, accounts, outfile):
    """throughput vs threads for a fixed account count, one line per version."""
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for v, label, color in VERSIONS:
        rows = [(t, tps) for t, a, tps in load(base, v, "a_contention") if a == accounts]
        rows.sort()
        if not rows:
            continue
        threads, tpsvals = zip(*rows)
        ax.plot(threads, tpsvals, marker="o", label=label, color=color, linewidth=1.8)
    ax.set_xscale("log", base=2)
    ax.set_yscale("log", base=10)
    ax.set_xticks([1, 2, 4, 8, 16, 32, 64, 128])
    ax.set_xticklabels(["1", "2", "4", "8", "16", "32", "64", "128"])
    ax.set_xlabel("threads")
    ax.set_ylabel("throughput (tx/s)")
    ax.set_title(f"scaling @ accounts={accounts}")
    ax.grid(True, alpha=0.3, which="both")
    ax.legend(loc="lower right", fontsize=8)
    fig.tight_layout()
    fig.savefig(outfile, dpi=150)
    plt.close(fig)
    print("wrote", outfile)

def plot_contention(base, threads, outfile):
    """throughput vs accounts at a fixed thread count, one line per version."""
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for v, label, color in VERSIONS:
        rows = [(a, tps) for t, a, tps in load(base, v, "a_contention") if t == threads]
        rows.sort()
        if not rows:
            continue
        accts, tpsvals = zip(*rows)
        ax.plot(accts, tpsvals, marker="o", label=label, color=color, linewidth=1.8)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("accounts  (fewer = higher contention)")
    ax.set_ylabel("throughput (tx/s)")
    ax.set_title(f"contention sweep @ {threads} threads")
    ax.grid(True, alpha=0.3, which="both")
    ax.legend(loc="lower right", fontsize=8)
    fig.tight_layout()
    fig.savefig(outfile, dpi=150)
    plt.close(fig)
    print("wrote", outfile)

def plot_hotcold(base, outfile):
    """hot/cold: throughput vs threads."""
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for v, label, color in VERSIONS:
        rows = [(t, tps) for t, _, tps in load(base, v, "b_hotcold")]
        rows.sort()
        if not rows:
            continue
        threads, tpsvals = zip(*rows)
        ax.plot(threads, tpsvals, marker="o", label=label, color=color, linewidth=1.8)
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks([1, 2, 4, 8, 16, 32, 64, 128])
    ax.set_xticklabels(["1", "2", "4", "8", "16", "32", "64", "128"])
    ax.set_xlabel("threads")
    ax.set_ylabel("throughput (tx/s)")
    ax.set_title("dex hot/cold (1000 accts, 50 hot, hot_tx_ratio=0.2)")
    ax.grid(True, alpha=0.3, which="both")
    ax.legend(loc="lower right", fontsize=8)
    fig.tight_layout()
    fig.savefig(outfile, dpi=150)
    plt.close(fig)
    print("wrote", outfile)

if __name__ == "__main__":
    base = "benchmark_records"
    out = Path("benchmark_records/plots_psc_40118226")
    out.mkdir(parents=True, exist_ok=True)

    for acc in [2, 100, 1000, 10000]:
        plot_scaling(base, acc, out / f"scaling_accounts{acc}.png")
    for threads in [8, 128]:
        plot_contention(base, threads, out / f"contention_{threads}t.png")
    plot_hotcold(base, out / "hotcold_scaling.png")
