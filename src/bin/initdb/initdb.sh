#!/bin/sh
#-------------------------------------------------------------------------
#
# initdb.sh--
#     Create (initialize) a Postgres database system.  
# 
#     A database system is a collection of Postgres databases all managed
#     by the same postmaster.  
#
#     To create the database system, we create the directory that contains
#     all its data, create the files that hold the global classes, create
#     a few other control files for it, and create one database:  the
#     template database.
#
#     The template database is an ordinary Postgres database.  Its data
#     never changes, though.  It exists to make it easy for Postgres to 
#     create other databases -- it just copies.
#
#     Optionally, we can skip creating the database system and just create
#     (or replace) the template database.
#
#     To create all those classes, we run the postgres (backend) program and
#     feed it data from bki files that are in the Postgres library directory.
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/initdb/Attic/initdb.sh,v 1.66 1999/12/17 01:05:30 momjian Exp $
#
#-------------------------------------------------------------------------

# ----------------
#       The OPT_..._PARAM gets set when the script is built (with make)
#       from parameters set in the make file.
#
# ----------------

CMDNAME=`basename $0`
MULTIBYTEID=0

MULTIBYTE=__MULTIBYTE__
if [  -n "$MULTIBYTE" ];then
	MULTIBYTEID=`pg_encoding $MULTIBYTE`
fi

# Find the default PGLIB directory (the directory that contains miscellaneous 
# files that are part of Postgres).  The user-written program postconfig
# outputs variable settings like "PGLIB=/usr/lib/whatever".  If it doesn't
# output a PGLIB value, then there is no default and the user must
# specify the pglib option.  Postconfig may validly not exist, in which case
# our invocation of it silently fails.

# The 2>/dev/null is to swallow the "postconfig: not found" message if there
# is no postconfig.

postconfig_result="`sh -c postconfig 2>/dev/null`"
if [ ! -z "$postconfig_result" ]; then
  set -a   # Make the following variable assignment exported to environment
  eval "$postconfig_result"
  set +a   # back to normal
fi

# Set defaults:
debug=0
noclean=0
template_only=0
POSTGRES_SUPERUSERNAME=$USER

while [ "$#" -gt 0 ]
do
# ${ARG#--username=} is not reliable or available on all platforms

    case "$1" in
        --debug|-d)
                debug=1
                echo "Running with debug mode on."
                ;;
        --noclean|-n)
                noclean=1
                echo "Running with noclean mode on. "
                     "Mistakes will not be cleaned up."
                ;;
        --template|-t)
                template_only=1
                echo "updating template1 database only."
                ;;
        --username=*)
                POSTGRES_SUPERUSERNAME="`echo $1 | sed 's/^--username=//'`"
                ;;
        -u)
                shift
                POSTGRES_SUPERUSERNAME="$1"
                ;;
        -u*)
                POSTGRES_SUPERUSERNAME="`echo $1 | sed 's/^-u//'`"
                ;;
        --pgdata=*)
                PGDATA="`echo $1 | sed 's/^--pgdata=//'`"
                ;;
        -r)
                shift
                PGDATA="$1"
                ;;
        -r*)
                PGDATA="`echo $1 | sed 's/^-r//'`"
                ;;
        --pglib=*)
                PGLIB="`echo $1 | sed 's/^--pglib=//'`"
                ;;
        -l)
                shift
                PGLIB="$1"
                ;;
        -l*)
                PGLIB="`echo $1 | sed 's/^-l//'`"
                ;;

        --pgencoding=*)
		if [ -z "$MULTIBYTE" ];then
			echo "MULTIBYTE support seems to be disabled"
			exit 100
		fi
                mb="`echo $1 | sed 's/^--pgencoding=//'`"
		MULTIBYTEID=`pg_encoding $mb`
		if [ -z "$MULTIBYTEID" ];then
			echo "$mb is not a valid encoding name"
			exit 100
		fi
                ;;
        -e)
		if [ -z "$MULTIBYTE" ];then
			echo "MULTIBYTE support seems to be disabled"
			exit 100
		fi
                shift
		MULTIBYTEID=`pg_encoding $1`
		if [ -z "$MULTIBYTEID" ];then
			echo "$1 is not a valid encoding name"
			exit 100
		fi
                ;;
        -e*)
		if [ -z "$MULTIBYTE" ];then
			echo "MULTIBYTE support seems to be disabled"
			exit 100
		fi
                mb="`echo $1 | sed 's/^-e//'`"
		MULTIBYTEID=`pg_encoding $mb`
		if [ -z "$MULTIBYTEID" ];then
			echo "$mb is not a valid encoding name"
			exit 100
		fi
                ;;
	--help)
		usage=t
		;;
	-\?)
		usage=t
		;;
        *)
                echo "Unrecognized option '$1'. Try -? for help."
		exit 100
        esac
        shift
