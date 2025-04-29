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

BUILD_TYPE=

case "$1" in
    debug)
        echo "Building with debug option"
        BUILD_TYPE=$1
        ;;

    debugoptimized)
        echo "Building with debugoptimized option"
        BUILD_TYPE=$1
        ;;

    sanitize)
        echo "Building with sanitize option"
        export CFLAGS="-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -fno-inline-functions"
        BUILD_TYPE=debug
        ;;

    *)
        echo "Unknown build type: $1"
        echo "Please use one of the following: debug, debugoptimized, sanitize"
        exit 1
        ;;
esac

meson setup build --prefix "$INSTALL_DIR" --buildtype="$BUILD_TYPE" -Dcassert=true -Dtap_tests=enabled $ENABLE_COVERAGE
cd build && ninja && ninja install
