# $Header: /cvsroot/pgsql/src/bin/scripts/nls.mk,v 1.3 2003/07/23 08:49:30 petere Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := de
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt
