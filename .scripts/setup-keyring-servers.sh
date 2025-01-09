#!/bin/bash

SCRIPT_DIR="$( cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P )"

cd /tmp

wget https://raw.githubusercontent.com/OpenKMIP/PyKMIP/refs/heads/master/bin/create_certificates.py
python3 create_certificates.py
cat client_certificate_jane_doe.pem >> client_key_jane_doe.pem

mkdir policies
cd policies
wget https://raw.githubusercontent.com/OpenKMIP/PyKMIP/refs/heads/master/examples/policy.json
wget https://raw.githubusercontent.com/OpenKMIP/PyKMIP/refs/heads/master/examples/legacy_policy.json
cd ..

echo $SCRIPT_DIR
pykmip-server -f "$SCRIPT_DIR/../contrib/pg_tde/pykmip-server.conf" -l /tmp/kmip-server.log &

TV=$(mktemp)
{ exec >$TV; vault server -dev; } &
sleep 10
ROOT_TOKEN=$(cat $TV | grep "Root Token" | cut -d ":" -f 2 | xargs echo -n)
echo "ROOT_TOKEN=$ROOT_TOKEN"  >> $GITHUB_ENV
echo "Root token: $ROOT_TOKEN"