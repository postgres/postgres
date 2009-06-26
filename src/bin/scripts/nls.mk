# $PostgreSQL: pgsql/src/bin/scripts/nls.mk,v 1.23 2009/06/26 19:33:51 petere Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := cs de es fr ja ko pt_BR ro sv ta tr
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c reindexdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt yesno_prompt
