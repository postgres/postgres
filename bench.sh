#psql -d postgres -c 'create database sysbench';
#psql -d sysbench -c 'select pg_reload_conf()';
#psql -d sysbench -c 'set default_transaction_isolation="read committed"';
#psql -d sysbench -c 'set default_cc_strategy="s2pl"';

#
#pgbench pgbench;
# --client=16  --jobs=16 --time=30


DB_DRIVER="pgsql"
# fixed parameters.
HOST="localhost"
PORT="5432"
USER="hexiang"
DB_NAME="sysbench"
TIME="5"
# adjustable parameters.
TABLES="1"
TABLE_SIZE="10000"


COMMON_OPTIONS="/usr/local/share/sysbench/oltp_read_write.lua
  --db-driver=$DB_DRIVER
  --pgsql-host=$HOST
  --pgsql-port=$PORT
  --pgsql-user=$USER
  --pgsql-db=$DB_NAME
  --tables=$TABLES
  --table-size=$TABLE_SIZE"

sysbench $COMMON_OPTIONS prepare

sysbench $COMMON_OPTIONS \
  --threads=4 \
  --time=5 \
  --warmup-time=2 \
  --validate=off \
  --range_selects=off \
  --index_updates=0 \
  --non_index_updates=5 \
  --delete_inserts=0 \
  --point_selects=5 \
  --rand-type=zipfian \
  --rand-zipfian-exp=0.5 \
  run


