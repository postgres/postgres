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
#    $Header: /cvsroot/pgsql/src/bin/createdb/Attic/createdb.sh,v 1.10 1998/07/26 04:31:13 scrappy Exp $
#
#-------------------------------------------------------------------------

CMDNAME=`basename $0`

MBENABLED=__MULTIBYTE__
MB=

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

PASSWDOPT="";

while test -n "$1"
do
	case $1 in
		--help) usage=1;;

		-a) AUTHSYS=$2; shift;;
		-h) PGHOST=$2; shift;;
		-p) PGPORT=$2; shift;;
		-u) PASSWDOPT=$1;;
		-D) dbpath=$2; shift;;
		-E)
			if [ -z "$MBENABLED" ];then
				echo "$CMDNAME: you need to turn on MB compile time option"
				exit 1
			fi
			MB=$2
			MBID=`pg_encoding $MB`
			if [ -z "$MBID" ];then
				echo "$CMDNAME: $MB is not a valid encoding name"
				exit 1
			fi
			shift;;
		-*) echo "$CMDNAME: unrecognized parameter $1"; usage=1;;
		 *) dbname=$1;;
	esac
	shift;
done

if [ "$usage" ]; then
	if [ -z "$MBENABLED" ];then
		echo "Usage: $CMDNAME -a <authtype> -h <server> -p <portnumber> -D <location> [dbname]"
	else
		echo "Usage: $CMDNAME -a <authtype> -h <server> -p <portnumber> -D <location> -E <encoding> [dbname]"
	exit 1
	fi
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
if [ -z "$MBENABLED" -o -z "$MB" ]; then
	encoding=""
else
	encoding="encoding = '$MB'"
	if [ -z "$location" ];then
		encoding="with $encoding"
	fi
fi

psql $PASSWDOPT -tq $AUTHOPT $PGHOSTOPT $PGPORTOPT -c "create database $dbname $location $encoding" template1

if [ $? -ne 0 ]; then
	echo "$CMDNAME: database creation failed on $dbname."
	exit 1
fi

exit 0
