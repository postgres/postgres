#!/usr/bin/env bash
cd "$(dirname "$0")"

xz -d pp-2019.csv.xz
RECORDS=`wc -l pp-2019.csv`
echo "CSV entries: $RECORDS"
cp pp-2019.csv /tmp/
createdb seq_read_test
psql seq_read_test < seq_read_prepare.sql > /dev/null
echo "Sequential scan read times"
echo "=========================="
echo -n "HEAP: "
HEAP=`psql seq_read_test < seq_read_run_heap.sql | grep "Execution" | tail -n 10 | cut -d " " -f 4 | paste -sd+ | bc`
echo $HEAP
echo -n "TDE: "
TDE=`psql seq_read_test < seq_read_run_tde.sql | grep "Execution" | tail -n 10 | cut -d " " -f 4 | paste -sd+ | bc`
TDE_PERC=`bc <<< "$TDE*100/$HEAP"`
echo "$TDE ($TDE_PERC%)"
echo -n "TDE_BASIC: "
TDE_BASIC=`psql seq_read_test < seq_read_run_tde_basic.sql | grep "Execution" | tail -n 10 | cut -d " " -f 4 | paste -sd+ | bc`
TDE_BASIC_PERC=`bc <<< "$TDE_BASIC*100/$HEAP"`
echo "$TDE ($TDE_BASIC_PERC%)"
