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
#    $Header: /cvsroot/pgsql/src/bin/initdb/Attic/initdb.sh,v 1.90 2000/04/08 18:35:29 momjian Exp $
#
#-------------------------------------------------------------------------

exit_nicely(){
    stty echo > /dev/null 2>&1
    echo
    echo "$CMDNAME failed."
    if [ "$noclean" -eq 0 ]; then
        echo "Removing $PGDATA."
        rm -rf "$PGDATA" || echo "Failed."
        echo "Removing temp file $TEMPFILE."
        rm -rf "$TEMPFILE" || echo "Failed."
    else
        echo "Data directory $PGDATA will not be removed at user's request."
    fi
    exit 1
}


CMDNAME=`basename $0`

if [ "$TMPDIR" ]; then
    TEMPFILE="$TMPDIR/initdb.$$"
else
    TEMPFILE="/tmp/initdb.$$"
fi


# Check for echo -n vs echo \c
if echo '\c' | grep -s c >/dev/null 2>&1
then
    ECHO_N="echo -n"
    ECHO_C=""
else
    ECHO_N="echo"
    ECHO_C='\c'
fi


#
# Find out where we're located
#
if echo "$0" | grep '/' > /dev/null 2>&1 
then
        # explicit dir name given
        PGPATH=`echo $0 | sed 's,/[^/]*$,,'`       # (dirname command is not portable)
else
        # look for it in PATH ('which' command is not portable)
        for dir in `echo "$PATH" | sed 's/:/ /g'`
	do
                # empty entry in path means current dir
                [ -z "$dir" ] && dir='.'
                if [ -f "$dir/$CMDNAME" ]
		then
                        PGPATH="$dir"
                        break
                fi
        done
fi

# Check if needed programs actually exist in path
for prog in postgres pg_version pg_id
do
        if [ ! -x "$PGPATH/$prog" ]
	then
                echo "The program $prog needed by $CMDNAME could not be found. It was"
                echo "expected at:"
                echo "    $PGPATH/$prog"
                echo "If this is not the correct directory, please start $CMDNAME"
                echo "with a full search path. Otherwise make sure that the program"
                echo "was installed successfully."
                exit 1
        fi
done


# Gotta wait for pg_id existence check above
EffectiveUser=`$PGPATH/pg_id -n -u`
if [ -z "$EffectiveUser" ]; then
    echo "Could not determine current user name. You are really hosed."
    exit 1
fi

if [ `$PGPATH/pg_id -u` -eq 0 ]
then
    echo "You cannot run $CMDNAME as root. Please log in (using, e.g., 'su')"
    echo "as the (unprivileged) user that will own the server process."
    exit 1
fi


# 0 is the default (non-)encoding
MULTIBYTEID=0
# This is placed here by configure --enable-multibyte[=XXX].
MULTIBYTE=__MULTIBYTE__

# Set defaults:
debug=0
noclean=0
template_only=0
show_setting=0

# Note: There is a single compelling reason that the name of the database
#       superuser be the same as the Unix user owning the server process:
#       The single user postgres backend will only connect as the database
#       user with the same name as the Unix user running it. That's
#       a security measure.
POSTGRES_SUPERUSERNAME="$EffectiveUser"
POSTGRES_SUPERUSERID=`$PGPATH/pg_id -u`

while [ "$#" -gt 0 ]
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
        --show|-s)
        	show_setting=1
        	;;        
        --noclean|-n)
                noclean=1
                echo "Running with noclean mode on. Mistakes will not be cleaned up."
                ;;
        --template|-t)
                template_only=1
                echo "Updating template1 database only."
                ;;
# The sysid of the database superuser. Can be freely changed.
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
# Make initdb prompt for the default password of the database superuser.
        --pwprompt|-W)
                PwPrompt=1
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
        --encoding|-E)
                MULTIBYTE="$2"
                shift;;
        --encoding=*)
                MULTIBYTE=`echo $1 | sed 's/^--encoding=//'`
                ;;
        -E*)
                MULTIBYTE=`echo $1 | sed 's/^-E//'`
                ;;
	-*)
		echo "$CMDNAME: invalid option: $1"
		echo "Try -? for help."
		exit 1
		;;
        *)
                PGDATA=$1
                ;;
    esac
    shift
done

