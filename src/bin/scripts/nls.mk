# $Header: /cvsroot/pgsql/src/bin/scripts/nls.mk,v 1.4 2003/07/23 09:36:09 petere Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := cs de
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt
