# Mobility-Adaptive MSF for TSCH

This example runs the mobility-aware 6TiSCH Minimal Scheduling Function
(MA-MSF) over Contiki-NG's 6top/6P stack. MA-MSF adapts MSF's cell-management
policy using a node's mobility score as well as traffic and link statistics.
The scheduler implementation is in [scheduler/ma-msf/ma-msf.c](scheduler/ma-msf/ma-msf.c).

MA-MSF uses the slotframe layout from RFC 9033:

* Slotframe 0 is the minimal shared slotframe.
* Slotframe 1 provides autonomous cells.
* Slotframe 2 holds cells negotiated with 6P.

The application in [node.c](node.c) initializes MA-MSF, starts TSCH, and
sends periodic UDP packets toward the RPL root (node 1). MA-MSF handles
slotframe setup, 6P exchanges, and reactions to parent or mobility changes.

## Mobility input

In Cooja, [coojalogger.js](coojalogger.js) samples each mote's simulated
position and writes its speed and moving state to `cooja_imu_speed_mm_s` and
`cooja_imu_is_moving`, the globals consumed by MA-MSF. This ground-truth input
is enabled by default for Cooja. To use MA-MSF's RSSI/ETX and parent-switch
mobility estimate instead, build with
`DEFINES=MA_MSF_CONF_WITH_COOJA_IMU=0`.

The simulation uses [positions.dat](positions.dat) to move a mote and
[cooja.csc](cooja.csc) to configure the network and Cooja plugins. The logger
also writes `COOJA.testlog` in the format expected by
[run-analysis.py](run-analysis.py).

## How MA-MSF adapts

MA-MSF keeps the RFC 9033 MSF cell-management rules as its static baseline.
The core MSF header parameters (channel offset, traffic counters, thresholds,
6P candidate count and most resource limits) have the same defaults as the
plain MSF implementation. There are two notable differences:

* MA-MSF changes the idle-cell cleanup default from 30 minutes to 5 minutes.
* MA-MSF adds a mobility score and changes selected MSF policy values as that
  score rises. Plain MSF uses fixed values and does not read a mobility signal.

All `MA_MSF_CONF_*` values below can be overridden at compile time. The
mobility settings are declared in
[ma-msf.h](scheduler/ma-msf/ma-msf.h); the callback and 6top requirements are
in [scheduler-conf.h](scheduler/ma-msf/scheduler-conf.h).

### Mobility input and score

With `MA_MSF_CONF_WITH_COOJA_IMU=1`, the Cooja logger samples position once a
second, computes speed in mm/s, and writes it and a moving flag into the
`cooja_imu_speed_mm_s` and `cooja_imu_is_moving` globals. The speed becomes a
sample score from 0 to 100:

`sample = min(speed_mm_s * 100 / MA_MSF_CONF_SPEED_FULL_MM_S, 100)`

At the default full-score speed of 1500 mm/s, 750 mm/s contributes a sample
of 50 and 1500 mm/s or faster contributes 100, so a node moving steadily
faster than about 825 mm/s reaches the mobile-state threshold
(`SCORE_HIGH` = 55). If the moving flag is set but the speed-derived sample is
below 25 (for example, a small or stale reading), MA-MSF raises that sample to
25. The flag is not itself the mobile-state switch: the filtered score must
reach the high threshold. Because this floor is above `SCORE_LOW` (20), a node
leaves the mobile state only after it has stopped moving.

The filtered score is updated once per MA-MSF housekeeping tick (one second).
When the sample is rising, the new score is `(old_score + 3 * sample) / 4`;
when it is falling, the score decays by at least 1 and otherwise by one
eighth of the difference. This gives a fast response to movement and slower
return to static settings. A parent change also raises the score to at least
`MA_MSF_CONF_SWITCH_BOOST` (30 by default). This boost is below `SCORE_HIGH`,
so a parent switch alone moves the interpolated policy values part of the way
towards their mobile endpoints but does not put the node in the mobile state;
static nodes that change parent therefore keep a single negotiated Tx cell.

If IMU input is disabled, MA-MSF instead estimates a 0-100 sample from RSSI
changes and rising ETX on the preferred-parent link. Missing or stale link
statistics produce a zero sample.

### Adaptive policy values

Values in the “score 0” and “score 100” columns are the endpoints; MA-MSF
linearly interpolates the applicable values between them using the filtered
score. The score thresholds decide whether the node is in the mobile state
with hysteresis.

