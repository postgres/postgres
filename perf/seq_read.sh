#!/usr/bin/env bash
cd "$(dirname "$0")"

xz -d pp-2019.csv.xz
cp pp-2019.csv /tmp/
createdb seq_read_test
psql seq_read_test < seq_read_prepare.sql > /dev/null
echo "Sequential scan read times"
echo "=========================="
echo -n "HEAP: "
psql seq_read_test < seq_read_run_heap.sql | grep "Execution" | tail -n 10 | cut -d " " -f 4 | paste -sd+ | bc
echo -n "TDE: "
psql seq_read_test < seq_read_run_tde.sql | grep "Execution" | tail -n 10 | cut -d " " -f 4 | paste -sd+ | bc
echo -n "TDE_BASIC: "
psql seq_read_test < seq_read_run_tde_basic.sql | grep "Execution" | tail -n 10 | cut -d " " -f 4 | paste -sd+ | bc
