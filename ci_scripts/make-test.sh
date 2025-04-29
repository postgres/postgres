#!/bin/bash

set -e

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
source "$SCRIPT_DIR/env.sh"


case "$1" in
    server)
        echo "Run server regression tests"
        cd "$SCRIPT_DIR/.."
        make -s check
        ;;

    tde)
        echo "Run tde tests"
        cd "$SCRIPT_DIR/../contrib/pg_tde"
        make -s check
        ;;

    all)
        echo "Run all tests"
        cd "$SCRIPT_DIR/.."
        make -s check-world
        ;;

    *)
        echo "Unknown test suite: $1"
        echo "Please use one of the following: server, tde, all"
        exit 1
        ;;
esac
