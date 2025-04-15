#!/bin/bash

set -e
ADD_FLAGS=
TDE_ONLY=0

for arg in "$@"
do
    case "$arg" in
        --continue)
            ADD_FLAGS="-k"
            shift;;
        --tde-only)
            TDE_ONLY=1
            shift;;
    esac
done

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
source $SCRIPT_DIR/env.sh
source $SCRIPT_DIR/configure-tde-server.sh

if [ "$TDE_ONLY" -eq 1 ];
then
    cd "$SCRIPT_DIR/../contrib/pg_tde"
    EXTRA_REGRESS_OPTS="--extra-setup=$SCRIPT_DIR/tde_setup.sql" make -s installcheck $ADD_FLAGS
else
    cd "$SCRIPT_DIR/.."
    EXTRA_REGRESS_OPTS="--extra-setup=$SCRIPT_DIR/tde_setup.sql" make -s installcheck-world $ADD_FLAGS
fi
