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
#    $Header: /cvsroot/pgsql/src/bin/initdb/Attic/initdb.sh,v 1.67 1999/12/17 01:16:03 momjian Exp $
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
        *)
                echo "Unrecognized option '$1'.  Syntax is:"
		if [ -z "$MULTIBYTE" ];then
                echo "initdb [-t | --template] [-d | --debug]" \
                     "[-n | --noclean]" \
                     "[-u SUPERUSER | --username=SUPERUSER]" \
                     "[-r DATADIR | --pgdata=DATADIR]" \
                     "[-l LIBDIR | --pglib=LIBDIR]"
		else
                echo "initdb [-t | --template] [-d | --debug]" \
                     "[-n | --noclean]" \
                     "[-u SUPERUSER | --username=SUPERUSER]" \
                     "[-r DATADIR | --pgdata=DATADIR]" \
                     "[-l LIBDIR | --pglib=LIBDIR]" \
                     "[-e ENCODING | --pgencoding=ENCODING]"
		fi
                exit 100
        esac
        shift
done

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
- 26,318 ----
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/initdb/Attic/initdb.sh,v 1.67 1999/12/17 01:16:03 momjian Exp $
#
#-------------------------------------------------------------------------

function exit_nicely () {
    echo
    echo "$CMDNAME failed."
    if [ $noclean -eq 0 ]; then
        echo "Removing $PGDATA."
        rm -rf $PGDATA || echo "Failed."
    else
        echo "Data directory $PGDATA will not be removed at user's request."
    fi
    exit 1
}


CMDNAME=`basename $0`
if [ $EUID -eq 0 ]; then
    echo "You cannot run $CMDNAME as root. Please log in (using, e.g., 'su')"
    echo "as the (unprivileged) user that will own the server process."
    exit 1
fi

EffectiveUser=`id -n -u 2> /dev/null` || EffectiveUser=`whoami 2> /dev/null`
TEMPFILE="/tmp/initdb.$$"

#
# Find out where we're located
#
if echo "$0" | grep -s '/' >& /dev/null ; then
        # explicit dir name given
        PGPATH=`echo $0 | sed 's,/[^/]*$,,'`       # (dirname command is not portable)
else
        # look for it in PATH ('which' command is not portable)
        for dir in `echo $PATH | sed 's/:/ /g'` ; do
                # empty entry in path means current dir
                [ -z "$dir" ] && dir='.'
                if [ -f "$dir/$CMDNAME" ]; then
                        PGPATH="$dir"
                        break
                fi
        done
fi

# Check if needed programs actually exist in path
for prog in postgres pg_version ; do
        if [ ! -x "$PGPATH/$prog" ]; then
                echo "The program $prog needed by $CMDNAME could not be found. It was"
                echo "expected at:"
                echo "    $PGPATH/$prog"
                echo "If this is not the correct directory, please start $CMDNAME"
                echo "with a full search path. Otherwise make sure that the program"
                echo "was installed successfully."
                exit 1
        fi
done

# 0 is the default (non-)encoding
MULTIBYTEID=0
# This is placed here by configure --with-mb=XXX.
MULTIBYTE=__MULTIBYTE__

# Set defaults:
debug=0
noclean=0
template_only=0


# Note: There is a single compelling reason that the name of the database
#       superuser be the same as the Unix user owning the server process:
#       The single user postgres backend will only connect as the database
#       user with the same name as the Unix user running it. That's
#       a security measure. It might change in the future (why?), but for
#       now the --username option is only a fallback if both id and whoami
#       fail, and in that case the argument _must_ be the name of the effective
#       user.
POSTGRES_SUPERUSERNAME=$EffectiveUser

# Note: The sysid can be freely selected. This will probably confuse matters,
#       but if your Unix user postgres is uid 48327 you might chose to start
#       at 0 (or 1) in the database.
POSTGRES_SUPERUSERID=$EUID

Password='_null_'

while [ $# -gt 0 ]
do
    case "$1" in
        --help|-\?)
                usage=t
                break
                ;;
        --debug|-d)
                debug=1
                echo "Running with debug mode on."
                ;;
        --noclean|-n)
                noclean=1
                echo "Running with noclean mode on. Mistakes will not be cleaned up."
                ;;
        --template|-t)
                template_only=1
                echo "Updating template1 database only."
                ;;
# The database superuser. See comments above.
        --username|-u)
                POSTGRES_SUPERUSERNAME="$2"
                shift;;
        --username=*)
                POSTGRES_SUPERUSERNAME=`echo $1 | sed 's/^--username=//'`
                ;;
        -u*)
                POSTGRES_SUPERUSERNAME=`echo $1 | sed 's/^-u//'`
                ;;
# The sysid of the database superuser. See comments above.
        --sysid|-i)
                POSTGRES_SUPERUSERID="$2"
                shift;;
        --sysid=*)
                POSTGRES_SUPERUSERID=`echo $1 | sed 's/^--sysid=//'`
                ;;
        -i*)
                POSTGRES_SUPERUSERID=`echo $1 | sed 's/^-i//'`
                ;;
# The default password of the database superuser.
        --password|-W)
                Password="$2"
                shift;;
        --password=*)
                Password=`echo $1 | sed 's/^--password=//'`
                ;;
        -W*)
                Password=`echo $1 | sed 's/^-W//'`
                ;;
