#!/bin/bash
#
# setup.sh - Build PostgreSQL from source and initialize a pgbench database
#            with pg_stat_statements enabled.
#
# Run from the postgres source root: bash contrib/pg_stat_statements/benchmark/setup.sh
#
set -euo pipefail

PGDATA="$HOME/pgdata"
PGINSTALL="$HOME/pginstall"
NCPU=$(nproc)

echo "=== Building PostgreSQL (using $NCPU cores) ==="

# Wait for user_data to finish installing deps
while [ ! -f /tmp/setup_done ]; do
    echo "Waiting for build dependencies to be installed..."
    sleep 5
done

./configure \
    --prefix="$PGINSTALL" \
    --without-icu \
    --with-openssl \
    2>&1 | tail -3

make -j"$NCPU" 2>&1 | tail -5
make install 2>&1 | tail -3

echo "=== Building pg_stat_statements ==="
make -C contrib/pg_stat_statements install 2>&1 | tail -3

# Also build pgbench
make -C contrib/pgbench install 2>&1 | tail -3 || \
    make -C src/bin/pgbench install 2>&1 | tail -3

export PATH="$PGINSTALL/bin:$PATH"

echo "=== Initializing database ==="
# Stop any existing server before wiping data
pg_ctl -D "$PGDATA" stop 2>/dev/null || true
rm -rf "$PGDATA"
initdb -D "$PGDATA" 2>&1 | tail -3

# Configure pg_stat_statements
cat >> "$PGDATA/postgresql.conf" <<EOF

# pg_stat_statements config
shared_preload_libraries = 'pg_stat_statements'
pg_stat_statements.max = 10000
pg_stat_statements.track = top
pg_stat_statements.track_utility = on
pg_stat_statements.track_planning = on
compute_query_id = on
EOF

echo "=== Starting PostgreSQL ==="
pg_ctl -D "$PGDATA" -l "$PGDATA/logfile" start
sleep 2

# Create the extension and initialize pgbench
createdb bench
psql bench -c "CREATE EXTENSION pg_stat_statements;"
pgbench -i -s 10 bench 2>&1 | tail -3

echo ""
echo "=== Setup complete ==="
echo "PostgreSQL is running with pg_stat_statements enabled."
echo "Run: bash contrib/pg_stat_statements/benchmark/run_benchmark.sh"
