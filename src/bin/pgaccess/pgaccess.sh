#! /bin/sh

PATH_TO_WISH=__wish__
PGACCESS_HOME=__PGACCESSHOME__

export PATH_TO_WISH
export PGACCESS_HOME

exec ${PATH_TO_WISH} ${PGACCESS_HOME}/main.tcl "$@"
