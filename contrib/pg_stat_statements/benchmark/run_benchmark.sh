#!/bin/bash
#
# run_benchmark.sh - Benchmark pg_stat_statements CPU time tracking
#
# Runs pgbench at various concurrency levels, with and without CPU saturation,
# then queries pg_stat_statements to compare wall time vs CPU time.
#
set -euo pipefail

export PATH="$HOME/pginstall/bin:$PATH"
DB="bench"
NCPU=$(nproc)
DURATION=30          # seconds per pgbench run
PGBENCH_SCALE=10     # matches -s 10 from setup
RESULTS_FILE="$HOME/benchmark_results.txt"

# Concurrency levels to test
CLIENTS=(1 4 8 $((NCPU * 2)) $((NCPU * 4)))

# Stress-ng intensity: 0 = no load, N = N CPU-burning workers
STRESS_LEVELS=(0 $NCPU $((NCPU * 2)))

header() {
    echo ""
    echo "================================================================"
    echo "  $1"
    echo "================================================================"
}

run_one() {
    local clients=$1
    local stress=$2
    local label="clients=$clients, stress=$stress"
    local stress_pid=""

    echo ""
    echo "--- $label ---"

    # Reset stats
    psql "$DB" -qc "SELECT pg_stat_statements_reset();" > /dev/null

    # Start CPU stress if requested
    if [ "$stress" -gt 0 ]; then
        stress-ng --cpu "$stress" --timeout $((DURATION + 10))s &>/dev/null &
        stress_pid=$!
        sleep 1
    fi

    # Run pgbench
    pgbench -c "$clients" -j "$clients" -T "$DURATION" "$DB" 2>&1 | grep -E "^(tps|latency|number)"

    # Stop stress
    if [ -n "$stress_pid" ]; then
        kill "$stress_pid" 2>/dev/null
        wait "$stress_pid" 2>/dev/null || true
    fi

    # Query pg_stat_statements for the pgbench queries
    psql "$DB" -P pager=off -c "
        SELECT
            left(query, 60) AS query,
            calls,
            round(total_exec_time::numeric, 2) AS wall_ms,
            round(total_exec_cpu_time::numeric, 2) AS cpu_ms,
            round((total_exec_time - total_exec_cpu_time)::numeric, 2) AS off_cpu_ms,
            CASE WHEN total_exec_time > 0
                THEN round((total_exec_cpu_time / total_exec_time * 100)::numeric, 1)
                ELSE 0
            END AS cpu_pct
        FROM pg_stat_statements
        WHERE query NOT LIKE '%pg_stat_statements%'
          AND query NOT LIKE '%BEGIN%'
          AND query NOT LIKE '%END%'
          AND calls > 10
        ORDER BY total_exec_time DESC
        LIMIT 8;
    "

    # Summary line for the aggregate
    psql "$DB" -t -A -c "
        SELECT
            '$clients' AS clients,
            '$stress' AS stress_workers,
            round(sum(total_exec_time)::numeric, 2) AS total_wall_ms,
            round(sum(total_exec_cpu_time)::numeric, 2) AS total_cpu_ms,
            round(sum(total_exec_time - total_exec_cpu_time)::numeric, 2) AS total_off_cpu_ms,
            CASE WHEN sum(total_exec_time) > 0
                THEN round((sum(total_exec_cpu_time) / sum(total_exec_time) * 100)::numeric, 1)
                ELSE 0
            END AS cpu_pct
        FROM pg_stat_statements
        WHERE query NOT LIKE '%pg_stat_statements%'
          AND calls > 0;
    " >> "$RESULTS_FILE"
}

# ============================================================
# Main
# ============================================================

echo "Benchmark: pg_stat_statements CPU time tracking"
echo "Instance: $(uname -n) ($(nproc) vCPUs)"
echo "Duration per run: ${DURATION}s"
echo ""

# Initialize results file with CSV header
echo "clients,stress_workers,total_wall_ms,total_cpu_ms,total_off_cpu_ms,cpu_pct" > "$RESULTS_FILE"

header "Verifying CPU time columns exist"
psql "$DB" -c "
    SELECT total_plan_cpu_time, total_exec_cpu_time
    FROM pg_stat_statements
    LIMIT 1;
"

for stress in "${STRESS_LEVELS[@]}"; do
    if [ "$stress" -eq 0 ]; then
        header "NO CPU STRESS"
    else
        header "CPU STRESS: $stress workers on $NCPU vCPUs"
    fi

    for clients in "${CLIENTS[@]}"; do
        run_one "$clients" "$stress"
    done
done

header "SUMMARY"
echo ""
echo "Instance: $(uname -n) ($(nproc) vCPUs)"
echo ""
echo "clients | stress | wall_ms       | cpu_ms        | off_cpu_ms    | cpu%"
echo "--------|--------|---------------|---------------|---------------|------"
while IFS='|' read -r c s w cpu off pct; do
    printf "%-8s| %-7s| %13s | %13s | %13s | %s%%\n" "$c" "$s" "$w" "$cpu" "$off" "$pct"
done < "$RESULTS_FILE"

echo ""
echo "Full results saved to: $RESULTS_FILE"
echo ""
echo "Key insight: as stress workers increase, cpu% should decrease"
echo "(more off-CPU time = scheduling delay from CPU contention)"
