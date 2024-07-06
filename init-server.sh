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
PORT_FILE="$ROOT_DIR/ports.txt" # Path to ports.txt

# If we're on OS X, make sure that globals aren't stripped out.
if [ "$(uname)" == "Darwin" ]; then
    export LDFLAGS="-Wl,-no_pie"
fi

echo "Compiling sharding library..."
cd $SHARDING_DIR
cargo clean
cargo build --release --lib
echo "Moving compiled library to psql directory..."
mv ./target/release/libsharding.a $PSQL_DIR
echo "Building the project..."
cd $ROOT_DIR
make

echo "Copying postgres executable to pg_ctl directory..."
cp $POSTGRES_EXECUTABLE $PG_CTL_DIR

# Function to check if a port is available
port_available() {
    local port=$1
    # Check if port is in use
    if nc -z localhost $port; then
        echo "Port $port is in use."
        return 1  # Port is in use
    else
        echo "Port $port is available."
        return 0  # Port is available
    fi
}

# Read ports from ports.txt into an array
ports=($(cat $PORT_FILE))

# Find an available port
selected_port=""
for port in "${ports[@]}"; do
    if port_available $port; then
        selected_port=$port
        break
    fi
done

if [ -z "$selected_port" ]; then
    echo "Error: No available ports found in ports.txt"
    exit 1
fi

echo "Starting PostgreSQL server on port $selected_port..."
cd $PG_CTL_DIR
./pg_ctl -D $DB_DIR -l $LOG_FILE -o "-p $selected_port" start

echo "[DISTRIBUTED POSTGRESQL] Database started successfully."
