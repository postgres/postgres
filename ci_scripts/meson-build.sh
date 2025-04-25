#!/bin/bash

ENABLE_COVERAGE=

for arg in "$@"
do
    case "$arg" in
        --enable-coverage)
            ENABLE_COVERAGE="-Db_coverage=true"
            shift;;
    esac
done

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
INSTALL_DIR="$SCRIPT_DIR/../../pginst"
source "$SCRIPT_DIR/env.sh"

cd "$SCRIPT_DIR/.."

meson setup build --prefix "$INSTALL_DIR" --buildtype="$1" -Dcassert=true -Dtap_tests=enabled $ENABLE_COVERAGE
cd build && ninja && ninja install
