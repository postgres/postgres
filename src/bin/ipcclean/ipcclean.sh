#!/bin/sh
#
# $Header: /cvsroot/pgsql/src/bin/ipcclean/Attic/ipcclean.sh,v 1.2 1998/08/22 05:19:31 momjian Exp $
#
PATH=PG_OPT_IPCCLEANPATH_PARAM:$PATH
export PATH
ipcs | egrep '^m .*|^s .*' | egrep "`whoami`|postgres" | \
awk '{printf "ipcrm -%s %s\n", $1, $2}' '-' | sh
