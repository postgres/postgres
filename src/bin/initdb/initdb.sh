#!@SHELL@
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
# To create template1, we run the postgres (backend) program in bootstrap
# mode and feed it data from the postgres.bki library file.  After this
# initial bootstrap phase, some additional stuff is created by normal
# SQL commands fed to a standalone backend.  Those commands are just
# embedded into this script (yeah, it's ugly).
#
# template0 is made just by copying the completed template1.
#
#
# Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/initdb/Attic/initdb.sh,v 1.204.2.3 2004/05/05 16:09:56 tgl Exp $
#
#-------------------------------------------------------------------------


##########################################################################
#
# INITIALIZATION

exit_nicely(){
    stty echo > /dev/null 2>&1
    echo 1>&2
    echo "$CMDNAME: failed" 1>&2
    if [ "$noclean" != yes ]; then
        if [ "$made_new_pgdata" = yes ]; then
            echo "$CMDNAME: removing data directory \"$PGDATA\"" 1>&2
            rm -rf "$PGDATA" || echo "$CMDNAME: failed" 1>&2
        fi
    else
        echo "$CMDNAME: data directory \"$PGDATA\" not removed at user's request" 1>&2
    fi
    exit 1
}

pg_getlocale(){
    arg=$1
    unset ret

    for var in "PGLC_$arg" PGLOCALE LC_ALL "LC_$arg" LANG; do
        varset=`eval echo '${'"$var"'+set}'`
        varval=`eval echo '$'"$var"`
        if test "$varset" = set; then
            ret=$varval
            break
        fi
    done

    if test "${ret+set}" != set; then
        ret=C
    fi

    echo "$ret"
}


CMDNAME=`basename $0`

# Placed here during build
VERSION='@VERSION@'
HAVE_IPV6='@HAVE_IPV6@'
bindir='@bindir@'
# Note that "datadir" is not the directory we're initializing, it's
# merely how Autoconf names PREFIX/share.
datadir='@datadir@'


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
        echo "    $bindir/postgres"
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
    echo "The program \"postgres\" is needed by $CMDNAME but was not found in" 1>&2
    echo "the directory \"$bindir\".  Check your installation." 1>&2
    exit 1
fi


# Now we can assume that 'pg_id' belongs to the same version as the
# verified 'postgres' in the same directory.
if [ ! -x "$PGPATH/pg_id" ]; then
    echo "The program \"pg_id\" is needed by $CMDNAME but was not found in" 1>&2
    echo "the directory \"$PGPATH\".  Check your installation." 1>&2
    exit 1
fi


EffectiveUser=`$PGPATH/pg_id -n -u`
if [ -z "$EffectiveUser" ]; then
    echo "$CMDNAME: could not determine current user name" 1>&2
    exit 1
fi

if [ `$PGPATH/pg_id -u` -eq 0 ]
then
    echo "$CMDNAME: cannot be run as root" 1>&2
    echo "Please log in (using, e.g., \"su\") as the (unprivileged) user that will" 1>&2
    echo "own the server process." 1>&2
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
ENCODINGID=0

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
                echo "Running in debug mode."
                ;;
        --show|-s)
        	show_setting=yes
        	;;        
        --noclean|-n)
                noclean=yes
                echo "Running in noclean mode.  Mistakes will not be cleaned up."
                ;;
# The name of the database superuser. Can be freely changed.
        --username|-U)
                POSTGRES_SUPERUSERNAME="$2"
                shift;;
        --username=*)
                POSTGRES_SUPERUSERNAME=`echo $1 | sed 's/^--username=//'`
                ;;
        -U*)
                POSTGRES_SUPERUSERNAME=`echo $1 | sed 's/^-U//'`
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
                ENCODING="$2"
                shift;;
        --encoding=*)
                ENCODING=`echo $1 | sed 's/^--encoding=//'`
                ;;
        -E*)
                ENCODING=`echo $1 | sed 's/^-E//'`
                ;;
# Locale flags
        --locale)
                PGLOCALE="$2"
                shift;;
        --locale=*)
                PGLOCALE=`echo $1 | sed 's/^[^=]*=//'`
                ;;
        --no-locale)
                PGLOCALE=C
                ;;

        --lc-collate)
                PGLC_COLLATE=$2
                shift;;
        --lc-collate=*)
                PGLC_COLLATE=`echo $1 | sed 's/^[^=]*=//'`
                ;;
        --lc-ctype)
                PGLC_CTYPE=$2
                shift;;
        --lc-ctype=*)
                PGLC_CTYPE=`echo $1 | sed 's/^[^=]*=//'`
                ;;
        --lc-messages)
                PGLC_MESSAGES=$2
                shift;;
        --lc-messages=*)
                PGLC_MESSAGES=`echo $1 | sed 's/^[^=]*=//'`
                ;;
        --lc-monetary)
                PGLC_MONETARY=$2
                shift;;
        --lc-monetary=*)
                PGLC_MONETARY=`echo $1 | sed 's/^[^=]*=//'`
                ;;
        --lc-numeric)
                PGLC_NUMERIC=$2
                shift;;
        --lc-numeric=*)
                PGLC_NUMERIC=`echo $1 | sed 's/^[^=]*=//'`
                ;;
        --lc-time)
                PGLC_TIME=$2
                shift;;
        --lc-time=*)
                PGLC_TIME=`echo $1 | sed 's/^[^=]*=//'`
                ;;

	-*)
		echo "$CMDNAME: invalid option: $1"
		echo "Try \"$CMDNAME --help\" for more information."
		exit 1
		;;

# Non-option argument specifies data directory
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
    echo "  $CMDNAME [OPTION]... [DATADIR]"
    echo
    echo "Options:"
    echo " [-D, --pgdata=]DATADIR     location for this database cluster"
    echo "  -E, --encoding=ENCODING   set default encoding for new databases"
    echo "  --locale=LOCALE           initialize database cluster with given locale"
    echo "  --lc-collate, --lc-ctype, --lc-messages=LOCALE"
    echo "  --lc-monetary, --lc-numeric, --lc-time=LOCALE"
    echo "                            initialize database cluster with given locale"
    echo "                            in the respective category (default taken from"
    echo "                            environment)"
    echo "  --no-locale               equivalent to --locale=C"
    echo "  -U, --username=NAME       database superuser name"
    echo "  -W, --pwprompt            prompt for a password for the new superuser"
    echo "  --help                    show this help, then exit"
    echo "  --version                 output version information, then exit"
    echo
    echo "Less commonly used options: "
    echo "  -d, --debug               generate lots of debugging output"
    echo "  -L DIRECTORY              where to find the input files"
    echo "  -n, --noclean             do not clean up after errors"
    echo
    echo "If the data directory is not specified, the environment variable PGDATA"
    echo "is used."
    echo
    echo "Report bugs to <pgsql-bugs@postgresql.org>."
    exit 0
fi

#-------------------------------------------------------------------------
# Resolve the encoding name
#-------------------------------------------------------------------------

if [ "$ENCODING" ]
then
	ENCODINGID=`$PGPATH/pg_encoding -b $ENCODING`
	if [ "$?" -ne 0 ]
	then
              (
                echo "$CMDNAME: pg_encoding failed"
                echo "Make sure the program was installed correctly."
              ) 1>&2
                exit 1
        fi
	if [ -z "$ENCODINGID" ]
	then
		echo "$CMDNAME: \"$ENCODING\" is not a valid server encoding name" 1>&2
		exit 1
	fi
fi


#-------------------------------------------------------------------------
# Make sure he told us where to build the database system
#-------------------------------------------------------------------------

if [ -z "$PGDATA" ]
then
  (
    echo "$CMDNAME: no data directory specified"
    echo "You must identify the directory where the data for this database system"
    echo "will reside.  Do this with either the invocation option -D or the"
    echo "environment variable PGDATA."
  ) 1>&2
    exit 1
fi


#-------------------------------------------------------------------------
# Find the input files
#-------------------------------------------------------------------------

POSTGRES_BKI="$datadir"/postgres.bki
POSTGRES_DESCR="$datadir"/postgres.description

PG_HBA_SAMPLE="$datadir"/pg_hba.conf.sample
PG_IDENT_SAMPLE="$datadir"/pg_ident.conf.sample
POSTGRESQL_CONF_SAMPLE="$datadir"/postgresql.conf.sample

if [ "$show_setting" = yes ] || [ "$debug" = yes ]
then
  (
    echo
    echo "$CMDNAME: internal variables:"
    for var in PGDATA datadir PGPATH ENCODING ENCODINGID \
        POSTGRES_SUPERUSERNAME POSTGRES_BKI \
        POSTGRES_DESCR POSTGRESQL_CONF_SAMPLE \
	PG_HBA_SAMPLE PG_IDENT_SAMPLE ; do
        eval "echo '  '$var=\$$var"
    done
  ) 1>&2
fi

if [ "$show_setting" = yes ] ; then
    exit 0
fi

for PREREQ_FILE in "$POSTGRES_BKI" "$POSTGRES_DESCR" \
    "$PG_HBA_SAMPLE" "$PG_IDENT_SAMPLE" "$POSTGRESQL_CONF_SAMPLE"
do
    if [ ! -f "$PREREQ_FILE" ] ; then
      (
        echo "$CMDNAME: file \"$PREREQ_FILE\" not found"
        echo "This means you have a corrupted installation or identified the"
        echo "wrong directory with the invocation option -L."
      ) 1>&2
        exit 1
    fi
