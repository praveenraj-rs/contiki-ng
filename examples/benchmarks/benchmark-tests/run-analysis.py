#!/usr/bin/env python3

import argparse
import csv
import os
import statistics
import sys
import time
import matplotlib.pyplot as pl
from matplotlib.backends.backend_pdf import PdfPages

###########################################

# If set to true, all nodes are plotted, even those with no valid data
PLOT_ALL_NODES = True

###########################################

LOG_FILE = 'COOJA.testlog'
OUTPUT_PDF = 'plots.pdf'
OUTPUT_CSV = 'metrics.csv'

# (key in results, name in the CSV)
CSV_METRICS = [
    ("pdr", "pdr"),
    ("par", "par"),
    ("queue_drops", "queue_drops"),
    ("rpl_switches", "rpl_parent_switches"),
    ("duty_cycle", "radio_duty_cycle"),
    ("duty_cycle_joined", "joined_radio_duty_cycle"),
    ("charge", "charge_consumption"),
]

COORDINATOR_ID = 1

# for charge calculations
CC2650_MHZ = 48
CC2650_RADIO_TX_CURRENT_MA          = 9.100 # at 5 dBm, from CC2650 datasheet
CC2650_RADIO_RX_CURRENT_MA          = 5.900 # from CC2650 datasheet
CC2650_RADIO_CPU_ON_CURRENT         = 0.061 * CC2650_MHZ # from CC2650 datasheet
CC2650_RADIO_CPU_SLEEP_CURRENT      = 1.335 # empirical
CC2650_RADIO_CPU_DEEP_SLEEP_CURRENT = 0.010 # empirical

###########################################

# for testbed: mapping between the node ID (Contiki_NG) and device ID (testbed)
node_id_to_device_id = {}

###########################################

class NodeStats:
    def __init__(self, id):
        self.id = id

        # intermediate metrics
        self.is_valid = False
        self.is_tsch_joined = False
        self.tsch_join_time_sec = None
        self.rpl_join_time_sec = None
        self.tsch_time_source = None
        self.rpl_parent = None
        self.max_seqnum_sent = 0
        self.seqnums_received_on_root = set()
        self.parent_packets_tx = 0
        self.parent_packets_ack = 0
        self.parent_packets_queue_dropped = 0
        self.energest_cpu_on = 0
        self.energest_cpu_sleep = 0
        self.energest_cpu_deep_sleep = 0
        self.energest_radio_tx = 0
        self.energest_radio_rx = 0
        self.energest_radio_rx_joined = 0
        self.energest_total = 0
        self.energest_total_joined = 0
        self.energest_ticks_per_second = 1
        self.energest_joined = False
        self.energest_period_seconds = 60

        # final metrics (uninitialized)
        self.pdr = 0.0
        self.rpl_parent_changes = 0
        self.par = 0.0
        self.rdc = None
        self.rdc_joined = None
        self.charge = None

    # calculate the final metrics
    def calc(self):
        if self.energest_total:
            radio_on = self.energest_radio_tx + self.energest_radio_rx
            self.rdc = 100.0 * radio_on / self.energest_total

            cpu_on_sec = self.energest_cpu_on / self.energest_ticks_per_second
            cpu_sleep_sec = self.energest_cpu_sleep / self.energest_ticks_per_second
            cpu_deep_sleep_sec = self.energest_cpu_deep_sleep / self.energest_ticks_per_second
            radio_tx_sec = self.energest_radio_tx / self.energest_ticks_per_second
            radio_rx_sec = self.energest_radio_rx / self.energest_ticks_per_second

            self.charge = CC2650_RADIO_TX_CURRENT_MA * radio_tx_sec \
                + CC2650_RADIO_RX_CURRENT_MA * radio_rx_sec \
                + CC2650_RADIO_CPU_ON_CURRENT * cpu_on_sec \
                + CC2650_RADIO_CPU_SLEEP_CURRENT * cpu_sleep_sec \
                + CC2650_RADIO_CPU_DEEP_SLEEP_CURRENT * cpu_deep_sleep_sec

        else:
            print("warning: no energest results for {}".format(self.id))
            self.rdc = 0.0
            self.charge = 0.0

        if self.energest_total_joined:
            radio_on_joined = self.energest_radio_tx + self.energest_radio_rx_joined
            self.rdc_joined = 100.0 * radio_on_joined / self.energest_total_joined
        else:
            self.rdc_joined = 0


        if self.tsch_join_time_sec is None:
            print("node {} never associated TSCH".format(self.id))
            return 0, 0, 0, 0, 0

        if self.rpl_join_time_sec is None:
            print("node {} never joined RPL DAG".format(self.id))
            return 0, 0, 0, 0, 0

        if self.max_seqnum_sent == 0:
            print("node {} never sent any data packets".format(self.id))
            return 0, 0, 0, 0, 0

        self.is_valid = True

        if self.parent_packets_tx:
            self.par = 100.0 * self.parent_packets_ack / self.parent_packets_tx
        else:
            self.par = 0.0

        expected = self.max_seqnum_sent
        actual = len(self.seqnums_received_on_root)
        if expected:
            self.pdr = 100.0 * actual / expected
        else:
            self.pdr = 0.0

        return self.parent_packets_tx, \
            self.parent_packets_ack, \
            self.parent_packets_queue_dropped, \
            self.max_seqnum_sent, \
            len(self.seqnums_received_on_root)


###########################################

def extract_macaddr(s):
    if "NULL" in s:
        return None
    return s

def extract_ipaddr(s):
    if "NULL" in s:
        return None
    return s

# (NULL IP addr) -> fe80::244:44:44:44
def extract_ipaddr_pair(fields):
    s = " ".join(fields)
    fields = s.split(" -> ")
    return extract_ipaddr(fields[0]), extract_ipaddr(fields[1])

def addr_to_id(addr):
    return int(addr.split(":")[-1], 16)

###########################################
# Parse a log file

def analyze_results(filename, is_testbed):
    nodes = {}

    in_initialization = True

    start_ts_unix = None

    with open(filename, "r") as f:
        for line in f:
            line = line.strip()
            if is_testbed:
                fields1 = line.split(";")
                fields2 = fields1[2].split()
                fields = fields1[:2] + fields2
            else:
                fields = line.split()

            try:
                # in milliseconds
                if is_testbed:
                    ts_unix = float(fields[0])
                    if start_ts_unix is None:
                        start_ts_unix = ts_unix
                    ts_unix -= start_ts_unix
                    ts = int(float(ts_unix) * 1000)
                    node = int(fields[1][3:])
                else:
                    ts = int(fields[0]) // 1000 # convert to ms
                    node = int(fields[1]) 
            except:
                # failed to extract timestamp
                continue

            if node not in nodes:
                nodes[node] = NodeStats(node)

            if "association done" in line:
                # has_assoc.add(node)
                #nodes[node].seqnums = set()
                if nodes[node].tsch_join_time_sec is None:
                    nodes[node].tsch_join_time_sec = ts / 1000
                nodes[node].is_tsch_joined = True
                continue

            if "leaving the network" in line:
                nodes[node].is_tsch_joined = False
                nodes[node].energest_joined = False
                continue

            # 536000 2 [INFO: TSCH Queue] update time source: (NULL LL addr) -> 0001.0001.0001.0001
            if "update time source" in line:
                nodes[node].tsch_time_source = extract_macaddr(line.split(" -> ")[1])
                continue

            # 2497128 2 [INFO: RPL       ] rpl_set_preferred_parent fe80::201:1:1:1 used to be NULL
            if "rpl_set_preferred_parent" in line:
                nodes[node].rpl_parent_changes += 1
                nodes[node].rpl_parent = extract_ipaddr(fields[6])
                if nodes[node].rpl_join_time_sec is None:
                    nodes[node].rpl_join_time_sec = ts / 1000
                continue

            # 377018480 76 [INFO: RPL       ] parent switch: (NULL IP addr) -> fe80::244:44:44:44
            if " parent switch: " in line:
                nodes[node].rpl_parent_changes += 1
                nodes[node].rpl_parent = extract_ipaddr_pair(fields[7:])[1]
                if nodes[node].rpl_join_time_sec is None:
                    nodes[node].rpl_join_time_sec = ts / 1000
                continue

            # 120904000 4 [INFO: App       ] app generate packet seqnum=1
            if "app generate packet" in line:
                seqnum = int(fields[8].split("=")[1])
                if is_testbed:
                    node_id = int(fields[9].split("=")[1])
                    node_id_to_device_id[node_id] = node
                nodes[node].max_seqnum_sent = max(nodes[node].max_seqnum_sent, seqnum)
                continue

            # 123047424 1 [INFO: App       ] app receive packet seqnum=1 from=fd00::208:8:8:8
            if "app receive packet" in line:
                seqnum = int(fields[8].split("=")[1])
                fromaddr = fields[9].split("=")[1]
                from_node = addr_to_id(fromaddr)
                if is_testbed:
                    from_node = node_id_to_device_id.get(from_node, 0)
                if from_node not in nodes:
                    nodes[from_node] = NodeStats(from_node)
                nodes[from_node].seqnums_received_on_root.add(seqnum)
                continue

            # 600142000 28 [INFO: Link Stats] num packets: tx=0 ack=0 rx=0 queue_drops=0 to=0014.0014.0014.0014
            if "num packets" in line:
                tx = int(fields[7].split("=")[1])
                ack = int(fields[8].split("=")[1])
                rx = int(fields[9].split("=")[1])
                queue_drops = int(fields[10].split("=")[1])
                to_addr = fields[11].split("=")[1]
                # only account for the (current) time source node
                if nodes[node].tsch_time_source == to_addr:
                    nodes[node].parent_packets_tx += tx
                    nodes[node].parent_packets_ack += ack
                    nodes[node].parent_packets_queue_dropped += queue_drops
                continue

            # 960073000 8 [INFO: Energest  ] Total time  :   60000000
            # 960073000 8 [INFO: Energest  ] CPU         :   60000000/  60000000 (69 permil)
            # 960073000 8 [INFO: Energest  ] LPM         :          0/  60000000 (0 permil)
            # 960073000 8 [INFO: Energest  ] Deep LPM    :          0/  60000000 (0 permil)
            # 960073000 8 [INFO: Energest  ] Radio Tx    :      49216/  60000000 (0 permil)
            # 960073000 8 [INFO: Energest  ] Radio Rx    :    2470552/  60000000 (41 permil)
            # 960073000 8 [INFO: Energest  ] Radio total :    2519768/  60000000 (41 permil)
            if "INFO: Energest" in line:
                if "Period" in line:
                    nodes[node].energest_period_seconds = int(fields[9][1:])
                elif "Total time" in line:
                    total = int(fields[8])
                    nodes[node].energest_total += total
                    nodes[node].energest_ticks_per_second = total / nodes[node].energest_period_seconds
                    if nodes[node].energest_joined:
                        nodes[node].energest_total_joined += total
                else:
                    ticks = int(fields[8][:-1])
                    if "CPU" in line:
                        nodes[node].energest_cpu_on += ticks
                    elif "Deep LPM" in line:
                        nodes[node].energest_cpu_sleep += ticks
                    elif "LPM" in line:
                        nodes[node].energest_cpu_deep_sleep += ticks
                    elif "Radio Tx" in line:
                        nodes[node].energest_radio_tx += ticks
                    elif "Radio Rx" in line:
                        nodes[node].energest_radio_rx += ticks
                        if nodes[node].energest_joined:
                            nodes[node].energest_radio_rx_joined += ticks
                        # update the state
                        nodes[node].energest_joined = nodes[node].is_tsch_joined
                    continue

    r = []
    # link layer PAR
    total_ll_sent = 0
    total_ll_acked = 0
    total_ll_queue_dropped = 0
    # end to end PDR
    total_e2e_sent = 0
    total_e2e_received = 0
    for k in sorted(nodes.keys()):
        n = nodes[k]
        if n.id == COORDINATOR_ID:
            continue
        ll_sent, ll_acked, ll_queue_dropped, e2e_sent, e2e_received = n.calc()
        if n.is_valid or PLOT_ALL_NODES:
            d = {
                "id": n.id,
                "pdr": n.pdr,
                "par": n.par,
                "queue_drops": ll_queue_dropped,
                "rpl_switches": n.rpl_parent_changes,
                "duty_cycle": n.rdc,
                "duty_cycle_joined": n.rdc_joined,
                "charge": n.charge
            }
            r.append(d)
            total_ll_sent += ll_sent
            total_ll_acked += ll_acked
            total_ll_queue_dropped += ll_queue_dropped
            total_e2e_sent += e2e_sent
            total_e2e_received += e2e_received
    ll_par = 100.0 * total_ll_acked / total_ll_sent if total_ll_sent else 0.0
    e2e_pdr = 100.0 * total_e2e_received / total_e2e_sent if total_e2e_sent else 0.0
    return r, ll_par, total_ll_queue_dropped, e2e_pdr

#######################################################
# Plot the results of a given metric as a bar chart

def plot(results, metric, ylabel, pdf):
    data = [r[metric] for r in results]
    avg = sum(data) / len(data) if data else 0.0

    fig, ax = pl.subplots(figsize=(max(6, 0.45 * len(data)), 4.5))

    x = range(len(data))
    bars = ax.bar(x, data, width=0.6, color="orange", edgecolor="black", linewidth=1)
    ax.bar_label(bars, labels=["{:.1f}".format(v) for v in data],
                 rotation=90, fontsize=7, padding=2)

    ax.axhline(avg, color="red", linestyle="--", linewidth=1.2,
               label="Average = {:.2f}".format(avg))
    ax.legend(loc="lower right")

    ids = [r["id"] for r in results]
    ax.set_xticks(list(x))
    ax.set_xticklabels([str(u) for u in ids], rotation=90)
    ax.set_xlabel("Node ID")
    ax.set_ylabel(ylabel)
    ax.set_title(ylabel)
    ax.grid(axis="y", linestyle=":", alpha=0.5)

    if metric == "pdr":
        miny = min(80, min(data)) if data else 80
        ax.set_ylim([miny, 108])
    else:
        ax.set_ylim(0, (max(data) if data and max(data) > 0 else 1) * 1.2)

    pdf.savefig(fig, bbox_inches='tight')
    pl.close(fig)

