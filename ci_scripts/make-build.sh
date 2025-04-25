#!/bin/bash

ENABLE_COVERAGE=

for arg in "$@"
do
    case "$arg" in
        --enable-coverage)
            ENABLE_COVERAGE="--enable-coverage"
            shift;;
    esac
done

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
INSTALL_DIR="$SCRIPT_DIR/../../pginst"
source "$SCRIPT_DIR/env.sh"

cd "$SCRIPT_DIR/.."

if [ "$1" = "debugoptimized" ]; then
    export CFLAGS="-O2"
    export CXXFLAGS="-O2"
fi

./configure --prefix="$INSTALL_DIR" --enable-debug --enable-cassert --enable-tap-tests $ENABLE_COVERAGE
make install-world -j
