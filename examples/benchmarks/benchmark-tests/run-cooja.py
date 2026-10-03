#!/usr/bin/env python3

import argparse
import re
import shutil
import sys
import os
import tempfile
from subprocess import Popen, PIPE, STDOUT, CalledProcessError

# get the path of this example
SELF_PATH = os.path.dirname(os.path.abspath(__file__))
# move three levels up
CONTIKI_PATH = os.path.dirname(os.path.dirname(os.path.dirname(SELF_PATH)))

COOJA_PATH = os.path.normpath(os.path.join(CONTIKI_PATH, "tools", "cooja"))
cooja_input = 'cooja-sims/1-cooja.csc'
COOJA_LOG_NAME = 'COOJA.testlog'

#######################################################
# Run a child process and get its output

def run_subprocess(args, input_string):
    retcode = -1
    stdoutdata = ''
    try:
        proc = Popen(args, stdout=PIPE, stderr=STDOUT, stdin=PIPE, shell=True, universal_newlines=True)
        (stdoutdata, stderrdata) = proc.communicate(input_string)
        if not stdoutdata:
            stdoutdata = '\n'
        if stderrdata:
            stdoutdata += stderrdata + '\n'
        retcode = proc.returncode
    except OSError as e:
        sys.stderr.write("run_subprocess OSError:" + str(e))
    except CalledProcessError as e:
        sys.stderr.write("run_subprocess CalledProcessError:" + str(e))
        retcode = e.returncode
    except Exception as e:
        sys.stderr.write("run_subprocess exception:" + str(e))
    finally:
        return (retcode, stdoutdata)

#############################################################
# Cooja ignores a missing positions file without any error (the motes then
# never move), so every file the simulation references is checked up front.

def missing_files(cooja_file):
    base = os.path.dirname(os.path.abspath(cooja_file))
    with open(cooja_file, "r") as f:
        text = f.read()
    paths = re.findall(r"<(positions|scriptfile|source)>(.*?)</\1>", text)
    missing = []
    for _, p in paths:
        full = os.path.normpath(p.replace("[CONFIG_DIR]", base).replace("[CONTIKI_DIR]", CONTIKI_PATH))
        if not os.path.exists(full) and full not in missing:
            missing.append(full)
    return missing

#############################################################
# Cooja does not pass our environment to the mote build, so the scheduler is
# put on the make command line of a temporary copy of the simulation file.

def make_scheduler_csc(cooja_file, scheduler, tmpdir):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", scheduler):
        raise ValueError("invalid scheduler name: {}".format(scheduler))
    with open(cooja_file, "r") as f:
        text = f.read()
    # [CONFIG_DIR] must keep pointing at the original directory
    text = text.replace("[CONFIG_DIR]", os.path.dirname(os.path.abspath(cooja_file)))
    text = text.replace("$(MAKE) ", "$(MAKE) SCHEDULER={} ".format(scheduler))
    tmp_file = os.path.join(tmpdir, os.path.basename(cooja_file))
    with open(tmp_file, "w") as f:
        f.write(text)
    return tmp_file

#############################################################
# Run a single instance of Cooja on a given simulation script

def execute_test(cooja_file, logdir=SELF_PATH, scheduler=None):
    os.makedirs(logdir, exist_ok=True)
    cooja_output = os.path.join(logdir, COOJA_LOG_NAME)

    # cleanup
    try:
        os.remove(cooja_output)
    except FileNotFoundError as ex:
        pass
    except PermissionError as ex:
        print("Cannot remove previous Cooja output:", ex)
        return False

    filename = os.path.join(SELF_PATH, cooja_file)
    missing = missing_files(filename)
    if missing:
        sys.stderr.write("Files referenced by {} do not exist:\n  {}\n".format(cooja_file, "\n  ".join(missing)))
        return False
    tmpdir = None
    if scheduler:
        tmpdir = tempfile.mkdtemp()
        filename = make_scheduler_csc(filename, scheduler, tmpdir)
    try:
        return run_cooja(filename, cooja_output, logdir, scheduler)
    finally:
        if tmpdir:
            shutil.rmtree(tmpdir, ignore_errors=True)

def run_cooja(filename, cooja_output, logdir, scheduler):
    args = " ".join([COOJA_PATH + "/gradlew --no-watch-fs --parallel --build-cache -p", COOJA_PATH, "run --args='--contiki=" + CONTIKI_PATH, "--no-gui", "--logdir=" + logdir, filename + "'"])
    sys.stdout.write("  Running Cooja, scheduler={}, args={}\n".format(scheduler or "default", args))

    (retcode, output) = run_subprocess(args, '')
    if retcode != 0:
        sys.stderr.write("Failed, retcode=" + str(retcode) + ", output:")
        sys.stderr.write(output)
        return False

    sys.stdout.write("  Checking for output...")

    is_done = False
    with open(cooja_output, "r") as f:
        for line in f.readlines():
            line = line.strip()
            if line == "TEST OK":
                sys.stdout.write(" done.\n")
                is_done = True
                continue

    if not is_done:
        sys.stdout.write("  test failed.\n")
        return False

    sys.stdout.write(" test done\n")
    return True

#######################################################
# Run the application

def main():
    parser = argparse.ArgumentParser(description="Run one Cooja simulation")
    parser.add_argument("sim", nargs="?", default=cooja_input, help="Cooja .csc file")
    parser.add_argument("--scheduler", help="scheduler folder name in schedulers/")
    parser.add_argument("--logdir", default=SELF_PATH, help="where COOJA.testlog is written")
    args = parser.parse_args()

    input_file = args.sim

    if not os.access(input_file, os.R_OK):
        print('Simulation script "{}" does not exist'.format(input_file))
        exit(-1)

    print('Using simulation script "{}"'.format(input_file))
    if not execute_test(input_file, os.path.abspath(args.logdir), args.scheduler):
        exit(-1)

#######################################################

if __name__ == '__main__':
    main()
