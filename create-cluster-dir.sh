#!/bin/bash

# Ask for the database cluster name
read -p "Enter the name of the database cluster to create: " DB_CLUSTER_NAME

# Define paths
ROOT_DIR=$(pwd)
SHARDING_DIR="$ROOT_DIR/sharding"
INITDB_DIR="$ROOT_DIR/src/bin/initdb"
PSQL_DIR="$ROOT_DIR/src/bin/psql"
POSTGRES_EXECUTABLE="$ROOT_DIR/src/backend/postgres"
DB_DIR="$ROOT_DIR/$DB_CLUSTER_NAME"
LOG_FILE="$ROOT_DIR/logfile"

echo "Compiling sharding library..."
cd $SHARDING_DIR
cargo build --release --lib

echo "Copying compiled library to initdb directory..."
cp ./target/release/libsharding.a $INITDB_DIR
echo "Copying compiled library to psql directory..."
cp ./target/release/libsharding.a $PSQL_DIR

echo "Building the project..."
cd $ROOT_DIR
make

echo "Creating database directory..."
mkdir -p $DB_DIR

echo "Copying postgres executable to initdb directory..."
cp $POSTGRES_EXECUTABLE $INITDB_DIR

echo "[DISTRIBUTED POSTGRESQL] Initializing the database..."
cd $INITDB_DIR
./initdb -D $DB_DIR

echo "[DISTRIBUTED POSTGRESQL] Database initialized successfully."
