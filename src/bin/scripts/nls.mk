# $Header: /cvsroot/pgsql/src/bin/scripts/nls.mk,v 1.6 2003/08/11 15:19:58 petere Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := cs de ru sv
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt
