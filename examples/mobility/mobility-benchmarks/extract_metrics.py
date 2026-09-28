#!/usr/bin/env python3
"""Extend examples/benchmarks/result-visualization/run-analysis.py's log
parsing with latency (matched from existing send/receive timestamps, no
firmware payload change needed) and ETX (from the "etx to parent=" line
added in node.c), and reduce a whole run down to ONE summary row instead of
per-node plots - the row format expected by run_benchmarks.py's
comparison.csv.

Usage: python3 extract_metrics.py <COOJA.testlog> <scheduler_label>
Prints one CSV line (no header) to stdout:
  scheduler,pdr,par,link_loss_rate,duty_cycle,queue_loss,parent_changes,latency_ms,etx,charge
"""
import sys
import os

COORDINATOR_ID = 1


class NodeStats:
    def __init__(self, node_id):
        self.id = node_id
        self.tsch_time_source = None
        self.max_seqnum_sent = 0
        self.seqnums_received_on_root = set()
        self.send_ts = {}          # seqnum -> timestamp (ms) this node sent at
        self.latencies_ms = []     # filled in on the receiving (root) side
        self.parent_packets_tx = 0
        self.parent_packets_ack = 0
        self.parent_packets_queue_dropped = 0
        self.rpl_parent_changes = 0
        self.etx_samples = []
        self.energest_radio_tx = 0
        self.energest_radio_rx = 0
        self.energest_total = 0


def addr_to_id(addr):
    return int(addr.split(":")[-1], 16)


def parse(filename):
    nodes = {}

    def get(node_id):
        if node_id not in nodes:
            nodes[node_id] = NodeStats(node_id)
        return nodes[node_id]

    with open(filename, "r") as f:
        for line in f:
            line = line.strip()
            fields = line.split()
            try:
                ts = int(fields[0]) // 1000  # us -> ms
                node = int(fields[1])
            except (ValueError, IndexError):
                continue

            n = get(node)

            if "update time source" in line:
                n.tsch_time_source = line.split(" -> ")[1] if " -> " in line else None
                continue

            if "rpl_set_preferred_parent" in line or " parent switch: " in line:
                n.rpl_parent_changes += 1
                continue

            if "app generate packet" in line:
                seqnum = int(fields[8].split("=")[1])
                n.max_seqnum_sent = max(n.max_seqnum_sent, seqnum)
                n.send_ts[seqnum] = ts
                continue

            if "app receive packet" in line:
                seqnum = int(fields[8].split("=")[1])
                fromaddr = fields[9].split("=")[1]
                from_node = addr_to_id(fromaddr)
                sender = get(from_node)
                sender.seqnums_received_on_root.add(seqnum)
                send_ts = sender.send_ts.get(seqnum)
                if send_ts is not None:
                    sender.latencies_ms.append(ts - send_ts)
                continue

            if "etx to parent" in line:
                n.etx_samples.append(int(fields[7].split("=")[1]))
                continue

            if "num packets" in line:
                tx = int(fields[7].split("=")[1])
                ack = int(fields[8].split("=")[1])
                queue_drops = int(fields[10].split("=")[1])
                to_addr = fields[11].split("=")[1]
                if n.tsch_time_source == to_addr:
                    n.parent_packets_tx += tx
                    n.parent_packets_ack += ack
                    n.parent_packets_queue_dropped += queue_drops
                continue

            if "INFO: Energest" in line:
                if "Total time" in line:
                    n.energest_total += int(fields[8])
                elif "Radio Tx" in line:
                    n.energest_radio_tx += int(fields[8][:-1])
                elif "Radio Rx" in line:
                    n.energest_radio_rx += int(fields[8][:-1])
                continue

    return nodes


def average(values):
    return sum(values) / len(values) if values else 0.0


def summarize(nodes):
    non_root = [n for n in nodes.values() if n.id != COORDINATOR_ID]
    if not non_root:
        return None

    pdrs, pars, duty_cycles, latencies, etxs = [], [], [], [], []
    total_queue_drops = 0
    total_parent_changes = 0
    total_charge_proxy = 0.0

    for n in non_root:
        if n.max_seqnum_sent:
            pdrs.append(100.0 * len(n.seqnums_received_on_root) / n.max_seqnum_sent)
        if n.parent_packets_tx:
            pars.append(100.0 * n.parent_packets_ack / n.parent_packets_tx)
        if n.energest_total:
            duty_cycles.append(100.0 * (n.energest_radio_tx + n.energest_radio_rx) / n.energest_total)
            total_charge_proxy += n.energest_radio_tx + n.energest_radio_rx
        latencies.extend(n.latencies_ms)
        etxs.extend(n.etx_samples)
        total_queue_drops += n.parent_packets_queue_dropped
        total_parent_changes += n.rpl_parent_changes

    return {
        "pdr": average(pdrs),
        "par": average(pars),
        "link_loss_rate": 100.0 - average(pars) if pars else 0.0,
        "duty_cycle": average(duty_cycles),
        "queue_loss": total_queue_drops,
        "parent_changes": total_parent_changes,
        "latency_ms": average(latencies),
        "etx": average(etxs),
        "charge_proxy_ticks": total_charge_proxy,
    }


def main():
    if len(sys.argv) < 3:
        sys.stderr.write("Usage: extract_metrics.py <COOJA.testlog> <scheduler_label>\n")
        sys.exit(1)

    logfile, label = sys.argv[1], sys.argv[2]
    if not os.access(logfile, os.R_OK):
        sys.stderr.write("Cannot read {}\n".format(logfile))
        sys.exit(1)

    metrics = summarize(parse(logfile))
    if metrics is None:
        sys.stderr.write("No non-root node data found in {}\n".format(logfile))
        sys.exit(1)

    print(",".join([
        label,
        "{:.2f}".format(metrics["pdr"]),
        "{:.2f}".format(metrics["par"]),
        "{:.2f}".format(metrics["link_loss_rate"]),
        "{:.2f}".format(metrics["duty_cycle"]),
        str(metrics["queue_loss"]),
        str(metrics["parent_changes"]),
        "{:.1f}".format(metrics["latency_ms"]),
        "{:.1f}".format(metrics["etx"]),
        "{:.0f}".format(metrics["charge_proxy_ticks"]),
    ]))


if __name__ == "__main__":
    main()
