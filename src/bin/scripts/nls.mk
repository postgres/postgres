# $PostgreSQL: pgsql/src/bin/scripts/nls.mk,v 1.23.2.2 2010/05/13 10:50:06 petere Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := cs de es fr it ja ko pl pt_BR ro ru sv ta tr zh_CN zh_TW
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c reindexdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt yesno_prompt
