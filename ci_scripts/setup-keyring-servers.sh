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
rm -f /tmp/pykmip.db
pykmip-server -f "$SCRIPT_DIR/../contrib/pg_tde/pykmip-server.conf" -l /tmp/kmip-server.log &

CLUSTER_INFO=$(mktemp)
vault server -dev -dev-tls -dev-cluster-json="$CLUSTER_INFO" > /dev/null &
sleep 10
export VAULT_ROOT_TOKEN_FILE=$(mktemp)
jq -r .root_token "$CLUSTER_INFO" > "$VAULT_ROOT_TOKEN_FILE"
export VAULT_CACERT_FILE=$(jq -r .ca_cert_path "$CLUSTER_INFO")
rm "$CLUSTER_INFO"
if [ -v GITHUB_ACTIONS ]; then
    echo "VAULT_ROOT_TOKEN_FILE=$VAULT_ROOT_TOKEN_FILE" >> $GITHUB_ENV
    echo "VAULT_CACERT_FILE=$VAULT_CACERT_FILE" >> $GITHUB_ENV
fi
