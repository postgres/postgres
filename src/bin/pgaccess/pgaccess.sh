#! /bin/sh

PATH_TO_WISH='@WISH@'
PGACCESS_HOME='@PGACCESSHOME@'

export PATH_TO_WISH
export PGACCESS_HOME

exec "${PATH_TO_WISH}" "${PGACCESS_HOME}/main.tcl" "$@"
