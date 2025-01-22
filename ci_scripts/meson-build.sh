#!/bin/bash

SCRIPT_DIR="$( cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P )"

cd "$SCRIPT_DIR/../"

meson setup build --prefix `pwd`/../pginst --buildtype=$1 -Dcassert=true -Dtap_tests=enabled
cd build && ninja && ninja install