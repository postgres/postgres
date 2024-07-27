#!/usr/bin

rm -rf /var/postgresql/data/*
initdb -D $PGDATA

echo "max_connections = 1000" >> $PGDATA/postgresql.conf
echo "shared_buffers = '2GB'" >> $PGDATA/postgresql.conf
echo "fsync = 'off'" >> $PGDATA/postgresql.conf
echo "full_page_writes = 'on'" >> $PGDATA/postgresql.conf

pg_ctl start -D $PGDATA
psql -d postgres -c 'create database ycsb';
psql -d postgres -c 'create database sysbench';
pg_ctl stop -D $PGDATA -m fast

