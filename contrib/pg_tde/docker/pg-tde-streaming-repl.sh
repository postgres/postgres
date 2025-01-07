#!/bin/bash

set -e

PG_PRIMARY=${PG_PRIMARY:-"false"}
PG_REPLICATION=${PG_REPLICATION:-"false"}
REPL_PASS=${REPL_PASS:-"replpass"}
PG_USER=${POSTGRES_USER:-"postgres"}


if [ "$PG_REPLICATION" == "true" ] ; then
  if [ "$PG_PRIMARY" == "true" ] ; then
    psql -U ${PG_USER} -c "CREATE ROLE repl WITH REPLICATION PASSWORD '${REPL_PASS}' LOGIN;"
    echo "host replication repl 0.0.0.0/0 trust" >> ${PGDATA}/pg_hba.conf
  else
    rm -rf  ${PGDATA}/*
    pg_basebackup -h pg-primary -p 5432 -U repl -D ${PGDATA} -Fp -Xs -R
  fi
fi