done

for file in "$POSTGRES_BKI"
do
     if [ x"`sed 1q $file`" != x"# PostgreSQL $short_version" ]; then
       (
         echo "$CMDNAME: input file \"$file\" does not belong to PostgreSQL $VERSION"
         echo "Check your installation or specify the correct path using the option -L."
       ) 1>&2
         exit 1
     fi
done


trap 'echo "Caught signal." ; exit_nicely' 1 2 3 15

# Let's go
echo "The files belonging to this database system will be owned by user \"$EffectiveUser\"."
echo "This user must also own the server process."
echo

TAB='	'

if test x`pg_getlocale CTYPE` = x`pg_getlocale COLLATE` \
   && test x`pg_getlocale CTYPE` = x`pg_getlocale TIME` \
   && test x`pg_getlocale CTYPE` = x`pg_getlocale NUMERIC` \
   && test x`pg_getlocale CTYPE` = x`pg_getlocale MONETARY` \
   && test x`pg_getlocale CTYPE` = x`pg_getlocale MESSAGES`
then
    echo "The database cluster will be initialized with locale `pg_getlocale CTYPE`."
else
    echo "The database cluster will be initialized with locales:"
    echo "  COLLATE:  `pg_getlocale COLLATE`"
    echo "  CTYPE:    `pg_getlocale CTYPE`"
    echo "  MESSAGES: `pg_getlocale MESSAGES`"
    echo "  MONETARY: `pg_getlocale MONETARY`"
    echo "  NUMERIC:  `pg_getlocale NUMERIC`"
    echo "  TIME:     `pg_getlocale TIME`"
fi
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
      echo "$CMDNAME: directory \"$PGDATA\" exists but is not empty"
      echo "If you want to create a new database system, either remove or empty"
      echo "the directory \"$PGDATA\" or run $CMDNAME with an argument other than"
      echo "\"$PGDATA\"."
    ) 1>&2
    exit 1
else
    if [ ! -d "$PGDATA" ]; then
        $ECHO_N "creating directory $PGDATA... "$ECHO_C
        mkdir -p "$PGDATA" >/dev/null 2>&1 || mkdir "$PGDATA" || exit_nicely
        made_new_pgdata=yes
    else
        $ECHO_N "fixing permissions on existing directory $PGDATA... "$ECHO_C
	chmod go-rwx "$PGDATA" || exit_nicely
    fi
    echo "ok"

    if [ ! -d "$PGDATA"/base ]
	then
        $ECHO_N "creating directory $PGDATA/base... "$ECHO_C
        mkdir "$PGDATA"/base || exit_nicely
	echo "ok"
    fi
    if [ ! -d "$PGDATA"/global ]
    then
        $ECHO_N "creating directory $PGDATA/global... "$ECHO_C
        mkdir "$PGDATA"/global || exit_nicely
	echo "ok"
    fi
    if [ ! -d "$PGDATA"/pg_xlog ]
    then
        $ECHO_N "creating directory $PGDATA/pg_xlog... "$ECHO_C
        mkdir "$PGDATA"/pg_xlog || exit_nicely
	echo "ok"
    fi
    if [ ! -d "$PGDATA"/pg_clog ]
    then
        $ECHO_N "creating directory $PGDATA/pg_clog... "$ECHO_C
        mkdir "$PGDATA"/pg_clog || exit_nicely
	echo "ok"
    fi
fi

# Top level PG_VERSION is checked by bootstrapper, so make it first
echo "$short_version" > "$PGDATA/PG_VERSION"         || exit_nicely


##########################################################################
#
# DETERMINE PLATFORM-SPECIFIC CONFIG SETTINGS
#
# Use reasonable values if kernel will let us, else scale back.  Probe for
# max_connections first since it is subject to more constraints than
# shared_buffers.

# common backend options
PGSQL_OPT="-F -D$PGDATA"

cp /dev/null "$PGDATA"/postgresql.conf               || exit_nicely

$ECHO_N "selecting default max_connections... "$ECHO_C

for nconns in 100 50 40 30 20 10
do
    nbuffers=`expr $nconns '*' 5`
    TEST_OPT="$PGSQL_OPT -c shared_buffers=$nbuffers -c max_connections=$nconns"
    if "$PGPATH"/postgres -boot -x0 $TEST_OPT template1 </dev/null >/dev/null 2>&1
    then
	break
    fi
done

echo "$nconns"

$ECHO_N "selecting default shared_buffers... "$ECHO_C

for nbuffers in 1000 900 800 700 600 500 400 300 200 100 50
do
    TEST_OPT="$PGSQL_OPT -c shared_buffers=$nbuffers -c max_connections=$nconns"
    if "$PGPATH"/postgres -boot -x0 $TEST_OPT template1 </dev/null >/dev/null 2>&1
    then
	break
    fi
