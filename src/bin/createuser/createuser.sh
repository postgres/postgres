#!/bin/sh
#-------------------------------------------------------------------------
#
# createuser.sh--
#    utility for creating a user in the POSTGRES database
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/createuser/Attic/createuser.sh,v 1.10 1998/08/22 05:19:17 momjian Exp $
#
# Note - this should NOT be setuid.
#
#-------------------------------------------------------------------------

CMDNAME=`basename $0`
SYSID=
CANADDUSER=
CANCREATE=

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

while [ -n "$1" ]
do
    case $1 in 
	-a) AUTHSYS=$2; shift;;
        -h) PGHOST=$2; shift;;
        -p) PGPORT=$2; shift;;
        -d) CANCREATE=t;;
        -D) CANCREATE=f;;
        -u) CANADDUSER=t;;
        -U) CANADDUSER=f;;
        -i) SYSID=$2; shift;;
         *) NEWUSER=$1;;
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

PARGS="-tq $AUTHOPT $PGHOSTOPT $PGPORTOPT"

#
# generate the first part of the actual monitor command
#

PSQL="psql $PARGS"

#
# see if user $USER is allowed to create new users
#

QUERY="select usesuper from pg_user where usename = '$USER' "
#echo $QUERY

ADDUSER=`$PSQL -c "$QUERY" template1`

if [ $? -ne 0 ]
then
    echo "$CMDNAME: database access failed." 1>&2
    exit 1
fi

if [ -n "$ADDUSER" ]
then

if [ $ADDUSER != "t" ]
then
    echo "$CMDNAME: $USER cannot create users." 1>&2
    exit 1
fi
fi

#
# get the user name of the new user.  Make sure it doesn't already exist.
#

if [ -z "$NEWUSER" ]
then
    echo PG_OPT_DASH_N_PARAM "Enter name of user to add ---> PG_OPT_BACKSLASH_C_PARAM"
    read NEWUSER
fi

QUERY="select usesysid from pg_user where usename = '$NEWUSER' "

RES=`$PSQL -c "$QUERY" template1`

if [ $? -ne 0 ]
then
    echo "$CMDNAME: database access failed." 1>&2
    exit 1
fi

if [ -n "$RES" ]
then
    echo "$CMDNAME: user "\"$NEWUSER\"" already exists" 1>&2
    exit 1
fi

done=0

#
# get the system id of the new user.  Make sure it is unique.
#

while [ $done -ne 1 ]
do
    DEFSYSID=`pg_id $NEWUSER 2>/dev/null`
    if [ $? -eq 0 ]; then
	DEFMSG=" or RETURN to use unix user ID: $DEFSYSID"
    else
	DEFMSG=
	DEFSYSID=
    fi
    while  [ -z "$SYSID" ]
    do
	echo PG_OPT_DASH_N_PARAM "Enter user's postgres ID$DEFMSG -> PG_OPT_BACKSLASH_C_PARAM"
	read SYSID
	[ -z "$SYSID" ] && SYSID=$DEFSYSID;
	SYSIDISNUM=`echo $SYSID | egrep '^[0-9]+$'`
	if [ -z "$SYSIDISNUM" ]
	then
		echo "$CMDNAME: the postgres ID must be a number"
		SYSID=	
	fi
    done
    QUERY="select usename from pg_user where usesysid = '$SYSID'::int4"
    RES=`$PSQL -c "$QUERY" template1`	
    if [ $? -ne 0 ]
    then
	echo "$CMDNAME: database access failed."
	exit 1
    fi
    if [ -n "$RES" ]
    then
	echo 
	echo "$CMDNAME: $SYSID already belongs to $RES, pick another"
	DEFMSG= DEFSYSID= SYSID=
    else
	done=1
    fi
done

#
# get the rest of the user info...
#

#
# can the user create databases?
#
if [ -z "$CANCREATE" ]
then
	yn=f

	while [ "$yn" != y -a "$yn" != n ]
	do
		echo PG_OPT_DASH_N_PARAM "Is user \"$NEWUSER\" allowed to create databases (y/n) PG_OPT_BACKSLASH_C_PARAM"
		read yn
	done

	if [ "$yn" = y ]
	then
		CANCREATE=t
	else
		CANCREATE=f
	fi
fi

#
# can the user add users?
#

if [ -z "$CANADDUSER" ]
then
	yn=f

	while [ "$yn" != y -a "$yn" != n ]
	do
		echo PG_OPT_DASH_N_PARAM "Is user \"$NEWUSER\" allowed to add users? (y/n) PG_OPT_BACKSLASH_C_PARAM"
		read yn
	done

	if (test "$yn" = y)
	then
		CANADDUSER=t
	else
		CANADDUSER=f
	fi
fi

QUERY="insert into pg_shadow \
        (usename, usesysid, usecreatedb, usetrace, usesuper, usecatupd) \
       values \
         ('$NEWUSER', $SYSID, '$CANCREATE', 't', '$CANADDUSER','t')"

RES=`$PSQL -c "$QUERY" template1`

#
# Wrap things up.  If the user was created successfully, AND the user was
# NOT allowed to create databases, remind the DBA to create one for the user.
#

if [ $? -ne 0 ]
then
    echo "$CMDNAME: $NEWUSER was NOT added successfully"
else
    echo "$CMDNAME: $NEWUSER was successfully added"
    if [ "$CANCREATE" = f ]
    then
        echo "don't forget to create a database for $NEWUSER"
    fi
fi
