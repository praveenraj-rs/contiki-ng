#!/usr/bin/env python3
"""Run every selected Cooja simulation with every selected scheduler, then analyze the logs.

  python3 benchmark.py                                  all simulations x all schedulers
  python3 benchmark.py --sims 1-cooja --schedulers msf  one simulation, one scheduler
  python3 benchmark.py --analysis-only                  skip Cooja, re-analyze existing logs
  python3 benchmark.py --simulate-only                  skip the analysis
  python3 benchmark.py --list                           show available simulations and schedulers

Simulations are the cooja-sims/*.csc files, schedulers are the folders in schedulers/
(each with scheduler.mk and scheduler-conf.h). Results go to logs/<sim>/:
  testlogs/<scheduler>.testlog, plots/<scheduler>.pdf, metrics/<scheduler>.csv,
  result.csv (one row per scheduler, built from metrics/), result.pdf (same data as charts).
"""

import argparse
import csv
import glob
import os
import subprocess
import sys

SELF_PATH = os.path.dirname(os.path.abspath(__file__))
SIMS_DIR = os.path.join(SELF_PATH, "cooja-sims")
SCHEDULERS_DIR = os.path.join(SELF_PATH, "schedulers")
LOGS_DIR = os.path.join(SELF_PATH, "logs")

STATS = ("avg", "min", "max", "std")

# Titles of the metrics in result.pdf, keyed by the metric names of metrics/*.csv
METRIC_TITLES = {
    "pdr": "Packet Delivery Ratio, %",
    "par": "Packet Acknowledgement Ratio, %",
    "queue_drops": "Packets dropped from queue, per node",
    "rpl_parent_switches": "RPL parent switches, per node",
    "radio_duty_cycle": "Radio Duty Cycle, %",
    "joined_radio_duty_cycle": "Joined Radio Duty Cycle, %",
    "charge_consumption": "Charge consumption, mC",
}

#######################################################

def find_sims():
    return sorted(os.path.splitext(os.path.basename(p))[0]
                  for p in glob.glob(os.path.join(SIMS_DIR, "*.csc")))

def find_schedulers():
    return sorted(os.path.basename(os.path.dirname(p))
                  for p in glob.glob(os.path.join(SCHEDULERS_DIR, "*", "scheduler.mk")))

# Accept the full name ("1-cooja") or just its number prefix ("1")
def select(requested, available, kind):
    if not requested:
        return available
    selected = []
    for r in requested:
        matches = [a for a in available if a == r or a.split("-")[0] == r]
        if not matches:
            sys.exit('Unknown {} "{}", available: {}'.format(kind, r, ", ".join(available)))
        selected += [m for m in matches if m not in selected]
    return selected

#######################################################

class RunPaths:
    def __init__(self, logs_dir, sim, scheduler):
        base = os.path.join(logs_dir, sim)
        self.base = base
        self.testlog = os.path.join(base, "testlogs", scheduler + ".testlog")
        self.pdf = os.path.join(base, "plots", scheduler + ".pdf")
        self.csv = os.path.join(base, "metrics", scheduler + ".csv")
        self.result = os.path.join(base, "result.csv")
        for p in (self.testlog, self.pdf, self.csv):
            os.makedirs(os.path.dirname(p), exist_ok=True)

#######################################################

def simulate(sim, scheduler, paths):
    sim_file = os.path.join(SIMS_DIR, sim + ".csc")
    logdir = os.path.dirname(paths.testlog)
    cmd = [sys.executable, os.path.join(SELF_PATH, "run-cooja.py"), sim_file,
           "--scheduler", scheduler, "--logdir", logdir]
    if subprocess.call(cmd, cwd=SELF_PATH) != 0:
        return "Cooja run failed"

    produced = os.path.join(logdir, "COOJA.testlog")
    if not os.path.exists(produced):
        return "no COOJA.testlog was produced"
    os.replace(produced, paths.testlog)

    # node.c logs the scheduler it was built with: detects a build that ignored SCHEDULER
    with open(paths.testlog, "r") as f:
        if "scheduler: {}\n".format(scheduler) not in f.read():
            return "log does not show scheduler '{}' (wrong firmware was built?)".format(scheduler)
    return None

def analyze(paths):
    if not os.path.exists(paths.testlog):
        return "missing {}".format(paths.testlog)
    cmd = [sys.executable, os.path.join(SELF_PATH, "run-analysis.py"), paths.testlog,
           "--pdf", paths.pdf, "--csv", paths.csv]
    if subprocess.call(cmd, cwd=SELF_PATH) != 0:
        return "analysis failed"
    return None

#######################################################

# {scheduler: {metric: {stat: value as text}}} from metrics/<scheduler>.csv, and the metric names
def load_metrics(sim_dir):
    data = {}
    metrics = []
    for path in sorted(glob.glob(os.path.join(sim_dir, "metrics", "*.csv"))):
        scheduler = os.path.splitext(os.path.basename(path))[0]
        data[scheduler] = {}
        with open(path, "r", newline="") as f:
            for m in csv.DictReader(f):
                data[scheduler][m["metric"]] = {stat: m[stat] for stat in STATS}
                if m["metric"] not in metrics:
                    metrics.append(m["metric"])
    return data, metrics