done

echo "$nbuffers"

##########################################################################
#
# CREATE CONFIG FILES

$ECHO_N "creating configuration files... "$ECHO_C

sed -e "s/^#shared_buffers = 1000/shared_buffers = $nbuffers/" \
    -e "s/^#max_connections = 100/max_connections = $nconns/" \
    -e "s/^#lc_messages = 'C'/lc_messages = '`pg_getlocale MESSAGES`'/" \
    -e "s/^#lc_monetary = 'C'/lc_monetary = '`pg_getlocale MONETARY`'/" \
    -e "s/^#lc_numeric = 'C'/lc_numeric = '`pg_getlocale NUMERIC`'/" \
    -e "s/^#lc_time = 'C'/lc_time = '`pg_getlocale TIME`'/" \
    "$POSTGRESQL_CONF_SAMPLE"  > "$PGDATA"/postgresql.conf || exit_nicely
if [ "x$HAVE_IPV6" = xyes ]
then
    cp "$PG_HBA_SAMPLE" "$PGDATA"/pg_hba.conf          || exit_nicely
else
    sed -e "/ ::1 / s/^host/#host/" \
        "$PG_HBA_SAMPLE" > "$PGDATA"/pg_hba.conf       || exit_nicely
fi
cp "$PG_IDENT_SAMPLE" "$PGDATA"/pg_ident.conf          || exit_nicely

chmod 0600 "$PGDATA"/pg_hba.conf "$PGDATA"/pg_ident.conf \
	"$PGDATA"/postgresql.conf

echo "ok"

##########################################################################
#
# RUN BKI SCRIPT IN BOOTSTRAP MODE TO CREATE TEMPLATE1

if [ "$debug" = yes ]
then
    BOOTSTRAP_TALK_ARG="-d 5"
fi

unset PGCLIENTENCODING

$ECHO_N "creating template1 database in $PGDATA/base/1... "$ECHO_C

rm -rf "$PGDATA"/base/1 || exit_nicely
mkdir "$PGDATA"/base/1 || exit_nicely

cat "$POSTGRES_BKI" \
| sed -e "s/POSTGRES/$POSTGRES_SUPERUSERNAME/g" \
      -e "s/ENCODING/$ENCODINGID/g" \
| 
(
  LC_COLLATE=`pg_getlocale COLLATE`
  LC_CTYPE=`pg_getlocale CTYPE`
  export LC_COLLATE
  export LC_CTYPE
  unset LC_ALL
  "$PGPATH"/postgres -boot -x1 $PGSQL_OPT $BOOTSTRAP_TALK_ARG template1
) \
|| exit_nicely

# Make the per-database PGVERSION for template1 only after init'ing it
echo "$short_version" > "$PGDATA/base/1/PG_VERSION" || exit_nicely

echo "ok"

##########################################################################
#
# CREATE VIEWS and other things
#
# NOTE: because here we are driving a standalone backend (not psql), we must
# follow the standalone backend's convention that commands end at a newline.
# To break an SQL command across lines in this script, backslash-escape all
# internal newlines in the command.

PGSQL_OPT="$PGSQL_OPT -O -c search_path=pg_catalog -c exit_on_error=true"

$ECHO_N "initializing pg_shadow... "$ECHO_C

"$PGPATH"/postgres $PGSQL_OPT template1 >/dev/null <<EOF
-- Create a trigger so that direct updates to pg_shadow will be written
-- to the flat password/group files pg_pwd and pg_group
CREATE TRIGGER pg_sync_pg_pwd AFTER INSERT OR UPDATE OR DELETE ON pg_shadow \
FOR EACH ROW EXECUTE PROCEDURE update_pg_pwd_and_pg_group();
CREATE TRIGGER pg_sync_pg_group AFTER INSERT OR UPDATE OR DELETE ON pg_group \
FOR EACH ROW EXECUTE PROCEDURE update_pg_pwd_and_pg_group();
-- needs to be done before alter user, because alter user checks that
-- pg_shadow is secure ...
REVOKE ALL on pg_shadow FROM public;
EOF
if [ "$?" -ne 0 ]; then
    exit_nicely
fi
echo "ok"

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
    $ECHO_N "setting password... "$ECHO_C
    "$PGPATH"/postgres $PGSQL_OPT template1 >/dev/null <<EOF
	ALTER USER "$POSTGRES_SUPERUSERNAME" WITH PASSWORD '$FirstPw';
EOF
    if [ "$?" -ne 0 ]; then
	exit_nicely
    fi
    if [ ! -f "$PGDATA"/global/pg_pwd ]; then
        echo
        echo "$CMDNAME: The password file wasn't generated. Please report this problem." 1>&2
        exit_nicely
    fi
    echo "ok"
