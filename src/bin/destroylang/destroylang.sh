#!/bin/sh
#-------------------------------------------------------------------------
#
# createlang.sh--
#    Remove a procedural language from a database
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/destroylang/Attic/destroylang.sh,v 1.1 1999/05/20 16:50:03 wieck Exp $
#
#-------------------------------------------------------------------------

CMDNAME=`basename $0`

# ----------
# Determine username
# ----------
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

# ----------
# Get options, language name and dbname
# ----------
dbname=$USER
while [ -n "$1" ]
do
	case $1 in 
		-a) 		AUTHSYS=$2; shift;;
		-h) 		PGHOST=$2; shift;;
		-p) 		PGPORT=$2; shift;;
		 *) 		langname=$1
		 			if [ -n "$2" ]; then
						shift
						dbname=$1
					fi;;
	esac
	shift;
done

# ----------
# If not given on the commandline, ask for the language
# ----------
if [ -z "$langname" ]; then
	echo -n "Language to remove from database $dbname: "
	read langname
fi

# ----------
# Check if supported and set related values
# ----------
case "$langname" in
	plpgsql)	lancomp="PL/pgSQL"
				handler="plpgsql_call_handler";;
	pltcl)		lancomp="PL/Tcl"
				handler="pltcl_call_handler";;
	*)			echo "$CMDNAME: unsupported language '$langname'"
				echo "          supported languages are plpgsql and pltcl"
				exit 1;;
esac

# ----------
# Combine psql with options given
# ----------
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

MONITOR="psql -tq $AUTHOPT $PGHOSTOPT $PGPORTOPT -c"

# ----------
# Make sure the language is installed
# ----------
res=`$MONITOR "select oid from pg_language where lanname = '$langname'" $dbname`
if [ $? -ne 0 ]; then
	echo "Cannot remove language"
	exit 1
fi
if [ -z "$res" ]; then
	echo "The language '$langname' isn't installed in database $dbname"
	exit 1
fi


# ----------
# Check that there are no functions left defined in that language
# ----------
res=`$MONITOR "select count(proname) from pg_proc P, pg_language L where P.prolang = L.oid and L.lanname = '$langname'" $dbname`
if [ $? -ne 0 ]; then
	echo "Cannot remove language"
	exit 1
fi
if [ $res -ne 0 ]; then
	echo "There are $res functions/trigger procedures actually declared"
	echo "in language $lancomp."
	echo "Language not removed."
	exit 1
fi

# ----------
# Drop the language and the call handler function
# ----------
$MONITOR "drop procedural language '$langname'" $dbname
if [ $? -ne 0 ]; then
	echo "Language removal failed"
	exit 1
fi
$MONITOR "drop function $handler()" $dbname
if [ $? -ne 0 ]; then
	echo "Language removal failed"
	exit 1
fi

exit 0

