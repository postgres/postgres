#!/bin/bash
# This script is used to configure a TDE server for testing purposes.
export TDE_MODE=1

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
INSTALL_DIR="$SCRIPT_DIR/../../pginst"

cd "$SCRIPT_DIR/.."

export PATH=$INSTALL_DIR/bin:$PATH
export PGDATA=$INSTALL_DIR/data

if pgrep -x "postgres" > /dev/null; then
    pg_ctl -D "$PGDATA" stop
fi

if pgrep -x "postgres" > /dev/null; then
    echo "Error: a postgres process is already running"
    exit 1
fi

if [ -d "$PGDATA" ]; then
    rm -rf "$PGDATA"
fi

initdb -D "$PGDATA" --set shared_preload_libraries=pg_tde

pg_ctl -D "$PGDATA" start

createdb setup_helper
psql setup_helper -f "$SCRIPT_DIR/tde_setup_global.sql"

pg_ctl -D "$PGDATA" restart
