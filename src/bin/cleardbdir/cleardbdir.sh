#!/bin/sh
#-------------------------------------------------------------------------
#
# cleardbdir.sh--
#    completely clear out the database directory
#
#    this program clears out the database directory, but leaves the .bki
#    files so that initdb(1) can be run again.
#
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/cleardbdir/Attic/cleardbdir.sh,v 1.1.1.1 1996/07/09 06:22:11 scrappy Exp $
#
#-------------------------------------------------------------------------

[ -z "$PGDATA" ] && PGDATA=_fUnKy_DATADIR_sTuFf_

echo "This program completely destroys all the databases in the directory"
echo "$PGDATA"
echo _fUnKy_DASH_N_sTuFf_ "Are you sure you want to do this (y/n) [n]? "_fUnKy_BACKSLASH_C_sTuFf_
read resp || exit
case $resp in
	y*)	: ;;
	*)	exit ;;
esac

cd $PGDATA || exit
for i in *
do
if [ $i != "files" -a $i != "pg_hba" ]
then
	/bin/rm -rf $i
fi
done