fi

$ECHO_N "enabling unlimited row size for system tables... "$ECHO_C

"$PGPATH"/postgres $PGSQL_OPT template1 >/dev/null <<EOF
ALTER TABLE pg_attrdef CREATE TOAST TABLE;
ALTER TABLE pg_constraint CREATE TOAST TABLE;
ALTER TABLE pg_database CREATE TOAST TABLE;
ALTER TABLE pg_description CREATE TOAST TABLE;
ALTER TABLE pg_group CREATE TOAST TABLE;
ALTER TABLE pg_proc CREATE TOAST TABLE;
ALTER TABLE pg_rewrite CREATE TOAST TABLE;
ALTER TABLE pg_shadow CREATE TOAST TABLE;
ALTER TABLE pg_statistic CREATE TOAST TABLE;
EOF
if [ "$?" -ne 0 ]; then
    exit_nicely
fi
echo "ok"


$ECHO_N "initializing pg_depend... "$ECHO_C

"$PGPATH"/postgres $PGSQL_OPT template1 >/dev/null <<EOF
-- Make PIN entries in pg_depend for all objects made so far in the tables
-- that the dependency code handles.  This is overkill (the system doesn't
-- really depend on having every last weird datatype, for instance)
-- but generating only the minimum required set of dependencies seems hard.
-- Note that we deliberately do not pin the system views.
-- First delete any already-made entries; PINs override all else, and must
-- be the only entries for their objects.
DELETE FROM pg_depend;
INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' FROM pg_class;
INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' FROM pg_proc;
INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' FROM pg_type;
INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' FROM pg_cast;
INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' FROM pg_constraint;
INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' FROM pg_attrdef;
INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' FROM pg_language;
INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' FROM pg_operator;
INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' FROM pg_opclass;
INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' FROM pg_rewrite;
INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' FROM pg_trigger;
-- restriction here to avoid pinning the public namespace
INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' FROM pg_namespace \
    WHERE nspname LIKE 'pg%';
EOF
if [ "$?" -ne 0 ]; then
    exit_nicely
fi
echo "ok"


$ECHO_N "creating system views... "$ECHO_C

"$PGPATH"/postgres $PGSQL_OPT template1 >/dev/null <<EOF

CREATE VIEW pg_user AS \
    SELECT \
        usename, \
        usesysid, \
        usecreatedb, \
        usesuper, \
        usecatupd, \
        '********'::text as passwd, \
        valuntil, \
        useconfig \
    FROM pg_shadow;

CREATE VIEW pg_rules AS \
    SELECT \
        N.nspname AS schemaname, \
        C.relname AS tablename, \
        R.rulename AS rulename, \
        pg_get_ruledef(R.oid) AS definition \
    FROM (pg_rewrite R JOIN pg_class C ON (C.oid = R.ev_class)) \
        LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) \
    WHERE R.rulename != '_RETURN';

CREATE VIEW pg_views AS \
    SELECT \
        N.nspname AS schemaname, \
        C.relname AS viewname, \
        pg_get_userbyid(C.relowner) AS viewowner, \
        pg_get_viewdef(C.oid) AS definition \
    FROM pg_class C LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) \
    WHERE C.relkind = 'v';

CREATE VIEW pg_tables AS \
    SELECT \
        N.nspname AS schemaname, \
        C.relname AS tablename, \
        pg_get_userbyid(C.relowner) AS tableowner, \
        C.relhasindex AS hasindexes, \
        C.relhasrules AS hasrules, \
        (C.reltriggers > 0) AS hastriggers \
    FROM pg_class C LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) \
    WHERE C.relkind = 'r';

CREATE VIEW pg_indexes AS \
    SELECT \
        N.nspname AS schemaname, \
        C.relname AS tablename, \
        I.relname AS indexname, \
        pg_get_indexdef(I.oid) AS indexdef \
    FROM pg_index X JOIN pg_class C ON (C.oid = X.indrelid) \
         JOIN pg_class I ON (I.oid = X.indexrelid) \
         LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) \
    WHERE C.relkind = 'r' AND I.relkind = 'i';

