#!/bin/bash

# Define paths
ROOT_DIR=$(pwd)
SHARDING_DIR="$ROOT_DIR/sharding"
INITDB_DIR="$ROOT_DIR/src/bin/initdb"
PG_CTL_DIR="$ROOT_DIR/src/bin/pg_ctl"
POSTGRES_EXECUTABLE="$ROOT_DIR/src/backend/postgres"
DB_DIR="$ROOT_DIR/mydb"
LOG_FILE="$DB_DIR/logfile"

echo "Compiling sharding library..."
cd $SHARDING_DIR
cargo build --release --lib

echo "Moving compiled library to initdb directory..."
mv ./target/release/libsharding.a $INITDB_DIR

echo "Building the project..."
cd $ROOT_DIR
make

echo "Creating database directory..."
mkdir -p $DB_DIR

echo "Copying postgres executable to initdb directory..."
cp $POSTGRES_EXECUTABLE $INITDB_DIR

echo "[DISTRIBUTED POSTGRESQL]Initializing the database..."
cd $INITDB_DIR
./initdb -D $DB_DIR

echo "Copying postgres executable to pg_ctl directory..."
cp $POSTGRES_EXECUTABLE $PG_CTL_DIR

echo "Starting PostgreSQL server..."
cd $PG_CTL_DIR
./pg_ctl -D $DB_DIR -l $LOG_FILE start

echo "[DISTRIBUTED POSTGRESQL] Database initialized and server started successfully."
