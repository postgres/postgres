#!/bin/sh
#-------------------------------------------------------------------------
#
# createlang.sh--
#    Install a procedural language in a database
#
# Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/scripts/Attic/createlang.sh,v 1.23 2001/02/18 18:34:01 momjian Exp $
#
#-------------------------------------------------------------------------

CMDNAME=`basename $0`
PATHNAME=`echo $0 | sed "s,$CMDNAME\$,,"`

PSQLOPT=
dbname=
langname=
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
# Get options, language name and dbname
# ----------
while [ $# -gt 0 ]
do
    case "$1" in 
	--help|-\?)
		usage=t
                break
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
	--username|-U)
		PSQLOPT="$PSQLOPT -U $2"
		shift;;
        -U*)
                PSQLOPT="$PSQLOPT $1"
                ;;
        --username=*)
                PSQLOPT="$PSQLOPT -U "`echo $1 | sed 's/^--username=//'`
                ;;
	--password|-W)
		PSQLOPT="$PSQLOPT -W"
		;;
	--dbname|-d)
		dbname="$2"
		shift;;
        -d*)
                dbname=`echo "$1" | sed 's/^-d//'`
                ;;
        --dbname=*)
                dbname=`echo "$1" | sed 's/^--dbname=//'`
                ;;
# misc options
	--pglib|-L)
                PGLIB="$2"
                shift;;
        -L*)
                PGLIB=`echo "$1" | sed 's/^-L//'`
                ;;
        --pglib=*)
                PGLIB=`echo "$1" | sed 's/^--pglib=//'`
                ;;

	-*)
		echo "$CMDNAME: invalid option: $1" 1>&2
                echo "Try '$CMDNAME --help' for more information." 1>&2
		exit 1
		;;
	 *)
 		if [ "$list" != "t" ]
		then	langname="$1"
			if [ "$2" ]
			then
				shift
				dbname="$1"
			fi
		else	dbname="$1"
		fi
                ;;
    esac
    shift
done

if [ "$usage" ]; then	
        echo "$CMDNAME installs a procedural language into a PostgreSQL database."
	echo
	echo "Usage:"
        echo "  $CMDNAME [options] [langname] dbname"
        echo
	echo "Options:"
	echo "  -h, --host=HOSTNAME             Database server host"
	echo "  -p, --port=PORT                 Database server port"
	echo "  -U, --username=USERNAME         Username to connect as"
	echo "  -W, --password                  Prompt for password"
	echo "  -d, --dbname=DBNAME             Database to install language in"
	echo "  -L, --pglib=DIRECTORY           Find language interpreter file in DIRECTORY"
	echo "  -l, --list                      Show a list of currently installed languages"
        echo
        echo "If 'langname' is not specified, you will be prompted interactively."
        echo "A database name must be specified."
        echo
	echo "Report bugs to <pgsql-bugs@postgresql.org>."
	exit 0
fi


# ----------
# Check that we have a database
# ----------
if [ -z "$dbname" ]; then
	echo "$CMDNAME: missing required argument database name" 1>&2
        echo "Try '$CMDNAME -?' for help." 1>&2
	exit 1
fi


# ----------
# List option
# ----------
if [ "$list" ]; then
        ${PATHNAME}psql $PSQLOPT -d "$dbname" -P 'title=Procedural languages' -c "SELECT lanname as \"Name\", lanpltrusted as \"Trusted?\", lancompiler as \"Compiler\" FROM pg_language WHERE lanispl = 't'"
        exit $?
fi


# ----------
# Check that we have PGLIB
# ----------
if [ -z "$PGLIB" ]; then
	PGLIB='@libdir@'
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
		object="plpgsql"
		;;
	pltcl)
		lancomp="PL/Tcl"
		trusted="TRUSTED "
		handler="pltcl_call_handler"
		object="pltcl"
		;;
	pltclu)
		lancomp="PL/Tcl (untrusted)"
		trusted=""
		handler="pltclu_call_handler"
		object="pltcl"
		;;
	plperl)
		lancomp="PL/Perl"
		trusted="TRUSTED "
		handler="plperl_call_handler"
		object="plperl"
		;;
	*)
		echo "$CMDNAME: unsupported language '$langname'" 1>&2
		echo "Supported languages are 'plpgsql', 'pltcl', 'pltclu', and 'plperl'." 1>&2
		exit 1
        ;;
esac

DLSUFFIX='@DLSUFFIX@'

# ----------
# Check that the shared object for the call handler is installed
# in PGLIB
# ----------
if [ ! -f "$PGLIB/$object$DLSUFFIX" ]; then
      (
	echo "$CMDNAME: cannot find the file '$PGLIB/$langname$DLSUFFIX'"
        echo ""
	echo "This file contains the call handler for $lancomp.  By default,"
        echo "only PL/pgSQL is built and installed; other languages must be"
        echo "explicitly enabled at configure time."
	echo ""
	echo "To install PL/Tcl, make sure the option --with-tcl is given to"
        echo "configure, then recompile and install.  To install PL/Perl use"
        echo "--with-perl."
      ) 1>&2
	exit 1
fi


PSQL="${PATHNAME}psql -A -t -q $PSQLOPT -d $dbname -c"

# ----------
# Make sure the language isn't already installed
# ----------
res=`$PSQL "SELECT oid FROM pg_language WHERE lanname = '$langname'"`
if [ $? -ne 0 ]; then
	echo "$CMDNAME: external error" 1>&2
	exit 1
fi
if [ "$res" ]; then
	echo "$CMDNAME: '$langname' is already installed in database $dbname" 1>&2
	# separate exit status for "already installed"
	exit 2
fi

# ----------
# Check that there is no function named as the call handler
# ----------
res=`$PSQL "SELECT oid FROM pg_proc WHERE proname = '$handler'"`
if [ ! -z "$res" ]; then
	echo "$CMDNAME: A function named '$handler' already exists. Installation aborted." 1>&2
	exit 1
fi

# ----------
# Create the call handler and the language
# ----------
$PSQL "CREATE FUNCTION $handler () RETURNS OPAQUE AS '$PGLIB/${object}$DLSUFFIX' LANGUAGE 'C'"
if [ $? -ne 0 ]; then
	echo "$CMDNAME: language installation failed" 1>&2
	exit 1
fi

$PSQL "CREATE ${trusted}PROCEDURAL LANGUAGE '$langname' HANDLER $handler LANCOMPILER '$lancomp'"
if [ $? -ne 0 ]; then
	echo "$CMDNAME: language installation failed" 1>&2
	exit 1
fi

exit 0