CREATE VIEW pg_stats AS \
    SELECT \
        nspname AS schemaname, \
        relname AS tablename, \
        attname AS attname, \
        stanullfrac AS null_frac, \
        stawidth AS avg_width, \
        stadistinct AS n_distinct, \
        CASE 1 \
            WHEN stakind1 THEN stavalues1 \
            WHEN stakind2 THEN stavalues2 \
            WHEN stakind3 THEN stavalues3 \
            WHEN stakind4 THEN stavalues4 \
        END AS most_common_vals, \
        CASE 1 \
            WHEN stakind1 THEN stanumbers1 \
            WHEN stakind2 THEN stanumbers2 \
            WHEN stakind3 THEN stanumbers3 \
            WHEN stakind4 THEN stanumbers4 \
        END AS most_common_freqs, \
        CASE 2 \
            WHEN stakind1 THEN stavalues1 \
            WHEN stakind2 THEN stavalues2 \
            WHEN stakind3 THEN stavalues3 \
            WHEN stakind4 THEN stavalues4 \
        END AS histogram_bounds, \
        CASE 3 \
            WHEN stakind1 THEN stanumbers1[1] \
            WHEN stakind2 THEN stanumbers2[1] \
            WHEN stakind3 THEN stanumbers3[1] \
            WHEN stakind4 THEN stanumbers4[1] \
        END AS correlation \
    FROM pg_statistic s JOIN pg_class c ON (c.oid = s.starelid) \
         JOIN pg_attribute a ON (c.oid = attrelid AND attnum = s.staattnum) \
         LEFT JOIN pg_namespace n ON (n.oid = c.relnamespace) \
    WHERE has_table_privilege(c.oid, 'select');

REVOKE ALL on pg_statistic FROM public;

CREATE VIEW pg_stat_all_tables AS \
    SELECT \
            C.oid AS relid, \
            N.nspname AS schemaname, \
            C.relname AS relname, \
            pg_stat_get_numscans(C.oid) AS seq_scan, \
            pg_stat_get_tuples_returned(C.oid) AS seq_tup_read, \
            sum(pg_stat_get_numscans(I.indexrelid)) AS idx_scan, \
            sum(pg_stat_get_tuples_fetched(I.indexrelid)) AS idx_tup_fetch, \
            pg_stat_get_tuples_inserted(C.oid) AS n_tup_ins, \
            pg_stat_get_tuples_updated(C.oid) AS n_tup_upd, \
            pg_stat_get_tuples_deleted(C.oid) AS n_tup_del \
    FROM pg_class C LEFT JOIN \
         pg_index I ON C.oid = I.indrelid \
         LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) \
    WHERE C.relkind = 'r' \
    GROUP BY C.oid, N.nspname, C.relname;

CREATE VIEW pg_stat_sys_tables AS \
    SELECT * FROM pg_stat_all_tables \
    WHERE schemaname IN ('pg_catalog', 'pg_toast');

CREATE VIEW pg_stat_user_tables AS \
    SELECT * FROM pg_stat_all_tables \
    WHERE schemaname NOT IN ('pg_catalog', 'pg_toast');

CREATE VIEW pg_statio_all_tables AS \
    SELECT \
            C.oid AS relid, \
            N.nspname AS schemaname, \
            C.relname AS relname, \
            pg_stat_get_blocks_fetched(C.oid) - \
                    pg_stat_get_blocks_hit(C.oid) AS heap_blks_read, \
            pg_stat_get_blocks_hit(C.oid) AS heap_blks_hit, \
            sum(pg_stat_get_blocks_fetched(I.indexrelid) - \
                    pg_stat_get_blocks_hit(I.indexrelid)) AS idx_blks_read, \
            sum(pg_stat_get_blocks_hit(I.indexrelid)) AS idx_blks_hit, \
            pg_stat_get_blocks_fetched(T.oid) - \
                    pg_stat_get_blocks_hit(T.oid) AS toast_blks_read, \
            pg_stat_get_blocks_hit(T.oid) AS toast_blks_hit, \
            pg_stat_get_blocks_fetched(X.oid) - \
                    pg_stat_get_blocks_hit(X.oid) AS tidx_blks_read, \
            pg_stat_get_blocks_hit(X.oid) AS tidx_blks_hit \
    FROM pg_class C LEFT JOIN \
            pg_index I ON C.oid = I.indrelid LEFT JOIN \
            pg_class T ON C.reltoastrelid = T.oid LEFT JOIN \
            pg_class X ON T.reltoastidxid = X.oid \
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) \
    WHERE C.relkind = 'r' \
    GROUP BY C.oid, N.nspname, C.relname, T.oid, X.oid;

CREATE VIEW pg_statio_sys_tables AS \
    SELECT * FROM pg_statio_all_tables \
    WHERE schemaname IN ('pg_catalog', 'pg_toast');

CREATE VIEW pg_statio_user_tables AS \
    SELECT * FROM pg_statio_all_tables \
    WHERE schemaname NOT IN ('pg_catalog', 'pg_toast');

