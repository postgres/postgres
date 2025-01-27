
SCRIPT_DIR="$( cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P )"
INSTALL_DIR="$SCRIPT_DIR/../../pginst"
cd "$SCRIPT_DIR/../"

if ! test -f combined.typedefs; then
  echo "combined.typedefs doesn't exists, run dump-typedefs.sh first"
  exit 1
fi

cd src/tools/pg_bsd_indent
make install

cd "$SCRIPT_DIR/../"


export PATH=$SCRIPT_DIR/../src/tools/pgindent/:$INSTALL_DIR/bin/:$PATH

pgindent --typedefs=combined.typedefs "$@" .