#!/bin/sh
#
# postgres.init.sh - This script is used to start/stop 
#                    the postgreSQL listener process.
#
# Usage
#
#   You can use this script manually, and/or you
#   can install this script into the runlevel system
#   by running "sh postgres.init.sh install"
#
# Credits
#
#   Thomas Lockhart <lockhart@alumni.caltech.edu>
#   modified from other startup files in the
#   RedHat Linux distribution
#
#   Clark Evans <cce@clarkevans.com>
#   cleaned up, added comments, etc.
# 
# RedHat Stuff
#
#    chkconfig: 345 85 15
#    description: Starts and stops the PostgreSQL backend daemon\
#                 that handles all database requests.
#    processname: postmaster
#    pidfile:     /var/run/postmaster.pid
# 
#
# Note
#
#    This version can log backend output through syslog using
#    the local5 facility. To enable this, set USE_SYSLOG to "yes"
#    below and then edit /etc/syslog.conf to include a line
#    similar to:
#
#       local5.*  /var/log/postgres
#
# Config Variables
#
PGACCOUNT="postgres"      
#
#  The non-root user account which will be used to run the
#  PostgreSQL executeable.   For this script to work, the
#  shell for this account must be SH/BASH.
#
#  The following lines should be in this account's .bash_profile
#
#  PATH=$PATH:$HOME/bin
#  MANPATH=$MANPATH:/opt/pgsql/man
#  PGLIB=/opt/pgsql/lib
#  PGDATA=/opt/pgsql/data
#
POSTMASTER="postmaster"     
#
#  The executable program which is to be run, in this case
#  it is the listener, which waits for requests on the port
#  specified during configuration.
# 
USE_SYSLOG="yes"        
#
# "yes" to enable syslog, "no" to go to /tmp/postgres.log
#
FACILITY="local5"       
#
# can assign local0-local7 as the facility for logging
#
PGLOGFILE="/tmp/postgres.log"   
#
# only used if syslog is disabled
#
PGOPTS="" # -B 256
#
# The B option sets the number of shared buffers
#
# Add the "-i" option to enable TCP/IP sockets in addition
# to unix domain sockets.  This is needed for Java's JDBC
#
# PGOPTS="-i"
#
# Add the -D option if you want to ovverride the PGDATA 
# environment variable defined in
#
# PGOPTS="-D/opt/pgsql/data
#
# Add the -p option if you would like the listener to
# attach to a port other than the one configured (5432?)
#
# PGOPTS="-D/opt/pgsql_beta/data -p 5433"
#

# Source function library.
. /etc/rc.d/init.d/functions

# Get config.
. /etc/sysconfig/network

#
# Check that networking is up.
# Pretty much need it for postmaster.
#
if [ ${NETWORKING} = "no" ]
then
    exit 0
fi

#[ -f /opt/pgsq//bin/postmaster ] || exit 0

#
# See how we were called.
#
case "$1" in
  start)
    if [ -f ${PGLOGFILE} ]
    then
        mv ${PGLOGFILE} ${PGLOGFILE}.old
    fi
    echo -n "Starting postgres: "
#
# force full login to get PGDATA and PGLIB path names
# Since the login script for ${PGACCOUNT} is SH/BASH compliant, 
# we use proper redirection syntax...
#
    if [ ${USE_SYSLOG} = "yes" ]; then
        su - ${PGACCOUNT} -c "(${POSTMASTER} ${PGOPTS} 2>&1 | logger -p ${FACILITY}.notice) &" > /dev/null 2>&1 &
    else
        su - ${PGACCOUNT} -c "${POSTMASTER} ${PGOPTS} 2>>&1 ${PGLOGFILE} &" > /dev/null 2>&1 &
    fi
    sleep 5
    pid=`pidof ${POSTMASTER}`
    echo -n "${POSTMASTER} [$pid]"
#   touch /var/lock/subsys/${POSTMASTER}
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
    echo
    ;;
  install)
    echo "Adding postgres to runlevel system."
    cp $0 /etc/rc.d/init.d/postgres
    /sbin/chkconfig --add postgres
    echo
    ;;
  uninstall)
    echo "Deleting postgres from runlevel system."
    /sbin/chkconfig --del postgres
    rm /etc/rc.d/init.d/postgres
    echo
    ;;
  *)
    echo "Usage: $0 {start|stop|install|uninstall}"
    exit 1
esac

exit 0
