#!/bin/bash

ROOT_DIR=$(pwd)
PSQL_DIR="$ROOT_DIR/src/bin/psql"

cd $PSQL_DIR
./psql -h localhost -p 5433 -U aldanarastrelli -d postgres -nodeType $1