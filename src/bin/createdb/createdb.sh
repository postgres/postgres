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
#    $Header: /cvsroot/pgsql/src/bin/createdb/Attic/createdb.sh,v 1.7 1997/11/07 06:25:25 thomas Exp $
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
		--help) usage=1;;

		-a) AUTHSYS=$2; shift;;
		-h) PGHOST=$2; shift;;
		-p) PGPORT=$2; shift;;
		-D) dbpath=$2; shift;;
		-*) echo "$CMDNAME: unrecognized parameter $1"; usage=1;;
		 *) dbname=$1;;
	esac
	shift;
done

if [ "$usage" ]; then
	echo "Usage: $CMDNAME -a <authtype> -h <server> -p <portnumber> -D <location> [dbname]"
	exit 1
fi

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

if [ -z "$dbpath" ]; then
	location=""
else
#	if [ ! -d "$dbpath"/base ]; then
#		echo "$CMDNAME: database creation failed on $dbname."
#		echo "directory $dbpath/base not found."
#		exit 1
#	fi
	location="with location = '$dbpath'"
fi

psql -tq $AUTHOPT $PGHOSTOPT $PGPORTOPT -c "create database $dbname $location" template1

if [ $? -ne 0 ]; then
	echo "$CMDNAME: database creation failed on $dbname."
	exit 1
fi

exit 0
