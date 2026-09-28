#!/usr/bin/env python3
"""Turn results/comparison.csv (one row per scheduler) into one bar-chart
PDF per metric, saved to results/figures/.

Usage: python3 plot_comparison.py [comparison.csv]
"""
import csv
import os
import sys
import matplotlib.pyplot as pl

SELF_PATH = os.path.dirname(os.path.abspath(__file__))
METRICS = [
    ("pdr", "Packet Delivery Ratio, %"),
    ("par", "Link-layer Ack Ratio (PAR), %"),
    ("link_loss_rate", "Link Loss Rate, %"),
    ("duty_cycle", "Radio Duty Cycle, %"),
    ("queue_loss", "Queue-dropped packets (count)"),
    ("parent_changes", "RPL parent changes (count)"),
    ("latency_ms", "End-to-end Latency, ms"),
    ("etx", "ETX to parent (x128)"),
]


def load_rows(csv_path):
    with open(csv_path, "r") as f:
        return list(csv.DictReader(f))


def plot(rows, metric, ylabel, out_dir):
    labels = [r["scheduler"] for r in rows]
    values = [float(r[metric]) for r in rows]

    pl.figure(figsize=(5, 4))
    bars = pl.bar(range(len(values)), values, width=0.5)
    for b in bars:
        b.set_color("steelblue")
        b.set_edgecolor("black")
    pl.xticks(range(len(labels)), labels, rotation=30, ha="right")
    pl.ylabel(ylabel)
    pl.title(ylabel)
    pl.tight_layout()
    out_path = os.path.join(out_dir, "plot_{}.pdf".format(metric))
    pl.savefig(out_path)
    pl.close()
    print("Wrote {}".format(out_path))


def main():
    csv_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(SELF_PATH, "results", "comparison.csv")
    if not os.access(csv_path, os.R_OK):
        print("Cannot read {}".format(csv_path))
        sys.exit(1)

    rows = load_rows(csv_path)
    if not rows:
        print("{} has no data rows".format(csv_path))
        sys.exit(1)

    out_dir = os.path.join(os.path.dirname(csv_path), "figures")
    os.makedirs(out_dir, exist_ok=True)
    for metric, ylabel in METRICS:
        plot(rows, metric, ylabel, out_dir)


if __name__ == "__main__":
    main()
