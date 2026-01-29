#!/bin/bash
# Benchmark script for comparing ART vs QuART in PostgreSQL VACUUM
# Based on: https://pganalyze.com/blog/5mins-postgres-17-faster-vacuum-adaptive-radix-trees

set -e

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

WORKSPACE_DIR="/Users/cangokmen/Desktop/QuART-VACUUM"
INSTALL_DIR="$WORKSPACE_DIR/pg_install"
DATA_DIR="$WORKSPACE_DIR/pg_data"
RESULTS_DIR="$WORKSPACE_DIR/benchmark_results"

# Default parameters
NUM_ROWS=10000000
MAINTENANCE_MEM="64MB"
NUM_RUNS=3

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -N|--num-rows)
            NUM_ROWS="$2"
            shift 2
            ;;
        -m|--memory)
            MAINTENANCE_MEM="$2"
            shift 2
            ;;
        -r|--runs)
            NUM_RUNS="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  -N, --num-rows NUM     Number of rows to insert (default: 10000000)"
            echo "  -m, --memory SIZE      maintenance_work_mem setting (default: 64MB)"
            echo "  -r, --runs NUM         Number of runs per configuration (default: 3)"
            echo "  -h, --help             Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}PostgreSQL VACUUM Benchmark${NC}"
echo -e "${GREEN}ART vs QuART Comparison${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "Parameters:"
echo "  Rows: $NUM_ROWS"
echo "  Memory: $MAINTENANCE_MEM"
echo "  Runs per config: $NUM_RUNS"
echo ""

# Create results directory
mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
RESULT_FILE="$RESULTS_DIR/benchmark_${NUM_ROWS}_${TIMESTAMP}.csv"

# Write CSV header
echo "config,run,num_rows,maintenance_work_mem,index_vacuum_count,dead_tuple_bytes,elapsed_seconds,cpu_user,cpu_system" > "$RESULT_FILE"

# Function to rebuild PostgreSQL with specific configuration
rebuild_postgres() {
    local use_quart=$1
    local config_name=$2
    
    echo -e "${BLUE}Rebuilding PostgreSQL ($config_name)...${NC}"
    
    cd "$WORKSPACE_DIR"
    
    # Clean previous build
    make clean > /dev/null 2>&1 || true
    
    # Configure based on USE_QUART_TIDSTORE flag
    if [ "$use_quart" = "1" ]; then
        CFLAGS="-O2 -g -DUSE_QUART_TIDSTORE=1" ./configure \
            --prefix="$INSTALL_DIR" \
            --enable-debug \
            --enable-cassert \
            --quiet > /dev/null 2>&1
    else
        CFLAGS="-O2 -g -DUSE_QUART_TIDSTORE=0" ./configure \
            --prefix="$INSTALL_DIR" \
            --enable-debug \
            --enable-cassert \
            --quiet > /dev/null 2>&1
    fi
    
    # Build and install
    make -j$(sysctl -n hw.ncpu) > /dev/null 2>&1
    make install > /dev/null 2>&1
    
    echo -e "${GREEN}✓ Build complete${NC}"
}

# Function to initialize database
init_database() {
    echo -e "${BLUE}Initializing database...${NC}"
    
    # Stop if running
    if [ -f "$DATA_DIR/postmaster.pid" ]; then
        "$INSTALL_DIR/bin/pg_ctl" -D "$DATA_DIR" stop -m fast -w > /dev/null 2>&1 || true
        sleep 2
    fi
    
    # Remove old data
    if [ -d "$DATA_DIR" ]; then
        rm -rf "$DATA_DIR"
    fi
    
    # Initialize new cluster
    "$INSTALL_DIR/bin/initdb" -D "$DATA_DIR" > /dev/null 2>&1
    
    # Configure for testing
    cat >> "$DATA_DIR/postgresql.conf" <<EOF

# Benchmark configuration
log_autovacuum_min_duration = 0
log_min_messages = warning
logging_collector = on
log_directory = 'log'
log_filename = 'postgresql-%Y-%m-%d_%H%M%S.log'
shared_buffers = 256MB
max_wal_size = 2GB
checkpoint_timeout = 30min
autovacuum = off
EOF
    
    # Start PostgreSQL
    "$INSTALL_DIR/bin/pg_ctl" -D "$DATA_DIR" -l "$DATA_DIR/logfile" start -w > /dev/null 2>&1
    sleep 2
    
    echo -e "${GREEN}✓ Database initialized and started${NC}"
}

