#!/bin/sh
# $Header: /cvsroot/pgsql/src/test/bench/Attic/runwisc.sh,v 1.1.1.1 1996/07/09 06:22:23 scrappy Exp $
# 
# Note that in our published benchmark numbers, we executed the command in the
# following fashion:
#
# time $POSTGRES -texecutor -tplanner -f hashjoin -Q bench
#
if [ -d ./obj ]; then
	cd ./obj
fi

echo =============== vacuuming benchmark database... =================
echo "vacuum" | postgres -Q bench > /dev/null

echo =============== running benchmark... =================
time postgres -texecutor -tplanner -Q bench < bench.sql