CREATE VIEW pg_stat_all_indexes AS \
    SELECT \
            C.oid AS relid, \
            I.oid AS indexrelid, \
            N.nspname AS schemaname, \
            C.relname AS relname, \
            I.relname AS indexrelname, \
            pg_stat_get_numscans(I.oid) AS idx_scan, \
            pg_stat_get_tuples_returned(I.oid) AS idx_tup_read, \
            pg_stat_get_tuples_fetched(I.oid) AS idx_tup_fetch \
    FROM pg_class C JOIN \
            pg_index X ON C.oid = X.indrelid JOIN \
            pg_class I ON I.oid = X.indexrelid \
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) \
    WHERE C.relkind = 'r';

CREATE VIEW pg_stat_sys_indexes AS \
    SELECT * FROM pg_stat_all_indexes \
    WHERE schemaname IN ('pg_catalog', 'pg_toast');

CREATE VIEW pg_stat_user_indexes AS \
    SELECT * FROM pg_stat_all_indexes \
    WHERE schemaname NOT IN ('pg_catalog', 'pg_toast');

CREATE VIEW pg_statio_all_indexes AS \
    SELECT \
            C.oid AS relid, \
            I.oid AS indexrelid, \
            N.nspname AS schemaname, \
            C.relname AS relname, \
            I.relname AS indexrelname, \
            pg_stat_get_blocks_fetched(I.oid) - \
                    pg_stat_get_blocks_hit(I.oid) AS idx_blks_read, \
            pg_stat_get_blocks_hit(I.oid) AS idx_blks_hit \
    FROM pg_class C JOIN \
            pg_index X ON C.oid = X.indrelid JOIN \
            pg_class I ON I.oid = X.indexrelid \
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) \
    WHERE C.relkind = 'r';

CREATE VIEW pg_statio_sys_indexes AS \
    SELECT * FROM pg_statio_all_indexes \
    WHERE schemaname IN ('pg_catalog', 'pg_toast');

CREATE VIEW pg_statio_user_indexes AS \
    SELECT * FROM pg_statio_all_indexes \
    WHERE schemaname NOT IN ('pg_catalog', 'pg_toast');

CREATE VIEW pg_statio_all_sequences AS \
    SELECT \
            C.oid AS relid, \
            N.nspname AS schemaname, \
            C.relname AS relname, \
            pg_stat_get_blocks_fetched(C.oid) - \
                    pg_stat_get_blocks_hit(C.oid) AS blks_read, \
            pg_stat_get_blocks_hit(C.oid) AS blks_hit \
    FROM pg_class C \
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) \
    WHERE C.relkind = 'S';

CREATE VIEW pg_statio_sys_sequences AS \
    SELECT * FROM pg_statio_all_sequences \
    WHERE schemaname IN ('pg_catalog', 'pg_toast');

CREATE VIEW pg_statio_user_sequences AS \
    SELECT * FROM pg_statio_all_sequences \
    WHERE schemaname NOT IN ('pg_catalog', 'pg_toast');

CREATE VIEW pg_stat_activity AS \
    SELECT \
            D.oid AS datid, \
            D.datname AS datname, \
            pg_stat_get_backend_pid(S.backendid) AS procpid, \
            pg_stat_get_backend_userid(S.backendid) AS usesysid, \
            U.usename AS usename, \
            pg_stat_get_backend_activity(S.backendid) AS current_query, \
            pg_stat_get_backend_activity_start(S.backendid) AS query_start \
    FROM pg_database D, \
            (SELECT pg_stat_get_backend_idset() AS backendid) AS S, \
            pg_shadow U \
    WHERE pg_stat_get_backend_dbid(S.backendid) = D.oid AND \
            pg_stat_get_backend_userid(S.backendid) = U.usesysid;

CREATE VIEW pg_stat_database AS \
    SELECT \
            D.oid AS datid, \
            D.datname AS datname, \
            pg_stat_get_db_numbackends(D.oid) AS numbackends, \
            pg_stat_get_db_xact_commit(D.oid) AS xact_commit, \
            pg_stat_get_db_xact_rollback(D.oid) AS xact_rollback, \
            pg_stat_get_db_blocks_fetched(D.oid) - \
                    pg_stat_get_db_blocks_hit(D.oid) AS blks_read, \
            pg_stat_get_db_blocks_hit(D.oid) AS blks_hit \
    FROM pg_database D;

CREATE VIEW pg_locks AS \
    SELECT * \
    FROM pg_lock_status() AS L(relation oid, database oid, \
	transaction xid, pid int4, mode text, granted boolean);

CREATE VIEW pg_settings AS \
    SELECT * \
    FROM pg_show_all_settings() AS A \
    (name text, setting text, context text, vartype text, \
     source text, min_val text, max_val text);

CREATE RULE pg_settings_u AS \
    ON UPDATE TO pg_settings \
    WHERE new.name = old.name DO \
    SELECT set_config(old.name, new.setting, 'f');

CREATE RULE pg_settings_n AS \
    ON UPDATE TO pg_settings \
    DO INSTEAD NOTHING;

