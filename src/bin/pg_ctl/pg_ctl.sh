#! /bin/sh
#-------------------------------------------------------------------------
#
# pg_ctl.sh--
#    Start/Stop/Restart/Report status of postmaster
#
# Copyright (c) 2000, PostgreSQL Global Development Group
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_ctl/Attic/pg_ctl.sh,v 1.15 2000/11/27 02:50:17 tgl Exp $
#
#-------------------------------------------------------------------------

CMDNAME=`basename $0`

help="\
$CMDNAME is a utility to start, stop, restart, and report the status
of a PostgreSQL server.

Usage:
  $CMDNAME start   [-w] [-D DATADIR] [-p PATH-TO-POSTMASTER] [-o \"OPTIONS\"]
  $CMDNAME stop    [-w] [-D DATADIR] [-m SHUTDOWN-MODE]
  $CMDNAME restart [-w] [-D DATADIR] [-m SHUTDOWN-MODE] [-o \"OPTIONS\"]
  $CMDNAME status  [-D DATADIR]

Options:
  -D DATADIR            Location of the database storage area
  -m SHUTDOWN-MODE      May be 'smart', 'fast', or 'immediate'
  -o OPTIONS            Command line options to pass to the postmaster
                        (PostgreSQL server executable)
  -p PATH-TO-POSTMASTER Normally not necessary
  -w                    Wait until operation completes

If the -D option is omitted, the environment variable PGDATA is used.

Shutdown modes are:
  smart                 Quit after all clients have disconnected
  fast                  Quit directly, with proper shutdown
  immediate             Quit without complete shutdown; will lead
                        to recovery run on restart

Report bugs to <pgsql-bugs@postgresql.org>."

advice="\
Try '$CMDNAME --help' for more information."


# Placed here during build
bindir='@bindir@'
VERSION='@VERSION@'

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

# Check if needed programs actually exist in path
if [ -x "$self_path/postmaster" ] && [ -x "$self_path/psql" ]; then
    PGPATH="$self_path"
elif [ -x "$bindir/postmaster" ] && [ -x "$bindir/psql" ]; then
    PGPATH="$bindir"
else
    echo "The programs 'postmaster' and 'psql' are needed by $CMDNAME but" 1>&2
    echo "were not found in the directory '$bindir'." 1>&2
    echo "Check your installation." 1>&2
    exit 1
fi

po_path="$PGPATH/postmaster"

# set default shutdown signal
sig="-TERM"

while [ "$#" -gt 0 ]
do
    case $1 in
	-h|--help|-\?)
	    echo "$help"
	    exit 0
	    ;;
        -V|--version)
	    echo "pg_ctl (PostgreSQL) $VERSION"
	    exit 0
	    ;;
	-D)
	    shift
	    PGDATA="$1"
	    ;;
	-p)
	    shift
	    po_path="$1"
	    ;;
	-m)
	    shift
	    case $1 in
		s|smart)
		    ;;
		f|fast)
		    sig="-INT"
		    ;;
		i|immediate)
		    sig="-QUIT"
		    ;;
	    *)
		echo "$CMDNAME: wrong shutdown mode: $1" 1>&2
		echo "$advice" 1>&2
		exit 1
		;;
	    esac
	    ;;
	-w)
	    wait=1
	    ;;
	-o)
	    shift
	    POSTOPTS="$1"
	    ;;
	-*)
	    echo "$CMDNAME: invalid option: $1" 1>&2
	    echo "$advice" 1>&2
	    exit 1
	    ;;
	start)
	    op="start"
	    ;;
	stop)
	    op="stop"
	    ;;
	restart)
	    op="restart"
	    ;;
	status)
	    op="status"
	    ;;
	*)
	    echo "$CMDNAME: invalid operation mode: $1" 1>&2
	    echo "$advice" 1>&2
	    exit 1
	    ;;
    esac
    shift
done

if [ x"$op" = x"" ];then
    echo "$CMDNAME: no operation mode specified" 1>&2
    echo "$advice" 1>&2
    exit 1
