#!/bin/sh

# contrib/pg_upgrade/test.sh
#
# Test driver for pg_upgrade.  Initializes a new database cluster,
# runs the regression tests (to put in some data), runs pg_dumpall,
# runs pg_upgrade, runs pg_dumpall again, compares the dumps.
#
# Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California

set -e

: ${MAKE=make}

# Guard against parallel make issues (see comments in pg_regress.c)
unset MAKEFLAGS
unset MAKELEVEL

# Set listen_addresses desirably
testhost=`uname -s`

case $testhost in
	MINGW*)	LISTEN_ADDRESSES="localhost" ;;
	*)		LISTEN_ADDRESSES="" ;;
esac

POSTMASTER_OPTS="-F -c listen_addresses=$LISTEN_ADDRESSES"

temp_root=$PWD/tmp_check

if [ "$1" = '--install' ]; then
	temp_install=$temp_root/install
	bindir=$temp_install/$bindir
	libdir=$temp_install/$libdir

	"$MAKE" -s -C ../.. install DESTDIR="$temp_install"
	"$MAKE" -s -C ../pg_upgrade_support install DESTDIR="$temp_install"
	"$MAKE" -s -C . install DESTDIR="$temp_install"

	# platform-specific magic to find the shared libraries; see pg_regress.c
	LD_LIBRARY_PATH=$libdir:$LD_LIBRARY_PATH
	export LD_LIBRARY_PATH
	DYLD_LIBRARY_PATH=$libdir:$DYLD_LIBRARY_PATH
	export DYLD_LIBRARY_PATH
	LIBPATH=$libdir:$LIBPATH
	export LIBPATH
	PATH=$libdir:$PATH

	# We need to make it use psql from our temporary installation,
	# because otherwise the installcheck run below would try to
	# use psql from the proper installation directory, which might
	# be outdated or missing. But don't override anything else that's
	# already in EXTRA_REGRESS_OPTS.
	EXTRA_REGRESS_OPTS="$EXTRA_REGRESS_OPTS --psqldir=$bindir"
	export EXTRA_REGRESS_OPTS
fi

: ${oldbindir=$bindir}

: ${oldsrc=../..}
oldsrc=`cd "$oldsrc" && pwd`
newsrc=`cd ../.. && pwd`

PATH=$bindir:$PATH
export PATH

BASE_PGDATA=$temp_root/data
PGDATA="$BASE_PGDATA.old"
export PGDATA
rm -rf "$BASE_PGDATA" "$PGDATA"

logdir=$PWD/log
rm -rf "$logdir"
mkdir "$logdir"

# Clear out any environment vars that might cause libpq to connect to
# the wrong postmaster (cf pg_regress.c)
unset PGDATABASE
unset PGUSER
unset PGSERVICE
unset PGSSLMODE
unset PGREQUIRESSL
unset PGCONNECT_TIMEOUT
unset PGHOST
unset PGHOSTADDR

# Select a non-conflicting port number, similarly to pg_regress.c
PG_VERSION_NUM=`grep '#define PG_VERSION_NUM' $newsrc/src/include/pg_config.h | awk '{print $3}'`
PGPORT=`expr $PG_VERSION_NUM % 16384 + 49152`
export PGPORT

i=0
while psql -X postgres </dev/null 2>/dev/null
do
	i=`expr $i + 1`
	if [ $i -eq 16 ]
	then
		echo port $PGPORT apparently in use
		exit 1
	fi
	PGPORT=`expr $PGPORT + 1`
	export PGPORT
done

# buildfarm may try to override port via EXTRA_REGRESS_OPTS ...
EXTRA_REGRESS_OPTS="$EXTRA_REGRESS_OPTS --port=$PGPORT"
export EXTRA_REGRESS_OPTS

# enable echo so the user can see what is being executed
set -x

$oldbindir/initdb -N
$oldbindir/pg_ctl start -l "$logdir/postmaster1.log" -o "$POSTMASTER_OPTS" -w
if "$MAKE" -C "$oldsrc" installcheck; then
	pg_dumpall -f "$temp_root"/dump1.sql || pg_dumpall1_status=$?
	if [ "$newsrc" != "$oldsrc" ]; then
		oldpgversion=`psql -A -t -d regression -c "SHOW server_version_num"`
		fix_sql=""
		case $oldpgversion in
			804??)
				fix_sql="UPDATE pg_proc SET probin = replace(probin::text, '$oldsrc', '$newsrc')::bytea WHERE probin LIKE '$oldsrc%'; DROP FUNCTION public.myfunc(integer);"
				;;
			900??)
				fix_sql="SET bytea_output TO escape; UPDATE pg_proc SET probin = replace(probin::text, '$oldsrc', '$newsrc')::bytea WHERE probin LIKE '$oldsrc%';"
				;;
			901??)
				fix_sql="UPDATE pg_proc SET probin = replace(probin, '$oldsrc', '$newsrc') WHERE probin LIKE '$oldsrc%';"
				;;
		esac
		psql -d regression -c "$fix_sql;" || psql_fix_sql_status=$?

		mv "$temp_root"/dump1.sql "$temp_root"/dump1.sql.orig
		sed "s;$oldsrc;$newsrc;g" "$temp_root"/dump1.sql.orig >"$temp_root"/dump1.sql
	fi
else
	make_installcheck_status=$?
fi
$oldbindir/pg_ctl -m fast stop
if [ -n "$make_installcheck_status" ]; then
	exit 1
fi
if [ -n "$psql_fix_sql_status" ]; then
	exit 1
fi
if [ -n "$pg_dumpall1_status" ]; then
	echo "pg_dumpall of pre-upgrade database cluster failed"
	exit 1
fi

PGDATA=$BASE_PGDATA

initdb -N

pg_upgrade $PG_UPGRADE_OPTS -d "${PGDATA}.old" -D "${PGDATA}" -b "$oldbindir" -B "$bindir" -p "$PGPORT" -P "$PGPORT"

pg_ctl start -l "$logdir/postmaster2.log" -o "$POSTMASTER_OPTS" -w

case $testhost in
	MINGW*)	cmd /c analyze_new_cluster.bat ;;
	*)		sh ./analyze_new_cluster.sh ;;
esac

pg_dumpall -f "$temp_root"/dump2.sql || pg_dumpall2_status=$?
pg_ctl -m fast stop

# no need to echo commands anymore
set +x
echo

if [ -n "$pg_dumpall2_status" ]; then
	echo "pg_dumpall of post-upgrade database cluster failed"
	exit 1
fi

case $testhost in
	MINGW*)	cmd /c delete_old_cluster.bat ;;
	*)	    sh ./delete_old_cluster.sh ;;
esac

if diff -q "$temp_root"/dump1.sql "$temp_root"/dump2.sql; then
	echo PASSED
	exit 0
else
	echo "dumps were not identical"
	exit 1
fi
