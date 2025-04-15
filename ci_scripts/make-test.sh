#!/bin/bash

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P)"
INSTALL_DIR="$SCRIPT_DIR/../../pginst"
source $SCRIPT_DIR/env.sh

cd "$SCRIPT_DIR/.."

make -s check-world
