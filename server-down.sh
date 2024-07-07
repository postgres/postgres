#!/bin/bash

# Define paths
ROOT_DIR=$(pwd)
CLUSTERS_DIR="$ROOT_DIR/clusters"
PG_CTL_DIR="$ROOT_DIR/src/bin/pg_ctl"

# List available database clusters
echo "Available database clusters:"
clusters=()
for dir in $CLUSTERS_DIR/*/; do
    if [[ "$dir" != "$CLUSTERS_DIR/config/" && "$dir" != "$CLUSTERS_DIR/contrib/" && "$dir" != "$CLUSTERS_DIR/doc/" && "$dir" != "$CLUSTERS_DIR/sharding/" && "$dir" != "$CLUSTERS_DIR/src/" ]]; then
        clusters+=("$(basename "$dir")")
        echo "$(basename "$dir")"
    fi
done

# Determine if there is only one cluster available
if [ ${#clusters[@]} -eq 1 ]; then
    DB_CLUSTER_NAME="${clusters[0]}"
    echo "Only one cluster available. Automatically stopping: $DB_CLUSTER_NAME"
else
    # Ask for the database cluster name to stop
    read -p "Enter the name of the database cluster to stop: " DB_CLUSTER_NAME
fi

DB_DIR="$CLUSTERS_DIR/$DB_CLUSTER_NAME"

if [ ! -d "$DB_DIR" ]; then
    echo "[ERROR] The specified database cluster directory does not exist."
    exit 1
fi

echo "[DISTRIBUTED POSTGRESQL] Stopping PostgreSQL server for cluster $DB_CLUSTER_NAME..."
cd $PG_CTL_DIR
./pg_ctl -D $DB_DIR stop

echo "[DISTRIBUTED POSTGRESQL] PostgreSQL server stopped successfully for cluster $DB_CLUSTER_NAME."
