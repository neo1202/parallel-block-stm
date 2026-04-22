#!/usr/bin/env python3
# plot PSC job CSVs. usage: python3 plot_psc.py [job_id]

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

# 預設讀最新 job. 可以改成從 argv 吃
JOB_ID = sys.argv[1] if len(sys.argv) > 1 else "40118226"


def load(base, version, exp):
    path = Path(base) / f"psc_{JOB_ID}_{version}" / f"exp_{exp}.csv"
    if not path.exists():
        return []
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("threads,"):
                continue
            parts = line.split(",")
            t = int(parts[0])
            a = int(parts[1])
            tps = float(parts[3])
            # abort/wasted columns only exist in newer runs
            ar = float(parts[7]) if len(parts) > 7 else 0.0
            wr = float(parts[8]) if len(parts) > 8 else 0.0
            out.append((t, a, tps, ar, wr))
    return out


def plot_scaling(base, accounts, outfile):
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for v, label, color in VERSIONS:
        rows = [(t, tps) for t, a, tps, _, _ in load(base, v, "a_contention") if a == accounts]
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
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for v, label, color in VERSIONS:
        rows = [(a, tps) for t, a, tps, _, _ in load(base, v, "a_contention") if t == threads]
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
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for v, label, color in VERSIONS:
        rows = [(t, tps) for t, _, tps, _, _ in load(base, v, "b_hotcold")]
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


def plot_abort_rate_vs_contention(base, threads, outfile):
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for v, label, color in VERSIONS:
        rows = [(a, ar) for t, a, _, ar, _ in load(base, v, "a_contention") if t == threads]
        rows.sort()
        if not rows:
            continue
        accts, ars = zip(*rows)
        ax.plot(accts, ars, marker="o", label=label, color=color, linewidth=1.8)
    ax.set_xscale("log")
    ax.set_xlabel("accounts  (fewer = higher contention)")
    ax.set_ylabel("abort rate  (validation_aborts / block_size)")
    ax.set_title(f"abort rate vs contention @ {threads} threads")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper right", fontsize=8)
    fig.tight_layout()
    fig.savefig(outfile, dpi=150)
    plt.close(fig)
    print("wrote", outfile)


def plot_wasted_vs_threads(base, accounts, outfile):
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for v, label, color in VERSIONS:
        rows = [(t, wr) for t, a, _, _, wr in load(base, v, "a_contention") if a == accounts]
        rows.sort()
        if not rows:
            continue
        threads, wrs = zip(*rows)
        ax.plot(threads, wrs, marker="o", label=label, color=color, linewidth=1.8)
    ax.set_xscale("log", base=2)
    ax.set_xticks([1, 2, 4, 8, 16, 32, 64, 128])
    ax.set_xticklabels(["1", "2", "4", "8", "16", "32", "64", "128"])
    ax.set_xlabel("threads")
    ax.set_ylabel("wasted exec ratio  (re-executions / total_executions)")
    ax.set_title(f"wasted work vs threads @ accounts={accounts}")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left", fontsize=8)
    fig.tight_layout()
    fig.savefig(outfile, dpi=150)
    plt.close(fig)
    print("wrote", outfile)


if __name__ == "__main__":
    base = "benchmark_records"
    out = Path(f"benchmark_records/plots_psc_{JOB_ID}")
    out.mkdir(parents=True, exist_ok=True)

    for acc in [2, 100, 1000, 10000]:
        plot_scaling(base, acc, out / f"scaling_accounts{acc}.png")
    for threads in [8, 128]:
        plot_contention(base, threads, out / f"contention_{threads}t.png")
        plot_abort_rate_vs_contention(base, threads, out / f"abort_rate_{threads}t.png")
    plot_hotcold(base, out / "hotcold_scaling.png")

    # wasted work curves for the two configs where it matters most
    plot_wasted_vs_threads(base, 100, out / "wasted_accounts100.png")
    plot_wasted_vs_threads(base, 1000, out / "wasted_accounts1000.png")
