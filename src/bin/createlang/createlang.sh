#!/bin/sh
#-------------------------------------------------------------------------
#
# createlang.sh--
#    Install a procedural language in a database
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/createlang/Attic/createlang.sh,v 1.2 1999/07/09 17:57:46 momjian Exp $
#
#-------------------------------------------------------------------------

CMDNAME=`basename $0`

# ----------
# Find the default PGLIB directory
# ----------
postconfig_result="`sh -c postconfig 2>/dev/null`"
if [ ! -z "$postconfig_result" ]; then
    set -a
	eval "$postconfig_result"
	set +a
fi

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
		--pglib)	PGLIB=$2; shift;;
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
# Check that we have PGLIB
# ----------
if [ -z "$PGLIB" ]; then
	echo "Cannot determine PostgreSQL lib directory (PGLIB)."
	echo "You must identify the PGLIB either with a --pglib option"
	echo "or by setting the PGLIB environment variable."
	exit 1
fi

# ----------
# If not given on the commandline, ask for the language
# ----------
if [ -z "$langname" ]; then
	echo -n "Language to install in database $dbname: "
	read langname
fi

# ----------
# Check if supported and set related values
# ----------
case "$langname" in
	plpgsql)	lancomp="PL/pgSQL"
				trusted="TRUSTED"
				handler="plpgsql_call_handler";;
	pltcl)		lancomp="PL/Tcl"
				trusted="TRUSTED"
				handler="pltcl_call_handler";;
	*)			echo "$CMDNAME: unsupported language '$langname'"
				echo "          supported languages are plpgsql and pltcl"
				exit 1;;
esac

# ----------
# Check that the shared object for the call handler is installed
# in PGLIB
# ----------
if [ ! -f $PGLIB/${langname}__DLSUFFIX__ ]; then
	echo "Cannot find the file $PGLIB/${langname}__DLSUFFIX__"
	echo "This shared object contains the call handler for $lancomp."
	echo "By default, only PL/pgSQL is built and installed. Other"
	echo "languages must be explicitly enabled at configure."
	echo ""
	echo "To install PL/Tcl make sure the option --with-tcl is"
	echo "given to configure, then recompile and install."
	exit 1
fi

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
# Make sure the language isn't already installed
# ----------
res=`$MONITOR "select oid from pg_language where lanname = '$langname'" $dbname`
if [ $? -ne 0 ]; then
	echo "Cannot install language"
	exit 1
fi
if [ ! -z "$res" ]; then
	echo "The language '$langname' is already installed in database $dbname"
	exit 2
fi

# ----------
# Check that there is no function named as the call handler
# ----------
res=`$MONITOR "select oid from pg_proc where proname = '$handler'" $dbname`
if [ ! -z "$res" ]; then
	echo "The language $lancomp isn't created up to now but there"
	echo "is already a function named '$handler' declared."
	echo "Language installation aborted."
	exit 1
fi

# ----------
# Create the call handler and the language
# ----------
$MONITOR "create function $handler () returns opaque as '$PGLIB/${langname}__DLSUFFIX__' language 'C'" $dbname
if [ $? -ne 0 ]; then
	echo "Language installation failed"
	exit 1
fi
$MONITOR "create $trusted procedural language '$langname' handler $handler lancompiler '$lancomp'" $dbname
if [ $? -ne 0 ]; then
	echo "Language installation failed"
	exit 1
fi


exit 0

