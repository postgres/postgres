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
#    $Header: /cvsroot/pgsql/src/bin/pg_ctl/Attic/pg_ctl.sh,v 1.11 2000/03/27 02:12:03 ishii Exp $
#
#-------------------------------------------------------------------------
CMDNAME=`basename $0`
tmp=/tmp/tmp$$
trap "rm -f $tmp; exit" 0 1 2 13 15

# Check for echo -n vs echo \c

ECHO=echo
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
if $ECHO "$0" | grep '/' > /dev/null 2>&1 
then
        # explicit dir name given
        PGPATH=`$ECHO $0 | sed 's,/[^/]*$,,'`       # (dirname command is not portable)
else
        # look for it in PATH ('which' command is not portable)
        for dir in `$ECHO "$PATH" | sed 's/:/ /g'`
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
for prog in postmaster psql
do
        if [ ! -x "$PGPATH/$prog" ]
	then
                $ECHO "The program $prog needed by $CMDNAME could not be found. It was"
                $ECHO "expected at:"
                $ECHO "    $PGPATH/$prog"
                $ECHO "If this is not the correct directory, please start $CMDNAME"
                $ECHO "with a full search path. Otherwise make sure that the program"
                $ECHO "was installed successfully."
                exit 1
        fi
done

po_path=$PGPATH/postmaster

# set default shutdown signal
sig="-TERM"

while [ "$#" -gt 0 ]
do
    case $1 in
	-h|--help)
	usage=1
	break
	;;
	-D)
	    shift
	    PGDATA="$1"
	    export PGDATA
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
		$ECHO "$CMDNAME: Wrong shutdown mode $sigopt"
		usage=1
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
	    usage=1
	    break
	    ;;
    esac
    shift
done

if [ "$usage" = 1 -o "$op" = "" ];then
    $ECHO "Usage: $CMDNAME [-w][-D database_dir][-p path_to_postmaster][-o \"postmaster_opts\"] start"
    $ECHO "       $CMDNAME [-w][-D database_dir][-m s[mart]|f[ast]|i[mmediate]] stop"
    $ECHO "       $CMDNAME [-w][-D database_dir][-m s[mart]|f[ast]|i[mmediate]][-o \"postmaster_opts\"] restart"
    $ECHO "       $CMDNAME [-D database_dir] status"
    exit 1
fi

if [ -z "$PGDATA" ];then
    $ECHO "$CMDNAME: No database directory or environment variable \$PGDATA is specified"
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
	    $ECHO "$CMDNAME: postgres is running (pid: $PID)"
	else
	    $ECHO "$CMDNAME: postmaster is running (pid: $PID)"
	    $ECHO "options are:"
	    $ECHO "`cat $POSTOPTSFILE`"
	fi
	exit 0
    else
	$ECHO "$CMDNAME: postmaster or postgres is not running"
	exit 1
    fi
fi

if [ $op = "stop" -o $op = "restart" ];then
    if [ -f $PIDFILE ];then
	PID=`cat $PIDFILE`
	if [ $PID -lt 0 ];then
	    PID=`expr 0 - $PID`
	    $ECHO "$CMDNAME: Cannot restart postmaster. postgres is running (pid: $PID)"
	    $ECHO "Please terminate postgres and try again"
	    exit 1
	fi

	kill $sig `cat $PIDFILE`

	# wait for postmaster shutting down
	if [ "$wait" = 1 -o $op = "restart" ];then
	    cnt=0
	    $ECHO_N "Waiting for postmaster shutting down.."$ECHO_C

	    while :
	    do
		if [ -f $PIDFILE ];then
		    $ECHO_N "."$ECHO_C
		    cnt=`expr $cnt + 1`
		    if [ $cnt -gt 60 ];then
			$ECHO "$CMDNAME: postmaster does not shut down"
			exit 1
		    fi
		else
		    break
		fi
		sleep 1
	    done
	    $ECHO "done."
	fi

	$ECHO "postmaster successfully shut down."

    else
	$ECHO "$CMDNAME: Can't find $PIDFILE."
	$ECHO "Is postmaster running?"
	if [ $op = "restart" ];then
	    $ECHO "Anyway, I'm going to start up postmaster..."
	else
	    exit 1
	fi
    fi
fi

if [ $op = "start" -o $op = "restart" ];then
    if [ -f $PIDFILE ];then
	$ECHO "$CMDNAME: It seems another postmaster is running. Try to start postmaster anyway."
	pid=`cat $PIDFILE`
    fi

    # no -o given
    if [ -z "$POSTOPTS" ];then
	if [ $op = "start" ];then
	    # if we are in start mode, then look for postmaster.opts.default
	    if [ -f $DEFPOSTOPTS ];then
		eval "$po_path `cat $DEFPOSTOPTS`" >$tmp 2>&1&
	    else
		$ECHO "$CMDNAME: Can't find $DEFPOSTOPTS"
		exit 1
	    fi
	else
	    # if we are in restart mode, then look postmaster.opts
	    eval `cat $POSTOPTSFILE` >$tmp 2>&1 &
	fi
    else
	eval "$po_path $POSTOPTS " >$tmp 2>&1&
    fi

    if [ -f $PIDFILE ];then
	if [ "`cat $PIDFILE`" = "$pid" ];then
	    $ECHO "$CMDNAME: Cannot start postmaster. Is another postmaster is running?"
	    exit 1
        fi
    fi

    # wait for postmaster starting up
    if [ "$wait" = 1 ];then
	cnt=0
	$ECHO_N "Waiting for postmaster starting up.."$ECHO_C
	while :
	do
	    if psql -l >/dev/null 2>&1
	    then
		break;
	    else
		$ECHO_N "."$ECHO_C
		cnt=`expr $cnt + 1`
		if [ $cnt -gt 60 ];then
		    $ECHO "$CMDNAME: postmaster does not start up"
		    if [ -r $tmp ];then
			$ECHO "$CMDNAME: messages from postmaster:"
			$ECHO
			cat $tmp
		    fi
		    exit 1
		fi
		sleep 1
	    fi
	done
	$ECHO "done."
    fi

    $ECHO "postmaster successfully started up."
fi

exit 0