if [ "$usage" ]; then
        echo "initdb initialized a PostgreSQL database."
 	echo
 	echo "Usage:"
        echo "  $CMDNAME [options] datadir"
 	echo
        echo "Options:"
        echo " [-D, --pgdata] <datadir>     Location for this database"
        echo "  -W, --pwprompt              Prompt for a password for the new superuser's"
 	if [ -n "$MULTIBYTE" ]
	then 
 		echo "  -E, --encoding <encoding>   Set the default multibyte encoding for new databases"
	fi
        echo "  -i, --sysid <sysid>         Database sysid for the superuser"
        echo "Less commonly used options: "
        echo "  -L, --pglib <libdir>        Where to find the input files"
        echo "  -t, --template              Re-initialize template database only"
        echo "  -d, --debug                 Generate lots of debugging output"
        echo "  -n, --noclean               Do not clean up after errors"
 	echo
        echo "Report bugs to <pgsql-bugs@postgresql.org>."
 	exit 0
fi

#-------------------------------------------------------------------------
# Resolve the multibyte encoding name
#-------------------------------------------------------------------------

if [ "$MULTIBYTE" ]
then
	MULTIBYTEID=`$PGPATH/pg_encoding $MULTIBYTE 2> /dev/null`
        if [ "$?" -ne 0 ]
	then
                echo "$CMDNAME: pg_encoding failed"
                echo
                echo "Perhaps you did not configure PostgreSQL for multibyte support or"
                echo "the program was not successfully installed."
                exit 1
        fi
	if [ -z "$MULTIBYTEID" ]
	then
		echo "$CMDNAME: $MULTIBYTE is not a valid encoding name"
		exit 1
	fi
fi


#-------------------------------------------------------------------------
# Make sure he told us where to build the database system
#-------------------------------------------------------------------------

if [ -z "$PGDATA" ]
then
    echo "$CMDNAME: You must identify where the the data for this database"
    echo "system will reside.  Do this with either a --pgdata invocation"
    echo "option or a PGDATA environment variable."
    echo
    exit 1
fi

# The data path must be absolute, because the backend doesn't like
# '.' and '..' stuff. (Should perhaps be fixed there.)

echo "$PGDATA" | grep '^/' > /dev/null 2>&1
if [ "$?" -ne 0 ]
then
    echo "$CMDNAME: data path must be specified as an absolute path"
    exit 1
fi


#-------------------------------------------------------------------------
# Find the input files
#-------------------------------------------------------------------------

if [ -z "$PGLIB" ]
then
        for dir in "$PGPATH/../lib" "$PGPATH/../lib/pgsql"
	do
                if [ -f "$dir/global1.bki.source" ]
		then
                        PGLIB="$dir"
                        break
                fi
        done
fi

if [ -z "$PGLIB" ]
then
        echo "$CMDNAME: Could not find the \"lib\" directory, that contains"
        echo "the files needed by initdb. Please specify it with the"
        echo "--pglib option."
        exit 1
fi


TEMPLATE="$PGLIB"/local1_template1.bki.source
GLOBAL="$PGLIB"/global1.bki.source
PG_HBA_SAMPLE="$PGLIB"/pg_hba.conf.sample

TEMPLATE_DESCR="$PGLIB"/local1_template1.description
GLOBAL_DESCR="$PGLIB"/global1.description
PG_GEQO_SAMPLE="$PGLIB"/pg_geqo.sample
PG_POSTMASTER_OPTS_DEFAULT_SAMPLE="$PGLIB"/postmaster.opts.default.sample

if [ "$show_setting" -eq 1 ]
then
	echo
	echo "The initdb setting:"
 	echo
 	echo "  DATADIR:        $PGDATA"
 	echo "  PGLIB:          $PGLIB"
 	echo "  PGPATH:         $PGPATH"
 	echo "  TEMPFILE:       $TEMPFILE"
 	echo "  MULTIBYTE:      $MULTIBYTE"
 	echo "  MULTIBYTEID:    $MULTIBYTEID"
 	echo "  SUPERUSERNAME:  $POSTGRES_SUPERUSERNAME"
 	echo "  SUPERUSERID:    $POSTGRES_SUPERUSERID"
 	echo "  TEMPLATE:       $TEMPLATE"	 
	echo "  GLOBAL:         $GLOBAL"
	echo "  PG_HBA_SAMPLE:  $PG_HBA_SAMPLE"
	echo "  TEMPLATE_DESCR: $TEMPLATE_DESCR"
	echo "  GLOBAL_DESCR:   $GLOBAL_DESCR"
	echo "  PG_GEQO_SAMPLE: $PG_GEQO_SAMPLE"
	echo "  PG_POSTMASTER_OPTS_DEFAULT_SAMPLE: $PG_POSTMASTER_OPTS_DEFAULT_SAMPLE"
	echo 
	exit 0
fi

for PREREQ_FILE in "$TEMPLATE" "$GLOBAL" "$PG_HBA_SAMPLE"
do
    if [ ! -f "$PREREQ_FILE" ]
	then 
        echo "$CMDNAME does not find the file '$PREREQ_FILE'."
        echo "This means you have a corrupted installation or identified the"
        echo "wrong directory with the --pglib invocation option."
        exit 1
    fi
done

[ "$debug" -ne 0 ] && echo "$CMDNAME: Using $TEMPLATE as input to create the template database."

if [ "$template_only" -eq 0 ]
then
    [ "$debug" -ne 0 ] && echo "$CMDNAME: Using $GLOBAL as input to create the global classes."
    [ "$debug" -ne 0 ] && echo "$CMDNAME: Using $PG_HBA_SAMPLE as default authentication control file."
fi

trap 'echo "Caught signal." ; exit_nicely' 1 2 3 15

# Let's go
echo "This database system will be initialized with username \"$POSTGRES_SUPERUSERNAME\"."
echo "This user will own all the data files and must also own the server process."
echo

# -----------------------------------------------------------------------
# Create the data directory if necessary
# -----------------------------------------------------------------------

# umask must disallow access to group, other for files and dirs
umask 077

if [ -f "$PGDATA"/PG_VERSION ]
then
    if [ "$template_only" -eq 0 ]
    then
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
    if [ ! -d "$PGDATA" ]
	then
        echo "Creating database system directory $PGDATA"
        mkdir "$PGDATA" || exit_nicely
    else
        echo "Fixing permissions on pre-existing data directory $PGDATA"
	chmod go-rwx "$PGDATA" || exit_nicely
    fi

    if [ ! -d "$PGDATA"/base ]
	then
        echo "Creating database system directory $PGDATA/base"
        mkdir "$PGDATA"/base || exit_nicely
    fi
    if [ ! -d "$PGDATA"/pg_xlog ]
    then
        echo "Creating database XLOG directory $PGDATA/pg_xlog"
        mkdir "$PGDATA"/pg_xlog || exit_nicely
    fi
fi

#----------------------------------------------------------------------------
# Create the template1 database
#----------------------------------------------------------------------------

rm -rf "$PGDATA"/base/template1 || exit_nicely
mkdir "$PGDATA"/base/template1 || exit_nicely

if [ "$debug" -eq 1 ]
then
    BACKEND_TALK_ARG="-d"
else
    BACKEND_TALK_ARG="-Q"
fi

BACKENDARGS="-boot -C -F -D$PGDATA $BACKEND_TALK_ARG"
FIRSTRUN="-boot -x -C -F -D$PGDATA $BACKEND_TALK_ARG"

echo "Creating template database in $PGDATA/base/template1"
[ "$debug" -ne 0 ] && echo "Running: $PGPATH/postgres $FIRSTRUN template1"

cat "$TEMPLATE" \
| sed -e "s/PGUID/$POSTGRES_SUPERUSERID/g" \
| "$PGPATH"/postgres $FIRSTRUN template1 \
|| exit_nicely

"$PGPATH"/pg_version "$PGDATA"/base/template1 || exit_nicely

#----------------------------------------------------------------------------
# Create the global classes, if requested.
#----------------------------------------------------------------------------

if [ "$template_only" -eq 0 ]
then
    echo "Creating global relations in $PGDATA/base"
    [ "$debug" -ne 0 ] && echo "Running: $PGPATH/postgres $BACKENDARGS template1"

    cat "$GLOBAL" \
    | sed -e "s/POSTGRES/$POSTGRES_SUPERUSERNAME/g" \
          -e "s/PGUID/$POSTGRES_SUPERUSERID/g" \
    | "$PGPATH"/postgres $BACKENDARGS template1 \
    || exit_nicely

    "$PGPATH"/pg_version "$PGDATA" || exit_nicely

    cp "$PG_HBA_SAMPLE" "$PGDATA"/pg_hba.conf     || exit_nicely
    cp "$PG_GEQO_SAMPLE" "$PGDATA"/pg_geqo.sample || exit_nicely
    cp "$PG_POSTMASTER_OPTS_DEFAULT_SAMPLE" "$PGDATA"/postmaster.opts.default || exit_nicely

    echo "Adding template1 database to pg_database"

    echo "open pg_database" > "$TEMPFILE"
    echo "insert (template1 $POSTGRES_SUPERUSERID $MULTIBYTEID template1)" >> $TEMPFILE
    #echo "show" >> "$TEMPFILE"
    echo "close pg_database" >> "$TEMPFILE"

    [ "$debug" -ne 0 ] && echo "Running: $PGPATH/postgres $BACKENDARGS template1 < $TEMPFILE"

    "$PGPATH"/postgres $BACKENDARGS template1 < "$TEMPFILE"
    # Gotta remove that temp file before exiting on error.
    retval="$?"
    if [ "$noclean" -eq 0 ]
    then
            rm -f "$TEMPFILE" || exit_nicely
    fi
    [ "$retval" -ne 0 ] && exit_nicely
