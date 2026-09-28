# Scheduler Comparison Benchmarks: minimal vs. Orchestra (SB/RB) vs. sf-mobility

This folder runs the **same application and traffic pattern** under four
TSCH scheduling configurations, so results are directly comparable:

| `SCHEDULER=`     | What it builds                                            |
|------------------|-------------------------------------------------------------|
| `minimal`        | No 6P/Orchestra — TSCH's built-in 6TiSCH minimal schedule only |
| `orchestra-sb`   | Orchestra, **sender-based** unicast cell placement (`ORCHESTRA_CONF_UNICAST_SENDER_BASED=1`) |
| `orchestra-rb`   | Orchestra, **receiver-based** unicast cell placement (`ORCHESTRA_CONF_UNICAST_SENDER_BASED=0`, the Contiki-NG default) |
| `sf-mobility`    | `sf-mobility.c` — the mobility-adaptive 6P scheduling function developed in `../mobility-adaptive-msf` |

## Use-case scenario: industrial AGV / mobile robot

Motion: grid-constrained (`ManhattanGrid`-style) aisle movement, 0.5–2 m/s,
frequent stop-and-go at stations — see [positions/gen_agv_manhattan.py](positions/gen_agv_manhattan.py)
(generates [positions/agv_manhattan.dat](positions/agv_manhattan.dat)).
Traffic: periodic small telemetry + a bursty "pick/drop" event every
`AGV_EVENT_INTERVAL_SEC` (see `node.c`). Headline metrics: **latency** and
**parent-switch count**, plus the standard PDR/PAR/duty-cycle/ETX set.

Topology (`cooja-sims/cooja-agv-template.csc`): mote 1 = root (gateway),
motes 2/3 = static relays at the two aisle junctions, mote 4 = the AGV,
looping through the aisle via `positions/agv_manhattan.dat`.

## Bug fixed while wiring this up

`tsch_schedule_create_minimal()` (TSCH's own minimal-schedule installer)
wipes **all** slotframes whenever it runs — which happens automatically
when a node becomes TSCH coordinator, and again every time a node
resynchronizes via a schedule-less EB (e.g. after a mobility-induced
disassociation). Previously, this could silently delete sf-mobility's
negotiated slotframe (handle 1) on the root right after boot, breaking 6P
ADD/DELETE from then on. `sf-mobility.c` now recreates its slotframe
on demand (`get_slotframe()`) instead of assuming it always exists — fixed
in both this folder and `../mobility-adaptive-msf`.

## Workflow

1. Develop/tune the scheduler in `../mobility-adaptive-msf/sf-mobility.c`.
2. Pull the change in here:
   ```
   ./sync_sf_mobility.sh
   ```
3. Run all four schedulers and produce the comparison:
   ```
   python3 run_benchmarks.py
   ```
   This builds+runs each scheduler headless via Cooja/gradlew, saving:
   - `results/<scheduler>/run.csc` (the generated simulation)
   - `results/<scheduler>/COOJA.testlog` (raw log)
   - `results/comparison.csv` (one row per scheduler — the overall comparison)
   - `results/figures/plot_<metric>.pdf` (one bar chart per metric)

Run a subset / different seed:
```
python3 run_benchmarks.py --schedulers minimal sf-mobility --seed 2
```

Re-extract metrics or replot without re-running Cooja:
```
python3 extract_metrics.py results/sf-mobility/COOJA.testlog sf-mobility
python3 plot_comparison.py results/comparison.csv
```

## `comparison.csv` columns

`scheduler, pdr, par, link_loss_rate, duty_cycle, queue_loss, parent_changes, latency_ms, etx, charge_proxy_ticks`

- `pdr`/`par`/`duty_cycle`/`queue_loss` — same definitions as
  `examples/benchmarks/result-visualization/run-analysis.py`.
- `latency_ms` — end-to-end, computed by matching each node's
  "app generate packet" timestamp with the root's "app receive packet"
  timestamp for the same `seqnum` (no firmware payload change needed —
  both lines already carry the Cooja simulation timestamp).
- `etx` — from the new `etx to parent=` log line in `node.c`
  (`link_stats_from_lladdr()->etx`, fixed-point ×128).
- `charge_proxy_ticks` — sum of Radio Tx/Rx Energest ticks (relative energy
  proxy across schedulers within one run; see the real `run-analysis.py`
  for an actual mA·s charge estimate if you need absolute numbers).

## Extending to a full parameter sweep

This folder currently produces **one comparison point** (fixed slotframe
length, fixed traffic load, fixed AGV mobility). To sweep slotframe length
or traffic load (X-axes discussed earlier) and get multiple seeds per
point:
- `node.c`'s `AGV_EVENT_INTERVAL_SEC`/`APP_SEND_INTERVAL_SEC` and
  `project-conf.h`'s `TSCH_SCHEDULE_CONF_DEFAULT_LENGTH` are already
  override-friendly (`#ifndef` guarded) — pass them via
  `DEFINES=APP_SEND_INTERVAL_SEC=5,TSCH_SCHEDULE_CONF_DEFAULT_LENGTH=17` by
  filling in `__EXTRA_DEFINES__` in `run_benchmarks.py`'s `render_csc()`.
- Loop `run_one()` over a list of `(x_value, seed)` pairs instead of just
  `schedulers`, and append the swept value as an extra CSV column so
  `plot_comparison.py` can group by it (`x=slotframe_len`, `hue=scheduler`).

