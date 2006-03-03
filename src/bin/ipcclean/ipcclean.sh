#!/bin/sh
#
# $PostgreSQL: pgsql/src/bin/ipcclean/ipcclean.sh,v 1.18 2006/03/03 21:52:37 momjian Exp $
#

CMDNAME=`basename $0`

if [ "$1" = '-?' -o "$1" = "--help" ]; then
    echo "$CMDNAME cleans up shared memory and semaphores from aborted PostgreSQL"
    echo "backends."
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

# test for running as root

ISROOT="N"
if id -u >/dev/null 2>&1
then	if [ `id -u` -eq 0 ]
	then	ISROOT="Y"
	fi
elif	# only check $LOGNAME if $USER is not set
	[ "$USER" = 'root' -o \( ! "$USER" -a "$LOGNAME" = 'root' \) ]
then	ISROOT="Y"
fi

if [ "$ISROOT" = "Y" ]
then
  (
    echo "$CMDNAME: cannot be run as root" 1>&2
    echo "Please log in (using, e.g., \"su\") as the (unprivileged) user that" 1>&2
    echo "owned the server process." 1>&2
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
    did_anything=

    if ps x | grep -s '[p]ostmaster' >/dev/null 2>&1 ; then
        echo "$CMDNAME: a postmaster is still running" 1>&2
        exit 1
    fi

    # shared memory
    for val in `ipcs -m -p | grep '^[0-9]' | awk '{printf "%s:%s:%s\n", $1, $3, $4}'`
    do
	save_IFS=$IFS
	IFS=:
	set X $val
	shift
	IFS=$save_IFS
	ipcs_shmid=$1
	ipcs_cpid=$2
	ipcs_lpid=$3

        # Note: We can do -n here, because we know the platform.
        echo -n "Shared memory $ipcs_shmid ... "

        # Don't do anything if process still running.
        # (This check is conceptually phony, but it's
        # useful anyway in practice.)
        ps hj $ipcs_cpid $ipcs_lpid >/dev/null 2>&1
        if [ "$?" -eq 0 ]; then
            echo "skipped; process still exists (pid $ipcs_cpid or $ipcs_lpid)."
	    continue
	fi

        # try remove
        ipcrm shm $ipcs_shmid
        if [ "$?" -eq 0 ]; then
            did_anything=t
        else
            exit
        fi
    done

    # semaphores
    for val in `ipcs -s -c | grep '^[0-9]' | awk '{printf "%s\n", $1}'`; do
        echo -n "Semaphore $val ... "
        # try remove
        ipcrm sem $val
        if [ "$?" -eq 0 ]; then
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
