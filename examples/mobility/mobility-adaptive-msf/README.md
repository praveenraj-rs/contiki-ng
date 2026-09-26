# Mobility-Adaptive MSF-like TSCH Scheduling

This example extends Contiki-NG's 6P/6top infrastructure (the same one used
by `examples/6tisch/sixtop`) with a scheduling function, **sf-mobility**,
that adapts the TSCH schedule not only to *traffic load* (like MSF /
Orchestra) but also to an estimated **mobility/link-instability score** of
the node's RPL preferred parent.

## Why plain MSF/Orchestra fall short under mobility

* **Minimal schedule**: one shared slot for everybody — fair but low
  throughput, and it doesn't get worse *specifically* for mobile nodes.
* **Orchestra**: derives cells deterministically from addresses (e.g.
  `orchestra-rule-unicast-per-neighbor-rpl-storing`). It reacts to a parent
  change (new hash ⇒ new cell) but has no concept of "this link is about to
  fail" — it only reacts *after* the parent has actually switched.
* **MSF (IETF, draft-ietf-6tisch-msf)**: adds/removes negotiated cells based
  on `NumCellsUsed`/`NumCellsElapsed` counters — purely reactive to
  already-observed traffic, so a fast-degrading mobile link still sees
  queue growth and frame loss before MSF's counters catch up and a 6P ADD
  is negotiated.

## What sf-mobility adds

A periodic housekeeping task (`sf_mobility_housekeeping_process`, in
[sf-mobility.c](sf-mobility.c)) evaluates, every
`SF_MOBILITY_HOUSEKEEPING_PERIOD`:

1. **Traffic pressure** — `tsch_queue_nbr_packet_count()` for the parent.
2. **Mobility score (0-100)** — see below.
3. **Adaptive rule** (a mobility-aware variant of MSF's add/delete rule):
   - Add one Tx cell (6P ADD) *proactively* if queue pressure is high **or**
     mobility score is high — i.e. before the queue actually overflows.
   - Remove a spare Tx cell (6P DELETE) only after several consecutive
     rounds of an idle queue **and** a low mobility score, to save energy
     without causing schedule oscillation ("cell churn").
   - On a parent switch, cells with the old parent age out naturally (no
     more traffic scheduled there) while a cell with the new parent is
     negotiated immediately.

This keeps the same 6P transaction machinery (`sixtop`, `sixp`) and cell
negotiation pattern (random free-slot proposal, single-cell ADD/DELETE) as
`sf-simple`, so it is wire-compatible with any other Contiki-NG 6P
responder.

## Mobility score: ground truth from Cooja instead of RSSI/ETX guessing

Real position/velocity is only known to the simulator, exactly like a real
IMU chip's reading is only known to the mote's own hardware — no protocol
exposes it to peers. There are therefore two ways to feed a mobility signal
into `sf-mobility.c`:

1. **Infer it** from side effects visible to the radio stack: RSSI
   volatility and ETX trend of the parent link. This is the portable
   fallback (works on real hardware, with no extra dependency), implemented
   in the `#else` branch of `update_mobility_score()`.
2. **Read it directly**, as ground truth, when running in Cooja — the
   same way a real IMU driver would populate hardware registers for the
   application to read.

This example does (2) by default when built for the `cooja` target
(`SF_MOBILITY_CONF_WITH_COOJA_IMU`, auto-enabled via `CONTIKI_TARGET_COOJA`).
Two plain globals in `sf-mobility.c` act as the "IMU registers":

```c
uint8_t  cooja_imu_is_moving;    /* 0/1, ground truth */
uint16_t cooja_imu_speed_mm_s;   /* approx. speed in mm/s, ground truth */
```

They are deliberately non-`static` so their symbols show up in the firmware
ELF and can be looked up by name from the Cooja/Java side:

```
$ nm build/cooja/node.cooja | grep cooja_imu
0000000000072dc0 B cooja_imu_is_moving
0000000000072dc2 B cooja_imu_speed_mm_s
```

[imu-mobility-logger.js](imu-mobility-logger.js) is the Cooja test script
that plays the role of the "IMU driver": every second of simulated time it
reads each mote's `Position` (as moved by the Mobility plugin/BonnMotion
`.dat` trace, or any other position script), computes the speed since the
last sample, and writes it directly into that mote's memory using Cooja's
`VarMemory` API (`mote.getMemory()` + variable name — the standard
Cooja mechanism for poking a mote's C globals from a test script, also used
internally by things like button/sensor test scripts):

```js
var mem = new VarMemory(m.getMemory());
mem.setInt8ValueOf("cooja_imu_is_moving", moving);
mem.setInt16ValueOf("cooja_imu_speed_mm_s", speedInt);
```

`update_mobility_score()` then just reads these two globals directly (no
RSSI/ETX inference at all) and folds them into the same exponential moving
average used by the RSSI/ETX fallback, so the rest of the adaptive-cell
logic is unchanged either way.

Why this is better than deriving it: RSSI/ETX-based inference is a lagging,
noisy proxy — a link can degrade for reasons other than motion (interference,
shadowing, congestion), and a genuinely mobile node can briefly keep a good
RSSI right up to the moment it goes out of range. Ground-truth injection
removes that guesswork in simulation, letting you (a) evaluate the adaptive
add/delete *policy* on its own merits, independent of estimator error, and
(b) later swap in an inference-only implementation for real deployments and
compare its accuracy against this same policy driven by ground truth.

