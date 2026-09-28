#!/usr/bin/env python3
"""Run the AGV scheduler-comparison benchmark for every scheduler and
produce one comparative results/comparison.csv (plus each scheduler's raw
COOJA.testlog in its own results/<scheduler>/ folder).

Usage:
  python3 run_benchmarks.py                       # all 4 schedulers, seed 1
  python3 run_benchmarks.py --seed 2 --schedulers minimal sf-mobility

After a change to sf-mobility.c in ../mobility-adaptive-msf, sync it here
first (see sync_sf_mobility.sh), then just re-run this script.
"""
import argparse
import os
import subprocess
import sys

SELF_PATH = os.path.dirname(os.path.abspath(__file__))
CONTIKI_PATH = os.path.dirname(os.path.dirname(os.path.dirname(SELF_PATH)))
COOJA_PATH = os.path.normpath(os.path.join(CONTIKI_PATH, "tools", "cooja"))
TEMPLATE_PATH = os.path.join(SELF_PATH, "cooja-sims", "cooja-agv-template.csc")
RESULTS_ROOT = os.path.join(SELF_PATH, "results")

ALL_SCHEDULERS = ["minimal", "orchestra-sb", "orchestra-rb", "sf-mobility"]
CSV_HEADER = ("scheduler,pdr,par,link_loss_rate,duty_cycle,queue_loss,"
              "parent_changes,latency_ms,etx,charge_proxy_ticks")


def render_csc(scheduler, seed, out_path):
    with open(TEMPLATE_PATH, "r") as f:
        text = f.read()
    text = (text.replace("__SCHEDULER__", scheduler)
                .replace("__SEED__", str(seed))
                .replace("__EXTRA_DEFINES__", ""))
    with open(out_path, "w") as f:
        f.write(text)


def run_one(scheduler, seed):
    out_dir = os.path.join(RESULTS_ROOT, scheduler)
    os.makedirs(out_dir, exist_ok=True)
    csc_path = os.path.join(out_dir, "run.csc")
    render_csc(scheduler, seed, csc_path)

    testlog = os.path.join(out_dir, "COOJA.testlog")
    if os.path.exists(testlog):
        os.remove(testlog)

    args = " ".join([
        COOJA_PATH + "/gradlew", "--no-watch-fs", "--parallel", "--build-cache",
        "-p", COOJA_PATH, "run",
        "--args='--contiki=" + CONTIKI_PATH, "--no-gui",
        "--logdir=" + out_dir, "--random-seed=" + str(seed), csc_path + "'",
    ])
    print("[{}] running: {}".format(scheduler, args))
    proc = subprocess.run(args, shell=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, universal_newlines=True)
    if proc.returncode != 0:
        print("[{}] FAILED (exit {}):\n{}".format(scheduler, proc.returncode, proc.stdout))
        return False
    if not os.path.exists(testlog):
        print("[{}] FAILED: no COOJA.testlog produced".format(scheduler))
        return False
    print("[{}] done, log at {}".format(scheduler, testlog))
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--schedulers", nargs="+", default=ALL_SCHEDULERS,
                         choices=ALL_SCHEDULERS)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    os.makedirs(RESULTS_ROOT, exist_ok=True)
    comparison_csv = os.path.join(RESULTS_ROOT, "comparison.csv")
    rows = []

    for scheduler in args.schedulers:
        if not run_one(scheduler, args.seed):
            continue
        testlog = os.path.join(RESULTS_ROOT, scheduler, "COOJA.testlog")
        extract = os.path.join(SELF_PATH, "extract_metrics.py")
        proc = subprocess.run([sys.executable, extract, testlog, scheduler],
                               stdout=subprocess.PIPE, universal_newlines=True)
        if proc.returncode == 0:
            rows.append(proc.stdout.strip())
        else:
            print("[{}] extract_metrics.py failed".format(scheduler))

    with open(comparison_csv, "w") as f:
        f.write(CSV_HEADER + "\n")
        for row in rows:
            f.write(row + "\n")
    print("Wrote {}".format(comparison_csv))

    if rows:
        subprocess.run([sys.executable, os.path.join(SELF_PATH, "plot_comparison.py"),
                         comparison_csv])


if __name__ == "__main__":
    main()
