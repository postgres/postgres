#!/bin/bash

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
cd "$SCRIPT_DIR/../"

source src/tools/perlcheck/find_perl_files

find_perl_files contrib/pg_tde/ | xargs perltidy "$@" --profile=src/tools/pgindent/perltidyrc
