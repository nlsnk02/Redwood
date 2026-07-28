#!/bin/bash
# YCSB Concurrent Benchmark: A/B/C/D/F workloads running simultaneously
# Config: 20k records, 3 repetitions, 4 threads, DIO mode
set -euo pipefail

BENCH="/home/u332/ycsb/adapters/cbtree/ycsb_cbtree_bench"
RESULTS_DIR="/home/u332/ycsb/results/concurrent_$(date +%Y%m%d_%H%M%S)"
RECORDS=20000
THREADS=4
REPEAT=3

WORKLOADS=("A" "B" "C" "D" "F")
DB_BASE="/tmp/ycsb_cbtree_concurrent"

mkdir -p "$RESULTS_DIR"

echo "=============================================="
echo " YCSB Concurrent Benchmark"
echo " Workloads: ${WORKLOADS[*]}"
echo " Records:   $RECORDS"
echo " Threads:   $THREADS (per workload)"
echo " Repeat:    $REPEAT"
echo " Mode:      DIO"
echo " Total concurrent processes: ${#WORKLOADS[@]}"
echo " Total threads: $((${#WORKLOADS[@]} * THREADS))"
echo " Results:   $RESULTS_DIR"
echo "=============================================="
echo ""

START_TIME=$(date +%s.%N)

# bench_main.cc writes JSON results to ./results/ relative to CWD.
# Run from the cbtree adapter directory so results land in a known place.
BENCH_DIR="/home/u332/ycsb/adapters/cbtree"

# Launch all workloads in parallel
PIDS=()
for wl in "${WORKLOADS[@]}"; do
    DB_PATH="${DB_BASE}_${wl}"
    LOG_FILE="${RESULTS_DIR}/${wl}.log"

    echo "[$(date +%H:%M:%S)] Launching workload ${wl} (db=${DB_PATH})..."

    (
        cd "$BENCH_DIR"
        mkdir -p ./results
        "$BENCH" \
            -t "$THREADS" \
            -r "$REPEAT" \
            --records "$RECORDS" \
            -p "$DB_PATH" \
            "$wl" \
            > "$LOG_FILE" 2>&1
    ) &

    PIDS+=($!)
done

echo ""
echo "All workloads launched. Waiting for completion..."
echo ""

# Wait for all and collect exit codes
FAILED=0
for i in "${!WORKLOADS[@]}"; do
    wl="${WORKLOADS[$i]}"
    pid="${PIDS[$i]}"

    if wait "$pid"; then
        echo "[$(date +%H:%M:%S)] Workload ${wl} (pid=${pid}) — OK"
    else
        echo "[$(date +%H:%M:%S)] Workload ${wl} (pid=${pid}) — FAILED (exit=$?)"
        FAILED=1
    fi
done

END_TIME=$(date +%s.%N)
ELAPSED=$(echo "$END_TIME - $START_TIME" | bc)

echo ""
echo "=============================================="
echo " All workloads completed in ${ELAPSED}s"
echo "=============================================="
echo ""

# Print per-workload summary (extract throughput from logs)
echo "--- Throughput Summary ---"
printf "%-6s | %15s | %15s | %15s | %15s\n" "WL" "Run1(ops/s)" "Run2(ops/s)" "Run3(ops/s)" "Median(ops/s)"
printf "%-6s-|-%15s-|-%15s-|-%15s-|-%15s\n" "------" "---------------" "---------------" "---------------" "---------------"

for wl in "${WORKLOADS[@]}"; do
    LOG_FILE="${RESULTS_DIR}/${wl}.log"
    if [ -f "$LOG_FILE" ]; then
        # Extract "Run: X ops/s" lines from log
        TPUTS=($(grep -oP 'Run:\s+\K[0-9.]+' "$LOG_FILE" | head -3))
        TPUT_FLOATS=()
        for t in "${TPUTS[@]}"; do
            TPUT_FLOATS+=($(printf "%.0f" "$t"))
        done

        # Pad to 3 values
        while [ ${#TPUT_FLOATS[@]} -lt 3 ]; do
            TPUT_FLOATS+=("-")
        done

        # Calculate median
        MEDIAN="-"
        if [ ${#TPUTS[@]} -ge 3 ]; then
            SORTED=($(printf '%s\n' "${TPUT_FLOATS[@]}" | sort -n))
            MEDIAN="${SORTED[1]}"
        elif [ ${#TPUTS[@]} -gt 0 ]; then
            MEDIAN="${TPUT_FLOATS[0]}"
        fi

        printf "%-6s | %15s | %15s | %15s | %15s\n" \
            "$wl" "${TPUT_FLOATS[0]:--}" "${TPUT_FLOATS[1]:--}" "${TPUT_FLOATS[2]:--}" "$MEDIAN"
    else
        printf "%-6s | %15s | %15s | %15s | %15s\n" "$wl" "N/A" "N/A" "N/A" "N/A"
    fi
done

echo ""
echo "--- Aggregate Stats (from each workload's final run) ---"
for wl in "${WORKLOADS[@]}"; do
    LOG_FILE="${RESULTS_DIR}/${wl}.log"
    if [ -f "$LOG_FILE" ]; then
        echo "=== ${wl} ==="
        # Extract the Aggregate section
        sed -n '/=== Aggregate/,/^$/p' "$LOG_FILE" 2>/dev/null || true
        # Extract engine stats
        echo "--- Engine Stats ---"
        sed -n '/--- Engine Stats/,/^$/p' "$LOG_FILE" 2>/dev/null || true
        echo ""
    fi
done

echo ""
echo "Full logs: $RESULTS_DIR/"
echo "Done."

# Cleanup temp DB files
rm -f ${DB_BASE}_* 2>/dev/null || true