done

if [ "$usage" ]; then
	echo ""
	echo "Usage: $CMDNAME [options]"
	echo ""
        echo "    -t,           --template           "
        echo "    -d,           --debug              "
        echo "    -n,           --noclean            "
        echo "    -u SUPERUSER, --username=SUPERUSER " 
        echo "    -r DATADIR,   --pgdata=DATADIR     "
        echo "    -l LIBDIR,    --pglib=LIBDIR       "
	
	if [ -n "$MULTIBYTE" ]; then 
		echo "    -e ENCODING,  --pgencoding=ENCODING"
        fi
	
	echo "    -?,           --help               "           	
	echo ""	

	exit 100
fi

#-------------------------------------------------------------------------
# Make sure he told us where to find the Postgres files.
#-------------------------------------------------------------------------
if [ -z "$PGLIB" ]; then
    echo "$CMDNAME does not know where to find the files that make up "
    echo "Postgres (the PGLIB directory).  You must identify the PGLIB "
    echo "directory either with a --pglib invocation option, or by "
    echo "setting the PGLIB environment variable, or by having a program "
    echo "called 'postconfig' in your search path that outputs an asignment "
    echo "for PGLIB."
    exit 20
fi

#-------------------------------------------------------------------------
# Make sure he told us where to build the database system
#-------------------------------------------------------------------------

if [ -z "$PGDATA" ]; then
    echo "$CMDNAME: You must identify the PGDATA directory, where the data"
    echo "for this database system will reside.  Do this with either a"
    echo "--pgdata invocation option or a PGDATA environment variable."
    echo
    exit 20
fi

TEMPLATE=$PGLIB/local1_template1.bki.source
GLOBAL=$PGLIB/global1.bki.source
TEMPLATE_DESCR=$PGLIB/local1_template1.description
GLOBAL_DESCR=$PGLIB/global1.description
PG_HBA_SAMPLE=$PGLIB/pg_hba.conf.sample
PG_GEQO_SAMPLE=$PGLIB/pg_geqo.sample


#-------------------------------------------------------------------------
# Find the input files
#-------------------------------------------------------------------------

for PREREQ_FILE in $TEMPLATE $GLOBAL $PG_HBA_SAMPLE; do
    if [ ! -f $PREREQ_FILE ]; then 
        echo "$CMDNAME does not find the file '$PREREQ_FILE'."
        echo "This means you have identified an invalid PGLIB directory."
        echo "You specify a PGLIB directory with a --pglib invocation "
        echo "option, a PGLIB environment variable, or a postconfig program."
        exit 1
    fi
done

[ "$debug" -ne 0 ] && echo "$CMDNAME: using $TEMPLATE as input to create the template database."
if [ $template_only -eq 0 ]; then
    [ "$debug" -ne 0 ] && echo "$CMDNAME: using $GLOBAL as input to create the global classes."
    [ "$debug" -ne 0 ] && echo "$CMDNAME: using $PG_HBA_SAMPLE as the host-based authentication" \
         "control file."
    echo
fi  

#---------------------------------------------------------------------------
# Figure out who the Postgres superuser for the new database system will be.
#---------------------------------------------------------------------------

if [ -z "$POSTGRES_SUPERUSERNAME" ]; then 
    echo "Can't tell what username to use.  You don't have the USER"
    echo "environment variable set to your username and didn't specify the "
    echo "--username option"
    exit 1
fi

POSTGRES_SUPERUID=`pg_id $POSTGRES_SUPERUSERNAME`

if [ ${POSTGRES_SUPERUID:=-1} -eq -1 ]; then
    echo "Unable to determine a valid username.  If you are running"
    echo "initdb without an explicit username specified, then there"
    echo "may be a problem with finding the Postgres shared library"
    echo "and/or the pg_id utility."
    exit 10
