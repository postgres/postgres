#! /bin/sh

# pg_dumpaccounts
#
# Dumps the "pg_shadow" and "pg_group" tables, which belong to the 
# whole installation rather than any one individual database.
#
# $Header: /cvsroot/pgsql/contrib/pg_dumpaccounts/Attic/pg_dumpaccounts.sh,v 1.1 2000/11/02 18:20:12 wieck Exp $

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
for prog in psql ; do
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

#
# Scan options. We're interested in the -h (host) and -p (port) options.
#
while [ $# -gt 0 ] ; do
    case $1 in
        --help)
                usage=t
                break
                ;;
        --version)
                echo "pg_dumpaccounts (PostgreSQL) $VERSION"
                exit 0
                ;;
		--host|-h)
				connectopts="$connectopts -h $2"
				shift
				;;
        -h*)
                connectopts="$connectopts $1"
                ;;
        --host=*)
                connectopts="$connectopts -h "`echo $1 | sed 's/^--host=//'`
                ;;
		--port|-p)
				connectopts="$connectopts -p $2"
				shift
				;;
        -p*)
                connectopts="$connectopts $1"
                ;;
        --port=*)
                connectopts="$connectopts -p "`echo $1 | sed 's/^--port=//'`
                ;;
        *)
                echo "unknown option '$1'"
				usage=t
                ;;
    esac
    shift
done


if [ "$usage" ] ; then
    echo "$CMDNAME dumps users and groups of a PostgreSQL database cluster."
    echo
    echo "Usage:"
    echo "  $CMDNAME [ -c ] [ -h host ] [ -p port ]"
    echo
    echo "Options:"
    echo "  -h, --host <hostname>    server host name"
    echo "  -p, --port <port>        server port number"
    echo
    echo "Report bugs to <pgsql-bugs@postgresql.org>."
    exit 0
fi


PSQL="${PGPATH}/psql $connectopts"


echo "--"
echo "-- pg_dumpaccounts ($VERSION) $connectopts"
echo "--"
echo "${BS}connect template1"

#
# Dump users (but not the user created by initdb)
#
echo "DELETE FROM pg_shadow WHERE usesysid NOT IN (SELECT datdba FROM pg_database WHERE datname = 'template1');"
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
WHERE usesysid <> (SELECT datdba FROM pg_database WHERE datname = 'template1');
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

exit 0
