#!/bin/sh
#
# postgres.init Start postgres back end system.
#
# Author:       Thomas Lockhart <lockhart@alumni.caltech.edu>
#               modified from other startup files in the RedHat Linux distribution
#
# This version can log backend output through syslog using the local5 facility.
# To enable this, edit /etc/syslog.conf to include a line similar to:
#   local5.*  /var/log/postgres
# and then set USE_SYSLOG to "yes" and FACILITY to "local5" below
#
#PGBIN="/opt/postgres/current/bin"	# not used any more - thomas 1997-12-14
PGACCOUNT="postgres"		# the postgres account (you called it something else?)
POSTMASTER="postmaster"		# this probably won't change

USE_SOCKET="yes"		# "no" to enable tcp/ip(remote) access
PGSOCKETFILE="/tmp/.s.PGSQL.5432"
USE_SYSLOG="yes"		# "yes" to enable syslog, "no" to go to /tmp/postgres.log
FACILITY="local5"		# can assign local0-local7 as the facility for logging
PGLOGFILE="/tmp/postgres.log"	# only used if syslog is disabled

PGOPTS="" #-B 1024 -S -o '-Fe'
if [ ${USE_SOCKET} = "no" ]
then
	PGOPTS="-i ${PGOPTS}"	# -i to enable TCP/IP rather than Unix socket
fi

# Source function library.
. /etc/rc.d/init.d/functions

# Get config.
. /etc/sysconfig/network

# Check that networking is up.
# Pretty much need it for postmaster.
if [ ${NETWORKING} = "no" ]
then
	exit 0
fi

# Don't bother checking for file existance to avoid hardcoding any paths - thomas 1997-12-14
#[ -f ${PGBIN}/${POSTMASTER} ] || exit 0

# See how we were called.
case "$1" in
  start)
# force a stop to kill a running postmaster and to clean up sockets
# necessary if system crashed (a power outage provoked this feature)
	eval $0 stop
# save the old log file, if any
	if [ -f ${PGLOGFILE} ]
	then
		mv ${PGLOGFILE} ${PGLOGFILE}.old
	fi

	echo -n "Starting postgres: "

# force full login to get path names
# my postgres runs tcsh so use proper syntax in redirection...
	if [ ${USE_SYSLOG} = "yes" ]; then
		su - ${PGACCOUNT} -c "(${POSTMASTER} ${PGOPTS} |& logger -p ${FACILITY}.notice) &" > /dev/null&
	else
		su - ${PGACCOUNT} -c "${POSTMASTER} ${PGOPTS} >>&! ${PGLOGFILE} &" > /dev/null&
	fi
	sleep 5
	pid=`pidof ${POSTMASTER}`
	echo -n "${POSTMASTER} [$pid]"
#	touch /var/lock/subsys/${POSTMASTER}
	echo
	;;
  stop)
	echo -n "Stopping postgres: "
	pid=`pidof ${POSTMASTER}`
	if [ "$pid" != "" ] ; then
		echo -n "${POSTMASTER} [$pid]"
		kill -TERM $pid
		sleep 1
	fi
	if [ ${USE_SOCKET} = "yes" ] && [ -S ${PGSOCKETFILE} ]; then
		echo -n " (remove socket ${PGSOCKETFILE})"
		rm -f ${PGSOCKETFILE}
	fi
	echo
	;;
  *)
	echo "Usage: $0 {start|stop}"
	exit 1
esac

exit 0
