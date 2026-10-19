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
MODEL="ManhattanGrid"  # Options: RandomWaypoint, ManhattanGrid, GaussMarkov

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
echo ""

# Step 1: Generate BonnMotion scenario
echo "[1/4] Generating BonnMotion scenario..."
cd "$BONNMOTION_DIR"

if [ "$MODEL" == "RandomWaypoint" ]; then
    "$BM" -f "scenarios/$SCENARIO_NAME" RandomWaypoint \
        -n $NUM_NODES \
        -d $DURATION \
        -x $AREA_X \
        -y $AREA_Y \
        -h 10 \
        -l 2 \
        -p 5.0
elif [ "$MODEL" == "ManhattanGrid" ]; then
    "$BM" -f "scenarios/$SCENARIO_NAME" ManhattanGrid \
        -n $NUM_NODES \
        -d $DURATION \
        -x $AREA_X \
        -y $AREA_Y
elif [ "$MODEL" == "GaussMarkov" ]; then
    "$BM" -f "scenarios/$SCENARIO_NAME" GaussMarkov \
        -n $NUM_NODES \
        -d $DURATION \
        -x $AREA_X \
        -y $AREA_Y
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