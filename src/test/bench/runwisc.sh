#!/bin/sh
# $PostgreSQL: pgsql/src/test/bench/runwisc.sh,v 1.10 2007/08/19 01:41:25 adunstan Exp $

if [ ! -d $1 ]; then
        echo " you must specify a valid data directory " >&2
        exit
fi

if [ -d ./obj ]; then
	cd ./obj
fi

echo =============== vacuuming benchmark database... ================= >&2
echo "vacuum" | postgres -D"$1" bench > /dev/null

echo =============== running benchmark... ================= >&2
time postgres -D"$1" -texecutor -tplanner -c log_min_messages=log -c log_destination=stderr -c start_log_collector=off bench < bench.sql 2>&1
