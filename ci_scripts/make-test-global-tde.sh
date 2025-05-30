#!/bin/bash

set -e

ADD_FLAGS=

for arg in "$@"
do
    case "$arg" in
        --continue)
            ADD_FLAGS="-k"
            shift;;
    esac
done

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
source "$SCRIPT_DIR/configure-global-tde.sh"

EXTRA_REGRESS_OPTS="--extra-setup=$SCRIPT_DIR/tde_setup.sql" make -s installcheck-world $ADD_FLAGS
