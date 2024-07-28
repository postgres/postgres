#!/bin/bash

# Ask for the database cluster name
read -p "Enter the name of the database cluster to create: " DB_CLUSTER_NAME

# Define paths
ROOT_DIR=$(pwd)
SHARDING_DIR="$ROOT_DIR/sharding"
INITDB_DIR="$ROOT_DIR/src/bin/initdb"
PSQL_DIR="$ROOT_DIR/src/bin/psql"
POSTGRES_EXECUTABLE="$ROOT_DIR/src/backend/postgres"
CLUSTERS_DIR="$ROOT_DIR/clusters"
DB_DIR="$CLUSTERS_DIR/$DB_CLUSTER_NAME"
LOG_FILE="$CLUSTERS_DIR/logfile"

# Create clusters directory if it doesn't exist
mkdir -p $CLUSTERS_DIR

# If we're on OS X, make sure that globals aren't stripped out.
if [ "$(uname)" == "Darwin" ]; then
    export LDFLAGS="-Wl,-no_pie"
fi

echo "[create-cluster-dir] Building the project..."
cd $ROOT_DIR
make

echo "[create-cluster-dir] Creating database directory $DB_DIR..."
mkdir -p $DB_DIR

echo "[create-cluster-dir] Copying postgres executable to initdb directory..."
cp $POSTGRES_EXECUTABLE $INITDB_DIR

echo "[create-cluster-dir] Initializing the database..."
cd $INITDB_DIR
./initdb -D $DB_DIR

echo "[create-cluster-dir] Cluster created successfully."
