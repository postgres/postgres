#!/bin/bash

set -e

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"

cd /tmp

wget https://raw.githubusercontent.com/OpenKMIP/PyKMIP/refs/heads/master/bin/create_certificates.py
python3 create_certificates.py

mkdir policies
cd policies
wget https://raw.githubusercontent.com/OpenKMIP/PyKMIP/refs/heads/master/examples/policy.json
cd ..

echo $SCRIPT_DIR
pykmip-server -f "$SCRIPT_DIR/../contrib/pg_tde/pykmip-server.conf" -l /tmp/kmip-server.log &

TV=$(mktemp)
{ exec >$TV; vault server -dev; } &
sleep 10
export ROOT_TOKEN_FILE=$(mktemp)
cat $TV | grep "Root Token" | cut -d ":" -f 2 | xargs echo -n > $ROOT_TOKEN_FILE
echo "export ROOT_TOKEN_FILE=$ROOT_TOKEN_FILE"
if [ -v GITHUB_ACTIONS ]; then
    echo "ROOT_TOKEN_FILE=$ROOT_TOKEN_FILE" >> $GITHUB_ENV
fi
