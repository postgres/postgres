#! /bin/sh
#-------------------------------------------------------------------------
#
# initdb creates (initializes) a PostgreSQL database cluster (site,
# instance, installation, whatever).  A database cluster is a
# collection of PostgreSQL databases all managed by the same postmaster.
#
# To create the database cluster, we create the directory that contains
# all its data, create the files that hold the global tables, create
# a few other control files for it, and create two databases: the
# template0 and template1 databases.
#
# The template databases are ordinary PostgreSQL databases.  template0
# is never supposed to change after initdb, whereas template1 can be
# changed to add site-local standard data.  Either one can be copied
# to produce a new database.
#
# To create template1, we run the postgres (backend) program and
# feed it data from the bki files that were installed.  template0 is
# made just by copying the completed template1.
#
#
# Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/initdb/Attic/initdb.sh,v 1.123 2001/03/27 05:45:50 ishii Exp $
#
#-------------------------------------------------------------------------


##########################################################################
#
# INITIALIZATION

exit_nicely(){
    stty echo > /dev/null 2>&1
    echo 1>&2
    echo "$CMDNAME failed." 1>&2
    if [ "$noclean" != yes ]; then
        if [ "$made_new_pgdata" = yes ]; then
            echo "Removing $PGDATA." 1>&2
            rm -rf "$PGDATA" || echo "Failed." 1>&2
        fi
        echo "Removing temp file $TEMPFILE." 1>&2
        rm -rf "$TEMPFILE" || echo "Failed." 1>&2
    else
        echo "Data directory $PGDATA will not be removed at user's request." 1>&2
    fi
    exit 1
}


CMDNAME=`basename $0`

# Placed here during build
VERSION='@VERSION@'
bindir='@bindir@'
# Note that "datadir" is not the directory we're initializing, it's
# merely how Autoconf names PREFIX/share.
datadir='@datadir@'
# as set by configure --enable-multibyte[=XXX].
MULTIBYTE='@MULTIBYTE@'

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
        self_path=`echo $0 | sed 's,/[^/]*$,,'`       # (dirname command is not portable)
else
        # look for it in PATH ('which' command is not portable)
        for dir in `echo "$PATH" | sed 's/:/ /g'`
	do
                # empty entry in path means current dir
                [ -z "$dir" ] && dir='.'
                if [ -f "$dir/$CMDNAME" ]
		then
                        self_path="$dir"
                        break
                fi
        done
fi


# Check for right version of backend.  First we check for an
# executable in the same directory is this initdb script (presuming
# the above code worked).  Then we fall back to the hard-wired bindir.
# We do it in this order because during upgrades users might move
# their trees to backup places, so $bindir might be inaccurate.

if [ x"$self_path" != x"" ] \
  && [ -x "$self_path/postgres" ] \
  && [ x"`$self_path/postgres -V 2>/dev/null`" = x"postgres (PostgreSQL) $VERSION" ]
then
    PGPATH=$self_path
elif [ -x "$bindir/postgres" ]; then
    if [ x"`$bindir/postgres -V 2>/dev/null`" = x"postgres (PostgreSQL) $VERSION" ]
    then
        PGPATH=$bindir
    else
        # Maybe there was an error message?
        errormsg=`$bindir/postgres -V 2>&1 >/dev/null`
      (
        echo "The program "
        echo "    '$bindir/postgres'"
        echo "needed by $CMDNAME does not belong to PostgreSQL version $VERSION, or"
        echo "there may be a configuration problem."
        if test x"$errormsg" != x""; then
            echo
            echo "This was the error message issued by that program:"
            echo "$errormsg"
        fi
      ) 1>&2
        exit 1
    fi
else
    echo "The program 'postgres' is needed by $CMDNAME but was not found in" 1>&2
    echo "the directory '$bindir'.  Check your installation." 1>&2
    exit 1
fi


# Now we can assume that 'pg_id' belongs to the same version as the
# verified 'postgres' in the same directory.
if [ ! -x "$PGPATH/pg_id" ]; then
    echo "The program 'pg_id' is needed by $CMDNAME but was not found in" 1>&2
    echo "the directory '$PGPATH'.  Check your installation." 1>&2
    exit 1
fi


EffectiveUser=`$PGPATH/pg_id -n -u`
if [ -z "$EffectiveUser" ]; then
    echo "$CMDNAME: could not determine current user name" 1>&2
    exit 1
fi

if [ `$PGPATH/pg_id -u` -eq 0 ]
then
    echo "You cannot run $CMDNAME as root. Please log in (using, e.g., 'su')" 1>&2
    echo "as the (unprivileged) user that will own the server process." 1>&2
    exit 1
fi


short_version=`echo $VERSION | sed -e 's!^\([0-9][0-9]*\.[0-9][0-9]*\).*!\1!'`
if [ x"$short_version" = x"" ] ; then
  echo "$CMDNAME: bug: version number has wrong format" 1>&2
  exit 1
fi


##########################################################################
#
# COMMAND LINE OPTIONS

# 0 is the default (non-)encoding
MULTIBYTEID=0

# Set defaults:
debug=
noclean=
show_setting=

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
        --version|-V)
                echo "initdb (PostgreSQL) $VERSION"
                exit 0
                ;;
        --debug|-d)
                debug=yes
                echo "Running with debug mode on."
                ;;
        --show|-s)
        	show_setting=yes
        	;;        
        --noclean|-n)
                noclean=yes
                echo "Running with noclean mode on. Mistakes will not be cleaned up."
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
# The directory where the .bki input files are stored. Normally
# they are in PREFIX/share and this option should be unnecessary.
        -L)
                datadir="$2"
                shift;;
        -L*)
                datadir=`echo $1 | sed 's/^-L//'`
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
		echo "Try '$CMDNAME --help' for more information."
		exit 1
		;;
        *)
                PGDATA=$1
                ;;
    esac
    shift
done

if [ "$usage" ]; then
    echo "$CMDNAME initializes a PostgreSQL database cluster."
    echo
    echo "Usage:"
    echo "  $CMDNAME [options] datadir"
    echo
    echo "Options:"
    echo " [-D, --pgdata] DATADIR       Location for this database cluster"
    echo "  -W, --pwprompt              Prompt for a password for the new superuser"
    if [ -n "$MULTIBYTE" ] ; then 
        echo "  -E, --encoding ENCODING     Set the default multibyte encoding for new databases"
    fi
    echo "  -i, --sysid SYSID           Database sysid for the superuser"
    echo "Less commonly used options: "
    echo "  -L DIRECTORY                Where to find the input files"
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
	MULTIBYTEID=`$PGPATH/pg_encoding $MULTIBYTE`
        if [ "$?" -ne 0 ]
	then
              (
                echo "$CMDNAME: pg_encoding failed"
                echo
                echo "Perhaps you did not configure PostgreSQL for multibyte support or"
                echo "the program was not successfully installed."
              ) 1>&2
                exit 1
        fi
	if [ -z "$MULTIBYTEID" ]
	then
		echo "$CMDNAME: $MULTIBYTE is not a valid encoding name" 1>&2
		exit 1
	elif [ $MULTIBYTEID -gt 31 ]
	then
		echo "$CMDNAME: $MULTIBYTE cannot be used as a database encoding" 1>&2
		exit 1
	fi
fi


#-------------------------------------------------------------------------
# Make sure he told us where to build the database system
#-------------------------------------------------------------------------

if [ -z "$PGDATA" ]
then
  (
    echo "$CMDNAME: You must identify where the the data for this database"
    echo "system will reside.  Do this with either a -D invocation"
    echo "option or a PGDATA environment variable."
  ) 1>&2
    exit 1
fi


#-------------------------------------------------------------------------
# Find the input files
#-------------------------------------------------------------------------

TEMPLATE1_BKI="$datadir"/template1.bki
GLOBAL_BKI="$datadir"/global.bki

TEMPLATE1_DESCR="$datadir"/template1.description
GLOBAL_DESCR="$datadir"/global.description

PG_HBA_SAMPLE="$datadir"/pg_hba.conf.sample
PG_IDENT_SAMPLE="$datadir"/pg_ident.conf.sample
POSTGRESQL_CONF_SAMPLE="$datadir"/postgresql.conf.sample

if [ "$show_setting" = yes ] || [ "$debug" = yes ]
then
    echo
    echo "Initdb variables:"
    for var in PGDATA datadir PGPATH TEMPFILE MULTIBYTE MULTIBYTEID \
        POSTGRES_SUPERUSERNAME POSTGRES_SUPERUSERID TEMPLATE1_BKI GLOBAL_BKI \
        TEMPLATE1_DESCR GLOBAL_DESCR POSTGRESQL_CONF_SAMPLE \
	PG_HBA_SAMPLE PG_IDENT_SAMPLE ; do
        eval "echo '  '$var=\$$var"
    done
fi

if [ "$show_setting" = yes ] ; then
    exit 0
fi

for PREREQ_FILE in "$TEMPLATE1_BKI" "$GLOBAL_BKI" "$PG_HBA_SAMPLE" \
    "$PG_IDENT_SAMPLE"
do
    if [ ! -f "$PREREQ_FILE" ] ; then
      (
        echo "$CMDNAME does not find the file '$PREREQ_FILE'."
        echo "This means you have a corrupted installation or identified the"
        echo "wrong directory with the -L invocation option."
      ) 1>&2
        exit 1
    fi
done

for file in "$TEMPLATE1_BKI" "$GLOBAL_BKI"; do
     if [ x"`sed 1q $file`" != x"# PostgreSQL $short_version" ]; then
       (
         echo "The input file '$file' needed by $CMDNAME does not"
         echo "belong to PostgreSQL $VERSION.  Check your installation or specify the"
         echo "correct path using the -L option."
       ) 1>&2
         exit 1
     fi
done


trap 'echo "Caught signal." ; exit_nicely' 1 2 3 15

# Let's go
echo "This database system will be initialized with username \"$POSTGRES_SUPERUSERNAME\"."
echo "This user will own all the data files and must also own the server process."
echo


##########################################################################
#
# CREATE DATABASE DIRECTORY

# umask must disallow access to group, other for files and dirs
umask 077

# find out if directory is empty
pgdata_contents=`ls -A "$PGDATA" 2>/dev/null`
if [ x"$pgdata_contents" != x ]
then
    (
      echo "$CMDNAME: The directory $PGDATA exists but is not empty."
      echo "If you want to create a new database system, either remove or empty"
      echo "the directory $PGDATA or run initdb with"
      echo "an argument other than $PGDATA."
    ) 1>&2
    exit 1
else
    if [ ! -d "$PGDATA" ]; then
        echo "Creating directory $PGDATA"
        mkdir -p "$PGDATA" >/dev/null 2>&1 || mkdir "$PGDATA" || exit_nicely
        made_new_pgdata=yes
    else
        echo "Fixing permissions on existing directory $PGDATA"
	chmod go-rwx "$PGDATA" || exit_nicely
    fi

    if [ ! -d "$PGDATA"/base ]
	then
        echo "Creating directory $PGDATA/base"
        mkdir "$PGDATA"/base || exit_nicely
    fi
    if [ ! -d "$PGDATA"/global ]
    then
        echo "Creating directory $PGDATA/global"
        mkdir "$PGDATA"/global || exit_nicely
    fi
    if [ ! -d "$PGDATA"/pg_xlog ]
    then
        echo "Creating directory $PGDATA/pg_xlog"
        mkdir "$PGDATA"/pg_xlog || exit_nicely
    fi
fi


##########################################################################
#
# CREATE TEMPLATE1 DATABASE

rm -rf "$PGDATA"/base/1 || exit_nicely
mkdir "$PGDATA"/base/1 || exit_nicely

if [ "$debug" = yes ]
then
    BACKEND_TALK_ARG="-d"
else
    BACKEND_TALK_ARG="-Q"
fi

BACKENDARGS="-boot -C -F -D$PGDATA $BACKEND_TALK_ARG"
FIRSTRUN="-boot -x1 -C -F -D$PGDATA $BACKEND_TALK_ARG"

echo "Creating template1 database in $PGDATA/base/1"
[ "$debug" = yes ] && echo "Running: $PGPATH/postgres $FIRSTRUN template1"

cat "$TEMPLATE1_BKI" \
| sed -e "s/PGUID/$POSTGRES_SUPERUSERID/g" \
| "$PGPATH"/postgres $FIRSTRUN template1 \
|| exit_nicely

echo $short_version > "$PGDATA"/base/1/PG_VERSION || exit_nicely


##########################################################################
#
# CREATE GLOBAL TABLES
#

echo "Creating global relations in $PGDATA/global"

[ "$debug" = yes ] && echo "Running: $PGPATH/postgres $BACKENDARGS template1"

cat "$GLOBAL_BKI" \
| sed -e "s/POSTGRES/$POSTGRES_SUPERUSERNAME/g" \
      -e "s/PGUID/$POSTGRES_SUPERUSERID/g" \
      -e "s/ENCODING/$MULTIBYTEID/g" \
| "$PGPATH"/postgres $BACKENDARGS template1 \
|| exit_nicely

echo $short_version > "$PGDATA/PG_VERSION" || exit_nicely

cp "$PG_HBA_SAMPLE" "$PGDATA"/pg_hba.conf              || exit_nicely
cp "$PG_IDENT_SAMPLE" "$PGDATA"/pg_ident.conf          || exit_nicely
cp "$POSTGRESQL_CONF_SAMPLE" "$PGDATA"/postgresql.conf || exit_nicely
chmod 0600 "$PGDATA"/pg_hba.conf "$PGDATA"/pg_ident.conf \
	"$PGDATA"/postgresql.conf


##########################################################################
#
# CREATE VIEWS and other things

echo "Initializing pg_shadow."

PGSQL_OPT="-o /dev/null -O -F -D$PGDATA"

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
        echo "Passwords didn't match." 1>&2
        exit_nicely
    fi
    echo "ALTER USER \"$POSTGRES_SUPERUSERNAME\" WITH PASSWORD '$FirstPw'" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
    if [ ! -f $PGDATA/global/pg_pwd ]; then
        echo "The password file wasn't generated. Please report this problem." 1>&2
        exit_nicely
    fi
    echo "Setting password"
fi


echo "Enabling unlimited row width for system tables."

echo "ALTER TABLE pg_attrdef CREATE TOAST TABLE" \
        | "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "ALTER TABLE pg_description CREATE TOAST TABLE" \
        | "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "ALTER TABLE pg_proc CREATE TOAST TABLE" \
        | "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "ALTER TABLE pg_relcheck CREATE TOAST TABLE" \
        | "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "ALTER TABLE pg_rewrite CREATE TOAST TABLE" \
        | "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "ALTER TABLE pg_statistic CREATE TOAST TABLE" \
        | "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely


echo "Creating system views."

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

echo "CREATE VIEW pg_rules AS \
        SELECT \
            C.relname AS tablename, \
            R.rulename AS rulename, \
	    pg_get_ruledef(R.rulename) AS definition \
	FROM pg_rewrite R, pg_class C \
	WHERE R.rulename !~ '^_RET' \
            AND C.oid = R.ev_class;" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "CREATE VIEW pg_views AS \
        SELECT \
            C.relname AS viewname, \
            pg_get_userbyid(C.relowner) AS viewowner, \
            pg_get_viewdef(C.relname) AS definition \
        FROM pg_class C \
        WHERE C.relkind = 'v';" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

# XXX why does pg_tables include sequences?

echo "CREATE VIEW pg_tables AS \
        SELECT \
            C.relname AS tablename, \
	    pg_get_userbyid(C.relowner) AS tableowner, \
	    C.relhasindex AS hasindexes, \
	    C.relhasrules AS hasrules, \
	    (C.reltriggers > 0) AS hastriggers \
        FROM pg_class C \
        WHERE C.relkind IN ('r', 's');" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "CREATE VIEW pg_indexes AS \
        SELECT \
            C.relname AS tablename, \
	    I.relname AS indexname, \
            pg_get_indexdef(X.indexrelid) AS indexdef \
        FROM pg_index X, pg_class C, pg_class I \
	WHERE C.relkind = 'r' AND I.relkind = 'i' \
	    AND C.oid = X.indrelid \
            AND I.oid = X.indexrelid;" \
        | "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Loading pg_description."
echo "COPY pg_description FROM STDIN" > $TEMPFILE
cat "$TEMPLATE1_DESCR" >> $TEMPFILE
cat "$GLOBAL_DESCR" >> $TEMPFILE

cat $TEMPFILE \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
rm -f "$TEMPFILE" || exit_nicely

echo "Setting lastsysoid."
echo "UPDATE pg_database SET \
	datistemplate = 't', \
	datlastsysoid = (SELECT max(oid) FROM pg_description) \
        WHERE datname = 'template1'" \
		| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Vacuuming database."
echo "VACUUM ANALYZE" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely

echo "Copying template1 to template0."
echo "CREATE DATABASE template0" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "UPDATE pg_database SET \
	datistemplate = 't', \
	datallowconn = 'f' \
        WHERE datname = 'template0'" \
		| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "VACUUM pg_database" \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely


##########################################################################
#
# FINISHED

echo
echo "Success. You can now start the database server using:"
echo ""
echo "    $PGPATH/postmaster -D $PGDATA"
echo "or"
# (Advertise -l option here, otherwise we have a background
#  process writing to the terminal.)
echo "    $PGPATH/pg_ctl -D $PGDATA -l logfile start"
echo

exit 0
