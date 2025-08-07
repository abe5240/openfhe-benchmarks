#!/bin/bash
# profile.sh - Profile an already-built OpenFHE executable
# Usage: ./profile.sh <executable>

set -euo pipefail

# Check argument
if [ $# -ne 1 ]; then
    echo "Usage: $0 <executable>"
    exit 1
fi

EXEC="$1"

if [ ! -f "$EXEC" ]; then
    echo "Error: Executable '$EXEC' not found"
    exit 1
fi

# Profiling tools location
PIN="/opt/intel/pin/pin"
PINTOOL="/opt/intel/pin/profiling-tools/pintool.so"

# Check prerequisites
if [ ! -f "$PIN" ]; then
    echo "Error: Intel Pin not found at /opt/intel/pin"
    exit 1
fi

if [ ! -f "$PINTOOL" ]; then
    echo "Error: Profiling tools not found at /opt/intel/pin/profiling-tools/"
    exit 1
fi

# Create directories
mkdir -p logs data

# Clean old logs
rm -f logs/*.out logs/*.log

# Run with Pin instrumentation
echo "=== Profiling $(basename $EXEC) ==="
echo "Running with Intel Pin instrumentation..."
sudo "$PIN" -logfile logs/pintool.log -t "$PINTOOL" -quiet -- "$EXEC"

# Clean up data directory
rm -rf data

# Parse and display results
echo
echo "=== Arithmetic Intensity Results ==="
echo "─────────────────────────────────────"

# Get integer operation count
OPS=0
if [ -f "logs/int_counts.out" ]; then
    OPS=$(awk '/TOTAL/ {print $3}' logs/int_counts.out)
    printf "Integer operations : %'d\n" $OPS
else
    echo "Integer operations : Not measured"
fi

# Get DRAM traffic
if [ -f "logs/dram_counts.out" ]; then
    source logs/dram_counts.out
    
    # Display DRAM in appropriate units
    if (( DRAM_TOTAL_BYTES > 1073741824 )); then
        DRAM_DISPLAY=$(printf "%.2f GiB" $(echo "scale=2; $DRAM_TOTAL_BYTES/1073741824" | bc))
    elif (( DRAM_TOTAL_BYTES > 1048576 )); then
        DRAM_DISPLAY=$(printf "%.2f MiB" $(echo "scale=2; $DRAM_TOTAL_BYTES/1048576" | bc))
    elif (( DRAM_TOTAL_BYTES > 1024 )); then
        DRAM_DISPLAY=$(printf "%.2f KiB" $(echo "scale=2; $DRAM_TOTAL_BYTES/1024" | bc))
    else
        DRAM_DISPLAY="$DRAM_TOTAL_BYTES bytes"
    fi
    printf "DRAM traffic       : %s\n" "$DRAM_DISPLAY"
    
    # Calculate arithmetic intensity
    if (( OPS > 0 && DRAM_TOTAL_BYTES > 0 )); then
        AI_BYTE=$(printf "%.9f" $(echo "scale=9; $OPS/$DRAM_TOTAL_BYTES" | bc))
        echo "─────────────────────────────────────"
        printf "AI (ops/byte)      : %s\n" "$AI_BYTE"
    fi
else
    echo "DRAM traffic       : Not measured"
fi

echo "─────────────────────────────────────"
echo "✓ Profiling complete"