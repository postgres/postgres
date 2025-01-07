#!/bin/bash

set -e

PG_USER=${POSTGRES_USER:-"postgres"}

psql -U ${PG_USER} -c 'CREATE EXTENSION pg_tde;'
psql -U ${PG_USER} -d template1 -c 'CREATE EXTENSION pg_tde;'
