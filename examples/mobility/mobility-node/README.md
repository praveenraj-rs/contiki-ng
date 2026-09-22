# Mobility Node Example

This is a simple Cooja mobility example. The simulated motes move according to
the BonnMotion-style trace in `positions.dat`. The JavaScript Cooja script
samples each mote's position, calculates its speed, injects the result into
the mote memory, and logs the values. The mote application also reads the
values from memory and prints them in its mote output.

## Files

- `node.c`: Contiki application that reads and logs the mobility values.
- `imu-mobility-logger.js`: Cooja script that samples position and injects
	`cooja_imu_is_moving` and `cooja_imu_speed_mm_s`.
- `positions.dat`: Mobility trace used by the Cooja Mobility plugin.
- `cooja.csc`: Cooja simulation configuration.

## Speed and moving state

The script samples every 1000 ms of simulated time. It calculates the 3D
distance between the current and previous positions and converts Cooja's
coordinates from meters to millimeters:

```text
speed_mm_s = distance_mm / elapsed_time_s
```

A mote is considered moving when its speed is greater than 5 mm/s. The values
are rounded and written to the simulated mote memory as:

```text
cooja_imu_is_moving   (0 or 1)
cooja_imu_speed_mm_s  (millimeters per second)
```

The first sample has no previous position, so its speed is 0 mm/s.

## Running

Build the mote firmware from this directory with:

```sh
make TARGET=cooja node.cooja
```

Open `cooja.csc` in Cooja and start the simulation. The JavaScript test
script is configured by the Cooja simulation and periodically logs records in
this format:

```text
<time_ms> <mote_id> imu moving=<0|1> speed_mm_s=<value>
```

The mote application logs the values it reads from memory in this format:

```text
IMU moving: <0|1>, speed: <value> mm/s
```