GRANT SELECT, UPDATE ON pg_settings TO PUBLIC;

EOF
if [ "$?" -ne 0 ]; then
    exit_nicely
fi
echo "ok"

$ECHO_N "loading pg_description... "$ECHO_C
(
  cat <<EOF
    CREATE TEMP TABLE tmp_pg_description ( \
	objoid oid, \
	classname name, \
	objsubid int4, \
	description text) WITHOUT OIDS;
    COPY tmp_pg_description FROM '$POSTGRES_DESCR';
    INSERT INTO pg_description SELECT \
	t.objoid, c.oid, t.objsubid, t.description \
    FROM tmp_pg_description t, pg_class c WHERE c.relname = t.classname;
EOF
) \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "ok"

# Create pg_conversion and support functions
$ECHO_N "creating conversions... "$ECHO_C
grep -v '^DROP CONVERSION' $datadir/conversion_create.sql | "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "ok"

# Set most system catalogs and built-in functions as world-accessible.
# Some objects may require different permissions by default, so we
# make sure we don't overwrite privilege sets that have already been
# set (NOT NULL).
$ECHO_N "setting privileges on built-in objects... "$ECHO_C
(
  cat <<EOF
    UPDATE pg_class SET relacl = '{"=r/\\\\"$POSTGRES_SUPERUSERNAME\\\\""}' \
        WHERE relkind IN ('r', 'v', 'S') AND relacl IS NULL;
    UPDATE pg_proc SET proacl = '{"=X/\\\\"$POSTGRES_SUPERUSERNAME\\\\""}' \
        WHERE proacl IS NULL;
    UPDATE pg_language SET lanacl = '{"=U/\\\\"$POSTGRES_SUPERUSERNAME\\\\""}' \
        WHERE lanpltrusted;
    GRANT USAGE ON SCHEMA pg_catalog TO PUBLIC;
    GRANT CREATE, USAGE ON SCHEMA public TO PUBLIC;
EOF
) \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "ok"

$ECHO_N "creating information schema... "$ECHO_C
"$PGPATH"/postgres $PGSQL_OPT -N template1 > /dev/null < "$datadir"/information_schema.sql || exit_nicely
(
  # Format version number to format required by information schema (09.08.0007abc).
  major_version=`echo $VERSION | sed 's/^\([0-9]*\).*/00\1/;s/.*\(..\)$/\1/'`
  minor_version=`echo $VERSION | sed 's/^[0-9]*\.\([0-9]*\).*/00\1/;s/.*\(..\)$/\1/'`
  micro_version=`echo $VERSION | sed -e 's/^[0-9]*\.[0-9]*\.\([0-9]*\).*/0000\1/' -e 't L' -e 's/.*/0000/;q' -e ': L' -e 's/.*\(....\)$/\1/'`
  letter_version=`echo $VERSION | sed 's/^.*[0-9]\([^0-9]*\)$/\1/'`
  combined_version="$major_version.$minor_version.$micro_version$letter_version"

  echo "UPDATE information_schema.sql_implementation_info SET character_value = '$combined_version' WHERE implementation_info_name = 'DBMS VERSION';"

  echo "COPY information_schema.sql_features (feature_id, feature_name, sub_feature_id, sub_feature_name, is_supported, comments) FROM '$datadir/sql_features.txt';"
) \
	| "$PGPATH"/postgres $PGSQL_OPT template1 > /dev/null || exit_nicely
echo "ok"

$ECHO_N "vacuuming database template1... "$ECHO_C

"$PGPATH"/postgres $PGSQL_OPT template1 >/dev/null <<EOF
ANALYZE;
VACUUM FULL FREEZE;
EOF
if [ "$?" -ne 0 ]; then
    exit_nicely
fi
echo "ok"

$ECHO_N "copying template1 to template0... "$ECHO_C

"$PGPATH"/postgres $PGSQL_OPT template1 >/dev/null <<EOF
CREATE DATABASE template0;

UPDATE pg_database SET \
	datistemplate = 't', \
	datallowconn = 'f' \
    WHERE datname = 'template0';

-- We use the OID of template0 to determine lastsysoid

UPDATE pg_database SET datlastsysoid = \
    (SELECT oid::int4 - 1 FROM pg_database WHERE datname = 'template0');

-- Explicitly revoke public create-schema and create-temp-table privileges
-- in template1 and template0; else the latter would be on by default

REVOKE CREATE,TEMPORARY ON DATABASE template1 FROM public;
REVOKE CREATE,TEMPORARY ON DATABASE template0 FROM public;

-- Finally vacuum to clean up dead rows in pg_database

VACUUM FULL pg_database;
EOF
if [ "$?" -ne 0 ]; then
    exit_nicely
fi
echo "ok"


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
