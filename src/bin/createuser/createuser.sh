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
#    $Header: /cvsroot/pgsql/src/bin/createuser/Attic/createuser.sh,v 1.7 1996/11/17 03:54:54 bryanh Exp $
#
# Note - this should NOT be setuid.
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

while [ -n "$1" ]
do
    case $1 in 
	-a) AUTHSYS=$2; shift;;
        -h) PGHOST=$2; shift;;
        -p) PGPORT=$2; shift;;
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
    echo _fUnKy_DASH_N_sTuFf_ "Enter name of user to add ---> "_fUnKy_BACKSLASH_C_sTuFf_
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
    SYSID=
    DEFSYSID=`pg_id $NEWUSER 2>/dev/null`
    if [ $? -eq 0 ]; then
	DEFMSG=" or RETURN to use unix user ID: $DEFSYSID"
    else
	DEFMSG=
	DEFSYSID=
    fi
    while  [ -z "$SYSID" ]
    do
	echo _fUnKy_DASH_N_sTuFf_ "Enter user's postgres ID$DEFMSG -> "_fUnKy_BACKSLASH_C_sTuFf_
	read SYSID
	[ -z "$SYSID" ] && SYSID=$DEFSYSID;
	SYSIDISNUM=`echo $SYSID | egrep '^[0-9]+$'`
	if [ -z "$SYSIDISNUM" ]
	then
		echo "$CMDNAME: the postgres ID must be a number"
		exit 1
	fi
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
done

#
# get the rest of the user info...
#

#
# can the user create databases?
#

yn=f

while [ "$yn" != y -a "$yn" != n ]
do
    echo _fUnKy_DASH_N_sTuFf_ "Is user \"$NEWUSER\" allowed to create databases (y/n) "_fUnKy_BACKSLASH_C_sTuFf_
    read yn
done

if [ "$yn" = y ]
then
    CANCREATE=t
else
    CANCREATE=f
fi

#
# can the user add users?
#

yn=f

while [ "$yn" != y -a "$yn" != n ]
do
    echo _fUnKy_DASH_N_sTuFf_ "Is user \"$NEWUSER\" allowed to add users? (y/n) "_fUnKy_BACKSLASH_C_sTuFf_
    read yn
done

if (test "$yn" = y)
then
    CANADDUSER=t
else
    CANADDUSER=f
fi

QUERY="insert into pg_user \
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
