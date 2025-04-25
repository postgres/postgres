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

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
source "$SCRIPT_DIR/env.sh"

cd "$SCRIPT_DIR/../build"

if [ "$TDE_ONLY" -eq 1 ];
then
    meson test --suite setup --suite pg_tde
else
    meson test
fi
