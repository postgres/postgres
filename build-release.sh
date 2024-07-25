ROOT_DIR=$(pwd)
SHARDING_DIR="$ROOT_DIR/sharding"
PG_CTL_DIR="$ROOT_DIR/src/bin/pg_ctl"
PSQL_DIR="$ROOT_DIR/src/bin/psql"

echo "[init-server] Compiling sharding library..."
cd $SHARDING_DIR
cargo build --release --lib
echo "[init-server] Moving compiled library to psql directory..."
cp ./target/release/libsharding.a $PSQL_DIR
cd $ROOT_DIR