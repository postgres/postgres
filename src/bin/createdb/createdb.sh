#!/bin/sh
#-------------------------------------------------------------------------
#
# createdb.sh--
#    create a postgres database
#
#    this program runs the monitor with the "-c" option to create
#    the requested database.
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/createdb/Attic/createdb.sh,v 1.6 1996/11/17 03:54:44 bryanh Exp $
#
#-------------------------------------------------------------------------

CMDNAME=`basename $0`

if [ -z "$USER" ]; then
    if [ -z "$LOGNAME" ]; then
	if [ -z "`whoami`" ]; then
	    echo "$CMDNAME: cannot determine user name"
	    exit 1
	fi
    else
	USER=$LOGNAME
	export USER
    fi
fi

dbname=$USER

while test -n "$1"
do
    case $1 in
	-a) AUTHSYS=$2; shift;;
        -h) PGHOST=$2; shift;;
        -p) PGPORT=$2; shift;;
         *) dbname=$1;;
    esac
    shift;
done

if [ -z "$AUTHSYS" ]; then
  AUTHOPT=""
else
  AUTHOPT="-a $AUTHSYS"
fi

if [ -z "$PGHOST" ]; then
  PGHOSTOPT=""
else
  PGHOSTOPT="-h $PGHOST"
fi

if [ -z "$PGPORT" ]; then
  PGPORTOPT=""
else
  PGPORTOPT="-p $PGPORT"
fi

psql -tq $AUTHOPT $PGHOSTOPT $PGPORTOPT -c "create database $dbname" template1

if [ $? -ne 0 ]; then
    echo "$CMDNAME: database creation failed on $dbname."
    exit 1
fi

exit 0
