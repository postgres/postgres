#! /bin/sh

# pg_dumpall
#
# Dumps all databases to standard output. It also dumps the "pg_shadow"
# and "pg_group" tables, which belong to the whole installation rather
# than any one individual database.
#
# $Header: /cvsroot/pgsql/src/bin/pg_dump/Attic/pg_dumpall.sh,v 1.9 2000/12/19 22:12:47 petere Exp $

CMDNAME=`basename $0`

# substituted at build
VERSION='@VERSION@'
MULTIBYTE='@MULTIBYTE@'
bindir='@bindir@'

#
# Find out where we're located
#
PGPATH=
if echo "$0" | grep '/' > /dev/null 2>&1 ; then
    # explicit dir name given
    PGPATH=`echo $0 | sed 's,/[^/]*$,,'`       # (dirname command is not portable)
else
    # look for it in PATH ('which' command is not portable)
    for dir in `echo "$PATH" | sed 's/:/ /g'` ; do
        # empty entry in path means current dir
        [ x"$dir" = x ] && dir='.'
        if [ -f "$dir/$CMDNAME" ] ; then
            PGPATH="$dir"
            break
        fi
    done
fi

# As last resort use the installation directory. We don't want to use
# this as first resort because depending on how users do release upgrades
# they might temporarily move the installation tree elsewhere, so we'd
# accidentally invoke the newly installed versions of pg_dump and psql.
if [ x"$PGPATH" = x"" ]; then
    PGPATH=$bindir
fi

#
# Look for needed programs
#
for prog in pg_dump psql ; do
    if [ ! -x "$PGPATH/$prog" ] ; then
        echo "The program $prog needed by $CMDNAME could not be found. It was"
        echo "expected at:"
        echo "    $PGPATH/$prog"
        echo "If this is not the correct directory, please start $CMDNAME"
        echo "with a full search path. Otherwise make sure that the program"
        echo "was installed successfully."
        exit 1
    fi
done

#
# to adapt to System V vs. BSD 'echo'
#
if echo '\\' | grep '\\\\' >/dev/null 2>&1
then	
    BS='\' dummy='\'            # BSD
else
    BS='\\'                     # System V
fi
# The dummy assignment is necessary to prevent Emacs' font-lock
# mode from going ballistic when editing this file.


usage=
cleanschema=
globals_only=

#
# Scan options. We're interested in the -h (host), -p (port), and -c (clean) options.
# The rest we pass to pg_dump, which may or may not be useful.
#
while [ $# -gt 0 ] ; do
    case $1 in
        --help)
                usage=t
                break
                ;;
        --version)
                echo "pg_dumpall (PostgreSQL) $VERSION"
                exit 0
                ;;
	--host|-h)
		connectopts="$connectopts -h $2"
		shift;;
        -h*)
                connectopts="$connectopts $1"
                ;;
        --host=*)
                connectopts="$connectopts -h "`echo $1 | sed 's/^--host=//'`
                ;;
	--port|-p)
		connectopts="$connectopts -p $2"
		shift;;
        -p*)
                connectopts="$connectopts $1"
                ;;
        --port=*)
                connectopts="$connectopts -p "`echo $1 | sed 's/^--port=//'`
                ;;
        -c|--clean)
                cleanschema=yes
                pgdumpextraopts="$pgdumpextraopts -c"
                ;;
        --globals-only)
                globals_only=yes
                ;;
        *)
                pgdumpextraopts="$pgdumpextraopts $1"
                ;;
    esac
    shift
done


if [ "$usage" ] ; then
    echo "$CMDNAME extracts a PostgreSQL database cluster into an SQL script file."
    echo
    echo "Usage:"
    echo "  $CMDNAME [ -c ] [ -h HOSTNAME ] [ -p PORT ] [ --globals-only ]"
    echo
    echo "Options:"
    echo "  -c, --clean            Clean (drop) schema prior to create"
    echo "  -h, --host=HOSTNAME    Server host name"
    echo "  -p, --port=PORT        Server port number"
    echo "  --globals-only         Only dump global objects, no databases"
    echo "Any extra options will be passed to pg_dump."
    echo
    echo "Report bugs to <pgsql-bugs@postgresql.org>."
    exit 0