fi

if [ $POSTGRES_SUPERUID = NOUSER ]; then
    echo "Valid username not given.  You must specify the username for "
    echo "the Postgres superuser for the database system you are "
    echo "initializing, either with the --username option or by default "
    echo "to the USER environment variable."
    exit 10
fi

if [ $POSTGRES_SUPERUID -ne `pg_id` -a `pg_id` -ne 0 ]; then 
    echo "Only the unix superuser may initialize a database with a different"
    echo "Postgres superuser.  (You must be able to create files that belong"
    echo "to the specified unix user)."
    exit 2
fi

echo "We are initializing the database system with username" \
  "$POSTGRES_SUPERUSERNAME (uid=$POSTGRES_SUPERUID)."   
echo "This user will own all the files and must also own the server process."
echo

# -----------------------------------------------------------------------
# Create the data directory if necessary
# -----------------------------------------------------------------------

# umask must disallow access to group, other for files and dirs
umask 077

if [ -f "$PGDATA/PG_VERSION" ]; then
    if [ $template_only -eq 0 ]; then
        echo "$CMDNAME: The file $PGDATA/PG_VERSION already exists."
        echo "This probably means initdb has already been run and the"
        echo "database system already exists."
        echo 
        echo "If you want to create a new database system, either remove"
        echo "the directory $PGDATA or run initdb with a --pgdata argument"
        echo "other than $PGDATA."
        exit 1
    fi
else
    if [ ! -d $PGDATA ]; then
        echo "Creating database system directory $PGDATA"
        mkdir $PGDATA || exit_nicely
    else
        echo "Fixing permissions on pre-existing data directory $PGDATA"
	chmod go-rwx $PGDATA || exit_nicely
    fi

    if [ ! -d $PGDATA/base ]; then
        echo "Creating database system directory $PGDATA/base"
        mkdir $PGDATA/base || exit_nicely
    fi
    if [ ! -d $PGDATA/pg_xlog ]; then
        echo "Creating database XLOG directory $PGDATA/pg_xlog"
        mkdir $PGDATA/pg_xlog || exit_nicely
    fi
fi

#----------------------------------------------------------------------------
# Create the template1 database
#----------------------------------------------------------------------------

rm -rf $PGDATA/base/template1 || exit_nicely
mkdir $PGDATA/base/template1 || exit_nicely

if [ "$debug" -eq 1 ]; then
    BACKEND_TALK_ARG="-d"
else
    BACKEND_TALK_ARG="-Q"
fi

BACKENDARGS="-boot -C -F -D$PGDATA $BACKEND_TALK_ARG"
FIRSTRUN="-boot -x -C -F -D$PGDATA $BACKEND_TALK_ARG"

echo "Creating template database in $PGDATA/base/template1"
[ "$debug" -ne 0 ] && echo "Running: $PGPATH/postgres $FIRSTRUN template1"

cat $TEMPLATE \
| sed -e "s/PGUID/$POSTGRES_SUPERUSERID/g" \
| $PGPATH/postgres $FIRSTRUN template1 \
|| exit_nicely

$PGPATH/pg_version $PGDATA/base/template1 || exit_nicely

#----------------------------------------------------------------------------
# Create the global classes, if requested.
#----------------------------------------------------------------------------

if [ $template_only -eq 0 ]; then
    echo "Creating global relations in $PGDATA/base"
    [ "$debug" -ne 0 ] && echo "Running: $PGPATH/postgres $BACKENDARGS template1"

    cat $GLOBAL \
    | sed -e "s/POSTGRES/$POSTGRES_SUPERUSERNAME/g" \
          -e "s/PGUID/$POSTGRES_SUPERUSERID/g" \
          -e "s/PASSWORD/$Password/g" \
    | $PGPATH/postgres $BACKENDARGS template1 \
    || exit_nicely

    $PGPATH/pg_version $PGDATA || exit_nicely

    cp $PG_HBA_SAMPLE $PGDATA/pg_hba.conf     || exit_nicely
    cp $PG_GEQO_SAMPLE $PGDATA/pg_geqo.sample || exit_nicely

    echo "Adding template1 database to pg_database"

    echo "open pg_database" > $TEMPFILE
    echo "insert (template1 $POSTGRES_SUPERUSERID $MULTIBYTEID template1)" >> $TEMPFILE
    #echo "show" >> $TEMPFILE
    echo "close pg_database" >> $TEMPFILE

    [ "$debug" -ne 0 ] && echo "Running: $PGPATH/postgres $BACKENDARGS template1 < $TEMPFILE"

    $PGPATH/postgres $BACKENDARGS template1 < $TEMPFILE
    # Gotta remove that temp file before exiting on error.
    retval=$?
    if [ $noclean -eq 0 ]; then
            rm -f $TEMPFILE || exit_nicely
    fi
    [ $retval -ne 0 ] && exit_nicely
