#!/bin/bash

ROOT_DIR=$(pwd)
PSQL_DIR="$ROOT_DIR/src/bin/psql"
SELECTED_PORT=$1
NODE_TYPE=$2
CURRENT_USER=$(whoami)

cd $PSQL_DIR
echo "[start-psql] Running psql on port $SELECTED_PORT with node type $NODE_TYPE..."
./psql -h localhost -p $SELECTED_PORT -U $CURRENT_USER -d postgres -nodeType $NODE_TYPE
