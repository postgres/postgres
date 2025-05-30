#!/bin/bash

set -e

ARGS=

for arg in "$@"
do
    case "$arg" in
        --enable-coverage)
            ARGS+=" --enable-coverage"
            ;;
    esac
done

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
INSTALL_DIR="$SCRIPT_DIR/../../pginst"
source "$SCRIPT_DIR/env.sh"

cd "$SCRIPT_DIR/.."

case "$1" in
    debug)
        echo "Building with debug option"
        ARGS+=" --enable-cassert"
        ;;

    debugoptimized)
        echo "Building with debugoptimized option"
        export CFLAGS="-O2"
        ARGS+=" --enable-cassert"
        ;;

    sanitize)
        echo "Building with sanitize option"
        export CFLAGS="-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -fno-inline-functions"
        ;;

    *)
        echo "Unknown build type: $1"
        echo "Please use one of the following: debug, debugoptimized, sanitize"
        exit 1
        ;;
esac

./configure --prefix="$INSTALL_DIR" --enable-debug --enable-tap-tests $ARGS 
make install-world -j
