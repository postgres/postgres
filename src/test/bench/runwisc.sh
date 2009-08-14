#!/bin/sh
# $PostgreSQL: pgsql/src/test/bench/runwisc.sh,v 1.11 2009/08/14 18:49:34 tgl Exp $

if [ ! -d $1 ]; then
        echo " you must specify a valid data directory " >&2
        exit
fi

if [ -d ./obj ]; then
	cd ./obj
fi

echo =============== vacuuming benchmark database... ================= >&2
echo "vacuum" | postgres --single -D"$1" bench > /dev/null

echo =============== running benchmark... ================= >&2
time postgres --single -D"$1" -texecutor -tplanner -c log_min_messages=log -c log_destination=stderr -c logging_collector=off bench < bench.sql 2>&1
