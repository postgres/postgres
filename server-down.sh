#!/bin/bash

ROOT_DIR=$(pwd)
PG_CTL_DIR="$ROOT_DIR/src/bin/pg_ctl"

echo "Available database clusters:"
for dir in $ROOT_DIR/*/; do
    if [[ "$dir" != "$ROOT_DIR/config/" && "$dir" != "$ROOT_DIR/contrib/" && "$dir" != "$ROOT_DIR/doc/" && "$dir" != "$ROOT_DIR/sharding/" && "$dir" != "$ROOT_DIR/src/" ]]; then
        echo "$(basename "$dir")"
    fi
done

read -p "Enter the name of the database cluster to stop: " DB_CLUSTER_NAME
DB_DIR="$ROOT_DIR/$DB_CLUSTER_NAME"

if [ ! -d "$DB_DIR" ]; then
    echo "[ERROR] The specified database cluster directory does not exist."
    exit 1
fi

echo "[DISTRIBUTED POSTGRESQL] Stopping PostgreSQL server for cluster $DB_CLUSTER_NAME..."
cd $PG_CTL_DIR
./pg_ctl -D $DB_DIR stop

echo "[DISTRIBUTED POSTGRESQL] PostgreSQL server stopped successfully for cluster $DB_CLUSTER_NAME."
