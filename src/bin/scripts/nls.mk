# $Header: /cvsroot/pgsql/src/bin/scripts/nls.mk,v 1.5 2003/08/01 16:19:14 petere Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := cs de ru
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt
