#!/bin/bash

# Ask for the database cluster name
read -p "Enter the name of the database cluster to start: " DB_CLUSTER_NAME

# Define paths
ROOT_DIR=$(pwd)
SHARDING_DIR="$ROOT_DIR/sharding"
PG_CTL_DIR="$ROOT_DIR/src/bin/pg_ctl"
PSQL_DIR="$ROOT_DIR/src/bin/psql"
POSTGRES_EXECUTABLE="$ROOT_DIR/src/backend/postgres"
DB_DIR="$ROOT_DIR/$DB_CLUSTER_NAME"
LOG_FILE="$ROOT_DIR/logfile"

# If we're on OS X, make sure that globals aren't stripped out.
if [ "$(uname)" == "Darwin" ]; then
    export LDFLAGS="-Wl,-no_pie"
fi

echo "Compiling sharding library..."
cd $SHARDING_DIR
cargo build --release --lib
echo "Moving compiled library to psql directory..."
mv ./target/release/libsharding.a $PSQL_DIR
echo "Building the project..."
cd $ROOT_DIR
make

echo "Copying postgres executable to pg_ctl directory..."
cp $POSTGRES_EXECUTABLE $PG_CTL_DIR

echo "Starting PostgreSQL server..."
cd $PG_CTL_DIR
./pg_ctl -D $DB_DIR -l $LOG_FILE start

echo "[DISTRIBUTED POSTGRESQL] Database started successfully."