| Parameter (`MA_MSF_CONF_*`) | Score 0 / static | Score 100 / fast mobility | Effect |
|---|---:|---:|---|
| `SPEED_FULL_MM_S` | — | 1500 mm/s | Speed that maps to score 100; larger readings are capped at 100. |
| `SCORE_HIGH` / `SCORE_LOW` | enter at 55 | leave at 20 | Hysteresis for the mobile state; avoids rapid toggling. |
| `SWITCH_BOOST` | — | 30 minimum score | A preferred-parent change immediately raises the score to at least 30. Kept below `SCORE_HIGH`, so a parent switch alone does not enter the mobile state. |
| `MAX_NUM_CELLS` / `MOBILE_MAX_NUM_CELLS` | 100-cell usage window | 30-cell usage window | `NumCellsElapsed` decision window; shorter at high mobility. |
| `LIM_NUMCELLSUSED_HIGH` / `MOBILE_HIGH_PCT` | 75% | 40% | Usage above this window percentage requests a Tx/Rx cell. The absolute threshold is derived from the interpolated window and percentage. |
| `LIM_NUMCELLSUSED_LOW` / `MOBILE_LOW_PCT` | 25% | 0% | Usage below this percentage can request deletion; at score 100 the threshold is zero, so low use does not trigger cell release. |
| `EXTRA_CELLS` | — | 1 extra Tx cell | Target is one negotiated Tx cell when static and `1 + EXTRA_CELLS` (2 by default) when mobile. Each extra cell costs a 6P ADD per parent switch and an Rx cell the parent listens on every slotframe. |
| `QUEUE_HIGH` | — | 2 queued packets | While mobile, a parent queue at or above this size triggers another Tx-cell ADD, subject to the per-parent cap. |
| `WAIT_DURATION_MIN` / `MOBILE_WAIT_MIN` | 30 s | 3 s | Lower bound for randomized waitretry after a 6P transaction. |
| `WAIT_DURATION_MAX` / `MOBILE_WAIT_MAX` | 60 s | 8 s | Upper bound for randomized waitretry; shorter mobile waits allow quicker retry. |
| `6P_TIMEOUT` / `MOBILE_6P_TIMEOUT` | RFC 9033-derived timeout | 20 s | 6P transaction timeout; shortened as the score rises. |
| `SWITCH_TIMEOUT` / `MOBILE_SWITCH_TIMEOUT` | 300 s | 150 s | Maximum time to finish moving the schedule to a new preferred parent before clearing the old parent. |
| `CELL_IDLE_TIMEOUT` / `MOBILE_CELL_IDLE_TIMEOUT` | 300 s | 60 s for a child that signalled mobility | Cleanup delay for idle negotiated cells; a child’s 6P mobility metadata selects the shorter mobile timeout. |
| `SIGNAL_MOBILITY` | — | 1 (enabled) | Puts mobile state in bit 8 and the score in the low byte of 6P request metadata, so a parent can manage that child’s cells accordingly. |

`MOBILE_HIGH_PCT`, `MOBILE_LOW_PCT`, and `MOBILE_MAX_NUM_CELLS` affect the
regular MSF `NumCellsUsed`/`NumCellsElapsed` rule. Separately, while the mobile
state is active MA-MSF proactively keeps the target number of Tx cells and
adds cells when the parent queue reaches `QUEUE_HIGH`; this is why it can act
before the normal traffic window would request an ADD.

### RPL leaf mode while moving

With RPL Classic, a node that moves becomes an RPL leaf (`RPL_LEAF_WHEN_MOBILE`,
default on; set `MA_MSF_CONF_RPL_LEAF_WHEN_MOBILE` to 0 in [scheduler/ma-msf/scheduler-conf.h](scheduler/ma-msf/scheduler-conf.h) or build with `DEFINES=MA_MSF_CONF_RPL_LEAF_WHEN_MOBILE=0` to disable).
It stops sending DIOs and, when it becomes a leaf, sends one DIO with an
infinite rank so that its children pick another parent. It still sends EBs,
so other nodes can join through it. Leaf mode follows the node's own motion
(on at `SCORE_HIGH`, off at `SCORE_LOW`); the parent switch boost does not
trigger it, and the root never becomes a leaf. The RPL side is
`rpl_set_leaf_mode()` in `os/net/routing/rpl-classic`.

### Other scheduler defaults

These MA-MSF header values are fixed implementation limits or static MSF
settings rather than mobility-interpolated endpoints:

| Parameter | Default | Purpose |
|---|---:|---|
| `NUM_CH_OFFSET` | 16 | Number of channel offsets used for candidate-cell selection. |
| `MAX_NUMTX` | 256 | Per-cell transmission count limit before packet statistics are halved. |
| `HOUSEKEEPINGCOLLISION_PERIOD` | 60 s | Period for negotiated-cell collision checks. |
| `RELOCATE_PDRTHRES` | 50% | PDR threshold used by the MSF relocation rule; MA-MSF suppresses relocation while mobile. |
| `QUARANTINE_DURATION` | 300 s | Quarantine period after the relevant 6P failure handling. |
| `NUM_CANDIDATE_CELLS` | 5 | Candidate cells offered in a 6P request. |
| `MAX_NEGOTIATED_CELLS` | 24 | Total negotiated-cell state entries tracked by this implementation. |
| `MAX_CELLS_PER_PARENT` | 8 per direction | Per-parent Tx/Rx cell cap. |
| `MAX_AUTO_TX_CELLS` | 4 | Maximum on-demand AutoTxCells. |
| `SLOTFRAME_LENGTH` | 101 slots in this example | Shared length for the minimal, autonomous, and negotiated slotframes. |

Plain MSF has a 30-minute default `CELL_IDLE_TIMEOUT`; MA-MSF uses 5 minutes
for ordinary idle-cell cleanup, and the separate 60-second mobile-child
timeout when the peer has signalled mobility.

## Build and run

From this directory, build the Cooja mote:

```sh
make TARGET=cooja node.cooja SCHEDULER=ma-msf
```

Run the headless simulation through the Python runner:

```sh
python3 run-cooja.py --scheduler ma-msf
```

`ma-msf` is the Makefile's default scheduler, so the `--scheduler` option can
be omitted. The option is useful for explicitly selecting the scheduler in
Cooja's mote build command. For a GUI simulation, open `cooja.csc` in Cooja.
Node 1 is the RPL root; the moving node follows the configured position trace.

Analyze a completed run with:

```sh
python3 run-analysis.py
```

The runner writes logs under `log/`; the analysis script reports packet
delivery and link statistics and generates plots from `COOJA.testlog`.
