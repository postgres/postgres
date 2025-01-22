#!/bin/bash

export TDE_MODE=1

SCRIPT_DIR="$( cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P )"
INSTALL_DIR="$SCRIPT_DIR/../../pginst"

cd "$SCRIPT_DIR/../"

export PATH=$INSTALL_DIR/bin:$PATH
export PGDATA=$INSTALL_DIR/data

initdb -D $PGDATA

echo "shared_preload_libraries ='pg_tde'" >> $PGDATA/postgresql.conf
echo "pg_tde.wal_encrypt = on" >> $PGDATA/postgresql.conf

pg_ctl -D $PGDATA start

EXTRA_REGRESS_OPTS="--extra-setup=$SCRIPT_DIR/tde_setup.sql --load-extension=pg_tde" make installcheck-world -k