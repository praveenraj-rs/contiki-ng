// IMU-based mobility injector for sf-mobility, + COOJA.testlog generator.
//
// Real position/velocity ground truth only exists on the Cooja/Java side of
// the simulation (exactly as a real IMU chip's reading is only known to the
// mote's own hardware). Instead of letting sf-mobility.c infer mobility
// indirectly from RSSI/ETX side effects, this script periodically:
//
//   1. Reads every mote's simulated Position (as moved by the "Mobility"
//      plugin from a BonnMotion .dat trace, or by any other position script).
//   2. Computes each mote's speed since the last sample.
//   3. Pokes that ground truth directly into the two globals declared in
//      sf-mobility.c (cooja_imu_is_moving, cooja_imu_speed_mm_s) using
//      Cooja's VarMemory, i.e. writing the mote's simulated RAM by C
//      variable name - the same mechanism a real IMU driver would use to
//      make hardware register values visible to application code.
//
// sf-mobility.c only trusts these two globals when built for the cooja
// target (SF_MOBILITY_CONF_WITH_COOJA_IMU); on real hardware the same code
// path would instead be fed by an actual accelerometer/gyroscope driver.
//
// It also mirrors examples/benchmarks/result-visualization/coojalogger.js:
// every mote log line is printed as "<time> <id> <msg>" so that
// run-analysis.py can be reused unmodified.
//
// NOTE: keep comments here as single-line comments starting with two
// slashes, not multi-line slash-star blocks: Cooja's ScriptParser strips
// slash-star comments with a regex that backtracks catastrophically on
// multi-line, asterisk-heavy comments (like the old version of this one),
// hanging/crashing the whole simulation before the script ever runs (only
// "Random seed: ..." ever gets written).

TIMEOUT(3600000); // 3600 seconds or 1 hour

var VarMemory = null;
try {
  VarMemory = Java.type("org.contikios.cooja.mote.memory.VarMemory");
} catch (e) {
  // Mobility injection is unavailable in this Cooja runtime.
}

// How often (ms of simulated time) the ground-truth speed is resampled.
var SAMPLE_PERIOD_MS = 1000;
// Below this speed, a mote is considered static (filters position jitter).
var MOVING_THRESHOLD_MM_S = 5;

var lastPos = {}; // mote ID -> {x, y, z, t (ms)}

function distanceMm(ax, ay, az, bx, by, bz) {
  var dx = (ax - bx) * 1000.0;
  var dy = (ay - by) * 1000.0;
  var dz = (az - bz) * 1000.0;
  return Math.sqrt(dx * dx + dy * dy + dz * dz);
}

function sampleAndInjectMobility() {
  var motes = sim.getMotes();
  var nowMs = sim.getSimulationTimeMillis();

  for (var i = 0; i < motes.length; i++) {
    var m = motes[i];
    var mid = m.getID();
    var pos = m.getInterfaces().getPosition();
    var x = pos.getXCoordinate();
    var y = pos.getYCoordinate();
    var z = pos.getZCoordinate();

    var speedMmS = 0;
    var prev = lastPos[mid];
    if (prev) {
      var dtS = (nowMs - prev.t) / 1000.0;
      if (dtS > 0) {
        speedMmS = distanceMm(x, y, z, prev.x, prev.y, prev.z) / dtS;
      }
    }
    lastPos[mid] = { x: x, y: y, z: z, t: nowMs };

    var moving = speedMmS > MOVING_THRESHOLD_MM_S ? 1 : 0;
    var speedInt = Math.min(65535, Math.round(speedMmS));

    if (VarMemory !== null) {
      try {
        var mem = new VarMemory(m.getMemory());
        mem.setInt8ValueOf("cooja_imu_is_moving", moving);
        mem.setInt16ValueOf("cooja_imu_speed_mm_s", speedInt);
      } catch (e) {
        // Firmware without sf-mobility (e.g. a plain root image), or symbol
        // stripped: nothing to inject for this mote, skip silently.
      }
    }
  }
}

timeout_function = function () {
    log.log("Script timed out.\n");
    log.testOK();
}

log.log("Starting COOJA logger\n");

/* Kick off the periodic ground-truth sampling loop. */
GENERATE_MSG(SAMPLE_PERIOD_MS, "sf-mobility-imu-poll");

while (true) {
    // Any uncaught error here (Java reflection included) would otherwise
    // propagate out of the script and make Cooja call stopSimulation(),
    // truncating COOJA.testlog after just a couple of lines - so nothing
    // inside this loop is allowed to escape uncaught.
    try {
        if (msg == "sf-mobility-imu-poll") {
            sampleAndInjectMobility();
            GENERATE_MSG(SAMPLE_PERIOD_MS, "sf-mobility-imu-poll");
        } else if (msg) {
            log.log(time + " " + id + " " + msg + "\n");
        }
    } catch (e) {
        log.log("imu-mobility-logger: ignored error: " + e + "\n");
    }
    YIELD();
}

