#! /bin/sh
#-------------------------------------------------------------------------
#
# pg_ctl.sh--
#    Start/Stop/Restart/Report status of postmaster
#
# Copyright (c) 1999, PostgreSQL Global Development Group
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_ctl/Attic/pg_ctl.sh,v 1.4 1999/12/22 04:12:55 ishii Exp $
#
#-------------------------------------------------------------------------
CMDNAME=`basename $0`

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
for prog in postmaster
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
	    ;;
	-p)
	    shift
	    po_path="$1"
	    ;;
	-m)
	    shift
	    case $1 in
		f|fast)
		    sig="-INT"
		    ;;
		i|immediate)
		    sig="-QUIT"
		    ;;
	    *)
		echo "$CMDNAME: Wrong shutdown mode $sigopt"
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
    echo "Usage: $CMDNAME [-w][-D database_dir][-p path_to_postmaster][-o \"postmaster_opts\"] start"
    echo "       $CMDNAME [-w][-D database_dir][-m s[mart]|f[ast]|i[mmediate]] stop"
    echo "       $CMDNAME [-w][-D database_dir][-m s[mart]|f[ast]|i[mmediate]][-o \"postmaster_opts\"] restart"
    echo "       $CMDNAME [-D database_dir] status"
    exit 1
fi

if [ -z "$PGDATA" ];then
    echo "$CMDNAME: No database directory or environment variable \$PGDATA is specified"
    exit 1
fi

DEFPOSTOPTS=$PGDATA/postmaster.opts.default
POSTOPTSFILE=$PGDATA/postmaster.opts
PIDFILE=$PGDATA/postmaster.pid

if [ $op = "status" ];then
    if [ -f $PIDFILE ];then
	echo "$CMDNAME: postmaster is running (pid: `cat $PIDFILE`)"
	echo "options are:"
	echo "`cat $POSTOPTSFILE`"
	exit 0
    else
	echo "$CMDNAME: postmaster is not running"
	exit 1
    fi
fi

if [ $op = "stop" -o $op = "restart" ];then
    if [ -f $PIDFILE ];then
	kill $sig `cat $PIDFILE`

	# wait for postmaster shutting down
	if [ "$wait" = 1 -o $op = "restart" ];then
	    cnt=0
	    echo -n "Waiting for postmaster shutting down.."

	    while :
	    do
		if [ -f $PIDFILE ];then
		    echo -n "."
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
	    echo "done."
	fi

	echo "postmaster successfully shut down."

    else
	echo "$CMDNAME: Can't find $PIDFILE."
	echo "Is postmaster running?"
	if [ $op = "restart" ];then
	    echo "Anyway, I'm going to start up postmaster..."
	else
	    exit 1
	fi
    fi
fi

if [ $op = "start" -o $op = "restart" ];then
    if [ -f $PIDFILE ];then
	echo "$CMDNAME: It seems another postmaster is running. Try to start postmaster anyway."
	pid=`cat $PIDFILE`
    fi

    # no -o given
    if [ -z "$POSTOPTS" ];then
	if [ $op = "start" ];then
	    # if we are in start mode, then look postmaster.opts.default
	    if [ -f $DEFPOSTOPTS ];then
		eval `cat $DEFPOSTOPTS` &
	    else
		echo "$CMDNAME: Can't find $DEFPOSTOPTS"
		exit 1
	    fi
	else
	    # if we are in restart mode, then look postmaster.opts
	    eval `cat $POSTOPTSFILE` &
	fi
    else
	eval "$po_path $POSTOPTS "&
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
	echo -n "Waiting for postmaster starting up.."
	while :
	do
	    if [ ! -f $PIDFILE ];then
		echo -n "."
		cnt=`expr $cnt + 1`
		if [ $cnt -gt 60 ];then
		    echo "$CMDNAME: postmaster does not start up"
		    exit 1
		fi
		sleep 1
	    else
		break
	    fi
	done
	echo "done."
    fi

    echo "postmaster successfully started up."
fi

exit 0
