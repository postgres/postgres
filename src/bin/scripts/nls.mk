# $PostgreSQL: pgsql/src/bin/scripts/nls.mk,v 1.12 2003/11/29 19:52:07 pgsql Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := cs de es it pt_BR ru sl sv zh_CN
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt
