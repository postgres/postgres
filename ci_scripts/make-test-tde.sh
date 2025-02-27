#!/bin/bash

set -e

export TDE_MODE=1

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
source $SCRIPT_DIR/configure-tde-server.sh

ADD_FLAGS=

if [ "$1" = "--continue" ]; then
    ADD_FLAGS="-k"
fi

EXTRA_REGRESS_OPTS="--extra-setup=$SCRIPT_DIR/tde_setup.sql" make installcheck-world $ADD_FLAGS