fi

echo

PGSQL_OPT="-o /dev/null -O -F -Q -D$PGDATA"

# Create a trigger so that direct updates to pg_shadow will be written
# to the flat password file pg_pwd
echo "CREATE TRIGGER pg_sync_pg_pwd AFTER INSERT OR UPDATE OR DELETE ON pg_shadow" \
     "FOR EACH ROW EXECUTE PROCEDURE update_pg_pwd()" \
     | $PGPATH/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

# Create the initial pg_pwd (flat-file copy of pg_shadow)
echo "Writing password file."
echo "COPY pg_shadow TO '$PGDATA/pg_pwd' USING DELIMITERS '\\t'" \
	| $PGPATH/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

# An ordinary COPY will leave the file too loosely protected.
# Note: If you lied above and specified a --username different from the one
# you really are, this will manifest itself in this command failing because
# of a missing file, since the COPY command above failed. It would perhaps
# be better if postgres returned an error code.
chmod go-rw $PGDATA/pg_pwd || exit_nicely

echo "Creating view pg_user."
echo "CREATE VIEW pg_user AS
        SELECT
            usename,
            usesysid,
            usecreatedb,
            usetrace,
            usesuper,
            usecatupd,
            '********'::text as passwd,
            valuntil
        FROM pg_shadow" \
        | $PGPATH/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "REVOKE ALL on pg_shadow FROM public" \
	| $PGPATH/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Creating view pg_rules."
echo "CREATE VIEW pg_rules AS
        SELECT
            C.relname AS tablename,
            R.rulename AS rulename,
	    pg_get_ruledef(R.rulename) AS definition
	FROM pg_rewrite R, pg_class C
	WHERE R.rulename !~ '^_RET'
            AND C.oid = R.ev_class;" \
	| $PGPATH/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Creating view pg_views."
echo "CREATE VIEW pg_views AS
        SELECT
            C.relname AS viewname,
            pg_get_userbyid(C.relowner) AS viewowner,
            pg_get_viewdef(C.relname) AS definition
        FROM pg_class C
        WHERE C.relhasrules
            AND	EXISTS (
                SELECT rulename FROM pg_rewrite R
                    WHERE ev_class = C.oid AND ev_type = '1'
            )" \
	| $PGPATH/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Creating view pg_tables."
echo "CREATE VIEW pg_tables AS
        SELECT
            C.relname AS tablename,
	    pg_get_userbyid(C.relowner) AS tableowner,
	    C.relhasindex AS hasindexes,
	    C.relhasrules AS hasrules,
	    (C.reltriggers > 0) AS hastriggers
        FROM pg_class C
        WHERE C.relkind IN ('r', 's')
            AND NOT EXISTS (
                SELECT rulename FROM pg_rewrite
                    WHERE ev_class = C.oid AND ev_type = '1'
            )" \
	| $PGPATH/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Creating view pg_indexes."
echo "CREATE VIEW pg_indexes AS
        SELECT
            C.relname AS tablename,
	    I.relname AS indexname,
            pg_get_indexdef(X.indexrelid) AS indexdef
        FROM pg_index X, pg_class C, pg_class I
	WHERE C.oid = X.indrelid
            AND I.oid = X.indexrelid" \
        | $PGPATH/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Loading pg_description."
echo "COPY pg_description FROM '$TEMPLATE_DESCR'" \
	| $PGPATH/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "COPY pg_description FROM '$GLOBAL_DESCR'" \
	| $PGPATH/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "Vacuuming database."
echo "VACUUM ANALYZE" \
	| $PGPATH/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo
echo "$CMDNAME completed successfully. You can now start the database server."
echo "($PGPATH/postmaster -D $PGDATA)"
echo

exit 0