#######################################################
# avg/min/max/std of each metric over the nodes, as [(name, [avg, min, max, std])]

def metric_stats(results):
    rows = []
    for key, name in CSV_METRICS:
        data = [r[key] for r in results]
        if data:
            stats = [statistics.mean(data), min(data), max(data), statistics.pstdev(data)]
        else:
            stats = [0.0, 0.0, 0.0, 0.0]
        rows.append((name, stats))
    return rows

#######################################################
# First page of the PDF: the same table as in the metrics CSV

def plot_summary_table(results, title, pdf):
    rows = metric_stats(results)
    fig, ax = pl.subplots(figsize=(8, 0.45 * len(rows) + 1.5))
    ax.axis("off")
    table = ax.table(cellText=[[name] + ["{:.2f}".format(v) for v in stats] for name, stats in rows],
                     colLabels=["Metric", "Avg", "Min", "Max", "Std"],
                     colWidths=[0.36, 0.16, 0.16, 0.16, 0.16],
                     loc="center", cellLoc="center")
    table.auto_set_font_size(False)
    table.set_fontsize(9)
    table.scale(1, 1.5)
    ax.set_title(title, fontsize=10)
    pdf.savefig(fig, bbox_inches='tight')
    pl.close(fig)

#######################################################
# Write avg/min/max/std of each metric over the nodes, one row per metric

def write_metrics_csv(results, filename):
    with open(filename, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["metric", "avg", "min", "max", "std"])
        for name, stats in metric_stats(results):
            w.writerow([name] + ["{:.4f}".format(v) for v in stats])

#######################################################
# Run the application

def main():
    parser = argparse.ArgumentParser(description="Analyze a Cooja test log")
    parser.add_argument("log", nargs="?", default=LOG_FILE, help="log file to analyze")
    parser.add_argument("--pdf", default=OUTPUT_PDF, help="output PDF with the plots")
    parser.add_argument("--csv", default=OUTPUT_CSV, help="output CSV with the metrics")
    args = parser.parse_args()

    input_file = args.log

    if not os.access(input_file, os.R_OK):
        print('The input file "{}" does not exist'.format(input_file))
        exit(-1)

    with open(input_file, "r") as f:
        is_testbed = "Starting COOJA logger" not in f.read()

    results, ll_par, ll_queue_dropped, e2e_pdr = analyze_results(input_file, is_testbed)

    print("Link-layer PAR={:.2f} ({} packets queue dropped) End-to-end PDR={:.2f}".format(
        ll_par, ll_queue_dropped, e2e_pdr))

    with PdfPages(args.pdf) as pdf:
        plot_summary_table(results,
            "Summary over {} nodes\nLink-layer PAR={:.2f}%  End-to-end PDR={:.2f}%  Packets queue dropped={}".format(
                len(results), ll_par, e2e_pdr, ll_queue_dropped), pdf)
        plot(results, "pdr", "Packet Delivery Ratio, %", pdf)
        plot(results, "par", "Packet Acknowledgement Ratio, %", pdf)
        plot(results, "queue_drops", "Packets dropped from queue", pdf)
        plot(results, "rpl_switches", "RPL parent switches", pdf)
        plot(results, "duty_cycle", "Radio Duty Cycle, %", pdf)
        plot(results, "duty_cycle_joined", "Joined Radio Duty Cycle, %", pdf)
        plot(results, "charge", "Charge consumption, mC", pdf)

    print("Plots written to {}".format(args.pdf))

    write_metrics_csv(results, args.csv)
    print("Metrics written to {}".format(args.csv))

#######################################################

if __name__ == '__main__':
    main()
