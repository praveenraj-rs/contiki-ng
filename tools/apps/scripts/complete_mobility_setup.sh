#!/bin/bash

set -e

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# if [ "$#" -gt 1 ]; then
#     echo "Usage: $0 [start_node]" >&2
#     exit 2
# fi

# START_NODE=${1:-0}
# if [[ ! "$START_NODE" =~ ^[0-9]+$ ]]; then
#     echo "Error: start_node must be a non-negative integer" >&2
#     echo "Usage: $0 [start_node]" >&2
#     exit 2
# fi

echo "=========================================="
echo "BonnMotion to Cooja .dat Converter"
echo "=========================================="

# Configuration
NUM_NODES=5
START_NODE=3
DURATION=3600
AREA_X=100
AREA_Y=100
MODEL="RandomWaypoint"  # Options: RandomWaypoint, ManhattanGrid, GaussMarkov
SEED=123456     # BonnMotion random seed (fixed for reproducible traces; empty = random)
# RandomWaypoint only: movement dimension. 3 = classic 2D RWP (straight/diagonal
# legs to uniformly random waypoints). BonnMotion's default (4) moves along only
# one axis per leg, which gives L-shaped, Manhattan-like paths.
RW_DIM=3

# Speed / pause settings (applied to all models)
MIN_SPEED=0.5   # m/s
MAX_SPEED=1.5   # m/s (about walking speed)
MAX_PAUSE=5.0   # s   (RandomWaypoint, ManhattanGrid; GaussMarkov never pauses)
# ManhattanGrid only: chance of pausing at each update (0 disables pauses).
# Must be <= 0.8 (BonnMotion adds a fixed 0.2 speed-change probability).
MG_PAUSE_PROB=0.1
# ManhattanGrid draws speeds from a Gaussian around a mean (clipped at MIN_SPEED),
# so use the midpoint of MIN/MAX as the mean and keep the spread small.
MG_MEAN_SPEED=$(awk -v a="$MIN_SPEED" -v b="$MAX_SPEED" 'BEGIN { printf "%.3f", (a + b) / 2 }')
MG_SPEED_STDDEV=$(awk -v a="$MIN_SPEED" -v b="$MAX_SPEED" 'BEGIN { printf "%.3f", (b - a) / 4 }')

SCENARIO_NAME="mobility_${NUM_NODES}nodes"
OUTPUT_SUFFIX=""
if [[ "$START_NODE" =~ [1-9] ]]; then
    OUTPUT_SUFFIX="_start${START_NODE}"
fi
BONNMOTION_DIR="$SCRIPT_DIR/../bonnmotion-3.0.1"
BM="$BONNMOTION_DIR/bin/bm"
CONVERTER_DAT="$SCRIPT_DIR/bonnmotion_to_dat.py"
CONVERTER_JS="$SCRIPT_DIR/dat_to_cooja_js.py"
OUTPUT_DAT="$SCRIPT_DIR/../outputs/cooja_mobility_${NUM_NODES}nodes${OUTPUT_SUFFIX}.dat"
OUTPUT_JS="$SCRIPT_DIR/../outputs/cooja_mobility_${NUM_NODES}nodes${OUTPUT_SUFFIX}.js"

echo "Configuration:"
echo "  Nodes: $NUM_NODES"
echo "  First mobile Cooja mote index: $START_NODE"
echo "  Duration: $DURATION seconds"
echo "  Area: ${AREA_X}x${AREA_Y} meters"
echo "  Model: $MODEL"
echo "  Seed: ${SEED:-random}"
if [ "$MODEL" == "RandomWaypoint" ]; then
    echo "  Dimension: $RW_DIM"
    echo "  Speed: ${MIN_SPEED}-${MAX_SPEED} m/s"
    echo "  Max pause: ${MAX_PAUSE} s"
elif [ "$MODEL" == "ManhattanGrid" ]; then
    echo "  Speed: mean ${MG_MEAN_SPEED} m/s (stddev ${MG_SPEED_STDDEV}, min ${MIN_SPEED})"
    echo "  Max pause: ${MAX_PAUSE} s (pause probability ${MG_PAUSE_PROB})"
elif [ "$MODEL" == "GaussMarkov" ]; then
    echo "  Speed: ${MIN_SPEED}-${MAX_SPEED} m/s"
    echo "  Pause: none (continuous movement)"
fi
echo ""

# Step 1: Generate BonnMotion scenario
echo "[1/4] Generating BonnMotion scenario..."
cd "$BONNMOTION_DIR"

SEED_ARGS=()
if [ -n "$SEED" ]; then
    SEED_ARGS=(-R "$SEED")
fi

if [ "$MODEL" == "RandomWaypoint" ]; then
    "$BM" -f "scenarios/$SCENARIO_NAME" RandomWaypoint \
        "${SEED_ARGS[@]}" \
        -o $RW_DIM \
        -n $NUM_NODES \
        -d $DURATION \
        -x $AREA_X \
        -y $AREA_Y \
        -h $MAX_SPEED \
        -l $MIN_SPEED \
        -p $MAX_PAUSE
elif [ "$MODEL" == "ManhattanGrid" ]; then
    "$BM" -f "scenarios/$SCENARIO_NAME" ManhattanGrid \
        "${SEED_ARGS[@]}" \
        -n $NUM_NODES \
        -d $DURATION \
        -x $AREA_X \
        -y $AREA_Y \
        -e $MIN_SPEED \
        -m $MG_MEAN_SPEED \
        -s $MG_SPEED_STDDEV \
        -o $MAX_PAUSE \
        -p $MG_PAUSE_PROB
elif [ "$MODEL" == "GaussMarkov" ]; then
    # Note: for GaussMarkov, -m is MIN speed (for ManhattanGrid it is mean speed)
    "$BM" -f "scenarios/$SCENARIO_NAME" GaussMarkov \
        "${SEED_ARGS[@]}" \
        -n $NUM_NODES \
        -d $DURATION \
        -x $AREA_X \
        -y $AREA_Y \
        -h $MAX_SPEED \
        -m $MIN_SPEED
fi

# Step 2: Convert to NS-2 format
echo "[2/4] Converting to NS-2 format..."
"$BM" NSFile -f "scenarios/$SCENARIO_NAME"

# Step 3: Convert to .dat format
echo "[3/4] Converting to .dat format..."
python3 "$CONVERTER_DAT" \
    "$BONNMOTION_DIR/scenarios/${SCENARIO_NAME}.ns_movements" \
    "$OUTPUT_DAT" \
    "$NUM_NODES" \
    "$START_NODE"

# Step 4: Convert to JavaScript (as backup)
echo "[4/4] Converting to Cooja JavaScript..."
python3 "$CONVERTER_JS" \
    "$OUTPUT_DAT" \
    "$OUTPUT_JS"

echo ""
echo "=========================================="
echo "Conversion Complete!"
echo "=========================================="
echo "Output files:"
echo "  .dat file: $OUTPUT_DAT"
echo "  .js file:  $OUTPUT_JS"
echo ""
echo "To use in Cooja:"
echo "  1. Use mote index $START_NODE for the first mobile node; lower indices remain static"
echo "  2. Try: Tools → Mobility → Load → $OUTPUT_DAT"
echo "  3. Or use Script Editor to load: $OUTPUT_JS"
echo ""