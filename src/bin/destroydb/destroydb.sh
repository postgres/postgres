#!/bin/sh
#-------------------------------------------------------------------------
#
# destroydb.sh--
#    destroy a postgres database
#
#    this program runs the monitor with the ? option to destroy
#    the requested database.
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/destroydb/Attic/destroydb.sh,v 1.4 1996/09/21 06:24:24 scrappy Exp $
#
#-------------------------------------------------------------------------

# ----------------
#       Set paths from environment or default values.
#       The _fUnKy_..._sTuFf_ gets set when the script is installed
#       from the default value for this build.
#       Currently the only thing we look for from the environment is
#       PGDATA, PGHOST, and PGPORT
#
# ----------------
[ -z "$PGPORT" ] && PGPORT=_fUnKy_POSTPORT_sTuFf_
[ -z "$PGHOST" ] && PGHOST=localhost
BINDIR=_fUnKy_BINDIR_sTuFf_
PATH=$BINDIR:$PATH

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

while [ -n "$1" ]
do
	case $1 in 
		-a) AUTHSYS=$2; shift;;
		-h) PGHOST=$2; shift;;
		-p) PGPORT=$2; shift;;
		 *) dbname=$1;;
	esac
	shift;
done

AUTHOPT="-a $AUTHSYS"
[ -z "$AUTHSYS" ] && AUTHOPT=""

psql -tq -h $PGHOST -p $PGPORT -c "drop database $dbname" template1

if [ $? -ne 0 ]
then
	echo "$CMDNAME: database destroy failed on $dbname."
	exit 1
fi

exit 0
