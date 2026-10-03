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
  result.csv (one row per scheduler, built from metrics/).
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

# One row per scheduler from every metrics/<scheduler>.csv of the simulation
def update_result_csv(sim_dir):
    columns = []
    rows = []
    for path in sorted(glob.glob(os.path.join(sim_dir, "metrics", "*.csv"))):
        row = {"scheduler": os.path.splitext(os.path.basename(path))[0]}
        with open(path, "r", newline="") as f:
            for m in csv.DictReader(f):
                for stat in STATS:
                    col = "{}_{}".format(m["metric"], stat)
                    row[col] = m[stat]
                    if col not in columns:
                        columns.append(col)
        rows.append(row)

    with open(os.path.join(sim_dir, "result.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["scheduler"] + columns, restval="")
        w.writeheader()
        w.writerows(rows)

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
            update_result_csv(os.path.join(args.logs_dir, sim))
            print("\nComparison written to {}".format(os.path.join(args.logs_dir, sim, "result.csv")))

    print("\n===== summary =====")
    print("{} run(s), {} failed".format(len(sims) * len(schedulers), len(failures)))
    for sim, scheduler, error in failures:
        print("  {} / {}: {}".format(sim, scheduler, error))
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