fi


PSQL="${PGPATH}/psql $connectopts"
PGDUMP="${PGPATH}/pg_dump $connectopts $pgdumpextraopts -Fp"


echo "--"
echo "-- pg_dumpall ($VERSION) $connectopts $pgdumpextraopts"
echo "--"
echo "${BS}connect template1"

#
# Dump users (but not the user created by initdb)
#
echo "DELETE FROM pg_shadow WHERE usesysid <> (SELECT datdba FROM pg_database WHERE datname = 'template0');"
echo

$PSQL -d template1 -At <<__END__
SELECT
  'CREATE USER "' || usename || '" WITH SYSID ' || usesysid
  || CASE WHEN passwd IS NOT NULL THEN ' PASSWORD ''' || passwd || '''' else '' end
  || CASE WHEN usecreatedb THEN ' CREATEDB'::text ELSE ' NOCREATEDB' END
  || CASE WHEN usesuper THEN ' CREATEUSER'::text ELSE ' NOCREATEUSER' END
  || CASE WHEN valuntil IS NOT NULL THEN ' VALID UNTIL '''::text
    || CAST(valuntil AS TIMESTAMP) || '''' ELSE '' END || ';'
FROM pg_shadow
WHERE usesysid <> (SELECT datdba FROM pg_database WHERE datname = 'template0');
__END__
echo

#
# Dump groups
#
echo "DELETE FROM pg_group;"
echo

$PSQL -d template1 -At -F ' ' -c 'SELECT * FROM pg_group;' | \
while read GRONAME GROSYSID GROLIST ; do
    echo "CREATE GROUP \"$GRONAME\" WITH SYSID ${GROSYSID};"
    raw_grolist=`echo "$GROLIST" | sed 's/^{\(.*\)}$/\1/' | tr ',' ' '`
    for userid in $raw_grolist ; do
        username=`$PSQL -d template1 -At -c "SELECT usename FROM pg_shadow WHERE usesysid = ${userid};"`
        echo "  ALTER GROUP \"$GRONAME\" ADD USER \"$username\";"
    done
done


test "$globals_only" = yes && exit 0


# For each database, run pg_dump to dump the contents of that database.
# We skip databases marked not datallowconn, since we'd be unable to
# connect to them anyway (and besides, we don't want to dump template0).

$PSQL -d template1 -At -F ' ' \
  -c "SELECT datname, usename, pg_encoding_to_char(d.encoding), datistemplate, datpath FROM pg_database d LEFT JOIN pg_shadow u ON (datdba = usesysid) WHERE datallowconn;" | \
while read DATABASE DBOWNER ENCODING ISTEMPLATE DBPATH; do
    echo
    echo "--"
    echo "-- Database $DATABASE"
    echo "--"
    echo "${BS}connect template1 $DBOWNER"

    if [ "$cleanschema" = yes -a "$DATABASE" != template1 ] ; then
        echo "DROP DATABASE \"$DATABASE\";"
    fi

    if [ "$DATABASE" != template1 ] ; then
	createdbcmd="CREATE DATABASE \"$DATABASE\" WITH TEMPLATE = template0"
	if [ x"$DBPATH" != x"" ] ; then
	    createdbcmd="$createdbcmd LOCATION = '$DBPATH'"
	fi
	if [ x"$MULTIBYTE" != x"" ] ; then
	    createdbcmd="$createdbcmd ENCODING = '$ENCODING'"
	fi
	echo "$createdbcmd;"
    fi

    echo "${BS}connect $DATABASE $DBOWNER"
    $PGDUMP "$DATABASE"
    if [ "$?" -ne 0 ] ; then
        echo "pg_dump failed on $DATABASE, exiting" 1>&2
        exit 1
    fi
    if [ x"$ISTEMPLATE" = xt ] ; then
        echo "UPDATE pg_database SET datistemplate = 't' WHERE datname = '$DATABASE';"
    fi
done

exit 0
