#!/bin/bash

ROOT_DIR=$(pwd)
CLUSTERS_DIR="$ROOT_DIR/clusters"
PG_CTL_DIR="$ROOT_DIR/src/bin/pg_ctl"

echo "Available database clusters:"
for dir in $CLUSTERS_DIR/*/; do
    if [[ "$dir" != "$CLUSTERS_DIR/config/" && "$dir" != "$CLUSTERS_DIR/contrib/" && "$dir" != "$CLUSTERS_DIR/doc/" && "$dir" != "$CLUSTERS_DIR/sharding/" && "$dir" != "$CLUSTERS_DIR/src/" ]]; then
        echo "$(basename "$dir")"
    fi
done

read -p "Enter the name of the database cluster to stop: " DB_CLUSTER_NAME
DB_DIR="$CLUSTERS_DIR/$DB_CLUSTER_NAME"

if [ ! -d "$DB_DIR" ]; then
    echo "[ERROR] The specified database cluster directory does not exist."
    exit 1
fi

echo "[DISTRIBUTED POSTGRESQL] Stopping PostgreSQL server for cluster $DB_CLUSTER_NAME..."
cd $PG_CTL_DIR
./pg_ctl -D $DB_DIR stop

echo "[DISTRIBUTED POSTGRESQL] PostgreSQL server stopped successfully for cluster $DB_CLUSTER_NAME."
