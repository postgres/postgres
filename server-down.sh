#!/bin/bash

ROOT_DIR=$(pwd)
PG_CTL_DIR="$ROOT_DIR/src/bin/pg_ctl"
DB_DIR="$ROOT_DIR/mydb"

echo "[DISTRIBUTED POSTGRESQL] Stopping PostgreSQL server..."
cd $PG_CTL_DIR
./pg_ctl -D $DB_DIR stop

echo "[DISTRIBUTED POSTGRESQL] PostgreSQL server stopped successfully."