On real hardware, replace the two globals with an actual IMU/accelerometer
driver (e.g. compute `speed_mm_s` from integrated acceleration, and
`is_moving` from a motion-detection interrupt) — the rest of
`sf-mobility.c` does not need to change.

## Schedule layout

* Slotframe handle `0` ("minimal"): one shared RX/TX/ADVERTISING cell at
  `(0,0)` for EB and broadcast — equivalent to the 6TiSCH minimal schedule.
* Slotframe handle `1` (`SF_MOBILITY_SLOTFRAME_HANDLE`): starts empty,
  entirely populated/depopulated at runtime by sf-mobility via 6P.

A production MSF stack typically keeps a *third* slotframe for autonomous
(hash-derived) cells used during the join process; it was intentionally
left out here to keep the demo focused on the mobility-adaptive negotiated
part. It can be added the same way Orchestra's
`orchestra-rule-unicast-per-neighbor-rpl-ns.c` does.

## Expected effect vs. Orchestra / minimal / plain MSF

* **Throughput / reliability**: proactively reserving a spare cell when the
  mobility score rises should reduce queue drops and MAC-layer retries
  during/after motion, compared to schemes that only add cells after
  traffic has already backed up.
* **Latency after parent switch**: a new Tx cell is requested in the very
  next housekeeping round after the RPL parent changes, rather than waiting
  for MSF-style usage counters to accumulate.
* **Energy**: for static deployments, the mobility score stays near 0, so
  the schedule converges to `SF_MOBILITY_MIN_CELLS` — no worse than MSF.
  The energy cost of this approach shows up only on mobile nodes/links,
  where a few extra idle cells are kept scheduled in exchange for lower
  loss and faster recovery.

## Files

* `sf-mobility.h` / `sf-mobility.c` — the scheduling function.
* `node.c` — app: sets up the two slotframes, registers sf-mobility, sends
  periodic UDP traffic to the DAG root (node 1), and logs in the same
  format as `examples/benchmarks/result-visualization` (see Benchmarking).
* `project-conf.h` — TSCH/6top/RPL/logging configuration for this demo.
* `imu-mobility-logger.js` — Cooja script: ground-truth IMU injector +
  `COOJA.testlog` generator (see above and Benchmarking).
* `positions.dat` — example BonnMotion-style trace that walks one mote back
  and forth between two static relays, to exercise repeated parent switches.
* `cooja.csc` — 4-mote simulation (root + 2 static relays + 1 mobile mote)
  wiring together `node.cooja`, the Mobility plugin (`positions.dat`) and
  `imu-mobility-logger.js`.
* `run-cooja.py` / `run-analysis.py` — copied unmodified from
  `examples/benchmarks/result-visualization` (see Benchmarking).

## Running in Cooja (GUI)

```
make TARGET=cooja node.cooja
```
then open `cooja.csc` in Cooja. Node 1 is the DAG root; node 4 is mobile
and will alternate its RPL parent between nodes 2 and 3 as it walks back and
forth (see `positions.dat`). Watch the `sf-mobility:` log lines (raise
`LOG_LEVEL` in `sf-mobility.c` if needed, or use the print-based `PRINTF`
statements already in the driver) to see cell adds/removes correlate with
parent switches and the injected mobility score.

## Benchmarking

This example reuses the benchmarking pipeline from
`examples/benchmarks/result-visualization` as-is:

```
cd examples/6tisch/mobility-adaptive-msf
python3 run-cooja.py cooja.csc        # runs the simulation headless via Cooja/gradlew
python3 run-analysis.py               # parses COOJA.testlog, prints PAR/PDR, plots metrics
```

`run-analysis.py` produces, per node: end-to-end PDR, link-layer PAR
(parent-link ACK ratio), number of RPL parent switches, radio duty cycle,
and estimated charge consumption (`plot_*.pdf`), plus aggregate
link-layer PAR / end-to-end PDR / queue-drop totals on stdout — exactly the
metrics you need to argue "more efficient/reliable than Orchestra/minimal".

Recommended comparison to quantify the value of the mobility term:

1. **Baseline A — minimal/`sf-simple`**: run `examples/6tisch/sixtop` (or
   this project with 6P disabled) on the same `cooja.csc` topology/trace.
2. **Baseline B — traffic-only adaptive**: build this example with
   `SF_MOBILITY_CONF_WITH_COOJA_IMU=0` (add
   `CFLAGS += -DSF_MOBILITY_CONF_WITH_COOJA_IMU=0` or edit
   `project-conf.h`) so `sf-mobility` falls back to the RSSI/ETX heuristic
   only — this isolates the effect of the *proactive* rule from the
   *ground-truth* mobility input.
3. **This example (default)** — Cooja-IMU ground truth mobility.

Run all three against the identical `positions.dat` trace (same random
seed in `cooja.csc`) and compare the `run-analysis.py` output: expect (2)
and (3) to show fewer `queue_drops`/higher PAR than (1) during the periods
around a parent switch, and (3) to react at least one housekeeping period
faster than (2) since it doesn't need to wait for RSSI/ETX to visibly
degrade first. For a mobility-speed sweep, scale the `x`/`y` deltas in
`positions.dat` (or regenerate it with a BonnMotion model) to compare
low/medium/high mobility scenarios.