fi

echo

PGSQL_OPT="-o /dev/null -O -F -Q -D$PGDATA"

# Create a trigger so that direct updates to pg_shadow will be written
# to the flat password file pg_pwd
echo "CREATE TRIGGER pg_sync_pg_pwd AFTER INSERT OR UPDATE OR DELETE ON pg_shadow" \
     "FOR EACH ROW EXECUTE PROCEDURE update_pg_pwd()" \
     | "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

# needs to be done before alter user
echo "REVOKE ALL on pg_shadow FROM public" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

# set up password
if [ "$PwPrompt" ]; then
    $ECHO_N "Enter new superuser password: "$ECHO_C
    stty -echo > /dev/null 2>&1
    read FirstPw
    stty echo > /dev/null 2>&1
    echo
    $ECHO_N "Enter it again: "$ECHO_C
    stty -echo > /dev/null 2>&1
    read SecondPw
    stty echo > /dev/null 2>&1
    echo
    if [ "$FirstPw" != "$SecondPw" ]; then
        echo "Passwords didn't match."
        exit_nicely
    fi
    echo "ALTER USER \"$POSTGRES_SUPERUSERNAME\" WITH PASSWORD '$FirstPw'" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
    if [ ! -f $PGDATA/pg_pwd ]; then
        echo "The password file wasn't generated. Please report this problem."
        exit_nicely
    fi
    echo "Setting password"
fi


echo "Creating view pg_user."
echo "CREATE VIEW pg_user AS \
        SELECT \
            usename, \
            usesysid, \
            usecreatedb, \
            usetrace, \
            usesuper, \
            usecatupd, \
            '********'::text as passwd, \
            valuntil \
        FROM pg_shadow" \
        | "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Creating view pg_rules."
echo "CREATE VIEW pg_rules AS \
        SELECT \
            C.relname AS tablename, \
            R.rulename AS rulename, \
	    pg_get_ruledef(R.rulename) AS definition \
	FROM pg_rewrite R, pg_class C \
	WHERE R.rulename !~ '^_RET' \
            AND C.oid = R.ev_class;" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Creating view pg_views."
echo "CREATE VIEW pg_views AS \
        SELECT \
            C.relname AS viewname, \
            pg_get_userbyid(C.relowner) AS viewowner, \
            pg_get_viewdef(C.relname) AS definition \
        FROM pg_class C \
        WHERE C.relhasrules \
            AND	EXISTS ( \
                SELECT rulename FROM pg_rewrite R \
                    WHERE ev_class = C.oid AND ev_type = '1' \
            )" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Creating view pg_tables."
echo "CREATE VIEW pg_tables AS \
        SELECT \
            C.relname AS tablename, \
	    pg_get_userbyid(C.relowner) AS tableowner, \
	    C.relhasindex AS hasindexes, \
	    C.relhasrules AS hasrules, \
	    (C.reltriggers > 0) AS hastriggers \
        FROM pg_class C \
        WHERE C.relkind IN ('r', 's') \
            AND NOT EXISTS ( \
                SELECT rulename FROM pg_rewrite \
                    WHERE ev_class = C.oid AND ev_type = '1' \
            )" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Creating view pg_indexes."
echo "CREATE VIEW pg_indexes AS \
        SELECT \
            C.relname AS tablename, \
	    I.relname AS indexname, \
            pg_get_indexdef(X.indexrelid) AS indexdef \
        FROM pg_index X, pg_class C, pg_class I \
	WHERE C.oid = X.indrelid \
            AND I.oid = X.indexrelid" \
        | "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Loading pg_description."
echo "COPY pg_description FROM STDIN" > $TEMPFILE
cat "$TEMPLATE_DESCR" >> $TEMPFILE
cat "$GLOBAL_DESCR" >> $TEMPFILE

cat $TEMPFILE \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
if [ "$noclean" -eq 0 ]
then
    rm -f "$TEMPFILE" || exit_nicely
fi

echo "Vacuuming database."
echo "VACUUM ANALYZE" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo
echo "Success. You can now start the database server using:"
echo ""
echo "	$PGPATH/postmaster -D $PGDATA"
echo "or"
echo "	$PGPATH/pg_ctl -D $PGDATA start"
echo

exit 0
