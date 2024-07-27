#!/usr/bin
#psql -d sysbench -c 'select pg_reload_conf()';
#psql -d postgres -c 'drop database sysbench';
#psql -d postgres -c 'create database sysbench';

# For benchmarking of learned CC

N_THREAD=4
RUN_TIME=5
TABLES=1

while getopts ":t:T:a" opt; do
  case $opt in
      a)
          LAUNCH=1
          ;;
      T)
          RUN_TIME=$OPTARG
          ;;
      t)
          N_THREAD=$OPTARG
          ;;
      \?)
          echo "Invalid option: -$OPTARG" >&2
          usage
          ;;
      :)
          echo "Option -$OPTARG requires an argument." >&2
          usage
          ;;
  esac
done

DB_DRIVER="pgsql"
# fixed parameters.
HOST="localhost"
PORT="5432"
USER="hexiang"
DB_NAME="sysbench"

if [ -n "$LAUNCH" ]; then
  echo "Kill pending pg_service"
  pg_ctl stop -D $PGDATA -m fast
  echo "Restart pg_service"
  pg_ctl start -D $PGDATA > ./run_log.txt
fi

# For becnhmarking of SSI.
psql -d sysbench -c 'set default_transaction_isolation="serializable"';
psql -d sysbench -c 'set default_cc_strategy="ssi"';

#psql -d sysbench -c 'set default_cc_strategy="learned"';
#psql -d sysbench -c 'drop table sbtest1';
cd sysbench-tpcc

COMMON_OPTIONS="./tpcc.lua
  --db-driver=$DB_DRIVER
  --pgsql-host=$HOST
  --pgsql-port=$PORT
  --pgsql-user=$USER
  --pgsql-db=$DB_NAME
  --time=$RUN_TIME
  --threads=$N_THREAD
  --scale=1
  --tables=$TABLES"

#sysbench $COMMON_OPTIONS \
#  cleanup

#sysbench $COMMON_OPTIONS prepare

sysbench $COMMON_OPTIONS \
  --warmup-time=2 \
  run

if [ -n "$LAUNCH" ]; then
  pg_ctl stop -D $PGDATA -m fast
fi