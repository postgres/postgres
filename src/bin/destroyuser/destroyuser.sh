#!/bin/sh
#-------------------------------------------------------------------------
#
# destroyuser.sh--
#    utility for destroying a user from the POSTGRES database.
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/destroyuser/Attic/destroyuser.sh,v 1.7 1997/05/07 02:59:52 scrappy Exp $
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

while (test -n "$1")
do
    case $1 in 
	-a) AUTHSYS=$2; shift;;
        -h) PGHOST=$2; shift;;
        -p) PGPORT=$2; shift;;
         *) DELUSER=$1;;
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
# see if user $USER is allowed to create new users.  Only a user who can
# create users can delete them.
#

QUERY="select usesuper from pg_user where usename = '$USER'"
ADDUSER=`$PSQL -c "$QUERY" template1`

if [ $? -ne 0 ]
then
    echo "$CMDNAME: database access failed."
    exit 1
fi

if [ $ADDUSER != "t" ]
then
    echo "$CMDNAME: $USER cannot delete users."
fi

#
# get the user name of the user to delete.  Make sure it exists.
#

if [ -z "$DELUSER" ]
then
    echo _fUnKy_DASH_N_sTuFf_ "Enter name of user to delete ---> _fUnKy_BACKSLASH_C_sTuFf_"
    read DELUSER
fi

QUERY="select usesysid from pg_user where usename = '$DELUSER'"

RES=`$PSQL -c "$QUERY" template1`

if [ $? -ne 0 ]
then
    echo "$CMDNAME: database access failed."
    exit 1
fi

if [ ! -n "$RES" ]
then
    echo "$CMDNAME: user "\"$DELUSER\"" does not exist."
    exit 1
fi

SYSID=`echo $RES | sed 's/ //g'`

#
# destroy the databases owned by the deleted user.  First, use this query
# to find out what they are.
#

QUERY="select datname from pg_database where datdba = '$SYSID'::oid"
       

ALLDBS=`$PSQL -c "$QUERY" template1`

if [ $? -ne 0 ]
then
    echo "$CMDNAME: database access failed - exiting..."
    exit 1
fi


#
# don't try to delete template1!
#

for i in $ALLDBS
do
    if [ $i != "template1" ]
    then
        DBLIST="$DBLIST $i"
    fi
done

if [ -n "$DBLIST" ]
then
    echo "User $DELUSER owned the following databases:"
    echo $DBLIST
    echo

#
# Now we warn the DBA that deleting this user will destroy a bunch of databases
#

    yn=f
    while [ $yn != y -a $yn != n ]
    do
        echo _fUnKy_DASH_N_sTuFf_ "Deleting user $DELUSER will destroy them. Continue (y/n)? _fUnKy_BACKSLASH_C_sTuFf_"
        read yn
    done

    if [ $yn = n ]
    then
        echo "$CMDNAME: exiting"
        exit 1
    fi

    #
    # now actually destroy the databases
    #

    for i in $DBLIST
    do
        echo "destroying database $i"

        QUERY="drop database $i"
        $PSQL -c "$QUERY" template1
        if [ $? -ne 0 ]
        then
            echo "$CMDNAME: drop database on $i failed - exiting"
            exit 1
        fi
    done
fi

QUERY="delete from pg_user where usename = '$DELUSER'"

$PSQL -c "$QUERY" template1
if [ $? -ne 0 ]
then
    echo "$CMDNAME: delete of user $DELUSER was UNSUCCESSFUL"
else
    echo "$CMDNAME: delete of user $DELUSER was successful."
fi

exit 0