# One row per scheduler
def write_result_csv(sim_dir, data, metrics):
    columns = ["{}_{}".format(m, stat) for m in metrics for stat in STATS]
    with open(os.path.join(sim_dir, "result.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["scheduler"] + columns)
        for scheduler, values in data.items():
            w.writerow([scheduler] + [values.get(m, {}).get(stat, "") for m in metrics for stat in STATS])

# Page 1: table of the averages, then one chart per metric (bar = avg, whisker = std, triangles = min/max)
def write_result_pdf(sim_dir, data, metrics):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as pl
    from matplotlib.backends.backend_pdf import PdfPages

    schedulers = list(data)
    nan = float("nan")

    def value(scheduler, metric, stat):
        text = data[scheduler].get(metric, {}).get(stat, "")
        return float(text) if text != "" else nan

    sim = os.path.basename(os.path.normpath(sim_dir))
    with PdfPages(os.path.join(sim_dir, "result.pdf")) as pdf:
        fig, ax = pl.subplots(figsize=(max(8, 1.3 * len(metrics) + 2), 0.5 * len(schedulers) + 2))
        ax.axis("off")
        table = ax.table(cellText=[[s] + ["{:.2f}".format(value(s, m, "avg")) for m in metrics] for s in schedulers],
                         colLabels=["Scheduler"] + [m.replace("_", "\n") for m in metrics],
                         loc="center", cellLoc="center")
        table.auto_set_font_size(False)
        table.set_fontsize(8)
        table.scale(1, 2)
        for j in range(len(metrics) + 1):
            table[0, j].set_height(2 * table[0, j].get_height())
        ax.set_title("{}: average over the nodes, per scheduler".format(sim), fontsize=10)
        pdf.savefig(fig, bbox_inches="tight")
        pl.close(fig)

        for metric in metrics:
            avgs = [value(s, metric, "avg") for s in schedulers]
            mins = [value(s, metric, "min") for s in schedulers]
            maxs = [value(s, metric, "max") for s in schedulers]
            stds = [value(s, metric, "std") for s in schedulers]
            x = range(len(schedulers))

            fig, ax = pl.subplots(figsize=(max(5, 1.2 * len(schedulers) + 2), 4.5))
            ax.bar(x, avgs, width=0.5, color="orange", edgecolor="black", label="average")
            ax.errorbar(x, avgs, yerr=stds, fmt="none", ecolor="black", capsize=4, label="std")
            ax.scatter(x, mins, marker="v", color="blue", zorder=3, label="min (node)")
            ax.scatter(x, maxs, marker="^", color="red", zorder=3, label="max (node)")
            tops = [max(maxs[i], avgs[i] + stds[i]) for i in x]
            for i in x:
                if avgs[i] == avgs[i]:
                    ax.text(i, tops[i], "{:.2f}".format(avgs[i]), ha="center", va="bottom", fontsize=8)

            top = max([v for v in tops if v == v] or [0])
            bottom = min([v for v in mins if v == v] + [80]) if metric == "pdr" else 0
            if top <= bottom:
                top = bottom + 1
            ax.set_ylim(bottom, top + 0.12 * (top - bottom))
            ax.set_xticks(list(x))
            ax.set_xticklabels(schedulers, rotation=30, ha="right")
            ax.set_ylabel(METRIC_TITLES.get(metric, metric))
            ax.set_title("{}: {}".format(sim, METRIC_TITLES.get(metric, metric)))
            ax.grid(axis="y", linestyle=":", alpha=0.5)
            ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.3), ncol=4, fontsize=8)
            pdf.savefig(fig, bbox_inches="tight")
            pl.close(fig)

def update_results(sim_dir):
    data, metrics = load_metrics(sim_dir)
    write_result_csv(sim_dir, data, metrics)
    if data:
        write_result_pdf(sim_dir, data, metrics)

#######################################################

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sims", nargs="+", metavar="SIM", help="simulations to run (default: all)")
    parser.add_argument("--schedulers", nargs="+", metavar="SCHED", help="schedulers to run (default: all)")
    parser.add_argument("--logs-dir", default=LOGS_DIR, help="output directory (default: logs)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--analysis-only", action="store_true", help="do not run Cooja, analyze existing testlogs")
    mode.add_argument("--simulate-only", action="store_true", help="run Cooja but skip the analysis")
    parser.add_argument("--list", action="store_true", help="list simulations and schedulers, then exit")
    args = parser.parse_args()

    all_sims = find_sims()
    all_schedulers = find_schedulers()
    if args.list:
        print("simulations:", ", ".join(all_sims))
        print("schedulers: ", ", ".join(all_schedulers))
        return 0

    sims = select(args.sims, all_sims, "simulation")
    schedulers = select(args.schedulers, all_schedulers, "scheduler")

    failures = []
    for sim in sims:
        for scheduler in schedulers:
            print("\n===== {} / {} =====".format(sim, scheduler), flush=True)
            paths = RunPaths(args.logs_dir, sim, scheduler)

            error = None
            if not args.analysis_only:
                error = simulate(sim, scheduler, paths)
            if error is None and not args.simulate_only:
                error = analyze(paths)
            if error is not None:
                print("FAILED: {}".format(error), flush=True)
                failures.append((sim, scheduler, error))

        if not args.simulate_only:
            update_results(os.path.join(args.logs_dir, sim))
            print("\nComparison written to {} and result.pdf".format(os.path.join(args.logs_dir, sim, "result.csv")))

    print("\n===== summary =====")
    print("{} run(s), {} failed".format(len(sims) * len(schedulers), len(failures)))
    for sim, scheduler, error in failures:
        print("  {} / {}: {}".format(sim, scheduler, error))
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
