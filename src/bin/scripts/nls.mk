# $PostgreSQL: pgsql/src/bin/scripts/nls.mk,v 1.20 2006/09/22 18:50:41 petere Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := cs de es fr it ko pt_BR ro ru sk sl sv tr zh_CN zh_TW
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c reindexdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt yesno_prompt
