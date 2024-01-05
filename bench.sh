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
  --threads=8 \
  --time=$TIME \
  --range_selects=off \
  --index_updates=0 \
  --non_index_updates=8 \
  --delete_inserts=0 \
  --point_selects=8 \
  --rand-type=zipfian \
  --rand-zipfian-exp=0.8 \
  run