# Function to run single benchmark
run_benchmark() {
    local config_name=$1
    local run_num=$2
    
    echo -e "${YELLOW}  Run $run_num/$NUM_RUNS...${NC}"
    
    # Create test SQL
    local TEST_SQL="$RESULTS_DIR/test_${config_name}_${run_num}.sql"
    
    cat > "$TEST_SQL" <<EOF
-- Benchmark test for $config_name (run $run_num)
SET client_min_messages = 'WARNING';
SET maintenance_work_mem = '$MAINTENANCE_MEM';

-- Drop and recreate table
DROP TABLE IF EXISTS vacuum_benchmark CASCADE;

-- Create table
CREATE TABLE vacuum_benchmark AS 
SELECT id, md5(id::text) as data
FROM generate_series(1, $NUM_ROWS) id;

-- Create index
CREATE INDEX vacuum_benchmark_idx ON vacuum_benchmark(id);

-- Update all rows to create dead tuples
UPDATE vacuum_benchmark SET data = md5((id + 1)::text);

-- Run VACUUM with timing
\timing on
VACUUM (VERBOSE, INDEX_CLEANUP ON) vacuum_benchmark;
\timing off

-- Get final statistics
SELECT 
    'STATS:' || n_live_tup || ',' || n_dead_tup || ',' || vacuum_count
FROM pg_stat_all_tables 
WHERE relname = 'vacuum_benchmark';
EOF
    
    # Run test and capture output
    local OUTPUT=$("$INSTALL_DIR/bin/psql" -d postgres -f "$TEST_SQL" 2>&1)
    
    # Parse VACUUM VERBOSE output for metrics
    local INDEX_VACUUM_COUNT=$(echo "$OUTPUT" | grep -oE "index scans: [0-9]+" | grep -oE "[0-9]+" | head -1)
    local DEAD_TUPLE_BYTES=$(echo "$OUTPUT" | grep -oE "[0-9]+ dead row versions" | grep -oE "[0-9]+" | head -1)
    local ELAPSED=$(echo "$OUTPUT" | grep "Time:" | tail -1 | grep -oE "[0-9]+\.[0-9]+" | head -1)
    
    # Convert elapsed from ms to seconds if needed
    if [ -n "$ELAPSED" ]; then
        ELAPSED=$(echo "scale=3; $ELAPSED / 1000" | bc)
    fi
    
    # Get CPU times from VACUUM VERBOSE
    local CPU_USER=$(echo "$OUTPUT" | grep -oE "CPU: user: [0-9]+\.[0-9]+" | grep -oE "[0-9]+\.[0-9]+" | head -1)
    local CPU_SYSTEM=$(echo "$OUTPUT" | grep -oE "system: [0-9]+\.[0-9]+" | grep -oE "[0-9]+\.[0-9]+" | head -1)
    
    # Handle missing values
    if [ -z "$INDEX_VACUUM_COUNT" ]; then INDEX_VACUUM_COUNT="0"; fi
    if [ -z "$DEAD_TUPLE_BYTES" ]; then DEAD_TUPLE_BYTES="0"; fi
    if [ -z "$ELAPSED" ]; then ELAPSED="0"; fi
    if [ -z "$CPU_USER" ]; then CPU_USER="0"; fi
    if [ -z "$CPU_SYSTEM" ]; then CPU_SYSTEM="0"; fi
    
    # Write results to CSV
    echo "$config_name,$run_num,$NUM_ROWS,$MAINTENANCE_MEM,$INDEX_VACUUM_COUNT,$DEAD_TUPLE_BYTES,$ELAPSED,$CPU_USER,$CPU_SYSTEM" >> "$RESULT_FILE"
    
    echo -e "${GREEN}    ✓ Completed: ${ELAPSED}s, index_scans=$INDEX_VACUUM_COUNT${NC}"
    
    # Cleanup test SQL
    rm -f "$TEST_SQL"
}

# Main benchmark loop
echo -e "${BLUE}Starting benchmark...${NC}"
echo ""

# Test 1: Standard ART
echo -e "${YELLOW}Testing Standard Radix Tree (ART)...${NC}"
rebuild_postgres 0 "ART"
init_database

for ((i=1; i<=NUM_RUNS; i++)); do
    run_benchmark "ART" $i
done

# Test 2: QuART optimized
echo ""
echo -e "${YELLOW}Testing QuART-Optimized Radix Tree...${NC}"
rebuild_postgres 1 "QuART"
init_database

for ((i=1; i<=NUM_RUNS; i++)); do
    run_benchmark "QuART" $i
done

# Stop PostgreSQL
"$INSTALL_DIR/bin/pg_ctl" -D "$DATA_DIR" stop -m fast -w > /dev/null 2>&1

# Generate summary report
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Benchmark Complete!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "Results saved to: $RESULT_FILE"
echo ""

# Calculate averages using awk
echo -e "${BLUE}Summary Statistics:${NC}"
echo ""

awk -F',' '
BEGIN {
    print "Configuration | Avg Elapsed (s) | Avg Index Scans | Improvement"
    print "------------- | --------------- | --------------- | -----------"
}
NR>1 {
    config=$1
    elapsed=$7
    index_scans=$5
    
    sum_elapsed[config] += elapsed
    sum_scans[config] += index_scans
    count[config]++
}
END {
    for (config in sum_elapsed) {
        avg_elapsed = sum_elapsed[config] / count[config]
        avg_scans = sum_scans[config] / count[config]
        
        printf "%-13s | %15.2f | %15.1f | ", config, avg_elapsed, avg_scans
        
        if (config == "QuART" && "ART" in sum_elapsed) {
            art_elapsed = sum_elapsed["ART"] / count["ART"]
            improvement = ((art_elapsed - avg_elapsed) / art_elapsed) * 100
            printf "%+.1f%%\n", improvement
        } else if (config == "ART") {
            printf "baseline\n"
        } else {
            printf "N/A\n"
        }
    }
}
' "$RESULT_FILE"

echo ""
echo -e "${BLUE}View detailed results:${NC}"
echo "  cat $RESULT_FILE"
echo ""
echo -e "${BLUE}Analyze with Python:${NC}"
echo "  python3 analyze_benchmark.py $RESULT_FILE"
echo ""
