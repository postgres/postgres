#!/bin/sh
#
# $Header: /cvsroot/pgsql/src/bin/ipcclean/Attic/ipcclean.sh,v 1.9 2001/02/10 06:12:15 momjian Exp $
#

CMDNAME=`basename $0`

if [ "$1" = '-?' -o "$1" = "--help" ]; then
    echo "$CMDNAME cleans up shared memory and semaphores from aborted PostgreSQL backends."
    echo
    echo "Usage:"
    echo "  $CMDNAME"
    echo
    echo "Note: Since the utilities underlying this script are very different"
    echo "from platform to platform, chances are that it might not work on"
    echo "yours. If that is the case, please write to <pgsql-bugs@postgresql.org>"
    echo "so that your platform can be supported in the future."
    exit 0
fi

if [ "$USER" = 'root' -o "$LOGNAME" = 'root' ]
then
  (
    echo "You cannot run $CMDNAME as root. Please log in (using, e.g., 'su')"
    echo "as the (unprivileged) user that owned the server process."
  ) 1>&2
    exit 1
fi

EffectiveUser=`id -n -u 2>/dev/null || whoami 2>/dev/null`

#-----------------------------------
# List of platform-specific hacks
# Feel free to add yours here.
#-----------------------------------

#
# This is based on RedHat 5.2.
#
if [ `uname` = 'Linux' ]; then
    ipcs_id=
    ipcs_lpid=
    did_anything=

    if ps x | grep -s '[p]ostmaster' >/dev/null 2>&1 ; then
        echo "$CMDNAME: You still have a postmaster running." 1>&2
        exit 1
    fi

    # shared memory
    for val in `ipcs -m -p | grep '^[0-9]' | awk '{printf "%s %s\n", $1, $3, $4}'`
    do
        if [ -z "$ipcs_id" ]; then
            ipcs_id=$val
            # Note: We can do -n here, because we know the platform.
            echo -n "Shared memory $ipcs_id ... "
            continue
        fi

        ipcs_lpid=$val

        # Don't do anything if process still running.
        # (This check is conceptually phony, but it's
        # useful anyway in practice.)
        ps hj$ipcs_lpid >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            echo "skipped. Process still exists (pid $ipcs_lpid)."
        else
            # try remove
            ipcrm shm $ipcs_id
            if [ $? -eq 0 ]; then
                did_anything=t
            else
                exit
            fi
        fi
        ipcs_id=
        ipcs_lpid=
    done

    # semaphores
    for val in `ipcs -s -c | grep '^[0-9]' | awk '{printf "%s\n", $1}'`; do
        echo -n "Semaphore $val ... "
        # try remove
        ipcrm sem $val
        if [ $? -eq 0 ]; then
            did_anything=t
        else
            exit
        fi
    done

    [ -z "$did_anything" ] && echo "$CMDNAME: nothing removed" && exit 1
    exit 0
fi # end Linux


# This is the original implementation. It seems to work
# on FreeBSD, SunOS/Solaris, HP-UX, IRIX, and probably
# some others.

ipcs | egrep '^m .*|^s .*' | egrep "$EffectiveUser" | \
awk '{printf "ipcrm -%s %s\n", $1, $2}' '-' | sh
