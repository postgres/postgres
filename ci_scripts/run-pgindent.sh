#!/bin/bash

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
INSTALL_DIR="$SCRIPT_DIR/../../pginst"
cd "$SCRIPT_DIR/../"

if ! test -f combined.typedefs; then
  echo "combined.typedefs doesn't exists, run dump-typedefs.sh first"
  exit 1
fi

cd src/tools/pg_bsd_indent
make install

cd "$SCRIPT_DIR/.."

export PATH=$SCRIPT_DIR/../src/tools/pgindent/:$INSTALL_DIR/bin/:$PATH

# Check everything except pg_tde with the list in the repo
# TODO: Disabled due to incorrectly indented upsrteam as of 17.4
#pgindent --typedefs=src/tools/pgindent/typedefs.list --excludes=<(echo "contrib/pg_tde") "$@" .

# Check pg_tde with the fresh list extraxted from the object file
pgindent --typedefs=combined.typedefs "$@" contrib/pg_tde
