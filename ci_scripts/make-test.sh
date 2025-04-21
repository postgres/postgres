#!/bin/bash

set -e
TDE_ONLY=0

for arg in "$@"
do
    case "$arg" in
        --tde-only)
            TDE_ONLY=1
            shift;;
    esac
done

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
