#!/bin/sh
# update
# Script to apply patches to existing databases
#  to upgrade from Postgres v6.0 to v6.1.
echo ""
echo "This utility does a minimal upgrade for existing v6.0 databases."
echo "Note that several new features and functions in Postgres"
echo " will not be available unless the databases are reloaded"
echo " from a clean install of v6.1."
echo ""
echo "This update script is not necessary for new or reloaded databases,"
echo " but will not damage them. You should update existing v6.1beta"
echo " databases created on or before 1997-04-04 (when the patches for"
echo " aggregate functions were applied to the v6.1beta source tree)."
echo ""
echo "Features present with this simple update include:"
echo " - aggregate operators sum() and avg() behave correctly with NULLs"
echo " - the point distance operator '<===>' returns float8 rather than int4"
echo " - some duplicate function OIDs are renumbered to eliminate conflicts"
echo ""
echo "Features unavailable with only this simple update include:"
echo " - new string handling functions a la Oracle/Ingres"
echo " - new date and time data types and expanded functionality"
echo " - some new function overloading to simplify function names"
echo ""
echo "Note that if v6.0 databases are not reloaded from a clean install of v6.1"
echo " or if this update is not applied to existing v6.0 databases:"
echo " - aggregate functions avg() and sum() may divide-by-zero for int4 data types"
#
srcdir=`pwd`
srcsql="update6_0-6_1.sql"
CMDSQL="psql"
SRCSQL="$srcdir/$srcsql"
#
if [ -z $SRCSQL ]; then
	echo "unable to locate $SRCSQL"
	exit 1
fi
#
echo ""
echo "updating databases found in $PGDATA/base"
echo ""
#
cd $PGDATA/base
for d in *
do
	echo "updating $d at `date` ..."
	echo "try $CMDSQL $d < $SRCSQL"
	$CMDSQL $d < $SRCSQL
	echo "completed updating $d at `date`"
done
#
echo ""
echo "completed all updates at `date`"
echo ""
exit
