#!/bin/bash

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P)"
INSTALL_DIR="$SCRIPT_DIR/../../pginst"
source $SCRIPT_DIR/env.sh

if [ "$TDE_ONLY" -eq 1 ];
then
    cd "$SCRIPT_DIR/../contrib/pg_tde"
    make -s check
else
    cd "$SCRIPT_DIR/.."
    make -s check-world
fi
