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
#    $Header: /cvsroot/pgsql/src/bin/scripts/Attic/createlang.sh,v 1.3 1999/12/16 20:10:02 momjian Exp $
#
#-------------------------------------------------------------------------

CMDNAME=`basename $0`

PSQLOPT=
dbname=
langname=
echo=
list=

# Check for echo -n vs echo \c

if echo '\c' | grep -s c >/dev/null 2>&1
then
    ECHO_N="echo -n"
    ECHO_C=""
else
    ECHO_N="echo"
    ECHO_C='\c'
fi


# ----------
# Find the default PGLIB directory
# ----------
postconfig_result="`sh -c postconfig 2>/dev/null`"
if [ "$postconfig_result" ]; then
        set -a
        eval "$postconfig_result"
        set +a
fi


# ----------
# Get options, language name and dbname
# ----------
while [ $# -gt 0 ]
do
    case "$1" in 
	--help|-\?)
		usage=t
		;;
        --list|-l)
                list=t
                ;;
# options passed on to psql
	--host|-h)
		PSQLOPT="$PSQLOPT -h $2"
		shift;;
        -h*)
                PSQLOPT="$PSQLOPT $1"
                ;;
        --host=*)
                PSQLOPT="$PSQLOPT -h "`echo $1 | sed 's/^--host=//'`
                ;;
	--port|-p)
		PSQLOPT="$PSQLOPT -p $2"
		shift;;
        -p*)
                PSQLOPT="$PSQLOPT $1"
                ;;
        --port=*)
                PSQLOPT="$PSQLOPT -p "`echo $1 | sed 's/^--port=//'`
                ;;
	--user|--username|-U)
		PSQLOPT="$PSQLOPT -U '$2'"
		shift;;
        -U*)
                PSQLOPT="$PSQLOPT $1"
                ;;
        --user=*)
                PSQLOPT="$PSQLOPT -U "`echo $1 | sed 's/^--user=//'`
                ;;
        --username=*)
                PSQLOPT="$PSQLOPT -U "`echo $1 | sed 's/^--username=//'`
                ;;
	--password|-W)
		PSQLOPT="$PSQLOPT -W"
		;;
	--echo|-e)
                echo=t
		;;
	--dbname|--database|-d)
		dbname="$2"
		shift;;
        -d*)
                dbname=`echo $1 | sed 's/^-d//'`
                ;;
        --dbname=*)
                dbname=`echo $1 | sed 's/^--dbname=//'`
                ;;
        --database=*)
                dbname=`echo $1 | sed 's/^--database=//'`
                ;;
# misc options
	--pglib|-L)
                PGLIB="$2"
                shift;;
        -L*)
                PGLIB=`echo $1 | sed 's/^-L//'`
                ;;
        --pglib=*)
                PGLIB=`echo $1 | sed 's/^--pglib=//'`
                ;;

	 *)
 		langname="$1"
                if [ "$2" ]; then
                        shift
			dbname="$1"
		fi
                ;;
    esac
    shift
done

if [ "$usage" ]; then	
	echo ""
	echo "Usage: $CMDNAME [options] [langname [dbname]]"
	echo ""
	echo "    -h HOSTNAME, --host=HOSTNAME     "
	echo "    -p PORT,     --port=PORT         "
	echo "    -U USERNAME, --username=USERNAME "
	echo "    -W,          --password          "
	echo "    -d DBNAME,   --database=DBNAME   "
	echo "    -e,          --echo              "
        echo "    -q,          --quiet             "   
	echo "    -D PATH,     --location=PATH     "     
	echo "    -L PGLIB     --pglib=PGLIB       "
	echo "    -?,          --help              "
	echo ""
	exit 1
fi

if [ "$list" ]; then
        psql $PSQLOPT -d "$dbname" -c "SELECT lanname, lanpltrusted, lancompiler FROM pg_language WHERE lanispl = 't'"
        exit $?
fi


# ----------
# Check that we have a database
# ----------
if [ -z "$dbname" ]; then
	echo "$CMDNAME: Missing required argument database name. Try -? for help."
	exit 1
fi


# ----------
# Check that we have PGLIB
# ----------
if [ -z "$PGLIB" ]; then
	echo "Cannot determine the PostgreSQL lib directory (PGLIB). You must"
        echo "identify it either with a --pglib option or by setting the PGLIB"
        echo "environment variable."
	exit 1
fi

# ----------
# If not given on the command line, ask for the language
# ----------
if [ -z "$langname" ]; then
	$ECHO_N "Language to install in database $dbname: "$ECHO_C
	read langname
fi

# ----------
# Check if supported and set related values
# ----------
case "$langname" in
	plpgsql)
                lancomp="PL/pgSQL"
		trusted="TRUSTED "
		handler="plpgsql_call_handler"
                ;;
	pltcl)
		lancomp="PL/Tcl"
		trusted="TRUSTED "
		handler="pltcl_call_handler";;
	*)
		echo "$CMDNAME: Unsupported language '$langname'."
		echo "Supported languages are 'plpgsql' and 'pltcl'."
		exit 1
        ;;
esac


# ----------
# Check that the shared object for the call handler is installed
# in PGLIB
# ----------
if [ ! -f $PGLIB/${langname}__DLSUFFIX__ ]; then
	echo "Cannot find the file $PGLIB/${langname}__DLSUFFIX__."
        echo ""
	echo "This file contains the call handler for $lancomp. By default,"
        echo "only PL/pgSQL is built and installed; other languages must be"
        echo "explicitly enabled at configure time."
	echo ""
	echo "To install PL/Tcl, make sure the option --with-tcl is given to"
        echo "configure, then recompile and install."
	exit 1
fi


if [ "$echo" ]; then
        PSQLOPT="$PSQLOPT -e"
else
        PSQLOPT="$PSQLOPT -q"
fi

PSQL="psql -A -t $PSQLOPT -d $dbname -c"

# ----------
# Make sure the language isn't already installed
# ----------
res=`$PSQL "SELECT oid FROM pg_language WHERE lanname = '$langname'"`
if [ $? -ne 0 ]; then
	echo "Language installation failed."
	exit 1
fi
if [ "$res" ]; then
	echo "The language '$langname' is already installed in database $dbname."
	exit 2
fi

# ----------
# Check that there is no function named as the call handler
# ----------
res=`$PSQL "SELECT oid FROM pg_proc WHERE proname = '$handler'"`
if [ ! -z "$res" ]; then
	echo "The language $lancomp isn't created up to now but there is"
        echo "already a function named '$handler' declared."
	echo "Language installation aborted."
	exit 1
fi

# ----------
# Create the call handler and the language
# ----------
$PSQL "CREATE FUNCTION $handler () RETURNS OPAQUE AS '$PGLIB/${langname}__DLSUFFIX__' LANGUAGE 'C'"
if [ $? -ne 0 ]; then
	echo "Language installation failed."
	exit 1
fi
$PSQL "CREATE ${trusted}PROCEDURAL LANGUAGE '$langname' HANDLER $handler LANCOMPILER '$lancomp'"
if [ $? -ne 0 ]; then
	echo "Language installation failed."
	exit 1
fi

exit 0