# Directory where to install the data. No default, unless the environment
# variable PGDATA is set.
        --pgdata|-D)
                PGDATA="$2"
                shift;;
        --pgdata=*)
                PGDATA=`echo $1 | sed 's/^--pgdata=//'`
                ;;
        -D*)
                PGDATA=`echo $1 | sed 's/^-D//'`
                ;;
# The directory where the database templates are stored (traditionally in
# $prefix/lib). This is now autodetected for the most common layouts.
        --pglib|-L)
                PGLIB="$2"
                shift;;
        --pglib=*)
                PGLIB=`echo $1 | sed 's/^--pglib=//'`
                ;;
        -L*)
                PGLIB=`echo $1 | sed 's/^-L//'`
                ;;
# The encoding of the template1 database. Defaults to what you chose
# at configure time. (see above)
        --pgencoding|-e)
                MULTIBYTE="$2"
                shift;;
        --pgencoding=*)
                MULTIBYTE=`echo $1 | sed 's/^--pgencoding=//'`
                ;;
        -e*)
                MULTIBYTE=`echo $1 | sed 's/^-e//'`
                ;;
        *)
                echo "Unrecognized option '$1'. Try -? for help."
                exit 1
                ;;
    esac
    shift
done


if [ "$usage" ]; then
        echo "$CMDNAME [-t|--template] [-d|--debug] [-n|--noclean] \\"
        echo "  [-u|--username SUPERUSER] [-D|--pgdata DATADIR] \\"
        echo "  [-L|--pglib=LIBDIR] [-e|--pgencoding=ENCODING]"
        exit 0
fi


#-------------------------------------------------------------------------
# Resolve the multibyte encoding name
#-------------------------------------------------------------------------

if [ "$MULTIBYTE" ]; then
	MULTIBYTEID=`$PGPATH/pg_encoding $MULTIBYTE`
        if [ $? -ne 0 ]; then
                echo "The program pg_encoding failed. Perhaps you did not configure"
                echo "PostgreSQL for multibyte support or the program was not success-"
                echo "fully installed."
                exit 1
        fi
	if [ -z "$MULTIBYTEID" ]; then
		echo "$CMDNAME: $MULTIBYTE is not a valid encoding name."
		exit 1
	fi
fi


#-------------------------------------------------------------------------
# Make sure he told us where to build the database system
#-------------------------------------------------------------------------

if [ -z "$PGDATA" ]; then
    echo "$CMDNAME: You must identify where the the data for this database"
    echo "system will reside.  Do this with either a --pgdata invocation"
    echo "option or a PGDATA environment variable."
    echo
    exit 1
fi

# The data path must be absolute, because the backend doesn't like
# '.' and '..' stuff. (Should perhaps be fixed there.)

if ! echo $PGDATA | grep -s '^/' >& /dev/null ; then
    echo "$CMDNAME: The data path must be specified as an absolute path."
    exit 1
fi

#---------------------------------------------------------------------------
# Figure out who the Postgres superuser for the new database system will be.
#---------------------------------------------------------------------------

# This means they have neither 'id' nor 'whoami'!
if [ -z "$POSTGRES_SUPERUSERNAME" ]; then 
    echo "$CMDNAME: Could not determine what the name of the database"
    echo "superuser should be. Please use the --username option."
    exit 1
fi

echo "This database system will be initialized with username \"$POSTGRES_SUPERUSERNAME\"."
echo "This user will own all the data files and must also own the server process."
echo


#-------------------------------------------------------------------------
# Find the input files
#-------------------------------------------------------------------------

if [ -z "$PGLIB" ]; then
        for dir in "$PGPATH/../lib" "$PGPATH/../lib/pgsql"; do
                if [ -f "$dir/global1.bki.source" ]; then
                        PGLIB=$dir
                        break
                fi
        done
fi

if [ -z "$PGLIB" ]; then
        echo "$CMDNAME: Could not find the \"lib\" directory, that contains"
        echo "the files needed by initdb. Please specify it with the"
        echo "--pglib option."
        exit 1
fi


TEMPLATE=$PGLIB/local1_template1.bki.source
GLOBAL=$PGLIB/global1.bki.source
PG_HBA_SAMPLE=$PGLIB/pg_hba.conf.sample

TEMPLATE_DESCR=$PGLIB/local1_template1.description
GLOBAL_DESCR=$PGLIB/global1.description
PG_GEQO_SAMPLE=$PGLIB/pg_geqo.sample

for PREREQ_FILE in $TEMPLATE $GLOBAL $PG_HBA_SAMPLE; do
    if [ ! -f $PREREQ_FILE ]; then 
        echo "$CMDNAME does not find the file '$PREREQ_FILE'."
        echo "This means you have a corrupted installation or identified the"
        echo "wrong directory with the --pglib invocation option."
        exit 1
    fi
done

[ "$debug" -ne 0 ] && echo "$CMDNAME: Using $TEMPLATE as input to create the template database."

if [ $template_only -eq 0 ]; then
    [ "$debug" -ne 0 ] && echo "$CMDNAME: Using $GLOBAL as input to create the global classes."
    [ "$debug" -ne 0 ] && echo "$CMDNAME: Using $PG_HBA_SAMPLE as default authentication control file."
fi  

trap 'echo "Caught signal." ; exit_nicely' SIGINT SIGTERM


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
