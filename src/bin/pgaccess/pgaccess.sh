#!/bin/sh

PATH_TO_WISH= __wish__
PGACCESS_HOME=__POSTGRESDIR__/pgaccess

export PATH_TO_WISH
export PGACCESS_HOME

exec $(PATH_TO_WISH) $(PGACCESS_HOME)/main.tcl "$@"
