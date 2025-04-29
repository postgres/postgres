#!/bin/bash

set -e

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
source "$SCRIPT_DIR/env.sh"

cd "$SCRIPT_DIR/../build"


case "$1" in
    server)
        echo "Run server regression tests"
        meson test --suite setup --suite regress
        ;;

    tde)
        echo "Run tde tests"
        meson test --suite setup --suite pg_tde
        ;;

    all)
        echo "Run all tests"
        meson test
        ;;

    *)
        echo "Unknown test suite: $1"
        echo "Please use one of the following: server, tde, all"
        exit 1
        ;;
esac