fi

if [ -z "$PGDATA" ];then
    echo "$CMDNAME: no database directory or environment variable \$PGDATA is specified" 1>&2
    echo "$advice" 1>&2
    exit 1
fi

DEFPOSTOPTS=$PGDATA/postmaster.opts.default
POSTOPTSFILE=$PGDATA/postmaster.opts
PIDFILE=$PGDATA/postmaster.pid

if [ $op = "status" ];then
    if [ -f $PIDFILE ];then
	PID=`cat $PIDFILE`
	if [ $PID -lt 0 ];then
	    PID=`expr 0 - $PID`
	    echo "$CMDNAME: postgres is running (pid: $PID)"
	else
	    echo "$CMDNAME: postmaster is running (pid: $PID)"
	    echo "Command line was:"
	    echo "`cat $POSTOPTSFILE`"
	fi
	exit 0
    else
	echo "$CMDNAME: postmaster or postgres is not running"
	exit 1
    fi
fi

if [ $op = "stop" -o $op = "restart" ];then
    if [ -f $PIDFILE ];then
	PID=`cat $PIDFILE`
	if [ $PID -lt 0 ];then
	    PID=`expr 0 - $PID`
	    echo "$CMDNAME: Cannot restart postmaster. postgres is running (pid: $PID)"
	    echo "Please terminate postgres and try again"
	    exit 1
	fi

	kill $sig `cat $PIDFILE`

	# wait for postmaster shutting down
	if [ "$wait" = 1 -o $op = "restart" ];then
	    cnt=0
	    $ECHO_N "Waiting for postmaster to shut down.."$ECHO_C

	    while :
	    do
		if [ -f $PIDFILE ];then
		    $ECHO_N "."$ECHO_C
		    cnt=`expr $cnt + 1`
		    if [ $cnt -gt 60 ];then
			echo "$CMDNAME: postmaster does not shut down"
			exit 1
		    fi
		else
		    break
		fi
		sleep 1
	    done
	    echo "done"
	fi

	echo "postmaster successfully shut down"

    else
	echo "$CMDNAME: cannot find $PIDFILE"
	echo "Is postmaster running?"
	if [ $op = "restart" ];then
	    echo "starting postmaster anyway..."
	else
	    exit 1
	fi
    fi
fi

if [ $op = "start" -o $op = "restart" ];then
    if [ -f $PIDFILE ];then
	echo "$CMDNAME: It seems another postmaster is running. Trying to start postmaster anyway."
	pid=`cat $PIDFILE`
    fi

    # no -o given
    if [ -z "$POSTOPTS" ];then
	if [ $op = "start" ];then
	    # if we are in start mode, then look for postmaster.opts.default
	    if [ -f $DEFPOSTOPTS ];then
		$po_path -D $PGDATA `cat $DEFPOSTOPTS` &
	    else
		$po_path -D $PGDATA &
	    fi
	else
	    # if we are in restart mode, then look postmaster.opts
	    `cat $POSTOPTSFILE` &
	fi
    else
    # -o given
	$po_path -D $PGDATA $POSTOPTS &
    fi

    if [ -f $PIDFILE ];then
	if [ "`cat $PIDFILE`" = "$pid" ];then
	    echo "$CMDNAME: Cannot start postmaster. Is another postmaster is running?"
	    exit 1
        fi
    fi

    # wait for postmaster starting up
    if [ "$wait" = 1 ];then
	cnt=0
	$ECHO_N "Waiting for postmaster to start up.."$ECHO_C
	while :
	do
	    if "$PGPATH/psql" -l >/dev/null 2>&1
	    then
		break;
	    else
		$ECHO_N "."$ECHO_C
		cnt=`expr $cnt + 1`
		if [ $cnt -gt 60 ];then
		    echo "$CMDNAME: postmaster does not start up"
		    exit 1
		fi
		sleep 1
	    fi
	done
	echo "done"
    fi

    echo "postmaster successfully started up"
fi

exit 0
