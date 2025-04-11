#!/bin/bash

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
cd "$SCRIPT_DIR/.."

if ! test -f src/backend/postgres; then
  echo "src/backend/postgres doesn't exists, run make-build.sh first in debug mode"
  exit 1
fi

if ! test -f contrib/pg_tde/pg_tde.so; then
  echo "contrib/pg_tde/pg_tde.so doesn't exists, run make-build.sh first in debug mode"
  exit 1
fi

src/tools/find_typedef src/backend contrib/pg_tde > percona.typedefs

# Combine with original typedefs
cat percona.typedefs src/tools/pgindent/typedefs.list | sort -u > combined.typedefs
