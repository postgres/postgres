#!/bin/sh

# PostgreSQL server start script (launched by org.postgresql.postgres.plist)

# edit these as needed:

# directory containing postgres executable:
PGBINDIR="/usr/local/pgsql/bin"
# data directory:
PGDATA="/usr/local/pgsql/data"
# file to receive postmaster's initial log messages:
PGLOGFILE="${PGDATA}/pgstart.log"

# (it's recommendable to enable the Postgres logging_collector feature
# so that PGLOGFILE doesn't grow without bound)


# set umask to ensure PGLOGFILE is not created world-readable
umask 077

# wait for networking to be up (else server may not bind to desired ports)
/usr/sbin/ipconfig waitall

# and launch the server
exec "$PGBINDIR"/postgres -D "$PGDATA" >>"$PGLOGFILE" 2>&1
