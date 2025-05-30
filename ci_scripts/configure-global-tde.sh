#!/bin/bash

set -e

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
INSTALL_DIR="$SCRIPT_DIR/../../pginst"
source $SCRIPT_DIR/env.sh

cd "$SCRIPT_DIR/.."

source "$SCRIPT_DIR/env.sh"
export TDE_MODE=1
export PATH=$INSTALL_DIR/bin:$PATH
export DATA_DIR=$INSTALL_DIR/data
export PGDATA="${1:-$DATA_DIR}"
export PGPORT="${2:-5432}"

if [ -d "$PGDATA" ]; then
    if pg_ctl -D "$PGDATA" status -o "-p $PGPORT" >/dev/null; then
        pg_ctl -D "$PGDATA" stop -o "-p $PGPORT"
    fi

    rm -rf "$PGDATA"
fi

initdb -D "$PGDATA" --set shared_preload_libraries=pg_tde

pg_ctl -D "$PGDATA" start -o "-p $PGPORT"

psql postgres -f "$SCRIPT_DIR/tde_setup_global.sql" -v ON_ERROR_STOP=on

pg_ctl -D "$PGDATA" restart -o "-p $PGPORT"
