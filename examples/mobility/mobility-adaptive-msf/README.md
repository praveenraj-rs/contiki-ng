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
